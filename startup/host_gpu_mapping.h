#ifndef TOTK_HOST_GPU_MAPPING_H
#define TOTK_HOST_GPU_MAPPING_H
#include "host_gpu_va.h"
#include "host_nvmap.h"
/* 16384: the final Ganondorf cutscene (movie + gameplay) needs more than 4096 live mappings; a full
 * table rejected MapBufferEx (nvdrv-reject.log, play-81: "gpu mappings 4096/4096") and the game fell
 * back to raw IPC (svc 0x21 stop). */
#define BOOT_GPU_MAPPING_CAPACITY 16384
/* Device-address translation to live guest CPU backing. These are not host
 * GPU virtual addresses. Command execution must resolve and synchronize data. */
typedef struct BootGpuMapping {
    uint64_t gpu,cpu,size;
    uint32_t as_fd,owner_fd,handle,page,automatic,flags,references,kind,padding;
} BootGpuMapping;
typedef struct BootGpuMappings {BootGpuMapping entries[BOOT_GPU_MAPPING_CAPACITY];} BootGpuMappings;
/* size owns the whole GPU span; padding has no accessible nvmap backing. */
static inline uint64_t boot_gpu_mapping_backing_size(const BootGpuMapping*r){return r->padding<=r->size?r->size-r->padding:0;}
/* Resolve one contiguous segment. Mapped pages override sparse holes. A
 * mapping with unsupported layout/ownership must never become a zero read. */
static inline int boot_gpu_memory_segment(const BootGpuMappings*m,const BootGpuAddressSpace*a,
        uint32_t fd,uint32_t owner,uint64_t address,uint64_t size,uint64_t*cpu,uint64_t*length){
    if(!m || !a || !a->initialized || !fd || !owner || !size || address>UINT64_MAX-size)return 0;
    uint64_t end=address+size;
    /* One scan collects everything the resolution needs: the first automatic (overlay) mapping
     * containing the address (preferred), else the first other one; the lowest overlay start above
     * the address (an overlay clips the segment it starts in); the lowest start of any mapping above
     * the address (end of a sparse hole). */
    const BootGpuMapping*overlay_hit=NULL,*plain_hit=NULL;
    uint64_t overlay_above=UINT64_MAX,any_above=UINT64_MAX;
    for(unsigned i=0;i<BOOT_GPU_MAPPING_CAPACITY;++i){const BootGpuMapping*r=&m->entries[i];
        if(!r->size || r->as_fd!=fd)continue;
        if(address>=r->gpu && address-r->gpu<r->size){
            if(r->automatic==2){if(!overlay_hit)overlay_hit=r;}
            else if(!plain_hit)plain_hit=r;
        }else if(r->gpu>address){
            if(r->gpu<any_above)any_above=r->gpu;
            if(r->automatic==2 && r->gpu<overlay_above)overlay_above=r->gpu;
        }
    }
    const BootGpuMapping*selected=overlay_hit?overlay_hit:plain_hit;
    if(selected){const BootGpuMapping*r=selected;
        if(r->owner_fd!=owner || r->kind || r->cpu>UINT64_MAX-(address-r->gpu))return 0;
        uint64_t backed=boot_gpu_mapping_backing_size(r);if(address-r->gpu>=backed)return 0;
        *length=backed-(address-r->gpu);if(*length>size)*length=size;
        if(r->automatic!=2 && overlay_above<address+*length)*length=overlay_above-address;
        *cpu=r->cpu+address-r->gpu;return 1;
    }
    if(any_above<end)end=any_above;
    for(unsigned i=0;i<BOOT_GPU_VA_CAPACITY;++i){
        if(a->reservations[i].sparse && address>=a->reservations[i].offset && address-a->reservations[i].offset<a->reservations[i].size){
            uint64_t left=a->reservations[i].size-(address-a->reservations[i].offset);
            *length=end-address;if(*length>left)*length=left;*cpu=0;return 1;
        }
    }
    return 0;
}
/* Read-only index for many boot_gpu_memory_segment lookups against one unchanging (m,a,fd), e.g.
 * every GPFIFO entry of one submission while the window lock is held. The index must be rebuilt
 * after any change to m or a (the caller builds it, uses it and frees it with no change between).
 * The result of every lookup is identical to boot_gpu_memory_segment: overlay (automatic==2) and
 * other mappings of fd are each sorted by GPU address; when each class is pairwise disjoint (the
 * usual case) the first containing mapping in table order is the only containing one, found by
 * binary search, and the lowest start above an address is the next sorted start. Same for sparse
 * reservations (if those overlap, a lookup that reaches them scans the reservation table in order).
 * If a mapping class overlaps (or allocation fails) every lookup uses the full scan. */
typedef struct BootGpuSegmentEntry { uint64_t gpu,size;unsigned index; } BootGpuSegmentEntry;
typedef struct BootGpuSegmentIndex {
    const BootGpuMappings*m;const BootGpuAddressSpace*a;uint32_t fd;int usable,sparse_usable;
    unsigned overlay_count,plain_count,start_count,sparse_count;
    BootGpuSegmentEntry*overlay,*plain,*sparse;uint64_t*starts;
} BootGpuSegmentIndex;
static inline int boot_gpu_segment_entry_order(const void*x,const void*y){
    const BootGpuSegmentEntry*a=x,*b=y;return a->gpu<b->gpu?-1:a->gpu>b->gpu?1:(a->index<b->index?-1:a->index>b->index);}
static inline int boot_gpu_segment_start_order(const void*x,const void*y){
    uint64_t a=*(const uint64_t*)x,b=*(const uint64_t*)y;return a<b?-1:a>b;}
static inline int boot_gpu_segment_disjoint(const BootGpuSegmentEntry*e,unsigned n){
    for(unsigned i=0;i<n;++i)if(e[i].gpu>UINT64_MAX-e[i].size)return 0;
    for(unsigned i=1;i<n;++i)if(e[i].gpu<e[i-1].gpu+e[i-1].size)return 0;
    return 1;
}
static inline void boot_gpu_segment_index_free(BootGpuSegmentIndex*x){free(x->overlay);memset(x,0,sizeof(*x));}
static inline void boot_gpu_segment_index_build(BootGpuSegmentIndex*x,const BootGpuMappings*m,const BootGpuAddressSpace*a,uint32_t fd){
    memset(x,0,sizeof(*x));x->m=m;x->a=a;x->fd=fd;
    if(!m || !a)return;
    /* one allocation: overlay, plain and sparse entry arrays, then the start array */
    size_t entries=(size_t)BOOT_GPU_MAPPING_CAPACITY*2+BOOT_GPU_VA_CAPACITY;
    BootGpuSegmentEntry*storage=malloc(entries*sizeof(BootGpuSegmentEntry)+(size_t)BOOT_GPU_MAPPING_CAPACITY*sizeof(uint64_t));
    if(!storage)return;
    x->overlay=storage;x->plain=storage+BOOT_GPU_MAPPING_CAPACITY;x->sparse=storage+2*BOOT_GPU_MAPPING_CAPACITY;
    x->starts=(uint64_t*)(storage+entries);
    for(unsigned i=0;i<BOOT_GPU_MAPPING_CAPACITY;++i){const BootGpuMapping*r=&m->entries[i];
        if(!r->size || r->as_fd!=fd)continue;
        BootGpuSegmentEntry e={r->gpu,r->size,i};
        if(r->automatic==2)x->overlay[x->overlay_count++]=e;else x->plain[x->plain_count++]=e;
        x->starts[x->start_count++]=r->gpu;
    }
    for(unsigned i=0;i<BOOT_GPU_VA_CAPACITY;++i)if(a->reservations[i].sparse && a->reservations[i].size)
        x->sparse[x->sparse_count++]=(BootGpuSegmentEntry){a->reservations[i].offset,a->reservations[i].size,i};
    qsort(x->overlay,x->overlay_count,sizeof(BootGpuSegmentEntry),boot_gpu_segment_entry_order);
    qsort(x->plain,x->plain_count,sizeof(BootGpuSegmentEntry),boot_gpu_segment_entry_order);
    qsort(x->sparse,x->sparse_count,sizeof(BootGpuSegmentEntry),boot_gpu_segment_entry_order);
    qsort(x->starts,x->start_count,sizeof(uint64_t),boot_gpu_segment_start_order);
    x->usable=boot_gpu_segment_disjoint(x->overlay,x->overlay_count) && boot_gpu_segment_disjoint(x->plain,x->plain_count);
    x->sparse_usable=boot_gpu_segment_disjoint(x->sparse,x->sparse_count);
}
/* Index of the first sorted entry whose gpu is > address (count if none). */
static inline unsigned boot_gpu_segment_upper(const BootGpuSegmentEntry*e,unsigned n,uint64_t address){
    unsigned lo=0,hi=n;while(lo<hi){unsigned mid=lo+(hi-lo)/2;if(e[mid].gpu<=address)lo=mid+1;else hi=mid;}return lo;}
static inline const BootGpuSegmentEntry*boot_gpu_segment_containing(const BootGpuSegmentEntry*e,unsigned n,uint64_t address){
    unsigned k=boot_gpu_segment_upper(e,n,address);
    return k && address-e[k-1].gpu<e[k-1].size?&e[k-1]:NULL;
}
static inline int boot_gpu_memory_segment(const BootGpuMappings*m,const BootGpuAddressSpace*a,
        uint32_t fd,uint32_t owner,uint64_t address,uint64_t size,uint64_t*cpu,uint64_t*length);
static inline int boot_gpu_memory_segment_indexed(const BootGpuSegmentIndex*x,uint32_t owner,uint64_t address,uint64_t size,uint64_t*cpu,uint64_t*length){
    const BootGpuMappings*m=x->m;const BootGpuAddressSpace*a=x->a;uint32_t fd=x->fd;
    if(!x->usable)return boot_gpu_memory_segment(m,a,fd,owner,address,size,cpu,length);
    if(!m || !a || !a->initialized || !fd || !owner || !size || address>UINT64_MAX-size)return 0;
    uint64_t end=address+size;
    const BootGpuSegmentEntry*overlay=boot_gpu_segment_containing(x->overlay,x->overlay_count,address);
    const BootGpuSegmentEntry*plain=overlay?NULL:boot_gpu_segment_containing(x->plain,x->plain_count,address);
    unsigned k=boot_gpu_segment_upper(x->overlay,x->overlay_count,address);
    uint64_t overlay_above=k<x->overlay_count?x->overlay[k].gpu:UINT64_MAX;
    const BootGpuSegmentEntry*hit=overlay?overlay:plain;
    if(hit){const BootGpuMapping*r=&m->entries[hit->index];
        if(r->owner_fd!=owner || r->kind || r->cpu>UINT64_MAX-(address-r->gpu))return 0;
        uint64_t backed=boot_gpu_mapping_backing_size(r);if(address-r->gpu>=backed)return 0;
        *length=backed-(address-r->gpu);if(*length>size)*length=size;
        if(r->automatic!=2 && overlay_above<address+*length)*length=overlay_above-address;
        *cpu=r->cpu+address-r->gpu;return 1;
    }
    unsigned lo=0,hi=x->start_count;while(lo<hi){unsigned mid=lo+(hi-lo)/2;if(x->starts[mid]<=address)lo=mid+1;else hi=mid;}
    if(lo<x->start_count && x->starts[lo]<end)end=x->starts[lo];
    if(x->sparse_usable){
        const BootGpuSegmentEntry*s=boot_gpu_segment_containing(x->sparse,x->sparse_count,address);
        if(s){uint64_t left=s->size-(address-s->gpu);*length=end-address;if(*length>left)*length=left;*cpu=0;return 1;}
        return 0;
    }
    for(unsigned i=0;i<BOOT_GPU_VA_CAPACITY;++i){
        if(a->reservations[i].sparse && address>=a->reservations[i].offset && address-a->reservations[i].offset<a->reservations[i].size){
            uint64_t left=a->reservations[i].size-(address-a->reservations[i].offset);
            *length=end-address;if(*length>left)*length=left;*cpu=0;return 1;
        }
    }
    return 0;
}
static inline int boot_gpu_mapping_cpu_overlap(const BootGpuMappings*m,uint64_t begin,uint64_t end){
    for(unsigned i=0;i<BOOT_GPU_MAPPING_CAPACITY;++i){const BootGpuMapping*r=&m->entries[i];
        uint64_t backed=boot_gpu_mapping_backing_size(r);
        if(backed && begin<r->cpu+backed && r->cpu<end)return 1;}
    return 0;
}
static inline int boot_gpu_mapping_object_used(const BootGpuMappings*m,uint32_t owner,uint32_t handle){
    for(unsigned i=0;i<BOOT_GPU_MAPPING_CAPACITY;++i)if(m->entries[i].size && m->entries[i].owner_fd==owner && (!handle || m->entries[i].handle==handle))return 1;
    return 0;
}
static inline int boot_gpu_mapping_va_overlap(const BootGpuMappings*m,uint32_t as_fd,uint64_t begin,uint64_t end){
    for(unsigned i=0;i<BOOT_GPU_MAPPING_CAPACITY;++i){const BootGpuMapping*r=&m->entries[i];
        if(r->size && r->as_fd==as_fd && begin<r->gpu+r->size && r->gpu<end)return 1;}
    return 0;
}
static inline int boot_gpu_remap_kind_valid(uint32_t kind);
static inline int boot_gpu_mapping_add(BootGpuMappings*m,BootGpuAddressSpace*a,uint32_t as_fd,const BootNvMapObject*object,
        uint32_t flags,uint32_t kind,uint64_t offset,uint64_t size,uint64_t requested,uint64_t*out){
    if(!a->initialized || !as_fd || !object || !object->allocated || (flags&~5u) ||
       offset>object->size || (offset&4095))return 0;
    if(!size)size=object->size-offset;
    if(kind==UINT32_MAX)kind=object->kind;
    if(!boot_gpu_remap_kind_valid(kind))return 0;
    uint64_t available=object->size-offset,backed=size<available?size:available;
    if(!size || !backed || (size&4095) || object->address>UINT64_MAX-offset-backed)return 0;
    uint64_t cpu=object->address+offset;uint32_t page=4096;int fixed=(flags&1)!=0;
    if(cpu&4095)return 0;
    if(fixed){
        if(requested<a->start || requested>a->end || size>a->end-requested || (requested&4095))return 0;
        unsigned i=0;for(;i<BOOT_GPU_VA_CAPACITY;++i)if(a->reservations[i].size && requested>=a->reservations[i].offset &&
            size<=a->reservations[i].size && requested-a->reservations[i].offset<=a->reservations[i].size-size)break;
        if(i==BOOT_GPU_VA_CAPACITY)return 0;
        page=a->reservations[i].page_size;
        if((requested|size)&(page-1))return 0;
    }else if(requested)return 0; /* Dynamic alignment variants remain explicit stops. */
    if(size>available && (!fixed || size!=((available+page-1)&~((uint64_t)page-1))))return 0;
    uint32_t padding=(uint32_t)(size-backed);
    for(unsigned i=0;i<BOOT_GPU_MAPPING_CAPACITY;++i){BootGpuMapping*r=&m->entries[i];
        if(r->size==size && r->as_fd==as_fd && r->owner_fd==object->owner_fd && r->handle==object->handle &&
           r->cpu==cpu && r->flags==flags && r->kind==kind && r->padding==padding && (!fixed || r->gpu==requested)){
            if(r->references==UINT32_MAX)return 0;++r->references;*out=r->gpu;return 1;}
    }
    unsigned slot=0;while(slot<BOOT_GPU_MAPPING_CAPACITY && m->entries[slot].size)++slot;if(slot==BOOT_GPU_MAPPING_CAPACITY)return 0;
    uint64_t gpu=requested;
    if(fixed){if(boot_gpu_mapping_va_overlap(m,as_fd,gpu,gpu+size))return 0;}
    else if(!boot_gpu_va_reserve(a,(uint32_t)(size/4096),4096,0,object->alignment,&gpu))return 0;
    m->entries[slot]=(BootGpuMapping){gpu,cpu,size,as_fd,object->owner_fd,object->handle,page,!fixed,flags,1,kind,padding};*out=gpu;return 1;
}
static inline int boot_gpu_mapping_remove(BootGpuMappings*m,BootGpuAddressSpace*a,uint32_t fd,uint64_t gpu){
    for(unsigned i=0;i<BOOT_GPU_MAPPING_CAPACITY;++i){BootGpuMapping*r=&m->entries[i];if(!r->size || r->as_fd!=fd || r->gpu!=gpu)continue;
        if(r->automatic==2)continue;
        if(r->references>1){--r->references;return 1;}
        if(r->automatic && !boot_gpu_va_release(a,r->gpu,(uint32_t)(r->size/r->page),r->page))return 0;
        uint64_t begin=r->gpu,end=begin+r->size;
        for(unsigned j=0;j<BOOT_GPU_MAPPING_CAPACITY;++j){BootGpuMapping*overlay=&m->entries[j];
            if(overlay->automatic==2 && overlay->as_fd==fd && overlay->size &&
               overlay->gpu>=begin && overlay->gpu<end)memset(overlay,0,sizeof(*overlay));
        }
        memset(r,0,sizeof(*r));return 1;
    }return 0;
}
/* Replace a subrange of a sparse reservation. Stage splits and replacement
 * before committing, so capacity or backing failures preserve the old map. */
static inline int boot_gpu_remap_kind_valid(uint32_t kind){
    /* Validity facts from Linux v6.12 nouveau gm200_mmu_kind, also selected
     * by gm20b.c. Preserve the original kind, including compression metadata;
     * this is registration only, not a conversion to its uncompressed kind.
     * https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/nouveau/nvkm/subdev/mmu/gm200.c */
    static const uint8_t ranges[][2]={
        {0x00,0x05},{0x07,0x0a},{0x11,0x11},{0x17,0x1a},
        {0x26,0x2b},{0x3a,0x41},{0x46,0x46},{0x51,0x54},
        {0x7b,0x7b},{0x86,0x89},{0x8b,0x8e},{0xa3,0xaa},
        {0xbf,0xc3},{0xcc,0xd1},{0xd8,0xd8},{0xdb,0xdb},
        {0xdd,0xdd},{0xdf,0xe0},{0xe2,0xe6},{0xe9,0xe9},
        {0xeb,0xeb},{0xed,0xee},{0xf0,0xfb},{0xfd,0xfe}
    };
    for(unsigned i=0;i<sizeof(ranges)/sizeof(ranges[0]);++i)
        if(kind>=ranges[i][0] && kind<=ranges[i][1])return 1;
    return 0;
}
static inline int boot_gpu_mapping_remap(BootGpuMappings*m,const BootGpuAddressSpace*a,uint32_t fd,
        const BootNvMapObject*object,uint32_t flags,uint32_t kind,uint64_t offset,uint64_t gpu,uint64_t size){
    if(!a->initialized || !fd || !size || (flags&~4u) || !boot_gpu_remap_kind_valid(kind) ||
       ((offset|gpu|size)&65535) || gpu>UINT64_MAX-size)return 0;
    unsigned reservation=0;for(;reservation<BOOT_GPU_VA_CAPACITY;++reservation)if(a->reservations[reservation].sparse &&
        a->reservations[reservation].page_size==65536 && gpu>=a->reservations[reservation].offset &&
        size<=a->reservations[reservation].size && gpu-a->reservations[reservation].offset<=a->reservations[reservation].size-size)break;
    if(reservation==BOOT_GPU_VA_CAPACITY)return 0;
    uint64_t cpu=0;
    if(object){if(!object->allocated || !object->owner_fd || !object->handle || offset>object->size || size>object->size-offset || object->address>UINT64_MAX-offset-size)return 0;
        /* CPU virtual alignment does not describe physical GPU page backing.
         * The observed nvmap allocation is 4 KiB-aligned with 64 KiB GPU pages. */
        cpu=object->address+offset;if(cpu&4095)return 0;
    }else if(offset)return 0;
    /* static, not on the stack (1 MiB at 16384 entries): every caller holds w->lock exclusively */
    static BootGpuMappings next;memset(&next,0,sizeof next);unsigned count=0;uint64_t end=gpu+size;
    for(unsigned i=0;i<BOOT_GPU_MAPPING_CAPACITY;++i){BootGpuMapping r=m->entries[i];if(!r.size)continue;
        if(r.as_fd!=fd || r.gpu>=end || gpu>=r.gpu+r.size){if(count==BOOT_GPU_MAPPING_CAPACITY)return 0;next.entries[count++]=r;continue;}
        if(r.automatic || r.references!=1 || r.page!=65536)return 0;
        uint64_t old_end=r.gpu+r.size;
        if(r.gpu<gpu){if(count==BOOT_GPU_MAPPING_CAPACITY)return 0;BootGpuMapping left=r;left.size=gpu-r.gpu;uint64_t backed=boot_gpu_mapping_backing_size(&r);left.padding=left.size>backed?(uint32_t)(left.size-backed):0;next.entries[count++]=left;}
        if(old_end>end){if(count==BOOT_GPU_MAPPING_CAPACITY)return 0;BootGpuMapping right=r;right.cpu+=end-r.gpu;right.gpu=end;right.size=old_end-end;if(right.padding>right.size)right.padding=(uint32_t)right.size;next.entries[count++]=right;}
    }
    if(object){if(count==BOOT_GPU_MAPPING_CAPACITY)return 0;next.entries[count++]=(BootGpuMapping){gpu,cpu,size,fd,object->owner_fd,object->handle,65536,0,flags|1u,1,kind,0};}
    *m=next;return 1;
}
/* MapBufferEx flag 0x100 rebinds a subrange of an existing GPU mapping.
 * The request carries no new NVMap handle; retain the already registered CPU
 * backing and ownership while updating the subrange's source surface kind. */
static inline int boot_gpu_mapping_rebind_subrange(BootGpuMappings*m,const BootGpuAddressSpace*a,
        uint32_t fd,uint64_t gpu,uint64_t size,uint32_t kind){
    if(!m || !a || !a->initialized || !fd || !size || gpu>UINT64_MAX-size ||
       ((gpu|size)&65535) || !boot_gpu_remap_kind_valid(kind))return 0;
    for(unsigned i=0;i<BOOT_GPU_MAPPING_CAPACITY;++i){const BootGpuMapping*r=&m->entries[i];
        if(!r->size || r->as_fd!=fd || gpu<r->gpu || gpu-r->gpu>=r->size ||
           size>r->size-(gpu-r->gpu) || gpu-r->gpu>=boot_gpu_mapping_backing_size(r) ||
           size>boot_gpu_mapping_backing_size(r)-(gpu-r->gpu))continue;
        if(r->automatic==2)continue;
        if(r->references!=1 || !r->owner_fd || !r->handle || r->cpu>UINT64_MAX-(gpu-r->gpu))return 0;
        if(r->automatic==1){
            if(r->page!=4096)return 0;
            unsigned free_slot=BOOT_GPU_MAPPING_CAPACITY;
            for(unsigned j=0;j<BOOT_GPU_MAPPING_CAPACITY;++j){BootGpuMapping*other=&m->entries[j];
                if(!other->size){if(free_slot==BOOT_GPU_MAPPING_CAPACITY)free_slot=j;continue;}
                if(other->automatic!=2 || other->as_fd!=fd || gpu>=other->gpu+other->size ||
                   other->gpu>=gpu+size)continue;
                if(other->gpu!=gpu || other->size!=size || other->owner_fd!=r->owner_fd ||
                   other->handle!=r->handle)return 0;
                other->kind=kind;return 1;
            }
            if(free_slot==BOOT_GPU_MAPPING_CAPACITY)return 0;
            BootGpuMapping overlay=*r;overlay.gpu=gpu;overlay.cpu=r->cpu+(gpu-r->gpu);
            overlay.size=size;overlay.kind=kind;overlay.automatic=2;overlay.padding=0;
            overlay.flags|=UINT32_C(0x100);
            m->entries[free_slot]=overlay;return 1;
        }
        if(r->page!=65536)return 0;
        BootNvMapObject backing={0};backing.allocated=1;backing.owner_fd=r->owner_fd;
        backing.handle=r->handle;backing.address=r->cpu+(gpu-r->gpu);backing.size=size;
        return boot_gpu_mapping_remap(m,a,fd,&backing,0,kind,0,gpu,size);
    }
    return 0;
}
static inline void boot_gpu_mapping_close(BootGpuMappings*m,uint32_t fd){
    for(unsigned i=0;i<BOOT_GPU_MAPPING_CAPACITY;++i)if(m->entries[i].as_fd==fd)memset(&m->entries[i],0,sizeof(m->entries[i]));
}
#endif
