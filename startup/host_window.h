#ifndef TOTK_HOST_WINDOW_H
#define TOTK_HOST_WINDOW_H
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include "host_limits.h"
#include "host_vsync.h"
#include "host_gpu_va.h"
#include "host_nvmap.h"
#include "host_gpu_mapping.h"
#include "host_gpu_channel.h"
#include "host_gpu_command_stage.h"
#include "host_gpu_sync_memory.h"
#include "host_buffer_queue.h"
#include "host_graphic_registry.h"
#include "host_account_init.h"
#include "host_hid_memory.h"
#include "host_gpu_zbc.h"
#ifdef TOTK_DRAW_PATH
#include "host_present_trace.h"   /* diagnostics initialized on the UI thread, never lazily in a producer */
#include "host_nvn_wait_trace.h"
#ifdef TOTK_DRAW_PATH
static inline void boot_psoc_exit(void);   /* host_gpu_render.h (play (73)): persists the pipeline library when the window closes */
#endif
#endif

/* The UI thread owns HWND; a locked queue and manual-reset event connect it
 * to the native guest scheduler. No BootContext pointer crosses threads. */
typedef struct BootWindow {
    SRWLOCK lock;
    HANDLE ready,thread,notification;
    HWND hwnd;
    HPOWERNOTIFY power_registration;
    DWORD thread_id;
    uint32_t messages[32],head,count,focus,closed,error,visible;
    uint64_t foreground_owner;
    uint32_t focus_policy_set,out_of_focus_suspend;
    uint32_t resume_notifications,suspend_observed;
    uint32_t operation_mode_notifications,performance_mode_notifications;
    HANDLE execution_thread;
    DWORD execution_thread_id;
    int execution_priority;
    uint32_t cpu_boost_mode;
    uint32_t display_groups[BOOT_SESSION_CAPACITY],display_opens[BOOT_SESSION_CAPACITY];
    uint64_t managed_layer_owner,managed_layer_ids[2];
    uint32_t producer_next_id,producer_ids[2],producer_opens[BOOT_SESSION_CAPACITY][2];
    uint32_t producer_refs[2][2];
    BootBufferQueue buffer_queues[2];
    BootGraphicRegistry graphic_buffers;
    HANDLE producer_events[2];
    BootVsync*vsync;
    uint32_t nv_device_next,nv_device_fds[16],nv_device_kinds[16];
    HANDLE nv_control_events[2];
    BootGpuAddressSpace nv_address_spaces[16];
    BootNvMap nvmap;
    BootGpuMappings gpu_mappings;
    BootGpuChannel nv_channels[16];
    BootZbc zbc;
    BootGpu*gpu;
    uint32_t nv_syncpoint_next;
    BootHidMemory hid_memory;
#ifdef TOTK_DRAW_PATH
    struct BootRender *render;    /* D3D12 draw executor (host_gpu_render.h), created on first draw */
    struct BootPresent *present;  /* swapchain + frame dumps (host_gpu_present.h) */
#endif
} BootWindow;

/* One physical Default display belongs to this HWND. Cloned service sessions
 * share their logical open count; closing the last session releases it. */
static inline int boot_window_display(BootWindow*w,unsigned slot,int open,uint64_t id){
    if(!w || slot>=BOOT_SESSION_CAPACITY)return 0;
    AcquireSRWLockExclusive(&w->lock);
    unsigned group=w->display_groups[slot];int ok=!w->closed && w->hwnd;
    if(open){
        if(!group){
            for(group=1;group<=BOOT_SESSION_CAPACITY;++group){unsigned i=0;while(i<BOOT_SESSION_CAPACITY && w->display_groups[i]!=group)++i;if(i==BOOT_SESSION_CAPACITY)break;}
        }
        ok=ok && group<=BOOT_SESSION_CAPACITY && w->display_opens[group-1]!=UINT32_MAX;
        if(ok){w->display_groups[slot]=group;++w->display_opens[group-1];}
    }else{
        ok=ok && id==1 && group && w->display_opens[group-1];
        if(ok)--w->display_opens[group-1];
    }
    ReleaseSRWLockExclusive(&w->lock);return ok;
}
static inline void boot_window_display_clone(BootWindow*w,unsigned source,unsigned target){
    if(!w || source>=BOOT_SESSION_CAPACITY || target>=BOOT_SESSION_CAPACITY)return;
    AcquireSRWLockExclusive(&w->lock);
    unsigned group=w->display_groups[source];
    if(!group){
        for(group=1;group<=BOOT_SESSION_CAPACITY;++group){unsigned i=0;while(i<BOOT_SESSION_CAPACITY && w->display_groups[i]!=group)++i;if(i==BOOT_SESSION_CAPACITY)break;}
        w->display_groups[source]=group;
    }
    w->display_groups[target]=group;ReleaseSRWLockExclusive(&w->lock);
}
static inline void boot_window_display_release(BootWindow*w,unsigned slot){
    if(!w || slot>=BOOT_SESSION_CAPACITY)return;
    AcquireSRWLockExclusive(&w->lock);
    unsigned group=w->display_groups[slot];w->display_groups[slot]=0;
    if(group){unsigned i=0;while(i<BOOT_SESSION_CAPACITY && w->display_groups[i]!=group)++i;if(i==BOOT_SESSION_CAPACITY){w->display_opens[group-1]=0;memset(w->producer_opens[group-1],0,sizeof(w->producer_opens[0]));}}
    ReleaseSRWLockExclusive(&w->lock);
}

/* Bounded application-owned layer registry. No producer buffers or GPU
 * resources are created here. The pair lives until the host app/window ends. */
static inline int boot_window_create_layers(BootWindow*w,uint64_t owner,uint64_t ids[2]){
    if(!w || !owner)return 0;
    AcquireSRWLockExclusive(&w->lock);
    unsigned i=0;while(i<BOOT_SESSION_CAPACITY && !w->display_opens[i])++i;
    int ok=!w->closed && !w->error && w->hwnd && w->foreground_owner==owner && i<BOOT_SESSION_CAPACITY && !w->managed_layer_owner;
    if(ok){
        w->managed_layer_owner=owner;w->managed_layer_ids[0]=1;w->managed_layer_ids[1]=2;
        ids[0]=w->managed_layer_ids[0];ids[1]=w->managed_layer_ids[1];
    }
    ReleaseSRWLockExclusive(&w->lock);return ok;
}

/* Producer identities remain stable for each managed layer until window reset.
 * Open counts belong to a display service group, shared by its clones. */
static inline int boot_window_layer(BootWindow*w,unsigned slot,uint64_t owner,uint64_t layer,int opening,uint32_t*out){
    if(!w || slot>=BOOT_SESSION_CAPACITY || !owner)return 0;
    AcquireSRWLockExclusive(&w->lock);
    unsigned group=w->display_groups[slot],index=0;
    while(index<2 && w->managed_layer_ids[index]!=layer)++index;
    int ok=!w->closed && !w->error && w->hwnd && w->managed_layer_owner==owner && group && index<2;
    if(ok && opening){
        ok=w->display_opens[group-1] && w->producer_opens[group-1][index]!=UINT32_MAX &&
           (w->producer_ids[index] || w->producer_next_id<INT32_MAX);
        if(ok){if(!w->producer_ids[index])w->producer_ids[index]=++w->producer_next_id;
            ++w->producer_opens[group-1][index];*out=w->producer_ids[index];}
    }else if(ok){ok=w->producer_opens[group-1][index]!=0;if(ok)--w->producer_opens[group-1][index];}
    ReleaseSRWLockExclusive(&w->lock);return ok;
}

static inline void boot_window_enqueue_locked(BootWindow*w,uint32_t message){
    if(w->count==32){__atomic_store_n(&w->error,1,__ATOMIC_RELEASE);SetEvent(w->notification);return;}
    w->messages[(w->head+w->count)%32]=message;++w->count;SetEvent(w->notification);
}
static inline void boot_window_focus(BootWindow*w,uint32_t focus){
    AcquireSRWLockExclusive(&w->lock);
    if(w->focus!=focus){w->focus=focus;boot_window_enqueue_locked(w,15);}
    ReleaseSRWLockExclusive(&w->lock);
}
static inline void boot_window_power(BootWindow*w,WPARAM event){
    AcquireSRWLockExclusive(&w->lock);
    if(!w->closed && !w->error){
        if(event==PBT_APMSUSPEND)w->suspend_observed=1;
        else if(event==PBT_APMRESUMEAUTOMATIC || event==PBT_APMRESUMESUSPEND){
            if(w->suspend_observed && w->resume_notifications)boot_window_enqueue_locked(w,16);
            w->suspend_observed=0;
        }
    }
    ReleaseSRWLockExclusive(&w->lock);
}
static LRESULT CALLBACK boot_window_proc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp){
    BootWindow*w=(BootWindow*)(uintptr_t)GetWindowLongPtrW(hwnd,GWLP_USERDATA);
    if(message==WM_NCCREATE){
        w=(BootWindow*)((CREATESTRUCTW*)lp)->lpCreateParams;
        SetWindowLongPtrW(hwnd,GWLP_USERDATA,(LONG_PTR)w);
    }
    if(w)switch(message){
        case WM_POWERBROADCAST:boot_window_power(w,wp);return TRUE;
        /* Desktop window activation is not the Switch HOME button: losing activation keeps the
         * applet InFocus (otherwise the game suspends whenever another window is used). Set
         * TOTK_FOCUS_FOLLOWS_WINDOW=1 to map deactivation to OutOfFocus for focus testing. */
        case WM_ACTIVATE:boot_window_focus(w,LOWORD(wp)==WA_INACTIVE && getenv("TOTK_FOCUS_FOLLOWS_WINDOW")?3:1);InvalidateRect(hwnd,NULL,TRUE);break;
        case WM_CLOSE:
#ifdef TOTK_DRAW_PATH
            boot_psoc_exit();
#endif
            AcquireSRWLockExclusive(&w->lock);boot_window_enqueue_locked(w,4);ReleaseSRWLockExclusive(&w->lock);
            DestroyWindow(hwnd);return 0;
        case WM_DESTROY:
            if(w->power_registration){
                BOOL released=UnregisterSuspendResumeNotification(w->power_registration);
                w->power_registration=NULL;
                if(!released){AcquireSRWLockExclusive(&w->lock);__atomic_store_n(&w->error,1,__ATOMIC_RELEASE);ReleaseSRWLockExclusive(&w->lock);}
            }
            AcquireSRWLockExclusive(&w->lock);__atomic_store_n(&w->closed,1,__ATOMIC_RELEASE);w->hwnd=NULL;SetEvent(w->notification);ReleaseSRWLockExclusive(&w->lock);
            PostQuitMessage(0);return 0;
        case WM_PAINT:{
            PAINTSTRUCT paint;HDC dc=BeginPaint(hwnd,&paint);RECT area;GetClientRect(hwnd,&area);
            FillRect(dc,&area,(HBRUSH)(COLOR_WINDOW+1));
            AcquireSRWLockShared(&w->lock);uint32_t focus=w->focus;ReleaseSRWLockShared(&w->lock);
            const wchar_t*text=focus==1?
                L"TOTK native Windows startup\n\nWindow focus: ACTIVE\n\nGraphics backend is not implemented yet.\nSwitch to another app and back to test focus notifications.\nClose this window to finish the test.":
                L"TOTK native Windows startup\n\nWindow focus: BACKGROUND\n\nGraphics backend is not implemented yet.\nSwitch to another app and back to test focus notifications.\nClose this window to finish the test.";
            DrawTextW(dc,text,-1,&area,DT_CENTER|DT_WORDBREAK|DT_NOPREFIX);
            EndPaint(hwnd,&paint);return 0;
        }
    }
    return DefWindowProcW(hwnd,message,wp,lp);
}
static DWORD WINAPI boot_window_thread(void*parameter){
    BootWindow*w=parameter;HINSTANCE instance=GetModuleHandleW(NULL);
#ifdef TOTK_DRAW_PATH
    boot_present_trace_start();
    boot_nw_start();   /*80: UI thread before window-ready, never a native callback */
#endif
    const wchar_t*class_name=L"TotkNativeStartupHostWindow";
    WNDCLASSW cls={0};cls.lpfnWndProc=boot_window_proc;cls.hInstance=instance;cls.lpszClassName=class_name;
    cls.hCursor=LoadCursorW(NULL,MAKEINTRESOURCEW(32512));cls.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);
    ATOM atom=RegisterClassW(&cls);
    if(!atom && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS){w->error=1;SetEvent(w->ready);return 1;}
    HWND hwnd=CreateWindowExW(0,class_name,L"TOTK native startup - host window",WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT,CW_USEDEFAULT,960,540,NULL,NULL,instance,w);
    if(!hwnd){w->error=1;SetEvent(w->ready);UnregisterClassW(class_name,instance);return 1;}
    w->power_registration=RegisterSuspendResumeNotification(hwnd,DEVICE_NOTIFY_WINDOW_HANDLE);
    if(!w->power_registration){w->error=1;DestroyWindow(hwnd);SetEvent(w->ready);UnregisterClassW(class_name,instance);return 1;}
    AcquireSRWLockExclusive(&w->lock);w->hwnd=hwnd;ReleaseSRWLockExclusive(&w->lock);
    boot_window_focus(w,3);
    if(w->visible){ShowWindow(hwnd,SW_SHOWNORMAL);UpdateWindow(hwnd);}
    SetEvent(w->ready);
    MSG message;int result;
    while((result=GetMessageW(&message,NULL,0,0))>0){TranslateMessage(&message);DispatchMessageW(&message);}
    if(result<0){AcquireSRWLockExclusive(&w->lock);__atomic_store_n(&w->error,1,__ATOMIC_RELEASE);ReleaseSRWLockExclusive(&w->lock);}
    if(IsWindow(hwnd))DestroyWindow(hwnd);
    UnregisterClassW(class_name,instance);return 0;
}
static inline BootWindow*boot_window_open(int visible){
    BootWindow*w=calloc(1,sizeof(*w));if(!w)return NULL;InitializeSRWLock(&w->lock);w->visible=visible!=0;
    w->ready=CreateEventW(NULL,TRUE,FALSE,NULL);w->notification=CreateEventW(NULL,TRUE,FALSE,NULL);
    if(w->ready && w->notification)w->thread=CreateThread(NULL,0,boot_window_thread,w,0,&w->thread_id);
    if(w->thread){
        WaitForSingleObject(w->ready,INFINITE);
        if(!w->error)return w;
        WaitForSingleObject(w->thread,INFINITE);CloseHandle(w->thread);
    }
    if(w->notification)CloseHandle(w->notification);if(w->ready)CloseHandle(w->ready);
    boot_nvmap_destroy(&w->nvmap);free(w);return NULL;
}
static inline int boot_window_receive(BootWindow*w,uint32_t*out){
    AcquireSRWLockExclusive(&w->lock);
    int result=w->error?-1:w->count?1:0;
    if(result==1){*out=w->messages[w->head];w->head=(w->head+1)%32;--w->count;}
    if(!w->count && !w->error)ResetEvent(w->notification);
    ReleaseSRWLockExclusive(&w->lock);return result;
}
static inline uint32_t boot_window_get_focus(BootWindow*w){
    AcquireSRWLockShared(&w->lock);uint32_t focus=w->focus;ReleaseSRWLockShared(&w->lock);return focus;
}
/* Single-application desktop profile: rights belong to this window's applet.
 * This does not force Windows activation. Unsupported acquisition while in
 * the background stops explicitly instead of inventing a Horizon result. */
static inline int boot_window_acquire_foreground(BootWindow*w,uint64_t owner){
    AcquireSRWLockExclusive(&w->lock);
    int valid=owner && w->hwnd && !w->closed && !w->error && w->focus==1 &&
              (!w->foreground_owner || w->foreground_owner==owner);
    if(valid)w->foreground_owner=owner;
    ReleaseSRWLockExclusive(&w->lock);return valid;
}
static inline int boot_window_set_focus_policy(BootWindow*w,unsigned command,const uint8_t*values){
    /* Desktop has no HOME/library-applet or console-sleep state. Both observed
     * SuspendHomeSleep variants use background execution here. The Notify
     * variant receives the real focus-change messages already queued by HWND.
     * Keep the selected variant (1=baseline, 2=Notify); other policies stop. */
    AcquireSRWLockExclusive(&w->lock);
    int valid=w->foreground_owner && !w->closed && !w->error;
    if(command==13)valid=valid && values[0]<=1 && values[1]==0 && values[2]==1;
    else valid=valid && command==16 && values[0]==0 && w->focus_policy_set;
    if(valid){if(command==13)w->focus_policy_set=1+values[0];else w->out_of_focus_suspend=values[0];}
    ReleaseSRWLockExclusive(&w->lock);return valid;
}
static inline int boot_window_set_resume_notifications(BootWindow*w,unsigned enabled){
    AcquireSRWLockExclusive(&w->lock);
    int valid=enabled<=1 && w->foreground_owner && !w->closed && !w->error;
    if(valid)w->resume_notifications=enabled;
    ReleaseSRWLockExclusive(&w->lock);return valid;
}
static inline int boot_window_set_mode_notification(BootWindow*w,unsigned command,unsigned enabled){
    /* This desktop profile exposes fixed handheld/normal mode (zero). There
     * are no docking transitions; subscriptions are retained without inventing
     * mode-change events or translating focus changes into docking changes. */
    AcquireSRWLockExclusive(&w->lock);
    int valid=(command==11 || command==12) && enabled<=1 && w->foreground_owner && !w->closed && !w->error;
    if(valid){if(command==11)w->operation_mode_notifications=enabled;else w->performance_mode_notifications=enabled;}
    ReleaseSRWLockExclusive(&w->lock);return valid;
}
static inline HWND boot_window_handle(BootWindow*w){
    AcquireSRWLockShared(&w->lock);HWND hwnd=w->hwnd;ReleaseSRWLockShared(&w->lock);return hwnd;
}
/* overlap-work P1: lock-free. closed and error only ever change 0 -> 1 (release stores, under w->lock after the
 * window exists), so an acquire load gives the answer the shared lock gave, at most one poll later than a writer still
 * inside its critical section. Taking w->lock here made every interrupted guest thread (boot_play_stop, the scheduler
 * and kernel exit checks) sleep until a native NVN submit or present, which hold it exclusively, had finished. */
/* TOTK_OVERLAP=0 (guardfix): the shared lock again, the serial behaviour before overlap-work step 2 (play-84). Not a
 * boot_window_* name, so it stays out of the window facade (split_host_window.py). */
static inline int boot_overlap_window_on(void){
    static volatile LONG v=-1;
    if(v<0){char e[8];DWORD n=GetEnvironmentVariableA("TOTK_OVERLAP",e,sizeof e);v=!(n&&n<sizeof e&&e[0]=='0');}
    return (int)v;
}
static inline int boot_window_is_closed(BootWindow*w){
    if(!boot_overlap_window_on()){
        AcquireSRWLockShared(&w->lock);
        int closed=__atomic_load_n(&w->closed,__ATOMIC_ACQUIRE) || __atomic_load_n(&w->error,__ATOMIC_ACQUIRE);
        ReleaseSRWLockShared(&w->lock);return closed;
    }
    return __atomic_load_n(&w->closed,__ATOMIC_ACQUIRE) || __atomic_load_n(&w->error,__ATOMIC_ACQUIRE);
}
/* Windows performance-policy adapter: FastLoad raises only the native
 * execution thread to at least ABOVE_NORMAL. Hardware clocks stay managed by
 * Windows; no GPU backend exists yet. Normal/reset restores its prior value. */
static inline int boot_window_set_cpu_boost(BootWindow*w,unsigned mode){
    if(mode>1)return 0;
    AcquireSRWLockExclusive(&w->lock);
    int ok=1;
    if(mode==0){
        if(w->execution_thread){
            DWORD exit_code=STILL_ACTIVE;
            ok=SetThreadPriority(w->execution_thread,w->execution_priority) ||
               (GetExitCodeThread(w->execution_thread,&exit_code) && exit_code!=STILL_ACTIVE);
            if(ok){CloseHandle(w->execution_thread);w->execution_thread=NULL;w->execution_thread_id=0;}
        }
        if(ok)w->cpu_boost_mode=0;
    }else if(w->closed || w->error || (w->execution_thread && w->execution_thread_id!=GetCurrentThreadId()))ok=0;
    else {
        HANDLE thread=w->execution_thread;int previous=w->execution_priority,created=thread==NULL;
        if(created){
            previous=GetThreadPriority(GetCurrentThread());
            ok=previous!=THREAD_PRIORITY_ERROR_RETURN &&
               DuplicateHandle(GetCurrentProcess(),GetCurrentThread(),GetCurrentProcess(),&thread,0,FALSE,DUPLICATE_SAME_ACCESS);
        }
        if(ok)ok=SetThreadPriority(thread,previous>THREAD_PRIORITY_ABOVE_NORMAL?previous:THREAD_PRIORITY_ABOVE_NORMAL);
        if(ok){w->execution_thread=thread;w->execution_thread_id=GetCurrentThreadId();w->execution_priority=previous;w->cpu_boost_mode=1;}
        else if(created && thread)CloseHandle(thread);
    }
    ReleaseSRWLockExclusive(&w->lock);return ok;
}
static inline int boot_window_close(BootWindow*w){
    if(!w)return 1;
    /* Retain queue and resource ownership if completion is uncertain. */
    for(unsigned i=0;i<16;++i)if(w->nv_channels[i].pending && !w->nv_channels[i].pending->completed)return 0;
    int restored=boot_window_set_cpu_boost(w,0);
#ifdef TOTK_DRAW_PATH
    boot_psoc_exit();
#endif
    if(WaitForSingleObject(w->thread,0)==WAIT_TIMEOUT)PostThreadMessageW(w->thread_id,WM_QUIT,0,0);
    if(WaitForSingleObject(w->thread,5000)!=WAIT_OBJECT_0)return 0;
    if(w->vsync && !boot_vsync_close(w->vsync))return 0;
    if(w->execution_thread)CloseHandle(w->execution_thread);
    for(unsigned i=0;i<2;++i)if(w->nv_control_events[i])CloseHandle(w->nv_control_events[i]);
    for(unsigned i=0;i<2;++i)if(w->producer_events[i])CloseHandle(w->producer_events[i]);
    for(unsigned i=0;i<16;++i)boot_gpu_channel_close(&w->nv_channels[i]);
    boot_gpu_close(w->gpu);
    boot_hid_memory_close(&w->hid_memory);
    boot_nvmap_destroy(&w->nvmap);
    CloseHandle(w->thread);CloseHandle(w->notification);CloseHandle(w->ready);free(w);return restored;
}
#ifdef TOTK_DRAW_PATH
#include "host_gpu_draw_exec.h"   /* backend only: pages see the opaque window facade */
#endif
#endif
