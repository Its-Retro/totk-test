#ifndef TOTK_HOST_LIGHT_COPY_PROBE_CORE_H
#define TOTK_HOST_LIGHT_COPY_PROBE_CORE_H
#include <stdint.h>
#include <string.h>
#include <limits.h>
#define BOOT_L84_BATCHES 41u
#define BOOT_L84_STRIPS 3u
#define BOOT_L84_ROWS 12u
#define BOOT_L84_ROW_BYTES 256u
#define BOOT_L84_PLACEMENT 512u
#define BOOT_L84_BATCH_BYTES (BOOT_L84_ROWS*BOOT_L84_PLACEMENT)
#define BOOT_L84_BYTES (BOOT_L84_BATCHES*BOOT_L84_BATCH_BYTES)
#define BOOT_L84_MATCHES 256u
enum { BOOT_L84_UNUSED=0,BOOT_L84_RESERVED=1,BOOT_L84_RECORDED=2,BOOT_L84_FENCED=3,BOOT_L84_READING=4,BOOT_L84_DONE=5,BOOT_L84_ABORTED=6,BOOT_L84_EXPORTING=7,BOOT_L84_PERSISTED=8 };
enum { BOOT_L84_ABORT_CLOSE=1,BOOT_L84_ABORT_SIGNAL=2,BOOT_L84_ABORT_RESET=3,BOOT_L84_ABORT_DEVICE=4,BOOT_L84_ABORT_TIMEOUT=5,BOOT_L84_ABORT_MAP=6,BOOT_L84_ABORT_GENERATION=7,BOOT_L84_ABORT_PARTIAL=8 };
typedef struct BootLight84Config { uint64_t first,last,base,bytes; } BootLight84Config;
typedef struct BootLight84Source { uint64_t resource,address,sequence,index,generation; } BootLight84Source;
typedef struct BootLight84Batch {
    volatile LONG state,sub_done,draw;
    unsigned slot,tic,event,why,abort_reason,row_mask,segment;
    uint64_t sub,recording,execute,fence,completed,cl,renderer,device,array,address,qpc,queue,allocator,fence_object;
    BootLight84Source source[2];
} BootLight84Batch;
typedef struct BootLight84Match { uint64_t sub,array,address; unsigned event,tic,selected; } BootLight84Match;
static inline int boot_light84_number(const char **at,unsigned radix,uint64_t *out,char delimiter) {
    const char *p=*at; uint64_t n=0; unsigned digits=0;
    for(;*p && *p!=delimiter;++p) {
        unsigned d=*p>='0' && *p<='9'?(unsigned)(*p-'0'):*p>='a' && *p<='f'?(unsigned)(*p-'a'+10):*p>='A' && *p<='F'?(unsigned)(*p-'A'+10):99u;
        if(d>=radix || n>(UINT64_MAX-d)/radix) return 0; n=n*radix+d; ++digits;
    }
    if(!digits || *p!=delimiter) return 0;
    *out=n; *at=p+(delimiter!=0); return 1;
}
static inline int boot_light84_parse(const char *text,BootLight84Config *out) {
    BootLight84Config c={0}; const char *p=text;
    if(!p || !*p) return 0;
    if(!boot_light84_number(&p,10,&c.first,':') || !boot_light84_number(&p,10,&c.last,':') ||
       !boot_light84_number(&p,16,&c.base,':') || !boot_light84_number(&p,16,&c.bytes,0) ||
       !c.first || c.last<c.first || c.last-c.first>=BOOT_L84_BATCHES || !c.base || !c.bytes || c.bytes>(UINT64_C(64)<<20) || c.base>UINT64_MAX-c.bytes) return -1;
    *out=c; return 1;
}
static inline int boot_light84_in_window(const BootLight84Config *c,uint64_t sub,uint64_t address) {
    return sub>=c->first && sub<=c->last && address>=c->base && address-c->base<c->bytes;
}
static inline int boot_light84_shape(unsigned width,unsigned height,unsigned layers,unsigned levels,unsigned samples,unsigned format,
                                     uint64_t array,uint64_t source0,uint64_t source1) {
    return width==640 && height==360 && layers==2 && levels==1 && samples==1 && format==26 && array && source0 && source1 && source0!=source1 && array!=source0 && array!=source1;
}
static inline unsigned boot_light84_x(unsigned strip) { return strip==0?64:strip==1?288:512; }
static inline unsigned boot_light84_y(unsigned strip) { return strip==0?64:strip==1?180:300; }
static inline uint64_t boot_light84_offset(unsigned slot,unsigned phase,unsigned layer,unsigned strip) {
    return (uint64_t)slot*BOOT_L84_BATCH_BYTES+(uint64_t)(phase*6+layer*3+strip)*BOOT_L84_PLACEMENT;
}
/* The single render producer publishes immutable fields before these interlocked states.
 * A sub-end publication closes same-resolve-event draw observations before worker ownership. */
static inline int boot_light84_attach(BootLight84Batch *b,uint64_t renderer,uint64_t cl,uint64_t recording,uint64_t execute,uint64_t fence) {
    if(InterlockedCompareExchange(&b->state,0,0)!=BOOT_L84_RECORDED || b->renderer!=renderer || b->cl!=cl || b->recording!=recording || !fence || fence==UINT64_MAX) return 0;
    b->execute=execute; b->fence=fence; InterlockedExchange(&b->state,BOOT_L84_FENCED); return 1;
}
static inline int boot_light84_claim(BootLight84Batch *b,uint64_t completed) {
    if(!InterlockedCompareExchange(&b->sub_done,0,0) || !b->fence || completed==UINT64_MAX || completed<b->fence) return 0;
    return InterlockedCompareExchange(&b->state,BOOT_L84_READING,BOOT_L84_FENCED)==BOOT_L84_FENCED;
}
#endif
