#ifndef TOTK_CHAIN_RUNTIME_H
#define TOTK_CHAIN_RUNTIME_H
#include <stdint.h>
#include <stddef.h>

enum { CH_OK, CH_ADDRESS, CH_PERMISSION, CH_BUDGET, CH_UNTRANSLATED,
       CH_RETURN, CH_STACK, CH_ARGUMENT, CH_DEPTH };
enum { READ=1, WRITE=2, EXECUTE=4 };
typedef struct Region {
    uint64_t base, size;
    uint8_t *data;
    uint32_t permissions, reserved;
} Region;
typedef struct Context {
    Region regions[16];
    uint32_t region_count, status;
    uint64_t x[31];
    uint64_t sp, main_base, limit, steps, visited, taken, not_taken;
    uint64_t calls, max_depth, fault_pc, fault_address, fault_width;
    uint32_t z, depth;
} Context;

static inline void fault(Context *c, unsigned status, uint64_t pc, uint64_t address, uint64_t width) {
    if (!c->status) {
        c->status=status; c->fault_pc=pc; c->fault_address=address; c->fault_width=width;
    }
}
static inline uint8_t *access_guest(Context *c,uint64_t address,unsigned width,unsigned permission,uint64_t pc) {
    if(c->status)return NULL;
    for(unsigned i=0;i<c->region_count;++i) {
        Region *r=&c->regions[i];
        if(address>=r->base && width<=r->size && address-r->base<=r->size-width) {
            if((r->permissions&permission)!=permission) {fault(c,CH_PERMISSION,pc,address,width);return NULL;}
            return r->data+(size_t)(address-r->base);
        }
    }
    fault(c,CH_ADDRESS,pc,address,width);return NULL;
}
static inline uint64_t little_read(const uint8_t *p,unsigned width) {
    uint64_t value=0;for(unsigned i=0;i<width;++i)value|=(uint64_t)p[i]<<(8*i);return value;
}
static inline void little_write(uint8_t *p,uint64_t value,unsigned width) {
    for(unsigned i=0;i<width;++i)p[i]=(uint8_t)(value>>(8*i));
}
static inline int load_guest(Context*c,uint64_t a,unsigned w,uint64_t pc,uint64_t*out) {
    uint8_t*p=access_guest(c,a,w,READ,pc);if(!p)return 0;*out=little_read(p,w);return 1;
}
static inline int save_frame(Context*c,uint64_t pc) {
    if(c->sp&15) {fault(c,CH_STACK,pc,c->sp,16);return 0;}
    uint64_t address=c->sp-16;
    uint8_t*p=access_guest(c,address,16,WRITE,pc);if(!p)return 0;
    little_write(p,c->x[29],8);little_write(p+8,c->x[30],8);c->sp=address;return 1;
}
static inline int restore_frame(Context*c,uint64_t pc) {
    if(c->sp&15) {fault(c,CH_STACK,pc,c->sp,16);return 0;}
    uint8_t*p=access_guest(c,c->sp,16,READ,pc);if(!p)return 0;
    c->x[29]=little_read(p,8);c->x[30]=little_read(p+8,8);c->sp+=16;return 1;
}
static inline int tick(Context*c,uint64_t offset,unsigned index) {
    uint64_t pc=c->main_base+offset;
    if(c->status)return 0;
    if(c->steps>=c->limit){fault(c,CH_BUDGET,pc,0,0);return 0;}
    if(!access_guest(c,pc,4,EXECUTE,pc))return 0;
    ++c->steps;c->visited|=UINT64_C(1)<<index;return 1;
}
static inline int branch(Context*c,unsigned index,int taken) {
    if(taken)c->taken|=UINT64_C(1)<<index;else c->not_taken|=UINT64_C(1)<<index;
    return taken;
}
static inline int valid_context(Context*c) {
    if(c->region_count>16 || !c->main_base || c->main_base>UINT64_MAX-UINT64_C(0x5000000))return 0;
    for(unsigned i=0;i<c->region_count;++i) {
        Region*r=&c->regions[i];
        if(!r->base || !r->size || !r->data || r->size>SIZE_MAX || r->size>UINT64_MAX-r->base ||
           (r->permissions&~7u))return 0;
        for(unsigned j=0;j<i;++j) {
            Region*s=&c->regions[j];
            if(r->base<s->base+s->size && s->base<r->base+r->size)return 0;
        }
    }
    return 1;
}
__declspec(dllexport) uint64_t chain_run(Context*c,uint64_t descriptor);
/* Registry entry for explicit unresolved-target testing; never executes raw ARM. */
__declspec(dllexport) uint64_t chain_run_address(Context*c,uint64_t address,uint64_t argument);
#endif
