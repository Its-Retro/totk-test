#ifndef TOTK_HOST_GPU_CHANNEL_H
#define TOTK_HOST_GPU_CHANNEL_H
#include <initguid.h>
#include <stddef.h>
#include "host_gpu.h"
#include "host_gpu_memory_batch.h"
#include "host_gpu_write_plan.h"
#include <string.h>
typedef struct BootGpuChannel {
    uint32_t as_fd,nvmap_fd,capacity,syncpoint;
    uint64_t *entries;
    uint64_t user_data;
    uint32_t timeout_ms;
    ID3D12CommandAllocator*allocator;
    uint32_t class_id;
    HANDLE error_event;
    uint32_t error_enabled,error_info[32];
    uint32_t syncpoint_value; /* Reuses the former four-byte alignment gap. */
    BootGpuStage*state;
    BootGpuMemoryBatch*pending;
    uint64_t verified_submissions;
} BootGpuChannel;
_Static_assert(offsetof(BootGpuChannel,state)==200 && sizeof(BootGpuChannel)==224,"retained channel ABI");

/* Allocation only. No source command is submitted or acknowledged here. */
static inline int boot_gpu_channel_allocate(BootGpuChannel*c,BootGpu**gpu,uint32_t*next,uint32_t count){
    if(!c || !gpu || !next || !c->as_fd || !c->nvmap_fd || c->entries ||
       !count || count>65536 || (count&(count-1)) || *next==UINT32_MAX)return 0;
    uint64_t*entries=calloc(count,sizeof(*entries));if(!entries)return 0;
    BootGpu*g=*gpu;if(!g)g=boot_gpu_open();
    if(!g){free(entries);return 0;}
    c->entries=entries;c->capacity=count;c->syncpoint=++*next;*gpu=g;return 1;
}
static inline int boot_gpu_channel_context(BootGpuChannel*c,BootGpu*g,uint32_t class_id,uint32_t flags){
    if(!c || !g || !c->entries || c->allocator || class_id!=0xb197 || flags)return 0;
    ID3D12CommandAllocator*a=NULL;
    if(FAILED(ID3D12Device_CreateCommandAllocator(g->device,D3D12_COMMAND_LIST_TYPE_DIRECT,&IID_ID3D12CommandAllocator,(void**)&a)))return 0;
    c->allocator=a;c->class_id=class_id;return 1;
}
static inline int boot_gpu_channel_close(BootGpuChannel*c){
    if(c->pending && !boot_gpu_memory_batch_close(c->pending))return 0;
    if(c->allocator)ID3D12CommandAllocator_Release(c->allocator);
    if(c->error_event)CloseHandle(c->error_event);
    free(c->state);free(c->entries);memset(c,0,sizeof(*c));return 1;
}
static inline int boot_gpu_channel_notifier(BootGpuChannel*c,uint32_t enable){
    if(!c || !c->allocator)return 0;
    HANDLE event=c->error_event;
    if(!event){event=CreateEventW(NULL,TRUE,FALSE,NULL);if(!event)return 0;}
    if(!ResetEvent(event)){if(!c->error_event)CloseHandle(event);return 0;}
    c->error_event=event;c->error_enabled=enable!=0;memset(c->error_info,0,sizeof(c->error_info));return 1;
}
#endif
