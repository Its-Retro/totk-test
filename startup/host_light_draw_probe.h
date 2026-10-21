#ifndef TOTK_HOST_LIGHT_DRAW_PROBE_H
#define TOTK_HOST_LIGHT_DRAW_PROBE_H
/* Diagnostic86: disabled by default. Fixed READBACK ownership is independent of
 * texture references; only a successful normal Signal can publish a Map lease. */
#include "host_light_draw_probe_core.h"
static struct {
    BootLight86Config config; int requested;
    volatile LONG enabled,ready,stop,window_end,used,match_used,producer;
    _Alignas(8) volatile LONG64 generation,execute,loss,skipped,match_lost;
    uint64_t renderer,cl,device,start_qpc,frequency,queue,allocator,creation_filetime; unsigned segment;
    ID3D12Resource *rb; ID3D12Fence *fence;
    HANDLE thread,data,meta;
    char receipt_path[MAX_PATH];
    BootLight86Batch batch[BOOT_L86_BATCHES]; BootLight86Match match[BOOT_L86_MATCHES];
    uint8_t cpu[BOOT_L86_BATCHES][BOOT_L86_ROWS*BOOT_L86_ROW_BYTES];
} boot_light86;
static inline int boot_light86_on(void) {
    return boot_light86.requested && InterlockedCompareExchange(&boot_light86.enabled,0,0);
}
static inline int boot_light86_enter(void) {
    if(!boot_light86_on()) return 0;
    if(InterlockedCompareExchange(&boot_light86.producer,1,0)) { InterlockedIncrement64(&boot_light86.loss); return 0; }
    if(!boot_light86_on()) { InterlockedExchange(&boot_light86.producer,0); return 0; }
    return 1;
}
static inline void boot_light86_leave(void) { InterlockedExchange(&boot_light86.producer,0); }
static inline int boot_light86_write(HANDLE f,const void *bytes,DWORD n) {
    DWORD wrote=0; return WriteFile(f,bytes,n,&wrote,NULL) && wrote==n;
}
static inline void boot_light86_abort_pending(unsigned why) {
    for(unsigned i=0;i<(unsigned)InterlockedCompareExchange(&boot_light86.used,0,0);++i) {
        BootLight86Batch *b=&boot_light86.batch[i]; LONG s=InterlockedCompareExchange(&b->state,0,0);
        if(s!=BOOT_L86_UNUSED && s!=BOOT_L86_DONE && s!=BOOT_L86_ABORTED && s!=BOOT_L86_EXPORTING && s!=BOOT_L86_PERSISTED) {
            b->abort_reason=why; InterlockedExchange(&b->state,BOOT_L86_ABORTED);
        }
    }
}
static inline void boot_light86_error(unsigned why) {
    if(!boot_light86.requested) return;
    InterlockedExchange(&boot_light86.enabled,0); InterlockedExchange(&boot_light86.stop,(LONG)why);
    /* Worker waits for the nonblocking producer lease before freezing pending
     * metadata. No callback waits for the writer or an added GPU fence. */
}
static inline void boot_light86_device_removed(void) {
    if(!boot_light86.requested) return;
    InterlockedExchange(&boot_light86.enabled,0); InterlockedExchange(&boot_light86.stop,BOOT_L86_ABORT_DEVICE);
    /* Confirmed removal aborts GPU work; do not Map or pin the old device into
     * recovery. Command lists do not AddRef these resources for us. */
    if(boot_light86.rb) ID3D12Resource_Release(boot_light86.rb);
    if(boot_light86.fence) ID3D12Fence_Release(boot_light86.fence);
    boot_light86.rb=NULL; boot_light86.fence=NULL; InterlockedExchange(&boot_light86.ready,0);
}
static DWORD WINAPI boot_light86_worker(void *unused) {
    (void)unused; unsigned emitted[BOOT_L86_BATCHES]={0},batches=0,aborted=0,io_error=0; uint64_t bytes=0;
    char line[16384]; unsigned quiesced=0,stop_waits=0;
    for(;;) {
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        if((uint64_t)now.QuadPart<boot_light86.start_qpc || (uint64_t)now.QuadPart-boot_light86.start_qpc>=240*boot_light86.frequency)
            boot_light86_error(BOOT_L86_ABORT_TIMEOUT);
        if(InterlockedCompareExchange(&boot_light86.stop,0,0) && !quiesced) {
            if(InterlockedCompareExchange(&boot_light86.producer,0,0)) {
                if(++stop_waits>=100) { io_error=2; break; }
                Sleep(10); continue;
            }
            boot_light86_abort_pending((unsigned)InterlockedCompareExchange(&boot_light86.stop,0,0)); quiesced=1;
        }
        unsigned used=(unsigned)InterlockedCompareExchange(&boot_light86.used,0,0),pending=0;
        for(unsigned i=0;i<used;++i) {
            BootLight86Batch *b=&boot_light86.batch[i]; if(emitted[i]) continue;
            BootLight86Batch copy; int have=0;
            LONG state=InterlockedCompareExchange(&b->state,0,0);
            if(state==BOOT_L86_DONE && InterlockedCompareExchange(&b->state,BOOT_L86_EXPORTING,BOOT_L86_DONE)==BOOT_L86_DONE) {
                have=1;
            }
            state=InterlockedCompareExchange(&b->state,0,0);
            if(have || state==BOOT_L86_ABORTED) memcpy(&copy,b,sizeof copy);
            if(!have && state!=BOOT_L86_ABORTED) { ++pending; continue; }
            int n=snprintf(line,sizeof line,
                "{\"kind\":\"batch\",\"slot\":%u,\"sub\":%llu,\"event\":%u,\"ordinal\":%u,\"recording\":%llu,\"execute\":%llu,\"fence\":%llu,\"completed\":%llu,\"fence_object\":%llu,\"queue\":%llu,\"allocator\":%llu,\"segment\":%u,\"renderer\":%llu,\"device\":%llu,\"cl\":%llu,\"qpc\":%llu,\"draw_issued\":%ld,\"sub_end_observed\":%ld,\"row_mask\":%u,\"aborted\":%u,\"abort_reason\":%u,\"offset\":%llu,\"valid_bytes\":%u,\"sources\":[",
                i,(unsigned long long)copy.sub,copy.event,copy.tic,(unsigned long long)copy.recording,(unsigned long long)copy.execute,(unsigned long long)copy.fence,
                (unsigned long long)copy.completed,(unsigned long long)copy.fence_object,(unsigned long long)copy.queue,(unsigned long long)copy.allocator,copy.segment,
                (unsigned long long)copy.renderer,(unsigned long long)copy.device,(unsigned long long)copy.cl,(unsigned long long)copy.qpc,(long)copy.draw,(long)copy.sub_done,copy.row_mask,!have,copy.abort_reason,(unsigned long long)bytes,have?(unsigned)sizeof boot_light86.cpu[i]:0u);
            for(unsigned j=0;n>0 && (size_t)n<sizeof line && j<3;++j) {
                const BootLight86Source *s=&copy.source[j];
                int k=snprintf(line+n,sizeof line-(size_t)n,"%s[%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu]",j?",":"",
                    (unsigned long long)s->resource,(unsigned long long)s->address,(unsigned long long)s->sequence,(unsigned long long)s->index,(unsigned long long)s->generation,
                    (unsigned long long)s->width,(unsigned long long)s->height,(unsigned long long)s->format,(unsigned long long)s->state,(unsigned long long)s->layer,(unsigned long long)s->layers,(unsigned long long)s->pitch);
                if(k<=0 || (size_t)k>=sizeof line-(size_t)n) n=-1; else n+=k;
            }
            if(n>0 && (size_t)n<sizeof line) { int k=snprintf(line+n,sizeof line-(size_t)n,"],\"values\":["); if(k<=0 || (size_t)k>=sizeof line-(size_t)n) n=-1; else n+=k; }
            for(unsigned j=0;n>0 && (size_t)n<sizeof line && j<BOOT_L86_VALUES;++j) {
                int k=snprintf(line+n,sizeof line-(size_t)n,"%s%llu",j?",":"",(unsigned long long)copy.values[j]);
                if(k<=0 || (size_t)k>=sizeof line-(size_t)n) n=-1; else n+=k;
            }
            if(n>0 && (size_t)n<sizeof line) { int k=snprintf(line+n,sizeof line-(size_t)n,"]}\n"); if(k<=0 || (size_t)k>=sizeof line-(size_t)n) n=-1; else n+=k; }
            if(n<=0 || (size_t)n>=sizeof line || (have && !boot_light86_write(boot_light86.data,boot_light86.cpu[i],sizeof boot_light86.cpu[i])) || !boot_light86_write(boot_light86.meta,line,(DWORD)n)) { io_error=1; break; }
            if(have) { bytes+=sizeof boot_light86.cpu[i]; ++batches; InterlockedExchange(&b->state,BOOT_L86_PERSISTED); } else ++aborted;
            emitted[i]=1;
        }
        if(io_error) break;
        if(!pending && (InterlockedCompareExchange(&boot_light86.stop,0,0) || InterlockedCompareExchange(&boot_light86.window_end,0,0))) {
            InterlockedExchange(&boot_light86.enabled,0);
            if(InterlockedCompareExchange(&boot_light86.producer,0,0)) { Sleep(10); continue; }
            quiesced=1; break;
        }
        Sleep(10);
    }
    InterlockedExchange(&boot_light86.enabled,0);
    /* If a paused producer did not quiesce, fail closed without reading its
     * mutable match slots. It cannot be mistaken for accepted pixel coverage. */
    unsigned matches=quiesced?(unsigned)InterlockedCompareExchange(&boot_light86.match_used,0,0):0;
    for(unsigned i=0;!io_error && i<matches;++i) {
        const BootLight86Match *m=&boot_light86.match[i];
        int n=snprintf(line,sizeof line,"{\"kind\":\"match\",\"sub\":%llu,\"event\":%u,\"reason\":%u,\"resource2\":\"%llx\",\"address\":\"%llx\",\"selected\":%u}\n",(unsigned long long)m->sub,m->event,m->tic,(unsigned long long)m->array,(unsigned long long)m->address,m->selected);
        if(n<=0 || (size_t)n>=sizeof line || !boot_light86_write(boot_light86.meta,line,(DWORD)n)) io_error=1;
    }
    LONG64 skipped=InterlockedCompareExchange64(&boot_light86.skipped,0,0),loss=InterlockedCompareExchange64(&boot_light86.loss,0,0),match_lost=InterlockedCompareExchange64(&boot_light86.match_lost,0,0);
    LONG stop_reason=InterlockedCompareExchange(&boot_light86.stop,0,0);
    int n=snprintf(line,sizeof line,"{\"kind\":\"end\",\"batches\":%u,\"aborted\":%u,\"bytes\":%llu,\"matches\":%u,\"skipped\":%lld,\"loss\":%lld,\"match_lost\":%lld,\"stop_reason\":%ld,\"capture_complete_proven\":false}\n",batches,aborted,(unsigned long long)bytes,matches,(long long)skipped,(long long)loss,(long long)match_lost,(long)stop_reason);
    if(n<=0 || (size_t)n>=sizeof line || !boot_light86_write(boot_light86.meta,line,(DWORD)n)) io_error=1;
    if(!FlushFileBuffers(boot_light86.data)) io_error=1; if(!FlushFileBuffers(boot_light86.meta)) io_error=1;
    if(!CloseHandle(boot_light86.data)) io_error=1; if(!CloseHandle(boot_light86.meta)) io_error=1;
    char temp[MAX_PATH]; snprintf(temp,sizeof temp,"%s.tmp",boot_light86.receipt_path);
    HANDLE receipt=CreateFileA(temp,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
    n=snprintf(line,sizeof line,"{\"version\":86,\"pid\":%lu,\"creation_filetime\":%llu,\"start_qpc\":%llu,\"batches\":%u,\"aborted\":%u,\"bytes\":%llu,\"matches\":%u,\"skipped\":%lld,\"loss\":%lld,\"match_lost\":%lld,\"stop_reason\":%ld,\"io_error\":%u,\"producer_quiesced\":%s,\"capture_complete_proven\":false}\n",(unsigned long)GetCurrentProcessId(),(unsigned long long)boot_light86.creation_filetime,(unsigned long long)boot_light86.start_qpc,batches,aborted,(unsigned long long)bytes,matches,(long long)skipped,(long long)loss,(long long)match_lost,(long)stop_reason,io_error,quiesced?"true":"false");
    int receipt_ok=receipt!=INVALID_HANDLE_VALUE && n>0 && (size_t)n<sizeof line;
    if(receipt!=INVALID_HANDLE_VALUE) { if(receipt_ok && !boot_light86_write(receipt,line,(DWORD)n)) receipt_ok=0; if(!FlushFileBuffers(receipt)) receipt_ok=0; if(!CloseHandle(receipt)) receipt_ok=0; }
    if(!receipt_ok || !MoveFileExA(temp,boot_light86.receipt_path,MOVEFILE_WRITE_THROUGH)) fprintf(stderr,"light-draw86: receipt publish failed\n");
    return 0;
}
static BOOL CALLBACK boot_light86_initialize(PINIT_ONCE once,PVOID arg,PVOID *result) {
    (void)once; (void)arg; (void)result;
    char text[160]; DWORD n=GetEnvironmentVariableA("TOTK_LIGHT_DRAW_PROBE",text,sizeof text);
    if(!n) return TRUE; boot_light86.requested=1;
    if(n>=sizeof text || boot_light86_parse(text,&boot_light86.config)!=1) { fprintf(stderr,"light-draw86: invalid configuration; disabled\n"); return TRUE; }
    FILETIME ct,et,kt,ut; if(!GetProcessTimes(GetCurrentProcess(),&ct,&et,&kt,&ut)) return TRUE;
    boot_light86.creation_filetime=((uint64_t)ct.dwHighDateTime<<32)|ct.dwLowDateTime;
    LARGE_INTEGER q,f; QueryPerformanceCounter(&q); QueryPerformanceFrequency(&f);
    if(q.QuadPart<=0 || f.QuadPart<=0) return TRUE;
    boot_light86.start_qpc=(uint64_t)q.QuadPart; boot_light86.frequency=(uint64_t)f.QuadPart;
    char data[MAX_PATH],meta[MAX_PATH];
    snprintf(data,sizeof data,"frames/light-draw86-%lu-%llu.bin",(unsigned long)GetCurrentProcessId(),(unsigned long long)q.QuadPart);
    snprintf(meta,sizeof meta,"frames/light-draw86-%lu-%llu.jsonl",(unsigned long)GetCurrentProcessId(),(unsigned long long)q.QuadPart);
    snprintf(boot_light86.receipt_path,sizeof boot_light86.receipt_path,"frames/light-draw86-%lu-%llu.receipt.json",(unsigned long)GetCurrentProcessId(),(unsigned long long)q.QuadPart);
    boot_light86.data=CreateFileA(data,GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
    boot_light86.meta=CreateFileA(meta,GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
    if(boot_light86.data==INVALID_HANDLE_VALUE || boot_light86.meta==INVALID_HANDLE_VALUE) { if(boot_light86.data!=INVALID_HANDLE_VALUE) CloseHandle(boot_light86.data); if(boot_light86.meta!=INVALID_HANDLE_VALUE) CloseHandle(boot_light86.meta); fprintf(stderr,"light-draw86: fresh output creation failed\n"); return TRUE; }
    char line[2048]; int size=snprintf(line,sizeof line,"{\"kind\":\"header\",\"version\":86,\"pid\":%lu,\"creation_filetime\":%llu,\"start_qpc\":%llu,\"qpc_frequency\":%llu,\"first\":%llu,\"last\":%llu,\"address0\":%llu,\"address1\":%llu,\"per_sub\":8,\"max_batches\":328,\"width\":640,\"height\":360,\"coordinates\":[[64,64],[288,180],[512,300]],\"row_bytes\":256,\"placement_stride\":512,\"binary_order\":\"batch,third_pre3,r11_0_post3,r11_1_post3,third_post3\",\"vs\":\"ee7a38093696d24514f09c9a2dfc4fd72899b6101484f941f3402105b9337e9f\",\"ps\":\"96e44eaaa19a7fab384c2dff8171d3a522f2f600d61c1a657ef51bbdfb416109\",\"capture_complete_proven\":false}\n",
        (unsigned long)GetCurrentProcessId(),(unsigned long long)boot_light86.creation_filetime,(unsigned long long)q.QuadPart,(unsigned long long)f.QuadPart,
        (unsigned long long)boot_light86.config.first,(unsigned long long)boot_light86.config.last,(unsigned long long)boot_light86.config.address0,(unsigned long long)boot_light86.config.address1);
    if(size<=0 || (size_t)size>=sizeof line || !boot_light86_write(boot_light86.meta,line,(DWORD)size)) { CloseHandle(boot_light86.data); CloseHandle(boot_light86.meta); return TRUE; }
    InterlockedExchange(&boot_light86.enabled,1);
    boot_light86.thread=CreateThread(NULL,0,boot_light86_worker,NULL,0,NULL);
    if(!boot_light86.thread) { InterlockedExchange(&boot_light86.enabled,0); CloseHandle(boot_light86.data); CloseHandle(boot_light86.meta); }
    else CloseHandle(boot_light86.thread); /* worker owns output; no callback joins it */
    return TRUE;
}
static inline void boot_light86_init(void) {
    static INIT_ONCE once=INIT_ONCE_STATIC_INIT; InitOnceExecuteOnce(&once,boot_light86_initialize,NULL,NULL);
}
static inline void boot_light86_prepare(const void *renderer,ID3D12Device *device,ID3D12Fence *fence,const void *queue,const void *cl) {
    if(!boot_light86_enter()) return;
    if(InterlockedCompareExchange(&boot_light86.ready,0,0)) { if(boot_light86.renderer!=(uintptr_t)renderer || boot_light86.device!=(uintptr_t)device) boot_light86_error(BOOT_L86_ABORT_GENERATION); boot_light86_leave(); return; }
    D3D12_HEAP_PROPERTIES hp={D3D12_HEAP_TYPE_READBACK,D3D12_CPU_PAGE_PROPERTY_UNKNOWN,D3D12_MEMORY_POOL_UNKNOWN,0,0};
    D3D12_RESOURCE_DESC d={0}; d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; d.Width=BOOT_L86_BYTES; d.Height=1; d.DepthOrArraySize=1; d.MipLevels=1; d.SampleDesc.Count=1; d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *rb=NULL;
    if(FAILED(ID3D12Device_CreateCommittedResource(device,&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,NULL,&IID_ID3D12Resource,(void **)&rb))) { boot_light86_error(BOOT_L86_ABORT_MAP); boot_light86_leave(); return; }
    if(!boot_light86_on()) { ID3D12Resource_Release(rb); boot_light86_leave(); return; }
    boot_light86.renderer=(uintptr_t)renderer; boot_light86.device=(uintptr_t)device; boot_light86.cl=(uintptr_t)cl; boot_light86.queue=(uintptr_t)queue; boot_light86.rb=rb; boot_light86.fence=fence; ID3D12Fence_AddRef(fence);
    InterlockedExchange(&boot_light86.ready,1); boot_light86_leave();
}
static inline void boot_light86_reset(const void *renderer,const void *cl,const void *allocator,unsigned segment) {
    if(!boot_light86_enter()) { if(boot_light86_on()) boot_light86_error(BOOT_L86_ABORT_GENERATION); return; }
    if(boot_light86.renderer!=(uintptr_t)renderer || boot_light86.cl!=(uintptr_t)cl) boot_light86_error(BOOT_L86_ABORT_GENERATION);
    else {
        for(unsigned i=0;i<(unsigned)InterlockedCompareExchange(&boot_light86.used,0,0);++i) {
            BootLight86Batch *b=&boot_light86.batch[i]; LONG state=InterlockedCompareExchange(&b->state,0,0);
            if(state==BOOT_L86_RESERVED || state==BOOT_L86_RECORDED) { b->abort_reason=BOOT_L86_ABORT_RESET; InterlockedExchange(&b->state,BOOT_L86_ABORTED); }
        }
        boot_light86.allocator=(uintptr_t)allocator; boot_light86.segment=segment; InterlockedIncrement64(&boot_light86.generation);
    }
    boot_light86_leave();
}
static inline void boot_light86_fence(const void *renderer,const void *cl,ID3D12Fence *fence,uint64_t value) {
    if(!boot_light86_enter()) { if(boot_light86_on()) boot_light86_error(BOOT_L86_ABORT_GENERATION); return; }
    if(fence!=boot_light86.fence) { boot_light86_error(BOOT_L86_ABORT_GENERATION); boot_light86_leave(); return; }
    uint64_t exec=(uint64_t)InterlockedIncrement64(&boot_light86.execute),gen=(uint64_t)InterlockedCompareExchange64(&boot_light86.generation,0,0);
    for(unsigned i=0;i<(unsigned)InterlockedCompareExchange(&boot_light86.used,0,0);++i) boot_light86_attach(&boot_light86.batch[i],(uintptr_t)renderer,(uintptr_t)cl,gen,exec,value);
    boot_light86_leave();
}
/* Called only with an already-observed normal completion value. The render
 * owner performs Map/raw-copy; the writer never touches a D3D object. */
static inline void boot_light86_retire(ID3D12Fence *fence,uint64_t completed) {
    if(!boot_light86_enter()) return;
    if(!boot_light86.rb || fence!=boot_light86.fence) { boot_light86_leave(); return; }
    if(completed==UINT64_MAX) { boot_light86_device_removed(); boot_light86_leave(); return; }
    unsigned used=(unsigned)InterlockedCompareExchange(&boot_light86.used,0,0);
    for(unsigned i=0;i<used;++i) {
        BootLight86Batch *b=&boot_light86.batch[i];
        if(!boot_light86_claim(b,completed)) continue;
        b->completed=completed;
        uint8_t *map=NULL; D3D12_RANGE rr={(SIZE_T)i*BOOT_L86_BATCH_BYTES,(SIZE_T)(i+1)*BOOT_L86_BATCH_BYTES};
        if(FAILED(ID3D12Resource_Map(boot_light86.rb,0,&rr,(void **)&map)) || !map) {
            b->abort_reason=BOOT_L86_ABORT_MAP; InterlockedExchange(&b->state,BOOT_L86_ABORTED); boot_light86_error(BOOT_L86_ABORT_MAP); break;
        }
        for(unsigned row=0;row<BOOT_L86_ROWS;++row)
            memcpy(boot_light86.cpu[i]+row*BOOT_L86_ROW_BYTES,map+boot_light86_offset(i,row),BOOT_L86_ROW_BYTES);
        D3D12_RANGE none={0,0}; ID3D12Resource_Unmap(boot_light86.rb,0,&none);
        InterlockedExchange(&b->state,BOOT_L86_DONE);
    }
    boot_light86_leave();
}
static inline int boot_light86_hashes(const uint8_t vs[32],const uint8_t ps[32]) {
    static const uint8_t v[32]={0xee,0x7a,0x38,0x09,0x36,0x96,0xd2,0x45,0x14,0xf0,0x9c,0x9a,0x2d,0xfc,0x4f,0xd7,0x28,0x99,0xb6,0x10,0x14,0x84,0xf9,0x41,0xf3,0x40,0x21,0x05,0xb9,0x33,0x7e,0x9f};
    static const uint8_t p[32]={0x96,0xe4,0x4e,0xaa,0xa1,0x9a,0x7f,0xab,0x38,0x4c,0x2d,0xff,0x81,0x71,0xd3,0xa5,0x22,0xf2,0xf6,0x00,0xd6,0x1c,0x1a,0x65,0x7e,0xf5,0x1b,0xbd,0xfb,0x41,0x61,0x09};
    return !memcmp(v,vs,32) && !memcmp(p,ps,32);
}
static inline BootLight86Batch *boot_light86_begin(const void *renderer,const void *cl,uint64_t sub,unsigned event,
        const BootLight86Source source[3],const uint64_t values[BOOT_L86_VALUES],unsigned skip) {
    if(!boot_light86_on() || !boot_light86_in_window(&boot_light86.config,sub,source[0].address,source[1].address)) return NULL;
    if(!boot_light86_enter()) return NULL;
    unsigned used=(unsigned)InterlockedCompareExchange(&boot_light86.used,0,0),ordinal=0;
    for(unsigned i=0;i<used;++i) if(boot_light86.batch[i].sub==sub) ++ordinal;
    if(!skip && !boot_light86_sources(source)) skip=BOOT_L86_SKIP_SHAPE;
    if(!skip && (!InterlockedCompareExchange(&boot_light86.ready,0,0) || !boot_light86.rb || boot_light86.renderer!=(uintptr_t)renderer ||
       boot_light86.cl!=(uintptr_t)cl || !InterlockedCompareExchange64(&boot_light86.generation,0,0))) skip=BOOT_L86_SKIP_GENERATION;
    if(!skip && (ordinal>=BOOT_L86_PER_SUB || used>=BOOT_L86_BATCHES)) skip=BOOT_L86_SKIP_CAP;
    unsigned mi=(unsigned)InterlockedCompareExchange(&boot_light86.match_used,0,0);
    if(mi<BOOT_L86_MATCHES) { BootLight86Match *m=&boot_light86.match[mi]; m->sub=sub; m->event=event; m->tic=skip; m->array=source[2].resource; m->address=source[2].address; m->selected=!skip; InterlockedExchange(&boot_light86.match_used,(LONG)(mi+1)); }
    else InterlockedIncrement64(&boot_light86.match_lost);
    if(skip) { InterlockedIncrement64(&boot_light86.skipped); boot_light86_leave(); return NULL; }
    BootLight86Batch *b=&boot_light86.batch[used]; memset(b,0,sizeof *b); b->slot=used; b->sub=sub; b->event=event; b->tic=ordinal;
    b->recording=(uint64_t)InterlockedCompareExchange64(&boot_light86.generation,0,0); b->renderer=(uintptr_t)renderer; b->cl=(uintptr_t)cl; b->device=boot_light86.device;
    b->queue=boot_light86.queue; b->allocator=boot_light86.allocator; b->segment=boot_light86.segment; b->fence_object=(uintptr_t)boot_light86.fence;
    memcpy(b->source,source,sizeof b->source); memcpy(b->values,values,sizeof b->values); LARGE_INTEGER q; QueryPerformanceCounter(&q); b->qpc=(uint64_t)q.QuadPart;
    InterlockedExchange(&b->state,BOOT_L86_RESERVED); InterlockedExchange(&boot_light86.used,(LONG)(used+1)); return b;
}
static inline void boot_light86_snapshot(BootLight86Batch *b,ID3D12GraphicsCommandList *cl,unsigned target,int before) {
    if(!b) return;
    ID3D12Resource *source=(ID3D12Resource *)(uintptr_t)b->source[target].resource;
    D3D12_RESOURCE_BARRIER barrier={0}; barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource=source; barrier.Transition.Subresource=0;
    barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET; barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
    ID3D12GraphicsCommandList_ResourceBarrier(cl,1,&barrier);
    unsigned row0=before?0:target==0?3:target==1?6:9;
    for(unsigned s=0;s<BOOT_L86_STRIPS;++s) {
        unsigned row=row0+s;
        D3D12_TEXTURE_COPY_LOCATION dl={boot_light86.rb,D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,{0}},sl={source,D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,{0}};
        dl.PlacedFootprint.Offset=boot_light86_offset(b->slot,row); dl.PlacedFootprint.Footprint.Format=target==2?DXGI_FORMAT_R8G8B8A8_UNORM:DXGI_FORMAT_R11G11B10_FLOAT;
        dl.PlacedFootprint.Footprint.Width=64; dl.PlacedFootprint.Footprint.Height=1; dl.PlacedFootprint.Footprint.Depth=1; dl.PlacedFootprint.Footprint.RowPitch=256; sl.SubresourceIndex=0;
        unsigned x=boot_light86_x(s),y=boot_light86_y(s); D3D12_BOX box={x,y,0,x+64,y+1,1};
        ID3D12GraphicsCommandList_CopyTextureRegion(cl,&dl,0,0,0,&sl,&box); b->row_mask|=1u<<row;
    }
    barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_SOURCE; barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET;
    ID3D12GraphicsCommandList_ResourceBarrier(cl,1,&barrier);
}
static inline void boot_light86_end(BootLight86Batch *b,int issued) {
    if(!b) return;
    if(issued && b->row_mask==4095) { b->draw=1; InterlockedExchange(&b->state,BOOT_L86_RECORDED); }
    else { b->abort_reason=BOOT_L86_ABORT_PARTIAL; InterlockedExchange(&b->state,BOOT_L86_ABORTED); }
    boot_light86_leave();
}
static inline uint32_t boot_light86_bits(float f) { uint32_t bits; memcpy(&bits,&f,4); return bits; }
static inline void boot_light86_sub_end(uint64_t sub) {
    if(!boot_light86_enter()) return;
    for(unsigned i=0;i<(unsigned)InterlockedCompareExchange(&boot_light86.used,0,0);++i) {
        BootLight86Batch *b=&boot_light86.batch[i]; LONG state=InterlockedCompareExchange(&b->state,0,0);
        if((state==BOOT_L86_RECORDED || state==BOOT_L86_FENCED) && b->sub==sub) InterlockedExchange(&b->sub_done,1);
    }
    if(sub>=boot_light86.config.last) InterlockedExchange(&boot_light86.window_end,1);
    boot_light86_leave();
}
#endif
