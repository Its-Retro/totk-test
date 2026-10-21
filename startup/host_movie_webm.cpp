// WebM demux shim for host_movie.h: the weak tm_webm_* API over libwebm 1.0.0.32 mkvparser (BSD,
// startup/third_party/libwebm). Built by startup/third_party/build_webm.py into webm-build/libtotk_movie_webm.a
// (linked whole-archive) with -fno-exceptions -fno-rtti -fno-threadsafe-statics: no C++ runtime library is
// linked into the game DLL, so the few runtime pieces mkvparser needs (operator new/delete, the pure-virtual
// handler) are defined here on top of the C heap.
#define TOTK_HOST_MOVIE_SHIM 1
#include "host_movie.h"
#include <cstdlib>
#include <cstring>
#include <new>
#include "mkvparser/mkvparser.h"

void *operator new(std::size_t n) { void *p = std::malloc(n ? n : 1); if (!p) std::abort(); return p; }
void *operator new[](std::size_t n) { void *p = std::malloc(n ? n : 1); if (!p) std::abort(); return p; }
void *operator new(std::size_t n, const std::nothrow_t &) noexcept { return std::malloc(n ? n : 1); }
void *operator new[](std::size_t n, const std::nothrow_t &) noexcept { return std::malloc(n ? n : 1); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
extern "C" void __cxa_pure_virtual() { std::abort(); }
namespace std { const nothrow_t nothrow{}; }   // libc++ defines it in its (unlinked) library

namespace {
class Reader : public mkvparser::IMkvReader {
 public:
  Reader(TmReadFn fn, void *user, long long size) : fn_(fn), user_(user), size_(size) {}
  virtual ~Reader() {}
  int Read(long long pos, long len, unsigned char *buf) override {
    if (pos < 0 || len < 0 || pos > size_ || len > size_ - pos) return -1;
    return len == 0 || fn_(user_, pos, (int32_t)len, buf) == 0 ? 0 : -1;
  }
  int Length(long long *total, long long *available) override {
    if (total) *total = size_;
    if (available) *available = size_;
    return 0;
  }
 private:
  TmReadFn fn_; void *user_; long long size_;
};
struct Webm {
  Reader reader;
  mkvparser::Segment *segment;
  const mkvparser::Cluster *cluster;
  const mkvparser::BlockEntry *entry;   // NULL: before the cluster's first entry
  int frame;                            // next frame of entry's block
  Webm(TmReadFn fn, void *user, long long size) : reader(fn, user, size), segment(NULL), cluster(NULL), entry(NULL), frame(0) {}
};
}  // namespace

extern "C" void *tm_webm_open(TmReadFn read, void *user, int64_t size, int32_t *error) {
  Webm *w = new (std::nothrow) Webm(read, user, size);
  if (!w) { if (error) *error = -1; return NULL; }
  long long pos = 0;
  mkvparser::EBMLHeader header;
  long long r = header.Parse(&w->reader, pos);
  if (r >= 0) r = mkvparser::Segment::CreateInstance(&w->reader, pos, w->segment);
  if (r == 0 && w->segment) r = w->segment->Load();   // headers and every cluster header (blocks stay lazy)
  if (r < 0 || !w->segment || !w->segment->GetTracks()) {
    if (error) *error = (int32_t)(r < 0 ? r : -2);
    delete w->segment; delete w;
    return NULL;
  }
  w->cluster = w->segment->GetFirst();
  if (error) *error = 0;
  return w;
}
extern "C" int32_t tm_webm_tracks(void *p, TmWebmTrack *out, int32_t cap) {
  Webm *w = static_cast<Webm *>(p);
  const mkvparser::Tracks *tracks = w->segment->GetTracks();
  int32_t n = 0;
  for (unsigned long i = 0; i < tracks->GetTracksCount() && n < cap; ++i) {
    const mkvparser::Track *t = tracks->GetTrackByIndex(i);
    if (!t) continue;
    TmWebmTrack &o = out[n++];
    std::memset(&o, 0, sizeof o);
    o.number = (int32_t)t->GetNumber();
    o.type = (int32_t)t->GetType();   // 1 video, 2 audio
    o.default_duration_ns = (int64_t)t->GetDefaultDuration();
    const char *id = t->GetCodecId();
    if (id) std::strncpy(o.codec_id, id, sizeof o.codec_id - 1);
    if (o.type == mkvparser::Track::kVideo) {
      const mkvparser::VideoTrack *v = static_cast<const mkvparser::VideoTrack *>(t);
      o.width = (int32_t)v->GetWidth(); o.height = (int32_t)v->GetHeight();
    }
  }
  return n;
}
extern "C" int64_t tm_webm_duration_ns(void *p) {
  const mkvparser::SegmentInfo *info = static_cast<Webm *>(p)->segment->GetInfo();
  return info ? (int64_t)info->GetDuration() : 0;
}
extern "C" int32_t tm_webm_next(void *p, TmWebmFrame *f) {
  Webm *w = static_cast<Webm *>(p);
  for (;;) {
    if (!w->cluster || w->cluster->EOS()) return 0;
    if (!w->entry) {
      const mkvparser::BlockEntry *first = NULL;
      if (w->cluster->GetFirst(first) < 0) return -1;
      if (!first) { w->cluster = w->segment->GetNext(w->cluster); continue; }
      w->entry = first; w->frame = 0;
    }
    if (w->entry->EOS()) { w->cluster = w->segment->GetNext(w->cluster); w->entry = NULL; continue; }
    const mkvparser::Block *b = w->entry->GetBlock();
    if (b && w->frame < b->GetFrameCount()) {
      const mkvparser::Block::Frame &fr = b->GetFrame(w->frame++);
      f->track = (int32_t)b->GetTrackNumber();
      f->key = b->IsKey() ? 1 : 0;
      f->time_ns = (int64_t)b->GetTime(w->cluster);
      f->pos = (int64_t)fr.pos;
      f->size = (int32_t)fr.len;
      f->pad = 0;
      return 1;
    }
    const mkvparser::BlockEntry *next = NULL;
    if (w->cluster->GetNext(w->entry, next) < 0) return -1;
    w->frame = 0;
    if (next) w->entry = next;
    else { w->cluster = w->segment->GetNext(w->cluster); w->entry = NULL; }
  }
}
extern "C" int32_t tm_webm_rewind(void *p, int64_t time_ns) {
  Webm *w = static_cast<Webm *>(p);
  const mkvparser::Cluster *c = w->segment->GetFirst(), *pick = c;
  while (time_ns > 0 && c && !c->EOS() && c->GetTime() <= time_ns) { pick = c; c = w->segment->GetNext(c); }
  w->cluster = pick; w->entry = NULL; w->frame = 0;
  return pick && !pick->EOS() ? 0 : -1;
}
extern "C" void tm_webm_close(void *p) {
  Webm *w = static_cast<Webm *>(p);
  if (!w) return;
  delete w->segment;
  delete w;
}
