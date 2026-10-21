#ifndef TOTK_HOST_GPU_REGISTER_PROFILE_H
#define TOTK_HOST_GPU_REGISTER_PROFILE_H
#include <stdint.h>
#include <stddef.h>
/* Exact initialization writes corroborated by deko3d gpu_3d_base.cpp.
 * These describe source configuration. Windows owns physical GPU registers;
 * this layer never writes host MMIO or claims any rendering has completed. */
typedef struct BootGpuProfileEntry {uint32_t address,value,known_mask;} BootGpuProfileEntry;
typedef struct BootGpuRegisterProfile {BootGpuProfileEntry entries[32];uint32_t writes;} BootGpuRegisterProfile;
static const uint32_t boot_gpu_profile_initialization[][3]={
    {UINT32_C(0x404468),UINT32_C(0x7ffffff),UINT32_C(0x3fffffff)},
    {UINT32_C(0x418800),UINT32_C(0x0),UINT32_C(0x1800000)},
    {UINT32_C(0x418800),UINT32_C(0x1),UINT32_C(0x1)},
    {UINT32_C(0x418e40),UINT32_C(0x7),UINT32_C(0xf)},
    {UINT32_C(0x418e40),UINT32_C(0x70),UINT32_C(0xf0)},
    {UINT32_C(0x418e40),UINT32_C(0x700),UINT32_C(0xf00)},
    {UINT32_C(0x418e40),UINT32_C(0x7000),UINT32_C(0xf000)},
    {UINT32_C(0x418e40),UINT32_C(0x70000),UINT32_C(0xf0000)},
    {UINT32_C(0x418e40),UINT32_C(0x700000),UINT32_C(0xf00000)},
    {UINT32_C(0x418e40),UINT32_C(0x7000000),UINT32_C(0xf000000)},
    {UINT32_C(0x418e40),UINT32_C(0x70000000),UINT32_C(0xf0000000)},
    {UINT32_C(0x418e44),UINT32_C(0x7),UINT32_C(0xf)},
    {UINT32_C(0x418e44),UINT32_C(0x70),UINT32_C(0xf0)},
    {UINT32_C(0x418e44),UINT32_C(0x700),UINT32_C(0xf00)},
    {UINT32_C(0x418e58),UINT32_C(0x842),UINT32_C(0xffff)},
    {UINT32_C(0x418e58),UINT32_C(0x4f10000),UINT32_C(0xffff0000)},
    {UINT32_C(0x418e5c),UINT32_C(0x53),UINT32_C(0xffff)},
    {UINT32_C(0x418e5c),UINT32_C(0xe90000),UINT32_C(0xffff0000)},
    {UINT32_C(0x418e60),UINT32_C(0xea),UINT32_C(0xffff)},
    {UINT32_C(0x418e60),UINT32_C(0xeb0000),UINT32_C(0xffff0000)},
    {UINT32_C(0x418e64),UINT32_C(0x208),UINT32_C(0xffff)},
    {UINT32_C(0x418e64),UINT32_C(0x2090000),UINT32_C(0xffff0000)},
    {UINT32_C(0x418e68),UINT32_C(0x20a),UINT32_C(0xffff)},
    {UINT32_C(0x418e68),UINT32_C(0x20b0000),UINT32_C(0xffff0000)},
    {UINT32_C(0x418e6c),UINT32_C(0x644),UINT32_C(0xffff)},
    {UINT32_C(0x419a04),UINT32_C(0x1),UINT32_C(0x1)},
    {UINT32_C(0x419a04),UINT32_C(0x2),UINT32_C(0x2)},
    {UINT32_C(0x419a08),UINT32_C(0x0),UINT32_C(0x10)},
    {UINT32_C(0x419f78),UINT32_C(0x0),UINT32_C(0x8)},
};
static inline int boot_gpu_profile_write(BootGpuRegisterProfile*p,uint32_t address,uint32_t value,uint32_t mask){
    if(!p)return 0;
    size_t i=0;for(;i<sizeof(boot_gpu_profile_initialization)/sizeof(boot_gpu_profile_initialization[0]);++i){
        const uint32_t*t=boot_gpu_profile_initialization[i];if(t[0]==address && t[1]==value && t[2]==mask)break;
    }
    if(i==sizeof(boot_gpu_profile_initialization)/sizeof(boot_gpu_profile_initialization[0]))return 0;
    unsigned slot=0;while(slot<32 && p->entries[slot].address && p->entries[slot].address!=address)++slot;
    if(slot==32 || p->writes==UINT32_MAX)return 0;
    BootGpuProfileEntry*e=&p->entries[slot];e->address=address;e->value=(e->value&~mask)|(value&mask);e->known_mask|=mask;++p->writes;return 1;
}
#endif
