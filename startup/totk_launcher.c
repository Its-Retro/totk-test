/* totk.exe: the play launcher. Loads the game DLL directly and runs the game in this process.
 *
 * Replaces python.exe + live_service_supervisor.py + run_window_probe_live.py + run_startup.py for
 * playing: no probe protocol, no coverage loop, no budget stops, no checkpoints, no heartbeat files.
 * The setup is exactly what those scripts do (run_startup.Startup(with_tls=True) + reset(), the
 * expanded-lab memory profile, RomFS, window, focus fixture), see build_launcher.sh for checks.
 *
 *   totk.exe [game.dll]       default: TOTK_STARTUP_DLL, else totk-play.dll next to totk.exe
 *
 * Paths: the repo root is TOTK_ROOT, else the parent of the directory holding totk.exe (startup\).
 * The process runs with the repo root as working directory (save data, logs, frames as today).
 * Defaults set when not already in the environment (the user's boot environment):
 *   TOTK_DRAW_PATH=1 TOTK_MEMORY_PROFILE=expanded-lab TOTK_FASTMEM=1 TOTK_NVN=native TOTK_CORES=1
 *   TOTK_SHADER_MANIFEST=<root>\startup\shader-manifest.tsv
 * Debug features of the runtime stay opt-in through their own environment variables.
 * Exit: 0 when the window was closed; otherwise the stop is printed and written to totk-stop.txt. */
#include "boot_runtime.h"
#include <bcrypt.h>
#include <stdio.h>
#include <wchar.h>

typedef uint32_t (*RunFn)(BootContext*,uint64_t);
typedef uint64_t (*SizeFn)(void);
typedef void (*CtxFn)(BootContext*);
typedef int (*WindowFn)(BootContext*,int);
typedef int (*RomfsFn)(BootContext*,const wchar_t*);
typedef uint64_t (*HandleFn)(BootContext*);
typedef int (*FocusFn)(BootContext*,uint32_t);

static const char*const module_names[4]={"rtld","main","subsdk0","sdk"};
#define MODULE_BASE(i) (UINT64_C(0x7100000000)+(uint64_t)(i)*UINT64_C(0x10000000))
#define STACK_BASE UINT64_C(0x7400100000)
#define STACK_SIZE UINT64_C(0x100000)
#define TLS_BASE UINT64_C(0x7400200000)
#define TLS_SIZE UINT64_C(4096)
#define THREAD_HANDLE UINT64_C(0x1234)

static wchar_t root[MAX_PATH];
static BootContext*ctx;
static void fail(const char*what){fprintf(stderr,"totk: %s (error %lu)\n",what,GetLastError());exit(2);}
static void env_default(const wchar_t*name,const wchar_t*value){
    /* both the Win32 block and the C runtime copy: the game DLL reads some settings with getenv(), which
     * does not see SetEnvironmentVariableW changes made after the CRT started (TOTK_NVN was silently ignored) */
    wchar_t cur[MAX_PATH];DWORD n=GetEnvironmentVariableW(name,cur,MAX_PATH);
    if(!n){SetEnvironmentVariableW(name,value);_wputenv_s(name,value);}
    else if(n<MAX_PATH)_wputenv_s(name,cur);
}
/* call-chain/module_loader.py Module: an uncompressed NSO placed by its three segment headers (text r-x,
 * rodata r--, data+bss rw-), each page-aligned. The image stays owned by this process for its lifetime. */
static void load_module(unsigned index){
    wchar_t path[MAX_PATH];swprintf(path,MAX_PATH,L"%ls\\uncompressed\\%hs",root,module_names[index]);
    HANDLE f=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
    if(f==INVALID_HANDLE_VALUE)fail("cannot open an uncompressed module (uncompressed\\rtld|main|subsdk0|sdk)");
    LARGE_INTEGER size;GetFileSizeEx(f,&size);
    uint8_t*raw=malloc((size_t)size.QuadPart);DWORD got=0;
    if(!raw || !ReadFile(f,raw,(DWORD)size.QuadPart,&got,NULL) || got!=(DWORD)size.QuadPart)fail("cannot read a module");
    CloseHandle(f);
    if(size.QuadPart<256 || memcmp(raw,"NSO0",4))fail("invalid NSO header");
    uint32_t flags;memcpy(&flags,raw+12,4);if(flags&7)fail("module must be decompressed");
    uint32_t seg[3][4];uint64_t end=0,allocated[3];
    for(unsigned s=0;s<3;++s){memcpy(seg[s],raw+16+16*s,16);
        uint64_t extra=s==2?seg[2][3]:0;allocated[s]=(seg[s][2]+extra+4095)&~UINT64_C(4095);
        if((uint64_t)seg[s][0]+seg[s][2]>(uint64_t)size.QuadPart || (seg[s][1]&4095) || seg[s][1]<end)fail("bad NSO segment");
        end=seg[s][1]+allocated[s];}
    uint8_t*image=VirtualAlloc(NULL,(SIZE_T)end,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    if(!image)fail("module allocation");
    static const uint32_t permissions[3]={5,1,3};
    for(unsigned s=0;s<3;++s){
        memcpy(image+seg[s][1],raw+seg[s][0],seg[s][2]);
        Region*r=&ctx->regions[ctx->region_count];
        r->base=MODULE_BASE(index)+seg[s][1];r->size=allocated[s];r->data=image+seg[s][1];r->permissions=permissions[s];
        ctx->memory_types[ctx->region_count]=permissions[s]&2?4:3;++ctx->region_count;
    }
    free(raw);
    ctx->code_bases[index]=MODULE_BASE(index);ctx->code_sizes[index]=end;
    if(!index)ctx->rtld_base=MODULE_BASE(0);
}
static void add_region(uint64_t base,uint64_t size,uint32_t type,uint8_t fill){
    uint8_t*p=VirtualAlloc(NULL,(SIZE_T)size,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);if(!p)fail("region allocation");
    memset(p,fill,(size_t)size);
    Region*r=&ctx->regions[ctx->region_count];r->base=base;r->size=size;r->data=p;r->permissions=3;
    ctx->memory_types[ctx->region_count]=type;++ctx->region_count;
}
/* Window closed: ask translated code to stop (run token, play_config.h); a game that does not stop
 * within 3 s (e.g. several cores in guest code) is ended. */
static HANDLE main_done;
static DWORD WINAPI window_watch(void*hwnd){
    while(IsWindow((HWND)hwnd))Sleep(200);
    __atomic_store_n(&ctx->limit,0,__ATOMIC_SEQ_CST);
    if(WaitForSingleObject(main_done,3000)==WAIT_TIMEOUT){fprintf(stderr,"totk: window closed\n");ExitProcess(0);}
    return 0;
}
/* Process-scoped timer precision: 1 ms system timer resolution while the game runs (Windows otherwise
 * rounds sleeps and timed waits up to ~15.6 ms), and opt this process out of timer-resolution coarsening
 * by power throttling (EcoQoS). Both are per process and revert when it exits. */
static void timer_precision(void){
    HMODULE winmm=LoadLibraryW(L"winmm.dll");
    typedef unsigned(WINAPI*TbpFn)(unsigned);TbpFn tbp=winmm?(TbpFn)(void*)GetProcAddress(winmm,"timeBeginPeriod"):NULL;
    if(tbp)tbp(1);
    PROCESS_POWER_THROTTLING_STATE s;memset(&s,0,sizeof s);s.Version=PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    s.ControlMask=PROCESS_POWER_THROTTLING_EXECUTION_SPEED|PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;s.StateMask=0;
    SetProcessInformation(GetCurrentProcess(),ProcessPowerThrottling,&s,sizeof s);
}
int wmain(int argc,wchar_t**argv){
    wchar_t exe[MAX_PATH],dll[MAX_PATH];GetModuleFileNameW(NULL,exe,MAX_PATH);
    if(!GetEnvironmentVariableW(L"TOTK_ROOT",root,MAX_PATH)){
        wcscpy(root,exe);for(int k=0;k<2;++k){wchar_t*s=wcsrchr(root,L'\\');if(s)*s=0;}
    }
    SetCurrentDirectoryW(root);timer_precision();
    if(argc>1)wcsncpy(dll,argv[1],MAX_PATH-1),dll[MAX_PATH-1]=0;
    else if(!GetEnvironmentVariableW(L"TOTK_STARTUP_DLL",dll,MAX_PATH)){wcscpy(dll,exe);wchar_t*s=wcsrchr(dll,L'\\');wcscpy(s?s+1:dll,L"totk-play.dll");}
    wchar_t manifest[MAX_PATH];swprintf(manifest,MAX_PATH,L"%ls\\startup\\shader-manifest.tsv",root);
    env_default(L"TOTK_DRAW_PATH",L"1");env_default(L"TOTK_MEMORY_PROFILE",L"expanded-lab");env_default(L"TOTK_FASTMEM",L"1");
    env_default(L"TOTK_NVN",L"native");env_default(L"TOTK_CORES",L"1");env_default(L"TOTK_SHADER_MANIFEST",manifest);
    HMODULE game=LoadLibraryW(dll);if(!game){fwprintf(stderr,L"totk: cannot load %ls\n",dll);return 2;}
    RunFn run=(RunFn)(void*)GetProcAddress(game,"boot_run"),resume=(RunFn)(void*)GetProcAddress(game,"boot_resume");
    SizeFn size=(SizeFn)(void*)GetProcAddress(game,"boot_context_size");
    CtxFn configure=(CtxFn)(void*)GetProcAddress(game,"boot_configure_layout"),release=(CtxFn)(void*)GetProcAddress(game,"boot_free_owned_memory");
    WindowFn window=(WindowFn)(void*)GetProcAddress(game,"boot_open_host_window");RomfsFn romfs=(RomfsFn)(void*)GetProcAddress(game,"boot_open_romfs");
    HandleFn handle=(HandleFn)(void*)GetProcAddress(game,"boot_host_window_handle");FocusFn focus=(FocusFn)(void*)GetProcAddress(game,"boot_test_window_focus");
    if(!run || !resume || !size || !configure || !release || !window || !romfs || !handle || !focus)fail("game DLL lacks an export");
    if(size()!=sizeof(BootContext)){fprintf(stderr,"totk: BootContext ABI mismatch (DLL %llu, launcher %llu): rebuild totk.exe\n",(unsigned long long)size(),(unsigned long long)sizeof(BootContext));return 2;}
    ctx=VirtualAlloc(NULL,sizeof(BootContext),MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);if(!ctx)fail("context allocation");
    /* run_startup.Startup.__init__(with_tls=True) */
    for(unsigned i=0;i<4;++i)load_module(i);
    add_region(STACK_BASE,STACK_SIZE,0xb,0xa6);
    add_region(TLS_BASE,TLS_SIZE,12,0);
    ctx->tpidrro=TLS_BASE;
    BCryptGenRandom(NULL,(PUCHAR)ctx->entropy,sizeof ctx->entropy,BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    ctx->main_thread_id=1;ctx->main_thread_priority=44;ctx->thread_enabled=1;ctx->clock_mode=1;
    ctx->address_space_end=UINT64_C(1)<<39;ctx->return_sentinel=UINT64_MAX;
    configure(ctx);
    /* Startup.reset() on a fresh context (counters and tables are already zero) */
    release(ctx);
    ctx->x[1]=THREAD_HANDLE;ctx->main_thread_handle=(uint32_t)THREAD_HANDLE;ctx->tpidr=0;
    ctx->x[30]=ctx->return_sentinel;ctx->sp=STACK_BASE+STACK_SIZE;
    /* run_window_probe_live.py */
    char profile[32]="";GetEnvironmentVariableA("TOTK_MEMORY_PROFILE",profile,sizeof profile);
    if(!strcmp(profile,"expanded-lab")){ctx->memory_budget=UINT64_C(0xe0000000);ctx->layout_info[5]=UINT64_C(0xc0000000);}
    else if(strcmp(profile,"baseline")){fprintf(stderr,"totk: unknown TOTK_MEMORY_PROFILE %s\n",profile);return 2;}
    ctx->limit=UINT64_MAX;   /* play build: a run token, never a budget */
    ctx->query_enabled=ctx->terminal_query_enabled=ctx->info_enabled=ctx->layout_enabled=ctx->ipc_enabled=1;
    wchar_t rom[MAX_PATH];swprintf(rom,MAX_PATH,L"%ls\\assets\\romfs.bin",root);
    if(!romfs(ctx,rom))fail("cannot open assets\\romfs.bin");
    if(!window(ctx,1))fail("cannot open the game window");
    if(!focus(ctx,1))fail("cannot set the window focus state");
    main_done=CreateEventW(NULL,TRUE,FALSE,NULL);
    HWND hwnd=(HWND)(uintptr_t)handle(ctx);
    if(hwnd){ShowWindow(hwnd,SW_SHOW);SetForegroundWindow(hwnd);CloseHandle(CreateThread(NULL,0,window_watch,hwnd,0,NULL));}
    uint32_t status=run(ctx,0);
    /* CH_BUDGET is a stop request (run token cleared), resumable at fault_pc: continue unless the window is gone. */
    while(status==CH_BUDGET && ctx->fault_pc && (!hwnd || IsWindow(hwnd))){ctx->limit=UINT64_MAX;status=resume(ctx,ctx->fault_pc);}
    SetEvent(main_done);
    if(status==BOOT_WINDOW_CLOSED || (hwnd && !IsWindow(hwnd)))return 0;
    uint64_t pc=ctx->fault_pc;unsigned owner=4;
    for(unsigned i=0;i<4;++i)if(pc>=ctx->code_bases[i] && pc-ctx->code_bases[i]<ctx->code_sizes[i])owner=i;
    char line[512];
    snprintf(line,sizeof line,"stop status %u svc 0x%x pc 0x%llx (%s+0x%llx) address 0x%llx width %llu thread %u\n",status,ctx->svc_id,(unsigned long long)pc,
             owner<4?module_names[owner]:"?",(unsigned long long)(owner<4?pc-ctx->code_bases[owner]:pc),(unsigned long long)ctx->fault_address,
             (unsigned long long)ctx->fault_width,ctx->current_thread);
    fputs(line,stderr);
    FILE*f=fopen("totk-stop.txt","a");if(f){fputs(line,f);fclose(f);}
    return 1;
}
