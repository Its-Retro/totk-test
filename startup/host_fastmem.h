#ifndef TOTK_HOST_FASTMEM_H
#define TOTK_HOST_FASTMEM_H
/* fastmem: direct guest memory access (design: startup/fm-work/DESIGN.md).
 *
 * Included by boot_runtime.h after BootContext. Compile switch TOTK_FASTMEM (default 1; 0 compiles the
 * previous TLB-only access path). Runtime switch: environment TOTK_FASTMEM, read once per process:
 *   1 (default)  direct mode: guest memory is backed by pagefile sections, mapped into one 512 GiB
 *                placeholder reservation (the "guest view", guest address a at boot_fm.base+a, host
 *                protections mirror guest permissions). Translated loads/stores (boot_load, boot_store,
 *                pairs, boot_store_bytes <=16 bytes) are one volatile host access there; a host access
 *                violation is converted into the guest fault by a vectored exception handler that
 *                unwinds the translated page function (as if it returned c->status).
 *   2            table mode: no sections, no guest view; boot_access (and every helper through it)
 *                uses a complete two-level page table (addend + permission per 4 KiB page) built from
 *                the region/alias/transfer tables. No misses, no region scans.
 *   0            previous software TLB path only.
 * Direct mode also maintains the table (host-side boot_access uses it; it returns Region.data based
 * pointers, which stay read-write for HLE code regardless of guest permissions).
 *
 * Sync: every region/alias/transfer change calls boot_tlb_flush(), which detaches the context from
 * the fast paths (table_ctx/direct_ctx=NULL, pending=ctx). The next slow-path access of that context
 * re-synchronizes incrementally (hash diff of the region table + override ranges) and re-attaches.
 * boot_valid (boot_run/boot_resume entry) marks the context pending, so host-side edits between runs
 * (Python, checkpoint restore) are picked up the same way.
 *
 * Region.data in direct mode always points into a host view of a section owned by boot_fm (or an
 * external section: HID shared memory, time page). Regions whose data is not (Python-owned buffers,
 * checkpoint-restored calloc/VirtualAlloc memory) are adopted at sync: copied into a new section and
 * Region.data rewritten. Python-owned (reserved 0) originals are written back at every stop
 * (boot_fault) and restored on boot_release_memory, so Python keeps seeing its buffers.
 * Single guest host thread (the multi-core work makes pc/page mark per core). */
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifndef TOTK_FASTMEM
#define TOTK_FASTMEM 1
#endif

#if TOTK_FASTMEM
#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#endif
#ifndef MEM_REPLACE_PLACEHOLDER
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#endif
#ifndef MEM_COALESCE_PLACEHOLDERS
#define MEM_COALESCE_PLACEHOLDERS 0x00000001
#endif
#ifndef MEM_PRESERVE_PLACEHOLDER
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#endif
#define BOOT_FM_SPACE_BITS 39
/* TOTK_FM_FIXED=1: the guest view is reserved at a fixed host address, so a translated access is
 * BOOT_FM_FIXED_BASE + guest address with no gate: no context compare, no base load, and core contexts
 * (TOTK_CORES>1, TOTK_THREADS=host) take the same direct path. Table changes are applied to the view
 * eagerly at every kernel and host-function exit (boot_fm_sync_pending) instead of lazily at the next
 * slow-path access; an access that still meets a stale page faults into the VEH, which applies a pending
 * sync and retries. Requires direct mode (TOTK_FASTMEM=1 and VirtualAlloc2): boot_mt_execute refuses to
 * run otherwise. Off by default: not yet run in the game. */
#ifndef TOTK_FM_FIXED
#define TOTK_FM_FIXED 1   /* play (46): on; host-thread workers (core contexts) otherwise take the out-of-line page table for every access */
#endif
#define BOOT_FM_FIXED_BASE UINT64_C(0x200000000000)   /* 32 TiB: no ASLR collision in 60 test processes (1 TiB: 15%) */
#define BOOT_FM_LEAVES (1u<<(BOOT_FM_SPACE_BITS-21))
/* Direct accesses need a+16 inside the reservation. */
#define BOOT_FM_DIRECT_LIMIT ((UINT64_C(1)<<BOOT_FM_SPACE_BITS)-16)
enum { BOOT_FM_MAPPED=0x80 };
typedef struct BootFmLeaf { uint64_t addend[512]; uint8_t base[512]; uint8_t state[512]; } BootFmLeaf;
typedef struct BootFmOverride { uint64_t base,size; uint32_t mask,pad; } BootFmOverride;
typedef struct BootFmAlloc { uint8_t*host; uint64_t size,hwm; HANDLE section; } BootFmAlloc;
typedef struct BootFmView { uint64_t guest,size; } BootFmView;
typedef struct BootFmFree { uint64_t start,end; } BootFmFree;
typedef struct BootFmAdopt { uint8_t*original,*copy; uint64_t size; uint32_t writable,pad; } BootFmAdopt;
typedef PVOID (WINAPI *BootFmVirtualAlloc2)(HANDLE,PVOID,SIZE_T,ULONG,ULONG,void*,ULONG);
typedef PVOID (WINAPI *BootFmMapViewOfFile3)(HANDLE,HANDLE,PVOID,ULONG64,SIZE_T,ULONG,ULONG,void*,ULONG);
typedef BOOL  (WINAPI *BootFmUnmapViewOfFile2)(HANDLE,PVOID,ULONG);
#define BOOT_FM_OVERRIDES (16+BOOT_ALIAS_CAPACITY)
typedef struct BootFm {
    /* hot fields first: read by every translated access */
    BootContext*direct_ctx,*table_ctx;
    uint8_t*base;
    BootFmLeaf**leaves;
    volatile uint64_t pc;            /* guest pc of the current direct access (fault attribution) */
    uint64_t page_rsp;               /* host rsp of the running translated page function */
    /* cold */
    BootContext*pending,*synced;     /* context awaiting sync; context the tables describe */
    uint32_t initialized,mode,direct_ok,veh_installed;
    DWORD guest_thread;
    Region*prev;uint32_t prev_count,prev_cap;
    uint32_t*hash;uint32_t hash_cap,pad0;
    BootFmOverride over[BOOT_FM_OVERRIDES];uint32_t over_count,pad1;
    BootFmAlloc*allocs;uint32_t alloc_count,alloc_cap;
    BootFmView*views;uint32_t view_count,view_cap;
    BootFmFree*frees;uint32_t free_count,free_cap;
    BootFmAdopt*adopts;uint32_t adopt_count,adopt_cap;
    BootFmVirtualAlloc2 VirtualAlloc2;BootFmMapViewOfFile3 MapViewOfFile3;BootFmUnmapViewOfFile2 UnmapViewOfFile2;
    PVOID veh;
    /* Installed by boot_fm_init in the translation unit that sees the full BootWindow (the split build's
     * runtime_support unit; page units only see the opaque window facade). */
    HANDLE (*resolve_external)(BootContext*c,const Region*r);
    /* statistics (fm-stats in boot_fm_report) */
    uint64_t syncs,full_syncs,regions_added,regions_removed,pages_set,override_changes,
             views_mapped,views_unmapped,protects,host_map_calls,adopted,adopted_bytes,copybacks,copyback_bytes,
             faults,retries,section_allocs,section_frees,direct_disables,map_svc,unmap_svc;
    uint64_t sync_ticks,report_tick;
    char disable_reason[160];
} BootFm;
__declspec(selectany) BootFm boot_fm={0};

static inline void boot_fm_log(const char*fmt,...){
    FILE*f=fopen("fastmem.log","a");if(!f)return;
    SYSTEMTIME t;GetLocalTime(&t);fprintf(f,"%04u-%02u-%02u %02u:%02u:%02u.%03u ",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds);
    va_list a;va_start(a,fmt);vfprintf(f,fmt,a);va_end(a);fputc('\n',f);fclose(f);
}
static inline void boot_fm_disable_direct(const char*why,uint64_t x){
    if(boot_fm.direct_ok){++boot_fm.direct_disables;
        snprintf(boot_fm.disable_reason,sizeof boot_fm.disable_reason,"%s %llx",why,(unsigned long long)x);
        boot_fm_log("direct mode disabled (table mode continues): %s",boot_fm.disable_reason);}
    boot_fm.direct_ok=0;boot_fm.direct_ctx=NULL;
}
/* ---------- arrays ---------- */
#define BOOT_FM_GROW(arr,count,cap) do{ if((count)==(cap)){uint32_t n_=(cap)?(cap)*2:64;void*p_=realloc((arr),n_*sizeof(*(arr)));if(!p_)return 0;(arr)=p_;(cap)=n_;} }while(0)

/* ---------- sections (owned allocations) ---------- */
static inline int boot_fm_alloc_find(const void*p){   /* index of the allocation containing p, or -1 */
    uint32_t lo=0,hi=boot_fm.alloc_count;
    while(lo<hi){uint32_t mid=(lo+hi)/2;if(boot_fm.allocs[mid].host<=(const uint8_t*)p)lo=mid+1;else hi=mid;}
    if(!lo)return -1;BootFmAlloc*a=&boot_fm.allocs[lo-1];
    return (const uint8_t*)p<a->host+a->size?(int)(lo-1):-1;
}
static inline int boot_fm_alloc_insert(BootFmAlloc a){
    BOOT_FM_GROW(boot_fm.allocs,boot_fm.alloc_count,boot_fm.alloc_cap);
    uint32_t i=0;while(i<boot_fm.alloc_count && boot_fm.allocs[i].host<a.host)++i;
    memmove(&boot_fm.allocs[i+1],&boot_fm.allocs[i],(boot_fm.alloc_count-i)*sizeof a);
    boot_fm.allocs[i]=a;++boot_fm.alloc_count;return 1;
}
/* Section-backed guest memory (zero filled). commit_all: SEC_COMMIT (D3D12 can open the view as a
 * heap); otherwise SEC_RESERVE, committed with boot_fm_commit. Returns the host view or NULL. */
static inline uint8_t*boot_fm_section_alloc(uint64_t size,int commit_all){
    size=(size+4095)&~UINT64_C(4095);if(!size)size=4096;
    HANDLE s=CreateFileMappingW(INVALID_HANDLE_VALUE,NULL,PAGE_READWRITE|(commit_all?SEC_COMMIT:SEC_RESERVE),(DWORD)(size>>32),(DWORD)size,NULL);
    if(!s)return NULL;
    uint8_t*v=MapViewOfFile(s,FILE_MAP_ALL_ACCESS,0,0,(SIZE_T)size);
    if(!v){CloseHandle(s);return NULL;}
    if(!boot_fm_alloc_insert((BootFmAlloc){v,size,commit_all?size:0,s})){UnmapViewOfFile(v);CloseHandle(s);return NULL;}
    ++boot_fm.section_allocs;return v;
}
static inline int boot_fm_owns(const void*p){return boot_fm.alloc_count && boot_fm_alloc_find(p)>=0;}
static inline int boot_fm_section_free(void*p){
    int i=boot_fm_alloc_find(p);if(i<0 || boot_fm.allocs[i].host!=p)return 0;
    BootFmAlloc a=boot_fm.allocs[i];
    memmove(&boot_fm.allocs[i],&boot_fm.allocs[i+1],(boot_fm.alloc_count-i-1)*sizeof a);--boot_fm.alloc_count;
    UnmapViewOfFile(a.host);CloseHandle(a.section);   /* guest views keep the section alive until unmapped */
    ++boot_fm.section_frees;return 1;
}
/* Commit [host, host+size) of a SEC_RESERVE allocation; pages committed before (high-water mark) are
 * zeroed instead, so regrown memory reads as zero without touching fresh pages. */
static inline int boot_fm_commit_zero(uint8_t*host,uint64_t size){
    int i=boot_fm_alloc_find(host);if(i<0)return 0;BootFmAlloc*a=&boot_fm.allocs[i];
    uint64_t off=(uint64_t)(host-a->host),end=off+size;
    if(end>a->size)return 0;
    if(off<a->hwm)memset(host,0,(size_t)((end<a->hwm?end:a->hwm)-off));
    if(end>a->hwm){if(!VirtualAlloc(a->host+a->hwm,(SIZE_T)(end-a->hwm),MEM_COMMIT,PAGE_READWRITE))return 0;a->hwm=end;}
    return 1;
}
/* Runtime allocation sites: section memory once fastmem direct mode is initialized, else calloc.
 * Table mode with TOTK_CORES>1 uses sections too: an unmap can then split a region in place (kept
 * pages keep their host memory while other cores access them; see boot_physical_memory). */
static inline uint8_t*boot_guest_alloc(uint64_t size){
    if(boot_fm.initialized && (boot_fm.mode==1 || (boot_fm.mode==2 && boot_mt.requested>1))){uint8_t*p=boot_fm_section_alloc(size,1);if(p)return p;}
    return calloc(1,(size_t)size);
}
/* Release guest memory of a removed region: owned sections, else the previous rules (free() for
 * calloc'd memory, keep whole VirtualAlloc blocks, which may back a D3D12 heap). */
static inline void boot_guest_free(void*p){
    if(!p)return;
    if(boot_fm_section_free(p))return;
    if(boot_fm_owns(p))return;   /* interior pointer of a section (arena or split): owner frees it */
    MEMORY_BASIC_INFORMATION mbi;
    if(!VirtualQuery(p,&mbi,sizeof mbi) || mbi.AllocationBase!=p)free(p);
}

/* ---------- page table ---------- */
static inline BootFmLeaf*boot_fm_leaf(uint64_t g,int create){
    BootFmLeaf**slot=&boot_fm.leaves[(g>>21)&(BOOT_FM_LEAVES-1)];
    if(!*slot && create)*slot=calloc(1,sizeof(BootFmLeaf));
    return *slot;
}
static inline uint8_t boot_fm_state(uint64_t g){
    if(g>>BOOT_FM_SPACE_BITS)return 0;BootFmLeaf*l=boot_fm.leaves[g>>21];return l?l->state[(g>>12)&511]:0;
}
static inline uint64_t boot_fm_addend(uint64_t g){
    if(g>>BOOT_FM_SPACE_BITS)return 0;BootFmLeaf*l=boot_fm.leaves[g>>21];return l?l->addend[(g>>12)&511]:0;
}
/* Pages fully covered by [base,base+size) inside the space. */
static inline int boot_fm_pages(uint64_t base,uint64_t size,uint64_t*first,uint64_t*end){
    if(!size || base>UINT64_MAX-size)return 0;
    uint64_t f=(base+4095)&~UINT64_C(4095),e=(base+size)&~UINT64_C(4095);
    uint64_t lim=UINT64_C(1)<<BOOT_FM_SPACE_BITS;
    if(e>lim)e=lim;
    if(f>=e)return 0;*first=f;*end=e;return 1;
}
static inline uint8_t boot_fm_effective(uint64_t g,uint8_t base){
    if(!base)return 0;
    unsigned allowed=base&7u;
    for(uint32_t i=0;i<boot_fm.over_count;++i){BootFmOverride*o=&boot_fm.over[i];
        if(g<o->base+o->size && o->base<g+4096)allowed&=o->mask;}
    return (uint8_t)(BOOT_FM_MAPPED|allowed);
}

/* ---------- guest view (direct mode) ---------- */
static inline int boot_fm_free_insert(uint64_t s,uint64_t e){
    BOOT_FM_GROW(boot_fm.frees,boot_fm.free_count,boot_fm.free_cap);
    uint32_t i=0;while(i<boot_fm.free_count && boot_fm.frees[i].start<s)++i;
    memmove(&boot_fm.frees[i+1],&boot_fm.frees[i],(boot_fm.free_count-i)*sizeof(BootFmFree));
    boot_fm.frees[i]=(BootFmFree){s,e};++boot_fm.free_count;return 1;
}
static inline void boot_fm_free_remove(uint32_t i){memmove(&boot_fm.frees[i],&boot_fm.frees[i+1],(boot_fm.free_count-i-1)*sizeof(BootFmFree));--boot_fm.free_count;}
static inline int boot_fm_view_find(uint64_t g){   /* index of the view starting at g, or -1 */
    uint32_t lo=0,hi=boot_fm.view_count;
    while(lo<hi){uint32_t mid=(lo+hi)/2;if(boot_fm.views[mid].guest<g)lo=mid+1;else hi=mid;}
    return lo<boot_fm.view_count && boot_fm.views[lo].guest==g?(int)lo:-1;
}
/* play (48): why the last view map failed (logged by boot_fm_set_region) */
static char boot_fm_map_why[512];
static inline void boot_fm_map_explain(uint64_t g,uint64_t size,uint64_t offset,const char*step,DWORD err){
    int n=snprintf(boot_fm_map_why,sizeof boot_fm_map_why,"%s (error %lu): guest %llx size %llx offset %llx;",step,(unsigned long)err,
                   (unsigned long long)g,(unsigned long long)size,(unsigned long long)offset);
    for(uint32_t i=0;i<boot_fm.free_count && n>0 && n<(int)sizeof boot_fm_map_why;++i)if(boot_fm.frees[i].start<g+size && g<boot_fm.frees[i].end)
        n+=snprintf(boot_fm_map_why+n,sizeof boot_fm_map_why-n," free %llx-%llx",(unsigned long long)boot_fm.frees[i].start,(unsigned long long)boot_fm.frees[i].end);
    for(uint32_t k=0;k<boot_fm.view_count && n>0 && n<(int)sizeof boot_fm_map_why;++k)if(boot_fm.views[k].guest<g+size && g<boot_fm.views[k].guest+boot_fm.views[k].size)
        n+=snprintf(boot_fm_map_why+n,sizeof boot_fm_map_why-n," view %llx+%llx",(unsigned long long)boot_fm.views[k].guest,(unsigned long long)boot_fm.views[k].size);
    if(n>0 && n<(int)sizeof boot_fm_map_why)snprintf(boot_fm_map_why+n,sizeof boot_fm_map_why-n," (%u views, %u free ranges, thread %lu)",boot_fm.view_count,boot_fm.free_count,GetCurrentThreadId());
}
static inline int boot_fm_view_map(uint64_t g,uint64_t size,HANDLE section,uint64_t offset){
    uint32_t i=0;while(i<boot_fm.free_count && !(boot_fm.frees[i].start<=g && g+size<=boot_fm.frees[i].end))++i;
    if(i==boot_fm.free_count){boot_fm_map_explain(g,size,offset,"no free range holds it",0);return 0;}
    BootFmFree f=boot_fm.frees[i];
    if(f.start!=g || f.end!=g+size){
        if(!VirtualFree(boot_fm.base+g,(SIZE_T)size,MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER)){boot_fm_map_explain(g,size,offset,"placeholder split failed",GetLastError());return 0;}
        ++boot_fm.host_map_calls;
        boot_fm_free_remove(i);
        if(f.start<g)boot_fm_free_insert(f.start,g);
        if(g+size<f.end)boot_fm_free_insert(g+size,f.end);
    }else boot_fm_free_remove(i);
    void*v=boot_fm.MapViewOfFile3(section,GetCurrentProcess(),boot_fm.base+g,offset,(SIZE_T)size,MEM_REPLACE_PLACEHOLDER,PAGE_READWRITE,NULL,0);
    ++boot_fm.host_map_calls;
    if(v!=boot_fm.base+g){DWORD err=GetLastError();boot_fm_free_insert(g,g+size);boot_fm_map_explain(g,size,offset,"MapViewOfFile3 failed",err);return 0;}   /* placeholder stays split; harmless */
    BOOT_FM_GROW(boot_fm.views,boot_fm.view_count,boot_fm.view_cap);
    uint32_t k=0;while(k<boot_fm.view_count && boot_fm.views[k].guest<g)++k;
    memmove(&boot_fm.views[k+1],&boot_fm.views[k],(boot_fm.view_count-k)*sizeof(BootFmView));
    boot_fm.views[k]=(BootFmView){g,size};++boot_fm.view_count;++boot_fm.views_mapped;
    return 1;
}
static inline int boot_fm_view_unmap(uint32_t k){
    BootFmView v=boot_fm.views[k];
    if(!boot_fm.UnmapViewOfFile2(GetCurrentProcess(),boot_fm.base+v.guest,MEM_PRESERVE_PLACEHOLDER))return 0;
    ++boot_fm.host_map_calls;++boot_fm.views_unmapped;
    memmove(&boot_fm.views[k],&boot_fm.views[k+1],(boot_fm.view_count-k-1)*sizeof(BootFmView));--boot_fm.view_count;
    uint64_t s=v.guest,e=v.guest+v.size;
    for(uint32_t i=0;i<boot_fm.free_count;){
        if(boot_fm.frees[i].end==s){s=boot_fm.frees[i].start;boot_fm_free_remove(i);continue;}
        if(boot_fm.frees[i].start==e){e=boot_fm.frees[i].end;boot_fm_free_remove(i);continue;}
        ++i;
    }
    if((s!=v.guest || e!=v.guest+v.size) && VirtualFree(boot_fm.base+s,(SIZE_T)(e-s),MEM_RELEASE|MEM_COALESCE_PLACEHOLDERS))++boot_fm.host_map_calls;
    else if(s!=v.guest || e!=v.guest+v.size){   /* coalesce failed: keep the pieces separate */
        boot_fm_free_insert(v.guest,v.guest+v.size);return 1;}
    return boot_fm_free_insert(s,e);
}
static inline void boot_fm_unmap_all_views(void){
    while(boot_fm.view_count)if(!boot_fm_view_unmap(boot_fm.view_count-1)){boot_fm.view_count=0;break;}
}
/* Host protection of [first,end) (inside one view) from the effective page states, run by run. */
static inline void boot_fm_reprotect(uint64_t first,uint64_t end){
    uint64_t p=first;
    while(p<end){uint8_t s=boot_fm_state(p);uint64_t q=p+4096;
        while(q<end && boot_fm_state(q)==s)q+=4096;
        DWORD prot=(s&3)==3?PAGE_READWRITE:(s&1)?PAGE_READONLY:PAGE_NOACCESS,old;
        if(!(s&BOOT_FM_MAPPED))prot=PAGE_NOACCESS;
        if(VirtualProtect(boot_fm.base+p,(SIZE_T)(q-p),prot,&old))++boot_fm.protects;
        p=q;
    }
}
/* Reprotect the pages of [first,end) that lie in mapped views. */
static inline void boot_fm_reprotect_range(uint64_t first,uint64_t end){
    for(uint32_t k=0;k<boot_fm.view_count;++k){BootFmView*v=&boot_fm.views[k];
        uint64_t s=v->guest>first?v->guest:first,e=v->guest+v->size<end?v->guest+v->size:end;
        if(s<e)boot_fm_reprotect(s,e);
    }
}

/* ---------- sections behind a region's data ---------- */
/* External sections: HID shared memory and the time shared page. */
#if defined(TOTK_HOST_HID_MEMORY_H)   /* BootWindow is complete here */
static inline HANDLE boot_fm_external_section_impl(BootContext*c,const Region*r){
    BootWindow*w=(BootWindow*)(uintptr_t)c->window_native;
    if(w && w->hid_memory.data && r->data==w->hid_memory.data)return w->hid_memory.mapping;
#else
static inline HANDLE boot_fm_external_section_impl(BootContext*c,const Region*r){
#endif
    if(r->size==0x1000)for(unsigned i=0;i<16;++i){
        if(!c->event_handles[i] || c->event_sources[i]!=700 || !c->event_native_handles[i])continue;
        HANDLE h=(HANDLE)(uintptr_t)c->event_native_handles[i];
        const uint8_t*v=MapViewOfFile(h,FILE_MAP_READ,0,0,0x1000);if(!v)continue;
        int same=!memcmp(v,r->data,64);UnmapViewOfFile(v);
        if(same)return h;
    }
    return NULL;
}
static inline HANDLE boot_fm_external_section(BootContext*c,const Region*r){
    return boot_fm.resolve_external?boot_fm.resolve_external(c,r):NULL;
}
/* Copy a region's memory into a new section and point the region (and aliases into it) there. */
static inline int boot_fm_adopt(BootContext*c,unsigned index){
    Region*r=&c->regions[index];uint8_t*old=r->data;
    uint8_t*copy=boot_fm_section_alloc(r->size,1);if(!copy)return 0;
    memcpy(copy,old,(size_t)r->size);
    for(unsigned j=0;j<c->region_count;++j){Region*d=&c->regions[j];
        if(j!=index && d->reserved==2 && d->data>=old && d->data<old+r->size)d->data=copy+(d->data-old);}
    r->data=copy;++boot_fm.adopted;boot_fm.adopted_bytes+=r->size;
    if(r->reserved==0){   /* Python-owned: remember for write-back and restore */
        for(uint32_t k=0;k<boot_fm.adopt_count;++k)if(boot_fm.adopts[k].original==old){   /* re-adopted (context rebuilt) */
            boot_fm_section_free(boot_fm.adopts[k].copy);boot_fm.adopts[k]=boot_fm.adopts[--boot_fm.adopt_count];break;}
        BOOT_FM_GROW(boot_fm.adopts,boot_fm.adopt_count,boot_fm.adopt_cap);
        boot_fm.adopts[boot_fm.adopt_count++]=(BootFmAdopt){old,copy,r->size,(r->permissions&WRITE)!=0,0};
    }else if(r->reserved==3)VirtualFree(old,0,MEM_RELEASE);
    else {   /* reserved 1: calloc'd (free) or a whole VirtualAlloc block (release); GPU arenas stay */
        MEMORY_BASIC_INFORMATION mbi;
        if(VirtualQuery(old,&mbi,sizeof mbi) && mbi.Type==MEM_PRIVATE && mbi.AllocationBase==old)VirtualFree(old,0,MEM_RELEASE);
        else if(VirtualQuery(old,&mbi,sizeof mbi) && mbi.Type==MEM_PRIVATE && mbi.AllocationBase &&
                !memcmp(mbi.AllocationBase,"TOTK-GPU-ARENA1",15)){}
        else if(mbi.Type!=MEM_MAPPED)free(old);
    }
    return 1;
}
/* Write Python-owned adopted memory back to the original buffers (writable ones). */
static inline void boot_fm_copy_back(void){
    for(uint32_t i=0;i<boot_fm.adopt_count;++i){BootFmAdopt*a=&boot_fm.adopts[i];
        if(!a->writable)continue;memcpy(a->original,a->copy,(size_t)a->size);++boot_fm.copybacks;boot_fm.copyback_bytes+=a->size;}
}

/* ---------- sync ---------- */
static inline uint32_t boot_fm_hash_region(const Region*r){
    uint64_t h=r->base*UINT64_C(0x9e3779b97f4a7c15)^r->size*UINT64_C(0xc2b2ae3d27d4eb4f)^(uint64_t)(uintptr_t)r->data*UINT64_C(0x165667b19e3779f9)^r->permissions;
    return (uint32_t)(h^(h>>29));
}
static inline int boot_fm_same(const Region*a,const Region*b){
    return a->base==b->base && a->size==b->size && a->data==b->data && a->permissions==b->permissions;
}
static inline int boot_fm_rehash(void){
    uint32_t want=64;while(want<boot_fm.prev_count*2+16)want*=2;
    if(want!=boot_fm.hash_cap){uint32_t*h=realloc(boot_fm.hash,want*sizeof(uint32_t));if(!h)return 0;boot_fm.hash=h;boot_fm.hash_cap=want;}
    memset(boot_fm.hash,0xff,boot_fm.hash_cap*sizeof(uint32_t));
    for(uint32_t i=0;i<boot_fm.prev_count;++i){uint32_t k=boot_fm_hash_region(&boot_fm.prev[i])&(boot_fm.hash_cap-1);
        while(boot_fm.hash[k]!=UINT32_MAX)k=(k+1)&(boot_fm.hash_cap-1);boot_fm.hash[k]=i;}
    return 1;
}
static inline int64_t boot_fm_prev_find(const Region*r){
    if(!boot_fm.hash_cap)return -1;
    uint32_t k=boot_fm_hash_region(r)&(boot_fm.hash_cap-1);
    while(boot_fm.hash[k]!=UINT32_MAX){if(boot_fm_same(&boot_fm.prev[boot_fm.hash[k]],r))return boot_fm.hash[k];k=(k+1)&(boot_fm.hash_cap-1);}
    return -1;
}
/* Page-table updates race with lock-free readers on other cores (TOTK_CORES>1: boot_fm_table_ptr reads
 * state, then addend). A cleared page keeps its addend and a set page gets its addend before its state,
 * so a reader that saw a mapped state never pairs it with a zero addend. A sync that removes a region
 * and re-adds a piece of it (unmap split in place) then only ever shows that page's one valid addend
 * or state 0 (slow path, which waits for the sync under the kernel lock). */
static inline void boot_fm_clear_region(const Region*r){
    uint64_t f,e;if(!boot_fm_pages(r->base,r->size,&f,&e))return;
    for(uint64_t g=f;g<e;g+=4096){BootFmLeaf*l=boot_fm_leaf(g,0);if(!l)continue;unsigned i=(g>>12)&511;
        __atomic_store_n(&l->state[i],0,__ATOMIC_RELEASE);l->base[i]=0;}
    if(boot_fm.direct_ok){int k=boot_fm_view_find(r->base);if(k>=0)boot_fm_view_unmap((uint32_t)k);}
}
static inline int boot_fm_set_region(BootContext*c,const Region*r){
    uint64_t f,e;if(!boot_fm_pages(r->base,r->size,&f,&e))return 1;
    uint8_t base=(uint8_t)(BOOT_FM_MAPPED|(r->permissions&7u));
    uint64_t addend=(uint64_t)(uintptr_t)r->data-r->base;
    for(uint64_t g=f;g<e;g+=4096){BootFmLeaf*l=boot_fm_leaf(g,1);if(!l)return 0;unsigned i=(g>>12)&511;
        l->base[i]=base;l->addend[i]=addend;__atomic_store_n(&l->state[i],boot_fm_effective(g,base),__ATOMIC_RELEASE);++boot_fm.pages_set;}
    if(boot_fm.direct_ok){
        if((r->base|r->size)&4095){boot_fm_disable_direct("unaligned region",r->base);return 1;}
        int a=boot_fm_alloc_find(r->data);HANDLE section=NULL;uint64_t offset=0;
        if(a>=0){section=boot_fm.allocs[a].section;offset=(uint64_t)(r->data-boot_fm.allocs[a].host);}
        else {section=boot_fm_external_section(c,r);offset=0;}
        if(!section || (offset&4095)){boot_fm_disable_direct("region without section",r->base);return 1;}
        if(!boot_fm_view_map(r->base,r->size,section,offset)){
            boot_fm_log("view map failed: %s; region data %p permissions %x reserved %u",boot_fm_map_why,(void*)r->data,(unsigned)r->permissions,(unsigned)r->reserved);
            boot_fm_disable_direct("view map failed",r->base);return 1;}
        boot_fm_reprotect(f,e);
    }
    return 1;
}
/* Recompute effective states (and protections) of pages in the given override ranges. */
static inline void boot_fm_refresh_overrides(const BootFmOverride*ranges,uint32_t count){
    for(uint32_t k=0;k<count;++k){uint64_t f,e;if(!boot_fm_pages(ranges[k].base,ranges[k].size,&f,&e))continue;
        for(uint64_t g=f;g<e;g+=4096){BootFmLeaf*l=boot_fm_leaf(g,0);if(!l)continue;unsigned i=(g>>12)&511;l->state[i]=boot_fm_effective(g,l->base[i]);}
        if(boot_fm.direct_ok)boot_fm_reprotect_range(f,e);
    }
}
static inline int boot_fm_collect_overrides(BootContext*c,BootFmOverride*out){
    uint32_t n=0;
    for(unsigned i=0;i<c->alias_count && i<BOOT_ALIAS_CAPACITY;++i)out[n++]=(BootFmOverride){c->aliases[i].source,c->aliases[i].size,0,0};
    for(unsigned i=0;i<16;++i){BootTransfer*t=&c->transfers[i];if(t->handle)out[n++]=(BootFmOverride){t->base,t->size,t->permissions&7u,0};}
    return (int)n;
}
static LONG CALLBACK boot_fm_veh(EXCEPTION_POINTERS*e);
#if defined(TOTK_DRAW_PATH) && defined(TOTK_HOST_GPU_DRAW_EXEC_H)
static inline uint8_t*boot_fm_arena_alloc(uint64_t size){return boot_fm_section_alloc(size,1);}
#endif
static inline int boot_fm_init(BootContext*c){
    if(boot_fm.initialized)return boot_fm.mode!=0;
    boot_fm.initialized=1;boot_fm.mode=1;
    char env[16];DWORD n=GetEnvironmentVariableA("TOTK_FASTMEM",env,sizeof env);
    if(n && n<sizeof env)boot_fm.mode=(uint32_t)(env[0]=='0'?0:env[0]=='2'?2:1);
    if(c->address_space_end>(UINT64_C(1)<<BOOT_FM_SPACE_BITS))boot_fm.mode=0;
    if(!boot_fm.mode){boot_fm_log("fastmem off (TOTK_FASTMEM=0 or address space > 39 bits)");return 0;}
    boot_fm.leaves=calloc(BOOT_FM_LEAVES,sizeof(BootFmLeaf*));
    if(!boot_fm.leaves){boot_fm.mode=0;return 0;}
    if(boot_fm.mode==1){
        HMODULE kb=GetModuleHandleW(L"kernelbase.dll");
        if(kb){boot_fm.VirtualAlloc2=(BootFmVirtualAlloc2)(void(*)(void))GetProcAddress(kb,"VirtualAlloc2");
            boot_fm.MapViewOfFile3=(BootFmMapViewOfFile3)(void(*)(void))GetProcAddress(kb,"MapViewOfFile3");
            boot_fm.UnmapViewOfFile2=(BootFmUnmapViewOfFile2)(void(*)(void))GetProcAddress(kb,"UnmapViewOfFile2");}
        if(boot_fm.VirtualAlloc2 && boot_fm.MapViewOfFile3 && boot_fm.UnmapViewOfFile2)
            boot_fm.base=boot_fm.VirtualAlloc2(GetCurrentProcess(),TOTK_FM_FIXED?(PVOID)(uintptr_t)BOOT_FM_FIXED_BASE:NULL,(SIZE_T)(UINT64_C(1)<<BOOT_FM_SPACE_BITS),MEM_RESERVE|MEM_RESERVE_PLACEHOLDER,PAGE_NOACCESS,NULL,0);
        if(boot_fm.base && boot_fm_free_insert(0,UINT64_C(1)<<BOOT_FM_SPACE_BITS)){
            boot_fm.veh=AddVectoredExceptionHandler(1,boot_fm_veh);
            boot_fm.direct_ok=boot_fm.veh!=NULL;
        }
#if defined(TOTK_DRAW_PATH) && defined(TOTK_HOST_GPU_DRAW_EXEC_H)
        if(boot_fm.direct_ok)boot_gpu_arena_alloc_hook=boot_fm_arena_alloc;
#endif
#if defined(TOTK_HOST_HID_MEMORY_H)
        boot_fm.resolve_external=boot_fm_external_section_impl;
#else
        boot_fm.resolve_external=boot_fm_external_section_impl;   /* time page only (no window here) */
#endif
        if(!boot_fm.direct_ok){boot_fm.mode=2;boot_fm_log("guest view unavailable (err %lu): table mode",GetLastError());}
    }
    boot_fm_log("fastmem mode %u (%s), guest view %p",boot_fm.mode,boot_fm.mode==1?"direct":"table",(void*)boot_fm.base);
    return 1;
}
/* Bring the tables (and guest view) in line with c; attach c to the fast paths. */
/* One process-wide scratch list (sync runs under the map lock); a static local here would be
 * instantiated in every translation unit (32 KiB x ~13k page units in the image). */
__declspec(selectany) uint32_t boot_fm_sync_added[BOOT_REGION_CAPACITY]={0};
static volatile LONG boot_fm_sync_depth; static volatile DWORD boot_fm_sync_thread;   /* play (48) */
static inline int boot_fm_sync_body(BootContext*c);
static inline __attribute__((noinline,cold)) int boot_fm_sync(BootContext*c){
    DWORD me=GetCurrentThreadId(),other=boot_fm_sync_thread;
    if(InterlockedIncrement(&boot_fm_sync_depth)>1 && other!=me){
        static volatile LONG logged;if(InterlockedIncrement(&logged)<=16)boot_fm_log("concurrent fastmem sync: thread %lu while thread %lu syncs (kernel core %d)",me,other,(int)boot_mt.kernel_core);}
    boot_fm_sync_thread=me;
    int r=boot_fm_sync_body(c);
    InterlockedDecrement(&boot_fm_sync_depth);
    return r;
}
static inline int boot_fm_sync_body(BootContext*c){
    LARGE_INTEGER t0,t1;QueryPerformanceCounter(&t0);
    if(!boot_fm_init(c)){boot_fm.pending=NULL;return 0;}
    int full=boot_fm.synced!=c;
    if(full){   /* another context: drop everything */
        for(uint32_t i=0;i<boot_fm.prev_count;++i)boot_fm_clear_region(&boot_fm.prev[i]);
        if(boot_fm.mode==1)boot_fm_unmap_all_views();
        boot_fm.prev_count=0;boot_fm.over_count=0;boot_fm_rehash();++boot_fm.full_syncs;
        if(boot_fm.mode==1 && !boot_fm.direct_ok && boot_fm.base && boot_fm.veh)boot_fm.direct_ok=1;   /* fresh chance per context */
    }
    /* direct mode: every region must live in a section (adopt the rest) */
    if(boot_fm.mode==1 && boot_fm.direct_ok){
        for(unsigned i=0;i<c->region_count;++i){Region*r=&c->regions[i];
            if(r->reserved==2 || !r->data || boot_fm_owns(r->data))continue;
            if(c->memory_types[i]==6)continue;
            if(!boot_fm_adopt(c,i)){boot_fm_disable_direct("adopt failed",r->base);break;}
        }
    }
    BootFmOverride now[BOOT_FM_OVERRIDES];uint32_t now_count=(uint32_t)boot_fm_collect_overrides(c,now);
    int overrides_changed=now_count!=boot_fm.over_count || memcmp(now,boot_fm.over,now_count*sizeof(BootFmOverride));
    BootFmOverride old[BOOT_FM_OVERRIDES];uint32_t old_count=boot_fm.over_count;
    memcpy(old,boot_fm.over,old_count*sizeof(BootFmOverride));
    /* adopt the new overrides first so pages set below get their final state */
    memcpy(boot_fm.over,now,now_count*sizeof(BootFmOverride));boot_fm.over_count=now_count;
    /* region diff */
    uint8_t*seen=calloc(boot_fm.prev_count+1,1);if(!seen)return 0;
    uint32_t*added=boot_fm_sync_added;uint32_t added_count=0;
    for(unsigned i=0;i<c->region_count && i<BOOT_REGION_CAPACITY;++i){
        int64_t k=boot_fm_prev_find(&c->regions[i]);
        if(k>=0 && !seen[k])seen[k]=1;else added[added_count++]=i;
    }
    for(uint32_t i=0;i<boot_fm.prev_count;++i)if(!seen[i]){boot_fm_clear_region(&boot_fm.prev[i]);++boot_fm.regions_removed;}
    free(seen);
    for(uint32_t k=0;k<added_count;++k){if(!boot_fm_set_region(c,&c->regions[added[k]]))return 0;++boot_fm.regions_added;}
    if(overrides_changed){++boot_fm.override_changes;boot_fm_refresh_overrides(old,old_count);boot_fm_refresh_overrides(now,now_count);}
    /* remember the table */
    if(c->region_count>boot_fm.prev_cap){Region*p=realloc(boot_fm.prev,c->region_count*sizeof(Region));if(!p)return 0;boot_fm.prev=p;boot_fm.prev_cap=c->region_count;}
    memcpy(boot_fm.prev,c->regions,c->region_count*sizeof(Region));boot_fm.prev_count=c->region_count;boot_fm_rehash();
    boot_fm.synced=c;boot_fm.pending=NULL;
    boot_fm.table_ctx=c;
    boot_fm.direct_ctx=boot_fm.mode==1 && boot_fm.direct_ok?c:NULL;
    if(boot_fm.direct_ctx)boot_fm.guest_thread=GetCurrentThreadId();
    ++boot_fm.syncs;QueryPerformanceCounter(&t1);boot_fm.sync_ticks+=(uint64_t)(t1.QuadPart-t0.QuadPart);
    if(full)boot_fm_log("full sync: %u regions, %u views, %u allocs, adopted %llu (%llu MiB), mode %s",c->region_count,boot_fm.view_count,boot_fm.alloc_count,
        (unsigned long long)boot_fm.adopted,(unsigned long long)(boot_fm.adopted_bytes>>20),boot_fm.direct_ctx?"direct":"table");
    return 1;
}
/* boot_tlb_flush hook: tables are about to change (or just changed). */
static inline void boot_fm_changed(void){
    BootContext*c=boot_fm.table_ctx?boot_fm.table_ctx:boot_fm.pending;
    boot_fm.table_ctx=NULL;boot_fm.direct_ctx=NULL;
    if(c)boot_fm.pending=c;
}
/* boot_valid hook: (re)attach at the next slow-path access, picking up host-side edits. */
static inline void boot_fm_enter(BootContext*c){
    /* Incremental: host-side edits of the tables (Python, checkpoint restore, a rebuilt context at the
     * same address) show up in the diff by value; memory contents need no sync. */
    boot_fm.table_ctx=NULL;boot_fm.direct_ctx=NULL;boot_fm.pending=c;
    boot_fm_sync(c);
}
/* Context memory is being released: write Python buffers back, restore their pointers. */
static inline void boot_fm_release(BootContext*c){
    if(boot_fm.synced!=c && boot_fm.pending!=c && boot_fm.table_ctx!=c)return;
    boot_fm_copy_back();
    for(uint32_t i=0;i<boot_fm.adopt_count;++i){BootFmAdopt*a=&boot_fm.adopts[i];
        for(unsigned j=0;j<c->region_count;++j)if(c->regions[j].data==a->copy && c->regions[j].reserved==0)c->regions[j].data=a->original;
        boot_fm_section_free(a->copy);}
    boot_fm.adopt_count=0;
    for(uint32_t i=0;i<boot_fm.prev_count;++i)boot_fm_clear_region(&boot_fm.prev[i]);
    if(boot_fm.mode==1)boot_fm_unmap_all_views();
    boot_fm.prev_count=0;boot_fm.over_count=0;boot_fm_rehash();
    boot_fm.synced=boot_fm.pending=boot_fm.table_ctx=boot_fm.direct_ctx=NULL;
}
/* A stop (first boot_fault): keep Python-owned buffers current. CH_ADDRESS from the exact lookup can be
 * transient (the split/gather recovery paths clear it and continue), so it is skipped here; the fault
 * handler copies back for its (terminal) faults itself. */
static inline void boot_fm_on_stop(BootContext*c,uint32_t status){
    if(boot_fm.adopt_count && boot_fm.synced==c && status!=CH_ADDRESS)boot_fm_copy_back();
}
/* fm-stats line (also written at most every 30 s from the slow path) */
static inline void boot_fm_report(void){
    LARGE_INTEGER f;QueryPerformanceFrequency(&f);
    boot_fm_log("stats mode=%u direct=%d syncs=%llu full=%llu sync_ms=%.1f regions+%llu/-%llu pages_set=%llu override_changes=%llu views=%u mapped=%llu unmapped=%llu protects=%llu host_calls=%llu "
        "sections=%u (+%llu/-%llu) adopted=%llu/%lluMiB copybacks=%llu/%lluMiB faults=%llu retries=%llu direct_disables=%llu map_svc=%llu unmap_svc=%llu%s%s",
        boot_fm.mode,boot_fm.direct_ctx!=NULL,(unsigned long long)boot_fm.syncs,(unsigned long long)boot_fm.full_syncs,boot_fm.sync_ticks*1000.0/(double)f.QuadPart,
        (unsigned long long)boot_fm.regions_added,(unsigned long long)boot_fm.regions_removed,(unsigned long long)boot_fm.pages_set,(unsigned long long)boot_fm.override_changes,
        boot_fm.view_count,(unsigned long long)boot_fm.views_mapped,(unsigned long long)boot_fm.views_unmapped,(unsigned long long)boot_fm.protects,(unsigned long long)boot_fm.host_map_calls,
        boot_fm.alloc_count,(unsigned long long)boot_fm.section_allocs,(unsigned long long)boot_fm.section_frees,(unsigned long long)boot_fm.adopted,(unsigned long long)(boot_fm.adopted_bytes>>20),
        (unsigned long long)boot_fm.copybacks,(unsigned long long)(boot_fm.copyback_bytes>>20),(unsigned long long)boot_fm.faults,(unsigned long long)boot_fm.retries,
        (unsigned long long)boot_fm.direct_disables,(unsigned long long)boot_fm.map_svc,(unsigned long long)boot_fm.unmap_svc,boot_fm.disable_reason[0]?" last_disable=":"",boot_fm.disable_reason);
}
/* Slow-path hook (boot_access_slow): sync a pending context; periodic stats. */
static inline __attribute__((noinline,cold)) void boot_fm_slow(BootContext*c){
    if(boot_fm.pending==c)boot_fm_sync(c);
    LARGE_INTEGER t,f;QueryPerformanceCounter(&t);QueryPerformanceFrequency(&f);
    if(boot_fm.initialized && boot_fm.mode && (uint64_t)t.QuadPart-boot_fm.report_tick>(uint64_t)f.QuadPart*30){boot_fm.report_tick=(uint64_t)t.QuadPart;boot_fm_report();}
}

/* ---------- fault conversion (direct mode) ---------- */
/* Effective address of the x86-64 memory operand at ip (legacy/REX/VEX; others return 0). */
static inline int boot_fm_x86_ea(const uint8_t*ip,const CONTEXT*ctx,uint64_t*ea){
    const DWORD64*gpr=&ctx->Rax;unsigned rex=0,addr32=0;const uint8_t*p=ip;
    for(int n=0;n<15;++n,++p){uint8_t b=*p;
        if(b==0x66||b==0xf2||b==0xf3||b==0xf0||b==0x2e||b==0x36||b==0x3e||b==0x26)continue;
        if(b==0x64||b==0x65)return 0;
        if(b==0x67){addr32=1;continue;}
        break;}
    if((*p&0xf0)==0x40)rex=*p++;
    if(*p==0xc5){rex=((p[1]&0x80)?0:4);p+=3;}
    else if(*p==0xc4){unsigned b1=p[1];rex=((b1&0x80)?0:4)|((b1&0x40)?0:2)|((b1&0x20)?0:1);if((b1&0x1f)<1||(b1&0x1f)>3)return 0;p+=4;}
    else if(*p==0x62)return 0;
    else if(*p==0x0f){++p;if(*p==0x38||*p==0x3a)++p;++p;}
    else {uint8_t op=*p++;if((op>=0xa0&&op<=0xa7)||(op>=0xaa&&op<=0xaf))return 0;}
    uint8_t modrm=*p++;unsigned mod=modrm>>6,rm=modrm&7;
    if(mod==3)return 0;
    uint64_t addr=0;
    if(rm==4){uint8_t sib=*p++;unsigned scale=sib>>6,index=((sib>>3)&7)|((rex&2)?8:0),base=(sib&7)|((rex&1)?8:0);
        if(index!=4)addr+=gpr[index]<<scale;
        if((sib&7)==5 && mod==0){addr+=(uint64_t)(int64_t)*(const int32_t*)p;p+=4;}else addr+=gpr[base];
    }else if(rm==5 && mod==0)return 0;
    else addr+=gpr[rm|((rex&1)?8:0)];
    if(mod==1)addr+=(uint64_t)(int64_t)*(const int8_t*)p;else if(mod==2)addr+=(uint64_t)(int64_t)*(const int32_t*)p;
    if(addr32)addr&=0xffffffffu;
    *ea=addr;return 1;
}
/* Width of the ARM64 load/store instruction (0 if unknown). */
static inline uint64_t boot_fm_arm64_width(uint32_t i){
    if((i&0x3b000000)==0x39000000 || (i&0x3b200000)==0x38000000 || (i&0x3b200c00)==0x38200800){
        unsigned size=i>>30,v=(i>>26)&1,opc=(i>>22)&3;if(v && size==0 && (opc&2))return 16;return (uint64_t)1<<size;}
    if((i&0x3b200c00)==0x38200000)return (uint64_t)1<<(i>>30);
    if((i&0x3a000000)==0x28000000){unsigned opc=i>>30,v=(i>>26)&1;if(v)return 2u*(4u<<opc);return opc==2?16:8;}
    if((i&0x3f000000)==0x08000000){unsigned size=i>>30,pair=(i>>21)&1;return ((uint64_t)1<<size)*(pair&&size>=2?2:1);}
    if((i&0xbfbf0000)==0x0c000000 || (i&0xbfa00000)==0x0c800000){static const uint8_t regs[16]={4,0,4,0,3,0,3,1,2,0,2,0,0,0,0,0};
        return (uint64_t)regs[(i>>12)&15]*((i>>30)&1?16:8);}
    if((i&0xbf9f0000)==0x0d000000 || (i&0xbf800000)==0x0d800000){unsigned opcode=(i>>13)&7,r=(i>>21)&1,size=(i>>10)&3;
        unsigned selem=(((opcode&1)<<1)|r)+1,scale=opcode>>1,esize=scale==3?1u<<size:scale==2?((size&1)?8:4):1u<<scale;return (uint64_t)selem*esize;}
    if((i&0xffffffe0)==0xd50b7420)return 64;
    return 0;
}
/* play (49): what the fixed view, the page table and the region table say about guest page g */
static inline __attribute__((noinline,cold)) uint8_t*boot_access_lookup(BootContext*c,uint64_t a,unsigned w,unsigned p,uint64_t pc);
static inline void boot_fm_describe(const BootContext*M,uint64_t g,char*out,size_t cap){
    uint8_t s=boot_fm_state(g);int n=snprintf(out,cap,"state %02x",s);
    for(unsigned i=0;M && i<M->region_count && n>0 && n<(int)cap;++i){const Region*r=&M->regions[i];
        if(g>=r->base && g-r->base<r->size)n+=snprintf(out+n,cap-n,"; region %u %llx+%llx perm %x reserved %u type %u data %p owned %d",i,(unsigned long long)r->base,
            (unsigned long long)r->size,(unsigned)r->permissions,(unsigned)r->reserved,(unsigned)M->memory_types[i],(void*)r->data,r->data?boot_fm_owns(r->data):0);}
    for(unsigned i=0;M && i<M->alias_count && i<BOOT_ALIAS_CAPACITY && n>0 && n<(int)cap;++i)if(g<M->aliases[i].source+M->aliases[i].size && M->aliases[i].source<=g)
        n+=snprintf(out+n,cap-n,"; alias source %llx+%llx",(unsigned long long)M->aliases[i].source,(unsigned long long)M->aliases[i].size);
    for(unsigned i=0;M && i<16 && n>0 && n<(int)cap;++i){const BootTransfer*t=&M->transfers[i];
        if(t->handle && g<t->base+t->size && t->base<=g)n+=snprintf(out+n,cap-n,"; transfer %llx+%llx perm %x",(unsigned long long)t->base,(unsigned long long)t->size,(unsigned)t->permissions);}
    for(uint32_t k=0;k<boot_fm.view_count && n>0 && n<(int)cap;++k)if(g>=boot_fm.views[k].guest && g-boot_fm.views[k].guest<boot_fm.views[k].size)
        n+=snprintf(out+n,cap-n,"; view %llx+%llx",(unsigned long long)boot_fm.views[k].guest,(unsigned long long)boot_fm.views[k].size);
    MEMORY_BASIC_INFORMATION mbi;
    if(n>0 && n<(int)cap && VirtualQuery(boot_fm.base+(g&~UINT64_C(4095)),&mbi,sizeof mbi))
        n+=snprintf(out+n,cap-n,"; host page state %lx protect %lx type %lx",(unsigned long)mbi.State,(unsigned long)mbi.Protect,(unsigned long)mbi.Type);
    if(n>0 && n<(int)cap)snprintf(out+n,cap-n,"; pending %d synced %d views %u",boot_fm.pending!=NULL,boot_fm.synced==M,boot_fm.view_count);
}
static LONG CALLBACK boot_fm_veh(EXCEPTION_POINTERS*e){
    BootContext*c=boot_fm.direct_ctx;
    if(e->ExceptionRecord->ExceptionCode!=EXCEPTION_ACCESS_VIOLATION || e->ExceptionRecord->NumberParameters<2 || !boot_fm.base)return EXCEPTION_CONTINUE_SEARCH;
    uint64_t host=(uint64_t)e->ExceptionRecord->ExceptionInformation[1],kind=(uint64_t)e->ExceptionRecord->ExceptionInformation[0];
    uint64_t base=(uint64_t)(uintptr_t)boot_fm.base;
    if(host<base || host-base>=(UINT64_C(1)<<BOOT_FM_SPACE_BITS) || kind==8)return EXCEPTION_CONTINUE_SEARCH;
    uint64_t page_rsp=boot_fm.page_rsp,hint=boot_fm.pc;
    int worker=boot_mt_worker_of_thread(GetCurrentThreadId());
    if(worker>=0){
        /* TOTK_CORES>1: a core context's access (kernel code on the master never faults in the view).
         * Let a table change in progress finish (or finish a pending sync) before judging the page. */
        if(boot_mt.kernel_core==(unsigned)worker)return EXCEPTION_CONTINUE_SEARCH;
        c=(BootContext*)(void*)boot_mt.core[worker];
        page_rsp=BOOT_CORE_HOT(c)->fm_page_rsp;hint=BOOT_CORE_HOT(c)->fm_pc;
        AcquireSRWLockExclusive(&boot_mt.klock);
        if(boot_fm.pending)boot_fm_sync(boot_fm.pending);
        ReleaseSRWLockExclusive(&boot_mt.klock);
    }else{
#if TOTK_FM_FIXED
        if(!c)c=boot_fm.pending?boot_fm.pending:boot_fm.synced;
        if(c && boot_fm.pending && GetCurrentThreadId()==boot_fm.guest_thread){   /* play (48): under the kernel lock while cores run */
            int lock=boot_mt.active;if(lock)AcquireSRWLockExclusive(&boot_mt.klock);
            if(boot_fm.pending)boot_fm_sync(boot_fm.pending);
            if(lock)ReleaseSRWLockExclusive(&boot_mt.klock);}
#endif
        if(!c || GetCurrentThreadId()!=boot_fm.guest_thread)return EXCEPTION_CONTINUE_SEARCH;
    }
    if(!page_rsp || e->ContextRecord->Rsp>page_rsp)return EXCEPTION_CONTINUE_SEARCH;
    uint64_t g=host-base;uint8_t s=boot_fm_state(g);unsigned need=kind==1?WRITE:READ;
    static _Thread_local uint64_t retry_rip;static _Thread_local unsigned retry_count;   /* per host thread */
    if((s&need)==need && (s&BOOT_FM_MAPPED)){   /* protection lagging the table: reprotect once and retry */
        if(retry_rip!=e->ContextRecord->Rip){retry_rip=e->ContextRecord->Rip;retry_count=0;}
        if(++retry_count<3){++boot_fm.retries;boot_fm_reprotect_range(g&~UINT64_C(4095),(g&~UINT64_C(4095))+4096);return EXCEPTION_CONTINUE_EXECUTION;}
    }
#if TOTK_FM_FIXED
    {   /* play (49): the exact lookup decides; a valid access gets the view rebuilt and is retried */
        static _Thread_local uint64_t repair_rip;static _Thread_local unsigned repairs;
        BootContext*M=boot_mt.active && boot_mt.master?boot_mt.master:c;
        int lock=boot_mt.active;if(lock)AcquireSRWLockExclusive(&boot_mt.klock);
        uint32_t st=M->status;uint64_t fpc=M->fault_pc,fa=M->fault_address,fw=M->fault_width;M->status=0;
        uint8_t*h=boot_access_lookup(M,g,1,need,0);
        M->status=st;M->fault_pc=fpc;M->fault_address=fa;M->fault_width=fw;
        char d[1024];boot_fm_describe(M,g,d,sizeof d);
        if(repair_rip!=e->ContextRecord->Rip){repair_rip=e->ContextRecord->Rip;repairs=0;}
        if(h && repairs<2){
            ++repairs;
            /* targeted: (re)map the covering region's view when none covers the page, then reprotect its pages.
             * No full rebuild: host code may be reading guest memory through the view on other cores. */
            const Region*r=NULL;for(unsigned i=0;i<M->region_count;++i)if(g>=M->regions[i].base && g-M->regions[i].base<M->regions[i].size){r=&M->regions[i];break;}
            int viewed=0;for(uint32_t k=0;k<boot_fm.view_count;++k)viewed|=g>=boot_fm.views[k].guest && g-boot_fm.views[k].guest<boot_fm.views[k].size;
            const char*how="reprotect";
            if(r && !viewed && boot_fm.direct_ok){how="view mapped";boot_fm_set_region(M,r);}
            if(r){uint64_t f,en;if(boot_fm_pages(r->base,r->size,&f,&en))boot_fm_reprotect(f,en);}
            static volatile LONG logged;if(InterlockedIncrement(&logged)<=64)boot_fm_log("fixed view: valid %s of %llx faulted (thread %lu, worker %d): %s; repair: %s",
                kind==1?"write":"read",(unsigned long long)g,GetCurrentThreadId(),worker,d,how);
            if(lock)ReleaseSRWLockExclusive(&boot_mt.klock);
            retry_rip=0;return EXCEPTION_CONTINUE_EXECUTION;
        }
        boot_fm_log("fixed view: guest fault: %s of %llx (thread %lu, worker %d, lookup %s): %s",kind==1?"write":"read",(unsigned long long)g,
            GetCurrentThreadId(),worker,h?"valid but repairs exhausted":"invalid",d);
        if(lock)ReleaseSRWLockExclusive(&boot_mt.klock);
    }
#endif
    retry_rip=0;++boot_fm.faults;
    uint64_t ea,a=g,pc=hint,w=0;
    if(boot_fm_x86_ea((const uint8_t*)(uintptr_t)e->ContextRecord->Rip,e->ContextRecord,&ea) && ea-base<(UINT64_C(1)<<BOOT_FM_SPACE_BITS))a=ea-base;
    {BootFmLeaf*l=pc>>BOOT_FM_SPACE_BITS?NULL:boot_fm.leaves[pc>>21];unsigned i=(pc>>12)&511;
     if(l && (l->state[i]&READ) && (pc&4095)<=4092)w=boot_fm_arm64_width(*(const uint32_t*)(uintptr_t)(pc+l->addend[i]));}
    if(!w)w=(uint64_t)(host-base)-a+1;
    if(!c->status && boot_fm.adopt_count)boot_fm_copy_back();
    boot_fault(c,(s&BOOT_FM_MAPPED)?CH_PERMISSION:CH_ADDRESS,pc,a,w);
    /* Leave the translated page function as if it returned c->status: unwind frames until the
     * stack pointer is above the page function's frame (recorded at page entry). */
    CONTEXT ctx=*e->ContextRecord;
    for(int n=0;n<64 && ctx.Rsp<=page_rsp;++n){
        DWORD64 image=0;PRUNTIME_FUNCTION fe=RtlLookupFunctionEntry(ctx.Rip,&image,NULL);
        if(!fe){ctx.Rip=*(DWORD64*)(uintptr_t)ctx.Rsp;ctx.Rsp+=8;continue;}
        PVOID handler_data=NULL;DWORD64 frame=0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER,image,ctx.Rip,fe,&ctx,&handler_data,&frame,NULL);
    }
    if(ctx.Rsp<=page_rsp)return EXCEPTION_CONTINUE_SEARCH;
    ctx.Rax=c->status;
    *e->ContextRecord=ctx;
    return EXCEPTION_CONTINUE_EXECUTION;
}
/* Fast-path context tests. The attached master context and, with TOTK_CORES>1, every core context
 * while a master is attached (the mapping state is process-wide). The pc hint is per host thread: the
 * master's in boot_fm.pc, a core's in its BootCoreHot. */
/* Direct path: the attached master context only. Core contexts (TOTK_CORES>1) take the complete page table
 * through the out-of-line slow paths, so each inlined access carries a single context compare. */
#if TOTK_FM_FIXED
#define BOOT_FM_DIRECT(c,pc) ((void)sizeof(c),(void)sizeof(pc),1)
#define BOOT_FM_VIEW ((uint8_t*)(uintptr_t)BOOT_FM_FIXED_BASE)
#elif TOTK_PLAY
/* Play build: no per-access guest pc store (the pc expression is not even evaluated). A fault in the
 * view is still caught and stops with CH_ADDRESS/CH_PERMISSION at the right address, but its reported
 * pc is the last one a slow path recorded; the probe build (TOTK_PLAY=0) gives the exact pc. */
#define BOOT_FM_DIRECT(c,pc) ((void)sizeof(pc),(c)==boot_fm.direct_ctx)
#else
#define BOOT_FM_DIRECT(c,pc) ((c)==boot_fm.direct_ctx?(boot_fm.pc=(pc),1):0)
#endif
#ifndef BOOT_FM_VIEW
#define BOOT_FM_VIEW boot_fm.base
#endif
#define BOOT_FM_TABLE(c) ((c)==boot_fm.table_ctx || (boot_fm.table_ctx && boot_mt_core_ctx(c)))
/* Page-function entry mark (boot_block_check, always inlined): the frame the handler unwinds. */
static inline __attribute__((always_inline)) void boot_fm_mark(void){
    uint64_t rsp;__asm__ volatile("movq %%rsp,%0":"=r"(rsp));boot_fm.page_rsp=rsp;
}
static inline __attribute__((always_inline)) void boot_fm_mark_ctx(const BootContext*c){
    uint64_t rsp;__asm__ volatile("movq %%rsp,%0":"=r"(rsp));
    if(__builtin_expect(boot_mt_core_ctx(c),0))BOOT_CORE_HOT(c)->fm_page_rsp=rsp;else boot_fm.page_rsp=rsp;
}
/* Page-table translation for an access inside one page with permissions p; NULL: take the slow path. */
static inline __attribute__((always_inline)) uint8_t*boot_fm_table_ptr(const BootContext*c,uint64_t a,unsigned w,unsigned p){
    const BootFmLeaf*l=boot_fm.leaves[(a>>21)&(BOOT_FM_LEAVES-1)];unsigned i=(unsigned)(a>>12)&511;
    if(__builtin_expect(!(a>>BOOT_FM_SPACE_BITS) && l && !c->status && (a&4095)+(uint64_t)w<=4096 && (l->state[i]&p)==p,1))
        return (uint8_t*)(uintptr_t)(a+l->addend[i]);
    return NULL;
}
/* Unaligned volatile access types: one host instruction per guest access. */
typedef uint16_t boot_fm_u16 __attribute__((aligned(1),may_alias));
typedef uint32_t boot_fm_u32 __attribute__((aligned(1),may_alias));
typedef uint64_t boot_fm_u64 __attribute__((aligned(1),may_alias));
typedef long long boot_fm_v128 __attribute__((vector_size(16),aligned(1),may_alias));
static inline __attribute__((always_inline)) uint64_t boot_fm_read(const uint8_t*m,unsigned w){
    switch(w){case 1:return *(volatile const uint8_t*)m;case 2:return *(volatile const boot_fm_u16*)m;
              case 4:return *(volatile const boot_fm_u32*)m;default:return *(volatile const boot_fm_u64*)m;}
}
static inline __attribute__((always_inline)) void boot_fm_write(uint8_t*m,unsigned w,uint64_t v){
    switch(w){case 1:*(volatile uint8_t*)m=(uint8_t)v;break;case 2:*(volatile boot_fm_u16*)m=(uint16_t)v;break;
              case 4:*(volatile boot_fm_u32*)m=(uint32_t)v;break;default:*(volatile boot_fm_u64*)m=v;break;}
}
/* 16-byte compare-exchange (host pointer must be 16-byte aligned). */
static inline int boot_fm_cas16(void*p,uint64_t elo,uint64_t ehi,uint64_t nlo,uint64_t nhi){
    unsigned char ok;
    __asm__ __volatile__("lock cmpxchg16b %1\n\tsete %0":"=q"(ok),"+m"(*(volatile uint64_t(*)[2])p),"+a"(elo),"+d"(ehi):"b"(nlo),"c"(nhi):"memory","cc");
    return ok;
}
static inline int boot_fm_cas(void*p,unsigned w,uint64_t expected,uint64_t desired){
    switch(w){
        case 1:{uint8_t e=(uint8_t)expected;return __atomic_compare_exchange_n((uint8_t*)p,&e,(uint8_t)desired,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);}
        case 2:{uint16_t e=(uint16_t)expected;return __atomic_compare_exchange_n((uint16_t*)p,&e,(uint16_t)desired,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);}
        case 4:{uint32_t e=(uint32_t)expected;return __atomic_compare_exchange_n((uint32_t*)p,&e,(uint32_t)desired,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);}
        default:{uint64_t e=expected;return __atomic_compare_exchange_n((uint64_t*)p,&e,desired,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);}
    }
}
#endif /* TOTK_FASTMEM */
#endif
