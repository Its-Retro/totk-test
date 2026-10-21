#ifndef TOTK_HOST_LIGHT_INPUT_PROBE_H
#define TOTK_HOST_LIGHT_INPUT_PROBE_H
#include "host_light_input_probe_core.h"
#include <stdio.h>
#include <bcrypt.h>
static struct {
    BootLight88Config config; int requested;
    volatile LONG enabled,producer,used,window_end;
    _Alignas(8) volatile LONG64 loss,skipped,cap_lost;
    uint64_t creation,start,frequency; char text[160],config_hash[65],receipt_path[MAX_PATH];
    HANDLE data,meta;
    BootLight88Record record[BOOT_L88_RECORDS];
} boot_light88;
static _Thread_local unsigned boot_light88_pending;
static inline int boot_light88_clock(uint64_t *out) {
    LARGE_INTEGER q={0}; if(!QueryPerformanceCounter(&q) || q.QuadPart<=0) return 0;
    *out=(uint64_t)q.QuadPart; return 1;
}
static inline int boot_light88_on(void) { return boot_light88.requested && InterlockedCompareExchange(&boot_light88.enabled,0,0); }
static inline int boot_light88_enter(void) {
    if(!boot_light88_on()) return 0;
    if(InterlockedCompareExchange(&boot_light88.producer,1,0)) { InterlockedIncrement64(&boot_light88.loss); return 0; }
    if(!boot_light88_on()) { InterlockedExchange(&boot_light88.producer,0); return 0; }
    return 1;
}
static inline void boot_light88_leave(void) { InterlockedExchange(&boot_light88.producer,0); }
static inline void boot_light88_abort(BootLight88Record *r,unsigned why) {
    r->abort_reason=why; InterlockedExchange(&r->state,BOOT_L88_ABORTED);
}
static inline BootLight88Record *boot_light88_current(uint64_t token,uintptr_t draw) {
    if(!boot_light88_pending || boot_light88_pending>BOOT_L88_RECORDS) return NULL;
    BootLight88Record *r=&boot_light88.record[boot_light88_pending-1];
    if(InterlockedCompareExchange(&r->state,0,0)!=BOOT_L88_PENDING) { boot_light88_pending=0; return NULL; }
    if(r->begin.token!=token || r->begin.draw!=draw || r->tid!=GetCurrentThreadId() ||
       r->loss_begin!=(uint64_t)InterlockedCompareExchange64(&boot_light88.loss,0,0)) {
        boot_light88_abort(r,BOOT_L88_CONTEXT); boot_light88_pending=0; return NULL;
    }
    return r;
}
static unsigned boot_light88_begin_cb(const BootCbvTapBegin *begin) {
    if(!boot_light88_enter()) return 0;
    if(boot_light88_pending && boot_light88_pending<=BOOT_L88_RECORDS) {
        BootLight88Record *old=&boot_light88.record[boot_light88_pending-1];
        if(InterlockedCompareExchange(&old->state,0,0)==BOOT_L88_PENDING) boot_light88_abort(old,BOOT_L88_CONTEXT);
    }
    boot_light88_pending=0;
    if(!boot_light88_select(&boot_light88.config,begin)) { boot_light88_leave(); return 0; }
    uint64_t qpc=0;
    if(!boot_light88_clock(&qpc)) { InterlockedIncrement64(&boot_light88.loss); boot_light88_leave(); return 0; }
    unsigned used=(unsigned)InterlockedCompareExchange(&boot_light88.used,0,0),ordinal=0;
    for(unsigned i=0;i<used;++i) if(boot_light88.record[i].begin.sub==begin->sub) ++ordinal;
    if(used>=BOOT_L88_RECORDS || ordinal>=BOOT_L88_PER_SUB) { InterlockedIncrement64(&boot_light88.cap_lost); boot_light88_leave(); return 0; }
    BootLight88Record *r=&boot_light88.record[used]; memset(r,0,sizeof *r);
    r->slot=used; r->ordinal=ordinal; r->tid=GetCurrentThreadId(); r->begin=*begin;
    r->qpc_begin=qpc;
    r->loss_begin=(uint64_t)InterlockedCompareExchange64(&boot_light88.loss,0,0);
    InterlockedExchange(&r->state,BOOT_L88_PENDING); InterlockedExchange(&boot_light88.used,(LONG)(used+1));
    boot_light88_pending=used+1; boot_light88_leave(); return 1;
}
static void boot_light88_binding_cb(const BootCbvTapBinding *binding) {
    if(!boot_light88_enter()) return;
    BootLight88Record *r=boot_light88_current(binding->token,binding->draw);
    if(r && !boot_light88_binding(r,binding)) { boot_light88_abort(r,BOOT_L88_BINDING); boot_light88_pending=0; }
    boot_light88_leave();
}
static void boot_light88_table_cb(const BootCbvTapTable *table,const BootCbvTapCv actual[19]) {
    if(!boot_light88_enter()) return;
    BootLight88Record *r=boot_light88_current(table->token,table->draw);
    if(r && !boot_light88_table(r,table,actual)) { boot_light88_abort(r,BOOT_L88_TABLE); boot_light88_pending=0; }
    else if(r && table->stage==1 && table->group==4 && !boot_light88_clock(&r->qpc_table)) { boot_light88_abort(r,BOOT_L88_CONTEXT); boot_light88_pending=0; }
    boot_light88_leave();
}
static void boot_light88_end_cb(uint64_t token,uintptr_t draw,unsigned aborted) {
    if(!boot_light88_enter()) return;
    BootLight88Record *r=boot_light88_current(token,draw);
    if(r) {
        if(aborted || r->resolver_closed || !r->table_seen || r->bindings!=7) { boot_light88_abort(r,BOOT_L88_TABLE); boot_light88_pending=0; }
        else r->resolver_closed=1;
    }
    boot_light88_leave();
}
static void boot_light88_issued_cb(uint64_t token,uintptr_t draw,unsigned issued) {
    if(!boot_light88_enter()) return;
    BootLight88Record *r=boot_light88_current(token,draw);
    if(r) {
        if(issued!=1 || !r->resolver_closed || !r->table_seen || r->bindings!=7) boot_light88_abort(r,BOOT_L88_UNISSUED);
        else if(!boot_light88_clock(&r->qpc_draw)) boot_light88_abort(r,BOOT_L88_CONTEXT);
        else {
            r->loss_end=(uint64_t)InterlockedCompareExchange64(&boot_light88.loss,0,0);
            if(r->loss_end!=r->loss_begin) boot_light88_abort(r,BOOT_L88_CONTEXT);
            else { r->issued=1; InterlockedExchange(&r->state,BOOT_L88_DONE); }
        }
        boot_light88_pending=0;
    }
    boot_light88_leave();
}
static void boot_light88_fault_cb(unsigned why) {
    (void)why; if(!boot_light88_enter()) return;
    if(boot_light88_pending && boot_light88_pending<=BOOT_L88_RECORDS) {
        BootLight88Record *r=&boot_light88.record[boot_light88_pending-1];
        if(InterlockedCompareExchange(&r->state,0,0)==BOOT_L88_PENDING) boot_light88_abort(r,BOOT_L88_CONTEXT);
    }
    boot_light88_pending=0; boot_light88_leave();
}
static inline void boot_light88_sub_end(uint64_t sub) {
    if(boot_light88_on() && sub>=boot_light88.config.last) InterlockedExchange(&boot_light88.window_end,1);
}
static inline int boot_light88_write(HANDLE file,const void *bytes,DWORD count) {
    DWORD wrote=0; return WriteFile(file,bytes,count,&wrote,NULL) && wrote==count;
}
/* CPU-only writer: never dereferences a renderer, guest pointer or D3D object. */
static DWORD WINAPI boot_light88_worker(void *unused) {
    (void)unused; unsigned emitted[BOOT_L88_RECORDS]={0},done=0,aborted=0,io_error=0,quiesced=0,reason=0;
    uint64_t bytes=0; char line[16384];
    for(;;) {
        uint64_t now=0;
        if(!boot_light88_clock(&now) || now<boot_light88.start || now-boot_light88.start>=240*boot_light88.frequency) reason=BOOT_L88_TIMEOUT;
        if(reason || InterlockedCompareExchange(&boot_light88.window_end,0,0)) {
            InterlockedExchange(&boot_light88.enabled,0);
            for(unsigned tries=0;tries<100;++tries) {
                if(!InterlockedCompareExchange(&boot_light88.producer,1,0)) {
                    unsigned used=(unsigned)InterlockedCompareExchange(&boot_light88.used,0,0);
                    for(unsigned i=0;i<used;++i) if(InterlockedCompareExchange(&boot_light88.record[i].state,0,0)==BOOT_L88_PENDING) boot_light88_abort(&boot_light88.record[i],reason?reason:BOOT_L88_STOP);
                    quiesced=1; boot_light88_leave(); break;
                }
                Sleep(10);
            }
            if(!quiesced) io_error=2;
        }
        unsigned used=(unsigned)InterlockedCompareExchange(&boot_light88.used,0,0);
        for(unsigned i=0;!io_error && i<used;++i) {
            const BootLight88Record *r=&boot_light88.record[i]; if(emitted[i]) continue;
            LONG state=InterlockedCompareExchange((volatile LONG *)&r->state,0,0);
            if(state!=BOOT_L88_DONE && state!=BOOT_L88_ABORTED) continue;
            unsigned valid=state==BOOT_L88_DONE?BOOT_L88_BYTES:0;
            int n=snprintf(line,sizeof line,"{\"kind\":\"record\",\"slot\":%u,\"ordinal\":%u,\"tid\":%u,\"sub\":%llu,\"event\":%u,\"token\":%llu,\"renderer\":%llu,\"draw\":%llu,\"pso\":%llu,\"qpc_begin\":%llu,\"qpc_table\":%llu,\"qpc_draw\":%llu,\"bindings_seen\":%u,\"payload_mask\":%u,\"table_seen\":%u,\"resolver_closed\":%u,\"draw_issued\":%u,\"aborted\":%u,\"abort_reason\":%u,\"offset\":%llu,\"valid_bytes\":%u,\"values\":[%u,%u,%u,%u,%u,%u,%u],\"targets\":[",
                i,r->ordinal,r->tid,(unsigned long long)r->begin.sub,r->begin.event,(unsigned long long)r->begin.token,(unsigned long long)r->begin.renderer,(unsigned long long)r->begin.draw,(unsigned long long)r->begin.pso,
                (unsigned long long)r->qpc_begin,(unsigned long long)r->qpc_table,(unsigned long long)r->qpc_draw,r->bindings,r->payload_mask,r->table_seen,r->resolver_closed,r->issued,!valid,r->abort_reason,(unsigned long long)bytes,valid,
                r->begin.count,r->begin.instances,r->begin.indexed,r->begin.topology,r->begin.vs_mask,r->begin.rt_count,r->begin.samples);
            for(unsigned j=0;n>0 && (size_t)n<sizeof line && j<3;++j) {
                const BootCbvTapTarget *t=&r->begin.target[j];
                int k=snprintf(line+n,sizeof line-(size_t)n,"%s[%llu,%llu,%u,%u,%u,%u,%u,%u,%u,%u]",j?",":"",(unsigned long long)r->begin.rt_resource[j],(unsigned long long)r->begin.rt_address[j],t->width,t->height,t->format,t->state,t->layer,t->layers,t->pitch,t->map);
                if(k<=0 || (size_t)k>=sizeof line-(size_t)n) n=-1; else n+=k;
            }
            if(n>0 && (size_t)n<sizeof line) { int k=snprintf(line+n,sizeof line-(size_t)n,"],\"table\":[%u,%u,%u,%u,%llu,%llu,%llu],\"bindings\":[",r->table.stage,r->table.group,r->table.base,r->table.reused,(unsigned long long)r->table.scope,(unsigned long long)r->table.list,(unsigned long long)r->table.map_generation); if(k<=0 || (size_t)k>=sizeof line-(size_t)n) n=-1; else n+=k; }
            for(unsigned j=0;n>0 && (size_t)n<sizeof line && j<BOOT_L88_BANKS;++j) {
                const BootCbvTapBinding *b=&r->binding[j];
                int k=snprintf(line+n,sizeof line-(size_t)n,"%s[%u,%u,%u,%llu,%u,%u,%llu,%u,%llu,%llu,%llu,%u,%u,%u,%llu,%llu,%llu]",j?",":"",b->slot,b->stage,b->group,(unsigned long long)b->requested_va,b->requested_size,b->valid,(unsigned long long)b->location,b->size,(unsigned long long)b->resource,(unsigned long long)b->offset,(unsigned long long)b->resource_bytes,b->mode,b->region,b->flags,(unsigned long long)b->scope,(unsigned long long)b->list,(unsigned long long)b->map_generation);
                if(k<=0 || (size_t)k>=sizeof line-(size_t)n) n=-1; else n+=k;
            }
            if(n>0 && (size_t)n<sizeof line) { int k=snprintf(line+n,sizeof line-(size_t)n,"],\"loss_begin\":%llu,\"loss_end\":%llu}\n",(unsigned long long)r->loss_begin,(unsigned long long)r->loss_end); if(k<=0 || (size_t)k>=sizeof line-(size_t)n) n=-1; else n+=k; }
            if(n<=0 || (size_t)n>=sizeof line || (valid && !boot_light88_write(boot_light88.data,r->payload,valid)) || !boot_light88_write(boot_light88.meta,line,(DWORD)n)) { io_error=1; break; }
            if(valid) { ++done; bytes+=valid; } else ++aborted;
            emitted[i]=1;
        }
        if(io_error || quiesced) break;
        Sleep(10);
    }
    InterlockedExchange(&boot_light88.enabled,0);
    LONG used=InterlockedCompareExchange(&boot_light88.used,0,0);
    LONG64 loss=InterlockedCompareExchange64(&boot_light88.loss,0,0),caps=InterlockedCompareExchange64(&boot_light88.cap_lost,0,0);
    int n=snprintf(line,sizeof line,"{\"kind\":\"end\",\"allocated\":%ld,\"records\":%u,\"aborted\":%u,\"bytes\":%llu,\"loss\":%lld,\"cap_lost\":%lld,\"stop_reason\":%u,\"capture_complete_proven\":false}\n",(long)used,done,aborted,(unsigned long long)bytes,(long long)loss,(long long)caps,reason);
    if(n<=0 || (size_t)n>=sizeof line || !boot_light88_write(boot_light88.meta,line,(DWORD)n)) io_error=1;
    if(!FlushFileBuffers(boot_light88.data)) io_error=1; if(!FlushFileBuffers(boot_light88.meta)) io_error=1;
    if(!CloseHandle(boot_light88.data)) io_error=1; if(!CloseHandle(boot_light88.meta)) io_error=1;
    char temp[MAX_PATH]; snprintf(temp,sizeof temp,"%s.tmp",boot_light88.receipt_path);
    HANDLE receipt=CreateFileA(temp,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
    n=snprintf(line,sizeof line,"{\"version\":88,\"pid\":%lu,\"creation_filetime\":%llu,\"start_qpc\":%llu,\"config_sha256\":\"%s\",\"allocated\":%ld,\"records\":%u,\"aborted\":%u,\"bytes\":%llu,\"loss\":%lld,\"cap_lost\":%lld,\"stop_reason\":%u,\"io_error\":%u,\"producer_quiesced\":%s,\"capture_complete_proven\":false}\n",(unsigned long)GetCurrentProcessId(),(unsigned long long)boot_light88.creation,(unsigned long long)boot_light88.start,boot_light88.config_hash,(long)used,done,aborted,(unsigned long long)bytes,(long long)loss,(long long)caps,reason,io_error,quiesced?"true":"false");
    int receipt_ok=receipt!=INVALID_HANDLE_VALUE && n>0 && (size_t)n<sizeof line;
    if(receipt!=INVALID_HANDLE_VALUE) { if(receipt_ok && !boot_light88_write(receipt,line,(DWORD)n)) receipt_ok=0; if(!FlushFileBuffers(receipt)) receipt_ok=0; if(!CloseHandle(receipt)) receipt_ok=0; }
    if(!receipt_ok || !MoveFileExA(temp,boot_light88.receipt_path,MOVEFILE_WRITE_THROUGH)) { fprintf(stderr,"light-input88: receipt publish failed\n"); return 1; }
    return io_error?1:0;
}
static BOOL CALLBACK boot_light88_initialize(PINIT_ONCE once,PVOID argument,PVOID *result) {
    (void)once; (void)argument; (void)result;
    DWORD n=GetEnvironmentVariableA("TOTK_LIGHT_INPUT_TRACE",boot_light88.text,sizeof boot_light88.text);
    if(!n) return TRUE; boot_light88.requested=1;
    if(n>=sizeof boot_light88.text || boot_light88_parse(boot_light88.text,&boot_light88.config)!=1) { fprintf(stderr,"light-input88: invalid configuration; disabled\n"); return TRUE; }
    BCRYPT_ALG_HANDLE alg=NULL; BCRYPT_HASH_HANDLE hash=NULL; uint8_t digest[32];
    int hash_ok=BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,NULL,0)==0;
    if(hash_ok) hash_ok=BCryptCreateHash(alg,&hash,NULL,0,NULL,0,0)==0;
    if(hash_ok) hash_ok=BCryptHashData(hash,(PUCHAR)boot_light88.text,n,0)==0 && BCryptFinishHash(hash,digest,sizeof digest,0)==0;
    if(hash) BCryptDestroyHash(hash); if(alg) BCryptCloseAlgorithmProvider(alg,0); if(!hash_ok) return TRUE;
    for(unsigned i=0;i<32;++i) snprintf(boot_light88.config_hash+2*i,3,"%02x",digest[i]);
    FILETIME ct,et,kt,ut; if(!GetProcessTimes(GetCurrentProcess(),&ct,&et,&kt,&ut)) return TRUE;
    boot_light88.creation=((uint64_t)ct.dwHighDateTime<<32)|ct.dwLowDateTime;
    LARGE_INTEGER q={0},f={0}; if(!QueryPerformanceCounter(&q) || !QueryPerformanceFrequency(&f) || q.QuadPart<=0 || f.QuadPart<=0) return TRUE;
    boot_light88.start=(uint64_t)q.QuadPart; boot_light88.frequency=(uint64_t)f.QuadPart;
    char data[MAX_PATH],meta[MAX_PATH];
    snprintf(data,sizeof data,"frames/light-input88-%lu-%llu.bin",(unsigned long)GetCurrentProcessId(),(unsigned long long)q.QuadPart);
    snprintf(meta,sizeof meta,"frames/light-input88-%lu-%llu.jsonl",(unsigned long)GetCurrentProcessId(),(unsigned long long)q.QuadPart);
    snprintf(boot_light88.receipt_path,sizeof boot_light88.receipt_path,"frames/light-input88-%lu-%llu.receipt.json",(unsigned long)GetCurrentProcessId(),(unsigned long long)q.QuadPart);
    boot_light88.data=CreateFileA(data,GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
    boot_light88.meta=CreateFileA(meta,GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
    if(boot_light88.data==INVALID_HANDLE_VALUE || boot_light88.meta==INVALID_HANDLE_VALUE) { if(boot_light88.data!=INVALID_HANDLE_VALUE) CloseHandle(boot_light88.data); if(boot_light88.meta!=INVALID_HANDLE_VALUE) CloseHandle(boot_light88.meta); return TRUE; }
    char line[2048]; int size=snprintf(line,sizeof line,"{\"kind\":\"header\",\"version\":88,\"pid\":%lu,\"creation_filetime\":%llu,\"start_qpc\":%llu,\"qpc_frequency\":%llu,\"config\":\"%s\",\"config_sha256\":\"%s\",\"first\":%llu,\"last\":%llu,\"address0\":%llu,\"address1\":%llu,\"per_sub\":8,\"max_records\":328,\"record_bytes\":256,\"binary_order\":\"record,ps_cb4_0_7,ps_cb3_10_15,ps_cb1_0_1\",\"vs\":\"ee7a38093696d24514f09c9a2dfc4fd72899b6101484f941f3402105b9337e9f\",\"ps\":\"96e44eaaa19a7fab384c2dff8171d3a522f2f600d61c1a657ef51bbdfb416109\",\"direct_payload_available\":false,\"capture_complete_proven\":false}\n",(unsigned long)GetCurrentProcessId(),(unsigned long long)boot_light88.creation,(unsigned long long)q.QuadPart,(unsigned long long)f.QuadPart,boot_light88.text,boot_light88.config_hash,(unsigned long long)boot_light88.config.first,(unsigned long long)boot_light88.config.last,(unsigned long long)boot_light88.config.address0,(unsigned long long)boot_light88.config.address1);
    if(size<=0 || (size_t)size>=sizeof line || !boot_light88_write(boot_light88.meta,line,(DWORD)size)) { CloseHandle(boot_light88.data); CloseHandle(boot_light88.meta); return TRUE; }
    InterlockedExchange(&boot_light88.enabled,1);
    HANDLE thread=CreateThread(NULL,0,boot_light88_worker,NULL,0,NULL);
    if(!thread) { InterlockedExchange(&boot_light88.enabled,0); CloseHandle(boot_light88.data); CloseHandle(boot_light88.meta); return TRUE; }
    CloseHandle(thread);
    const BootCbvTapObserver observer={boot_light88_begin_cb,boot_light88_binding_cb,boot_light88_table_cb,boot_light88_end_cb,boot_light88_issued_cb,boot_light88_fault_cb};
    if(!boot_cbv_tap_register(BOOT_CBV_TAP_88,&observer)) { InterlockedExchange(&boot_light88.window_end,1); InterlockedExchange(&boot_light88.enabled,0); }
    return TRUE;
}
static inline void boot_light88_init(void) {
    static INIT_ONCE once=INIT_ONCE_STATIC_INIT; InitOnceExecuteOnce(&once,boot_light88_initialize,NULL,NULL);
}
#endif
