#ifndef TOTK_HOST_GRAPHIC_REGISTRY_H
#define TOTK_HOST_GRAPHIC_REGISTRY_H
#include "host_graphic_buffer.h"
#include "host_nvmap.h"
typedef struct BootGraphicSlot {BootGraphicBuffer buffer;uint64_t cpu;uint32_t owner_fd,handle,registered,state,requested;} BootGraphicSlot;
typedef struct BootGraphicRegistry {BootGraphicSlot slots[2][64];} BootGraphicRegistry;
static inline int boot_graphic_available(const BootGraphicRegistry*r,unsigned producer){
    if(!r || producer>=2)return 0;
    int available=0;for(unsigned i=0;i<64;++i){const BootGraphicSlot*s=&r->slots[producer][i];
        if(s->registered && s->state==1)return 0;
        if(s->registered && !s->state)available=1;}
    return available;
}
static inline int boot_graphic_bind(const BootGraphicBuffer*b,const BootNvMapObject*object,BootGraphicSlot*out){
    if(!b || !object || !out || b->slot>=64 || !b->size || !object->address || !object->allocated || object->flags!=1 || !object->owner_fd || object->handle!=b->object_id ||
        b->total_size>object->size || b->offset>object->size || b->size>object->size-b->offset ||
        object->address>UINT64_MAX-b->offset-b->size)return 0;
    BootGraphicSlot s={0};s.buffer=*b;s.cpu=object->address+b->offset;s.owner_fd=object->owner_fd;s.handle=object->handle;s.registered=1;*out=s;return 1;
}
static inline int boot_graphic_object_used(const BootGraphicRegistry*r,uint32_t owner,uint32_t handle){
    for(unsigned i=0;i<2;++i)for(unsigned j=0;j<64;++j){const BootGraphicSlot*s=&r->slots[i][j];
        if(s->registered && s->owner_fd==owner && (!handle || s->handle==handle))return 1;}
    return 0;
}
static inline int boot_graphic_cpu_overlap(const BootGraphicRegistry*r,uint64_t begin,uint64_t end){
    for(unsigned i=0;i<2;++i)for(unsigned j=0;j<64;++j){const BootGraphicSlot*s=&r->slots[i][j];
        if(s->registered && begin<s->cpu+s->buffer.size && s->cpu<end)return 1;}
    return 0;
}
static inline int boot_graphic_set_count(const BootGraphicRegistry*r,unsigned producer,BootBufferQueue*q,const uint8_t*input,size_t size,uint8_t*output,size_t capacity){
    uint32_t count;if(!r || producer>=2 || !q || !q->connected_api || !output || capacity<20 ||
        !boot_buffer_queue_value(input,size,&count) || count<2 || count>64)return 0;
    for(unsigned i=0;i<64;++i)if(r->slots[producer][i].registered && (i>=count || r->slots[producer][i].state))return 0;
    uint8_t reply[20]={0};boot_parcel_put(reply,4);boot_parcel_put(reply+4,16);boot_parcel_put(reply+12,20);
    q->buffer_count=count;memcpy(output,reply,20);return 1;
}
/* Prepare a first-use acquisition. No fence is invented: these free buffers
 * have no outstanding GPU use. A previously queued buffer is never selected. */
static inline int boot_graphic_dequeue_plan(const BootGraphicRegistry*r,unsigned producer,const BootBufferQueue*q,const uint8_t*input,size_t size,uint8_t*output,size_t capacity,unsigned*out_slot){
    static const char token[]="android.gui.IGraphicBufferProducer";
    if(!r || producer>=2 || !q || !q->connected_api || !q->buffer_count || q->buffer_count>64 || !input || !output || !out_slot || size!=116 || capacity<72)return 0;
    if(boot_parcel_u32(input)!=100 || boot_parcel_u32(input+4)!=16 || boot_parcel_u32(input+8) || boot_parcel_u32(input+12)!=116 ||
        boot_parcel_u32(input+16)!=256 || boot_parcel_u32(input+20)!=34 || input[94] || input[95] || boot_parcel_u32(input+96)!=1)return 0;
    for(unsigned i=0;i<sizeof(token);++i)if(input[24+i*2]!=(uint8_t)token[i] || input[25+i*2])return 0;
    uint32_t width=boot_parcel_u32(input+100),height=boot_parcel_u32(input+104),format=boot_parcel_u32(input+108),usage=boot_parcel_u32(input+112);
    if(!width || !height || (format && format!=q->format) || (usage&~UINT32_C(0xb00)))return 0;
    unsigned slot=64;
    int available=boot_graphic_available(r,producer);
    if(available)for(unsigned i=0;i<q->buffer_count;++i){const BootGraphicSlot*s=&r->slots[producer][i];
        if(s->registered && !s->state && s->buffer.width==width && s->buffer.height==height && (!format || s->buffer.format==format) &&
            (s->buffer.usage&usage)==usage){slot=i;break;}}
    if(available && slot==64)return 0; /* Resizing/reallocation is not implemented. */
    uint8_t reply[72]={0};boot_parcel_put(reply,56);boot_parcel_put(reply+4,16);boot_parcel_put(reply+12,72);
    boot_parcel_put(reply+16,slot<64?slot:UINT32_MAX);boot_parcel_put(reply+20,1);boot_parcel_put(reply+24,36);
    boot_parcel_put(reply+68,slot<64?(r->slots[producer][slot].requested?0:1):(uint32_t)-11);
    *out_slot=slot;memcpy(output,reply,72);return 1;
}
static inline int boot_graphic_request_buffer(BootGraphicRegistry*r,unsigned producer,const BootBufferQueue*q,const uint8_t*input,size_t size,uint8_t*output,size_t capacity){
    uint32_t slot;if(!r || producer>=2 || !q || !q->connected_api || !output || capacity<396 || !boot_buffer_queue_value(input,size,&slot) ||
        slot>=64 || slot>=q->buffer_count || !r->slots[producer][slot].registered || r->slots[producer][slot].state!=1)return 0;
    uint8_t reply[396]={0};boot_parcel_put(reply,380);boot_parcel_put(reply+4,16);boot_parcel_put(reply+12,396);
    boot_parcel_put(reply+16,1);boot_parcel_put(reply+20,364);memcpy(reply+28,r->slots[producer][slot].buffer.serialized,364);
    r->slots[producer][slot].requested=1;memcpy(output,reply,396);return 1;
}
#endif
