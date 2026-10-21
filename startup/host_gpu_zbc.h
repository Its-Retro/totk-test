#ifndef TOTK_HOST_GPU_ZBC_H
#define TOTK_HOST_GPU_ZBC_H
#include <stdint.h>
#include <string.h>
/* Source-GPU clear metadata. Hardware index zero is reserved; ioctl query
 * indices are zero-based. No GPU execution is implied by registration. */
#define BOOT_ZBC_CAPACITY 15
typedef struct BootZbcEntry { uint32_t ds[4],l2[4],depth,format,refs; } BootZbcEntry;
typedef struct BootZbc { BootZbcEntry color[15],depth[15];uint32_t colors,depths,initialized; } BootZbc;
static inline void boot_zbc_init(BootZbc*t){
    if(t->initialized)return;
    memset(t,0,sizeof(*t));t->colors=4;t->depths=2;t->initialized=1;
    t->color[0].format=1;t->color[1].format=2;t->color[2].format=t->color[3].format=4;
    for(unsigned i=0;i<4;++i){t->color[i].refs=1;t->color[1].ds[i]=UINT32_MAX;
        t->color[1].l2[i]=t->color[3].ds[i]=t->color[3].l2[i]=UINT32_C(0x3f800000);}
    t->depth[0].format=t->depth[1].format=1;t->depth[0].refs=t->depth[1].refs=1;
    t->depth[1].depth=UINT32_C(0x3f800000);
}
static inline int boot_zbc_set(BootZbc*t,const uint32_t input[11]){
    uint32_t type=input[10];if(type!=1 && type!=2)return 0;
    BootZbc next=*t;boot_zbc_init(&next);
    BootZbcEntry*entries=type==1?next.color:next.depth;
    uint32_t*count=type==1?&next.colors:&next.depths;
    for(unsigned i=0;i<*count;++i){BootZbcEntry*e=entries+i;
        if(e->format!=input[9] || (type==1?memcmp(e->ds,input,16)!=0:e->depth!=input[8]))continue;
        if(e->refs==UINT32_MAX || (type==1 && memcmp(e->l2,input+4,16)))return 0;
        ++e->refs;*t=next;return 1;
    }
    if(*count>=BOOT_ZBC_CAPACITY)return 0;
    BootZbcEntry*e=entries+(*count)++;memset(e,0,sizeof(*e));e->format=input[9];e->refs=1;
    if(type==1){memcpy(e->ds,input,16);memcpy(e->l2,input+4,16);}else e->depth=input[8];
    *t=next;return 1;
}
static inline int boot_zbc_query(const BootZbc*t,uint32_t inout[13]){
    uint32_t type=inout[11],index=inout[12];
    if(!type){inout[12]=BOOT_ZBC_CAPACITY;return 1;}
    if((type!=1 && type!=2) || index>=BOOT_ZBC_CAPACITY)return 0;
    BootZbc snapshot=*t;boot_zbc_init(&snapshot);
    const BootZbcEntry*e=(type==1?snapshot.color:snapshot.depth)+index;
    if(type==1){memcpy(inout,e->ds,16);memcpy(inout+4,e->l2,16);}else inout[8]=e->depth;
    inout[9]=e->refs;inout[10]=e->format;return 1;
}
#endif
