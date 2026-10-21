#ifndef TOTK_MEMORY_ATTRIBUTES_H
#define TOTK_MEMORY_ATTRIBUTES_H
#include <stdint.h>
#include <string.h>
/* Sorted disjoint uncached guest ranges. Host CPU memory remains coherent;
 * a GPU upload must read current backing bytes and synchronize its copy/queue.
 * This metadata does not claim direct GPU access to host malloc/VirtualAlloc. */
typedef struct BootUncachedRange { uint64_t begin,end; } BootUncachedRange;
#define BOOT_MEMORY_ATTRIBUTE_CAPACITY 4096
typedef struct BootMemoryAttributes { BootUncachedRange ranges[BOOT_MEMORY_ATTRIBUTE_CAPACITY]; uint32_t count; } BootMemoryAttributes;
static inline int boot_attributes_update(BootMemoryAttributes*m,uint64_t begin,uint64_t end,int set){
    BootUncachedRange temporary[BOOT_MEMORY_ATTRIBUTE_CAPACITY*2+1];unsigned count=0;
    for(unsigned i=0;i<m->count;++i){BootUncachedRange r=m->ranges[i];
        if(r.end<=begin || r.begin>=end)temporary[count++]=r;
        else {if(r.begin<begin)temporary[count++]=(BootUncachedRange){r.begin,begin};
              if(r.end>end)temporary[count++]=(BootUncachedRange){end,r.end};}
    }
    if(set)temporary[count++]=(BootUncachedRange){begin,end};
    for(unsigned i=1;i<count;++i){BootUncachedRange r=temporary[i];unsigned j=i;
        while(j && temporary[j-1].begin>r.begin){temporary[j]=temporary[j-1];--j;}temporary[j]=r;}
    unsigned merged=0;
    for(unsigned i=0;i<count;++i){BootUncachedRange r=temporary[i];
        if(merged && temporary[merged-1].end>=r.begin){if(r.end>temporary[merged-1].end)temporary[merged-1].end=r.end;}
        else temporary[merged++]=r;
    }
    if(merged>BOOT_MEMORY_ATTRIBUTE_CAPACITY)return 0;
    memset(m,0,sizeof(*m));memcpy(m->ranges,temporary,merged*sizeof(*temporary));m->count=merged;return 1;
}
static inline int boot_attributes_overlap(const BootMemoryAttributes*m,uint64_t begin,uint64_t end){
    for(unsigned i=0;i<m->count;++i)if(m->ranges[i].begin<end && begin<m->ranges[i].end)return 1;
    return 0;
}
#endif
