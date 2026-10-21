#ifndef TOTK_HOST_STORAGE_H
#define TOTK_HOST_STORAGE_H
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct BootStorage { HANDLE file; SRWLOCK lock; uint64_t size; } BootStorage;
#define BOOT_STORAGE_READ_LIMIT (16u*1024u*1024u)
static inline uint64_t boot_storage_u64(const uint8_t*p){
    uint64_t value=0;for(unsigned i=0;i<8;++i)value|=(uint64_t)p[i]<<(8*i);return value;
}
static inline BootStorage*boot_storage_open(const wchar_t*path){
    if(!path)return NULL;
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if(file==INVALID_HANDLE_VALUE)return NULL;
    LARGE_INTEGER size;uint8_t header[80];DWORD read=0;
    if(!GetFileSizeEx(file,&size) || size.QuadPart<80 ||
       !ReadFile(file,header,sizeof(header),&read,NULL) || read!=sizeof(header) ||
       boot_storage_u64(header)!=80)goto invalid;
    uint64_t bytes=(uint64_t)size.QuadPart,data=boot_storage_u64(header+72);
    if(data<80 || data>bytes)goto invalid;
    for(unsigned i=8;i<72;i+=16){
        uint64_t offset=boot_storage_u64(header+i),length=boot_storage_u64(header+i+8);
        if(offset<80 || offset>bytes || length>bytes-offset)goto invalid;
    }
    BootStorage*s=calloc(1,sizeof(*s));if(!s)goto invalid;
    s->file=file;s->size=bytes;InitializeSRWLock(&s->lock);return s;
invalid:
    CloseHandle(file);return NULL;
}
static inline int boot_storage_read(BootStorage*s,uint64_t offset,uint64_t size,void*output){
    if(!s || offset>s->size || size>s->size-offset || size>BOOT_STORAGE_READ_LIMIT || (size && !output))return 0;
    if(!size)return 1;
    uint8_t*temporary=malloc((size_t)size);if(!temporary)return 0;
    LARGE_INTEGER position;position.QuadPart=(LONGLONG)offset;DWORD read=0;
    AcquireSRWLockExclusive(&s->lock);
    int ok=SetFilePointerEx(s->file,position,NULL,FILE_BEGIN) &&
           ReadFile(s->file,temporary,(DWORD)size,&read,NULL) && read==size;
    ReleaseSRWLockExclusive(&s->lock);
    if(ok)memcpy(output,temporary,(size_t)size);
    free(temporary);return ok;
}
static inline void boot_storage_close(BootStorage*s){if(s){CloseHandle(s->file);free(s);}}
#endif
