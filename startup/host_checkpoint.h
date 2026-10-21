/* Checkpoints of the whole guest state, taken at a stop (no guest code running).
 *
 * A checkpoint lets a fatal guest abort (svcBreak) cost a reload instead of a
 * replay from boot. It is restored into a freshly started probe: Python boots the
 * modules, opens RomFS and the host window as usual, then boot_checkpoint_load
 * replaces the guest state and the probe resumes at the saved stop.
 *
 * What is saved: BootContext, every guest region's bytes (aliases and shared
 * memory views by reference), every host event/mapping the guest holds (by kernel
 * object identity, with signal state and mapping contents) and the logical window /
 * GPU-channel / nvmap state. Built as its own DLL (checkpoint_dll.c) from the same
 * headers, so the game DLL needs no rebuild; DLL statics such as the shadow-RAM
 * record already reset on every resume DLL swap and are not saved. Render caches (D3D12
 * resources, pipelines, swapchain) are not saved: they rebuild on the next draw.
 * Guest time is the host QPC, so a checkpoint is only valid within the same
 * Windows session; load refuses one taken after a reboot.
 */
#ifndef TOTK_HOST_CHECKPOINT_H
#define TOTK_HOST_CHECKPOINT_H
#include <stdio.h>

#define BOOT_CKPT_MAGIC "TOTKCKP1"
#define BOOT_CKPT_VERSION 1u
#define BOOT_CKPT_MAX_OBJECTS 64
enum { CKPT_REGION_OWNED=1, CKPT_REGION_ALIAS=2, CKPT_REGION_MAPPING=3 };
enum { CKPT_OBJECT_EVENT=1, CKPT_OBJECT_MAPPING=2, CKPT_OBJECT_VSYNC=3 };
/* Owner fields of the window/context that hold a kernel object, in file order. */
enum { CKPT_OWNER_SOURCE0=0, CKPT_OWNER_SOURCE1, CKPT_OWNER_SOURCE2, CKPT_OWNER_PRODUCER0, CKPT_OWNER_PRODUCER1,
       CKPT_OWNER_CONTROL0, CKPT_OWNER_CONTROL1, CKPT_OWNER_HID, CKPT_OWNER_VSYNC, CKPT_OWNER_ERROR0,
       CKPT_OWNER_COUNT=CKPT_OWNER_ERROR0+16 };

typedef struct BootCkptObject { uint32_t kind,manual,signaled,size; uint8_t*content; HANDLE native; } BootCkptObject;
typedef struct BootCkptHeader {
    char magic[8]; uint32_t version,context_size; uint64_t qpc,window_size,stage_size,region_count;
} BootCkptHeader;

typedef LONG (NTAPI*BootNtQueryEvent)(HANDLE,int,PVOID,ULONG,PULONG);
typedef BOOL (WINAPI*BootCompareObjectHandles)(HANDLE,HANDLE);

static inline int boot_ckpt_same(HANDLE a,HANDLE b){
    static BootCompareObjectHandles compare;
    if(!a || !b)return 0;
    if(a==b)return 1;
    if(!compare)compare=(BootCompareObjectHandles)(void*)GetProcAddress(GetModuleHandleW(L"kernelbase.dll"),"CompareObjectHandles");
    return compare && compare(a,b);
}
/* Event type (0 notification = manual reset, 1 synchronization = auto reset) and
 * state, read without consuming an auto-reset signal. */
static inline int boot_ckpt_event_state(HANDLE h,uint32_t*manual,uint32_t*signaled){
    static BootNtQueryEvent query;
    struct { int type; LONG state; } info={0};
    if(!query)query=(BootNtQueryEvent)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtQueryEvent");
    if(!query || query(h,0,&info,sizeof info,NULL)<0)return 0;
    *manual=info.type==0;*signaled=info.state!=0;return 1;
}
static inline int boot_ckpt_write(FILE*f,const void*p,uint64_t n){
    const uint8_t*b=(const uint8_t*)p;
    while(n){size_t part=n>((size_t)1<<26)?((size_t)1<<26):(size_t)n;
        if(fwrite(b,1,part,f)!=part)return 0;b+=part;n-=part;}
    return 1;
}
static inline int boot_ckpt_read(FILE*f,void*p,uint64_t n){
    uint8_t*b=(uint8_t*)p;
    while(n){size_t part=n>((size_t)1<<26)?((size_t)1<<26):(size_t)n;
        if(fread(b,1,part,f)!=part)return 0;b+=part;n-=part;}
    return 1;
}
static inline void boot_ckpt_log(const char*message,uint64_t a,uint64_t b){
    FILE*f=fopen("checkpoint.log","a");
    if(f){fprintf(f,"%s %llu %llu",message,(unsigned long long)a,(unsigned long long)b);fputc(10,f);fclose(f);}
}
/* Index of a kernel object in the table, adding it on first sight. */
static inline int boot_ckpt_object(BootCkptObject*objects,uint32_t*count,HANDLE h,uint32_t kind){
    if(!h)return -1;
    for(uint32_t i=0;i<*count;++i)if(boot_ckpt_same(objects[i].native,h)){
        if(kind>objects[i].kind)objects[i].kind=kind;   /* mapping/vsync classification wins */
        return (int)i;
    }
    if(*count==BOOT_CKPT_MAX_OBJECTS)return -2;
    objects[*count]=(BootCkptObject){kind,0,0,0,NULL,h};return (int)(*count)++;
}

__declspec(dllexport) int boot_checkpoint_save(BootContext*c,const wchar_t*path){
    if(!c || !path)return 0;
    BootWindow*w=(BootWindow*)(uintptr_t)c->window_native;
    BootCkptObject objects[BOOT_CKPT_MAX_OBJECTS];uint32_t object_count=0;
    int32_t slots[16],waits[BOOT_THREAD_CAPACITY][64],owners[CKPT_OWNER_COUNT];
    for(unsigned i=0;i<16;++i)slots[i]=-1;
    for(unsigned i=0;i<CKPT_OWNER_COUNT;++i)owners[i]=-1;
    for(unsigned t=0;t<BOOT_THREAD_CAPACITY;++t)for(unsigned i=0;i<64;++i)waits[t][i]=-1;
    /* Owner fields first, so guest slots resolve to the same objects. */
    for(unsigned i=0;i<3;++i)owners[CKPT_OWNER_SOURCE0+i]=boot_ckpt_object(objects,&object_count,(HANDLE)(uintptr_t)c->event_source_handles[i],CKPT_OBJECT_EVENT);
    if(w){
        for(unsigned i=0;i<2;++i)owners[CKPT_OWNER_PRODUCER0+i]=boot_ckpt_object(objects,&object_count,w->producer_events[i],CKPT_OBJECT_EVENT);
        for(unsigned i=0;i<2;++i)owners[CKPT_OWNER_CONTROL0+i]=boot_ckpt_object(objects,&object_count,w->nv_control_events[i],CKPT_OBJECT_EVENT);
        owners[CKPT_OWNER_HID]=boot_ckpt_object(objects,&object_count,w->hid_memory.mapping,CKPT_OBJECT_MAPPING);
        owners[CKPT_OWNER_VSYNC]=boot_ckpt_object(objects,&object_count,w->vsync?w->vsync->event:NULL,CKPT_OBJECT_VSYNC);
        for(unsigned i=0;i<16;++i)owners[CKPT_OWNER_ERROR0+i]=boot_ckpt_object(objects,&object_count,w->nv_channels[i].error_event,CKPT_OBJECT_EVENT);
    }
    for(unsigned i=0;i<16;++i){
        if(!c->event_handles[i])continue;
        uint32_t source=c->event_sources[i];
        slots[i]=boot_ckpt_object(objects,&object_count,(HANDLE)(uintptr_t)c->event_native_handles[i],
                                  source==400 || source==700?CKPT_OBJECT_MAPPING:CKPT_OBJECT_EVENT);
        if(slots[i]<0){boot_ckpt_log("save: event slot has no object",i,source);return 0;}
    }
    for(unsigned t=0;t<c->thread_count && t<BOOT_THREAD_CAPACITY;++t){BootThread*th=&c->threads[t];
        if(th->state!=5)continue;   /* other wait states keep keys/deadlines, not handles */
        for(unsigned i=0;i<th->wait_count && i<64;++i){
            if(th->wait_threads[i] || !th->wait_native[i])continue;
            int found=-1;
            for(uint32_t k=0;k<object_count;++k)if(boot_ckpt_same(objects[k].native,(HANDLE)(uintptr_t)th->wait_native[i])){found=(int)k;break;}
            if(found<0){boot_ckpt_log("save: thread waits on an unknown object",t,i);return 0;}
            waits[t][i]=found;
        }
    }
    for(uint32_t k=0;k<object_count;++k){BootCkptObject*o=&objects[k];
        if(o->kind==CKPT_OBJECT_EVENT){
            if(!boot_ckpt_event_state(o->native,&o->manual,&o->signaled)){boot_ckpt_log("save: event state query failed",k,0);return 0;}
        }else if(o->kind==CKPT_OBJECT_MAPPING){
            int hid=(int)k==owners[CKPT_OWNER_HID];
            o->size=hid?BOOT_HID_MEMORY_SIZE:0x1000;
            if(hid)o->content=w->hid_memory.data;
            else {o->content=(uint8_t*)MapViewOfFile(o->native,FILE_MAP_READ,0,0,o->size);
                if(!o->content){boot_ckpt_log("save: mapping view failed",k,0);return 0;}}
        }
    }
    /* Regions: owned bytes, or a reference to a container region or a mapping object. */
    uint32_t*kinds=calloc(c->region_count?c->region_count:1,sizeof(uint32_t));
    uint32_t*links=calloc(c->region_count?c->region_count:1,sizeof(uint32_t));
    uint64_t*offsets=calloc(c->region_count?c->region_count:1,sizeof(uint64_t));
    int ok=kinds && links && offsets;
    for(unsigned i=0;ok && i<c->region_count;++i){Region*r=&c->regions[i];
        if(r->reserved!=2){kinds[i]=CKPT_REGION_OWNED;continue;}
        if(c->memory_types[i]==6){   /* shared memory view: HID or time */
            int found=-1;
            if(w && r->data==w->hid_memory.data)found=owners[CKPT_OWNER_HID];
            else for(uint32_t k=0;k<object_count;++k)
                if(objects[k].kind==CKPT_OBJECT_MAPPING && objects[k].size==r->size && (int)k!=owners[CKPT_OWNER_HID] &&
                   !memcmp(objects[k].content,r->data,(size_t)r->size)){found=(int)k;break;}
            if(found<0){boot_ckpt_log("save: shared view without a mapping",i,r->base);ok=0;break;}
            kinds[i]=CKPT_REGION_MAPPING;links[i]=(uint32_t)found;continue;
        }
        unsigned j=0;
        for(;j<c->region_count;++j){Region*o=&c->regions[j];
            if(j!=i && o->reserved!=2 && r->data>=o->data && r->data<o->data+o->size && r->size<=o->size-(uint64_t)(r->data-o->data))break;}
        if(j==c->region_count){boot_ckpt_log("save: alias without a container",i,r->base);ok=0;break;}
        kinds[i]=CKPT_REGION_ALIAS;links[i]=j;offsets[i]=(uint64_t)(r->data-c->regions[j].data);
    }
    wchar_t temporary[600];
    FILE*f=NULL;
    if(ok && _snwprintf(temporary,600,L"%ls.tmp",path)>0)f=_wfopen(temporary,L"wb");
    if(ok && !f){boot_ckpt_log("save: cannot create file",0,0);ok=0;}
    if(ok){
        static char buffer[1<<22];setvbuf(f,buffer,_IOFBF,sizeof buffer);
        LARGE_INTEGER now;QueryPerformanceCounter(&now);
        BootCkptHeader h={{0},BOOT_CKPT_VERSION,(uint32_t)sizeof(BootContext),(uint64_t)now.QuadPart,
                          w?sizeof(BootWindow):0,sizeof(BootGpuStage),c->region_count};
        memcpy(h.magic,BOOT_CKPT_MAGIC,8);
        ok=boot_ckpt_write(f,&h,sizeof h) && boot_ckpt_write(f,c,sizeof(BootContext));
        for(unsigned i=0;ok && i<c->region_count;++i){
            ok=boot_ckpt_write(f,&kinds[i],4) && boot_ckpt_write(f,&links[i],4) && boot_ckpt_write(f,&offsets[i],8);
            if(ok && kinds[i]==CKPT_REGION_OWNED)ok=boot_ckpt_write(f,c->regions[i].data,c->regions[i].size);
        }
        ok=ok && boot_ckpt_write(f,&object_count,4);
        for(uint32_t k=0;ok && k<object_count;++k){BootCkptObject*o=&objects[k];
            ok=boot_ckpt_write(f,&o->kind,4) && boot_ckpt_write(f,&o->manual,4) && boot_ckpt_write(f,&o->signaled,4) &&
               boot_ckpt_write(f,&o->size,4) && (!o->size || boot_ckpt_write(f,o->content,o->size));
        }
        ok=ok && boot_ckpt_write(f,slots,sizeof slots) && boot_ckpt_write(f,waits,sizeof waits) && boot_ckpt_write(f,owners,sizeof owners);
        if(ok && w){
            ok=boot_ckpt_write(f,w,sizeof(BootWindow));
            if(ok && w->nvmap.capacity)ok=boot_ckpt_write(f,w->nvmap.objects,(uint64_t)w->nvmap.capacity*sizeof(BootNvMapObject));
            for(unsigned i=0;ok && i<16;++i){BootGpuChannel*ch=&w->nv_channels[i];
                if(ch->pending){boot_ckpt_log("save: channel has a pending GPU batch",i,0);ok=0;break;}
                if(ch->entries)ok=boot_ckpt_write(f,ch->entries,(uint64_t)ch->capacity*8);
                if(ok && ch->state)ok=boot_ckpt_write(f,ch->state,sizeof(BootGpuStage));
            }
        }
        if(fclose(f))ok=0;
        if(!ok)boot_ckpt_log("save: write failed",0,0);
    }
    for(uint32_t k=0;k<object_count;++k)
        if(objects[k].kind==CKPT_OBJECT_MAPPING && (int)k!=owners[CKPT_OWNER_HID] && objects[k].content)UnmapViewOfFile(objects[k].content);
    free(kinds);free(links);free(offsets);
    if(ok && !MoveFileExW(temporary,path,MOVEFILE_REPLACE_EXISTING)){boot_ckpt_log("save: rename failed",GetLastError(),0);ok=0;}
    if(!ok && f)DeleteFileW(temporary);
    if(ok)boot_ckpt_log("save: ok steps/regions",c->steps,c->region_count);
    return ok;
}

/* Restore into a freshly booted context whose host window and RomFS are open and
 * whose guest has not run. Python-owned regions (module images, stack, TLS) are
 * matched by guest base; everything else is allocated anew. */
__declspec(dllexport) int boot_checkpoint_load(BootContext*c,const wchar_t*path){
    if(!c || !path)return 0;
    BootWindow*w=(BootWindow*)(uintptr_t)c->window_native;
    FILE*f=_wfopen(path,L"rb");if(!f){boot_ckpt_log("load: cannot open",0,0);return 0;}
    static char buffer[1<<22];setvbuf(f,buffer,_IOFBF,sizeof buffer);
    BootCkptHeader h;BootContext*s=malloc(sizeof(BootContext));BootWindow*sw=NULL;
    BootCkptObject objects[BOOT_CKPT_MAX_OBJECTS];uint32_t object_count=0;memset(objects,0,sizeof objects);
    int32_t slots[16],waits[BOOT_THREAD_CAPACITY][64],owners[CKPT_OWNER_COUNT];
    static Region regions[BOOT_REGION_CAPACITY];static uint32_t kinds[BOOT_REGION_CAPACITY],links[BOOT_REGION_CAPACITY];static uint64_t offsets[BOOT_REGION_CAPACITY];   /* static: too big for the stack at 32768 */
    LARGE_INTEGER now;QueryPerformanceCounter(&now);
    int ok=s && boot_ckpt_read(f,&h,sizeof h) && !memcmp(h.magic,BOOT_CKPT_MAGIC,8) && h.version==BOOT_CKPT_VERSION &&
           h.context_size==sizeof(BootContext) && h.stage_size==sizeof(BootGpuStage) &&
           (h.window_size==0 || h.window_size==sizeof(BootWindow)) && h.region_count<=BOOT_REGION_CAPACITY &&
           boot_ckpt_read(f,s,sizeof(BootContext)) && s->region_count==h.region_count;
    if(!ok)boot_ckpt_log("load: header or ABI mismatch",h.context_size,sizeof(BootContext));
    if(ok && (uint64_t)now.QuadPart<h.qpc){boot_ckpt_log("load: taken before this Windows session (QPC went back)",h.qpc,(uint64_t)now.QuadPart);ok=0;}
    if(ok && h.window_size && !w){boot_ckpt_log("load: checkpoint has a window, context has none",0,0);ok=0;}
    /* Regions. */
    for(unsigned i=0;ok && i<s->region_count;++i){Region r=s->regions[i];
        ok=boot_ckpt_read(f,&kinds[i],4) && boot_ckpt_read(f,&links[i],4) && boot_ckpt_read(f,&offsets[i],8);
        if(!ok)break;
        regions[i]=r;regions[i].data=NULL;
        if(kinds[i]!=CKPT_REGION_OWNED)continue;
        uint8_t*data=NULL;
        if(r.reserved==0){
            for(unsigned j=0;j<c->region_count;++j)
                if(c->regions[j].reserved==0 && c->regions[j].base==r.base && c->regions[j].size==r.size){data=c->regions[j].data;break;}
            if(!data){boot_ckpt_log("load: no Python-owned region at base",r.base,r.size);ok=0;break;}
        }else if(r.reserved==1)data=calloc(1,(size_t)r.size);
        else if(r.reserved==3)data=VirtualAlloc(NULL,(size_t)r.size,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
        if(!data){boot_ckpt_log("load: region allocation failed",r.base,r.size);ok=0;break;}
        regions[i].data=data;ok=boot_ckpt_read(f,data,r.size);
    }
    /* Kernel objects. */
    ok=ok && boot_ckpt_read(f,&object_count,4) && object_count<=BOOT_CKPT_MAX_OBJECTS;
    for(uint32_t k=0;ok && k<object_count;++k){BootCkptObject*o=&objects[k];
        ok=boot_ckpt_read(f,&o->kind,4) && boot_ckpt_read(f,&o->manual,4) && boot_ckpt_read(f,&o->signaled,4) &&
           boot_ckpt_read(f,&o->size,4) && o->size<=BOOT_HID_MEMORY_SIZE;
        if(ok && o->size){o->content=malloc(o->size);ok=o->content && boot_ckpt_read(f,o->content,o->size);}
    }
    ok=ok && boot_ckpt_read(f,slots,sizeof slots) && boot_ckpt_read(f,waits,sizeof waits) && boot_ckpt_read(f,owners,sizeof owners);
    if(ok && h.window_size){
        sw=malloc(sizeof(BootWindow));ok=sw && boot_ckpt_read(f,sw,sizeof(BootWindow));
    }
    BootNvMapObject*nvmap=NULL;uint64_t*entries[16]={0};BootGpuStage*stages[16]={0};
    if(ok && sw){
        if(sw->nvmap.capacity){nvmap=malloc((size_t)sw->nvmap.capacity*sizeof(BootNvMapObject));
            ok=nvmap && boot_ckpt_read(f,nvmap,(uint64_t)sw->nvmap.capacity*sizeof(BootNvMapObject));}
        for(unsigned i=0;ok && i<16;++i){BootGpuChannel*ch=&sw->nv_channels[i];
            if(ch->entries){entries[i]=malloc((size_t)ch->capacity*8);ok=entries[i] && boot_ckpt_read(f,entries[i],(uint64_t)ch->capacity*8);}
            if(ok && ch->state){stages[i]=malloc(sizeof(BootGpuStage));ok=stages[i] && boot_ckpt_read(f,stages[i],sizeof(BootGpuStage));}
        }
    }
    fclose(f);
    if(!ok){boot_ckpt_log("load: read failed",0,0);free(s);free(sw);return 0;}   /* allocations leak: the probe exits */
    /* Objects the new host already owns: the window notification (source 1). */
    if(owners[CKPT_OWNER_SOURCE1]>=0)objects[owners[CKPT_OWNER_SOURCE1]].native=(HANDLE)(uintptr_t)c->event_source_handles[1];
    if(sw && owners[CKPT_OWNER_VSYNC]>=0){
        if(!w->vsync)w->vsync=boot_vsync_open();
        if(!w->vsync){boot_ckpt_log("load: vsync open failed",0,0);return 0;}
        objects[owners[CKPT_OWNER_VSYNC]].native=w->vsync->event;
    }
    for(uint32_t k=0;k<object_count;++k){BootCkptObject*o=&objects[k];
        if(o->kind==CKPT_OBJECT_EVENT){
            if(!o->native)o->native=CreateEventW(NULL,o->manual?TRUE:FALSE,o->signaled?TRUE:FALSE,NULL);
            else if(o->signaled)SetEvent(o->native);else ResetEvent(o->native);
        }else if(o->kind==CKPT_OBJECT_MAPPING){
            o->native=CreateFileMappingW(INVALID_HANDLE_VALUE,NULL,PAGE_READWRITE,0,o->size,NULL);
            uint8_t*view=o->native?(uint8_t*)MapViewOfFile(o->native,FILE_MAP_ALL_ACCESS,0,0,o->size):NULL;
            if(view){memcpy(view,o->content,o->size);
                if((int)k==owners[CKPT_OWNER_HID] && w){w->hid_memory.mapping=o->native;w->hid_memory.data=view;}
                else UnmapViewOfFile(view);}
            else o->native=NULL;
        }
        if(!o->native){boot_ckpt_log("load: object recreate failed",k,o->kind);return 0;}
    }
    /* Shared-memory views and aliases, now that containers and mappings exist. */
    for(unsigned i=0;i<s->region_count;++i){
        if(kinds[i]==CKPT_REGION_MAPPING){
            if(links[i]>=object_count){boot_ckpt_log("load: bad mapping link",i,links[i]);return 0;}
            if((int)links[i]==owners[CKPT_OWNER_HID])regions[i].data=w->hid_memory.data;
            else regions[i].data=(uint8_t*)MapViewOfFile(objects[links[i]].native,FILE_MAP_READ,0,0,(SIZE_T)regions[i].size);
        }else if(kinds[i]==CKPT_REGION_ALIAS){
            if(links[i]>=s->region_count || kinds[links[i]]!=CKPT_REGION_OWNED){boot_ckpt_log("load: bad alias link",i,links[i]);return 0;}
            regions[i].data=regions[links[i]].data+offsets[i];
        }
        if(!regions[i].data){boot_ckpt_log("load: region not resolved",i,regions[i].base);return 0;}
    }
    /* Window: logical state from the checkpoint, host objects from this process. */
    if(sw){
        AcquireSRWLockExclusive(&w->lock);
        memcpy(w->messages,sw->messages,sizeof w->messages);w->head=sw->head;w->count=sw->count;
        w->focus=sw->focus;w->closed=sw->closed;w->error=sw->error;
        w->foreground_owner=sw->foreground_owner;w->focus_policy_set=sw->focus_policy_set;w->out_of_focus_suspend=sw->out_of_focus_suspend;
        w->resume_notifications=sw->resume_notifications;w->suspend_observed=sw->suspend_observed;
        w->operation_mode_notifications=sw->operation_mode_notifications;w->performance_mode_notifications=sw->performance_mode_notifications;
        w->cpu_boost_mode=sw->cpu_boost_mode;
        memcpy(w->display_groups,sw->display_groups,sizeof w->display_groups);memcpy(w->display_opens,sw->display_opens,sizeof w->display_opens);
        w->managed_layer_owner=sw->managed_layer_owner;memcpy(w->managed_layer_ids,sw->managed_layer_ids,sizeof w->managed_layer_ids);
        w->producer_next_id=sw->producer_next_id;memcpy(w->producer_ids,sw->producer_ids,sizeof w->producer_ids);
        memcpy(w->producer_opens,sw->producer_opens,sizeof w->producer_opens);memcpy(w->producer_refs,sw->producer_refs,sizeof w->producer_refs);
        memcpy(w->buffer_queues,sw->buffer_queues,sizeof w->buffer_queues);w->graphic_buffers=sw->graphic_buffers;
        for(unsigned i=0;i<2;++i)w->producer_events[i]=owners[CKPT_OWNER_PRODUCER0+i]>=0?objects[owners[CKPT_OWNER_PRODUCER0+i]].native:NULL;
        /* Buffers held for a guest vsync (host_present_hold.h) lost their release timers: free them now. */
        for(unsigned i=0;i<2;++i)if(boot_present_release(w,i,1))boot_present_signal(w,i);
        for(unsigned i=0;i<2;++i)w->nv_control_events[i]=owners[CKPT_OWNER_CONTROL0+i]>=0?objects[owners[CKPT_OWNER_CONTROL0+i]].native:NULL;
        w->nv_device_next=sw->nv_device_next;memcpy(w->nv_device_fds,sw->nv_device_fds,sizeof w->nv_device_fds);
        memcpy(w->nv_device_kinds,sw->nv_device_kinds,sizeof w->nv_device_kinds);
        memcpy(w->nv_address_spaces,sw->nv_address_spaces,sizeof w->nv_address_spaces);
        free(w->nvmap.objects);w->nvmap=sw->nvmap;w->nvmap.objects=nvmap;
        w->gpu_mappings=sw->gpu_mappings;w->zbc=sw->zbc;w->nv_syncpoint_next=sw->nv_syncpoint_next;
        if(sw->gpu && !w->gpu)w->gpu=boot_gpu_open();
        int channels_ok=!sw->gpu || w->gpu;
        for(unsigned i=0;i<16;++i){BootGpuChannel*ch=&w->nv_channels[i];
            *ch=sw->nv_channels[i];ch->entries=entries[i];ch->state=stages[i];ch->pending=NULL;ch->allocator=NULL;
            ch->error_event=owners[CKPT_OWNER_ERROR0+i]>=0?objects[owners[CKPT_OWNER_ERROR0+i]].native:NULL;
            if(sw->nv_channels[i].allocator && w->gpu &&
               FAILED(ID3D12Device_CreateCommandAllocator(w->gpu->device,D3D12_COMMAND_LIST_TYPE_DIRECT,&IID_ID3D12CommandAllocator,(void**)&ch->allocator)))channels_ok=0;
        }
        BootHidMemory hid=sw->hid_memory;hid.mapping=w->hid_memory.mapping;hid.data=w->hid_memory.data;w->hid_memory=hid;
        ReleaseSRWLockExclusive(&w->lock);
        if(!channels_ok){boot_ckpt_log("load: GPU channel restore failed",0,0);return 0;}
    }
    /* Context: the checkpoint, with this process's host pointers and handles. */
    uint64_t window=c->window_native,storage=c->storage_native,performance=c->performance_native;
    memcpy(c,s,sizeof(BootContext));free(s);
    c->window_native=window;c->storage_native=storage;c->performance_native=performance;
    for(unsigned i=0;i<c->region_count;++i)c->regions[i].data=regions[i].data;
    for(unsigned i=0;i<3;++i)c->event_source_handles[i]=owners[CKPT_OWNER_SOURCE0+i]>=0?(uint64_t)(uintptr_t)objects[owners[CKPT_OWNER_SOURCE0+i]].native:0;
    for(unsigned i=0;i<16;++i){
        c->event_native_handles[i]=0;
        if(slots[i]<0)continue;
        HANDLE copy=NULL;
        if(!DuplicateHandle(GetCurrentProcess(),objects[slots[i]].native,GetCurrentProcess(),&copy,0,FALSE,DUPLICATE_SAME_ACCESS)){
            boot_ckpt_log("load: event duplicate failed",i,GetLastError());return 0;}
        c->event_native_handles[i]=(uint64_t)(uintptr_t)copy;
    }
    for(unsigned t=0;t<BOOT_THREAD_CAPACITY;++t){BootThread*th=&c->threads[t];
        if(t>=c->thread_count || th->state!=5)continue;
        for(unsigned i=0;i<64;++i){
            if(waits[t][i]<0){if(!th->wait_threads[i])th->wait_native[i]=0;continue;}
            HANDLE copy=NULL;
            if(!DuplicateHandle(GetCurrentProcess(),objects[waits[t][i]].native,GetCurrentProcess(),&copy,0,FALSE,DUPLICATE_SAME_ACCESS)){
                boot_ckpt_log("load: wait duplicate failed",t,i);return 0;}
            th->wait_native[i]=(uint64_t)(uintptr_t)copy;
        }
    }
    c->exclusive_valid=0;
    for(uint32_t k=0;k<object_count;++k)free(objects[k].content);
    free(sw);
    boot_ckpt_log("load: ok steps/regions",c->steps,c->region_count);
    return 1;
}
#endif
