#ifndef TOTK_HOST_GPU_MEMORY_BATCH_H
#define TOTK_HOST_GPU_MEMORY_BATCH_H
#include "host_gpu.h"
#include "host_gpu_timestamp.h"
#include <string.h>
typedef struct BootGpuMemorySpan {uint8_t*destination;const uint8_t*contents;size_t size;} BootGpuMemorySpan;
typedef struct BootGpuMemoryBatch {
    struct {ID3D12Resource*upload,*device,*readback;uint8_t*expected,*mapped,*destination;size_t size;} spans[16];
    ID3D12CommandAllocator*allocator;
    ID3D12GraphicsCommandList*list;
    unsigned count,submitted,completed,verified;
    BootGpuTimestamp*pending_timestamp;
} BootGpuMemoryBatch;
/* Resources of completed batches are kept for reuse (per span index, grow-only) instead of being
 * created and destroyed per batch. A batch whose completion is uncertain keeps its own (the owner
 * retains it, see boot_gpu_memory_batch_close). Pool entries belong to `device`. */
/* Pooled span buffers are at most this large (three buffers per span index; larger spans are rare). */
#define BOOT_GPU_BATCH_POOL_MAX ((size_t)8<<20)
static struct {
    ID3D12Device*device;
    struct {ID3D12Resource*upload,*device,*readback;size_t capacity;} spans[16];
    ID3D12CommandAllocator*allocator;ID3D12GraphicsCommandList*list;
    uint64_t created;
} boot_gpu_batch_pool;
static inline void boot_gpu_batch_pool_drop(void){
    for(unsigned i=0;i<16;++i){
        if(boot_gpu_batch_pool.spans[i].upload)ID3D12Resource_Release(boot_gpu_batch_pool.spans[i].upload);
        if(boot_gpu_batch_pool.spans[i].device)ID3D12Resource_Release(boot_gpu_batch_pool.spans[i].device);
        if(boot_gpu_batch_pool.spans[i].readback)ID3D12Resource_Release(boot_gpu_batch_pool.spans[i].readback);
    }
    if(boot_gpu_batch_pool.list)ID3D12GraphicsCommandList_Release(boot_gpu_batch_pool.list);
    if(boot_gpu_batch_pool.allocator)ID3D12CommandAllocator_Release(boot_gpu_batch_pool.allocator);
    uint64_t created=boot_gpu_batch_pool.created;memset(&boot_gpu_batch_pool,0,sizeof boot_gpu_batch_pool);boot_gpu_batch_pool.created=created;
}
static inline int boot_gpu_memory_batch_close(BootGpuMemoryBatch*b){
    if(!b)return 1;
    /* On uncertain completion the owner must retain the entire batch. */
    if(b->submitted && !b->completed)return 0;
    if(!boot_gpu_timestamp_close(b->pending_timestamp))return 0;
    /* Completed (or never submitted): the queue no longer references these objects. */
    ID3D12Device*device=NULL;
    if(b->allocator && SUCCEEDED(ID3D12CommandAllocator_GetDevice(b->allocator,&IID_ID3D12Device,(void**)&device)))ID3D12Device_Release(device);
    int pool=device && device==boot_gpu_batch_pool.device;
    if(pool && !boot_gpu_batch_pool.list && b->list && b->allocator){boot_gpu_batch_pool.list=b->list;boot_gpu_batch_pool.allocator=b->allocator;b->list=NULL;b->allocator=NULL;}
    if(b->list)ID3D12GraphicsCommandList_Release(b->list);
    if(b->allocator)ID3D12CommandAllocator_Release(b->allocator);
    for(unsigned i=0;i<16;++i){
        if(b->spans[i].mapped){D3D12_RANGE empty={0,0};ID3D12Resource_Unmap(b->spans[i].readback,0,&empty);}
        if(pool && b->spans[i].upload && b->spans[i].device && b->spans[i].readback){
            D3D12_RESOURCE_DESC desc;b->spans[i].upload->lpVtbl->GetDesc(b->spans[i].upload,&desc);
            /* keep the larger set: a smaller pooled one made every larger span create and destroy three
             * committed buffers per batch (pooled buffers are idle, nothing references them) */
            if(desc.Width<=BOOT_GPU_BATCH_POOL_MAX && boot_gpu_batch_pool.spans[i].upload && boot_gpu_batch_pool.spans[i].capacity<(size_t)desc.Width){
                ID3D12Resource_Release(boot_gpu_batch_pool.spans[i].upload);ID3D12Resource_Release(boot_gpu_batch_pool.spans[i].device);
                ID3D12Resource_Release(boot_gpu_batch_pool.spans[i].readback);memset(&boot_gpu_batch_pool.spans[i],0,sizeof boot_gpu_batch_pool.spans[i]);
            }
        }
        if(pool && b->spans[i].upload && b->spans[i].device && b->spans[i].readback && !boot_gpu_batch_pool.spans[i].upload){
            D3D12_RESOURCE_DESC desc;b->spans[i].upload->lpVtbl->GetDesc(b->spans[i].upload,&desc);
            if(desc.Width<=BOOT_GPU_BATCH_POOL_MAX){boot_gpu_batch_pool.spans[i].upload=b->spans[i].upload;boot_gpu_batch_pool.spans[i].device=b->spans[i].device;
            boot_gpu_batch_pool.spans[i].readback=b->spans[i].readback;boot_gpu_batch_pool.spans[i].capacity=(size_t)desc.Width;
            b->spans[i].upload=b->spans[i].device=b->spans[i].readback=NULL;}
        }
        if(b->spans[i].readback)ID3D12Resource_Release(b->spans[i].readback);
        if(b->spans[i].device)ID3D12Resource_Release(b->spans[i].device);
        if(b->spans[i].upload)ID3D12Resource_Release(b->spans[i].upload);
        free(b->spans[i].expected);
    }
    free(b);return 1;
}
static inline BootGpuMemoryBatch*boot_gpu_memory_batch_prepare(BootGpu*g,const BootGpuMemorySpan*spans,unsigned count){
    if(!g || !spans || !count || count>16)return NULL;
    size_t total=0;
    for(unsigned i=0;i<count;++i){
        if(!spans[i].destination || !spans[i].contents || !spans[i].size || spans[i].size>16777216 || total>67108864-spans[i].size)return NULL;
        total+=spans[i].size;
        uintptr_t start=(uintptr_t)spans[i].destination;
        if(start>UINTPTR_MAX-spans[i].size)return NULL;
        for(unsigned j=0;j<i;++j){uintptr_t other=(uintptr_t)spans[j].destination;if(start<other+spans[j].size && other<start+spans[i].size)return NULL;}
    }
    if(boot_gpu_batch_pool.device!=g->device){boot_gpu_batch_pool_drop();boot_gpu_batch_pool.device=g->device;}
    BootGpuMemoryBatch*b=calloc(1,sizeof(*b));if(!b)return NULL;b->count=count;
    if(boot_gpu_batch_pool.list){
        b->allocator=boot_gpu_batch_pool.allocator;b->list=boot_gpu_batch_pool.list;boot_gpu_batch_pool.allocator=NULL;boot_gpu_batch_pool.list=NULL;
        if(FAILED(ID3D12CommandAllocator_Reset(b->allocator)) || FAILED(ID3D12GraphicsCommandList_Reset(b->list,b->allocator,NULL)))goto fail;
    }else{
        if(FAILED(ID3D12Device_CreateCommandAllocator(g->device,D3D12_COMMAND_LIST_TYPE_DIRECT,&IID_ID3D12CommandAllocator,(void**)&b->allocator)))goto fail;
        if(FAILED(ID3D12Device_CreateCommandList(g->device,0,D3D12_COMMAND_LIST_TYPE_DIRECT,b->allocator,NULL,&IID_ID3D12GraphicsCommandList,(void**)&b->list)))goto fail;
    }
    for(unsigned i=0;i<count;++i){
        b->spans[i].size=spans[i].size;b->spans[i].destination=spans[i].destination;
        b->spans[i].expected=malloc(spans[i].size);if(!b->spans[i].expected)goto fail;
        memcpy(b->spans[i].expected,spans[i].contents,spans[i].size);
        if(boot_gpu_batch_pool.spans[i].upload && boot_gpu_batch_pool.spans[i].capacity>=spans[i].size){
            /* reused: device buffer is back in COPY_DEST (the previous list ended with that barrier) */
            b->spans[i].upload=boot_gpu_batch_pool.spans[i].upload;b->spans[i].device=boot_gpu_batch_pool.spans[i].device;
            b->spans[i].readback=boot_gpu_batch_pool.spans[i].readback;
            boot_gpu_batch_pool.spans[i].upload=boot_gpu_batch_pool.spans[i].device=boot_gpu_batch_pool.spans[i].readback=NULL;
        }else{
            /* grow-only: power of two from 64 KiB */
            size_t capacity=65536;while(capacity<spans[i].size)capacity*=2;
            D3D12_RESOURCE_DESC desc={0};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=capacity;
            desc.Height=desc.DepthOrArraySize=desc.MipLevels=desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            D3D12_HEAP_PROPERTIES heap={0};heap.CreationNodeMask=heap.VisibleNodeMask=1;heap.Type=D3D12_HEAP_TYPE_UPLOAD;
            ++boot_gpu_batch_pool.created;
            if(FAILED(ID3D12Device_CreateCommittedResource(g->device,&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_GENERIC_READ,NULL,&IID_ID3D12Resource,(void**)&b->spans[i].upload)))goto fail;
            heap.Type=D3D12_HEAP_TYPE_DEFAULT;
            if(FAILED(ID3D12Device_CreateCommittedResource(g->device,&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,NULL,&IID_ID3D12Resource,(void**)&b->spans[i].device)))goto fail;
            heap.Type=D3D12_HEAP_TYPE_READBACK;
            if(FAILED(ID3D12Device_CreateCommittedResource(g->device,&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,NULL,&IID_ID3D12Resource,(void**)&b->spans[i].readback)))goto fail;
        }
        void*mapped=NULL;D3D12_RANGE empty={0,0},written={0,spans[i].size};
        if(FAILED(ID3D12Resource_Map(b->spans[i].upload,0,&empty,&mapped)))goto fail;
        memcpy(mapped,b->spans[i].expected,spans[i].size);ID3D12Resource_Unmap(b->spans[i].upload,0,&written);
        ID3D12GraphicsCommandList_CopyBufferRegion(b->list,b->spans[i].device,0,b->spans[i].upload,0,spans[i].size);
        D3D12_RESOURCE_BARRIER barrier={0};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource=b->spans[i].device;barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        ID3D12GraphicsCommandList_ResourceBarrier(b->list,1,&barrier);
        ID3D12GraphicsCommandList_CopyBufferRegion(b->list,b->spans[i].readback,0,b->spans[i].device,0,spans[i].size);
        barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_SOURCE;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_DEST;
        ID3D12GraphicsCommandList_ResourceBarrier(b->list,1,&barrier);
    }
    if(FAILED(ID3D12GraphicsCommandList_Close(b->list)))goto fail;
    return b;
fail:
    /* never submitted: nothing references the objects; a list left open is not pooled */
    if(b->list){ID3D12GraphicsCommandList_Close(b->list);}
    boot_gpu_memory_batch_close(b);return NULL;
}
static inline int boot_gpu_memory_batch_execute(BootGpu*g,BootGpuMemoryBatch*b){
    if(!g || !b || b->submitted)return 0;
    ID3D12CommandList*lists[]={(ID3D12CommandList*)b->list};
    ID3D12CommandQueue_ExecuteCommandLists(g->queue,1,lists);b->submitted=1;
    if(!boot_gpu_wait(g))return 0;b->completed=1;
    /* Verify every result before committing any destination bytes. */
    for(unsigned i=0;i<b->count;++i){D3D12_RANGE range={0,b->spans[i].size};
        if(FAILED(ID3D12Resource_Map(b->spans[i].readback,0,&range,(void**)&b->spans[i].mapped)))return 0;
        if(memcmp(b->spans[i].mapped,b->spans[i].expected,b->spans[i].size))return 0;
    }
    for(unsigned i=0;i<b->count;++i)memcpy(b->spans[i].destination,b->spans[i].mapped,b->spans[i].size);
    b->verified=1;return 1;
}
#endif
