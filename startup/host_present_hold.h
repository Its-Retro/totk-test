#ifndef TOTK_HOST_PRESENT_HOLD_H
#define TOTK_HOST_PRESENT_HOLD_H
/* Vsync-paced buffer release for the guest-dilated clock (guest_clock.h).
 *
 * On hardware a buffer queued to the display (IGraphicBufferProducer QUEUE_BUFFER) is latched at
 * a vsync and stays on screen for its swap interval; the producer gets it back (dequeue succeeds,
 * the producer event is signalled) only after that. The runtime presents synchronously, so in
 * wall mode it releases the buffer at once (unchanged). In dilated mode that made every frame
 * cost only its CPU instructions in guest time (no vsync wait anywhere), about 1/10 of a Switch
 * frame, so everything the game times with the system tick ran ~10x slower per frame.
 *
 * Here, in dilated mode only, a queued slot enters state 2 ("held"): not free, not dequeued. Its
 * release deadline is the next guest vsync plus (swap interval - 1) periods, and never earlier
 * than the previous held buffer's deadline plus one swap interval (one buffer per interval, like
 * the display). A guest-timed host event (a duplicate of the producer event) fires at the
 * deadline; the next dequeue turns every held slot whose deadline has passed back into a free
 * slot. When the guest blocks on the producer event and nothing else runs,
 * boot_guest_clock_idle fast-forwards the clock to that deadline at no real cost.
 *
 * Deadlines live in a per-DLL table (selectany, like the guest clock): a held slot the table does
 * not know (checkpoint restore, resume DLL swap, a lost timer) is released at once, and the first
 * clock update of a DLL releases every held slot and re-signals the producer event, so a guest
 * blocked on that event across a DLL swap cannot deadlock. No BootWindow / BootGraphicSlot layout
 * changes (state is a plain uint32_t; values 0 free, 1 dequeued, 2 held). Not page-visible.
 *
 * Lock order: w->lock, then the guest clock lock. The orphan sweep runs after the clock lock is
 * released and only try-acquires w->lock (callers of the clock may hold it). */
#define BOOT_GRAPHIC_SLOT_HELD 2u
typedef struct BootPresentHold {
    const void*window;            /* window the deadlines belong to */
    uint64_t release[2][64];      /* guest tick a held slot is released at; 0 = unknown */
    uint64_t chain[2];            /* deadline of the last held buffer, per producer */
    volatile uint32_t sweep;      /* 1: release orphaned held slots at the next clock update */
} BootPresentHold;
__declspec(selectany) BootPresentHold boot_present_hold={0,{{0}},{0},1};

/* Disabled with TOTK_PRESENT_HOLD=0 (then dilated mode releases at once, as before). */
static inline int boot_present_hold_enabled(void){
    static int enabled=-1;
    if(enabled<0){const char*v=getenv("TOTK_PRESENT_HOLD");enabled=!(v && !strcmp(v,"0"));}
    return enabled && boot_guest_dilated();
}
static inline uint64_t boot_present_now(void){
    AcquireSRWLockExclusive(&boot_guest_clock.lock);
    uint64_t now=boot_guest_clock.published;
    ReleaseSRWLockExclusive(&boot_guest_clock.lock);
    return now;
}
/* Producer event = "a dequeue would succeed" (w->lock held). */
static inline int boot_present_signal(BootWindow*w,unsigned index){
    if(index>=2 || !w->producer_events[index])return 1;
    return (boot_graphic_available(&w->graphic_buffers,index)?SetEvent(w->producer_events[index]):ResetEvent(w->producer_events[index]))!=0;
}
/* Release held slots: all of them (force), or those whose deadline passed or is unknown here.
 * Returns the number of slots released. w->lock held. */
static inline unsigned boot_present_release(BootWindow*w,unsigned index,int force){
    if(index>=2)return 0;
    int known=boot_present_hold.window==(const void*)w;
    uint64_t now=force?0:boot_present_now();unsigned released=0;
    for(unsigned i=0;i<64;++i){BootGraphicSlot*s=&w->graphic_buffers.slots[index][i];
        if(s->state!=BOOT_GRAPHIC_SLOT_HELD)continue;
        uint64_t due=known?boot_present_hold.release[index][i]:0;
        if(force || !due || due<=now){s->state=0;if(known)boot_present_hold.release[index][i]=0;++released;}
    }
    return released;
}
/* QUEUE_BUFFER of `slot` (state 1, just presented): hold it until its vsync deadline, or release
 * it now (wall mode, interval 0, no event, timer table full). Signals the producer event to
 * match. w->lock held. Returns 0 on a host failure. */
static inline int boot_present_queued(BootWindow*w,unsigned index,unsigned slot,uint32_t swap_interval){
    BootGraphicSlot*s=&w->graphic_buffers.slots[index][slot];
    if(!boot_present_hold_enabled() || !swap_interval || !w->producer_events[index]){
        s->state=0;
        return w->producer_events[index]?SetEvent(w->producer_events[index])!=0:1;
    }
    if(swap_interval>4)swap_interval=1;   /* not a plausible interval: assume 1 */
    if(boot_present_hold.window!=(const void*)w){memset(&boot_present_hold.release,0,sizeof boot_present_hold.release);
        boot_present_hold.chain[0]=boot_present_hold.chain[1]=0;boot_present_hold.window=w;}
    HANDLE event=NULL;uint64_t deadline=0;
    if(DuplicateHandle(GetCurrentProcess(),w->producer_events[index],GetCurrentProcess(),&event,0,FALSE,DUPLICATE_SAME_ACCESS)){
        deadline=boot_guest_present_deadline(&boot_present_hold.chain[index],swap_interval);
        if(!boot_guest_timer_at(event,deadline)){CloseHandle(event);deadline=0;}
    }
    if(!deadline){s->state=0;boot_present_hold.release[index][slot]=0;return SetEvent(w->producer_events[index])!=0;}
    s->state=BOOT_GRAPHIC_SLOT_HELD;boot_present_hold.release[index][slot]=deadline;
    return boot_present_signal(w,index);
}
/* DEQUEUE_BUFFER, before planning: free the held slots that are due. After a WOULD_BLOCK reply
 * (no slot selected) with buffers still held, keep the producer event reset so the guest blocks
 * until a release timer fires (instead of spinning). w->lock held. */
static inline void boot_present_before_dequeue(BootWindow*w,unsigned index){
    boot_present_release(w,index,0);
}
static inline int boot_present_would_block(BootWindow*w,unsigned index){
    if(index>=2 || !w->producer_events[index])return 1;
    int held=0;for(unsigned i=0;i<64;++i)held|=w->graphic_buffers.slots[index][i].state==BOOT_GRAPHIC_SLOT_HELD;
    if(!held || boot_graphic_available(&w->graphic_buffers,index))return 1;
    return ResetEvent(w->producer_events[index])!=0;
}
/* Orphan sweep after a DLL's first clock update (resume DLL swap, checkpoint restore): the timers
 * of held slots belonged to another clock instance. Releases every held slot and re-signals. */
static inline void boot_present_sweep(BootContext*c){
    if(!boot_present_hold.sweep)return;
    BootWindow*w=(BootWindow*)(uintptr_t)c->window_native;
    if(!w){boot_present_hold.sweep=0;return;}
    if(!TryAcquireSRWLockExclusive(&w->lock))return;   /* retried at the next clock update */
    for(unsigned i=0;i<2;++i)if(boot_present_release(w,i,1))boot_present_signal(w,i);
    boot_present_hold.sweep=0;
    ReleaseSRWLockExclusive(&w->lock);
}
#endif
