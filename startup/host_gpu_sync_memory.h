#ifndef TOTK_HOST_GPU_SYNC_MEMORY_H
#define TOTK_HOST_GPU_SYNC_MEMORY_H
#include "host_gpu_memory_batch.h"
#include "host_gpu_write_plan.h"
#include "guest_clock.h"
#include <stdio.h>
/* Diagnostic: why a commit was refused (appended to sync-memory-reject.txt). */
static inline int boot_gpu_sync_memory_reject(const char*why,uint64_t a,uint64_t b){
    FILE*f=fopen("sync-memory-reject.txt","a");if(f){fprintf(f,"%s %llu %llu\n",why,(unsigned long long)a,(unsigned long long)b);fclose(f);}return 0;}
/* Report timestamps: host time in nanoseconds (QPC), strictly increasing across reports. The
 * submission's GPU work has completed before the writes execute (draw execution waits for the
 * queue), so a report taken here is ordered after the work preceding it, which is what the source
 * timestamp guarantees. Reading the host clock needs no GPU round trip per report. */
static uint64_t boot_gpu_sync_last_ns, boot_gpu_sync_reports, boot_gpu_sync_batches, boot_gpu_sync_bytes;
static inline int boot_gpu_sync_host_ns(uint64_t*out){
    if(boot_guest_dilated()){   /* guest-visible: report the guest clock (guest_clock.h) */
        uint64_t ns=boot_guest_gpu_ns();
        if(ns<=boot_gpu_sync_last_ns)ns=boot_gpu_sync_last_ns+2;
        boot_gpu_sync_last_ns=ns;*out=ns;return 1;
    }
    LARGE_INTEGER counter,frequency;
    if(!QueryPerformanceCounter(&counter) || !QueryPerformanceFrequency(&frequency) || frequency.QuadPart<=0 || counter.QuadPart<0)return 0;
    uint64_t ns=(uint64_t)((__uint128_t)(uint64_t)counter.QuadPart*1000000000/(uint64_t)frequency.QuadPart);
    if(ns<=boot_gpu_sync_last_ns)ns=boot_gpu_sync_last_ns+2;   /* +2 ns: distinct after the 384/625 tick scaling */
    boot_gpu_sync_last_ns=ns;*out=ns;return 1;
}
/* Execute ordered writes into private backing, taking timestamps at the source
 * report points. Guest backing commits only after every operation and the
 * final GPU copy/readback have completed and verified successfully. */
static inline int boot_gpu_sync_memory_execute(BootGpu*g,const BootGpuWritePlan*plan,
        const BootGpuMemorySpan*spans,const BootGpuMemoryWrite*w,unsigned count,
        uint32_t syncpoint,uint32_t increment,BootGpuMemoryBatch**pending){
    if(!g || !plan || plan->count>16 || !spans || (!w && count) || count>BOOT_GPU_STAGE_MAX_WRITES || !pending || *pending)return boot_gpu_sync_memory_reject(pending && *pending?"pending":"args",count,plan?plan->count:0);
    unsigned reports=0,increments=0;
    for(unsigned i=0;i<count;++i){
        if(w[i].kind==3){if(w[i].gpu!=syncpoint || w[i].value!=1)return boot_gpu_sync_memory_reject("increment",w[i].gpu,w[i].value);++increments;continue;}
        if(w[i].kind==1){
            if(++reports>4096 || i+1==count || w[i+1].kind!=2 || w[i+1].gpu!=w[i].gpu+4)return boot_gpu_sync_memory_reject("report",reports,i);
            ++i;
        }else if(w[i].kind)return boot_gpu_sync_memory_reject("kind",w[i].kind,i);
    }
    if(increments!=increment)return boot_gpu_sync_memory_reject("increments",increments,increment);
    size_t total=0;
    for(unsigned j=0;j<plan->count;++j){
        uintptr_t address=(uintptr_t)spans[j].destination;
        if(!address || !spans[j].contents || spans[j].size!=plan->spans[j].size || !spans[j].size ||
           spans[j].size>16777216 || address>UINTPTR_MAX-spans[j].size || total>67108864-spans[j].size)return boot_gpu_sync_memory_reject("span",j,spans[j].size);
        total+=spans[j].size;
        for(unsigned k=0;k<j;++k){uintptr_t other=(uintptr_t)spans[k].destination;
            if(address<other+spans[k].size && other<address+spans[j].size)return boot_gpu_sync_memory_reject("overlap",j,k);}
    }
    BootGpuMemorySpan scratch[16]={0};int ok=1;
    for(unsigned j=0;j<plan->count;++j){
        if(!spans[j].destination || !spans[j].contents || spans[j].size!=plan->spans[j].size || !spans[j].size || spans[j].size>16777216){ok=0;break;}
        scratch[j].destination=malloc(spans[j].size);scratch[j].contents=scratch[j].destination;scratch[j].size=spans[j].size;
        if(!scratch[j].destination){ok=0;break;}memcpy(scratch[j].destination,spans[j].contents,spans[j].size);
    }
    for(unsigned i=0;ok && i<count;++i){
        if(w[i].kind==3)continue;
        uint64_t value=w[i].value;unsigned bytes=4;
        if(w[i].kind==1){
            uint64_t ns=0;
            if(!(ok=boot_gpu_sync_host_ns(&ns)))break;
            ++boot_gpu_sync_reports;
            value=(uint64_t)((__uint128_t)ns*384/625);bytes=8;++i;
        }
        uint64_t address=bytes==8?w[i-1].gpu:w[i].gpu;
        /* The caller has already validated sparse holes through the mapping
         * plan. Those writes intentionally have no CPU backing to commit. */
        for(unsigned word=0;word<bytes/4;++word){uint64_t target=address+word*4;
            for(unsigned j=0;j<plan->count;++j)if(target>=plan->spans[j].gpu &&
                    target-plan->spans[j].gpu<=scratch[j].size && 4<=scratch[j].size-(target-plan->spans[j].gpu)){
                for(unsigned k=0;k<4;++k)scratch[j].destination[target-plan->spans[j].gpu+k]=(uint8_t)(value>>(8*(word*4+k)));break;
            }
        }
    }
    if(ok && plan->count){BootGpuMemorySpan final[16]={0};++boot_gpu_sync_batches;
        for(unsigned j=0;j<plan->count;++j){final[j]=(BootGpuMemorySpan){spans[j].destination,scratch[j].contents,scratch[j].size};boot_gpu_sync_bytes+=scratch[j].size;}
        BootGpuMemoryBatch*b=boot_gpu_memory_batch_prepare(g,final,plan->count);ok=b && boot_gpu_memory_batch_execute(g,b);
        if(b && !boot_gpu_memory_batch_close(b)){*pending=b;ok=0;}
    }else if(ok)ok=boot_gpu_wait(g);
    for(unsigned j=0;j<16;++j)free(scratch[j].destination);
    if(!ok)boot_gpu_sync_memory_reject("execute",count,plan->count);
    return ok;
}
#endif
