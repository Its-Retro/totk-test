#ifndef TOTK_HOST_LIGHT_DRAW_PROBE_CORE_H
#define TOTK_HOST_LIGHT_DRAW_PROBE_CORE_H
#include <stdint.h>
#include <string.h>
#include <limits.h>
#define BOOT_L86_SUBS 41u
#define BOOT_L86_PER_SUB 8u
#define BOOT_L86_BATCHES (BOOT_L86_SUBS*BOOT_L86_PER_SUB)
#define BOOT_L86_STRIPS 3u
#define BOOT_L86_ROWS 12u
#define BOOT_L86_ROW_BYTES 256u
#define BOOT_L86_PLACEMENT 512u
#define BOOT_L86_BATCH_BYTES (BOOT_L86_ROWS*BOOT_L86_PLACEMENT)
#define BOOT_L86_BYTES (BOOT_L86_BATCHES*BOOT_L86_BATCH_BYTES)
#define BOOT_L86_MATCHES 4096u
#define BOOT_L86_VALUES 128u
enum { BOOT_L86_UNUSED=0,BOOT_L86_RESERVED=1,BOOT_L86_RECORDED=2,BOOT_L86_FENCED=3,BOOT_L86_READING=4,BOOT_L86_DONE=5,BOOT_L86_ABORTED=6,BOOT_L86_EXPORTING=7,BOOT_L86_PERSISTED=8 };
enum { BOOT_L86_ABORT_CLOSE=1,BOOT_L86_ABORT_SIGNAL=2,BOOT_L86_ABORT_RESET=3,BOOT_L86_ABORT_DEVICE=4,BOOT_L86_ABORT_TIMEOUT=5,BOOT_L86_ABORT_MAP=6,BOOT_L86_ABORT_GENERATION=7,BOOT_L86_ABORT_PARTIAL=8 };
typedef struct BootLight86Config { uint64_t first,last,address0,address1; } BootLight86Config;
typedef struct BootLight86Source { uint64_t resource,address,sequence,index,generation,width,height,format,state,layer,layers,pitch; } BootLight86Source;
typedef struct BootLight86Batch {
    volatile LONG state,sub_done,draw;
    unsigned slot,tic,event,why,abort_reason,row_mask,segment;
    uint64_t sub,recording,execute,fence,completed,cl,renderer,device,array,address,qpc,queue,allocator,fence_object;
    BootLight86Source source[3];
    uint64_t values[BOOT_L86_VALUES];
} BootLight86Batch;
typedef struct BootLight86Match { uint64_t sub,array,address; unsigned event,tic,selected; } BootLight86Match;
static inline int boot_light86_number(const char **at,unsigned radix,uint64_t *out,char delimiter) {
    const char *p=*at; uint64_t n=0; unsigned digits=0;
    for(;*p && *p!=delimiter;++p) {
        unsigned d=*p>='0' && *p<='9'?(unsigned)(*p-'0'):*p>='a' && *p<='f'?(unsigned)(*p-'a'+10):*p>='A' && *p<='F'?(unsigned)(*p-'A'+10):99u;
        if(d>=radix || n>(UINT64_MAX-d)/radix) return 0; n=n*radix+d; ++digits;
    }
    if(!digits || *p!=delimiter) return 0;
    *out=n; *at=p+(delimiter!=0); return 1;
}
static inline int boot_light86_parse(const char *text,BootLight86Config *out) {
    BootLight86Config c={0}; const char *p=text;
    if(!p || !*p) return 0;
    if(!boot_light86_number(&p,10,&c.first,':') || !boot_light86_number(&p,10,&c.last,':') ||
       !boot_light86_number(&p,16,&c.address0,':') || !boot_light86_number(&p,16,&c.address1,0) ||
       !c.first || c.last<c.first || c.last-c.first>=BOOT_L86_SUBS || !c.address0 || !c.address1 || c.address0==c.address1) return -1;
    *out=c; return 1;
}
static inline int boot_light86_in_window(const BootLight86Config *c,uint64_t sub,uint64_t address0,uint64_t address1) {
    return sub>=c->first && sub<=c->last && address0==c->address0 && address1==c->address1;
}
enum { BOOT_L86_SKIP_QUERY=1,BOOT_L86_SKIP_SHAPE=2,BOOT_L86_SKIP_GENERATION=3,BOOT_L86_SKIP_CAP=4 };
static inline int boot_light86_sources(const BootLight86Source source[3]) {
    for(unsigned i=0;i<3;++i) if(!source[i].resource || source[i].width!=640 || source[i].height!=360 ||
        source[i].format!=(i==2?28u:26u) || source[i].state!=4 || source[i].layer || source[i].layers!=1 || source[i].pitch || !source[i].generation) return 0;
    return source[0].resource!=source[1].resource && source[0].resource!=source[2].resource && source[1].resource!=source[2].resource;
}
static inline unsigned boot_light86_x(unsigned strip) { return strip==0?64:strip==1?288:512; }
static inline unsigned boot_light86_y(unsigned strip) { return strip==0?64:strip==1?180:300; }
static inline uint64_t boot_light86_offset(unsigned slot,unsigned row) {
    return (uint64_t)slot*BOOT_L86_BATCH_BYTES+(uint64_t)row*BOOT_L86_PLACEMENT;
}
/* The single render producer publishes immutable fields before these interlocked states.
 * A sub-end publication closes same-resolve-event draw observations before worker ownership. */
static inline int boot_light86_attach(BootLight86Batch *b,uint64_t renderer,uint64_t cl,uint64_t recording,uint64_t execute,uint64_t fence) {
    if(InterlockedCompareExchange(&b->state,0,0)!=BOOT_L86_RECORDED || b->renderer!=renderer || b->cl!=cl || b->recording!=recording || !fence || fence==UINT64_MAX) return 0;
    b->execute=execute; b->fence=fence; InterlockedExchange(&b->state,BOOT_L86_FENCED); return 1;
}
static inline int boot_light86_claim(BootLight86Batch *b,uint64_t completed) {
    if(!InterlockedCompareExchange(&b->sub_done,0,0) || !b->fence || completed==UINT64_MAX || completed<b->fence) return 0;
    return InterlockedCompareExchange(&b->state,BOOT_L86_READING,BOOT_L86_FENCED)==BOOT_L86_FENCED;
}
#endif
