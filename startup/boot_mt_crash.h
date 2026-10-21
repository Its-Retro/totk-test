/* Multi-core diagnostics (TOTK_CORES>1 only; included by boot_mt_late.h). Nothing here runs in
 * single-core execution.
 *  - mt-status.txt (overwritten about once a second by the thread waiting in boot_mt_execute): per-worker
 *    guest thread, resume pc, last traced guest pc, steps, flags; kernel owner; master thread table summary.
 *    The last status survives deaths that bypass every handler (__fastfail, a killed process).
 *  - mt-crash.txt (appended) + mt-crash-<pid>.dmp: a vectored handler registered last records every
 *    severe first-chance exception nobody handled before it (fastmem's handler runs first), at most 32;
 *    the unhandled-exception filter writes "UNHANDLED", the same per-core state and a minidump (written
 *    by a helper thread, so a stack overflow still gets one).
 * Directory: TOTK_MT_CRASH_DIR, else the process working directory. TOTK_MT_CRASHLOG=0 disables. */
#ifndef TOTK_BOOT_MT_CRASH_H
#define TOTK_BOOT_MT_CRASH_H
/* (no includes: stdio comes with host_fastmem.h; keeps the generated page header unchanged) */

typedef BOOL (WINAPI *BootMtMiniDumpWriteDump)(HANDLE,DWORD,HANDLE,int,void*,void*,void*);
typedef struct BootMtCrash {
    volatile LONG installed,records,writing;
    PVOID veh;LPTOP_LEVEL_EXCEPTION_FILTER previous;
    HANDLE request,done,writer;
    EXCEPTION_POINTERS*pending;DWORD pending_tid;const char*pending_kind;
    char dir[MAX_PATH];
} BootMtCrash;
__declspec(selectany) BootMtCrash boot_mt_crash={0};

static inline void boot_mt_crash_path(char*out,size_t cap,const char*name){
    snprintf(out,cap,"%s%s%s",boot_mt_crash.dir,boot_mt_crash.dir[0]?"\\":"",name);
}
static inline void boot_mt_crash_append(HANDLE f,const char*fmt,...){
    char line[1024];__builtin_va_list a;__builtin_va_start(a,fmt);int n=vsnprintf(line,sizeof line,fmt,a);__builtin_va_end(a);
    if(n<0)return;if(n>(int)sizeof line-1)n=(int)sizeof line-1;
    DWORD w;WriteFile(f,line,(DWORD)n,&w,NULL);
}
static inline void boot_mt_crash_module(uint64_t address,char*out,size_t cap){
    HMODULE m=NULL;char name[MAX_PATH]={0};
    if(GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)(uintptr_t)address,&m) && m &&
       GetModuleFileNameA(m,name,sizeof name)){
        const char*base=strrchr(name,'\\');snprintf(out,cap,"%s+%#llx",base?base+1:name,(unsigned long long)(address-(uint64_t)(uintptr_t)m));
    }else snprintf(out,cap,"%#llx",(unsigned long long)address);
}
/* Per-core and kernel state (read racily: it is a diagnostic). */
static inline void boot_mt_crash_state(HANDLE f){
    BootContext*M=boot_mt.master;
    boot_mt_crash_append(f,"mt: active=%ld workers=%u kernel_core=%d stop=%ld stop_set=%ld stop_status=%ld starts=%llu svcs=%llu services=%llu kicks=%llu idle_waits=%llu\r\n",
        boot_mt.active,boot_mt.workers,(int)boot_mt.kernel_core,boot_mt.stop,boot_mt.stop_set,boot_mt.stop_status,
        (unsigned long long)boot_mt.starts,(unsigned long long)boot_mt.svcs,(unsigned long long)boot_mt.services,(unsigned long long)boot_mt.kicks,(unsigned long long)boot_mt.idle_waits);
    for(unsigned w=0;w<boot_mt.workers && w<BOOT_MT_MAX;++w){BootCore*k=boot_mt.core[w];if(!k)continue;
        const BootContext*K=&k->ctx;uint64_t s=K->steps;
        uint64_t last=K->trace[s%BOOT_TRACE_CAPACITY],prev=K->trace[(s+BOOT_TRACE_CAPACITY-1)%BOOT_TRACE_CAPACITY];
        boot_mt_crash_append(f,"core %u: tid=%lu vcore=%u thread=%u pc=%#llx trace=%#llx,%#llx fm_pc=%#llx steps=%llu limit=%llu quota_end=%llu status=%u fault_pc=%#llx svc=%#x in_guest=%ld idle=%ld interrupt=%ld x30=%#llx sp=%#llx\r\n",
            w,(unsigned long)boot_mt.core_tid[w],k->vcore,k->thread,(unsigned long long)k->pc,(unsigned long long)last,(unsigned long long)prev,
            (unsigned long long)k->hot.fm_pc,(unsigned long long)s,(unsigned long long)K->limit,(unsigned long long)k->quota_end,K->status,
            (unsigned long long)K->fault_pc,K->svc_id,k->in_guest,k->idle,k->interrupt,(unsigned long long)K->x[30],(unsigned long long)K->sp);
    }
    if(M){
        boot_mt_crash_append(f,"master: current=%u threads=%u steps=%llu status=%u svc=%#x regions=%u\r\n",M->current_thread,M->thread_count,
            (unsigned long long)M->steps,M->status,M->svc_id,M->region_count);
        for(unsigned i=0;i<M->thread_count && i<BOOT_THREAD_CAPACITY;++i){const BootThread*t=&M->threads[i];
            boot_mt_crash_append(f,"  t%u h=%#x prio=%u state=%u core=%d mask=%#llx active=%u running=%u pc=%#llx waddr=%#llx\r\n",i,t->handle,t->priority,t->state,
                (int)t->core,(unsigned long long)t->core_mask,t->schedule.active,boot_mt.running[i],(unsigned long long)t->pc,(unsigned long long)t->wait_address);
        }
    }
}
static inline int boot_mt_crash_worker(DWORD tid){
    for(unsigned w=0;w<boot_mt.workers && w<BOOT_MT_MAX;++w)if(boot_mt.core_tid[w]==tid)return (int)w;
    return -1;
}
static inline void boot_mt_crash_record(EXCEPTION_POINTERS*e,DWORD tid,const char*kind){
    char path[MAX_PATH+32];boot_mt_crash_path(path,sizeof path,"mt-crash.txt");
    HANDLE f=CreateFileA(path,FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if(f==INVALID_HANDLE_VALUE)return;
    SYSTEMTIME st;GetLocalTime(&st);
    boot_mt_crash_append(f,"==== %04u-%02u-%02u %02u:%02u:%02u.%03u pid=%lu tid=%lu worker=%d %s\r\n",st.wYear,st.wMonth,st.wDay,st.wHour,st.wMinute,st.wSecond,st.wMilliseconds,
        (unsigned long)GetCurrentProcessId(),(unsigned long)tid,boot_mt_crash_worker(tid),kind);
    if(e && e->ExceptionRecord){
        const EXCEPTION_RECORD*r=e->ExceptionRecord;char where[MAX_PATH+40];boot_mt_crash_module((uint64_t)(uintptr_t)r->ExceptionAddress,where,sizeof where);
        boot_mt_crash_append(f,"code=%#lx address=%s params=%lu [%#llx %#llx %#llx]\r\n",(unsigned long)r->ExceptionCode,where,(unsigned long)r->NumberParameters,
            (unsigned long long)(r->NumberParameters>0?r->ExceptionInformation[0]:0),(unsigned long long)(r->NumberParameters>1?r->ExceptionInformation[1]:0),
            (unsigned long long)(r->NumberParameters>2?r->ExceptionInformation[2]:0));
    }
    if(e && e->ContextRecord){const CONTEXT*x=e->ContextRecord;char ret[MAX_PATH+40];
        uint64_t top=0;if(x->Rsp && !IsBadReadPtr((void*)(uintptr_t)x->Rsp,8))top=*(const uint64_t*)(uintptr_t)x->Rsp;
        boot_mt_crash_module(top,ret,sizeof ret);
        boot_mt_crash_append(f,"rip=%#llx rsp=%#llx [rsp]=%s rax=%#llx rbx=%#llx rcx=%#llx rdx=%#llx rsi=%#llx rdi=%#llx r8=%#llx r9=%#llx r12=%#llx r13=%#llx r14=%#llx r15=%#llx rbp=%#llx\r\n",
            (unsigned long long)x->Rip,(unsigned long long)x->Rsp,ret,(unsigned long long)x->Rax,(unsigned long long)x->Rbx,(unsigned long long)x->Rcx,(unsigned long long)x->Rdx,
            (unsigned long long)x->Rsi,(unsigned long long)x->Rdi,(unsigned long long)x->R8,(unsigned long long)x->R9,(unsigned long long)x->R12,(unsigned long long)x->R13,
            (unsigned long long)x->R14,(unsigned long long)x->R15,(unsigned long long)x->Rbp);
    }
    boot_mt_crash_state(f);
    FlushFileBuffers(f);CloseHandle(f);
}
static DWORD WINAPI boot_mt_crash_writer(void*arg){
    (void)arg;
    for(;;){
        WaitForSingleObject(boot_mt_crash.request,INFINITE);
        boot_mt_crash_record(boot_mt_crash.pending,boot_mt_crash.pending_tid,boot_mt_crash.pending_kind);
        HMODULE dbg=LoadLibraryA("dbghelp.dll");
        BootMtMiniDumpWriteDump dump=dbg?(BootMtMiniDumpWriteDump)(void(*)(void))GetProcAddress(dbg,"MiniDumpWriteDump"):NULL;
        if(dump && boot_mt_crash.pending){
            char path[MAX_PATH+40],name[64];snprintf(name,sizeof name,"mt-crash-%lu.dmp",(unsigned long)GetCurrentProcessId());boot_mt_crash_path(path,sizeof path,name);
            HANDLE f=CreateFileA(path,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
            if(f!=INVALID_HANDLE_VALUE){
#pragma pack(push,4)
                struct { DWORD tid; EXCEPTION_POINTERS*e; BOOL client; } info={boot_mt_crash.pending_tid,boot_mt_crash.pending,FALSE};   /* MINIDUMP_EXCEPTION_INFORMATION */
#pragma pack(pop)
                /* MiniDumpWithThreadInfo|WithIndirectlyReferencedMemory|WithDataSegs: stacks and registers, no guest memory */
                dump(GetCurrentProcess(),GetCurrentProcessId(),f,0x1000|0x40|0x1,boot_mt_crash.pending?&info:NULL,NULL,NULL);
                CloseHandle(f);
            }
        }
        SetEvent(boot_mt_crash.done);
    }
    return 0;
}
static inline int boot_mt_crash_severe(DWORD code){
    switch(code){
    case EXCEPTION_ACCESS_VIOLATION:case EXCEPTION_ILLEGAL_INSTRUCTION:case EXCEPTION_STACK_OVERFLOW:case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_PRIV_INSTRUCTION:case EXCEPTION_IN_PAGE_ERROR:case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:case EXCEPTION_DATATYPE_MISALIGNMENT:
    case EXCEPTION_NONCONTINUABLE_EXCEPTION:case EXCEPTION_INVALID_DISPOSITION:case 0xC0000409u:case EXCEPTION_BREAKPOINT:case 0xC0000374u:
        return 1;
    }
    return 0;
}
/* Registered last: sees exceptions no earlier vectored handler (fastmem) resolved. Records, never handles. */
static LONG CALLBACK boot_mt_crash_veh(EXCEPTION_POINTERS*e){
    if(!boot_mt.active || !e || !e->ExceptionRecord || !boot_mt_crash_severe(e->ExceptionRecord->ExceptionCode))return EXCEPTION_CONTINUE_SEARCH;
    if(InterlockedIncrement(&boot_mt_crash.records)<=32)
        boot_mt_crash_record(e,GetCurrentThreadId(),e->ExceptionRecord->ExceptionCode==EXCEPTION_STACK_OVERFLOW?"first-chance STACK OVERFLOW":"first-chance");
    return EXCEPTION_CONTINUE_SEARCH;
}
static LONG WINAPI boot_mt_crash_filter(EXCEPTION_POINTERS*e){
    if(InterlockedCompareExchange(&boot_mt_crash.writing,1,0)==0){
        boot_mt_crash.pending=e;boot_mt_crash.pending_tid=GetCurrentThreadId();boot_mt_crash.pending_kind="UNHANDLED";
        SetEvent(boot_mt_crash.request);WaitForSingleObject(boot_mt_crash.done,15000);
    }
    return boot_mt_crash.previous?boot_mt_crash.previous(e):EXCEPTION_CONTINUE_SEARCH;
}
static inline void boot_mt_crash_install(void){
    if(InterlockedCompareExchange(&boot_mt_crash.installed,1,0))return;
    const char*off=getenv("TOTK_MT_CRASHLOG");if(off && off[0]=='0')return;
    const char*dir=getenv("TOTK_MT_CRASH_DIR");snprintf(boot_mt_crash.dir,sizeof boot_mt_crash.dir,"%s",dir?dir:"");
    boot_mt_crash.request=CreateEventW(NULL,FALSE,FALSE,NULL);boot_mt_crash.done=CreateEventW(NULL,FALSE,FALSE,NULL);
    boot_mt_crash.writer=CreateThread(NULL,1u<<20,boot_mt_crash_writer,NULL,0,NULL);
    boot_mt_crash.veh=AddVectoredExceptionHandler(0,boot_mt_crash_veh);
    boot_mt_crash.previous=SetUnhandledExceptionFilter(boot_mt_crash_filter);
}
/* Status snapshot, overwritten (called about once a second while workers run). */
static inline void boot_mt_status_write(void){
    if(!boot_mt_crash.installed)return;
    char path[MAX_PATH+32],tmp[MAX_PATH+40];boot_mt_crash_path(path,sizeof path,"mt-status.txt");snprintf(tmp,sizeof tmp,"%s.tmp",path);
    HANDLE f=CreateFileA(tmp,GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if(f==INVALID_HANDLE_VALUE)return;
    SYSTEMTIME st;GetLocalTime(&st);
    boot_mt_crash_append(f,"%04u-%02u-%02u %02u:%02u:%02u.%03u pid=%lu\r\n",st.wYear,st.wMonth,st.wDay,st.wHour,st.wMinute,st.wSecond,st.wMilliseconds,(unsigned long)GetCurrentProcessId());
    boot_mt_crash_state(f);CloseHandle(f);
    MoveFileExA(tmp,path,MOVEFILE_REPLACE_EXISTING);
}
#endif
