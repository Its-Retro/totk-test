#ifndef TOTK_HOST_NVMAP_H
#define TOTK_HOST_NVMAP_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
/* Object identity precedes backing allocation. Creation alone owns no RAM.
 * refs (nvmap reference semantics, play-90 open-world stop): bits 0-30 count the handle's references
 * (CREATE makes one, FROM_ID adds one, FREE drops one; 0 in objects saved by older checkpoints counts as
 * one), bit 31 (BOOT_NVMAP_RETAINED) marks an object whose handle the guest has freed while GPU mappings
 * or display buffers still use it: it no longer answers handle lookups, only the internal
 * boot_nvmap_find of its users, and the runtime drops it once they are gone. refs sits in what was
 * padding, so sizeof(BootNvMapObject) and the checkpoint layout are unchanged. */
typedef struct BootNvMapObject {uint32_t handle,size,owner_fd,allocated,alignment,flags,kind,refs;uint64_t address;} BootNvMapObject;
typedef struct BootNvMap {uint32_t next_handle,capacity;BootNvMapObject*objects;} BootNvMap;
#define BOOT_NVMAP_RETAINED UINT32_C(0x80000000)
/* Off switch: TOTK_NVMAP_LEGACY=1 restores the pre-play-91 handling (lookups match the creating fd, FREE
 * removes the object at once and is refused while it is mapped or displayed, FROM_ID unsupported, and every
 * request on an unknown handle stops the guest instead of failing with the driver's NvError). */
static inline int boot_nvmap_exact(void){
    static volatile long state;long s=state;
    if(!s){const char*e=getenv("TOTK_NVMAP_LEGACY");s=e && *e && *e!='0'?2:1;state=s;}
    return s==1;
}
static inline int boot_nvmap_retained(const BootNvMapObject*o){return (o->refs&BOOT_NVMAP_RETAINED)!=0;}
static inline uint32_t boot_nvmap_references(const BootNvMapObject*o){uint32_t n=o->refs&~BOOT_NVMAP_RETAINED;return n?n:1;}
static inline uint32_t boot_nvmap_create(BootNvMap*m,uint32_t fd,uint32_t size){
    if(!fd || !size || size>UINT32_MAX-4095 || m->next_handle>UINT32_MAX-4)return 0;
    unsigned i=0;while(i<m->capacity && m->objects[i].handle)++i;
    if(i==m->capacity){
        /* Keep object identities stable while letting real games exceed the
         * original startup fixture's 128-object capacity. */
        if(m->capacity>=65536)return 0;
        uint32_t old=m->capacity,next=old?old*2:128;
        if(next>65536)next=65536;
        BootNvMapObject*grown=realloc(m->objects,(size_t)next*sizeof(*grown));
        if(!grown)return 0;
        memset(grown+old,0,(size_t)(next-old)*sizeof(*grown));
        m->objects=grown;m->capacity=next;
    }
    uint32_t handle=m->next_handle+4;
    m->objects[i].handle=handle;m->objects[i].size=(size+4095)&~UINT32_C(4095);m->objects[i].owner_fd=fd;m->objects[i].refs=1;
    m->next_handle=handle;return handle;
}
/* Internal lookup by the owner recorded in a GPU mapping / display buffer: retained objects included. */
static inline BootNvMapObject*boot_nvmap_find(BootNvMap*m,uint32_t fd,uint32_t handle){
    if(!fd || !handle)return NULL;
    for(unsigned i=0;i<m->capacity;++i)if(m->objects[i].handle==handle && m->objects[i].owner_fd==fd)return &m->objects[i];
    return NULL;
}
/* Handle lookup (MAP_BUFFER_EX, REMAP, display buffers): live handles only. */
static inline BootNvMapObject*boot_nvmap_find_handle(BootNvMap*m,uint32_t handle){
    if(!handle)return NULL;
    for(unsigned i=0;i<m->capacity;++i)if(m->objects[i].handle==handle && !boot_nvmap_retained(&m->objects[i]))return &m->objects[i];
    return NULL;
}
/* Diagnostics: the object holding `handle` on any fd, retained objects included. */
static inline BootNvMapObject*boot_nvmap_find_any(BootNvMap*m,uint32_t handle){
    if(!handle)return NULL;
    for(unsigned i=0;i<m->capacity;++i)if(m->objects[i].handle==handle)return &m->objects[i];
    return NULL;
}
/* nvmap ioctl lookup on nvmap fd `fd`. The guest driver keeps one handle table per process, shared by
 * every nvmap fd it opened; legacy mode keeps the old match on the creating fd. */
static inline BootNvMapObject*boot_nvmap_lookup(BootNvMap*m,uint32_t fd,uint32_t handle){
    if(!fd || !handle)return NULL;
    if(!boot_nvmap_exact())return boot_nvmap_find(m,fd,handle);
    return boot_nvmap_find_handle(m,handle);
}
/* NVMAP_IOC_FROM_ID: IDs and handles are the same value; importing adds a handle reference. */
static inline uint32_t boot_nvmap_import(BootNvMap*m,uint32_t fd,uint32_t id){
    BootNvMapObject*o=boot_nvmap_lookup(m,fd,id);
    if(!o)return 0;
    uint32_t n=boot_nvmap_references(o);if(n>=~BOOT_NVMAP_RETAINED)return 0;
    o->refs=n+1;return o->handle;
}
/* NVMAP_IOC_FREE (exact mode) on a live object: drops one handle reference. *out: the object as it was.
 * 1: references remain (the guest is told "not freed yet"), 2: handle and object are gone, 3: the handle
 * is gone but the object is retained because `used` (GPU mappings / display buffers still use it). */
static inline unsigned boot_nvmap_release(BootNvMapObject*o,int used,BootNvMapObject*out){
    uint32_t n=boot_nvmap_references(o);*out=*o;
    if(n>1){o->refs=n-1;return 1;}
    if(used){o->refs=BOOT_NVMAP_RETAINED;return 3;}
    memset(o,0,sizeof(*o));return 2;
}
static inline int boot_nvmap_free(BootNvMap*m,uint32_t fd,uint32_t handle,BootNvMapObject*out){
    if(!fd || !handle)return 0;
    for(unsigned i=0;i<m->capacity;++i)if(m->objects[i].handle==handle && m->objects[i].owner_fd==fd){
        *out=m->objects[i];memset(&m->objects[i],0,sizeof(m->objects[i]));return 1;
    }
    return 0;
}
static inline void boot_nvmap_close(BootNvMap*m,uint32_t fd){
    for(unsigned i=0;i<m->capacity;++i)if(m->objects[i].owner_fd==fd)memset(&m->objects[i],0,sizeof(m->objects[i]));
}
static inline void boot_nvmap_destroy(BootNvMap*m){free(m->objects);memset(m,0,sizeof(*m));}

/* Diagnostics: per-handle operation trail for nvdrv-reject.log. Handles are issued in steps of 4 and never
 * reused, so slot (handle/4) mod BOOT_NVMAP_TRAIL_SLOTS keeps the trail of each of the last 16384 handles:
 * its first operation (CREATE) and its last four. Only CREATE claims a slot; an operation on a handle
 * whose slot holds another handle is not recorded. Written under the window lock (exclusive), read by the
 * logger under the shared lock. Not part of any checkpoint. */
#define BOOT_NVMAP_TRAIL_SLOTS 16384
enum {BOOT_NVMAP_OP_CREATE=1,BOOT_NVMAP_OP_ALLOC,BOOT_NVMAP_OP_FREE,BOOT_NVMAP_OP_PARAM,BOOT_NVMAP_OP_GET_ID,
      BOOT_NVMAP_OP_FROM_ID,BOOT_NVMAP_OP_MAP,BOOT_NVMAP_OP_REMAP,BOOT_NVMAP_OP_DROP};
/* result: 0 done, an NvError returned to the guest, or UINT32_MAX for a rejected request (guest stop).
 * value: CREATE size, ALLOC flags, FREE outcome (boot_nvmap_release; 0 unknown handle), PARAM parameter,
 * MAP / REMAP GPU address >> 12 (low 32 bits), DROP (retained object released) 0. refs: after the op. */
typedef struct BootNvMapEvent {uint64_t steps,lr,caller;uint32_t thread,fd,op,result,refs,value;} BootNvMapEvent;
typedef struct BootNvMapTrail {uint32_t handle,count;BootNvMapEvent first,last[4];} BootNvMapTrail;
static inline BootNvMapTrail*boot_nvmap_trails(void){static BootNvMapTrail trails[BOOT_NVMAP_TRAIL_SLOTS];return trails;}
static inline const BootNvMapTrail*boot_nvmap_trail(uint32_t handle){
    const BootNvMapTrail*t=&boot_nvmap_trails()[(handle>>2)&(BOOT_NVMAP_TRAIL_SLOTS-1)];
    return handle && t->handle==handle?t:NULL;
}
static inline void boot_nvmap_trail_add(uint32_t handle,const BootNvMapEvent*e){
    if(!handle)return;
    BootNvMapTrail*t=&boot_nvmap_trails()[(handle>>2)&(BOOT_NVMAP_TRAIL_SLOTS-1)];
    if(t->handle!=handle){if(e->op!=BOOT_NVMAP_OP_CREATE)return;memset(t,0,sizeof(*t));t->handle=handle;}
    if(!t->count)t->first=*e;else t->last[(t->count-1)&3]=*e;
    if(t->count<UINT32_MAX)++t->count;
}
static inline const char*boot_nvmap_op_name(uint32_t op){
    static const char*const names[]={"?","create","alloc","free","param","get_id","from_id","map","remap","drop"};
    return op<sizeof(names)/sizeof(names[0])?names[op]:"?";
}
#endif
