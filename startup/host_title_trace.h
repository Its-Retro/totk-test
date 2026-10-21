#ifndef TOTK_HOST_TITLE_TRACE_H
#define TOTK_HOST_TITLE_TRACE_H
/* Diagnostic79. Numeric host metadata only; initialize before NVN callbacks.
 * No producer allocation, file I/O, guest access, GPU call, or waiting lock.
 * Accepted records and submission brackets do NOT prove all GPU writers/hooks.
 * Environment: decimal first:last:event_budget:hex_base:hex_bytes. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define BOOT_TITLE_CAP 4096
#define BOOT_TITLE_BATCH 128
#define BOOT_TITLE_RECORD_LIMIT 262144
#define BOOT_TITLE_SECONDS 240
enum { BOOT_TITLE_ARM=1, BOOT_TITLE_SUB_BEGIN=2, BOOT_TITLE_SUB_END=3,
       BOOT_TITLE_EVENT_BEGIN=4, BOOT_TITLE_EVENT_END=5, BOOT_TITLE_RAW_RT=6,
       BOOT_TITLE_DRAW=7, BOOT_TITLE_HOST_RT=8, BOOT_TITLE_CLEAR=9,
       BOOT_TITLE_ASSEMBLY=10, BOOT_TITLE_SOURCE=11, BOOT_TITLE_SAMPLE=12, BOOT_TITLE_EXHAUSTED=13 };
typedef struct BootTitleRecord { uint64_t qpc, serial, thread, kind, sub, event, v[24]; } BootTitleRecord;
static BootTitleRecord boot_title_ring[BOOT_TITLE_CAP];
static SRWLOCK boot_title_lock=SRWLOCK_INIT;
static volatile LONG boot_title_enabled, boot_title_done;
static int boot_title_requested, boot_title_initialized;
static uint64_t boot_title_first, boot_title_last, boot_title_budget, boot_title_base, boot_title_bytes;
static uint64_t boot_title_freq, boot_title_start, boot_title_serial, boot_title_clock;
static unsigned boot_title_head, boot_title_count, boot_title_accepted;
_Alignas(8) static volatile LONG64 boot_title_lost_lock, boot_title_lost_full, boot_title_lost_limit, boot_title_lost_stop, boot_title_lost_clock;
/* Renderer-thread context, serialized by the existing window lock. */
static uint64_t boot_title_sub, boot_title_event, boot_title_events;
static unsigned boot_title_event_active, boot_title_interesting, boot_title_exhausted;
static inline int boot_title_range(uint64_t va, uint64_t size) {
    if (!size || !boot_title_bytes) return 0;
    return va>=boot_title_base ? va-boot_title_base<boot_title_bytes : boot_title_base-va<size;
}
static inline int boot_title_window(uint64_t sub) {
    return boot_title_enabled && sub>=boot_title_first && sub<=boot_title_last;
}
/* Strict unsigned parser: no signs/whitespace/overflow/trailing characters. */
static inline int boot_title_parse(const char *p, uint64_t out[5]) {
    for (unsigned j=0;j<5;++j) {
        uint64_t v=0; unsigned digits=0, radix=j<3?10:16;
        for (;;) { unsigned d=(unsigned char)*p;
            if (d>='0'&&d<='9') d-='0';
            else if (d>='a'&&d<='f') d=d-'a'+10;
            else if (d>='A'&&d<='F') d=d-'A'+10;
            else break;
            if (d>=radix || v>(UINT64_MAX-d)/radix) return 0;
            v=v*radix+d; ++p; ++digits;
        }
        if (!digits || (j<4 ? *p++!=':' : *p!=0)) return 0;
        out[j]=v;
    }
    return out[0] && out[1]>=out[0] && out[1]-out[0]<64 && out[2] && out[2]<=65536 &&
           out[3] && out[4] && out[4]<=(UINT64_C(64)<<20) && out[3]<=UINT64_MAX-out[4];
}
static inline void boot_title_push(BootTitleRecord *e) {
    if (!boot_title_enabled) { InterlockedIncrement64(&boot_title_lost_stop); return; }
    if (!TryAcquireSRWLockExclusive(&boot_title_lock)) { InterlockedIncrement64(&boot_title_lost_lock); return; }
    if (!boot_title_enabled) InterlockedIncrement64(&boot_title_lost_stop);
    else if (boot_title_accepted>=BOOT_TITLE_RECORD_LIMIT) InterlockedIncrement64(&boot_title_lost_limit);
    else if (boot_title_count==BOOT_TITLE_CAP) InterlockedIncrement64(&boot_title_lost_full);
    else {
        LARGE_INTEGER q;
        if (!QueryPerformanceCounter(&q) || (uint64_t)q.QuadPart<boot_title_clock) InterlockedIncrement64(&boot_title_lost_clock);
        else { e->qpc=(uint64_t)q.QuadPart; boot_title_clock=e->qpc; e->thread=GetCurrentThreadId();
            e->serial=++boot_title_serial;
            boot_title_ring[(boot_title_head+boot_title_count)%BOOT_TITLE_CAP]=*e;
            ++boot_title_count; ++boot_title_accepted; }
    }
    ReleaseSRWLockExclusive(&boot_title_lock);
}
static inline unsigned boot_title_take(BootTitleRecord *out) {
    AcquireSRWLockExclusive(&boot_title_lock);
    unsigned n=boot_title_count<BOOT_TITLE_BATCH?boot_title_count:BOOT_TITLE_BATCH;
    for (unsigned i=0;i<n;++i) out[i]=boot_title_ring[(boot_title_head+i)%BOOT_TITLE_CAP];
    boot_title_head=(boot_title_head+n)%BOOT_TITLE_CAP; boot_title_count-=n;
    ReleaseSRWLockExclusive(&boot_title_lock); return n;
}
static inline int boot_title_write(HANDLE f,const char *p,size_t n) {
    DWORD wrote=0; return n<=MAXDWORD && WriteFile(f,p,(DWORD)n,&wrote,NULL) && wrote==n;
}
static DWORD WINAPI boot_title_writer(void *arg) {
    (void)arg; char path[192], receipt[224], pending[224], buf[131072];
    snprintf(path,sizeof path,"frames/title-window79-%lu-%llu.csv",(unsigned long)GetCurrentProcessId(),(unsigned long long)boot_title_start);
    HANDLE f=CreateFileA(path,GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
    if(f==INVALID_HANDLE_VALUE) { InterlockedExchange(&boot_title_enabled,0); fprintf(stderr,"title trace79: create failed %lu\n",(unsigned long)GetLastError()); return 1; }
    int k=snprintf(buf,sizeof buf,"# title_trace79,version=1,pid=%lu,qpc_frequency=%llu,start_qpc=%llu,first=%llu,last=%llu,event_budget=%llu,watch_base=%llx,watch_bytes=%llx,record_limit=%u,seconds=%u\nqpc,serial,thread,kind,sub,event",
        (unsigned long)GetCurrentProcessId(),(unsigned long long)boot_title_freq,(unsigned long long)boot_title_start,
        (unsigned long long)boot_title_first,(unsigned long long)boot_title_last,(unsigned long long)boot_title_budget,
        (unsigned long long)boot_title_base,(unsigned long long)boot_title_bytes,BOOT_TITLE_RECORD_LIMIT,BOOT_TITLE_SECONDS);
    for(unsigned j=0;j<24;++j) k+=snprintf(buf+k,sizeof buf-(size_t)k,",v%u",j);
    buf[k++]='\n'; if(!boot_title_write(f,buf,(size_t)k)) goto failed;
    uint64_t rows=0; unsigned reason=0;
    for(;;) {
        LARGE_INTEGER q; QueryPerformanceCounter(&q);
        if((uint64_t)q.QuadPart-boot_title_start>=boot_title_freq*BOOT_TITLE_SECONDS) { reason=1; InterlockedExchange(&boot_title_enabled,0); }
        if(boot_title_done) { reason=2; InterlockedExchange(&boot_title_enabled,0); }
        BootTitleRecord batch[BOOT_TITLE_BATCH]; unsigned n=boot_title_take(batch); size_t at=0;
        for(unsigned i=0;i<n;++i) { BootTitleRecord *e=&batch[i];
            int z=snprintf(buf+at,sizeof buf-at,"%llu,%llu,%llu,%llu,%llu,%llu",(unsigned long long)e->qpc,(unsigned long long)e->serial,
                (unsigned long long)e->thread,(unsigned long long)e->kind,(unsigned long long)e->sub,(unsigned long long)e->event);
            if(z<0 || (size_t)z>=sizeof buf-at) goto failed; at+=(size_t)z;
            for(unsigned j=0;j<24;++j) { z=snprintf(buf+at,sizeof buf-at,",%llx",(unsigned long long)e->v[j]);
                if(z<0 || (size_t)z>=sizeof buf-at) goto failed; at+=(size_t)z; }
            if(at>=sizeof buf) goto failed; buf[at++]='\n';
        }
        if(at && !boot_title_write(f,buf,at)) goto failed; rows+=n;
        if(!n) { if(!boot_title_enabled) break; Sleep(50); }
    }
    {
        uint64_t lock=(uint64_t)InterlockedCompareExchange64(&boot_title_lost_lock,0,0), full=(uint64_t)InterlockedCompareExchange64(&boot_title_lost_full,0,0);
        uint64_t limit=(uint64_t)InterlockedCompareExchange64(&boot_title_lost_limit,0,0), stop=(uint64_t)InterlockedCompareExchange64(&boot_title_lost_stop,0,0);
        uint64_t clock=(uint64_t)InterlockedCompareExchange64(&boot_title_lost_clock,0,0);
        int n=snprintf(buf,sizeof buf,"# end,reason=%u,rows=%llu,lock=%llu,full=%llu,limit=%llu,stop=%llu,clock=%llu,capture_complete_proven=0\n",
            reason,(unsigned long long)rows,(unsigned long long)lock,(unsigned long long)full,(unsigned long long)limit,(unsigned long long)stop,(unsigned long long)clock);
        if(!boot_title_write(f,buf,(size_t)n) || !FlushFileBuffers(f)) goto failed;
        { int ok=CloseHandle(f); f=INVALID_HANDLE_VALUE; if(!ok) goto failed; }
        snprintf(receipt,sizeof receipt,"%s.complete.json",path); snprintf(pending,sizeof pending,"%s.complete.tmp",path);
        HANDLE r=CreateFileA(pending,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,NULL);
        if(r==INVALID_HANDLE_VALUE) goto failed;
        n=snprintf(buf,sizeof buf,"{\"version\":1,\"pid\":%lu,\"start_qpc\":%llu,\"reason\":%u,\"rows\":%llu,\"lock\":%llu,\"full\":%llu,\"limit\":%llu,\"stop\":%llu,\"clock\":%llu,\"capture_complete_proven\":false}\n",
            (unsigned long)GetCurrentProcessId(),(unsigned long long)boot_title_start,reason,(unsigned long long)rows,
            (unsigned long long)lock,(unsigned long long)full,(unsigned long long)limit,(unsigned long long)stop,(unsigned long long)clock);
        int ok=boot_title_write(r,buf,(size_t)n); if(!CloseHandle(r)) ok=0;
        if(!ok || !MoveFileExA(pending,receipt,MOVEFILE_WRITE_THROUGH)) goto failed;
    }
    return 0;
failed:
    InterlockedExchange(&boot_title_enabled,0);
    fprintf(stderr,"title trace79: incomplete CSV/receipt %s (win32 %lu)\n",path,(unsigned long)GetLastError());
    if(f!=INVALID_HANDLE_VALUE) CloseHandle(f); return 1;
}
static inline void boot_title_init(void) {
    if(boot_title_initialized) return; boot_title_initialized=1;
    char text[128]; DWORD n=GetEnvironmentVariableA("TOTK_RT_WINDOW_TRACE",text,sizeof text);
    if(!n) return; boot_title_requested=1;
    uint64_t v[5]; if(n>=sizeof text || !boot_title_parse(text,v)) { fprintf(stderr,"title trace79: invalid window config; automatic payload watch suppressed\n"); return; }
    LARGE_INTEGER f,q; if(!QueryPerformanceFrequency(&f) || f.QuadPart<=0 || f.QuadPart>10000000000LL || !QueryPerformanceCounter(&q)) return;
    boot_title_first=v[0]; boot_title_last=v[1]; boot_title_budget=v[2]; boot_title_base=v[3]; boot_title_bytes=v[4];
    boot_title_freq=(uint64_t)f.QuadPart; boot_title_start=boot_title_clock=(uint64_t)q.QuadPart;
    InterlockedExchange(&boot_title_enabled,1);
    BootTitleRecord arm={0}; arm.kind=BOOT_TITLE_ARM; arm.v[0]=v[0]; arm.v[1]=v[1]; arm.v[2]=v[2]; arm.v[3]=v[3]; arm.v[4]=v[4]; boot_title_push(&arm);
    HANDLE w=CreateThread(NULL,0,boot_title_writer,NULL,0,NULL);
    if(w) CloseHandle(w); else { InterlockedExchange(&boot_title_enabled,0); fprintf(stderr,"title trace79: worker creation failed %lu\n",(unsigned long)GetLastError()); }
}
static inline void boot_title_submission(uint64_t sub,int end,uint64_t draws,uint64_t clears,uint64_t skipped,uint64_t ok) {
    if(!end) { boot_title_sub=sub; boot_title_event_active=boot_title_interesting=0; }
    if(boot_title_window(sub)) { BootTitleRecord e={0}; e.sub=sub; e.event=UINT64_MAX; e.kind=end?BOOT_TITLE_SUB_END:BOOT_TITLE_SUB_BEGIN;
        e.v[0]=draws; e.v[1]=clears; e.v[2]=skipped; e.v[3]=ok; e.v[4]=boot_title_events; e.v[5]=boot_title_exhausted; boot_title_push(&e); }
    if(end) { boot_title_event_active=boot_title_interesting=0; if(sub>=boot_title_last && boot_title_last) InterlockedExchange(&boot_title_done,1); }
}
static inline void boot_title_begin_event(uint64_t event,unsigned kind,const BootDrawState *s,unsigned instance,uint32_t clear,const uint32_t *regs) {
    boot_title_event=event; boot_title_event_active=boot_title_interesting=0;
    if(!boot_title_window(boot_title_sub)) return;
    if(boot_title_events>=boot_title_budget) {
        if(!boot_title_exhausted) { BootTitleRecord x={0}; x.kind=BOOT_TITLE_EXHAUSTED; x.sub=boot_title_sub; x.event=event;
            x.v[0]=boot_title_events; x.v[1]=boot_title_budget; boot_title_push(&x); }
        boot_title_exhausted=1; return;
    }
    ++boot_title_events; boot_title_event_active=1;
    BootTitleRecord e={0}; e.kind=BOOT_TITLE_EVENT_BEGIN; e.sub=boot_title_sub; e.event=event;
    e.v[0]=kind; e.v[1]=instance; e.v[2]=clear; e.v[3]=s->rt_count; e.v[4]=s->tic_address; e.v[5]=s->count; e.v[6]=s->topology;
    if(regs) for(unsigned j=0;j<8;++j) e.v[8+j]=regs[j]; boot_title_push(&e);
    /* Clear binds a raw slot, independent of active MRT mapping. Record all raw
     * slots touching the configured interval; no descriptor/guest reads. */
    for(unsigned j=0;j<8;++j) { const BootColorTarget *t=&s->rt[j];
        if(!t->format || !boot_title_range(t->address,1)) continue;
        boot_title_interesting=1; BootTitleRecord x={0}; x.kind=BOOT_TITLE_RAW_RT; x.sub=boot_title_sub; x.event=event;
        x.v[0]=j; x.v[1]=t->address; x.v[2]=t->width; x.v[3]=t->height; x.v[4]=t->format;
        x.v[5]=t->layers; x.v[6]=t->layer; x.v[7]=t->array_pitch; x.v[8]=t->write_mask;
        x.v[9]=t->log_gob_w; x.v[10]=t->log_gob_h; x.v[11]=t->log_gob_d; x.v[12]=t->pitch_layout;
        for(unsigned k=0;k<8;++k) x.v[13+k]=s->rt_map[k]; boot_title_push(&x);
    }
}
static inline void boot_title_end_event(int rc) {
    if(boot_title_event_active) { BootTitleRecord e={0}; e.kind=BOOT_TITLE_EVENT_END; e.sub=boot_title_sub; e.event=boot_title_event; e.v[0]=(uint32_t)rc; e.v[1]=boot_title_interesting; boot_title_push(&e); }
    boot_title_event_active=boot_title_interesting=0;
}
#endif
