#ifndef TOTK_HOST_HID_MEMORY_H
#define TOTK_HOST_HID_MEMORY_H
#include <windows.h>
#include <stdint.h>
#define BOOT_HID_MEMORY_SIZE UINT32_C(0x40000)
#define BOOT_HID_POINTER_CAPACITY 256
typedef struct BootHidMemory {HANDLE mapping;uint8_t*data;uint64_t owner;uint32_t npad_active,npad_revision;uint8_t pointer_buffer[BOOT_HID_POINTER_CAPACITY];uint32_t npad_ids[10],npad_id_count,npad_styles;uint32_t peripheral_active;} BootHidMemory;
/* An initially disconnected input backend: all LIFO counts and style sets are
 * zero. No samples or controller connection are fabricated. */
static inline HANDLE boot_hid_memory_copy(BootHidMemory*h,uint64_t owner){
    if(!h || !owner || (h->owner && h->owner!=owner))return NULL;
    HANDLE mapping=h->mapping;uint8_t*data=h->data;int created=0;
    if(!mapping){
        mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,NULL,PAGE_READWRITE,0,BOOT_HID_MEMORY_SIZE,NULL);if(!mapping)return NULL;
        data=MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,BOOT_HID_MEMORY_SIZE);
        if(!data){CloseHandle(mapping);return NULL;}created=1;
    }
    HANDLE copy=NULL;
    if(!DuplicateHandle(GetCurrentProcess(),mapping,GetCurrentProcess(),&copy,0,FALSE,DUPLICATE_SAME_ACCESS)){
        if(created){UnmapViewOfFile(data);CloseHandle(mapping);}return NULL;
    }
    if(created){h->mapping=mapping;h->data=data;h->owner=owner;}
    return copy;
}
static inline void boot_hid_memory_close(BootHidMemory*h){
    if(!h)return;
    if(h->data)UnmapViewOfFile(h->data);
    if(h->mapping)CloseHandle(h->mapping);
    h->data=NULL;h->mapping=NULL;h->owner=0;
    h->npad_active=0;h->npad_revision=0;h->peripheral_active=0;
}
#endif
