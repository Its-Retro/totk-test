#ifndef TOTK_HOST_AUDREN_VOICES_H
#define TOTK_HOST_AUDREN_VOICES_H
/* Audio renderer voice progress: timing only. Nothing is decoded, mixed or played.
 *
 * The game's nn::audio library learns how far each voice has played only from the VoiceOutStatus
 * entries of the RequestUpdate reply (played sample count, wave buffers consumed). If they never move,
 * no wave buffer is ever released: streamed sounds never refill or finish, and anything the game times
 * against a sound (the opening event among them) waits forever. boot_runtime.h answers RequestUpdate
 * silently and calls boot_audren_update() once per update to fill those entries.
 *
 * One call does what the server does for one update, in the order nxport's Renderer::update does it:
 *   1. the renderer frame that elapsed since the previous update is rendered (only while the renderer is
 *      started): every started voice consumes frame * (voice sample rate / target_rate) * pitch source
 *      samples from its queued wave buffers (1.0 per output sample when the voice skips sample-rate
 *      conversion);
 *   2. this update's voice parameters are taken in (new voices, play state, appended wave buffers, flush
 *      requests);
 *   3. each voice's OutStatus is written from what has been rendered so far.
 * Sample offsets count samples for every format, so the accounting needs no decoding.
 *
 * Adapted from nxport (https://github.com/nxporting/nxport, MIT, Copyright (c) 2026 the nxport authors),
 * crates/engine/src/horizon/audio/renderer.rs: Renderer::update, update_voice, write_voice_status,
 * render_frame, process_wave_buffers (its sample accounting) and mark_end_of_buffer. nxport follows
 * Ryujinx (MIT, Copyright (c) Ryujinx Team and Contributors), Ryujinx.Audio/Renderer/Server: VoiceInfo,
 * VoiceUpdateState and Dsp/DataSourceHelper. See THIRD-PARTY-NOTICES.md.
 *
 * State lives at the start of the renderer work buffer (the transfer memory handed to OpenAudioRenderer,
 * which only the server side uses), so checkpoints carry it. It is tagged with the transfer handle
 * (`key`); a missing tag or another key (renderer reopened, fresh memory) starts from a clean state.
 * Included by boot_runtime.h after <string.h>/<stdint.h>; it adds no includes of its own. */

#define BOOT_AVP_TAG UINT32_C(0x31505641)      /* "AVP1" */
#define BOOT_AVP_VOICES 96u                    /* the REV11 profile boot_runtime.h accepts: 96 voices */
#define BOOT_AVP_CHANNELS 96u                  /* and 96 voice channel resources */
#define BOOT_AVP_SLOTS 4u                      /* wave buffer slots per voice */
#define BOOT_AVP_VOICE_IN 0x170u               /* VoiceInParameter size before revision 15 */
#define BOOT_AVP_VOICE_OUT 0x10u               /* VoiceOutStatus size */
#define BOOT_AVP_STEP_LIMIT 4096u              /* wave-buffer segments one channel may walk in one frame */

/* nxport PlayState. Zero is Stopped, so a zeroed voice is an idle one. */
enum { BOOT_AVP_STOPPED=0, BOOT_AVP_STARTED=1, BOOT_AVP_STOPPING=2, BOOT_AVP_PAUSED=3 };

typedef struct BootAvpWave {        /* nxport WaveBuffer: the fields that time playback */
    int32_t start, end;             /* sample offsets of the buffer */
    uint32_t loop_start, loop_end;  /* sample offsets of the loop region (both nonzero to be used) */
    int32_t loop_count;             /* extra passes when looping; negative loops until stopped */
    uint8_t looping, end_of_stream;
    uint8_t queued;                 /* taken from the client but not yet handed to the channels
                                       (nxport sent_to_dsp == false) */
    uint8_t unused;
} BootAvpWave;

typedef struct BootAvpVoice {       /* nxport Voice */
    uint8_t in_use, is_new, play_state, channel_count;
    uint8_t reset_played_on_loop, skip_src, unused[2];
    uint32_t sample_rate;
    float pitch;
    uint32_t wave_buffers_index;    /* the client's current slot: where a flush starts */
    uint32_t flush_count;           /* flushes requested since the last rendered frame */
    uint32_t channel_ids[6];
    BootAvpWave waves[BOOT_AVP_SLOTS];
} BootAvpVoice;

typedef struct BootAvpChannel {     /* nxport ChannelState (Ryujinx VoiceUpdateState): sample accounting */
    uint64_t played_sample_count;
    uint32_t offset;                /* samples played of the current wave buffer (or of its loop region) */
    uint32_t wave_buffer_index;     /* slot being played */
    uint32_t wave_buffer_consumed;
    int32_t loop_count;             /* passes completed over the current wave buffer */
    float fraction;                 /* source position below one sample, carried to the next frame */
    uint8_t valid[BOOT_AVP_SLOTS];  /* slot holds a buffer waiting to be played */
} BootAvpChannel;

typedef struct BootAvpState {
    uint32_t tag, key;
    uint32_t frames;                /* renderer frames rendered (diagnostic only) */
    uint32_t unused;
    BootAvpVoice voices[BOOT_AVP_VOICES];
    BootAvpChannel channels[BOOT_AVP_CHANNELS];
} BootAvpState;

static inline uint32_t boot_avp_read32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint16_t boot_avp_read16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

/* nxport mark_end_of_buffer: the channel is done with the slot it was playing. */
static inline void boot_avp_finish_buffer(BootAvpChannel *ch, const BootAvpWave *wave) {
    ch->valid[ch->wave_buffer_index] = 0;
    ch->loop_count = 0;
    ch->wave_buffer_consumed++;
    ch->wave_buffer_index = (ch->wave_buffer_index + 1) % BOOT_AVP_SLOTS;
    if (wave->end_of_stream) ch->played_sample_count = 0;
}

/* nxport process_wave_buffers, sample accounting only: consume fraction + ratio * frame source samples
 * across the channel's valid wave buffers. Guards nxport does not need (it trusts the client): a ratio
 * that is not positive (zero, negative or NaN pitch) consumes nothing, the per-frame demand is capped at
 * 2^30 samples, and a channel walks at most BOOT_AVP_STEP_LIMIT segments per frame, which only an
 * endlessly looping region shorter than a few samples could reach (it then resumes next frame). */
static inline void boot_avp_consume(BootAvpChannel *ch, const BootAvpWave *waves, float ratio, uint32_t frame,
                                    int reset_on_loop) {
    if (!(ratio > 0.0f)) return;
    float wanted = ch->fraction + ratio * (float)frame;
    if (!(wanted < 1073741824.0f)) wanted = 1073741824.0f;
    uint32_t needed = (uint32_t)wanted;
    ch->fraction = wanted - (float)needed;
    uint32_t got = 0;
    for (uint32_t step = 0; got < needed && step < BOOT_AVP_STEP_LIMIT; ++step) {
        ch->wave_buffer_index %= BOOT_AVP_SLOTS;
        if (!ch->valid[ch->wave_buffer_index]) break;          /* starving until the client appends more */
        const BootAvpWave *wave = &waves[ch->wave_buffer_index];
        int64_t first = wave->start, last = wave->end;
        if (ch->loop_count > 0 && wave->loop_start != 0 && wave->loop_end != 0 && wave->loop_start <= wave->loop_end) {
            first = wave->loop_start; last = wave->loop_end;  /* passes after the first play the loop region */
        }
        uint32_t total = last <= first ? 0 : last - first > 0x7fffffff ? 0x7fffffffu : (uint32_t)(last - first);
        uint32_t take = total > ch->offset ? total - ch->offset : 0;
        if (take > needed - got) take = needed - got;
        got += take;
        ch->offset += take;
        ch->played_sample_count += take;
        if (ch->offset >= total) {                             /* end of the buffer, or of one loop pass */
            ch->offset = 0;
            if (wave->looping) {
                ch->loop_count++;
                if (wave->loop_count >= 0 && (take == 0 || ch->loop_count > wave->loop_count))
                    boot_avp_finish_buffer(ch, wave);
                if (take == 0) break;                          /* empty region: nothing more this frame */
                if (reset_on_loop) ch->played_sample_count = 0;
            } else {
                boot_avp_finish_buffer(ch, wave);
            }
        }
    }
}

/* nxport render_frame: command generation for every voice in use, then the wave-buffer walk of every
 * channel of every started voice. Channel ids outside the state are skipped. */
static inline void boot_avp_render_frame(BootAvpState *s, uint32_t frame, uint32_t target_rate) {
    s->frames++;
    for (uint32_t v = 0; v < BOOT_AVP_VOICES; ++v) {
        BootAvpVoice *voice = &s->voices[v];
        if (!voice->in_use) continue;
        BootAvpChannel *chans[6];
        uint32_t count = 0;
        for (uint32_t c = 0; c < voice->channel_count && c < 6; ++c)
            if (voice->channel_ids[c] < BOOT_AVP_CHANNELS) chans[count++] = &s->channels[voice->channel_ids[c]];
        if (voice->is_new) {                                   /* first frame of a new voice: fresh channels */
            for (uint32_t c = 0; c < count; ++c) memset(chans[c], 0, sizeof *chans[c]);
            voice->is_new = 0;
        }
        if (voice->flush_count) {
            /* Flush from the client's current slot; each flushed buffer still queued counts as consumed.
             * nxport loops flush_count times, but every pass after the fourth finds its slot already
             * flushed, so four passes give the same result. */
            uint32_t slot = voice->wave_buffers_index % BOOT_AVP_SLOTS;
            uint32_t passes = voice->flush_count < BOOT_AVP_SLOTS ? voice->flush_count : BOOT_AVP_SLOTS;
            for (uint32_t n = 0; n < passes; ++n, slot = (slot + 1) % BOOT_AVP_SLOTS) {
                voice->waves[slot].queued = 0;
                for (uint32_t c = 0; c < count; ++c) {
                    BootAvpChannel *ch = chans[c];
                    if (!ch->valid[slot]) continue;
                    ch->valid[slot] = 0;
                    ch->wave_buffer_consumed++;
                    ch->wave_buffer_index = (ch->wave_buffer_index + 1) % BOOT_AVP_SLOTS;
                }
            }
            voice->flush_count = 0;
        }
        if (voice->play_state == BOOT_AVP_STARTED) {
            for (uint32_t w = 0; w < BOOT_AVP_SLOTS; ++w) {    /* hand newly appended buffers to the channels */
                if (!voice->waves[w].queued) continue;
                voice->waves[w].queued = 0;
                for (uint32_t c = 0; c < count; ++c) chans[c]->valid[w] = 1;
            }
            float ratio = voice->skip_src ? 1.0f : (float)voice->sample_rate / (float)target_rate * voice->pitch;
            for (uint32_t c = 0; c < count; ++c)
                boot_avp_consume(chans[c], voice->waves, ratio, frame, voice->reset_played_on_loop);
        } else if (voice->play_state == BOOT_AVP_STOPPING) {
            /* Every buffer still queued is released (counted as consumed) and the position restarts. */
            for (uint32_t w = 0; w < BOOT_AVP_SLOTS; ++w) {
                voice->waves[w].queued = 0;
                for (uint32_t c = 0; c < count; ++c) {
                    BootAvpChannel *ch = chans[c];
                    if (ch->valid[w]) {
                        ch->wave_buffer_consumed++;
                        ch->wave_buffer_index = (ch->wave_buffer_index + 1) % BOOT_AVP_SLOTS;
                    }
                    ch->valid[w] = 0;
                }
            }
            for (uint32_t c = 0; c < count; ++c) {
                chans[c]->offset = 0;
                chans[c]->played_sample_count = 0;
                chans[c]->fraction = 0.0f;
            }
            voice->play_state = BOOT_AVP_STOPPED;
        }
    }
}

/* nxport update_voice: take one VoiceInParameter (REV11 layout) into the voice state. */
static inline void boot_avp_take_voice(BootAvpState *s, BootAvpVoice *voice, const uint8_t *in) {
    voice->in_use = in[0x09] != 0;
    if (!voice->in_use) return;
    int fresh = in[0x08] != 0;
    if (fresh) {                                               /* a new voice starts idle with empty slots */
        memset(voice, 0, sizeof *voice);
        voice->in_use = 1;
        voice->is_new = 1;
    }
    switch (in[0x0A]) {                                        /* requested play state */
        case 0: voice->play_state = BOOT_AVP_STARTED; break;
        case 1: if (voice->play_state != BOOT_AVP_STOPPED) voice->play_state = BOOT_AVP_STOPPING; break;
        default: voice->play_state = BOOT_AVP_PAUSED; break;
    }
    voice->sample_rate = boot_avp_read32(in + 0x0C);
    uint32_t channels = boot_avp_read32(in + 0x18);
    voice->channel_count = (uint8_t)(channels < 6 ? channels : 6);
    uint32_t pitch_bits = boot_avp_read32(in + 0x1C);
    memcpy(&voice->pitch, &pitch_bits, 4);
    voice->wave_buffers_index = boot_avp_read32(in + 0x40);
    for (uint32_t c = 0; c < 6; ++c) voice->channel_ids[c] = boot_avp_read32(in + 0x140 + 4 * c);
    voice->flush_count += in[0x159];
    uint16_t behaviour = boot_avp_read16(in + 0x15C);
    voice->reset_played_on_loop = behaviour & 1;              /* PlayedSampleCountResetAtLoopPoint */
    voice->skip_src = (behaviour >> 1) & 1;                    /* SkipPitchAndSampleRateConversion */
    if (fresh)
        for (uint32_t c = 0; c < voice->channel_count; ++c)
            if (voice->channel_ids[c] < BOOT_AVP_CHANNELS) memset(s->channels[voice->channel_ids[c]].valid, 0, BOOT_AVP_SLOTS);
    for (uint32_t w = 0; w < BOOT_AVP_SLOTS; ++w) {            /* WaveBuffer, 0x38 each */
        const uint8_t *b = in + 0x60 + 0x38 * w;
        if (b[0x1A]) continue;                                 /* already sent to the server earlier */
        BootAvpWave *wave = &voice->waves[w];
        wave->start = (int32_t)boot_avp_read32(b + 0x10);
        wave->end = (int32_t)boot_avp_read32(b + 0x14);
        wave->looping = b[0x18] != 0;
        wave->end_of_stream = b[0x19] != 0;
        wave->loop_count = (int32_t)boot_avp_read32(b + 0x1C);
        wave->loop_start = boot_avp_read32(b + 0x30);
        wave->loop_end = boot_avp_read32(b + 0x34);
        wave->queued = 1;
    }
}

/* nxport write_voice_status: the voice's first channel speaks for the voice. A voice that is not in use,
 * is new this update or has no channel reports zeros; voices are never dropped (VoiceDropFlag 0). */
static inline void boot_avp_write_status(const BootAvpState *s, const BootAvpVoice *voice, uint8_t *out) {
    memset(out, 0, BOOT_AVP_VOICE_OUT);
    if (!voice->in_use || voice->is_new || !voice->channel_count || voice->channel_ids[0] >= BOOT_AVP_CHANNELS) return;
    const BootAvpChannel *ch = &s->channels[voice->channel_ids[0]];
    memcpy(out, &ch->played_sample_count, 8);
    memcpy(out + 8, &ch->wave_buffer_consumed, 4);
}

/* One RequestUpdate. `input`/`output` are the update buffers (header and sizes checked by the caller);
 * `work` is the renderer work buffer; `started`: the renderer is started; `frame` output samples per
 * renderer frame at `target_rate` Hz. Returns 1, or 0 with nothing written when the work buffer is too
 * small for the state or the header's voice section is malformed. */
static inline int boot_audren_update(uint8_t *work, size_t work_size, uint32_t key, const uint8_t *input,
                                     uint8_t *output, int started, uint32_t frame, uint32_t target_rate) {
    if (!work || work_size < sizeof(BootAvpState) || !input || !output) return 0;
    /* UpdateDataHeader: section sizes in header order; in the data the voice channel resources come
     * before the voices. The reply has 0x10 bytes per memory pool before the voice statuses. */
    uint32_t behaviour_size = boot_avp_read32(input + 0x04), pool_size = boot_avp_read32(input + 0x08);
    uint32_t voice_size = boot_avp_read32(input + 0x0C), resource_size = boot_avp_read32(input + 0x10);
    uint64_t voices_at = 0x40u + (uint64_t)behaviour_size + pool_size + resource_size;
    if (voice_size % BOOT_AVP_VOICE_IN || voices_at + voice_size > boot_avp_read32(input + 0x3C)) return 0;
    uint32_t voice_count = voice_size / BOOT_AVP_VOICE_IN;
    if (voice_count > BOOT_AVP_VOICES) voice_count = BOOT_AVP_VOICES;
    uint8_t *status = output + 0x40 + (size_t)(pool_size / 0x20) * 0x10;

    BootAvpState *s = (BootAvpState *)(void *)work;
    if (s->tag != BOOT_AVP_TAG || s->key != key) {
        memset(s, 0, sizeof *s);
        s->tag = BOOT_AVP_TAG;
        s->key = key;
    }
    if (started && frame && target_rate) boot_avp_render_frame(s, frame, target_rate);
    for (uint32_t v = 0; v < voice_count; ++v) {
        boot_avp_take_voice(s, &s->voices[v], input + voices_at + (size_t)v * BOOT_AVP_VOICE_IN);
        boot_avp_write_status(s, &s->voices[v], status + (size_t)v * BOOT_AVP_VOICE_OUT);
    }
    return 1;
}
#endif
