#ifndef TOTK_HOST_BUFFER_QUEUE_H
#define TOTK_HOST_BUFFER_QUEUE_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef struct BootBufferQueue {uint32_t connected_api,width,height,controlled_by_app,format,buffer_count;} BootBufferQueue;
static inline uint32_t boot_parcel_u32(const uint8_t*p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static inline void boot_parcel_put(uint8_t*p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(n>>(i*8));}
/* The observed connect has no listener/object handles. Reject unknown parcel
 * variants before changing the connection or returning an output payload. */
static inline int boot_buffer_queue_connect(BootBufferQueue*q,const uint8_t*input,size_t size,
        uint32_t width,uint32_t height,uint8_t*output,size_t capacity){
    static const char token[]="android.gui.IGraphicBufferProducer";
    if(!q || !input || !output || size!=108 || capacity<36 || q->connected_api || !width || !height || width>16384 || height>16384)return 0;
    if(boot_parcel_u32(input)!=92 || boot_parcel_u32(input+4)!=16 || boot_parcel_u32(input+8)!=0 || boot_parcel_u32(input+12)!=108 ||
       boot_parcel_u32(input+16)!=256 || boot_parcel_u32(input+20)!=34)return 0;
    for(unsigned i=0;i<sizeof(token);++i)if(input[24+i*2]!=(uint8_t)token[i] || input[25+i*2])return 0;
    if(input[94] || input[95] || boot_parcel_u32(input+96)!=0 || boot_parcel_u32(input+100)!=1 || boot_parcel_u32(input+104)>1)return 0;
    uint8_t reply[36]={0};boot_parcel_put(reply,20);boot_parcel_put(reply+4,16);boot_parcel_put(reply+12,36);
    boot_parcel_put(reply+16,width);boot_parcel_put(reply+20,height);
    /* Identity transform, no queued buffers, successful connection. */
    q->connected_api=1;q->width=width;q->height=height;q->controlled_by_app=boot_parcel_u32(input+104);q->format=1; /* RGBA8888 queue configuration. */
    memcpy(output,reply,sizeof(reply));return 1;
}
static inline int boot_buffer_queue_value(const uint8_t*input,size_t size,uint32_t*out){
    static const char token[]="android.gui.IGraphicBufferProducer";
    if(!input || !out || size!=100)return 0;
    if(boot_parcel_u32(input)!=84 || boot_parcel_u32(input+4)!=16 || boot_parcel_u32(input+8) || boot_parcel_u32(input+12)!=100 ||
        boot_parcel_u32(input+16)!=256 || boot_parcel_u32(input+20)!=34 || input[94] || input[95])return 0;
    for(unsigned i=0;i<sizeof(token);++i)if(input[24+i*2]!=(uint8_t)token[i] || input[25+i*2])return 0;
    *out=boot_parcel_u32(input+96);return 1;
}
static inline int boot_buffer_queue_query(const BootBufferQueue*q,const uint8_t*input,size_t size,uint8_t*output,size_t capacity){
    uint32_t which;if(!q || !q->connected_api || !output || capacity<24 || !boot_buffer_queue_value(input,size,&which))return 0;
    uint32_t value;switch(which){case 0:value=q->width;break;case 1:value=q->height;break;case 2:value=q->format;break;default:return 0;}
    uint8_t reply[24]={0};boot_parcel_put(reply,8);boot_parcel_put(reply+4,16);boot_parcel_put(reply+12,24);boot_parcel_put(reply+16,value);
    memcpy(output,reply,sizeof(reply));return 1;
}
#endif
