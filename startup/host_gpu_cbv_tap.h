#ifndef TOTK_HOST_GPU_CBV_TAP_H
#define TOTK_HOST_GPU_CBV_TAP_H
/* Shared88/89 diagnostic tap. Registered once before renderer use, never from
 * a producer. No D3D/guest/file/heap operations. NULL observers are the default.
 * A callback may copy only validated owned-ring bytes, while this scope lives. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define BOOT_CBV_TAP_OBSERVERS 2u
#define BOOT_CBV_TAP_DEPTH 4u
enum { BOOT_CBV_TAP_88=0, BOOT_CBV_TAP_89=1 };
enum { BOOT_CBV_TAP_UNKNOWN=0, BOOT_CBV_TAP_DIRECT_REGION=1,
       BOOT_CBV_TAP_RING_FRESH=2, BOOT_CBV_TAP_RING_CACHE=3,
       BOOT_CBV_TAP_RING_ZERO=4, BOOT_CBV_TAP_RING_PATCHED=5,
       BOOT_CBV_TAP_HOST_FRESH=6, BOOT_CBV_TAP_HOST_CACHE=7 };
enum { BOOT_CBV_TAP_FLAG_PATCHED=1u, BOOT_CBV_TAP_FLAG_OVERLAY=2u };
enum { BOOT_CBV_TAP_FAULT_DEPTH=1u, BOOT_CBV_TAP_FAULT_TOKEN=2u, BOOT_CBV_TAP_FAULT_SEQUENCE=3u };
typedef struct BootCbvTapTarget {
    uint32_t width,height,format,state,layer,layers,pitch,map;
} BootCbvTapTarget;
typedef struct BootCbvTapBegin {
    uintptr_t renderer,draw,pso; uint8_t vs[32],ps[32];
    uint64_t token,sub,rt_address[8]; uintptr_t rt_resource[8];
    BootCbvTapTarget target[8];
    uint32_t event,count,instances,indexed,topology,vs_mask,rt_count,samples;
} BootCbvTapBegin;
typedef struct BootCbvTapCv { uint64_t location; uint32_t size; } BootCbvTapCv;
typedef struct BootCbvTapBinding {
    uintptr_t renderer,draw,resource; uint64_t token,requested_va,location,offset,resource_bytes,scope,list,map_generation;
    uint32_t requested_size,valid,stage,group,slot,size,mode,region,flags;
    const uint8_t *owned; size_t owned_bytes; /* private, immediate callback only */
} BootCbvTapBinding;
typedef struct BootCbvTapTable {
    uintptr_t renderer,draw; uint64_t token,scope,list,map_generation;
    uint32_t stage,group,base,reused;
} BootCbvTapTable;
typedef struct BootCbvTapObserver {
    unsigned (*begin)(const BootCbvTapBegin *);
    void (*binding)(const BootCbvTapBinding *);
    void (*table)(const BootCbvTapTable *,const BootCbvTapCv[19]);
    void (*end)(uint64_t,uintptr_t,unsigned); /* aborted=1 invalidates resolver scope */
    void (*issued)(uint64_t,uintptr_t,unsigned); /* after original Draw, or later pre-Draw abort */
    void (*fault)(unsigned); /* invalidate observer's pending TLS on untrusted nesting */
} BootCbvTapObserver;
typedef struct BootCbvTapScope {
    uint64_t token; uintptr_t renderer,draw; uint32_t active;
    BootCbvTapBinding request;
} BootCbvTapScope;
typedef struct BootCbvTapTls {
    uint32_t depth,sequence,disabled; uint64_t overflow,mismatch;
    BootCbvTapScope scopes[BOOT_CBV_TAP_DEPTH];
} BootCbvTapTls;
static BootCbvTapObserver boot_cbv_tap_observer[BOOT_CBV_TAP_OBSERVERS];
static unsigned boot_cbv_tap_registered;
static _Thread_local BootCbvTapTls boot_cbv_tap_tls;
/* Startup-only publication contract: callers initialize complete callback state
 * before registration; both registrations finish before any renderer use. */
static inline int boot_cbv_tap_register(unsigned index,const BootCbvTapObserver *observer) {
    if(index>=BOOT_CBV_TAP_OBSERVERS || !observer || !observer->begin || boot_cbv_tap_observer[index].begin) return 0;
    boot_cbv_tap_observer[index]=*observer; boot_cbv_tap_registered=1; return 1;
}
static inline BootCbvTapScope *boot_cbv_tap_current(void) {
    uint32_t depth=boot_cbv_tap_tls.depth;
    return depth && depth<=BOOT_CBV_TAP_DEPTH ? &boot_cbv_tap_tls.scopes[depth-1] : NULL;
}
static inline unsigned boot_cbv_tap_active(void) {
    if(!boot_cbv_tap_registered || boot_cbv_tap_tls.disabled) return 0;
    BootCbvTapScope *s=boot_cbv_tap_current(); return s?s->active:0;
}
static inline uint64_t boot_cbv_tap_begin(const BootCbvTapBegin *begin) {
    if(!boot_cbv_tap_registered || boot_cbv_tap_tls.disabled) return 0;
    uint32_t depth=++boot_cbv_tap_tls.depth,sequence=++boot_cbv_tap_tls.sequence;
    uint64_t token=((uint64_t)sequence<<32)|depth;
    if(depth>BOOT_CBV_TAP_DEPTH || !sequence) {
        ++boot_cbv_tap_tls.overflow;
        if(!sequence) boot_cbv_tap_tls.disabled=1; /* never reuse an earlier token */
        for(unsigned i=0;i<BOOT_CBV_TAP_OBSERVERS;++i)
            if(boot_cbv_tap_observer[i].fault) boot_cbv_tap_observer[i].fault(depth>BOOT_CBV_TAP_DEPTH?BOOT_CBV_TAP_FAULT_DEPTH:BOOT_CBV_TAP_FAULT_SEQUENCE);
        if(depth<=BOOT_CBV_TAP_DEPTH) { BootCbvTapScope *bad=&boot_cbv_tap_tls.scopes[depth-1]; memset(bad,0,sizeof *bad); bad->token=token; }
        return token;
    }
    BootCbvTapScope *s=&boot_cbv_tap_tls.scopes[depth-1]; memset(s,0,sizeof *s);
    s->token=token; s->renderer=begin->renderer; s->draw=begin->draw;
    BootCbvTapBegin with_token=*begin; with_token.token=token;
    for(unsigned i=0;i<BOOT_CBV_TAP_OBSERVERS;++i)
        if(boot_cbv_tap_observer[i].begin && boot_cbv_tap_observer[i].begin(&with_token)) s->active|=1u<<i;
    return token;
}
static inline void boot_cbv_tap_request(uint32_t stage,uint32_t group,uint32_t slot,uint64_t va,uint32_t size,uint32_t valid) {
    if(!boot_cbv_tap_registered || boot_cbv_tap_tls.disabled) return;
    BootCbvTapScope *s=boot_cbv_tap_current(); if(!s || !s->active) return;
    memset(&s->request,0,sizeof s->request); s->request.renderer=s->renderer; s->request.draw=s->draw;
    s->request.token=s->token;
    s->request.stage=stage; s->request.group=group; s->request.slot=slot;
    s->request.requested_va=va; s->request.requested_size=size; s->request.valid=valid;
}
static inline void boot_cbv_tap_resolved(uint64_t location,uint32_t size,const void *resource,uint64_t offset,uint64_t resource_bytes,
                                         uint32_t mode,uint32_t region,uint64_t scope,uint64_t list,uint64_t map_generation,
                                         uint32_t flags,const uint8_t *owned,size_t owned_bytes) {
    if(!boot_cbv_tap_registered || boot_cbv_tap_tls.disabled) return;
    BootCbvTapScope *s=boot_cbv_tap_current(); if(!s || !s->active) return;
    BootCbvTapBinding b=s->request; b.location=location; b.size=size; b.resource=(uintptr_t)resource;
    b.offset=offset; b.resource_bytes=resource_bytes; b.mode=mode; b.region=region;
    b.scope=scope; b.list=list; b.map_generation=map_generation; b.flags=flags;
    /* Only existing fenced upload-ring storage is offered; direct regions never
     * expose a CPU pointer. Reject overflowing/short spans without widening. */
    if(mode>=BOOT_CBV_TAP_RING_FRESH && mode<=BOOT_CBV_TAP_HOST_CACHE && owned &&
       offset<=resource_bytes && size<=resource_bytes-offset && owned_bytes>=size) {
        b.owned=owned; b.owned_bytes=size;
    }
    for(unsigned i=0;i<BOOT_CBV_TAP_OBSERVERS;++i)
        if((s->active&(1u<<i)) && boot_cbv_tap_observer[i].binding) boot_cbv_tap_observer[i].binding(&b);
}
static inline int boot_cbv_tap_copy_owned(const BootCbvTapBinding *b,size_t offset,size_t bytes,void *out) {
    if(!b || !out || !b->owned || b->mode<BOOT_CBV_TAP_RING_FRESH || b->mode>BOOT_CBV_TAP_HOST_CACHE ||
       offset>b->size || bytes>b->size-offset || offset>b->owned_bytes || bytes>b->owned_bytes-offset) return 0;
    memcpy(out,b->owned+offset,bytes); return 1;
}
static inline void boot_cbv_tap_table(uint32_t stage,uint32_t group,uint32_t base,uint32_t reused,
                                      uint64_t scope,uint64_t list,uint64_t map_generation,const BootCbvTapCv actual[19]) {
    if(!boot_cbv_tap_registered || boot_cbv_tap_tls.disabled) return;
    BootCbvTapScope *s=boot_cbv_tap_current(); if(!s || !s->active) return;
    BootCbvTapTable table={s->renderer,s->draw,s->token,scope,list,map_generation,stage,group,base,reused};
    for(unsigned i=0;i<BOOT_CBV_TAP_OBSERVERS;++i)
        if((s->active&(1u<<i)) && boot_cbv_tap_observer[i].table) boot_cbv_tap_observer[i].table(&table,actual);
}
static inline void boot_cbv_tap_end(uint64_t token,unsigned aborted) {
    if(!token || !boot_cbv_tap_registered) return;
    if(boot_cbv_tap_tls.disabled) aborted=1;
    uint32_t depth=boot_cbv_tap_tls.depth;
    if(!depth || (uint32_t)token!=depth || (depth<=BOOT_CBV_TAP_DEPTH && boot_cbv_tap_tls.scopes[depth-1].token!=token)) {
        ++boot_cbv_tap_tls.mismatch;
        boot_cbv_tap_tls.disabled=1; /* broken nesting is not silently repaired */
        for(unsigned i=0;i<BOOT_CBV_TAP_OBSERVERS;++i)
            if(boot_cbv_tap_observer[i].fault) boot_cbv_tap_observer[i].fault(BOOT_CBV_TAP_FAULT_TOKEN);
        /* No older-context recovery. Invalidate every supported active scope. */
        for(unsigned d=0;d<depth && d<BOOT_CBV_TAP_DEPTH;++d) {
            BootCbvTapScope *s=&boot_cbv_tap_tls.scopes[d];
            for(unsigned i=0;i<BOOT_CBV_TAP_OBSERVERS;++i)
                if((s->active&(1u<<i)) && boot_cbv_tap_observer[i].end) boot_cbv_tap_observer[i].end(s->token,s->draw,1);
            s->active=0;
        }
        boot_cbv_tap_tls.depth=0; return;
    }
    if(depth<=BOOT_CBV_TAP_DEPTH) {
        BootCbvTapScope *s=&boot_cbv_tap_tls.scopes[depth-1];
        for(unsigned i=0;i<BOOT_CBV_TAP_OBSERVERS;++i)
            if((s->active&(1u<<i)) && boot_cbv_tap_observer[i].end) boot_cbv_tap_observer[i].end(s->token,s->draw,aborted);
        s->active=0;
    }
    --boot_cbv_tap_tls.depth;
}
static inline void boot_cbv_tap_issued(uint64_t token,uintptr_t draw,unsigned issued) {
    if(!token || !boot_cbv_tap_registered) return;
    if(boot_cbv_tap_tls.disabled) issued=0;
    for(unsigned i=0;i<BOOT_CBV_TAP_OBSERVERS;++i)
        if(boot_cbv_tap_observer[i].issued) boot_cbv_tap_observer[i].issued(token,draw,issued);
}
#endif
