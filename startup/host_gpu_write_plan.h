#ifndef TOTK_HOST_GPU_WRITE_PLAN_H
#define TOTK_HOST_GPU_WRITE_PLAN_H
#include "host_gpu_mapping.h"
#include "host_gpu_command_stage.h"
#include <stdio.h>
#include <stdlib.h>
/* Resolve the whole plan before accessing or changing guest memory. Group by
 * backing mapping so sparse writes preserve the original bytes between them. */
typedef struct BootGpuWritePlan {
    struct {uint64_t gpu,cpu,size;unsigned mapping;} spans[16];
    unsigned count;
} BootGpuWritePlan;
/* Diagnostic: why a plan was refused (appended to write-plan-reject.txt). */
static inline int boot_gpu_write_plan_reject(const char*why,uint64_t a,uint64_t b){
    FILE*f=fopen("write-plan-reject.txt","a");if(f){fprintf(f,"%s 0x%llx 0x%llx\n",why,(unsigned long long)a,(unsigned long long)b);fclose(f);}return 0;}
typedef struct BootGpuWriteCandidate { uint64_t gpu,last;unsigned k; } BootGpuWriteCandidate;
static inline int boot_gpu_write_candidate_order(const void*a,const void*b){
    uint64_t x=((const BootGpuWriteCandidate*)a)->gpu,y=((const BootGpuWriteCandidate*)b)->gpu;return x<y?-1:x>y;}
static inline int boot_gpu_write_plan_sparse(const BootGpuMappings*m,const BootGpuAddressSpace*a,uint32_t as_fd,uint32_t owner_fd,
        const BootGpuMemoryWrite*w,unsigned count,BootGpuWritePlan*out){
    if(!m || !out || (!w && count) || count>BOOT_GPU_STAGE_MAX_WRITES || !as_fd || !owner_fd)return boot_gpu_write_plan_reject("args",count,as_fd);
    BootGpuWritePlan plan={0};
    /* The mappings a write can use, sorted by GPU address. When their ranges are disjoint (the usual
     * case) the first mapping in table order containing an address is the only one, found by binary
     * search; otherwise every write scans the table in order as before. */
    static BootGpuWriteCandidate cand[BOOT_GPU_MAPPING_CAPACITY];unsigned n=0;int disjoint=1;
    for(unsigned k=0;k<BOOT_GPU_MAPPING_CAPACITY;++k){const BootGpuMapping*r=&m->entries[k];
        uint64_t backed=boot_gpu_mapping_backing_size(r);
        if(r->as_fd==as_fd && r->owner_fd==owner_fd && !r->kind && backed>=4){
            if(r->gpu>UINT64_MAX-(backed-4))disjoint=0;
            cand[n].gpu=r->gpu;cand[n].last=r->gpu+(backed-4);cand[n++].k=k;
        }
    }
    if(disjoint && n>1){qsort(cand,n,sizeof cand[0],boot_gpu_write_candidate_order);
        for(unsigned j=1;j<n && disjoint;++j)disjoint=cand[j].gpu>cand[j-1].last;}
    for(unsigned i=0;i<count;++i){
        if(w[i].kind==3)continue;
        if(w[i].kind>3)return boot_gpu_write_plan_reject("kind",w[i].kind,i);
        uint64_t address=w[i].gpu;if((address&3) || address>UINT64_MAX-4)return boot_gpu_write_plan_reject("align",address,i);
        unsigned k=BOOT_GPU_MAPPING_CAPACITY;
        if(disjoint){unsigned lo=0,hi=n;   /* first candidate with gpu > address */
            while(lo<hi){unsigned mid=(lo+hi)/2;if(cand[mid].gpu<=address)lo=mid+1;else hi=mid;}
            if(lo && address<=cand[lo-1].last)k=cand[lo-1].k;
        }else for(k=0;k<BOOT_GPU_MAPPING_CAPACITY;++k){const BootGpuMapping*r=&m->entries[k];
            uint64_t backed=boot_gpu_mapping_backing_size(r);
            if(r->as_fd==as_fd && r->owner_fd==owner_fd && !r->kind && backed>=4 &&
               address>=r->gpu && address-r->gpu<=backed-4)break;}
        if(k==BOOT_GPU_MAPPING_CAPACITY){uint64_t cpu=0,length=0;
            if(a && boot_gpu_memory_segment(m,a,as_fd,owner_fd,address,4,&cpu,&length) && !cpu && length==4)continue;
            return boot_gpu_write_plan_reject("unmapped",address,i);
        }
        const BootGpuMapping*r=&m->entries[k];
        if(r->cpu>UINT64_MAX-(address-r->gpu)-4)return boot_gpu_write_plan_reject("cpu-range",address,i);
        unsigned j=0;for(;j<plan.count;++j)if(plan.spans[j].mapping==k)break;
        if(j==plan.count){if(plan.count==16)return boot_gpu_write_plan_reject("spans>16",address,i);++plan.count;
            plan.spans[j].mapping=k;plan.spans[j].gpu=address;plan.spans[j].cpu=r->cpu+address-r->gpu;plan.spans[j].size=4;
        }else{
            uint64_t low=address<plan.spans[j].gpu?address:plan.spans[j].gpu;
            uint64_t end=plan.spans[j].gpu+plan.spans[j].size;if(address+4>end)end=address+4;
            if(end-low>16777216 || r->cpu>UINT64_MAX-(low-r->gpu)-(end-low))return boot_gpu_write_plan_reject("span>16MB",low,end);
            plan.spans[j].gpu=low;plan.spans[j].cpu=r->cpu+low-r->gpu;plan.spans[j].size=end-low;
        }
    }
    uint64_t total=0;
    for(unsigned i=0;i<plan.count;++i){total+=plan.spans[i].size;if(total>67108864)return boot_gpu_write_plan_reject("total>64MB",total,i);
        for(unsigned j=0;j<i;++j)if(plan.spans[i].cpu<plan.spans[j].cpu+plan.spans[j].size &&
            plan.spans[j].cpu<plan.spans[i].cpu+plan.spans[i].size)return boot_gpu_write_plan_reject("overlap",plan.spans[i].cpu,plan.spans[j].cpu);
    }
    *out=plan;return 1;
}
static inline int boot_gpu_write_plan(const BootGpuMappings*m,uint32_t as_fd,uint32_t owner_fd,
        const BootGpuMemoryWrite*w,unsigned count,BootGpuWritePlan*out){
    return boot_gpu_write_plan_sparse(m,NULL,as_fd,owner_fd,w,count,out);
}
#endif
