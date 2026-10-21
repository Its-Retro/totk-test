#ifndef TOTK_HOST_LIGHT_COPY_PROBE_H
#define TOTK_HOST_LIGHT_COPY_PROBE_H
/* Diagnostic84: disabled by default. Fixed READBACK ownership is independent of
 * texture references; only a successful normal Signal can publish a Map lease. */
#include "host_light_copy_probe_core.h"
static struct {
    BootLight84Config config; int requested;
    volatile LONG enabled,ready,stop,window_end,used,match_used,producer;
    _Alignas(8) volatile LONG64 generation,execute,loss,skipped,match_lost;
    uint64_t renderer,cl,device,start_qpc,frequency,queue,allocator; unsigned segment;
    ID3D12Resource *rb; ID3D12Fence *fence;
    HANDLE thread,data,meta;
    char receipt_path[MAX_PATH];
    BootLight84Batch batch[BOOT_L84_BATCHES]; BootLight84Match match[BOOT_L84_MATCHES];
    uint8_t cpu[BOOT_L84_BATCHES][BOOT_L84_ROWS*BOOT_L84_ROW_BYTES];
} boot_light84;
static inline int boot_light84_on(void) {
    return boot_light84.requested && InterlockedCompareExchange(&boot_light84.enabled,0,0);
}
static inline int boot_light84_enter(void) {
    if(!boot_light84_on()) return 0;
    if(InterlockedCompareExchange(&boot_light84.producer,1,0)) { InterlockedIncrement64(&boot_light84.loss); return 0; }
    if(!boot_light84_on()) { InterlockedExchange(&boot_light84.producer,0); return 0; }
    return 1;
}
static inline void boot_light84_leave(void) { InterlockedExchange(&boot_light84.producer,0); }
static inline int boot_light84_write(HANDLE f,const void *bytes,DWORD n) {
    DWORD wrote=0; return WriteFile(f,bytes,n,&wrote,NULL) && wrote==n;
}
static inline void boot_light84_abort_pending(unsigned why) {
    for(unsigned i=0;i<(unsigned)InterlockedCompareExchange(&boot_light84.used,0,0);++i) {
        BootLight84Batch *b=&boot_light84.batch[i]; LONG s=InterlockedCompareExchange(&b->state,0,0);
        if(s!=BOOT_L84_UNUSED && s!=BOOT_L84_DONE && s!=BOOT_L84_ABORTED && s!=BOOT_L84_EXPORTING && s!=BOOT_L84_PERSISTED) {
            b->abort_reason=why; InterlockedExchange(&b->state,BOOT_L84_ABORTED);
        }
    }
}
static inline void boot_light84_error(unsigned why) {
    if(!boot_light84.requested) return;
    InterlockedExchange(&boot_light84.enabled,0); InterlockedExchange(&boot_light84.stop,(LONG)why);
    /* Worker waits for the nonblocking producer lease before freezing pending
     * metadata. No callback waits for the writer or an added GPU fence. */
}
static inline void boot_light84_device_removed(void) {
    if(!boot_light84.requested) return;
    InterlockedExchange(&boot_light84.enabled,0); InterlockedExchange(&boot_light84.stop,BOOT_L84_ABORT_DEVICE);
    /* Confirmed removal aborts GPU work; do not Map or pin the old device into
     * recovery. Command lists do not AddRef these resources for us. */
    if(boot_light84.rb) ID3D12Resource_Release(boot_light84.rb);
    if(boot_light84.fence) ID3D12Fence_Release(boot_light84.fence);
    boot_light84.rb=NULL; boot_light84.fence=NULL; InterlockedExchange(&boot_light84.ready,0);
}
static DWORD WINAPI boot_light84_worker(void *unused) {
    (void)unused; unsigned emitted[BOOT_L84_BATCHES]={0},batches=0,aborted=0,io_error=0; uint64_t bytes=0;
    char line[4096]; unsigned quiesced=0,stop_waits=0;
    for(;;) {
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        if((uint64_t)now.QuadPart<boot_light84.start_qpc || (uint64_t)now.QuadPart-boot_light84.start_qpc>=240*boot_light84.frequency)
            boot_light84_error(BOOT_L84_ABORT_TIMEOUT);
        if(InterlockedCompareExchange(&boot_light84.stop,0,0) && !quiesced) {
            if(InterlockedCompareExchange(&boot_light84.producer,0,0)) {
                if(++stop_waits>=100) { io_error=2; break; }
                Sleep(10); continue;
            }
            boot_light84_abort_pending((unsigned)InterlockedCompareExchange(&boot_light84.stop,0,0)); quiesced=1;
        }
        unsigned used=(unsigned)InterlockedCompareExchange(&boot_light84.used,0,0),pending=0;
        for(unsigned i=0;i<used;++i) {
            BootLight84Batch *b=&boot_light84.batch[i]; if(emitted[i]) continue;
            BootLight84Batch copy; int have=0;
            LONG state=InterlockedCompareExchange(&b->state,0,0);
            if(state==BOOT_L84_DONE && InterlockedCompareExchange(&b->state,BOOT_L84_EXPORTING,BOOT_L84_DONE)==BOOT_L84_DONE) {
                have=1;
            }
            state=InterlockedCompareExchange(&b->state,0,0);
            if(have || state==BOOT_L84_ABORTED) memcpy(&copy,b,sizeof copy);
            if(!have && state!=BOOT_L84_ABORTED) { ++pending; continue; }
            int n=snprintf(line,sizeof line,
                "{\"kind\":\"batch\",\"slot\":%u,\"sub\":%llu,\"event\":%u,\"tic\":%u,\"recording\":%llu,\"execute\":%llu,\"fence\":%llu,\"completed\":%llu,\"fence_object\":\"%llx\",\"queue\":\"%llx\",\"allocator\":\"%llx\",\"segment\":%u,\"renderer\":\"%llx\",\"device\":\"%llx\",\"cl\":\"%llx\",\"array\":\"%llx\",\"address\":\"%llx\",\"source0\":\"%llx\",\"address0\":\"%llx\",\"sequence0\":%llu,\"index0\":%llu,\"generation0\":%llu,\"source1\":\"%llx\",\"address1\":\"%llx\",\"sequence1\":%llu,\"index1\":%llu,\"generation1\":%llu,\"qpc\":%llu,\"assembly_reason\":%u,\"same_resolve_event_draw\":%ld,\"sub_end_observed\":%ld,\"row_mask\":%u,\"aborted\":%u,\"abort_reason\":%u,\"offset\":%llu,\"valid_bytes\":%u}\n",
                i,(unsigned long long)copy.sub,copy.event,copy.tic,(unsigned long long)copy.recording,(unsigned long long)copy.execute,(unsigned long long)copy.fence,
                (unsigned long long)copy.completed,(unsigned long long)copy.fence_object,(unsigned long long)copy.queue,(unsigned long long)copy.allocator,copy.segment,
                (unsigned long long)copy.renderer,(unsigned long long)copy.device,(unsigned long long)copy.cl,(unsigned long long)copy.array,(unsigned long long)copy.address,
                (unsigned long long)copy.source[0].resource,(unsigned long long)copy.source[0].address,(unsigned long long)copy.source[0].sequence,(unsigned long long)copy.source[0].index,(unsigned long long)copy.source[0].generation,
                (unsigned long long)copy.source[1].resource,(unsigned long long)copy.source[1].address,(unsigned long long)copy.source[1].sequence,(unsigned long long)copy.source[1].index,(unsigned long long)copy.source[1].generation,
                (unsigned long long)copy.qpc,copy.why,(long)copy.draw,(long)copy.sub_done,copy.row_mask,!have,copy.abort_reason,(unsigned long long)bytes,have?(unsigned)sizeof boot_light84.cpu[i]:0u);
            if(n<=0 || (size_t)n>=sizeof line || (have && !boot_light84_write(boot_light84.data,boot_light84.cpu[i],sizeof boot_light84.cpu[i])) || !boot_light84_write(boot_light84.meta,line,(DWORD)n)) { io_error=1; break; }
            if(have) { bytes+=sizeof boot_light84.cpu[i]; ++batches; InterlockedExchange(&b->state,BOOT_L84_PERSISTED); } else ++aborted;
            emitted[i]=1;
        }
        if(io_error) break;
        if(!pending && (InterlockedCompareExchange(&boot_light84.stop,0,0) || InterlockedCompareExchange(&boot_light84.window_end,0,0))) {
            InterlockedExchange(&boot_light84.enabled,0);
            if(InterlockedCompareExchange(&boot_light84.producer,0,0)) { Sleep(10); continue; }
            quiesced=1; break;
        }
        Sleep(10);
    }
    InterlockedExchange(&boot_light84.enabled,0);
    /* If a paused producer did not quiesce, fail closed without reading its
     * mutable match slots. It cannot be mistaken for accepted pixel coverage. */
    unsigned matches=quiesced?(unsigned)InterlockedCompareExchange(&boot_light84.match_used,0,0):0;
    for(unsigned i=0;!io_error && i<matches;++i) {
        const BootLight84Match *m=&boot_light84.match[i];
        int n=snprintf(line,sizeof line,"{\"kind\":\"match\",\"sub\":%llu,\"event\":%u,\"tic\":%u,\"array\":\"%llx\",\"address\":\"%llx\",\"selected\":%u}\n",(unsigned long long)m->sub,m->event,m->tic,(unsigned long long)m->array,(unsigned long long)m->address,m->selected);
        if(n<=0 || (size_t)n>=sizeof line || !boot_light84_write(boot_light84.meta,line,(DWORD)n)) io_error=1;
    }
    LONG64 skipped=InterlockedCompareExchange64(&boot_light84.skipped,0,0),loss=InterlockedCompareExchange64(&boot_light84.loss,0,0),match_lost=InterlockedCompareExchange64(&boot_light84.match_lost,0,0);
    LONG stop_reason=InterlockedCompareExchange(&boot_light84.stop,0,0);
    int n=snprintf(line,sizeof line,"{\"kind\":\"end\",\"batches\":%u,\"aborted\":%u,\"bytes\":%llu,\"matches\":%u,\"skipped\":%lld,\"loss\":%lld,\"match_lost\":%lld,\"stop_reason\":%ld,\"capture_complete_proven\":false}\n",batches,aborted,(unsigned long long)bytes,matches,(long long)skipped,(long long)loss,(long long)match_lost,(long)stop_reason);
    if(n<=0 || (size_t)n>=sizeof line || !boot_light84_write(boot_light84.meta,line,(DWORD)n)) io_error=1;
    if(!FlushFileBuffers(boot_light84.data)) io_error=1; if(!FlushFileBuffers(boot_light84.meta)) io_error=1;
    if(!CloseHandle(boot_light84.data)) io_error=1; if(!CloseHandle(boot_light84.meta)) io_error=1;
    char temp[MAX_PATH]; snprintf(temp,sizeof temp,"%s.tmp",boot_light84.receipt_path);
    HANDLE receipt=CreateFileA(temp,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
    n=snprintf(line,sizeof line,"{\"version\":1,\"pid\":%lu,\"start_qpc\":%llu,\"batches\":%u,\"aborted\":%u,\"bytes\":%llu,\"matches\":%u,\"skipped\":%lld,\"loss\":%lld,\"match_lost\":%lld,\"stop_reason\":%ld,\"io_error\":%u,\"producer_quiesced\":%s,\"capture_complete_proven\":false}\n",(unsigned long)GetCurrentProcessId(),(unsigned long long)boot_light84.start_qpc,batches,aborted,(unsigned long long)bytes,matches,(long long)skipped,(long long)loss,(long long)match_lost,(long)stop_reason,io_error,quiesced?"true":"false");
    int receipt_ok=receipt!=INVALID_HANDLE_VALUE && n>0 && (size_t)n<sizeof line;
    if(receipt!=INVALID_HANDLE_VALUE) { if(receipt_ok && !boot_light84_write(receipt,line,(DWORD)n)) receipt_ok=0; if(!FlushFileBuffers(receipt)) receipt_ok=0; if(!CloseHandle(receipt)) receipt_ok=0; }
    if(!receipt_ok || !MoveFileExA(temp,boot_light84.receipt_path,MOVEFILE_WRITE_THROUGH)) fprintf(stderr,"light-copy84: receipt publish failed\n");
    return 0;
}
static BOOL CALLBACK boot_light84_initialize(PINIT_ONCE once,PVOID arg,PVOID *result) {
    (void)once; (void)arg; (void)result;
    char text[160]; DWORD n=GetEnvironmentVariableA("TOTK_LIGHT_COPY_PROBE",text,sizeof text);
    if(!n) return TRUE; boot_light84.requested=1;
    if(n>=sizeof text || boot_light84_parse(text,&boot_light84.config)!=1) { fprintf(stderr,"light-copy84: invalid configuration; disabled\n"); return TRUE; }
    LARGE_INTEGER q,f; QueryPerformanceCounter(&q); QueryPerformanceFrequency(&f);
    if(q.QuadPart<=0 || f.QuadPart<=0) return TRUE;
    boot_light84.start_qpc=(uint64_t)q.QuadPart; boot_light84.frequency=(uint64_t)f.QuadPart;
    char data[MAX_PATH],meta[MAX_PATH];
    snprintf(data,sizeof data,"frames/light-copy84-%lu-%llu.bin",(unsigned long)GetCurrentProcessId(),(unsigned long long)q.QuadPart);
    snprintf(meta,sizeof meta,"frames/light-copy84-%lu-%llu.jsonl",(unsigned long)GetCurrentProcessId(),(unsigned long long)q.QuadPart);
    snprintf(boot_light84.receipt_path,sizeof boot_light84.receipt_path,"frames/light-copy84-%lu-%llu.receipt.json",(unsigned long)GetCurrentProcessId(),(unsigned long long)q.QuadPart);
    boot_light84.data=CreateFileA(data,GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
    boot_light84.meta=CreateFileA(meta,GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
    if(boot_light84.data==INVALID_HANDLE_VALUE || boot_light84.meta==INVALID_HANDLE_VALUE) { if(boot_light84.data!=INVALID_HANDLE_VALUE) CloseHandle(boot_light84.data); if(boot_light84.meta!=INVALID_HANDLE_VALUE) CloseHandle(boot_light84.meta); fprintf(stderr,"light-copy84: fresh output creation failed\n"); return TRUE; }
    char line[1024]; int size=snprintf(line,sizeof line,"{\"kind\":\"header\",\"version\":1,\"pid\":%lu,\"start_qpc\":%llu,\"qpc_frequency\":%llu,\"first\":%llu,\"last\":%llu,\"base\":\"%llx\",\"span\":\"%llx\",\"width\":640,\"height\":360,\"layers\":2,\"levels\":1,\"dxgi\":26,\"strip_width\":64,\"coordinates\":[[64,64],[288,180],[512,300]],\"row_bytes\":256,\"placement_stride\":512,\"binary_order\":\"batch,phase(source/array),layer,strip\",\"capture_complete_proven\":false}\n",(unsigned long)GetCurrentProcessId(),(unsigned long long)q.QuadPart,(unsigned long long)f.QuadPart,(unsigned long long)boot_light84.config.first,(unsigned long long)boot_light84.config.last,(unsigned long long)boot_light84.config.base,(unsigned long long)boot_light84.config.bytes);
    if(size<=0 || (size_t)size>=sizeof line || !boot_light84_write(boot_light84.meta,line,(DWORD)size)) { CloseHandle(boot_light84.data); CloseHandle(boot_light84.meta); return TRUE; }
    InterlockedExchange(&boot_light84.enabled,1);
    boot_light84.thread=CreateThread(NULL,0,boot_light84_worker,NULL,0,NULL);
    if(!boot_light84.thread) { InterlockedExchange(&boot_light84.enabled,0); CloseHandle(boot_light84.data); CloseHandle(boot_light84.meta); }
    else CloseHandle(boot_light84.thread); /* worker owns output; no callback joins it */
    return TRUE;
}
static inline void boot_light84_init(void) {
    static INIT_ONCE once=INIT_ONCE_STATIC_INIT; InitOnceExecuteOnce(&once,boot_light84_initialize,NULL,NULL);
}
static inline void boot_light84_prepare(const void *renderer,ID3D12Device *device,ID3D12Fence *fence,const void *queue,const void *cl) {
    if(!boot_light84_enter()) return;
    if(InterlockedCompareExchange(&boot_light84.ready,0,0)) { if(boot_light84.renderer!=(uintptr_t)renderer || boot_light84.device!=(uintptr_t)device) boot_light84_error(BOOT_L84_ABORT_GENERATION); boot_light84_leave(); return; }
    D3D12_HEAP_PROPERTIES hp={D3D12_HEAP_TYPE_READBACK,D3D12_CPU_PAGE_PROPERTY_UNKNOWN,D3D12_MEMORY_POOL_UNKNOWN,0,0};
    D3D12_RESOURCE_DESC d={0}; d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; d.Width=BOOT_L84_BYTES; d.Height=1; d.DepthOrArraySize=1; d.MipLevels=1; d.SampleDesc.Count=1; d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *rb=NULL;
    if(FAILED(ID3D12Device_CreateCommittedResource(device,&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,NULL,&IID_ID3D12Resource,(void **)&rb))) { boot_light84_error(BOOT_L84_ABORT_MAP); boot_light84_leave(); return; }
    if(!boot_light84_on()) { ID3D12Resource_Release(rb); boot_light84_leave(); return; }
    boot_light84.renderer=(uintptr_t)renderer; boot_light84.device=(uintptr_t)device; boot_light84.cl=(uintptr_t)cl; boot_light84.queue=(uintptr_t)queue; boot_light84.rb=rb; boot_light84.fence=fence; ID3D12Fence_AddRef(fence);
    InterlockedExchange(&boot_light84.ready,1); boot_light84_leave();
}
static inline void boot_light84_reset(const void *renderer,const void *cl,const void *allocator,unsigned segment) {
    if(!boot_light84_enter()) { if(boot_light84_on()) boot_light84_error(BOOT_L84_ABORT_GENERATION); return; }
    if(boot_light84.renderer!=(uintptr_t)renderer || boot_light84.cl!=(uintptr_t)cl) boot_light84_error(BOOT_L84_ABORT_GENERATION);
    else {
        for(unsigned i=0;i<(unsigned)InterlockedCompareExchange(&boot_light84.used,0,0);++i) {
            BootLight84Batch *b=&boot_light84.batch[i]; LONG state=InterlockedCompareExchange(&b->state,0,0);
            if(state==BOOT_L84_RESERVED || state==BOOT_L84_RECORDED) { b->abort_reason=BOOT_L84_ABORT_RESET; InterlockedExchange(&b->state,BOOT_L84_ABORTED); }
        }
        boot_light84.allocator=(uintptr_t)allocator; boot_light84.segment=segment; InterlockedIncrement64(&boot_light84.generation);
    }
    boot_light84_leave();
}
static inline void boot_light84_fence(const void *renderer,const void *cl,ID3D12Fence *fence,uint64_t value) {
    if(!boot_light84_enter()) { if(boot_light84_on()) boot_light84_error(BOOT_L84_ABORT_GENERATION); return; }
    if(fence!=boot_light84.fence) { boot_light84_error(BOOT_L84_ABORT_GENERATION); boot_light84_leave(); return; }
    uint64_t exec=(uint64_t)InterlockedIncrement64(&boot_light84.execute),gen=(uint64_t)InterlockedCompareExchange64(&boot_light84.generation,0,0);
    for(unsigned i=0;i<(unsigned)InterlockedCompareExchange(&boot_light84.used,0,0);++i) boot_light84_attach(&boot_light84.batch[i],(uintptr_t)renderer,(uintptr_t)cl,gen,exec,value);
    boot_light84_leave();
}
/* Called only with an already-observed normal completion value. The render
 * owner performs Map/raw-copy; the writer never touches a D3D object. */
static inline void boot_light84_retire(ID3D12Fence *fence,uint64_t completed) {
    if(!boot_light84_enter()) return;
    if(!boot_light84.rb || fence!=boot_light84.fence) { boot_light84_leave(); return; }
    if(completed==UINT64_MAX) { boot_light84_device_removed(); boot_light84_leave(); return; }
    unsigned used=(unsigned)InterlockedCompareExchange(&boot_light84.used,0,0);
    for(unsigned i=0;i<used;++i) {
        BootLight84Batch *b=&boot_light84.batch[i];
        if(!boot_light84_claim(b,completed)) continue;
        b->completed=completed;
        uint8_t *map=NULL; D3D12_RANGE rr={(SIZE_T)i*BOOT_L84_BATCH_BYTES,(SIZE_T)(i+1)*BOOT_L84_BATCH_BYTES};
        if(FAILED(ID3D12Resource_Map(boot_light84.rb,0,&rr,(void **)&map)) || !map) {
            b->abort_reason=BOOT_L84_ABORT_MAP; InterlockedExchange(&b->state,BOOT_L84_ABORTED); boot_light84_error(BOOT_L84_ABORT_MAP); break;
        }
        for(unsigned p=0;p<2;++p) for(unsigned l=0;l<2;++l) for(unsigned s=0;s<BOOT_L84_STRIPS;++s)
            memcpy(boot_light84.cpu[i]+(p*6+l*3+s)*BOOT_L84_ROW_BYTES,map+boot_light84_offset(i,p,l,s),BOOT_L84_ROW_BYTES);
        D3D12_RANGE none={0,0}; ID3D12Resource_Unmap(boot_light84.rb,0,&none);
        InterlockedExchange(&b->state,BOOT_L84_DONE);
    }
    boot_light84_leave();
}
static inline BootLight84Batch *boot_light84_begin(const void *renderer,const void *cl,uint64_t sub,unsigned event,unsigned tic,uint64_t address,
                                                 ID3D12Resource *array,const D3D12_RESOURCE_DESC *d,const BootLight84Source source[2],unsigned why) {
    if(!boot_light84_on() || !InterlockedCompareExchange(&boot_light84.ready,0,0) || !boot_light84_in_window(&boot_light84.config,sub,address) ||
       d->Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || d->Width!=640 || !boot_light84_shape((unsigned)d->Width,d->Height,d->DepthOrArraySize,d->MipLevels,d->SampleDesc.Count,d->Format,(uintptr_t)array,source[0].resource,source[1].resource)) return NULL;
    if(!boot_light84_enter()) return NULL;
    if(!boot_light84.rb || boot_light84.renderer!=(uintptr_t)renderer || boot_light84.cl!=(uintptr_t)cl || !InterlockedCompareExchange64(&boot_light84.generation,0,0)) { boot_light84_error(BOOT_L84_ABORT_GENERATION); boot_light84_leave(); return NULL; }
    unsigned selected=1,used=(unsigned)InterlockedCompareExchange(&boot_light84.used,0,0);
    for(unsigned i=0;i<used;++i) if(boot_light84.batch[i].sub==sub) selected=0;
    if(used>=BOOT_L84_BATCHES) selected=0;
    unsigned mi=(unsigned)InterlockedCompareExchange(&boot_light84.match_used,0,0);
    if(mi<BOOT_L84_MATCHES) { BootLight84Match *m=&boot_light84.match[mi]; m->sub=sub; m->event=event; m->tic=tic; m->array=(uintptr_t)array; m->address=address; m->selected=selected; InterlockedExchange(&boot_light84.match_used,(LONG)(mi+1)); }
    else InterlockedIncrement64(&boot_light84.match_lost);
    if(!selected) { InterlockedIncrement64(&boot_light84.skipped); boot_light84_leave(); return NULL; }
    BootLight84Batch *b=&boot_light84.batch[used]; memset(b,0,sizeof *b); b->slot=used; b->sub=sub; b->event=event; b->tic=tic; b->why=why;
    b->recording=(uint64_t)InterlockedCompareExchange64(&boot_light84.generation,0,0); b->renderer=(uintptr_t)renderer; b->cl=(uintptr_t)cl; b->device=boot_light84.device; b->array=(uintptr_t)array; b->address=address;
    b->queue=boot_light84.queue; b->allocator=boot_light84.allocator; b->segment=boot_light84.segment; b->fence_object=(uintptr_t)boot_light84.fence;
    memcpy(b->source,source,sizeof b->source); LARGE_INTEGER q; QueryPerformanceCounter(&q); b->qpc=(uint64_t)q.QuadPart;
    InterlockedExchange(&b->state,BOOT_L84_RESERVED); InterlockedExchange(&boot_light84.used,(LONG)(used+1)); return b; /* producer lease until end */
}
static inline void boot_light84_copy(BootLight84Batch *b,ID3D12GraphicsCommandList *cl,unsigned phase,unsigned layer,ID3D12Resource *source,unsigned subresource) {
    if(!b) return;
    for(unsigned s=0;s<BOOT_L84_STRIPS;++s) {
        D3D12_TEXTURE_COPY_LOCATION dl={boot_light84.rb,D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,{0}},sl={source,D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,{0}};
        dl.PlacedFootprint.Offset=boot_light84_offset(b->slot,phase,layer,s); dl.PlacedFootprint.Footprint.Format=DXGI_FORMAT_R11G11B10_FLOAT;
        dl.PlacedFootprint.Footprint.Width=64; dl.PlacedFootprint.Footprint.Height=1; dl.PlacedFootprint.Footprint.Depth=1; dl.PlacedFootprint.Footprint.RowPitch=256; sl.SubresourceIndex=subresource;
        unsigned x=boot_light84_x(s),y=boot_light84_y(s); D3D12_BOX box={x,y,0,x+64,y+1,1};
        ID3D12GraphicsCommandList_CopyTextureRegion(cl,&dl,0,0,0,&sl,&box); b->row_mask|=1u<<(phase*6+layer*3+s);
    }
}
static inline void boot_light84_end(BootLight84Batch *b,int success) {
    if(!b) return;
    if(success && b->row_mask==4095) InterlockedExchange(&b->state,BOOT_L84_RECORDED);
    else { b->abort_reason=BOOT_L84_ABORT_PARTIAL; InterlockedExchange(&b->state,BOOT_L84_ABORTED); }
    boot_light84_leave();
}
static inline void boot_light84_draw(uint64_t sub,unsigned event) {
    if(!boot_light84_enter()) return;
    for(unsigned i=0;i<(unsigned)InterlockedCompareExchange(&boot_light84.used,0,0);++i) {
        BootLight84Batch *b=&boot_light84.batch[i]; LONG state=InterlockedCompareExchange(&b->state,0,0);
        if((state==BOOT_L84_RECORDED || state==BOOT_L84_FENCED) && !InterlockedCompareExchange(&b->sub_done,0,0) && b->sub==sub && b->event==event) InterlockedExchange(&b->draw,1);
    }
    boot_light84_leave();
}
static inline void boot_light84_sub_end(uint64_t sub) {
    if(!boot_light84_enter()) return;
    for(unsigned i=0;i<(unsigned)InterlockedCompareExchange(&boot_light84.used,0,0);++i) {
        BootLight84Batch *b=&boot_light84.batch[i]; LONG state=InterlockedCompareExchange(&b->state,0,0);
        if((state==BOOT_L84_RECORDED || state==BOOT_L84_FENCED) && b->sub==sub) InterlockedExchange(&b->sub_done,1);
    }
    if(sub>=boot_light84.config.last) InterlockedExchange(&boot_light84.window_end,1);
    boot_light84_leave();
}
#endif
