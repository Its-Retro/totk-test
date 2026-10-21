#ifndef TOTK_HOST_GRAPHIC_BUFFER_H
#define TOTK_HOST_GRAPHIC_BUFFER_H
#include "host_buffer_queue.h"
typedef struct BootGraphicBuffer {
    uint32_t slot,object_id,width,height,stride,format,usage,pitch,kind,block_height_log2;
    uint32_t offset,total_size;
    uint64_t size;
    uint8_t serialized[364];
} BootGraphicBuffer;
static inline int boot_graphic_buffer_parse(const uint8_t*p,size_t length,BootGraphicBuffer*out){
    static const char token[]="android.gui.IGraphicBufferProducer";
    if(!p || !out || length!=476)return 0;
    if(boot_parcel_u32(p)!=460 || boot_parcel_u32(p+4)!=16 || boot_parcel_u32(p+8) || boot_parcel_u32(p+12)!=476 ||
        boot_parcel_u32(p+16)!=256 || boot_parcel_u32(p+20)!=34 || p[94] || p[95])return 0;
    for(unsigned i=0;i<sizeof(token);++i)if(p[24+i*2]!=(uint8_t)token[i] || p[25+i*2])return 0;
    if(boot_parcel_u32(p+96)>=64 || boot_parcel_u32(p+100)!=1 || boot_parcel_u32(p+104)!=364 || boot_parcel_u32(p+108) ||
        boot_parcel_u32(p+112)!=0x47424652 || boot_parcel_u32(p+144) || boot_parcel_u32(p+148)!=81 ||
        boot_parcel_u32(p+152)!=UINT32_MAX || !boot_parcel_u32(p+156) || boot_parcel_u32(p+160) || boot_parcel_u32(p+164)!=0xdaffcaff ||
        boot_parcel_u32(p+172) || boot_parcel_u32(p+196)!=1 || boot_parcel_u32(p+200))return 0;
    BootGraphicBuffer b={0};b.slot=boot_parcel_u32(p+96);b.object_id=boot_parcel_u32(p+156);
    b.width=boot_parcel_u32(p+116);b.height=boot_parcel_u32(p+120);b.stride=boot_parcel_u32(p+124);
    b.format=boot_parcel_u32(p+128);b.usage=boot_parcel_u32(p+132);b.total_size=boot_parcel_u32(p+192);
    b.pitch=boot_parcel_u32(p+224);b.offset=boot_parcel_u32(p+232);b.kind=boot_parcel_u32(p+236);b.block_height_log2=boot_parcel_u32(p+240);
    b.size=(uint64_t)boot_parcel_u32(p+260)|((uint64_t)boot_parcel_u32(p+264)<<32);
    if(!b.width || !b.height || b.width>8192 || b.height>8192 || b.stride<b.width || b.stride>8192 || b.format!=1 || b.usage!=0xb00 ||
        boot_parcel_u32(p+176)!=b.usage || boot_parcel_u32(p+180)!=b.format || boot_parcel_u32(p+184)!=b.format ||
        boot_parcel_u32(p+188)!=b.stride || boot_parcel_u32(p+204)!=b.width || boot_parcel_u32(p+208)!=b.height ||
        boot_parcel_u32(p+212)!=0x532120 || boot_parcel_u32(p+216)!=1 || boot_parcel_u32(p+220)!=3 ||
        b.pitch!=b.stride*4 || (b.pitch&63) || b.kind!=0xfe || b.block_height_log2>5 || boot_parcel_u32(p+244) || boot_parcel_u32(p+248) ||
        boot_parcel_u32(p+252) || boot_parcel_u32(p+256)!=0x80000000 || (b.offset&511) || b.total_size>67108864 ||
        b.size>b.total_size)return 0;
    uint32_t rows=8u<<b.block_height_log2;
    uint64_t expected=(uint64_t)b.pitch*((b.height+rows-1)/rows*rows);
    if(b.size!=expected)return 0;
    /* Compression metadata and unused planes must be absent in this profile.
     * Serialized process IDs, unused handle and trailing pointer confer no authority. */
    for(unsigned i=268;i<468;++i)if(p[i])return 0;
    memcpy(b.serialized,p+112,364);*out=b;return 1;
}
#endif
