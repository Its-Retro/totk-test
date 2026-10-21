#ifndef TOTK_HOST_GPU_VA_H
#define TOTK_HOST_GPU_VA_H
#include <stdint.h>
#include <string.h>
/* 16384: dynamic MapBufferEx mappings each take a reservation; at the final Ganondorf cutscene 4189 mappings
 * were live and a 4096-entry table rejected the next map (play-83 svc 0x21 stop). */
#define BOOT_GPU_VA_CAPACITY 16384
/* Source GPU addresses are logical IDs, never host pointers. No physical
 * allocation or GPU command is implied by a reservation. */
typedef struct BootGpuAddressSpace {
    uint64_t start,end,split;
    uint32_t page_size,initialized;
    struct {uint64_t offset,size;uint32_t page_size,sparse;} reservations[BOOT_GPU_VA_CAPACITY];
    uint64_t next[2];   /* next-fit hints for dynamic reservations (small, big pages): first-fit from the range start is O(n^2) */
} BootGpuAddressSpace;
static inline int boot_gpu_va_reserve(BootGpuAddressSpace*a,uint32_t pages,uint32_t page,uint32_t flags,uint64_t align_or_offset,uint64_t*out){
    if(!a->initialized || !pages || (page!=4096 && page!=a->page_size) || (flags&~3u) || ((flags&2) && page!=a->page_size))return 0;
    int fixed=(flags&1)!=0;
    uint64_t low=page==4096?a->start:a->split,high=page==4096?a->split:a->end;
    uint64_t size=(uint64_t)pages*page,alignment=fixed?page:align_or_offset;
    if(!alignment)alignment=page;
    if(alignment<page || (alignment&(alignment-1)) || size>high-low)return 0;
    unsigned slot=0;while(slot<BOOT_GPU_VA_CAPACITY && a->reservations[slot].size)++slot;if(slot==BOOT_GPU_VA_CAPACITY)return 0;
    uint64_t candidate=fixed?align_or_offset:low;int wrapped=1;
    if(!fixed){uint64_t h=a->next[page!=4096];if(h>low && h<high){candidate=h;wrapped=0;}}   /* next fit, then wrap to low once */
    for(unsigned pass=0;pass<=2*BOOT_GPU_VA_CAPACITY+1;++pass){
        if(!fixed){if(candidate>UINT64_MAX-(alignment-1))return 0;candidate=(candidate+alignment-1)&~(alignment-1);}
        if(candidate<low || candidate>high || size>high-candidate || (candidate&(page-1))){
            if(!fixed && !wrapped){wrapped=1;candidate=low;continue;}
            return 0;}
        unsigned i=0;
        for(;i<BOOT_GPU_VA_CAPACITY;++i)if(a->reservations[i].size && candidate<a->reservations[i].offset+a->reservations[i].size && a->reservations[i].offset<candidate+size)break;
        if(i==BOOT_GPU_VA_CAPACITY){a->reservations[slot].offset=candidate;a->reservations[slot].size=size;a->reservations[slot].page_size=page;a->reservations[slot].sparse=(flags&2)!=0;if(!fixed)a->next[page!=4096]=candidate+size;*out=candidate;return 1;}
        if(fixed)return 0;
        candidate=a->reservations[i].offset+a->reservations[i].size;
    }
    return 0;
}
static inline int boot_gpu_va_release(BootGpuAddressSpace*a,uint64_t offset,uint32_t pages,uint32_t page){
    if(!a->initialized || !pages || (page!=4096 && page!=a->page_size))return 0;
    uint64_t size=(uint64_t)pages*page;
    for(unsigned i=0;i<BOOT_GPU_VA_CAPACITY;++i)if(a->reservations[i].size==size && a->reservations[i].offset==offset && a->reservations[i].page_size==page){
        memset(&a->reservations[i],0,sizeof(a->reservations[i]));return 1;
    }
    return 0;
}
#endif
