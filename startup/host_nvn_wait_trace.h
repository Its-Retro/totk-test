#ifndef TOTK_HOST_NVN_WAIT_TRACE_H
#define TOTK_HOST_NVN_WAIT_TRACE_H
/* Diagnostic80 only. UI-started, default off; separate bounded observation stream.
 * Producers never allocate, initialize, wait for a lock, perform file I/O or query GPU state.
 * Admission clocks and optional original clocks are distinct. No call-intent completeness claim. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef BOOT_NW_CAPACITY
#define BOOT_NW_CAPACITY 1024u
#endif
#define BOOT_NW_BATCH 128u
enum { BOOT_NW_A=1,BOOT_NW_B=2,BOOT_NW_C=4,BOOT_NW_D=8,BOOT_NW_OBS=16 };
enum { BOOT_NW_OP_BEGIN=1,BOOT_NW_OP_END=2,BOOT_NW_RENDER_LOCK=3,
 BOOT_NW_FRONT_ENTRY=10,BOOT_NW_FRONT_READY=11,BOOT_NW_FRONT_BACKEND=12,BOOT_NW_FRONT_RESULT=13,BOOT_NW_FRONT_SIGNAL=14,
 BOOT_NW_BE_ENTRY=20,BOOT_NW_BE_READY=21,BOOT_NW_BE_ZERO=22,BOOT_NW_GUARD=23,
 BOOT_NW_RELEASE_BEGIN=24,BOOT_NW_RELEASE_END=25,BOOT_NW_REGISTER=26,
 BOOT_NW_WAIT_BEGIN=27,BOOT_NW_WAIT_END=28,BOOT_NW_VALUE_SPAN=29,
 BOOT_NW_REACQUIRE_BEGIN=30,BOOT_NW_REACQUIRE_END=31,BOOT_NW_WINDOW_BEGIN=32,BOOT_NW_WINDOW_END=33,
 BOOT_NW_HOUSE_BEGIN=34,BOOT_NW_HOUSE_END=35,BOOT_NW_HOUSE_IDLE_BEGIN=36,BOOT_NW_HOUSE_IDLE_END=37,
 BOOT_NW_TRIM_BEGIN=38,BOOT_NW_TRIM_END=39,BOOT_NW_NO_IDLE=40,
 BOOT_NW_COMPLETE_BEGIN=41,BOOT_NW_COMPLETE_END=42,BOOT_NW_PITCH_BEGIN=43,BOOT_NW_PITCH_END=44,
 BOOT_NW_BE_RETURN=45,BOOT_NW_COMPLETED=46,BOOT_NW_GPU_TARGET=49,BOOT_NW_GPU_SIGNAL=50,BOOT_NW_GPU_FIRST=51,
 BOOT_NW_GPU_WAIT_BEGIN=52,BOOT_NW_GPU_WAIT_END=53,BOOT_NW_GPU_FINAL=54,
 BOOT_NW_GPU_REASON=55,BOOT_NW_GPU_RETURN=56,BOOT_NW_HOUSE_COMPLETED=57,BOOT_NW_SUBMITTED=58 };
typedef struct BootNwContext { uint64_t op,parent,epoch;uint32_t kind,tid; } BootNwContext;
typedef struct BootNwEvent {
 uint64_t seq,qpc,op,parent,epoch,loss,observed_begin,observed_end,a,b,c,d;
 uint32_t pid,tid,kind,stage,mask;
} BootNwEvent;
typedef struct BootNwTrace {
 SRWLOCK lock;
 volatile LONG initialized,enabled,stop,io_error,stop_reason;
 volatile LONG64 attempted,full,contended,stopping,clock_lost,context_lost,loss,epochs;
 uint64_t accepted,written,frequency,qpc_low,qpc_high,filetime,creation,ordered,start,end;
 uint32_t head,count,pid,delay_ms,duration_ms,max_rows;
 BootNwEvent ring[BOOT_NW_CAPACITY],batch[BOOT_NW_BATCH];
 char csv[65536],path[260],receipt[280];
} BootNwTrace;
static BootNwTrace boot_nw;
static _Thread_local BootNwContext boot_nw_context;
static _Thread_local uint64_t boot_nw_next,boot_nw_epoch;
static inline int boot_nw_on(void){return boot_nw.enabled && !boot_nw.stop && !boot_nw.io_error;}
/* Exact existing registry keys, verified by tests against boot_host_fn_address("nvn:" + name).
 * No per-call string work, registry mutation, new guest access or function-pointer conversion. */
static inline unsigned boot_nw_kind(uint64_t key){
 switch(key){
 case UINT64_C(0x7ff00a3ed3e0):return 1; /* nvnSyncWait */
 case UINT64_C(0x7ff005c1d710):return 2; /* nvnQueueSubmitCommands */
 case UINT64_C(0x7ff0073e7a20):return 3; /* nvnQueueFinish */
 case UINT64_C(0x7ff001781710):return 4; /* nvnQueueFlush */
 case UINT64_C(0x7ff00536fa00):return 5; /* nvnQueueFenceSync */
 case UINT64_C(0x7ff00475e4e0):return 6; /* nvnQueuePresentTexture */
 default:return 0;
 }
}
static inline void boot_nw_emit(unsigned stage,unsigned mask,uint64_t begin,uint64_t end,
                                uint64_t a,uint64_t b,uint64_t c,uint64_t d){
 BootNwTrace*q=&boot_nw;
 if(!boot_nw_on() || !boot_nw_context.op)return;
 DWORD error=GetLastError();InterlockedIncrement64(&q->attempted);
 if(!TryAcquireSRWLockExclusive(&q->lock)){
  InterlockedIncrement64(&q->contended);InterlockedIncrement64(&q->loss);SetLastError(error);return;
 }
 if(q->stop){InterlockedIncrement64(&q->stopping);InterlockedIncrement64(&q->loss);goto done;}
 if(q->count==BOOT_NW_CAPACITY){InterlockedIncrement64(&q->full);InterlockedIncrement64(&q->loss);goto done;}
 LARGE_INTEGER now;
 if(!QueryPerformanceCounter(&now) || now.QuadPart<0 || (uint64_t)now.QuadPart<q->ordered){
  InterlockedIncrement64(&q->clock_lost);InterlockedIncrement64(&q->loss);goto done;
 }
 if((uint64_t)now.QuadPart>=q->end){q->stop=1;q->enabled=0;q->stop_reason=1;
  InterlockedIncrement64(&q->stopping);InterlockedIncrement64(&q->loss);goto done;}
 if(q->accepted>=q->max_rows){q->stop=1;q->enabled=0;q->stop_reason=2;goto done;}
 q->ordered=(uint64_t)now.QuadPart;
 BootNwEvent*e=&q->ring[(q->head+q->count)%BOOT_NW_CAPACITY];
 *e=(BootNwEvent){++q->accepted,(uint64_t)now.QuadPart,boot_nw_context.op,boot_nw_context.parent,boot_nw_context.epoch,
  (uint64_t)InterlockedCompareExchange64(&q->loss,0,0),begin,end,a,b,c,d,
  q->pid,boot_nw_context.tid,boot_nw_context.kind,stage,mask};
 ++q->count;
 if(q->accepted==q->max_rows){q->stop=1;q->enabled=0;q->stop_reason=2;}
done:ReleaseSRWLockExclusive(&q->lock);SetLastError(error);
}
static inline void boot_nw_note(unsigned stage,unsigned mask,uint64_t a,uint64_t b,uint64_t c,uint64_t d){
 boot_nw_emit(stage,mask,0,0,a,b,c,d);
}
static inline void boot_nw_span(unsigned stage,int64_t begin,int64_t end){
 if(begin>=0 && end>=begin)boot_nw_emit(stage,BOOT_NW_OBS,(uint64_t)begin,(uint64_t)end,0,0,0,0);
 else if(boot_nw_on() && boot_nw_context.op){InterlockedIncrement64(&boot_nw.clock_lost);InterlockedIncrement64(&boot_nw.loss);}
}
static inline BootNwContext boot_nw_enter(unsigned kind,uint64_t key,uint32_t flags){
 BootNwContext previous=boot_nw_context;DWORD error=GetLastError();
 if(!boot_nw_epoch){boot_nw_epoch=(uint64_t)InterlockedIncrement64(&boot_nw.epochs);
  if(!boot_nw_epoch){InterlockedExchange(&boot_nw.enabled,0);InterlockedExchange(&boot_nw.stop,1);InterlockedExchange(&boot_nw.stop_reason,5);boot_nw_next=UINT64_MAX;}}
 if(boot_nw_next==UINT64_MAX){boot_nw_context=(BootNwContext){0};
  InterlockedIncrement64(&boot_nw.context_lost);InterlockedIncrement64(&boot_nw.loss);SetLastError(error);return previous;}
 boot_nw_context=(BootNwContext){++boot_nw_next,previous.op,boot_nw_epoch,kind,GetCurrentThreadId()};
 boot_nw_note(BOOT_NW_OP_BEGIN,BOOT_NW_A|BOOT_NW_B,key,flags,0,0);SetLastError(error);return previous;
}
static inline void boot_nw_leave(BootNwContext previous,int rc,unsigned status){
 boot_nw_note(BOOT_NW_OP_END,BOOT_NW_A|BOOT_NW_B,(uint64_t)(int64_t)rc,status,0,0);
 boot_nw_context=previous;
}
static inline int boot_nw_result(unsigned stage,int value,uint64_t why){
 boot_nw_note(stage,BOOT_NW_A|BOOT_NW_B,(uint64_t)(int64_t)value,why,0,0);return value;
}
static inline uint64_t boot_nw_value(unsigned stage,uint64_t value,uint64_t source,uint64_t target){
 boot_nw_note(stage,BOOT_NW_A|BOOT_NW_B|BOOT_NW_C,value,source,target,0);return value;
}
static inline int boot_nw_guard(unsigned guard,int condition){return boot_nw_result(BOOT_NW_GUARD,condition,guard);}
static inline LONG boot_nw_hresult(unsigned stage,LONG result){
 boot_nw_note(stage,BOOT_NW_A,(uint32_t)result,0,0,0);return result;
}
/* Worker-only transport. It drains accepted records, never calls renderer/GPU/guest code. */
static inline int boot_nw_write(HANDLE file,const char*data,DWORD size){
 while(size){DWORD done=0;if(!WriteFile(file,data,size,&done,NULL) || !done)return 0;data+=done;size-=done;}return 1;
}
static inline unsigned boot_nw_take(void){
 BootNwTrace*q=&boot_nw;AcquireSRWLockExclusive(&q->lock);
 unsigned n=q->count<BOOT_NW_BATCH?q->count:BOOT_NW_BATCH;
 for(unsigned i=0;i<n;++i)q->batch[i]=q->ring[(q->head+i)%BOOT_NW_CAPACITY];
 q->head=(q->head+n)%BOOT_NW_CAPACITY;q->count-=n;ReleaseSRWLockExclusive(&q->lock);return n;
}
static inline int boot_nw_emit_batch(HANDLE file,unsigned n){
 BootNwTrace*q=&boot_nw;size_t used=0;
 for(unsigned i=0;i<n;++i){BootNwEvent*e=&q->batch[i];int k=snprintf(q->csv+used,sizeof q->csv-used,
  "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%lu,%lu,%u,%u,%u,%llu,%llu,%llu,%llu\n",
  (unsigned long long)e->seq,(unsigned long long)e->qpc,(unsigned long long)e->op,(unsigned long long)e->parent,
  (unsigned long long)e->epoch,(unsigned long long)e->loss,(unsigned long long)e->observed_begin,(unsigned long long)e->observed_end,
  (unsigned long)e->pid,(unsigned long)e->tid,e->kind,e->stage,e->mask,
  (unsigned long long)e->a,(unsigned long long)e->b,(unsigned long long)e->c,(unsigned long long)e->d);
  if(k<0 || (size_t)k>=sizeof q->csv-used){SetLastError(ERROR_INSUFFICIENT_BUFFER);return 0;}used+=(size_t)k;
 }
 if(used && !boot_nw_write(file,q->csv,(DWORD)used))return 0;q->written+=n;return 1;
}
static inline int boot_nw_finish_receipt(int drained){
 BootNwTrace*q=&boot_nw;char temporary[sizeof q->receipt+5];DWORD error=0;
 int path_size=snprintf(temporary,sizeof temporary,"%s.tmp",q->receipt);
 if(path_size<0 || (size_t)path_size>=sizeof temporary){error=ERROR_INSUFFICIENT_BUFFER;goto failed;}
 HANDLE file=CreateFileA(temporary,GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
 if(file==INVALID_HANDLE_VALUE){error=GetLastError();goto failed;}
 int n=snprintf(q->csv,sizeof q->csv,
  "{\"schema\":80,\"pid\":%lu,\"creation_filetime\":%llu,\"capture_filetime\":%llu,\"start_qpc\":%llu,\"end_qpc\":%llu,"
  "\"accepted\":%llu,\"written\":%llu,\"attempted_snapshot\":%llu,\"full_snapshot\":%llu,\"contended_snapshot\":%llu,"
  "\"stopping_snapshot\":%llu,\"clock_lost_snapshot\":%llu,\"context_lost_snapshot\":%llu,\"loss_snapshot\":%llu,"
  "\"stop_reason\":%ld,\"io_error\":%ld,\"writer_drained_accepted_ring\":%s,\"capture_complete_proven\":false,"
  "\"scope\":\"Bounded accepted observations; counters are a final writer snapshot, not all call intentions. No GPU completion or global hook completeness proof.\"}\n",
  (unsigned long)q->pid,(unsigned long long)q->creation,(unsigned long long)q->filetime,
  (unsigned long long)q->start,(unsigned long long)q->end,(unsigned long long)q->accepted,(unsigned long long)q->written,
  (unsigned long long)InterlockedCompareExchange64(&q->attempted,0,0),(unsigned long long)InterlockedCompareExchange64(&q->full,0,0),
  (unsigned long long)InterlockedCompareExchange64(&q->contended,0,0),(unsigned long long)InterlockedCompareExchange64(&q->stopping,0,0),
  (unsigned long long)InterlockedCompareExchange64(&q->clock_lost,0,0),(unsigned long long)InterlockedCompareExchange64(&q->context_lost,0,0),
  (unsigned long long)InterlockedCompareExchange64(&q->loss,0,0),(long)q->stop_reason,(long)q->io_error,drained?"true":"false");
 int ok=n>=0 && (size_t)n<sizeof q->csv;
 if(!ok)error=ERROR_INSUFFICIENT_BUFFER;
 if(ok && !boot_nw_write(file,q->csv,(DWORD)n)){error=GetLastError();ok=0;}
 if(ok && !FlushFileBuffers(file)){error=GetLastError();ok=0;}
 if(!CloseHandle(file)){if(ok)error=GetLastError();ok=0;}
 if(!ok)goto failed;
 /* No replace flag: a previous output is never overwritten. A final receipt
  * exists only after its entire temporary file wrote, flushed and closed. */
 if(!MoveFileExA(temporary,q->receipt,MOVEFILE_WRITE_THROUGH)){error=GetLastError();goto failed;}
 return 1;
failed:
 if(!error)error=ERROR_WRITE_FAULT;
 InterlockedCompareExchange(&q->io_error,(LONG)error,0);InterlockedExchange(&q->stop_reason,4);
 fprintf(stderr,"nvn wait80: receipt incomplete/unpublished (error %lu)\n",(unsigned long)error);return 0;
}
static DWORD WINAPI boot_nw_worker(void*unused){
 (void)unused;BootNwTrace*q=&boot_nw;CreateDirectoryA("frames",NULL);
 HANDLE file=CreateFileA(q->path,GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
 if(file==INVALID_HANDLE_VALUE){InterlockedExchange(&q->io_error,(LONG)GetLastError());
  fprintf(stderr,"nvn wait80: file creation failed %lu\n",(unsigned long)q->io_error);return 1;}
 Sleep(q->delay_ms);LARGE_INTEGER now;
 if(!QueryPerformanceCounter(&now) || now.QuadPart<0){InterlockedIncrement64(&q->clock_lost);InterlockedIncrement64(&q->loss);SetLastError(ERROR_INVALID_DATA);goto failed;}
 q->start=(uint64_t)now.QuadPart;q->end=q->start+q->frequency*q->duration_ms/1000;q->ordered=q->start;
 int n=snprintf(q->csv,sizeof q->csv,
  "#trace80,version=1,pid=%lu,creation_filetime=%llu,frequency=%llu,filetime0=%llu,qpc0_low=%llu,qpc0_high=%llu,start_qpc=%llu,end_qpc=%llu,max_rows=%u,capacity=%u,delay_ms=%u,duration_ms=%u\n"
  "seq,qpc,op,parent,epoch,loss,observed_begin,observed_end,pid,tid,kind,stage,mask,a,b,c,d\n",
  (unsigned long)q->pid,(unsigned long long)q->creation,(unsigned long long)q->frequency,(unsigned long long)q->filetime,
  (unsigned long long)q->qpc_low,(unsigned long long)q->qpc_high,(unsigned long long)q->start,(unsigned long long)q->end,q->max_rows,BOOT_NW_CAPACITY,q->delay_ms,q->duration_ms);
 if(n<0 || (size_t)n>=sizeof q->csv || !boot_nw_write(file,q->csv,(DWORD)n))goto failed;
 fprintf(stderr,"nvn wait80: %s (%ums, max%u accepted rows; observation only)\n",q->path,q->duration_ms,q->max_rows);
 InterlockedExchange(&q->enabled,1);
 for(;;){
  int clock_failed=!QueryPerformanceCounter(&now) || now.QuadPart<0;
  if(clock_failed){InterlockedIncrement64(&q->clock_lost);InterlockedIncrement64(&q->loss);}
  if(clock_failed || (uint64_t)now.QuadPart>=q->end){
   AcquireSRWLockExclusive(&q->lock);if(clock_failed)q->stop_reason=3;else if(!q->stop_reason)q->stop_reason=1;q->stop=1;q->enabled=0;ReleaseSRWLockExclusive(&q->lock);
  }
  unsigned count=boot_nw_take();if(!boot_nw_emit_batch(file,count))goto failed;
  if(q->stop && !count)break;
  if(count)continue;
  Sleep(50);
 }
 if(!FlushFileBuffers(file))goto failed;
 if(!CloseHandle(file)){file=INVALID_HANDLE_VALUE;goto failed;}
 file=INVALID_HANDLE_VALUE;
 return boot_nw_finish_receipt(1)?0:1;
failed:
 {DWORD error=GetLastError();if(!error)error=ERROR_WRITE_FAULT;InterlockedExchange(&q->io_error,(LONG)error);
  AcquireSRWLockExclusive(&q->lock);q->enabled=0;q->stop=1;q->stop_reason=4;ReleaseSRWLockExclusive(&q->lock);
  fprintf(stderr,"nvn wait80: incomplete diagnostic write/clock error %lu\n",(unsigned long)error);}
 if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);
 (void)boot_nw_finish_receipt(0);return 1;
}
static inline int boot_nw_env_ms(const char*name,uint32_t fallback,uint32_t low,uint32_t high,uint32_t*out){
 char value[32];DWORD n=GetEnvironmentVariableA(name,value,sizeof value);if(!n){*out=fallback;return 1;}
 if(n>=sizeof value)return 0;char*end=NULL;unsigned long v=strtoul(value,&end,10);
 if(!*value || *end || v<low || v>high)return 0;*out=(uint32_t)v;return 1;
}
/* UI window-thread only before window-ready; not registration or a native producer. */
static inline void boot_nw_start(void){
 BootNwTrace*q=&boot_nw;char value[8];DWORD n=GetEnvironmentVariableA("TOTK_NVN_WAIT_TRACE",value,sizeof value);
 if(n!=1 || value[0]!='1')return;
 if(InterlockedCompareExchange(&q->initialized,1,0))return;
 if(!boot_nw_env_ms("TOTK_NVN_WAIT_TRACE_DELAY_MS",0,0,1800000,&q->delay_ms) ||
    !boot_nw_env_ms("TOTK_NVN_WAIT_TRACE_DURATION_MS",10000,1,15000,&q->duration_ms) ||
    !boot_nw_env_ms("TOTK_NVN_WAIT_TRACE_MAX_ROWS",16384,256,65536,&q->max_rows)){
  fprintf(stderr,"nvn wait80: invalid bounded trace settings; disabled\n");return;}
 LARGE_INTEGER f,a,b;FILETIME wall,creation,exit,kernel,user;
 if(!QueryPerformanceFrequency(&f) || f.QuadPart<=0 || (uint64_t)f.QuadPart>UINT64_MAX/1800000 ||
    !GetProcessTimes(GetCurrentProcess(),&creation,&exit,&kernel,&user) || !QueryPerformanceCounter(&a) || a.QuadPart<0)return;
 GetSystemTimePreciseAsFileTime(&wall);if(!QueryPerformanceCounter(&b) || b.QuadPart<a.QuadPart)return;
 q->frequency=(uint64_t)f.QuadPart;q->qpc_low=(uint64_t)a.QuadPart;q->qpc_high=(uint64_t)b.QuadPart;
 q->filetime=((uint64_t)wall.dwHighDateTime<<32)|wall.dwLowDateTime;
 q->creation=((uint64_t)creation.dwHighDateTime<<32)|creation.dwLowDateTime;q->pid=GetCurrentProcessId();
 snprintf(q->path,sizeof q->path,"frames/nvn-wait-qpc-%lu-%llu-%llu.csv",(unsigned long)q->pid,(unsigned long long)q->creation,(unsigned long long)q->filetime);
 snprintf(q->receipt,sizeof q->receipt,"%s.receipt.json",q->path);
 HANDLE thread=CreateThread(NULL,0,boot_nw_worker,NULL,0,NULL);
 if(!thread){InterlockedExchange(&q->io_error,(LONG)GetLastError());fprintf(stderr,"nvn wait80: worker creation failed\n");return;}
 SetThreadPriority(thread,THREAD_PRIORITY_BELOW_NORMAL);CloseHandle(thread);
}
#endif
