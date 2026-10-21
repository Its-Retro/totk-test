#ifndef TOTK_HOST_GPU_SYNC_H
#define TOTK_HOST_GPU_SYNC_H
#include <stdint.h>
/* Decode the observed hardware synchronization operations. This layer never
 * acknowledges a fence, supplies a timestamp, or changes memory. Execution
 * must preserve packet order and use completed host GPU work. Field meanings
 * come from NVIDIA's NVB06F/NVB197 class definitions. */
typedef struct BootGpuSemaphore {
    uint64_t address;
    uint32_t payload,words,timestamp,after_writes;
} BootGpuSemaphore;
static inline int boot_gpu_semaphore_fifo(uint32_t upper,uint32_t lower,
        uint32_t payload,uint32_t control,BootGpuSemaphore*out){
    if(!out || upper>255 || (lower&3) ||
       (control & ~UINT32_C(0x0110001f)) || (control&31)!=2)return 0;
    BootGpuSemaphore result={((uint64_t)upper<<32)|lower,payload,
        (control&UINT32_C(0x01000000))?1:4,
        (control&UINT32_C(0x01000000))?0:1,
        (control&UINT32_C(0x00100000))?0:1};
    if(result.address>UINT64_C(0xffffffffff)-(result.words*4-1))return 0;
    *out=result;return 1;
}
static inline int boot_gpu_semaphore_report(uint32_t upper,uint32_t lower,
        uint32_t payload,uint32_t control,BootGpuSemaphore*out){
    /* Only ALL-pipeline releases and the observed REPORT_ONLY/NONE form.
     * Counter reports, reductions, acquire, trap and awaken need execution
     * support of their own and must not silently become memory stores. */
    if(!out || upper>255 || (lower&3) ||
       (control & ~UINT32_C(0x1000f013)) || (control&0xf000)!=0xf000 ||
       ((control&3)!=0 && (control&3)!=2) ||
       ((control&3)==2 && (payload || (control&UINT32_C(0x10000010)))))return 0;
    BootGpuSemaphore result={((uint64_t)upper<<32)|lower,payload,
        (control&UINT32_C(0x10000000))?1:4,
        (control&UINT32_C(0x10000000))?0:1,(control>>4)&1};
    if(result.address>UINT64_C(0xffffffffff)-(result.words*4-1))return 0;
    *out=result;return 1;
}
static inline int boot_gpu_syncpoint_decode(uint32_t value,uint32_t*index,
        uint32_t*clean_l2,uint32_t*rop_done){
    if(!index || !clean_l2 || !rop_done || (value&~UINT32_C(0x00110fff)))return 0;
    *index=value&4095;*clean_l2=(value>>16)&1;*rop_done=(value>>20)&1;return 1;
}
#endif
