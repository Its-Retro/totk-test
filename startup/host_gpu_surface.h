#ifndef TOTK_HOST_GPU_SURFACE_H
#define TOTK_HOST_GPU_SURFACE_H
#include "host_gpu.h"

/* Synchronous first-frame presentation foundation. The HWND and BootGpu must
 * outlive the surface. This interface does not label submitted work as a game
 * frame: that requires a guest submission provenance path, not yet present. */
typedef struct BootGpuSurface {
    BootGpu *gpu;
    IDXGISwapChain3 *chain;
    ID3D12Resource *buffers[2];
    ID3D12DescriptorHeap *rtvs;
    UINT descriptor_size;
    uint64_t present_attempts;
    HRESULT last_present;
} BootGpuSurface;

/* On failed GPU completion, retain resources and let the caller tear down
 * the device/process. Never release resources still referenced by the GPU. */
static inline int boot_gpu_surface_close(BootGpuSurface *s){
    if(!s)return 1;
    if(s->present_attempts && !boot_gpu_wait(s->gpu))return 0;
    for(unsigned i=0;i<2;++i)if(s->buffers[i])ID3D12Resource_Release(s->buffers[i]);
    if(s->rtvs)ID3D12DescriptorHeap_Release(s->rtvs);
    if(s->chain)IDXGISwapChain3_Release(s->chain);
    free(s);return 1;
}

static inline BootGpuSurface *boot_gpu_surface_open(BootGpu *gpu,HWND hwnd,UINT width,UINT height){
    if(!gpu || !IsWindow(hwnd) || !width || !height || width>16384 || height>16384)return NULL;
    BootGpuSurface *s=calloc(1,sizeof(*s));if(!s)return NULL;s->gpu=gpu;
    IDXGIFactory2 *factory=NULL;IDXGISwapChain1 *chain=NULL;
    if(FAILED(CreateDXGIFactory1(&IID_IDXGIFactory2,(void**)&factory)))goto fail;
    DXGI_SWAP_CHAIN_DESC1 desc={0};desc.Width=width;desc.Height=height;
    desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;
    desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=2;
    desc.Scaling=DXGI_SCALING_STRETCH;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;desc.AlphaMode=DXGI_ALPHA_MODE_IGNORE;
    HRESULT result=IDXGIFactory2_CreateSwapChainForHwnd(factory,(IUnknown*)gpu->queue,hwnd,&desc,NULL,NULL,&chain);
    IDXGIFactory2_Release(factory);factory=NULL;
    if(FAILED(result))goto fail;
    result=IDXGISwapChain1_QueryInterface(chain,&IID_IDXGISwapChain3,(void**)&s->chain);
    IDXGISwapChain1_Release(chain);chain=NULL;if(FAILED(result))goto fail;
    D3D12_DESCRIPTOR_HEAP_DESC hd={0};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=2;
    if(FAILED(ID3D12Device_CreateDescriptorHeap(gpu->device,&hd,&IID_ID3D12DescriptorHeap,(void**)&s->rtvs)))goto fail;
    s->descriptor_size=ID3D12Device_GetDescriptorHandleIncrementSize(gpu->device,D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE handle;s->rtvs->lpVtbl->GetCPUDescriptorHandleForHeapStart(s->rtvs,&handle);
    for(UINT i=0;i<2;++i){
        if(FAILED(IDXGISwapChain3_GetBuffer(s->chain,i,&IID_ID3D12Resource,(void**)&s->buffers[i])))goto fail;
        ID3D12Device_CreateRenderTargetView(gpu->device,s->buffers[i],NULL,handle);handle.ptr+=s->descriptor_size;
    }
    return s;
fail:
    if(chain)IDXGISwapChain1_Release(chain);
    if(factory)IDXGIFactory2_Release(factory);
    boot_gpu_surface_close(s);return NULL;
}

static inline int boot_gpu_surface_target(BootGpuSurface *s,ID3D12Resource **resource,D3D12_CPU_DESCRIPTOR_HANDLE *handle){
    if(!s || !resource || !handle)return 0;
    UINT index=IDXGISwapChain3_GetCurrentBackBufferIndex(s->chain);if(index>=2)return 0;
    s->rtvs->lpVtbl->GetCPUDescriptorHandleForHeapStart(s->rtvs,handle);handle->ptr+=index*s->descriptor_size;
    *resource=s->buffers[index];return 1;
}

static inline int boot_gpu_surface_present(BootGpuSurface *s){
    if(!s || s->present_attempts==UINT64_MAX)return 0;
    ++s->present_attempts;s->last_present=IDXGISwapChain3_Present(s->chain,0,0);
    /* Even an occluded window needs completion before allocator/resource reuse. */
    int completed=boot_gpu_wait(s->gpu);
    return SUCCEEDED(s->last_present) && completed;
}
#endif
