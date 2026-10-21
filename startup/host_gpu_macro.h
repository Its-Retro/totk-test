#ifndef TOTK_HOST_GPU_MACRO_H
#define TOTK_HOST_GPU_MACRO_H
#include <stdint.h>
#include <stddef.h>
typedef struct BootGpuMacroIo {
    void*opaque;
    int (*read)(void*,uint32_t,uint32_t*);
    int (*write)(void*,uint32_t,uint32_t);
    const uint32_t*params;
    size_t count,consumed;
    uint32_t steps,error;
} BootGpuMacroIo;
/* This layer is used by statically compiled macro programs. It contains no
 * source-instruction dispatch. Callers stage writes until validation succeeds. */
static inline int boot_gpu_macro_param(BootGpuMacroIo*io,uint32_t*out){
    if(io->consumed>=io->count){io->error=1;return 0;}
    *out=io->params[io->consumed++];return 1;
}
static inline int boot_gpu_macro_send(BootGpuMacroIo*io,uint32_t*method,uint32_t increment,uint32_t value){
    if(*method>4095 || !io->write || !io->write(io->opaque,*method*4,value)){io->error=2;return 0;}
    *method+=increment;return 1;
}
static inline int boot_gpu_macro_read(BootGpuMacroIo*io,uint32_t address,uint32_t*out){
    if(address>4095 || !io->read || !io->read(io->opaque,address*4,out)){io->error=3;return 0;}return 1;
}
#endif
