#ifndef TOTK_HOST_GPU_H
#define TOTK_HOST_GPU_H
#define COBJMACROS
#include <stdio.h>
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <stdint.h>
#include <stdlib.h>
#include "host_nvn_wait_trace.h"
#include "host_render_failure.h"

/* Host D3D12 foundation only. No guest GPU characteristics or fences are
 * fabricated from this device. Guest command translation is a separate layer. */
typedef struct BootGpu {
    ID3D12Device *device;
    ID3D12CommandQueue *queue;
    ID3D12Fence *fence;
    HANDLE event;
    uint64_t submitted;
    DXGI_ADAPTER_DESC1 adapter;
} BootGpu;
/*80: this replaces exactly one original query and returns its unmodified result.
 * Original receiver is evaluated once; the writer never dereferences it. */
static inline uint64_t boot_nw_fence_completed(ID3D12Fence*fence,unsigned stage,uint64_t owner,uint64_t target,int known_target){
    uint64_t value=ID3D12Fence_GetCompletedValue(fence);
    boot_nw_note(stage,BOOT_NW_A|BOOT_NW_B|(owner?BOOT_NW_D:0)|(known_target?BOOT_NW_C:0),
                 (uint64_t)(uintptr_t)fence,value,target,owner);return value;
}

static inline void boot_gpu_close(BootGpu *g){
    if(!g)return;
    if(g->fence)ID3D12Fence_Release(g->fence);
    if(g->queue)ID3D12CommandQueue_Release(g->queue);
    if(g->device)ID3D12Device_Release(g->device);
    if(g->event)CloseHandle(g->event);
    free(g);
}

/* DRED (device removed extended data): auto-breadcrumbs and page-fault reports, so a device loss names the
 * operation the GPU was executing and the faulting allocation. Enabled before the device is created; costs a
 * little CPU per command list op. TOTK_DRED=0 turns it off. The report: boot_gpu_dred_dump (host_gpu_draw_exec.h). */
static inline int boot_gpu_dred_on(void){
    static int v=-1;
    if(v<0){char e[8];DWORD n=GetEnvironmentVariableA("TOTK_DRED",e,sizeof e);v=!(n && n<sizeof e && e[0]=='0');}
    return v;
}
static inline void boot_gpu_dred_enable(void){
    static int done;
    if(done || !boot_gpu_dred_on())return;
    done=1;
    ID3D12DeviceRemovedExtendedDataSettings *s=NULL;
    HRESULT hr=D3D12GetDebugInterface(&IID_ID3D12DeviceRemovedExtendedDataSettings,(void**)&s);
    if(SUCCEEDED(hr) && s){
        ID3D12DeviceRemovedExtendedDataSettings_SetAutoBreadcrumbsEnablement(s,D3D12_DRED_ENABLEMENT_FORCED_ON);
        ID3D12DeviceRemovedExtendedDataSettings_SetPageFaultEnablement(s,D3D12_DRED_ENABLEMENT_FORCED_ON);
        {   /* render-30: DRED 1.2 keeps SetMarker/BeginEvent strings with the breadcrumbs only when asked to */
            ID3D12DeviceRemovedExtendedDataSettings1 *s1=NULL;
            if(SUCCEEDED(ID3D12DeviceRemovedExtendedDataSettings_QueryInterface(s,&IID_ID3D12DeviceRemovedExtendedDataSettings1,(void**)&s1)) && s1){
                ID3D12DeviceRemovedExtendedDataSettings1_SetBreadcrumbContextEnablement(s1,D3D12_DRED_ENABLEMENT_FORCED_ON);
                ID3D12DeviceRemovedExtendedDataSettings1_Release(s1);
                fprintf(stderr,"[gpu] DRED breadcrumb contexts (marker strings) enabled\n");
            }
        }
        ID3D12DeviceRemovedExtendedDataSettings_Release(s);
        fprintf(stderr,"[gpu] DRED breadcrumbs + page faults enabled\n");
    }else fprintf(stderr,"[gpu] DRED settings unavailable (hr 0x%08lx): no breadcrumbs on device removal\n",(unsigned long)hr);
}
/* render-35 (TOTK_D3D12_DEBUG=1: debug layer, =2: plus GPU-based validation; slow, diagnostics only): D3D12's own
 * validation names invalid bindings and state hazards (an SRV of a resource bound as a render target, render target /
 * depth views of different sizes, wrong resource states) that can hang the GPU without a page fault. Messages go to
 * draw-path.log at the start of each submission and in the device-removal report (boot_gpu_debug_drain). Needs the
 * Windows "Graphics Tools" optional feature (d3d12SDKLayers.dll); without it the boot says so and runs normally. */
static ID3D12InfoQueue *boot_gpu_info_queue;
static inline int boot_gpu_debug_level(void){
    static int v=-1;
    if(v<0){char e[8];DWORD n=GetEnvironmentVariableA("TOTK_D3D12_DEBUG",e,sizeof e);v=(n && n<sizeof e)?atoi(e):0;}
    return v;
}
static inline void boot_gpu_debug_enable(void){
    static int done; if(done || !boot_gpu_debug_level())return; done=1;
    ID3D12Debug *d=NULL;
    if(SUCCEEDED(D3D12GetDebugInterface(&IID_ID3D12Debug,(void**)&d)) && d){
        ID3D12Debug_EnableDebugLayer(d);
        if(boot_gpu_debug_level()>=2){
            ID3D12Debug1 *d1=NULL;
            if(SUCCEEDED(ID3D12Debug_QueryInterface(d,&IID_ID3D12Debug1,(void**)&d1)) && d1){ID3D12Debug1_SetEnableGPUBasedValidation(d1,TRUE);ID3D12Debug1_Release(d1);}
        }
        ID3D12Debug_Release(d);
        fprintf(stderr,"[gpu] D3D12 debug layer enabled%s\n",boot_gpu_debug_level()>=2?" with GPU-based validation":"");
    }else fprintf(stderr,"[gpu] D3D12 debug layer unavailable (install the Graphics Tools optional feature)\n");
}
static inline void boot_gpu_debug_attach(ID3D12Device *device){
    if(!boot_gpu_debug_level() || !device)return;
    if(boot_gpu_info_queue){ID3D12InfoQueue_Release(boot_gpu_info_queue);boot_gpu_info_queue=NULL;}
    if(FAILED(ID3D12Device_QueryInterface(device,&IID_ID3D12InfoQueue,(void**)&boot_gpu_info_queue)))boot_gpu_info_queue=NULL;
    if(boot_gpu_info_queue)ID3D12InfoQueue_SetMessageCountLimit(boot_gpu_info_queue,4096);
}
/* Stored validation messages to f (each message ID at most 4 times per run, 2000 lines in all), then cleared. */
static inline void boot_gpu_debug_drain(FILE *f){
    ID3D12InfoQueue *q=boot_gpu_info_queue; if(!q || !f)return;
    static uint16_t seen[4096]; static unsigned lines;
    UINT64 n=ID3D12InfoQueue_GetNumStoredMessages(q);
    for(UINT64 i=0;i<n && lines<2000;++i){
        SIZE_T len=0; if(FAILED(ID3D12InfoQueue_GetMessage(q,i,NULL,&len)) || !len)continue;
        D3D12_MESSAGE *m=(D3D12_MESSAGE*)malloc(len); if(!m)break;
        if(SUCCEEDED(ID3D12InfoQueue_GetMessage(q,i,m,&len))){
            unsigned id=(unsigned)m->ID;
            if(id>=4096 || seen[id]<4){ if(id<4096)++seen[id]; ++lines;
                fprintf(f,"d3d12 %s #%u: %.*s\n",m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR?"ERROR":m->Severity==D3D12_MESSAGE_SEVERITY_WARNING?"warning":"info",
                        id,(int)(m->DescriptionByteLength?m->DescriptionByteLength-1:0),m->pDescription); }
        }
        free(m);
    }
    ID3D12InfoQueue_ClearStoredMessages(q);
}
/* Recovery candidates must not replace the old global validation queue before
 * identity/capability checks. Ordinary open retains its original attach point. */
static inline BootGpu *boot_gpu_open_internal(int attach_debug){
    boot_gpu_dred_enable();
    boot_gpu_debug_enable();   /* render-35 */
    IDXGIFactory1 *factory=NULL;
    if(FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1,(void**)&factory)))return NULL;
    BootGpu *g=calloc(1,sizeof(*g));
    if(!g){IDXGIFactory1_Release(factory);return NULL;}
    for(UINT i=0;;++i){
        IDXGIAdapter1 *adapter=NULL;
        HRESULT hr=IDXGIFactory1_EnumAdapters1(factory,i,&adapter);
        if(hr==DXGI_ERROR_NOT_FOUND)break;
        if(FAILED(hr))break;
        DXGI_ADAPTER_DESC1 desc;
        if(SUCCEEDED(IDXGIAdapter1_GetDesc1(adapter,&desc)) && !(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE) &&
           (!g->device || desc.DedicatedVideoMemory>g->adapter.DedicatedVideoMemory)){
            ID3D12Device *device=NULL;
            if(SUCCEEDED(D3D12CreateDevice((IUnknown*)adapter,D3D_FEATURE_LEVEL_11_0,&IID_ID3D12Device,(void**)&device))){
                if(g->device)ID3D12Device_Release(g->device);
                g->device=device;g->adapter=desc;
            }
        }
        IDXGIAdapter1_Release(adapter);
    }
    IDXGIFactory1_Release(factory);
    if(!g->device)goto fail;
    if(attach_debug)boot_gpu_debug_attach(g->device);   /* render-35 */
    D3D12_COMMAND_QUEUE_DESC desc={0};desc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    if(FAILED(ID3D12Device_CreateCommandQueue(g->device,&desc,&IID_ID3D12CommandQueue,(void**)&g->queue)))goto fail;
    if(FAILED(ID3D12Device_CreateFence(g->device,0,D3D12_FENCE_FLAG_NONE,&IID_ID3D12Fence,(void**)&g->fence)))goto fail;
    g->event=CreateEventW(NULL,FALSE,FALSE,NULL);if(!g->event)goto fail;
    return g;
fail:boot_gpu_close(g);return NULL;
}

static inline BootGpu *boot_gpu_open(void){return boot_gpu_open_internal(1);}

/* Synchronous queue completion. Caller retains submitted resources on failure
 * until device teardown; a timeout must never be mistaken for completion. */
static inline int boot_gpu_wait(BootGpu *g){
    if(!g || g->submitted==UINT64_MAX) { BootRenderFailure98 x={.stage=BOOT_RF98_GPU_WAIT,.detail=1,.gpu=(uintptr_t)g};boot_rf98_note(&x);return boot_nw_result(BOOT_NW_GPU_RETURN,0,1); }
    uint64_t target=++g->submitted;
    boot_nw_note(BOOT_NW_GPU_TARGET,BOOT_NW_A|BOOT_NW_B,target,(uint64_t)(uintptr_t)g,0,0);
    HRESULT rf98_signal;
    if(FAILED(boot_nw_hresult(BOOT_NW_GPU_SIGNAL,rf98_signal=ID3D12CommandQueue_Signal(g->queue,g->fence,target)))) { BootRenderFailure98 x={BOOT_RF98_GPU(g),.stage=BOOT_RF98_GPU_WAIT,.detail=2,.known=BOOT_RF98_HR|BOOT_RF98_REQUEST,.hr=(uint32_t)rf98_signal,.target=target};boot_rf98_note(&x);return boot_nw_result(BOOT_NW_GPU_RETURN,0,2); }
    uint64_t completed=boot_nw_fence_completed(g->fence,BOOT_NW_GPU_FIRST,(uint64_t)(uintptr_t)g,target,1);
    if(completed==UINT64_MAX) { BootRenderFailure98 x={BOOT_RF98_GPU(g),.stage=BOOT_RF98_GPU_WAIT,.detail=3,.known=BOOT_RF98_FIRST|BOOT_RF98_REQUEST,.target=target,.first=completed};boot_rf98_note(&x);return boot_nw_result(BOOT_NW_GPU_RETURN,0,3); }
    uint64_t rf98_first=completed;unsigned rf98_known=BOOT_RF98_FIRST|BOOT_RF98_REQUEST;
    if(completed<target){
        HRESULT rf98_event;
        if(FAILED(boot_nw_hresult(BOOT_NW_REGISTER,rf98_event=ID3D12Fence_SetEventOnCompletion(g->fence,target,g->event)))) { BootRenderFailure98 x={BOOT_RF98_GPU(g),.stage=BOOT_RF98_GPU_WAIT,.detail=4,.known=rf98_known|BOOT_RF98_HR,.target=target,.first=rf98_first,.hr=(uint32_t)rf98_event};boot_rf98_note(&x);return boot_nw_result(BOOT_NW_GPU_RETURN,0,4); }
        boot_nw_note(BOOT_NW_GPU_WAIT_BEGIN,BOOT_NW_A|BOOT_NW_B,target,30000,0,0);
        DWORD rf98_wait=WaitForSingleObject(g->event,30000),rf98_error=0;
        if(rf98_wait==WAIT_FAILED)rf98_error=GetLastError();
        if(boot_nw_result(BOOT_NW_GPU_WAIT_END,(int)rf98_wait,0)!=WAIT_OBJECT_0) { BootRenderFailure98 x={BOOT_RF98_GPU(g),.stage=BOOT_RF98_GPU_WAIT,.detail=5,.known=rf98_known|BOOT_RF98_WAIT|(rf98_wait==WAIT_FAILED?BOOT_RF98_LAST_ERROR:0),.target=target,.first=rf98_first,.wait=rf98_wait,.last_error=rf98_error};boot_rf98_note(&x);return boot_nw_result(BOOT_NW_GPU_RETURN,0,5); }
        completed=boot_nw_fence_completed(g->fence,BOOT_NW_GPU_FINAL,(uint64_t)(uintptr_t)g,target,1);rf98_known|=BOOT_RF98_FINAL;
    }
    HRESULT rf98_reason=0;unsigned rf98_reason_called=0;
    int rf98_ok=completed!=UINT64_MAX && completed>=target && SUCCEEDED(boot_nw_hresult(BOOT_NW_GPU_REASON,(rf98_reason_called=1,rf98_reason=ID3D12Device_GetDeviceRemovedReason(g->device))));
    if(!rf98_ok) { BootRenderFailure98 x={BOOT_RF98_GPU(g),.stage=BOOT_RF98_GPU_WAIT,.detail=6,.known=rf98_known|(rf98_reason_called?BOOT_RF98_HR:0),.target=target,.first=rf98_first,.final=completed,.hr=(uint32_t)rf98_reason};boot_rf98_note(&x); }
    return boot_nw_result(BOOT_NW_GPU_RETURN,rf98_ok,0);
}
#endif
