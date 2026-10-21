#ifndef TOTK_HOST_VSYNC_H
#define TOTK_HOST_VSYNC_H
#include <windows.h>
#include <dwmapi.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef HRESULT (WINAPI *BootDwmTiming)(HWND,DWM_TIMING_INFO*);
typedef struct BootVsync {
    HMODULE library;BootDwmTiming timing;HANDLE event,stop,thread;
    uint64_t refresh;volatile LONG failed;
} BootVsync;
/* Observe compositor refresh advances. Manual reset coalesces missed ticks;
 * this is desktop-compositor timing, not an exclusive GPU scanout interrupt. */
static DWORD WINAPI boot_vsync_thread(void*parameter){
    BootVsync*v=parameter;
    while(WaitForSingleObject(v->stop,2)==WAIT_TIMEOUT){
        DWM_TIMING_INFO timing={0};timing.cbSize=sizeof(timing);
        if(FAILED(v->timing(NULL,&timing))){InterlockedExchange(&v->failed,1);return 1;}
        if(timing.cRefresh>v->refresh){v->refresh=timing.cRefresh;SetEvent(v->event);}
        else if(timing.cRefresh<v->refresh)v->refresh=timing.cRefresh;
    }
    return 0;
}
static inline int boot_vsync_close(BootVsync*v){
    if(!v)return 1;
    if(v->thread){SetEvent(v->stop);if(WaitForSingleObject(v->thread,5000)!=WAIT_OBJECT_0)return 0;CloseHandle(v->thread);}
    if(v->event)CloseHandle(v->event);if(v->stop)CloseHandle(v->stop);
    if(v->library)FreeLibrary(v->library);free(v);return 1;
}
static inline BootVsync*boot_vsync_open(void){
    BootVsync*v=calloc(1,sizeof(*v));if(!v)return NULL;
    v->library=LoadLibraryW(L"dwmapi.dll");
    if(v->library){FARPROC procedure=GetProcAddress(v->library,"DwmGetCompositionTimingInfo");memcpy(&v->timing,&procedure,sizeof(procedure));}
    DWM_TIMING_INFO timing={0};timing.cbSize=sizeof(timing);
    if(!v->timing || FAILED(v->timing(NULL,&timing)) || !timing.qpcRefreshPeriod){boot_vsync_close(v);return NULL;}
    v->refresh=timing.cRefresh;v->event=CreateEventW(NULL,TRUE,FALSE,NULL);v->stop=CreateEventW(NULL,TRUE,FALSE,NULL);
    if(v->event && v->stop)v->thread=CreateThread(NULL,0,boot_vsync_thread,v,0,NULL);
    if(!v->thread){boot_vsync_close(v);return NULL;}return v;
}
#endif
