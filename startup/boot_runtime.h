#ifndef TOTK_BOOT_RUNTIME_H
#define TOTK_BOOT_RUNTIME_H
#include "play_config.h"
#include "../call-chain/runtime.h"
#include <string.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <windows.h>
#include <emmintrin.h>   /* SQRTSS/SQRTSD for the native FSQRT path (BOOT_FP_NATIVE) */
#include "host_limits.h"
#include "host_window.h"
#include "boot_service_access.h"
#include "float32.h"
#include "host_storage.h"
#include "host_performance.h"
#include "memory_attributes.h"
#include "core_schedule.h"
#include "host_audren_voices.h"

enum { BOOT_SVC=10, BOOT_TARGET=11, BOOT_QUERY_SCOPE=12, BOOT_SYSREG=13, BOOT_WINDOW_CLOSED=14, BOOT_FP_SCOPE=15 };
#define BOOT_TRACE_CAPACITY 4096
typedef struct BootAlias { uint64_t source,destination,size; } BootAlias;
typedef struct BootTransfer { uint64_t base,size; uint32_t handle,permissions,owner_closed,driver_references; } BootTransfer;
typedef struct BootThread {
    uint64_t x[31],sp,pc,tpidrro,tpidr,id,core_mask,wait_address;
    uint8_t q[32][16];
    uint32_t n,z,carry,v,handle,priority,core,state,wait_owner;
    uint64_t wait_native[64];
    uint32_t wait_threads[64],wait_count,wait_cancel;
    uint32_t fpcr,fpsr;
    CoreScheduleThread schedule;
} BootThread;
typedef struct BootContext {
    Region regions[BOOT_REGION_CAPACITY];
    uint32_t region_count,status;
    uint64_t x[31];
    uint64_t sp,rtld_base,limit,steps,fault_pc,fault_address,fault_width;
    uint64_t return_sentinel;
    uint64_t trace[BOOT_TRACE_CAPACITY];
    uint32_t trace_count,svc_id,n,z,carry,v;
    uint32_t query_enabled,query_count;
    uint64_t address_space_end;
    uint32_t memory_types[BOOT_REGION_CAPACITY];
    uint64_t checkpoint_offset;
    uint32_t terminal_query_enabled;
    uint8_t q[32][16];
    /* Software TLB ownership (see boot_access). Occupies what was alignment padding, so the
     * layout and sizeof(BootContext) are unchanged; zero (never a live epoch) in a new context. */
    uint32_t tlb_epoch;
    uint64_t code_bases[4];
    uint64_t code_sizes[4];
    uint32_t info_enabled,info_count;
    uint64_t entropy[4];
    uint32_t layout_enabled,layout_query_count;
    uint64_t layout_info[16];
    uint64_t memory_budget;
    uint64_t tpidrro;
    uint64_t tpidr,main_thread_id;
    uint32_t thread_enabled,thread_query_count,main_thread_handle,main_thread_priority;
    uint64_t exclusive_address,exclusive_value;
    uint32_t exclusive_width,exclusive_valid;
    uint32_t ipc_enabled,port_connect_count,handle_close_count,next_session_handle;
    uint32_t session_handles[BOOT_SESSION_CAPACITY];
    uint64_t session_process_ids[BOOT_SESSION_CAPACITY];
    uint32_t sm_register_count;
    uint32_t session_types[BOOT_SESSION_CAPACITY],service_lookup_count;
    uint32_t session_domains[BOOT_SESSION_CAPACITY],domain_refs[16],domain_types[16][16],cmif_control_count;
    uint32_t logger_open_count;
    uint64_t system_resource_total;
    uint32_t physical_map_count,physical_unmap_count;
    uint32_t applet_proxy_open_count;
    uint64_t applet_resource_user_id;
    uint32_t exit_to_library_applet_on_next_program;
    uint32_t clock_mode,clock_read_count;
    uint64_t clock_test_ticks;
    uint32_t event_handles[16],event_sources[16],event_create_count;
    uint64_t event_native_handles[16],event_source_handles[3];
    BootAlias aliases[BOOT_ALIAS_CAPACITY];
    uint32_t alias_count,alias_map_count,alias_unmap_count;
    BootThread threads[BOOT_THREAD_CAPACITY];
    uint32_t thread_count,current_thread,thread_create_count,thread_start_count,thread_switch_count;
    uint64_t window_native;
    uint32_t fpcr,fpsr;
    uint64_t storage_native,storage_read_count,storage_bytes_read;
    uint64_t performance_native;
    BootTransfer transfers[16];
    uint32_t nvdrv_transfer_slots[BOOT_SESSION_CAPACITY];
    BootMemoryAttributes memory_attributes;
    uint64_t debug_wait_pc,debug_wait_timeout;
    uint32_t debug_wait_count,debug_wait_handles[64];
    uint64_t exclusive_value_hi;
    uint32_t audio_device_volume_bits[3],audio_device_volume_valid;
} BootContext;
#include "guest_time.h"
#include "boot_mt_early.h"
#include "boot_scheduler.h"
/* Software TLB invalidation (defined with boot_access): call after any change to regions,
 * aliases or transfers. */
static inline void boot_tlb_flush(void);

/* Logical addresses reserve 256 MiB per source module. Physical guest bases
 * may differ independently, including across module-to-module branches. */
static inline uint64_t boot_address(BootContext*c,uint64_t logical){
    unsigned index=(unsigned)(logical>>28);
    if(index>3)return UINT64_MAX;
    return (index?c->code_bases[index]:c->rtld_base)+(logical&UINT64_C(0xfffffff));
}
static inline uint64_t boot_offset(BootContext*c,uint64_t address){
    for(unsigned i=0;i<4;++i){uint64_t base=i?c->code_bases[i]:c->rtld_base;
        if(base && address>=base && address-base<c->code_sizes[i])
            return ((uint64_t)i<<28)+(address-base);
    }return UINT64_MAX;
}
static inline void boot_fault(BootContext*c,uint32_t status,uint64_t pc,uint64_t address,uint64_t width);
#include "host_fastmem.h"
static inline void boot_finish_wait(BootThread*t,uint32_t result,int index);
static inline void boot_release_wait(BootThread*t){
    /* Only wait_count slots own duplicated synchronization handles. Other
     * thread states reuse this array for guest addresses and queue metadata. */
    for(unsigned i=0;i<64;++i){if(i<t->wait_count && t->wait_native[i])CloseHandle((HANDLE)(uintptr_t)t->wait_native[i]);t->wait_native[i]=0;t->wait_threads[i]=0;}
    t->wait_count=0;
}
static inline void boot_release_events(BootContext*c){
    if(c->window_native){boot_window_close((BootWindow*)(uintptr_t)c->window_native);c->window_native=0;}
    for(unsigned i=0;i<16;++i){
        if(c->event_native_handles[i])CloseHandle((HANDLE)(uintptr_t)c->event_native_handles[i]);
        c->event_native_handles[i]=0;c->event_handles[i]=c->event_sources[i]=0;
    }
    for(unsigned i=0;i<3;++i){
        if(c->event_source_handles[i])CloseHandle((HANDLE)(uintptr_t)c->event_source_handles[i]);
        c->event_source_handles[i]=0;
    }
}
static inline int boot_attach_window(BootContext*c,int visible){
    if(c->window_native || c->event_source_handles[1])return 0;
    BootWindow*w=boot_window_open(visible);if(!w)return 0;
    HANDLE event=NULL;
    if(!DuplicateHandle(GetCurrentProcess(),w->notification,GetCurrentProcess(),&event,0,FALSE,DUPLICATE_SAME_ACCESS)){boot_window_close(w);return 0;}
    c->window_native=(uint64_t)(uintptr_t)w;c->event_source_handles[1]=(uint64_t)(uintptr_t)event;return 1;
}
static inline uint32_t boot_new_event(BootContext*c,uint64_t pc,unsigned source){
    unsigned slot=0;while(slot<16 && c->event_handles[slot])++slot;
    if(source>=3 || slot==16 || c->next_session_handle>=0x7ffffffe){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
    HANDLE owner=(HANDLE)(uintptr_t)c->event_source_handles[source],copy=NULL;
    int created=owner==NULL;
    if(created)owner=CreateEventW(NULL,TRUE,FALSE,NULL);
    if(!owner || !DuplicateHandle(GetCurrentProcess(),owner,GetCurrentProcess(),&copy,0,FALSE,DUPLICATE_SAME_ACCESS)){
        if(created && owner)CloseHandle(owner);
        boot_fault(c,BOOT_SVC,pc,0,0);return 0;
    }
    if(c->next_session_handle<0x10000)c->next_session_handle=0x10000;
    uint32_t handle=++c->next_session_handle;
    c->event_source_handles[source]=(uint64_t)(uintptr_t)owner;
    c->event_handles[slot]=handle;c->event_sources[slot]=source;
    c->event_native_handles[slot]=(uint64_t)(uintptr_t)copy;++c->event_create_count;
    return handle;
}
/* CNTPCT_EL0: the guest clock (guest_time.h; TOTK_GUEST_CLOCK=wall selects legacy host QPC). */
static inline int boot_system_tick(BootContext*c,uint64_t pc,uint64_t*out){
    if(c->tlb_epoch==BOOT_TLB_CORE && boot_mt.system_tick)return boot_mt.system_tick(c,pc,out);   /* core: via kernel */
    if(c->clock_mode==2){*out=c->clock_test_ticks;++c->clock_read_count;return 1;}
    if(c->clock_mode!=1){boot_fault(c,BOOT_SYSREG,pc,0,0);return 0;}
    uint64_t ticks;
    if(!boot_guest_clock_now(c,&ticks)){boot_fault(c,BOOT_SYSREG,pc,0,0);return 0;}
    boot_time_tick_read(pc,c->x[30]);
    *out=ticks;++c->clock_read_count;return 1;
}
static inline int boot_get_info(BootContext*c,uint64_t pc){
    uint64_t kind=c->x[1];
    if(c->layout_enabled && kind==20 && c->x[2]==UINT64_C(0xffff8001) && c->x[3]==0){
        /* Process-local exception context. Slot 8 is private runtime state;
         * it is not exposed by the supported layout queries. Keep the ABI
         * stable so a stopped probe can load this runtime in place. */
        if(!c->layout_info[8]){
            uint64_t base=c->layout_info[14],size=c->layout_info[15];
            if((base&0xfff) || size<0x1000 || base>UINT64_MAX-size ||
               c->region_count>=BOOT_REGION_CAPACITY){boot_fault(c,BOOT_QUERY_SCOPE,pc,base,0x1000);return 0;}
            uint64_t end=base+size;
            for(;;){
                uint64_t next=base;
                for(unsigned i=0;i<c->region_count;++i){Region*r=&c->regions[i];
                    if(r->size && r->base<=base+0xfff && (r->base>base || base-r->base<r->size)){
                        if(r->base>UINT64_MAX-r->size || r->base+r->size>UINT64_MAX-0xfff){boot_fault(c,BOOT_QUERY_SCOPE,pc,base,0x1000);return 0;}
                        uint64_t after=(r->base+r->size+0xfff)&~UINT64_C(0xfff);
                        if(after>next)next=after;
                    }
                }
                if(next==base)break;
                base=next;
                if(base>end-0x1000){boot_fault(c,BOOT_QUERY_SCOPE,pc,base,0x1000);return 0;}
            }
#if TOTK_FASTMEM
            uint8_t*data=boot_guest_alloc(0x1000);
#else
            uint8_t*data=calloc(1,0x1000);
#endif
            if(!data){boot_fault(c,BOOT_QUERY_SCOPE,pc,base,0x1000);return 0;}
            unsigned i=c->region_count++;
            c->regions[i]=(Region){base,0x1000,data,READ|WRITE,1};
            c->memory_types[i]=12;c->layout_info[8]=base;boot_tlb_flush();
        }
        c->x[1]=c->layout_info[8];c->x[0]=0;++c->layout_query_count;return 1;
    }
    if(c->layout_enabled && (kind==6 || kind==7 || kind==21 || kind==22) &&
       c->x[2]==UINT64_C(0xffff8001) && c->x[3]==0){
        uint64_t used=0;for(unsigned i=0;i<c->region_count;++i)if(c->regions[i].reserved!=2)used+=c->regions[i].size;
        if(c->system_resource_total>c->memory_budget || used>c->memory_budget-c->system_resource_total){boot_fault(c,BOOT_QUERY_SCOPE,pc,used,0);return 0;}
        c->x[1]=kind==6?c->memory_budget:kind==21?c->memory_budget-c->system_resource_total:
                 kind==7?used+c->system_resource_total:used;
        c->x[0]=0;++c->layout_query_count;return 1;
    }
    if(c->layout_enabled && kind==16 && c->x[2]==UINT64_C(0xffff8001) && c->x[3]==0){
        c->x[1]=c->system_resource_total;c->x[0]=0;++c->layout_query_count;return 1;
    }
    if(c->layout_enabled && (kind<=5 || (kind>=12 && kind<=15)) &&
       c->x[2]==UINT64_C(0xffff8001) && c->x[3]==0){
        c->x[1]=c->layout_info[kind];c->x[0]=0;++c->layout_query_count;return 1;
    }
    if(c->x[1]!=11 || c->x[2]!=0 || c->x[3]>=4){
        boot_fault(c,BOOT_SVC,pc,0,0);return 0;
    }
    c->x[1]=c->entropy[c->x[3]];c->x[0]=0;++c->info_count;return 1;
}
static inline void boot_default_layout(BootContext*c){
    /* NPDM kernel capabilities: virtual cores 0..2, priorities 28..59. */
    c->layout_info[0]=7;c->layout_info[1]=UINT64_C(0xffffffff)<<28;
    /* Deliberately chosen laboratory reservations, not a captured Switch map.
     * Reservations do not imply committed/mapped host memory. */
    c->layout_info[2]=UINT64_C(0x7800000000);c->layout_info[3]=UINT64_C(0x100000000);
    c->layout_info[4]=UINT64_C(0x7600000000);c->layout_info[5]=UINT64_C(0x80000000);
    c->layout_info[12]=UINT64_C(0x7000000000);c->layout_info[13]=UINT64_C(0x1000000000);
    c->layout_info[14]=UINT64_C(0x7400000000);c->layout_info[15]=UINT64_C(0x10000000);
    c->memory_budget=UINT64_C(0xc0000000);
    /* main.npdm offset 0x14: reserve 24 MiB for process memory management.
     * This laboratory accounting charges that reservation, without creating
     * a Windows/Switch secure kernel pool. Pool-used queries remain scoped. */
    c->system_resource_total=UINT64_C(0x1800000);
}
static inline int boot_thread_info(BootContext*c,uint64_t pc,unsigned svc){
    if(c->thread_count){
        for(unsigned i=0;i<c->thread_count;++i){BootThread*t=&c->threads[i];
            if(c->x[1]==t->handle || (c->x[1]==UINT64_C(0xffff8000) && i==c->current_thread)){
                c->x[1]=svc==0xc?t->priority:t->id;c->x[0]=0;++c->thread_query_count;return 1;
            }
        }
        boot_fault(c,BOOT_SVC,pc,0,0);return 0;
    }
    if(c->x[1]!=c->main_thread_handle && c->x[1]!=UINT64_C(0xffff8000)){
        boot_fault(c,BOOT_SVC,pc,0,0);return 0;
    }
    c->x[1]=svc==0xc?c->main_thread_priority:c->main_thread_id;
    c->x[0]=0;++c->thread_query_count;return 1;
}

static inline void boot_fault(BootContext*c,uint32_t status,uint64_t pc,uint64_t address,uint64_t width) {
    if(!c->status){c->status=status;c->fault_pc=pc;c->fault_address=address;c->fault_width=width;
#if TOTK_FASTMEM
        if(boot_fm.adopt_count)boot_fm_on_stop(c,status);
#endif
    }
}
static inline int boot_valid(BootContext*c) {
    /* boot_run/boot_resume entry: the host may have rebuilt, edited or restored (checkpoint) the
     * tables since the last run; drop this context's TLB ownership and every entry. */
    c->tlb_epoch=0;boot_tlb_flush();
    if(c->memory_attributes.count>BOOT_MEMORY_ATTRIBUTE_CAPACITY)return 0;
    if(c->alias_count>BOOT_ALIAS_CAPACITY || c->thread_count>BOOT_THREAD_CAPACITY || (c->thread_count && c->current_thread>=c->thread_count) ||
       !c->region_count || c->region_count>BOOT_REGION_CAPACITY || !c->rtld_base ||
       (c->rtld_base&4095) || c->rtld_base>UINT64_MAX-0x10000)return 0;
    for(unsigned i=0;i<c->region_count;++i){
        Region*r=&c->regions[i];
        if(!r->data || !r->base || !r->size || r->size>SIZE_MAX ||
           r->size>UINT64_MAX-r->base || (r->permissions&~7u))return 0;
        for(unsigned j=0;j<i;++j){Region*s=&c->regions[j];
            if(r->base<s->base+s->size && s->base<r->base+r->size)return 0;
        }
    }
    for(unsigned a=0;a<c->alias_count;++a){BootAlias*m=&c->aliases[a];unsigned sources=0,destinations=0;
        if(!m->size || ((m->source|m->destination|m->size)&4095) || m->source>UINT64_MAX-m->size || m->destination>UINT64_MAX-m->size)return 0;
        for(unsigned b=0;b<a;++b)if(m->source<c->aliases[b].source+c->aliases[b].size && c->aliases[b].source<m->source+m->size)return 0;
        for(unsigned i=0;i<c->region_count;++i){Region*r=&c->regions[i];
            if(r->reserved!=2 && m->source>=r->base && m->size<=r->size && m->source-r->base<=r->size-m->size){
                if(r->permissions!=(READ|WRITE) || (c->memory_types[i]!=4 && c->memory_types[i]!=5))return 0;
                ++sources;
                for(unsigned j=0;j<c->region_count;++j){Region*d=&c->regions[j];
                    if(d->reserved==2 && d->base==m->destination && d->size==m->size &&
                       d->data==r->data+(size_t)(m->source-r->base) && d->permissions==(READ|WRITE) && c->memory_types[j]==11)++destinations;
                }
            }
        }
        if(sources!=1 || destinations!=1)return 0;
    }
    unsigned alias_regions=0;
    for(unsigned i=0;i<c->region_count;++i)if(c->regions[i].reserved==2){
        Region*r=&c->regions[i];
        if(c->memory_types[i]==6){
            BootWindow*w=(BootWindow*)(uintptr_t)c->window_native;
            int hid=w && w->hid_memory.data && w->hid_memory.owner==c->applet_resource_user_id &&
                    r->data==w->hid_memory.data && r->size==BOOT_HID_MEMORY_SIZE;
            int time_page=r->size==0x1000 && r->data;
            if((!hid && !time_page) || r->permissions!=READ || (r->base&4095))return 0;
        }else ++alias_regions;
    }
    if(alias_regions!=c->alias_count)return 0;
    if(c->layout_enabled){
        uint64_t aslr=c->layout_info[12],length=c->layout_info[13];
        if(!aslr || !length || ((aslr|length)&4095) || aslr>=c->address_space_end ||
           length>c->address_space_end-aslr)return 0;
        const unsigned pairs[3]={2,4,14};
        for(unsigned i=0;i<3;++i){unsigned k=pairs[i];uint64_t base=c->layout_info[k],size=c->layout_info[k+1];
            if(!size || ((base|size)&4095) || base<aslr || base>=aslr+length || size>aslr+length-base)return 0;
            for(unsigned j=0;j<i;++j){unsigned t=pairs[j];
                if(base<c->layout_info[t]+c->layout_info[t+1] && c->layout_info[t]<base+size)return 0;
            }
        }
        for(unsigned i=0;i<c->region_count;++i){Region*r=&c->regions[i];
            if(r->base<aslr || r->base>=aslr+length || r->size>aslr+length-r->base)return 0;
            for(unsigned j=0;j<2;++j){unsigned k=pairs[j];uint64_t base=c->layout_info[k],size=c->layout_info[k+1];
                if(r->base<base+size && base<r->base+r->size){
                    if((j==0?r->reserved!=1:r->reserved!=3) || c->memory_types[i]!=5 || r->permissions!=(READ|WRITE) ||
                       r->base<base || r->size>base+size-r->base || ((r->base|r->size)&4095))return 0;
                    if(j==1 && (r->base!=base || (r->size&UINT64_C(0x1fffff))))return 0;
                }
            }
        }
    }
#if TOTK_FASTMEM
    boot_fm_enter(c);
#endif
    return 1;
}
/* Software TLB in front of boot_access_lookup (the complete, exact lookup below).
 * One process-wide direct-mapped table (guest code runs on one host thread; no BootContext
 * pointer crosses threads) keyed by guest 4 KiB page. It lives outside BootContext so the
 * context size and layout (checkpoints, hosts sizing BootContext) are unchanged; selectany gives
 * every translation unit of the DLL the same single instance. The table belongs to one context
 * at a time: c->tlb_epoch must equal boot_tlb.epoch (never 0; a new or reset context holds 0),
 * otherwise the slow path gives c a fresh epoch and flushes, so another context, or a new one
 * allocated at a recycled address, never sees entries filled for a different table.
 *
 * An entry is filled only for a page that exactly one region touches, that region covers the
 * whole page, no alias source touches the page, and every active transfer memory touching the
 * page covers all of it. For any access lying inside such a page boot_access_lookup's result is
 * therefore a function of the page alone: region data + offset, or CH_PERMISSION exactly when the
 * requested bits are not all in (region permissions & every covering transfer's permissions).
 * read/write/execute each hold (page|generation) when that one permission is allowed there, so a
 * hit needs a tag match for every requested bit; anything else (status set, foreign context,
 * page-crossing range, zero width, unusual permission set, miss) takes the exact slow path.
 *
 * Every change to regions, aliases or transfers calls boot_tlb_flush(), which advances the
 * generation (1..4095, never 0, so zeroed entries never match); on wrap the table is cleared.
 * boot_valid (boot_run entry) flushes too, covering host-side edits between runs. Code outside
 * this runtime that edits c->regions/aliases/transfers directly must call boot_tlb_flush()
 * before its next access through the runtime. */
#define BOOT_TLB_BITS 12
#define BOOT_TLB_GENERATIONS 4096
typedef struct BootTlbEntry { uint64_t read,write,execute,addend; } BootTlbEntry;
typedef struct BootTlb {
    uint32_t epoch,pad0;uint64_t generation;uint64_t misses,fills,flushes,pad[3];
    BootTlbEntry entries[1u<<BOOT_TLB_BITS];
} BootTlb;
__declspec(selectany) BootTlb boot_tlb={1,0,1,0,0,0,{0},{{0}}};
static inline void boot_tlb_flush(void){
#if TOTK_FASTMEM
    boot_fm_changed();
#endif
    ++boot_tlb.flushes;
    if(++boot_tlb.generation>=BOOT_TLB_GENERATIONS){memset(boot_tlb.entries,0,sizeof(boot_tlb.entries));boot_tlb.generation=1;}
}
/* The region boot_access_lookup resolved its last successful in-region access through (NULL for
 * gathered/bounced results). Per translation unit, like boot_access_cache. */
static Region*boot_access_hit;
static inline void boot_tlb_fill(BootContext*c,uint64_t a,Region*hit){
    uint64_t page=a&~UINT64_C(4095);
    if(page>UINT64_MAX-4096)return;
    uint64_t end=page+4096;
    /* Regions never overlap (boot_valid), so a region covering the whole page is the only region
     * touching it: no rescan of the region table is needed (it cost O(regions) per miss). */
    if(!hit || hit->base>page || hit->size<4096 || page-hit->base>hit->size-4096)return;
    /* Same (wrapping) overlap predicates as boot_access_lookup, evaluated for the whole page. */
    for(unsigned i=0;i<c->alias_count;++i){BootAlias*m=&c->aliases[i];
        if(page<m->source+m->size && m->source<end)return;
    }
    unsigned allowed=hit->permissions&7u;
    for(unsigned i=0;i<16;++i){BootTransfer*t=&c->transfers[i];
        if(!t->handle || !(page<t->base+t->size && t->base<end))continue;
        if(t->base>page || t->base+t->size<end)return;
        allowed&=t->permissions;
    }
    uint64_t key=page|boot_tlb.generation;
    BootTlbEntry*e=&boot_tlb.entries[(a>>12)&((1u<<BOOT_TLB_BITS)-1)];
    e->read=allowed&READ?key:0;e->write=allowed&WRITE?key:0;e->execute=allowed&EXECUTE?key:0;
    e->addend=(uint64_t)(uintptr_t)hit->data-hit->base;
    ++boot_tlb.fills;
}
static inline __attribute__((noinline,cold)) uint8_t*boot_access_lookup(BootContext*c,uint64_t a,unsigned w,unsigned p,uint64_t pc);
static inline __attribute__((noinline,cold)) uint8_t*boot_access_slow(BootContext*c,uint64_t a,unsigned w,unsigned p,uint64_t pc){
    if(c->tlb_epoch==BOOT_TLB_CORE && boot_mt.access_slow)return boot_mt.access_slow(c,a,w,p,pc);   /* TOTK_CORES>1 */
    ++boot_tlb.misses;
    if(c->tlb_epoch!=boot_tlb.epoch){if(!++boot_tlb.epoch)boot_tlb.epoch=1;c->tlb_epoch=boot_tlb.epoch;boot_tlb_flush();}
#if TOTK_FASTMEM
    boot_fm_slow(c);
#endif
    boot_access_hit=NULL;
    uint8_t*result=boot_access_lookup(c,a,w,p,pc);
    if(result)boot_tlb_fill(c,a,boot_access_hit);
    return result;
}
#if TOTK_FASTMEM
static inline __attribute__((noinline)) uint8_t*boot_access_legacy(BootContext*c,uint64_t a,unsigned w,unsigned p,uint64_t pc);
/* Out of line: generated loads/stores use the inlined direct paths; boot_access callers (SIMD, exclusives,
 * split paths) take one call, which keeps the page code under the 2 GiB image limit. */
static inline __attribute__((noinline)) uint8_t*boot_access_fm(BootContext*c,uint64_t a,unsigned w,unsigned p,uint64_t pc);
static inline __attribute__((always_inline)) uint8_t*boot_access(BootContext*c,uint64_t a,unsigned w,unsigned p,uint64_t pc) {
    return boot_access_fm(c,a,w,p,pc);
}
static inline __attribute__((noinline)) uint8_t*boot_access_fm(BootContext*c,uint64_t a,unsigned w,unsigned p,uint64_t pc) {
    /* fastmem page table: complete (no misses); same results as the exact lookup for accesses
     * inside one page; anything else takes the exact slow path. */
    if(__builtin_expect(BOOT_FM_TABLE(c),1)){
        const BootFmLeaf*l=boot_fm.leaves[(a>>21)&(BOOT_FM_LEAVES-1)];unsigned i=(unsigned)(a>>12)&511;
        if(__builtin_expect(!(a>>BOOT_FM_SPACE_BITS) && l && !c->status && w && (a&4095)+(uint64_t)w<=4096 && p && !(p&~7u) && (l->state[i]&p)==p,1))
            return (uint8_t*)(uintptr_t)(a+l->addend[i]);
        return boot_access_slow(c,a,w,p,pc);
    }
    return boot_access_legacy(c,a,w,p,pc);
}
static inline __attribute__((noinline)) uint8_t*boot_access_legacy(BootContext*c,uint64_t a,unsigned w,unsigned p,uint64_t pc) {
#else
static inline __attribute__((always_inline)) uint8_t*boot_access(BootContext*c,uint64_t a,unsigned w,unsigned p,uint64_t pc) {
#endif
    const BootTlbEntry*e=&boot_tlb.entries[(a>>12)&((1u<<BOOT_TLB_BITS)-1)];
    uint64_t key=(a&~UINT64_C(4095))|boot_tlb.generation;
    if(__builtin_expect(!c->status && c->tlb_epoch==boot_tlb.epoch && w && (a&4095)+(uint64_t)w<=4096 && p && !(p&~7u) &&
                        (!(p&READ) || e->read==key) && (!(p&WRITE) || e->write==key) &&
                        (!(p&EXECUTE) || e->execute==key),1))
        return (uint8_t*)(uintptr_t)(a+e->addend);
    return boot_access_slow(c,a,w,p,pc);
}
/* play (53): guest reads emitted by the translator (vector loads). With the fixed view: the address in the view
 * (a fault there is a direct-access fault: VEH, exact guest status); otherwise boot_access. w <= 64. */
static inline __attribute__((always_inline)) uint8_t*boot_access_read(BootContext*c,uint64_t a,unsigned w,uint64_t pc){
#if TOTK_FASTMEM && TOTK_FM_FIXED
    if(__builtin_expect(a<=(UINT64_C(1)<<BOOT_FM_SPACE_BITS)-64 && !c->status,1)){(void)w;(void)pc;return BOOT_FM_VIEW+a;}
#endif
    return boot_access(c,a,w,READ,pc);
}
/* The exact lookup: every fault and result of the access model is defined here. */
static inline __attribute__((noinline,cold)) uint8_t*boot_access_lookup(BootContext*c,uint64_t a,unsigned w,unsigned p,uint64_t pc) {
    if(c->status)return NULL;
    if(a>UINT64_MAX-w){boot_fault(c,CH_ADDRESS,pc,a,w);return NULL;}
    for(unsigned i=0;i<16;++i){BootTransfer*t=&c->transfers[i];
        if(t->handle && a<t->base+t->size && t->base<a+w && (t->permissions&p)!=p){boot_fault(c,CH_PERMISSION,pc,a,w);return NULL;}
    }
    /* Lookup cache: 64 direct-mapped entries per translation unit keyed by the 64 KiB block of
     * the address (instruction fetches use their own entry). Regions never overlap, so a cached
     * region that still contains the range is the region a full scan would find; every hit is
     * re-validated against the current table. Each entry also caches whether any alias source
     * overlaps its region, against the alias table's count and map/unmap counters (which change
     * on every alias map, unmap and reset). */
    static struct { unsigned index,count,maps,unmaps,clean; uint64_t base,size; } boot_access_cache[65];
    unsigned slot=p==EXECUTE?64:(unsigned)((a>>16)&63);
    __typeof__(boot_access_cache[0])*k=&boot_access_cache[slot];
    if(k->index<c->region_count){Region*r=&c->regions[k->index];
        if(r->base==k->base && r->size==k->size && a>=r->base && w<=r->size && a-r->base<=r->size-w){
            if(k->count!=c->alias_count || k->maps!=c->alias_map_count || k->unmaps!=c->alias_unmap_count){
                k->count=c->alias_count;k->maps=c->alias_map_count;k->unmaps=c->alias_unmap_count;k->clean=1;
                for(unsigned i=0;i<c->alias_count;++i){BootAlias*m=&c->aliases[i];
                    if(r->base<m->source+m->size && m->source<r->base+r->size){k->clean=0;break;}
                }
            }
            if(!k->clean)for(unsigned i=0;i<c->alias_count;++i){BootAlias*m=&c->aliases[i];
                if(a<m->source+m->size && m->source<a+w){boot_fault(c,CH_PERMISSION,pc,a,w);return NULL;}
            }
            if((r->permissions&p)!=p){boot_fault(c,CH_PERMISSION,pc,a,w);return NULL;}
            boot_access_hit=r;
            return r->data+(size_t)(a-r->base);
        }
    }
    for(unsigned i=0;i<c->alias_count;++i){BootAlias*m=&c->aliases[i];
        if(a<m->source+m->size && m->source<a+w){boot_fault(c,CH_PERMISSION,pc,a,w);return NULL;}
    }
    for(unsigned i=0;i<c->region_count;++i){Region*r=&c->regions[i];
        if(a>=r->base && a-r->base<r->size){
            if(w<=r->size && a-r->base<=r->size-w){
                if((r->permissions&p)!=p){boot_fault(c,CH_PERMISSION,pc,a,w);return NULL;}
                k->index=i;k->base=r->base;k->size=r->size;k->count=UINT32_MAX;
                boot_access_hit=r;
                return r->data+(size_t)(a-r->base);
            }
            /* The range continues past this region into guest-adjacent regions (separate host
             * allocations for memory that is contiguous on the console). Reads up to 64 bytes are
             * gathered piecewise into a bounce buffer the caller consumes immediately; writes
             * still fault. */
            if(p!=READ || w>64)break;
            static uint8_t boot_access_bounce[64];
            uint64_t done=0;
            while(done<w){
                uint64_t at=a+done;unsigned piece=0;Region*q=NULL;
                for(unsigned j=0;j<c->region_count;++j){Region*t=&c->regions[j];
                    if(at>=t->base && at-t->base<t->size){q=t;break;}}
                if(!q || (q->permissions&READ)!=READ)break;
                uint64_t left=q->size-(at-q->base);piece=(unsigned)(left<w-done?left:w-done);
                memcpy(boot_access_bounce+done,q->data+(size_t)(at-q->base),piece);done+=piece;
            }
            if(done<w)break;
            return boot_access_bounce;
        }
    }boot_fault(c,CH_ADDRESS,pc,a,w);return NULL;
}
static inline int boot_tick(BootContext*c,uint64_t offset) {
    uint64_t pc=boot_address(c,offset);
    if(c->status)return 0;
    if(c->window_native && !(c->steps&4095) && boot_window_is_closed((BootWindow*)(uintptr_t)c->window_native)){
        boot_fault(c,BOOT_WINDOW_CLOSED,pc,0,0);return 0;
    }
    if(c->checkpoint_offset && offset==c->checkpoint_offset){
        boot_fault(c,BOOT_TARGET,pc,pc,0);return 0;
    }
    if(c->steps>=c->limit){boot_fault(c,CH_BUDGET,pc,0,0);return 0;}
    if(!boot_access(c,pc,4,EXECUTE,pc))return 0;
    ++c->steps;
    c->trace[(c->steps-1)%BOOT_TRACE_CAPACITY]=pc;
    if(c->trace_count<BOOT_TRACE_CAPACITY)++c->trace_count;
    return 1;
}
/* Per-block execution checks. partition_native keeps the full boot_tick on every instruction a
 * branch can reach (and checks page entries with boot_block_check, which counts no step); all
 * other instructions follow their predecessor in straight-line code and only count the step. The
 * budget, code-fetch permission and trace are therefore per block; checkpoints stay exact. */
static inline __attribute__((always_inline)) int boot_block_check(BootContext*c,uint64_t offset) {
#if TOTK_FASTMEM
    boot_fm_mark_ctx(c);   /* page-function frame for the fastmem fault handler (per host thread) */
#endif
    uint64_t pc=boot_address(c,offset);
    if(c->status)return 0;
    if(c->checkpoint_offset && offset==c->checkpoint_offset){boot_fault(c,BOOT_TARGET,pc,pc,0);return 0;}
    if(c->steps>=c->limit){boot_fault(c,CH_BUDGET,pc,0,0);return 0;}
    if(!boot_access(c,pc,4,EXECUTE,pc))return 0;
    c->trace[c->steps%BOOT_TRACE_CAPACITY]=pc;
    return 1;
}
static inline int boot_tick_light(BootContext*c,uint64_t offset) {
    if(c->checkpoint_offset && offset==c->checkpoint_offset){uint64_t pc=boot_address(c,offset);boot_fault(c,BOOT_TARGET,pc,pc,0);return 0;}
    ++c->steps;return 1;
}
/* Page-function forms of boot_tick / boot_block_check (partition_native). A translated page keeps
 * the step count and the checkpoint offset in locals for the duration of one page call (no runtime
 * helper writes either; the page stores the count back to c->steps before every return, transfer
 * and any helper that may read it), so straight-line code counts steps in a register.
 * boot_tick_steps is exactly boot_tick with c->steps taken from steps and c->checkpoint_offset
 * from checkpoint; it returns the new step count (steps+1, never 0) or 0 where boot_tick returns 0
 * (the count is then unchanged). It is not inlined: block leaders are ~10% of instructions and
 * inlining its access check into every leader costs compile time for little gain.
 * boot_block_check_steps is exactly boot_block_check with c->steps taken from steps. The page stores
 * c->steps before every guest-memory helper too (partition_native STEP_NEUTRAL), so a fastmem fault
 * that unwinds the page function leaves the exact count. */
static inline __attribute__((noinline)) uint64_t boot_tick_steps(BootContext*c,uint64_t offset,uint64_t steps,uint64_t checkpoint) {
    uint64_t pc=boot_address(c,offset);
    if(c->status)return 0;
    if(c->window_native && !(steps&4095) && boot_window_is_closed((BootWindow*)(uintptr_t)c->window_native)){
        boot_fault(c,BOOT_WINDOW_CLOSED,pc,0,0);return 0;
    }
    if(checkpoint && offset==checkpoint){boot_fault(c,BOOT_TARGET,pc,pc,0);return 0;}
    if(steps>=c->limit){boot_fault(c,CH_BUDGET,pc,0,0);return 0;}
    if(!boot_access(c,pc,4,EXECUTE,pc))return 0;
    ++steps;
    c->trace[(steps-1)%BOOT_TRACE_CAPACITY]=pc;
    if(c->trace_count<BOOT_TRACE_CAPACITY)++c->trace_count;
    return steps;
}
static inline __attribute__((always_inline)) int boot_block_check_steps(BootContext*c,uint64_t offset,uint64_t steps,uint64_t checkpoint) {
#if TOTK_FASTMEM
    boot_fm_mark_ctx(c);   /* page-function frame for the fastmem fault handler, as boot_block_check */
#endif
    uint64_t pc=boot_address(c,offset);
    if(c->status)return 0;
    if(checkpoint && offset==checkpoint){boot_fault(c,BOOT_TARGET,pc,pc,0);return 0;}
    if(steps>=c->limit){boot_fault(c,CH_BUDGET,pc,0,0);return 0;}
    if(!boot_access(c,pc,4,EXECUTE,pc))return 0;
    c->trace[steps%BOOT_TRACE_CAPACITY]=pc;
    return 1;
}
/* Play build (play_config.h): no instruction counting. c->limit is a run token written by other host
 * threads (boot_mt_kick, boot_svc's limit hijack): 0 asks translated code to stop. Pages poll it at
 * their entry and at loop heads (a leader some goto of the page reaches backwards), so every guest loop
 * polls once per iteration (a loop either branches backwards inside a page or re-enters a page).
 * The stop is CH_BUDGET at the instruction about to run, resumable there (boot_resume); a closed host
 * window stops with BOOT_WINDOW_CLOSED instead. */
static inline __attribute__((always_inline)) int boot_play_running(const BootContext*c){
    return *(const volatile uint64_t*)&c->limit!=0;
}
static inline __attribute__((noinline)) int boot_play_stop(BootContext*c,uint64_t offset){
    if(c->status)return 0;
    uint64_t pc=boot_address(c,offset);
    if(c->window_native && boot_window_is_closed((BootWindow*)(uintptr_t)c->window_native))boot_fault(c,BOOT_WINDOW_CLOSED,pc,0,0);
    else boot_fault(c,CH_BUDGET,pc,0,0);
    return 0;
}
static inline __attribute__((always_inline)) int boot_play_enter(BootContext*c,uint64_t offset){
#if TOTK_FASTMEM
    boot_fm_mark_ctx(c);   /* page-function frame for the fastmem fault handler */
#endif
    if(__builtin_expect(c->status!=0 || !boot_play_running(c),0))return boot_play_stop(c,offset);
    return 1;
}
/* What partition_native emits for step counting and checkpoints. TOTK_PLAY=0 expands to exactly the
 * text it emitted before these macros (same code); TOTK_PLAY=1 to the poll only. Every use is followed
 * by ';'. BOOT_LEADER: a block leader reached only by forward gotos; BOOT_LOOP_HEAD: one reached by a
 * backward goto; BOOT_STEP_AT: a non-leader that is a page entry (a checkpoint may target it). */
#if TOTK_PLAY
#define BOOT_PAGE_LOCALS(c,next) const uint64_t boot_entry_=(next)[1]
#define BOOT_PAGE_ENTER(c) if(!boot_play_enter((c),boot_entry_))return (c)->status
#define BOOT_STEP_AT(offset) do{}while(0)
#define BOOT_STEP do{}while(0)
#define BOOT_LEADER(c,offset) do{}while(0)
#define BOOT_LOOP_HEAD(c,offset) do{if(__builtin_expect(!boot_play_running(c),0)){boot_play_stop((c),UINT64_C(offset));return (c)->status;}}while(0)
#define BOOT_STEPS_SYNC(c) do{}while(0)
#define BOOT_CHECKPOINT_STOP(c) return (c)->status
#else
#define BOOT_PAGE_LOCALS(c,next) const uint64_t boot_entry_=(next)[1],boot_ckpt_=(c)->checkpoint_offset;uint64_t boot_steps_=(c)->steps
#define BOOT_PAGE_ENTER(c) if(!boot_block_check_steps((c),boot_entry_,boot_steps_,boot_ckpt_))return (c)->status
#define BOOT_STEP_AT(offset) if(boot_ckpt_==UINT64_C(offset))goto boot_checkpoint_;++boot_steps_
#define BOOT_STEP ++boot_steps_
#define BOOT_LEADER(c,offset) {uint64_t boot_next_=boot_tick_steps((c),UINT64_C(offset),boot_steps_,boot_ckpt_);if(!boot_next_){(c)->steps=boot_steps_;return (c)->status;}boot_steps_=boot_next_;}
#define BOOT_LOOP_HEAD(c,offset) BOOT_LEADER(c,offset)
#define BOOT_STEPS_SYNC(c) (c)->steps=boot_steps_
#define BOOT_CHECKPOINT_STOP(c) {uint64_t pc=boot_address((c),boot_ckpt_);boot_fault((c),BOOT_TARGET,pc,pc,0);(c)->steps=boot_steps_;return (c)->status;}
#endif
/* FADD/FSUB/FMUL/FNMUL/FDIV scalar and FADD/FSUB/FMUL/FABD/FADDP vector (translate_startup.py): the play
 * build uses the host IEEE operation where it is provably the Arm result (not a NaN; with FPCR.FZ no
 * subnormal input and no result in (0, min normal]; FDIV not by zero); everything else keeps the exact code.
 * FPSR IXC/UFC/OFC are not accumulated on that path (the game reads FPSR at 7 fenv sites); the exact
 * FP model tests (test_float*.py check those flags) need -DBOOT_FP_NATIVE=0. */
/* Region-local guest registers (partition_native cache_registers): BOOT_R(n) names general register n
 * inside a region, BOOT_RL loads it, BOOT_WB stores it back at region exits, BOOT_RDECL declares the
 * page's register locals. The probe build keeps every register in c->x (the macros vanish); -DBOOT_REGCACHE=1
 * forces the locals in a probe build (differential tests with budgets and checkpoints). */
/* Default OFF: measured on 129 sdk pages (play build), caching every region made the code 7% larger and
 * 5-30% slower than leaving c->x to LLVM (which already keeps registers within blocks); the partial form
 * (memory-op blocks as barriers) was within noise. Correct either way: 11,440 + 11,800 differential runs
 * identical except register values at terminal guest-memory faults (dirty locals are not stored). */
#ifndef BOOT_REGCACHE
#define BOOT_REGCACHE 0
#endif
#if BOOT_REGCACHE
#define BOOT_R(n) r_##n
#define BOOT_RL(n) (r_##n=c->x[n])
#define BOOT_WB(n) (c->x[n]=r_##n)
#define BOOT_RDECL(...) uint64_t __VA_ARGS__
#else
#define BOOT_R(n) c->x[n]
#define BOOT_RL(n) ((void)0)
#define BOOT_WB(n) ((void)0)
#define BOOT_RDECL(...) ((void)0)
#endif
/* Exact eight-bit ARM estimate tables (generated by play-work/threads/tools/p57.py).
 * Values include the implicit leading bit; emitters use the low eight bits. */
__declspec(selectany) const uint16_t boot_fp_recip_table[256]={
    511,509,507,505,503,501,499,497,496,494,492,490,488,486,485,483,
    481,479,477,476,474,472,471,469,467,466,464,462,461,459,457,456,
    454,453,451,450,448,447,445,444,442,441,439,438,436,435,433,432,
    430,429,428,426,425,423,422,421,419,418,417,415,414,413,412,410,
    409,408,406,405,404,403,401,400,399,398,397,395,394,393,392,391,
    390,388,387,386,385,384,383,382,380,379,378,377,376,375,374,373,
    372,371,370,369,368,367,366,365,364,363,362,361,360,359,358,357,
    356,355,354,353,352,351,350,349,348,347,346,345,344,344,343,342,
    341,340,339,338,337,337,336,335,334,333,332,331,331,330,329,328,
    327,326,326,325,324,323,322,322,321,320,319,319,318,317,316,315,
    315,314,313,312,312,311,310,309,309,308,307,307,306,305,304,304,
    303,302,302,301,300,300,299,298,298,297,296,296,295,294,294,293,
    292,292,291,290,290,289,288,288,287,286,286,285,285,284,283,283,
    282,282,281,280,280,279,279,278,277,277,276,276,275,274,274,273,
    273,272,272,271,271,270,269,269,268,268,267,267,266,266,265,265,
    264,263,263,262,262,261,261,260,260,259,259,258,258,257,257,256,
};
__declspec(selectany) const uint16_t boot_fp_rsqrt_table[384]={
    511,509,507,505,503,501,499,498,496,494,492,490,489,487,485,484,
    482,480,479,477,475,474,472,471,469,468,466,465,463,462,460,459,
    457,456,454,453,452,450,449,448,446,445,444,442,441,440,439,437,
    436,435,434,432,431,430,429,428,426,425,424,423,422,421,420,419,
    418,416,415,414,413,412,411,410,409,408,407,406,405,404,403,402,
    401,400,399,398,397,396,396,395,394,393,392,391,390,389,388,387,
    387,386,385,384,383,382,382,381,380,379,378,377,377,376,375,374,
    374,373,372,371,370,370,369,368,367,367,366,365,365,364,363,362,
    361,361,360,360,359,359,357,357,356,356,355,355,353,353,352,352,
    351,351,349,349,348,348,347,347,346,346,344,344,343,343,342,342,
    341,341,340,340,338,338,337,337,336,336,335,335,334,334,333,333,
    332,332,331,331,330,330,328,328,327,327,326,326,325,325,324,324,
    323,323,322,322,321,321,320,320,319,319,318,318,317,317,316,316,
    316,316,315,315,314,314,313,313,312,312,311,311,310,310,309,309,
    308,308,307,307,307,307,306,306,305,305,304,304,303,303,302,302,
    302,302,301,301,300,300,299,299,298,298,298,298,297,297,296,296,
    295,295,294,294,294,294,293,293,292,292,291,291,291,291,290,290,
    289,289,289,289,288,288,287,287,286,286,286,286,285,285,284,284,
    284,284,283,283,282,282,282,282,281,281,280,280,280,280,279,279,
    278,278,278,278,277,277,276,276,276,276,275,275,275,275,274,274,
    273,273,273,273,272,272,272,272,271,271,270,270,270,270,269,269,
    269,269,268,268,267,267,267,267,266,266,266,266,265,265,265,265,
    264,264,264,264,263,263,262,262,262,262,261,261,261,261,260,260,
    260,260,259,259,259,259,258,258,258,258,257,257,257,257,256,256,
};

/* Native fused arithmetic is optional and exact including FPSR. These helpers are
 * out of line so each translated instruction does not duplicate CPU detection,
 * host-environment isolation or the exceptional-result guards.
 * Patch 58: they are the slow path now (boot_fp_fma32/64 in boot_fp_native.h try one inline host FMA first);
 * they run when that declines, notably while FPSR.IXC is still clear, and keep IXC exact from MXCSR.PE. */
static inline __attribute__((noinline)) int boot_fp_has_fma(void){
    static int available=-1;
    int value=__atomic_load_n(&available,__ATOMIC_RELAXED);
    if(value<0){
        uint32_t a=1,b,d,e;
        __asm__ volatile("cpuid":"+a"(a),"=b"(b),"=c"(d),"=d"(e));
        value=0;
        if((d&UINT32_C(0x18001000))==UINT32_C(0x18001000)){
            uint32_t lo,hi;
            __asm__ volatile("xgetbv":"=a"(lo),"=d"(hi):"c"(0));
            value=(lo&6)==6;
        }
        __atomic_store_n(&available,value,__ATOMIC_RELAXED);
    }
    return value;
}
static inline __attribute__((noinline,target("fma"))) uint32_t boot_fp_fma32_eval(
        uint32_t acc,uint32_t a,uint32_t b,uint32_t*out){
    float x,y,z;uint32_t saved,active=UINT32_C(0x1f80),status;
    memcpy(&x,&a,4);memcpy(&y,&b,4);memcpy(&z,&acc,4);
    __asm__ volatile("stmxcsr %0":"=m"(saved)::"memory");
    __asm__ volatile("ldmxcsr %0"::"m"(active):"memory");
    __asm__ volatile("vfmadd231ss %2,%1,%0":"+x"(z):"x"(x),"x"(y):"memory");
    __asm__ volatile("stmxcsr %0":"=m"(status)::"memory");
    __asm__ volatile("ldmxcsr %0"::"m"(saved):"memory");
    memcpy(out,&z,4);return status;
}
static inline __attribute__((noinline,target("fma"))) uint32_t boot_fp_fma64_eval(
        uint64_t acc,uint64_t a,uint64_t b,uint64_t*out){
    double x,y,z;uint32_t saved,active=UINT32_C(0x1f80),status;
    memcpy(&x,&a,8);memcpy(&y,&b,8);memcpy(&z,&acc,8);
    __asm__ volatile("stmxcsr %0":"=m"(saved)::"memory");
    __asm__ volatile("ldmxcsr %0"::"m"(active):"memory");
    __asm__ volatile("vfmadd231sd %2,%1,%0":"+x"(z):"x"(x),"x"(y):"memory");
    __asm__ volatile("stmxcsr %0":"=m"(status)::"memory");
    __asm__ volatile("ldmxcsr %0"::"m"(saved):"memory");
    memcpy(out,&z,8);return status;
}
static inline __attribute__((noinline)) int boot_fp_fma32_mxcsr(BootContext*c,
        uint32_t acc,uint32_t a,uint32_t b,uint32_t*out){
    uint32_t am=a&UINT32_C(2147483647),bm=b&UINT32_C(2147483647),cm=acc&UINT32_C(2147483647);
    if((c->fpcr&UINT32_C(0xc00000)) || am>=UINT32_C(2139095040) || bm>=UINT32_C(2139095040) || cm>=UINT32_C(2139095040) ||
        (am && am<UINT32_C(8388608)) || (bm && bm<UINT32_C(8388608)) || (cm && cm<UINT32_C(8388608)) || !boot_fp_has_fma())return 0;
    uint32_t result;uint32_t flags=boot_fp_fma32_eval(acc,a,b,&result);
    uint32_t magnitude=result&UINT32_C(2147483647);
    /* Arm detects tininess before rounding; x86 detects it after rounding. The
     * min-normal boundary and all tiny/overflow results use the exact path. */
    if((flags&31) || (magnitude && magnitude<=UINT32_C(8388608)) || magnitude>=UINT32_C(2139095040))return 0;
    c->fpsr|=(flags&32)?16:0;*out=result;return 1;
}
static inline __attribute__((noinline)) int boot_fp_fma64_mxcsr(BootContext*c,
        uint64_t acc,uint64_t a,uint64_t b,uint64_t*out){
    uint64_t am=a&UINT64_C(9223372036854775807),bm=b&UINT64_C(9223372036854775807),cm=acc&UINT64_C(9223372036854775807);
    if((c->fpcr&UINT32_C(0xc00000)) || am>=UINT64_C(9218868437227405312) || bm>=UINT64_C(9218868437227405312) || cm>=UINT64_C(9218868437227405312) ||
        (am && am<UINT64_C(4503599627370496)) || (bm && bm<UINT64_C(4503599627370496)) || (cm && cm<UINT64_C(4503599627370496)) || !boot_fp_has_fma())return 0;
    uint64_t result;uint32_t flags=boot_fp_fma64_eval(acc,a,b,&result);
    uint64_t magnitude=result&UINT64_C(9223372036854775807);
    /* Arm detects tininess before rounding; x86 detects it after rounding. The
     * min-normal boundary and all tiny/overflow results use the exact path. */
    if((flags&31) || (magnitude && magnitude<=UINT64_C(4503599627370496)) || magnitude>=UINT64_C(9218868437227405312))return 0;
    c->fpsr|=(flags&32)?16:0;*out=result;return 1;
}

#ifndef BOOT_FP_NATIVE
#define BOOT_FP_NATIVE TOTK_PLAY
#endif
#include "boot_fp_native.h"   /* patch 58: inline native FP fast paths + generated cold exact helpers (boot_fp_paths.h) */
/* Page dispatch (partition_native). Translated pages are indexed by boot_offset>>12 in
 * boot_page_table, defined with boot_execute in the generated source (a null entry is an
 * untranslated page). A page function is entered with next[0] = target address and
 * next[1] = boot_offset(c,next[0]). BOOT_PAGE_DISPATCH, at the end of a page after *next and *direct
 * are set, performs exactly one iteration of boot_execute's loop (status check, return sentinel
 * check, page lookup or BOOT_TARGET fault) and continues in the destination page by a tail call,
 * so chained transfers do not return to boot_execute and the stack does not grow. A target without a
 * page, or in a page marked in boot_host_fn_mark (a hooked guest entry), is first offered to the
 * host-function registry (host_fn.h); the translated path pays one byte load. */
typedef uint32_t (*BootPageFunction)(BootContext*,uint64_t*,uint32_t*);
#define BOOT_PAGE_TABLE_SIZE (UINT64_C(4)<<16)
extern const BootPageFunction boot_page_table[BOOT_PAGE_TABLE_SIZE];
__declspec(selectany) volatile uint8_t boot_host_fn_mark[BOOT_PAGE_TABLE_SIZE]={0};
/* Direct transfer to a known translated destination (partition_native): the destination page is called
 * through the page table without the dispatch lookup (status/sentinel checks, boot_offset, bounds). A page
 * marked for a host function takes the generic transfer that follows the macro. Uses `target`. */
#define BOOT_DIRECT_PAGE(c,next,direct,offset) do{ \
        if(!__builtin_expect(boot_host_fn_mark[(UINT64_C(offset))>>12],0)){ \
            (next)[0]=target;(next)[1]=UINT64_C(offset);BOOT_STEPS_SYNC(c); \
            __attribute__((musttail)) return boot_page_table[(UINT64_C(offset))>>12]((c),(next),(direct));}}while(0)
#define BOOT_PAGE_DISPATCH(c,next,direct) do{ \
        if((c)->status || (!*(direct) && (next)[0]==(c)->return_sentinel))return (c)->status; \
        uint64_t boot_dispatch_offset_=boot_offset((c),(next)[0]); \
        BootPageFunction boot_dispatch_function_=boot_dispatch_offset_<(BOOT_PAGE_TABLE_SIZE<<12)?boot_page_table[boot_dispatch_offset_>>12]:NULL; \
        if(!boot_dispatch_function_ || __builtin_expect(boot_host_fn_mark[boot_dispatch_offset_>>12],0)){ \
            if(boot_host_fn_dispatch((c),(next),(direct)))return (c)->status; \
            if(!boot_dispatch_function_){boot_fault((c),BOOT_TARGET,(next)[0],(next)[0],0);return (c)->status;}} \
        (next)[1]=boot_dispatch_offset_; \
        __attribute__((musttail)) return boot_dispatch_function_((c),(next),(direct)); \
    }while(0)
/* Direct guest calls (patch 56, play-work/DIRECT-CALLS.md). partition_native emits, at a BL/BLR site with
 * return address R, a real C call of the destination page function through boot_call_page /
 * boot_call_indirect, and at a RET `next[0]=target;*direct=0;return` (the page function returns to whoever
 * owns this host frame). Every transfer inside a frame stays a musttail, so a frame is owned by the call site
 * that created it, or by boot_execute. After the call the site compares the guest return target with R:
 *   1  returned to R: the site continues at R (straight-line code, no dispatch, no entry switch);
 *   2  returned elsewhere (longjmp, thread switch, x30 rewritten by a host-function continuation, the
 *      return sentinel): *target is set, the site dispatches it exactly like the old RET did;
 *   3  stop (c->status set: fault, run-token stop, idle SVC): the site returns c->status (unwinds);
 *   0  not called (hooked page, no page, host stack low): the site takes the old transfer.
 * Host frames hold no guest state (all of it is in c), so continuing at R when the target is R is exactly
 * what the dispatch would do: correct whatever happened inside the call. next[2] is the host stack
 * watermark (boot_call_watermark, set by boot_execute): below it a call site uses the old transfer, so deep
 * guest recursion degrades to the old behaviour. The fastmem VEH unwinds a faulting access to the innermost
 * page frame recorded at page entry (boot_fm_mark_ctx), so a returning call re-marks the caller's frame.
 * BOOT_DIRECT_CALLS=0 (default in the probe build) compiles all of it out; TOTK_DIRECT_CALLS=0 (environment)
 * turns calls off at run time (RET then returns to boot_execute, which dispatches as before). */
#ifndef BOOT_DIRECT_CALLS
#define BOOT_DIRECT_CALLS TOTK_PLAY
#endif
WINBASEAPI VOID WINAPI GetCurrentThreadStackLimits(PULONG_PTR LowLimit,PULONG_PTR HighLimit);
/* Lowest host stack address a call may start from: low limit + reserve for host code (host functions, the
 * kernel, the renderer, the VEH). Reserve: TOTK_CALL_RESERVE_KB, default half the stack up to 1 MiB.
 * UINT64_MAX (no calls) with TOTK_DIRECT_CALLS=0 or BOOT_DIRECT_CALLS=0. */
static inline uint64_t boot_call_watermark(void){
#if BOOT_DIRECT_CALLS
    static volatile LONG64 reserve_kb=-1;   /* -2: off */
    LONG64 r=reserve_kb;
    if(r==-1){
        const char*e=getenv("TOTK_DIRECT_CALLS");const char*k=getenv("TOTK_CALL_RESERVE_KB");
        r=e && e[0]=='0'?-2:k?(LONG64)strtoull(k,NULL,10):0;reserve_kb=r;
    }
    if(r==-2)return UINT64_MAX;
    ULONG_PTR lo=0,hi=0;GetCurrentThreadStackLimits(&lo,&hi);
    uint64_t size=(uint64_t)(hi-lo),reserve=r>0?(uint64_t)r<<10:size/2;
    if(r<=0 && reserve>(UINT64_C(1)<<20))reserve=UINT64_C(1)<<20;
    if(!lo || reserve>=size)return UINT64_MAX;
    return (uint64_t)lo+reserve;
#else
    return UINT64_MAX;
#endif
}
#if BOOT_DIRECT_CALLS
static inline __attribute__((always_inline)) uint64_t boot_call_rsp(void){
    uint64_t rsp;__asm__ volatile("movq %%rsp,%0":"=r"(rsp));return rsp;
}
static inline __attribute__((always_inline)) int boot_call_enter(BootContext*c,const uint64_t*next,BootPageFunction fn,
        uint64_t address,uint64_t offset,uint64_t ret,uint64_t*target){
    uint64_t state[3]={address,offset,next[2]};uint32_t direct=1;
    if(__builtin_expect(fn((c),state,&direct)!=0,0))return 3;
#if TOTK_FASTMEM
    boot_fm_mark_ctx(c);   /* the VEH unwinds to the innermost live page frame: this one again */
#endif
    *target=state[0];
    return __builtin_expect(state[0]==ret,1)?1:2;
}
/* BL to a known translated destination (logical offset; address = boot_address(c,offset)). */
static inline __attribute__((always_inline)) int boot_call_page(BootContext*c,const uint64_t*next,BootPageFunction fn,
        uint64_t address,uint64_t offset,uint64_t ret,uint64_t*target){
    if(__builtin_expect(!fn || boot_host_fn_mark[offset>>12] || boot_call_rsp()<=next[2],0))return 0;
    return boot_call_enter(c,next,fn,address,offset,ret,target);
}
/* BLR: the destination page is looked up as BOOT_PAGE_DISPATCH does; misses take the old path. */
static inline __attribute__((always_inline)) int boot_call_indirect(BootContext*c,const uint64_t*next,uint64_t address,uint64_t ret,uint64_t*target){
    uint64_t offset=boot_offset(c,address);
    if(__builtin_expect(offset>=(BOOT_PAGE_TABLE_SIZE<<12),0))return 0;
    BootPageFunction fn=boot_page_table[offset>>12];
    if(__builtin_expect(!fn || boot_host_fn_mark[offset>>12] || boot_call_rsp()<=next[2],0))return 0;
    return boot_call_enter(c,next,fn,address,offset,ret,target);
}
#else
#define boot_call_page(c,next,fn,address,offset,ret,target) ((void)(target),0)
#define boot_call_indirect(c,next,address,ret,target) ((void)(target),0)
#endif
static inline __attribute__((always_inline)) int boot_sp(BootContext*c,uint64_t pc) {
    if(c->sp&15){boot_fault(c,CH_STACK,pc,c->sp,16);return 0;}return 1;
}
#if TOTK_FASTMEM
static inline __attribute__((noinline)) int boot_load_legacy(BootContext*c,uint64_t a,unsigned w,uint64_t pc,uint64_t*out);
/* Page table and legacy paths stay out of line: only the direct path is inlined per access
 * (inlining the table path too doubled the translated code past the 2 GiB image limit). */
static inline __attribute__((noinline)) int boot_load_fm_slow(BootContext*c,uint64_t a,unsigned w,uint64_t pc,uint64_t*out) {
    if(BOOT_FM_TABLE(c)){uint8_t*h=boot_fm_table_ptr(c,a,w,READ);if(__builtin_expect(h!=NULL,1)){*out=boot_fm_read(h,w);return 1;}}
    return boot_load_legacy(c,a,w,pc,out);
}
static inline __attribute__((always_inline)) int boot_load(BootContext*c,uint64_t a,unsigned w,uint64_t pc,uint64_t*out) {
    if(__builtin_expect(a<=BOOT_FM_DIRECT_LIMIT && BOOT_FM_DIRECT(c,pc),1)){*out=boot_fm_read(BOOT_FM_VIEW+a,w);return 1;}
    return boot_load_fm_slow(c,a,w,pc,out);
}
static inline __attribute__((noinline)) int boot_load_legacy(BootContext*c,uint64_t a,unsigned w,uint64_t pc,uint64_t*out) {
#else
static inline int boot_load(BootContext*c,uint64_t a,unsigned w,uint64_t pc,uint64_t*out) {
#endif
    uint8_t*p=boot_access(c,a,w,READ,pc);if(!p)return 0;*out=little_read(p,w);return 1;
}
static inline void boot_release_memory(BootContext*c){
#if TOTK_FASTMEM
    boot_fm_release(c);   /* Python buffers back, guest view and tables dropped */
#endif
    c->layout_info[8]=0;
    memset(&c->memory_attributes,0,sizeof(c->memory_attributes));
    memset(c->transfers,0,sizeof(c->transfers));
    memset(c->nvdrv_transfer_slots,0,sizeof(c->nvdrv_transfer_slots));
    if(c->performance_native){boot_performance_close((BootPerformance*)(uintptr_t)c->performance_native);c->performance_native=0;}
    if(c->storage_native){boot_storage_close((BootStorage*)(uintptr_t)c->storage_native);c->storage_native=0;}
    c->storage_read_count=c->storage_bytes_read=0;
    for(unsigned i=0;i<BOOT_THREAD_CAPACITY;++i)boot_release_wait(&c->threads[i]);
    unsigned keep=0;
    for(unsigned i=0;i<c->region_count;++i){
        if(c->regions[i].reserved==1){
#ifdef TOTK_DRAW_PATH
            /* GPU arenas are kept (never free() an interior pointer); pool memory goes back to the pool */
            if(boot_gpu_arena_base(c->regions[i].data)){boot_gpu_pool_free(c->regions[i].data,c->regions[i].size);continue;}
#endif
#if TOTK_FASTMEM
            /* Pieces of a region split by an unmap share its section: the last piece releases it. */
            if(boot_fm_owns(c->regions[i].data)){int a=boot_fm_alloc_find(c->regions[i].data);
                uint8_t*owner=boot_fm.allocs[a].host;uint64_t owner_size=boot_fm.allocs[a].size;int later=0;
#ifdef TOTK_DRAW_PATH
                if(boot_gpu_arena_base(c->regions[i].data))continue;   /* GPU arena: kept, as before */
#endif
                for(unsigned j=i+1;j<c->region_count && !later;++j)
                    later=c->regions[j].reserved==1 && c->regions[j].data>=owner && c->regions[j].data<owner+owner_size;
                if(!later)boot_fm_section_free(owner);
                continue;}
#endif
            MEMORY_BASIC_INFORMATION mbi;   /* moved GPU blocks (VirtualAlloc) stay mapped */
            if(!VirtualQuery(c->regions[i].data,&mbi,sizeof mbi) || mbi.AllocationBase!=c->regions[i].data)free(c->regions[i].data);}
        else if(c->regions[i].reserved==3){
#if TOTK_FASTMEM
            if(boot_fm_section_free(c->regions[i].data))continue;
#endif
            VirtualFree(c->regions[i].data,0,MEM_RELEASE);}
        else if(c->regions[i].reserved==2 && c->memory_types[i]==6 &&
                c->regions[i].size==0x1000 && c->regions[i].data)UnmapViewOfFile(c->regions[i].data);
        else if(c->regions[i].reserved!=2){c->regions[keep]=c->regions[i];c->memory_types[keep++]=c->memory_types[i];}
    }
    for(unsigned i=keep;i<c->region_count;++i){memset(&c->regions[i],0,sizeof(Region));c->memory_types[i]=0;}
    c->region_count=keep;
    memset(c->aliases,0,sizeof(c->aliases));c->alias_count=0;boot_tlb_flush();
    memset(c->threads,0,sizeof(c->threads));c->thread_count=c->current_thread=0;
}
/* Cooperative execution of statically translated threads. All CPU state and
 * TLS are private; process memory/services remain shared. State 1 is created,
 * 2 runnable, 3 exited. Unsupported waits still stop before acknowledging. */
static inline void boot_save_thread(BootContext*c,uint64_t next){
    if(c->current_thread>=BOOT_THREAD_CAPACITY)return;   /* idle core (TOTK_CORES>1) */
    BootThread*t=&c->threads[c->current_thread];
    memcpy(t->x,c->x,sizeof(t->x));memcpy(t->q,c->q,sizeof(t->q));
    t->sp=c->sp;t->pc=next;t->tpidrro=c->tpidrro;t->tpidr=c->tpidr;
    t->n=c->n;t->z=c->z;t->carry=c->carry;t->v=c->v;
    t->fpcr=c->fpcr;t->fpsr=c->fpsr;
    t->schedule.last_scheduled=boot_order_now(c);
}
static inline void boot_load_thread(BootContext*c,unsigned index,uint64_t*target){
    if(index==BOOT_MT_IDLE){   /* TOTK_CORES>1: nothing to run on this core */
        c->exclusive_valid=0;c->current_thread=BOOT_MT_IDLE;*target=BOOT_MT_IDLE_PC;++c->thread_switch_count;return;
    }
    BootThread*t=&c->threads[index];
    memcpy(c->x,t->x,sizeof(c->x));memcpy(c->q,t->q,sizeof(c->q));
    c->sp=t->sp;c->tpidrro=t->tpidrro;c->tpidr=t->tpidr;
    c->n=t->n;c->z=t->z;c->carry=t->carry;c->v=t->v;
    c->fpcr=t->fpcr;c->fpsr=t->fpsr;
    c->exclusive_valid=0;c->current_thread=index;*target=t->pc;++c->thread_switch_count;
}
static inline int boot_reschedule(BootContext*c,uint64_t pc,uint64_t next,unsigned mode,uint64_t*target){
    *target=next;
    if(mode!=BOOT_SCHEDULE_CHECK)boot_time_current(c,BOOT_TS_YIELD,0);   /* SleepThread(0/-1) */
    if(!c->thread_count)return 1;
    unsigned chosen;
    if(!boot_scheduler_choose(c,mode,CORE_SCHEDULE_NONE,&chosen)){
        boot_fault(c,BOOT_SVC,pc,0,0);return 0;
    }
    if(chosen!=c->current_thread){boot_save_thread(c,next);boot_load_thread(c,chosen,target);}
    return 1;
}
static inline int boot_set_thread_priority(BootContext*c,uint64_t pc,uint64_t next,uint64_t*target){
    uint32_t handle=(uint32_t)c->x[0],priority=(uint32_t)c->x[1];
    if(priority>=64 || !(c->layout_info[1]&(UINT64_C(1)<<priority)))goto unsupported;
    /* Priority inheritance is not modelled anywhere in this scheduler: mutex owner and
     * waiter selection order by each thread's current priority. Setting a base priority
     * while other threads wait is therefore consistent with that model (previously this
     * stopped; the game reached it at ~12.03B instructions). */
    if(!c->thread_count){
        if(handle!=c->main_thread_handle && handle!=UINT32_C(0xffff8000))goto unsupported;
        c->main_thread_priority=priority;c->x[0]=0;*target=next;return 1;
    }
    for(unsigned i=0;i<c->thread_count;++i){BootThread*t=&c->threads[i];
        if(handle!=t->handle && !(handle==UINT32_C(0xffff8000) && i==c->current_thread))continue;
        if(t->state==3)goto unsupported;
        t->priority=priority;if(t->handle==c->main_thread_handle)c->main_thread_priority=priority;
        c->x[0]=0;*target=next;
        return boot_reschedule(c,pc,next,BOOT_SCHEDULE_CHECK,target);
    }
unsupported:
    boot_fault(c,BOOT_SVC,pc,0,0);return 0;
}
static inline int boot_create_thread(BootContext*c,uint64_t pc){
    uint64_t entry=c->x[1],arg=c->x[2],sp=c->x[3],priority=c->x[4],core=c->x[5];
    unsigned index=c->thread_count?c->thread_count:1;
    if(index>=BOOT_THREAD_CAPACITY || c->region_count>=BOOT_REGION_CAPACITY || c->next_session_handle>=0x7ffffffe ||
       priority>=64 || !(c->layout_info[1]&(UINT64_C(1)<<priority)) || core>=64 ||
       !(c->layout_info[0]&(UINT64_C(1)<<core)) || !sp || (sp&15))goto invalid;
    if(!boot_access(c,entry,4,EXECUTE,pc) || !boot_access(c,sp-16,16,READ|WRITE,pc))return 0;
    uint64_t tls=UINT64_C(0x7500000000)+(uint64_t)index*4096;
    for(unsigned i=0;i<c->region_count;++i)if(tls<c->regions[i].base+c->regions[i].size && c->regions[i].base<tls+4096)goto invalid;
    uint64_t used=c->system_resource_total;
    for(unsigned i=0;i<c->region_count;++i)if(c->regions[i].reserved!=2)used+=c->regions[i].size;
    if(used>c->memory_budget || 4096>c->memory_budget-used)goto invalid;
#if TOTK_FASTMEM
    uint8_t*data=boot_guest_alloc(4096);if(!data)goto invalid;
#else
    uint8_t*data=calloc(1,4096);if(!data)goto invalid;
#endif
    if(!c->thread_count){
        BootThread*main=&c->threads[0];main->handle=c->main_thread_handle;main->id=c->main_thread_id;
        main->priority=c->main_thread_priority;main->core_mask=c->layout_info[0];main->state=2;c->thread_count=1;c->current_thread=0;
    }
    if(c->next_session_handle<0x10000)c->next_session_handle=0x10000;
    BootThread*t=&c->threads[index];memset(t,0,sizeof(*t));
    t->handle=++c->next_session_handle;t->id=index+1;t->priority=(uint32_t)priority;t->core=(uint32_t)core;
    t->core_mask=UINT64_C(1)<<core;
    t->state=1;t->pc=entry;t->x[0]=arg;t->x[1]=t->handle;t->sp=sp;t->tpidrro=tls;
    /* A thread entry must call ExitThread; returning is an explicit boundary. */
    t->x[30]=0;
    unsigned region=c->region_count++;c->regions[region]=(Region){tls,4096,data,READ|WRITE,1};c->memory_types[region]=12;boot_tlb_flush();
    ++c->thread_count;++c->thread_create_count;c->x[0]=0;c->x[1]=t->handle;return 1;
invalid:
    boot_fault(c,BOOT_QUERY_SCOPE,pc,entry,0);return 0;
}
static inline int boot_schedule_thread(BootContext*c,uint64_t pc,uint64_t next,unsigned svc,uint64_t*target){
    if(svc==9){
        for(unsigned i=1;i<c->thread_count;++i)if(c->threads[i].handle==c->x[0]){
            if(c->threads[i].state!=1)break;
            c->threads[i].state=2;
            unsigned chosen;
            if(!boot_scheduler_choose(c,BOOT_SCHEDULE_CHECK,CORE_SCHEDULE_NONE,&chosen)){
                c->threads[i].state=1;break;
            }
            c->x[0]=0;++c->thread_start_count;*target=next;
            if(chosen!=c->current_thread){boot_save_thread(c,next);boot_load_thread(c,chosen,target);}
            return 1;
        }
    }else if(svc==10 && c->thread_count && c->current_thread){
        unsigned chosen=BOOT_THREAD_CAPACITY;
        for(unsigned i=0;i<c->thread_count;++i)if(i!=c->current_thread){BootThread*t=&c->threads[i];
            if(t->state==5)for(unsigned w=0;w<t->wait_count;++w)if(t->wait_threads[w]==c->current_thread+1){boot_finish_wait(t,0,(int)w);break;}
            if(t->state==2 && (chosen==BOOT_THREAD_CAPACITY || t->priority<c->threads[chosen].priority))chosen=i;
        }
        if(chosen!=BOOT_THREAD_CAPACITY || boot_mt_in_kernel()){
            if(!boot_scheduler_choose(c,BOOT_SCHEDULE_CHECK,c->current_thread,&chosen)){
                boot_fault(c,BOOT_SVC,pc,0,0);return 0;
            }
            boot_save_thread(c,next);c->threads[c->current_thread].state=3;boot_load_thread(c,chosen,target);return 1;
        }
    }
    boot_fault(c,BOOT_SVC,pc,0,0);return 0;
}
static inline int boot_thread_affinity(BootContext*c,uint64_t pc,unsigned svc){
    uint64_t handle=svc==14?c->x[2]:c->x[0];
    for(unsigned i=0;i<c->thread_count;++i){BootThread*t=&c->threads[i];
        if(handle!=t->handle && !(handle==UINT64_C(0xffff8000) && i==c->current_thread))continue;
        if(svc==14){c->x[1]=t->core;c->x[2]=t->core_mask;c->x[0]=0;return 1;}
        uint32_t core=(uint32_t)c->x[1];uint64_t mask=c->x[2];
        /* -1 means no preferred virtual core; the allowed mask still applies. */
        if(!mask || (mask&~c->layout_info[0]))break;
        if(core!=UINT32_MAX && (core>=64 || !(mask&(UINT64_C(1)<<core))))break;
        CoreScheduleThread schedule=t->schedule;
        if(!schedule.affinity)schedule.active=t->core;
        if(!core_schedule_affinity(&schedule,mask,core,c->layout_info[0]))break;
        t->schedule=schedule;t->core=core;t->core_mask=mask;c->x[0]=0;return 1;
    }
    boot_fault(c,BOOT_SVC,pc,0,0);return 0;
}
/* Guest synchronization words written by the kernel (mutex tags, condition-variable keys, address-
 * arbiter counters). With several host cores user code CASes these words while the kernel works on
 * them, so the kernel uses host atomics; single-threaded results equal little_read/little_write
 * (x86-64 is little-endian like the guest). Callers pass 4-byte-aligned guest words. */
static inline uint32_t boot_kernel_load32(const uint8_t*p){return __atomic_load_n((const uint32_t*)(const void*)p,__ATOMIC_SEQ_CST);}
static inline void boot_kernel_store32(uint8_t*p,uint32_t v){__atomic_store_n((uint32_t*)(void*)p,v,__ATOMIC_SEQ_CST);}
/* Kernel read-modify-write of a guest sync word. With guest threads on several host threads (TOTK_CORES>1,
 * TOTK_THREADS=host) user code changes these words concurrently with LDAXR/STLXR (a compare-exchange on
 * the host) while the kernel runs, so a kernel load followed by a store can lose a user update (a mutex
 * acquired in user space overwritten by a condition-variable hand-off). Like the Horizon kernel, the
 * kernel updates them with a compare-exchange and re-evaluates on failure. */
static inline int boot_kernel_cas32(uint8_t*p,uint32_t expected,uint32_t desired){
    return __atomic_compare_exchange_n((uint32_t*)(void*)p,&expected,desired,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);
}
/* STLR/STLRB/STLRH (the emitter checks alignment first). ARMv8 store-release is RCsc: a later LDAR
 * may not be satisfied before it, which on x86 needs the store followed by a full fence (the C11
 * seq_cst store mapping). LDAR stays a plain load plus acquire fence (x86 loads are acquire). */
static inline int boot_store(BootContext*c,uint64_t a,unsigned w,uint64_t pc,uint64_t value);
static inline int boot_store_release(BootContext*c,uint64_t a,unsigned w,uint64_t pc,uint64_t value){
    if(!boot_store(c,a,w,pc,value))return 0;
    atomic_thread_fence(memory_order_seq_cst);return 1;
}
static inline int boot_mutex(BootContext*c,uint64_t pc,uint64_t next,unsigned svc,uint64_t*target){
    if(!c->thread_count || c->thread_count>BOOT_THREAD_CAPACITY || !boot_condition_expire(c))goto unsupported;
    uint64_t address=svc==0x1a?c->x[1]:c->x[0];
    if(address&3)goto unsupported;
    uint8_t*word=boot_access(c,address,4,READ|WRITE,pc);if(!word)return 0;
    BootThread*self=&c->threads[c->current_thread];
    if(svc==0x1a){
        uint64_t owner=c->x[0];
        if(c->x[2]!=self->handle || owner==self->handle)goto unsupported;
        if(boot_kernel_load32(word)!=(owner|UINT64_C(0x40000000))){c->x[0]=0;*target=next;return 1;}
        for(unsigned i=0;i<c->thread_count;++i)if(c->threads[i].handle==owner &&
            c->threads[i].state>=2 && c->threads[i].state<=8 && c->threads[i].state!=3){
            unsigned chosen;
            if(!boot_scheduler_choose(c,BOOT_SCHEDULE_CHECK,c->current_thread,&chosen))goto unsupported;
            boot_save_thread(c,next);self->state=4;self->wait_address=address;self->wait_owner=(uint32_t)owner;
            boot_load_thread(c,chosen,target);return 1;
        }
    }else{
        unsigned chosen=BOOT_THREAD_CAPACITY,count=0;
        for(unsigned i=0;i<c->thread_count;++i){BootThread*t=&c->threads[i];
            if(t->state==4 && t->wait_address==address && t->wait_owner==self->handle){
                ++count;if(chosen==BOOT_THREAD_CAPACITY || t->priority<c->threads[chosen].priority)chosen=i;
            }
        }
        uint32_t value=chosen==BOOT_THREAD_CAPACITY?0:c->threads[chosen].handle|(count>1?UINT32_C(0x40000000):0);
        boot_kernel_store32(word,value);c->x[0]=0;*target=next;
        if(chosen!=BOOT_THREAD_CAPACITY){
            BootThread*t=&c->threads[chosen];t->state=2;t->wait_address=0;t->wait_owner=0;t->x[0]=0;t->wait_native[2]=0;
            for(unsigned i=0;i<c->thread_count;++i)if(c->threads[i].state==4 && c->threads[i].wait_address==address && c->threads[i].wait_owner==self->handle)c->threads[i].wait_owner=t->handle;
            if(!boot_reschedule(c,pc,next,BOOT_SCHEDULE_CHECK,target))return 0;
        }
        return 1;
    }
unsupported:
    {   /* one diagnostic line (stderr) for a rejected ArbitrateLock/Unlock: which check failed */
        uint64_t a=svc==0x1a?c->x[1]:c->x[0];uint32_t w=0;const Region*r=NULL;
        for(unsigned i=0;i<c->region_count && !r;++i)if(a>=c->regions[i].base && a-c->regions[i].base+4<=c->regions[i].size)r=&c->regions[i];
        if(r)memcpy(&w,r->data+(size_t)(a-r->base),4);
        int owner_state=-1;for(unsigned i=0;i<c->thread_count;++i)if(c->threads[i].handle==(uint32_t)c->x[0])owner_state=(int)c->threads[i].state;
        unsigned by_tls=BOOT_THREAD_CAPACITY,by_handle=BOOT_THREAD_CAPACITY;
        for(unsigned i=0;i<c->thread_count;++i){if(c->threads[i].tpidrro==c->tpidrro)by_tls=i;if(c->threads[i].handle==(uint32_t)c->x[2])by_handle=i;}
        fprintf(stderr,"svc %#x: tpidrro %#llx is thread %u's; x2 is thread %u's handle (tpidrro %#llx)\n",svc,(unsigned long long)c->tpidrro,by_tls,by_handle,
                (unsigned long long)(by_handle<c->thread_count?c->threads[by_handle].tpidrro:0));
        fprintf(stderr,"svc %#x rejected: x0=%#llx x1=%#llx x2=%#llx word=%#x thread=%u handle=%#x state=%u owner_state=%d threads=%u\n",svc,
                (unsigned long long)c->x[0],(unsigned long long)c->x[1],(unsigned long long)c->x[2],w,c->current_thread,
                c->current_thread<c->thread_count?c->threads[c->current_thread].handle:0,c->current_thread<c->thread_count?c->threads[c->current_thread].state:0,owner_state,c->thread_count);
    }
    boot_fault(c,BOOT_SVC,pc,0,0);return 0;
}
static inline void boot_invalidate_exclusive(BootContext*c,uint64_t a,unsigned w);
/* State 7 waits on a condition variable: wait_address is the mutex,
 * wait_native[0] the condition key, and wait_native[1] its enqueue step.
 * wait_native[2] holds a finite deadline (zero for infinite).
 * Plan queue and guest-word changes privately before any commit. */
static inline int boot_condition_variable(BootContext*c,uint64_t pc,uint64_t next,unsigned svc,uint64_t*target){
    BootSchedUndo undo_record,*undo=NULL;BootContext*p=c;   /* plan in place; undo restores a rejected plan */
    if(!c->thread_count || c->thread_count>BOOT_THREAD_CAPACITY || c->current_thread>=c->thread_count ||
       c->threads[c->current_thread].state!=2 || (svc!=0x1c && svc!=0x1d))goto unsupported;
    uint64_t key=svc==0x1c?c->x[1]:c->x[0];
    if(key&3)goto unsupported;
    uint64_t addresses[BOOT_THREAD_CAPACITY+1];uint8_t*words[BOOT_THREAD_CAPACITY+1];uint32_t values[BOOT_THREAD_CAPACITY+1];
    uint8_t applied[BOOT_THREAD_CAPACITY+1]={0};
    unsigned writes=1;addresses[0]=key;
    words[0]=boot_access(c,key,4,READ|WRITE,pc);if(!words[0])return 0;
    values[0]=(uint32_t)little_read(words[0],4);
    undo=&undo_record;boot_sched_undo_save(c,undo,1);
    if(!boot_condition_expire(p))goto unsupported;
    BootThread*self=&p->threads[p->current_thread];unsigned chosen;
    if(svc==0x1c){
        uint64_t address=c->x[0];int64_t timeout=(int64_t)c->x[3];
        if((address&3) || address==key || c->x[2]!=self->handle)goto unsupported;
        uint64_t deadline=0;
        if(timeout>0){
            uint64_t now;if(!boot_condition_clock(p,&now))goto unsupported;
            __uint128_t delta=((__uint128_t)(uint64_t)timeout*UINT64_C(19200000)+UINT64_C(999999999))/UINT64_C(1000000000);
            if(delta>UINT64_MAX-now)goto unsupported;
            deadline=now+(uint64_t)delta;
        }
        words[1]=boot_access(c,address,4,READ|WRITE,pc);if(!words[1])goto access_failed;
        uint32_t old=(uint32_t)little_read(words[1],4);
        if((old&~UINT32_C(0x40000000))!=self->handle)goto unsupported;
        unsigned owner=BOOT_THREAD_CAPACITY,count=0;
        for(unsigned i=0;i<p->thread_count;++i){BootThread*t=&p->threads[i];
            if(t->state==4 && t->wait_address==address && t->wait_owner==self->handle){
                ++count;
                if(owner==BOOT_THREAD_CAPACITY || t->priority<p->threads[owner].priority)owner=i;
            }
        }
        values[1]=owner==BOOT_THREAD_CAPACITY?0:p->threads[owner].handle|(count>1?UINT32_C(0x40000000):0);
        addresses[1]=address;writes=2;values[0]=1;
        if(owner!=BOOT_THREAD_CAPACITY){
            BootThread*t=&p->threads[owner];t->state=2;t->wait_address=0;t->wait_owner=0;t->x[0]=0;t->wait_native[2]=0;
            for(unsigned i=0;i<p->thread_count;++i)if(p->threads[i].state==4 && p->threads[i].wait_address==address && p->threads[i].wait_owner==self->handle)p->threads[i].wait_owner=t->handle;
        }
        if(timeout){
            boot_save_thread(p,next);self->state=7;self->wait_address=address;self->wait_owner=0;
            self->wait_native[0]=key;self->wait_native[1]=boot_order_now(c);self->wait_native[2]=deadline;self->wait_count=0;
        }else p->x[0]=UINT32_C(0xea01); /* Caller reacquires after timeout. */
        if(!boot_scheduler_choose(p,BOOT_SCHEDULE_CHECK,CORE_SCHEDULE_NONE,&chosen))goto unsupported;
        *target=next;
        if(chosen!=p->current_thread || timeout){if(!timeout)boot_save_thread(p,next);boot_load_thread(p,chosen,target);}
    }else{
        int32_t count=(int32_t)c->x[1];
        unsigned limit=count<=0?p->thread_count:(unsigned)count;
        for(unsigned n=0;n<limit && n<p->thread_count;++n){
            unsigned waiter=BOOT_THREAD_CAPACITY;
            for(unsigned i=0;i<p->thread_count;++i){BootThread*t=&p->threads[i];
                if(t->state!=7 || t->wait_native[0]!=key)continue;
                if(waiter==BOOT_THREAD_CAPACITY || t->priority<p->threads[waiter].priority ||
                   (t->priority==p->threads[waiter].priority && t->wait_native[1]<p->threads[waiter].wait_native[1]))waiter=i;
            }
            if(waiter==BOOT_THREAD_CAPACITY)break;
            BootThread*t=&p->threads[waiter];uint64_t address=t->wait_address;
            if((address&3) || address==key)goto unsupported;
            unsigned index=0;while(index<writes && addresses[index]!=address)++index;
            if(index==writes){
                if(writes==BOOT_THREAD_CAPACITY+1)goto unsupported;
                words[index]=boot_access(c,address,4,READ|WRITE,pc);if(!words[index])goto access_failed;
                addresses[index]=address;values[index]=(uint32_t)little_read(words[index],4);++writes;
            }
            /* The mutex word is updated now with a compare-exchange (a user thread may be acquiring or
             * releasing it concurrently) and skipped by the final store loop (applied). */
            for(;;){
                uint32_t old=boot_kernel_load32(words[index]);
                if(!old){
                    if(!boot_kernel_cas32(words[index],0,t->handle))continue;
                    values[index]=t->handle;applied[index]=1;
                    t->state=2;t->wait_address=0;t->wait_owner=0;t->x[0]=0;t->wait_native[2]=0;
                    break;
                }
                uint32_t owner=old&~UINT32_C(0x40000000);unsigned i=0;
                while(i<p->thread_count && p->threads[i].handle!=owner)++i;
                if(i==p->thread_count || i==waiter || p->threads[i].state<2 || p->threads[i].state==3)goto unsupported;
                if(!(old&UINT32_C(0x40000000)) && !boot_kernel_cas32(words[index],old,old|UINT32_C(0x40000000)))continue;
                values[index]=old|UINT32_C(0x40000000);applied[index]=1;t->state=4;t->wait_owner=owner;
                break;
            }
            t->wait_native[0]=t->wait_native[1]=0;
        }
        unsigned remaining=0;
        for(unsigned i=0;i<p->thread_count;++i)remaining+=p->threads[i].state==7 && p->threads[i].wait_native[0]==key;
        if(!remaining)values[0]=0;
        if(!boot_scheduler_choose(p,BOOT_SCHEDULE_CHECK,CORE_SCHEDULE_NONE,&chosen))goto unsupported;
        *target=next;
        if(chosen!=p->current_thread){boot_save_thread(p,next);boot_load_thread(p,chosen,target);}
    }
    atomic_thread_fence(memory_order_release);
    for(unsigned i=0;i<writes;++i){boot_invalidate_exclusive(p,addresses[i],4);if(!applied[i])boot_kernel_store32(words[i],values[i]);}
    return 1;
unsupported:
    if(undo)boot_sched_undo_restore(c,undo,BOOT_UNDO_ALL);
    boot_fault(c,BOOT_SVC,pc,0,0);return 0;
access_failed:   /* keep boot_access's fault, undo the plan */
    boot_sched_undo_restore(c,undo,BOOT_UNDO_THREADS|BOOT_UNDO_CPU|BOOT_UNDO_CLOCK);
    return 0;
}
/* WaitForAddress park (address_arbiter_emit.py): mark the caller as an address waiter, choose the
 * next thread, then apply the type-1 decrement and switch. A rejected plan restores the context
 * and faults BOOT_SVC at pc; nothing is written to guest memory. Out of line so translated pages
 * neither copy the context nor carry the undo record in their frames. */
static inline __attribute__((noinline)) int boot_address_wait_block(BootContext*c,uint64_t pc,uint64_t next,uint8_t*word,
        uint32_t type,uint32_t old,uint64_t address,uint64_t deadline,uint64_t*target){
    BootSchedUndo undo;boot_sched_undo_save(c,&undo,1);
    boot_save_thread(c,next);
    BootThread*waiter=&c->threads[c->current_thread];
    waiter->state=6;waiter->wait_address=address;waiter->wait_native[2]=deadline;
    unsigned chosen=BOOT_THREAD_CAPACITY;
    if(!boot_scheduler_choose(c,BOOT_SCHEDULE_CHECK,CORE_SCHEDULE_NONE,&chosen)){
        boot_sched_undo_restore(c,&undo,BOOT_UNDO_ALL);boot_fault(c,BOOT_SVC,pc,0,0);return 0;
    }
    if(type==1)boot_kernel_store32(word,old-1);
    boot_load_thread(c,chosen,target);return 1;
}
static inline int boot_poll_wait(BootContext*c,BootThread*t,uint64_t pc){
    for(unsigned i=0;i<t->wait_count;++i){
        if(t->wait_threads[i]){if(c->threads[t->wait_threads[i]-1].state==3)return (int)i;}
        else {DWORD value=WaitForSingleObject((HANDLE)(uintptr_t)t->wait_native[i],0);
            if(value==WAIT_OBJECT_0)return (int)i;
            if(value!=WAIT_TIMEOUT){boot_fault(c,BOOT_SVC,pc,0,0);return -2;}
        }
    }
    return -1;
}
static inline void boot_finish_wait(BootThread*t,uint32_t result,int index){
    boot_release_wait(t);t->state=2;t->x[0]=result;t->x[1]=(uint32_t)index;
}
static inline int boot_wait_synchronization(BootContext*c,uint64_t pc,uint64_t next,uint64_t*target){
    BootThread pending={0};uint64_t count=c->x[2],timeout=c->x[3];
    if(count>64)goto unsupported;
    uint8_t*handles=count?boot_access(c,c->x[1],(unsigned)count*4,READ,pc):NULL;
    if(count && !handles)return 0;
    c->debug_wait_pc=pc;c->debug_wait_timeout=timeout;c->debug_wait_count=(uint32_t)count;
    pending.wait_count=(uint32_t)count;
    for(unsigned i=0;i<count;++i){uint32_t handle=(uint32_t)little_read(handles+i*4,4);int found=0;
        c->debug_wait_handles[i]=handle;
        for(unsigned j=0;j<16;++j)if(c->event_handles[j] && c->event_handles[j]==handle){
            HANDLE duplicate=NULL;
            if(!DuplicateHandle(GetCurrentProcess(),(HANDLE)(uintptr_t)c->event_native_handles[j],GetCurrentProcess(),&duplicate,0,FALSE,DUPLICATE_SAME_ACCESS))goto unsupported;
            pending.wait_native[i]=(uint64_t)(uintptr_t)duplicate;found=1;break;
        }
        if(!found)for(unsigned j=0;j<c->thread_count;++j)if(c->threads[j].handle==handle){pending.wait_threads[i]=j+1;found=1;break;}
        if(!found)goto unsupported;
    }
    int ready=boot_poll_wait(c,&pending,pc);
    if(ready==-2){boot_release_wait(&pending);return 0;}
    if(ready>=0 || timeout==0){
        boot_time_current(c,ready>=0?BOOT_TS_WAITSYNC_READY:BOOT_TS_WAITSYNC_POLL_MISS,0);
        boot_release_wait(&pending);c->x[0]=ready>=0?0:UINT32_C(0xea01);c->x[1]=(uint32_t)ready;*target=next;return 1;}
    if(!c->thread_count)goto unsupported;
    BootThread*self=&c->threads[c->current_thread];
    if(self->wait_cancel){self->wait_cancel=0;boot_release_wait(&pending);c->x[0]=UINT32_C(0xec01);c->x[1]=UINT32_MAX;*target=next;return 1;}
    /* Finite timers and an all-blocked process require an idle host loop;
     * stop explicitly until that backend is supplied. Negative means infinite. */
    if((int64_t)timeout>=0)goto unsupported;
    unsigned chosen=BOOT_THREAD_CAPACITY;
    if(boot_mt_in_kernel())goto choose;   /* TOTK_CORES>1: other cores run; this one may idle */
    for(unsigned i=0;i<c->thread_count;++i)if(i!=c->current_thread){BootThread*t=&c->threads[i];
        if(t->state==5){int index=boot_poll_wait(c,t,pc);if(index==-2)goto unsupported;if(index>=0)boot_finish_wait(t,0,index);}
        if(t->state==2 && (chosen==BOOT_THREAD_CAPACITY || t->priority<c->threads[chosen].priority))chosen=i;
    }
    while(chosen==BOOT_THREAD_CAPACITY && c->window_native){
        if(!boot_condition_expire(c))goto unsupported;
        if(boot_window_is_closed((BootWindow*)(uintptr_t)c->window_native)){
            boot_release_wait(&pending);boot_fault(c,BOOT_WINDOW_CLOSED,pc,0,0);return 0;
        }
        ready=boot_poll_wait(c,&pending,pc);
        if(ready==-2)goto unsupported;
        if(ready>=0){boot_release_wait(&pending);c->x[0]=0;c->x[1]=(uint32_t)ready;*target=next;return 1;}
        for(unsigned i=0;i<c->thread_count;++i)if(i!=c->current_thread){BootThread*t=&c->threads[i];
            if(t->state==5){int index=boot_poll_wait(c,t,pc);if(index==-2)goto unsupported;if(index>=0)boot_finish_wait(t,0,index);}
            if(t->state==2 && (chosen==BOOT_THREAD_CAPACITY || t->priority<c->threads[chosen].priority))chosen=i;
        }
        /* All blocked: idle on guest time (fast-forward to the next guest deadline within the
         * credit, firing guest-timed events), else let real time pass. */
        if(chosen==BOOT_THREAD_CAPACITY && !boot_guest_clock_idle(c))Sleep(1);
    }
    if(chosen==BOOT_THREAD_CAPACITY)goto unsupported;
choose:
    if(!boot_scheduler_choose(c,BOOT_SCHEDULE_CHECK,c->current_thread,&chosen))goto unsupported;
    boot_time_current(c,BOOT_TS_WAITSYNC_BLOCK,0);
    boot_save_thread(c,next);self->state=5;self->wait_count=pending.wait_count;
    memcpy(self->wait_native,pending.wait_native,sizeof(self->wait_native));
    memcpy(self->wait_threads,pending.wait_threads,sizeof(self->wait_threads));
    boot_load_thread(c,chosen,target);return 1;
unsupported:
    boot_release_wait(&pending);boot_fault(c,BOOT_SVC,pc,0,0);return 0;
}
static inline int boot_cancel_synchronization(BootContext*c,uint64_t pc,uint64_t next,uint64_t*target){
    for(unsigned i=0;i<c->thread_count;++i){BootThread*t=&c->threads[i];
        if(c->x[0]!=t->handle && !(c->x[0]==UINT64_C(0xffff8000) && i==c->current_thread))continue;
        if(t->state==5)boot_finish_wait(t,UINT32_C(0xec01),-1);else t->wait_cancel=1;
        c->x[0]=0;*target=next;
        return boot_reschedule(c,pc,next,BOOT_SCHEDULE_CHECK,target);
    }
    boot_fault(c,BOOT_SVC,pc,0,0);return 0;
}
static inline int boot_reset_signal(BootContext*c,uint64_t pc){
    for(unsigned i=0;i<16;++i)if(c->event_handles[i] && c->event_handles[i]==c->x[0]){
        HANDLE handle=(HANDLE)(uintptr_t)c->event_native_handles[i];DWORD state=WaitForSingleObject(handle,0);
        if(state==WAIT_TIMEOUT){c->x[0]=UINT32_C(0xfa01);return 1;}
        if(state!=WAIT_OBJECT_0 || !ResetEvent(handle))break;
        c->x[0]=0;return 1;
    }
    boot_fault(c,BOOT_SVC,pc,0,0);return 0;
}
/* MapMemory shares backing storage while locking the original virtual range.
 * Whole alias lifetimes only; unsupported partial operations stop atomically. */
static inline int boot_alias_memory(BootContext*c,uint64_t pc,unsigned svc){
    uint64_t dst=c->x[0],src=c->x[1],size=c->x[2];
    uint64_t stack=c->layout_info[14],extent=c->layout_info[15];
    if(src<=UINT64_MAX-size)for(unsigned i=0;i<16;++i){BootTransfer*t=&c->transfers[i];if(t->handle && src<t->base+t->size && t->base<src+size)goto invalid;}
    if(!size || ((dst|src|size)&4095) || src>UINT64_MAX-size ||
       dst<stack || dst>=stack+extent || size>stack+extent-dst)goto invalid;
    if(svc==5){
        for(unsigned a=0;a<c->alias_count;++a){BootAlias*m=&c->aliases[a];
            if(m->source!=src || m->destination!=dst || m->size!=size)continue;
            for(unsigned t=0;t<c->thread_count;++t)if(c->threads[t].state!=3){
                uint64_t sp=t==c->current_thread?c->sp:c->threads[t].sp;
                if(sp>dst && sp<=dst+size)goto invalid;
            }
            for(unsigned i=0;i<c->region_count;++i)if(c->regions[i].base==dst && c->regions[i].reserved==2){
                --c->region_count;
                for(unsigned j=i;j<c->region_count;++j){c->regions[j]=c->regions[j+1];c->memory_types[j]=c->memory_types[j+1];}
                memset(&c->regions[c->region_count],0,sizeof(Region));c->memory_types[c->region_count]=0;
                --c->alias_count;for(unsigned j=a;j<c->alias_count;++j)c->aliases[j]=c->aliases[j+1];
                memset(&c->aliases[c->alias_count],0,sizeof(BootAlias));
                ++c->alias_unmap_count;
#if TOTK_FASTMEM
                ++boot_fm.unmap_svc;
#endif
                boot_tlb_flush();c->x[0]=0;return 1;
            }
            goto invalid;
        }
        goto invalid;
    }
    if(c->alias_count==BOOT_ALIAS_CAPACITY || c->region_count==BOOT_REGION_CAPACITY)goto invalid;
    if(boot_attributes_overlap(&c->memory_attributes,src,src+size))goto invalid;
    if(c->window_native && (boot_gpu_mapping_cpu_overlap(&((BootWindow*)(uintptr_t)c->window_native)->gpu_mappings,src,src+size) || boot_graphic_cpu_overlap(&((BootWindow*)(uintptr_t)c->window_native)->graphic_buffers,src,src+size)))goto invalid;
    for(unsigned a=0;a<c->alias_count;++a){BootAlias*m=&c->aliases[a];
        if(src<m->source+m->size && m->source<src+size)goto invalid;
    }
    Region*source=NULL;
    for(unsigned i=0;i<c->region_count;++i){Region*r=&c->regions[i];
        if(dst<r->base+r->size && r->base<dst+size)goto invalid;
        if(src>=r->base && size<=r->size && src-r->base<=r->size-size &&
           r->permissions==(READ|WRITE) && r->reserved!=2 && (c->memory_types[i]==4 || c->memory_types[i]==5))source=r;
    }
    if(!source)goto invalid;
    unsigned index=c->region_count++;
    c->regions[index]=(Region){dst,size,source->data+(size_t)(src-source->base),READ|WRITE,2};
    c->memory_types[index]=11;c->aliases[c->alias_count++]=(BootAlias){src,dst,size};
    ++c->alias_map_count;
#if TOTK_FASTMEM
    ++boot_fm.map_svc;
#endif
    boot_tlb_flush();c->x[0]=0;return 1;
invalid:
    boot_fault(c,BOOT_QUERY_SCOPE,pc,dst,size);return 0;
}
static inline int boot_physical_memory(BootContext*c,uint64_t pc,unsigned svc){
    uint64_t base=c->x[0],size=c->x[1],alias=c->layout_info[2],extent=c->layout_info[3];
    if(!c->system_resource_total || !size || ((base|size)&4095) || base<alias || base>=alias+extent || size>alias+extent-base || size>UINT64_C(0x10000000)){
        boot_fault(c,BOOT_QUERY_SCOPE,pc,base,size);return 0;
    }
    if(svc==0x2d){
        /* static: ~1.7 MB at 32768 regions would overflow a host thread stack; svc handling is serialized (kernel lock) */
        static Region kept[BOOT_REGION_CAPACITY];static uint32_t types[BOOT_REGION_CAPACITY];
        static uint8_t*allocated[BOOT_REGION_CAPACITY];unsigned count=0,alloc_count=0;int changed=0;
        BootMemoryAttributes next_attributes=c->memory_attributes;
        if(!boot_attributes_update(&next_attributes,base,base+size,0))goto unmap_invalid;
        if(c->window_native && (boot_gpu_mapping_cpu_overlap(&((BootWindow*)(uintptr_t)c->window_native)->gpu_mappings,base,base+size) || boot_graphic_cpu_overlap(&((BootWindow*)(uintptr_t)c->window_native)->graphic_buffers,base,base+size)))goto unmap_invalid;
        for(unsigned j=0;j<c->alias_count;++j)if(c->aliases[j].source<base+size && base<c->aliases[j].source+c->aliases[j].size)goto unmap_invalid;
        if(c->sp>base && c->sp<=base+size)goto unmap_invalid;
        for(unsigned j=0;j<c->thread_count;++j){BootThread*t=&c->threads[j];if(t->state!=3 && t->sp>base && t->sp<=base+size)goto unmap_invalid;}
        /* Preflight the entire range before allocating or releasing any page. */
        for(unsigned j=0;j<c->region_count;++j){Region*r=&c->regions[j];
            if(base>=r->base+r->size || r->base>=base+size){++count;continue;}
            if(r->reserved!=1 || c->memory_types[j]!=5 || r->permissions!=(READ|WRITE))goto unmap_invalid;
            count+=(r->base<base)+(r->base+r->size>base+size);changed=1;
        }
        if(count>BOOT_REGION_CAPACITY)goto unmap_invalid;
        count=0;
        /* Split in place: the pages of a partly unmapped region that stay mapped keep their host memory
         * (an interior pointer of the same fastmem section or GPU arena; host addend unchanged). With
         * TOTK_CORES>1 the other cores read and write those pages through the fastmem page table while
         * this runs under the kernel lock only, so moving them (copy to new memory, free the old block)
         * lost their concurrent writes and let them read the freed block through the not yet re-synced
         * table: the 3-core access violation in boot_load_fm_slow. A section is released below once no
         * region points into it. Memory that is not a section (plain calloc/VirtualAlloc blocks) is
         * still copied. */
        for(unsigned j=0;j<c->region_count;++j){Region*r=&c->regions[j];
            if(base>=r->base+r->size || r->base>=base+size){kept[count]=*r;types[count++]=c->memory_types[j];continue;}
            uint64_t starts[2]={r->base,base+size};
            uint64_t sizes[2]={r->base<base?base-r->base:0,r->base+r->size>base+size?r->base+r->size-base-size:0};
            int in_place=0;
#ifdef TOTK_DRAW_PATH
            if(boot_gpu_arena_base(r->data))in_place=1;
#endif
#if TOTK_FASTMEM
            if(boot_fm_owns(r->data))in_place=1;
#endif
            for(unsigned k=0;k<2;++k)if(sizes[k]){
                uint8_t*data=r->data+(size_t)(starts[k]-r->base);
                if(!in_place){
#if TOTK_FASTMEM
                    data=boot_guest_alloc(sizes[k]);if(!data)goto unmap_invalid;
#else
                    data=malloc((size_t)sizes[k]);if(!data)goto unmap_invalid;
#endif
                    allocated[alloc_count++]=data;memcpy(data,r->data+(size_t)(starts[k]-r->base),(size_t)sizes[k]);
                }
                kept[count]=(Region){starts[k],sizes[k],data,READ|WRITE,1};types[count++]=5;
            }
        }
#if TOTK_FASTMEM
        /* sections of unmapped regions; reuses allocated[] (no failure path below needs it) */
        uint8_t**owners=allocated;unsigned owner_count=0;
#endif
#ifdef TOTK_DRAW_PATH
        /* GPU arenas holding unmapped regions: released below once no region points into them
         * (before, an arena lived until a later submission repacked it, or forever: dead commit). */
        uint8_t*arenas[64];unsigned arena_count=0;int gpu_idle=0;
        BootWindow*fw=(BootWindow*)(uintptr_t)c->window_native;
#endif
        for(unsigned j=0;j<c->region_count;++j){Region*r=&c->regions[j];if(base<r->base+r->size && r->base<base+size){
            /* A block moved to VirtualAlloc for the GPU may still back a D3D12 heap: keep it. */
            MEMORY_BASIC_INFORMATION mbi;
#ifdef TOTK_DRAW_PATH
            {uint8_t*arena=boot_gpu_arena_base(r->data);
             if(arena && boot_gpu_pool_arena(arena)){   /* pool memory: the unmapped pages go back to the pool */
                uint64_t lo=r->base>base?r->base:base,hi=r->base+r->size<base+size?r->base+r->size:base+size;
                boot_gpu_pool_free(r->data+(size_t)(lo-r->base),hi-lo);continue;}
             if(arena){unsigned q=0;while(q<arena_count && arenas[q]!=arena)++q;if(q==arena_count && arena_count<64)arenas[arena_count++]=arena;continue;}}
#endif
#if TOTK_FASTMEM
            if(boot_fm_owns(r->data)){   /* the owning section: released below if nothing points into it */
                uint8_t*owner=boot_fm.allocs[boot_fm_alloc_find(r->data)].host;
                unsigned q=0;while(q<owner_count && owners[q]!=owner)++q;
                if(q==owner_count)owners[owner_count++]=owner;
                continue;}
#endif
            if(!VirtualQuery(r->data,&mbi,sizeof mbi) || mbi.AllocationBase!=r->data)free(r->data);}}
        memset(c->regions,0,sizeof(c->regions));memset(c->memory_types,0,sizeof(c->memory_types));
        memcpy(c->regions,kept,count*sizeof(Region));memcpy(c->memory_types,types,count*sizeof(uint32_t));c->region_count=count;
        for(unsigned a=0;a<c->alias_count;++a){BootAlias*m=&c->aliases[a];
            for(unsigned j=0;j<count;++j){Region*r=&c->regions[j];
                if(r->reserved==2 || m->source<r->base || m->size>r->size || m->source-r->base>r->size-m->size)continue;
                for(unsigned k=0;k<count;++k)if(c->regions[k].reserved==2 && c->regions[k].base==m->destination)c->regions[k].data=r->data+(size_t)(m->source-r->base);
                break;
            }
        }
        boot_tlb_flush();   /* detach the page table before any host memory goes away */
#if TOTK_FASTMEM
        for(unsigned q=0;q<owner_count;++q){uint8_t*owner=owners[q];int index=boot_fm_alloc_find(owner);
            if(index<0)continue;
            uint64_t owner_size=boot_fm.allocs[index].size;int used=0;
            for(unsigned j=0;j<c->region_count && !used;++j)used=c->regions[j].data>=owner && c->regions[j].data<owner+owner_size;
            if(used)continue;
#ifdef TOTK_DRAW_PATH
            if(fw && fw->render){
                /* a D3D12 heap over this section may still be used by submitted work */
                for(unsigned h=0;h<fw->render->region_count;++h){uint8_t*cpu=fw->render->region_cpu[h];
                    if(!cpu || cpu<owner || cpu>=owner+owner_size)continue;
                    if(!gpu_idle){boot_gpu_wait(fw->render->gpu);gpu_idle=1;}
                    boot_render_forget(fw->render,cpu);
                }
            }
            boot_gpu_arena_forget();
#endif
            boot_fm_section_free(owner);
        }
#endif
#ifdef TOTK_DRAW_PATH
        {static uint64_t released_arenas,released_bytes;
         for(unsigned q=0;q<arena_count;++q){uint8_t*a=arenas[q];uint64_t payload;memcpy(&payload,a+16,8);
            int used=0;for(unsigned j=0;j<c->region_count && !used;++j)used=c->regions[j].data>=a && c->regions[j].data<a+BOOT_GPU_ARENA_HEADER+payload;
            if(used)continue;
            if(fw && fw->render){if(!gpu_idle){boot_gpu_wait(fw->render->gpu);gpu_idle=1;}boot_render_forget(fw->render,a);}
            boot_gpu_arena_forget();
            ++released_arenas;released_bytes+=BOOT_GPU_ARENA_HEADER+payload;
#if TOTK_FASTMEM
            if(boot_fm_section_free(a)){boot_fm_log("released GPU arena %p (%llu KiB) on unmap; total %llu arenas %llu MiB",(void*)a,
                (unsigned long long)((BOOT_GPU_ARENA_HEADER+payload)>>10),(unsigned long long)released_arenas,(unsigned long long)(released_bytes>>20));continue;}
#endif
            VirtualFree(a,0,MEM_RELEASE);
         }}
#endif
        c->memory_attributes=next_attributes;boot_tlb_flush();
        if(changed)++c->physical_unmap_count;
#if TOTK_FASTMEM
        ++boot_fm.unmap_svc;
#endif
        c->x[0]=0;return 1;
unmap_invalid:
#if TOTK_FASTMEM
        for(unsigned j=0;j<alloc_count;++j)boot_guest_free(allocated[j]);
#else
        for(unsigned j=0;j<alloc_count;++j)free(allocated[j]);
#endif
        boot_fault(c,BOOT_QUERY_SCOPE,pc,base,size);return 0;
    }
    for(unsigned i=0;i<c->region_count;++i){Region*r=&c->regions[i];
        if(base<r->base+r->size && r->base<base+size){
            if(r->base!=base || r->size!=size || r->reserved!=1){boot_fault(c,BOOT_QUERY_SCOPE,pc,base,size);return 0;}
            c->x[0]=0;return 1;
        }
    }
    if(svc==0x2d || c->region_count==BOOT_REGION_CAPACITY){boot_fault(c,BOOT_QUERY_SCOPE,pc,base,size);return 0;}
    uint64_t used=c->system_resource_total;
    for(unsigned i=0;i<c->region_count;++i)if(c->regions[i].reserved!=2)used+=c->regions[i].size;
    if(used>c->memory_budget || size>c->memory_budget-used){boot_fault(c,BOOT_QUERY_SCOPE,pc,base,size);return 0;}
#if TOTK_FASTMEM
#ifdef TOTK_DRAW_PATH
    /* GPU-shareable from the start (pool arena, host_gpu_draw_exec.h): the submit paths never move it.
     * Direct mode only once arenas are sections (the guest view maps them). */
    uint8_t*data=(boot_fm.mode!=1 || boot_gpu_arena_alloc_hook)?boot_gpu_pool_alloc(size):NULL;
    if(!data)data=boot_guest_alloc(size);
#else
    uint8_t*data=boot_guest_alloc(size);
#endif
    if(!data){boot_fault(c,BOOT_QUERY_SCOPE,pc,base,size);return 0;}
#else
    uint8_t*data=calloc(1,(size_t)size);if(!data){boot_fault(c,BOOT_QUERY_SCOPE,pc,base,size);return 0;}
#endif
    unsigned index=c->region_count++;
    c->regions[index]=(Region){base,size,data,READ|WRITE,1};c->memory_types[index]=5;
    ++c->physical_map_count;
#if TOTK_FASTMEM
    ++boot_fm.map_svc;
#endif
    boot_tlb_flush();c->x[0]=0;return 1;
}
static inline int boot_set_heap_size(BootContext*c,uint64_t pc){
    uint64_t base=c->layout_info[4],maximum=c->layout_info[5],size=c->x[1],old_size=0;
    unsigned slot=BOOT_REGION_CAPACITY;uint8_t*data=NULL;
    if(!base || !maximum || maximum>SIZE_MAX || size>maximum || (size&UINT64_C(0x1fffff)))goto invalid;
    uint64_t used=c->system_resource_total;
    for(unsigned i=0;i<c->region_count;++i){Region*r=&c->regions[i];
        if(r->reserved==3){if(slot!=BOOT_REGION_CAPACITY || r->base!=base)goto invalid;slot=i;old_size=r->size;data=r->data;}
        else if(r->reserved!=2){if(r->size>UINT64_MAX-used)goto invalid;used+=r->size;}
    }
    if(used>c->memory_budget || size>c->memory_budget-used)goto invalid;
    if(size==old_size){c->x[0]=0;c->x[1]=base;return 1;}
    if(size<old_size){
        uint64_t first=base+size,last=base+old_size;
        if(c->window_native && (boot_gpu_mapping_cpu_overlap(&((BootWindow*)(uintptr_t)c->window_native)->gpu_mappings,first,last) || boot_graphic_cpu_overlap(&((BootWindow*)(uintptr_t)c->window_native)->graphic_buffers,first,last)))goto invalid;
        for(unsigned i=0;i<16;++i){BootTransfer*t=&c->transfers[i];if(t->handle && t->base<last && first<t->base+t->size)goto invalid;}
        for(unsigned a=0;a<c->alias_count;++a)if(c->aliases[a].source<last && first<c->aliases[a].source+c->aliases[a].size)goto invalid;
        if(c->sp>first && c->sp<=last)goto invalid;
        for(unsigned t=0;t<c->thread_count;++t)if(t!=c->current_thread && c->threads[t].state && c->threads[t].state!=3 &&
            c->threads[t].sp>first && c->threads[t].sp<=last)goto invalid;
#if TOTK_FASTMEM
        if(boot_fm_owns(data)){   /* section heap: pages stay committed; zeroed when regrown */
            if(!size && !boot_fm_section_free(data))goto invalid;
        }else
#endif
#ifdef TOTK_DRAW_PATH
        /* The heap stays fully committed (D3D12 shares it zero-copy); trimmed pages are
         * inaccessible through the region size and are zeroed again when the heap grows. */
        if(!size && !VirtualFree(data,0,MEM_RELEASE))goto invalid;
#else
        if(!VirtualFree(size?data+(size_t)size:data,size?(SIZE_T)(old_size-size):0,size?MEM_DECOMMIT:MEM_RELEASE))goto invalid;
#endif
        /* Trimming a heap suffix cannot increase the range count. */
        boot_attributes_update(&c->memory_attributes,first,last,0);
        if(size)c->regions[slot].size=size;
        else {
            --c->region_count;
            for(unsigned i=slot;i<c->region_count;++i){c->regions[i]=c->regions[i+1];c->memory_types[i]=c->memory_types[i+1];}
            memset(&c->regions[c->region_count],0,sizeof(Region));c->memory_types[c->region_count]=0;
        }
    }else {
        int created=data==NULL;
#if TOTK_FASTMEM
        if(boot_fm.initialized && boot_fm.mode==1 && (created || boot_fm_owns(data))){
            if(created){
                if(c->region_count==BOOT_REGION_CAPACITY)goto invalid;
#ifdef TOTK_DRAW_PATH
                data=boot_fm_section_alloc(maximum,1);   /* SEC_COMMIT: D3D12 opens the view as a heap */
#else
                data=boot_fm_section_alloc(maximum,0);
#endif
                if(!data)goto invalid;
            }
            if(!boot_fm_commit_zero(data+(size_t)old_size,size-old_size)){if(created)boot_fm_section_free(data);goto invalid;}
            if(created){slot=c->region_count++;c->regions[slot]=(Region){base,size,data,READ|WRITE,3};c->memory_types[slot]=5;}
            else c->regions[slot].size=size;
            boot_tlb_flush();c->x[0]=0;c->x[1]=base;return 1;
        }
#endif
        if(created){
            if(c->region_count==BOOT_REGION_CAPACITY)goto invalid;
#ifdef TOTK_DRAW_PATH
            /* Commit the whole reservation: OpenExistingHeapFromAddress requires it. This costs
             * commit charge, not physical memory, until pages are touched. */
            data=VirtualAlloc(NULL,(SIZE_T)maximum,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);if(!data)goto invalid;
#else
            data=VirtualAlloc(NULL,(SIZE_T)maximum,MEM_RESERVE,PAGE_READWRITE);if(!data)goto invalid;
#endif
        }
#ifdef TOTK_DRAW_PATH
        if(!created)memset(data+(size_t)old_size,0,(size_t)(size-old_size));   /* new heap pages read as zero */
#else
        if(!VirtualAlloc(data+(size_t)old_size,(SIZE_T)(size-old_size),MEM_COMMIT,PAGE_READWRITE)){
            if(created)VirtualFree(data,0,MEM_RELEASE);goto invalid;
        }
#endif
        if(created){slot=c->region_count++;c->regions[slot]=(Region){base,size,data,READ|WRITE,3};c->memory_types[slot]=5;}
        else c->regions[slot].size=size;
    }
    boot_tlb_flush();c->x[0]=0;c->x[1]=base;return 1;
invalid:
    boot_fault(c,BOOT_QUERY_SCOPE,pc,base,size);return 0;
}
enum { BOOT_SESSION_SM=0, BOOT_SESSION_LM=1, BOOT_SESSION_LOGGER=2, BOOT_SESSION_APM=3, BOOT_SESSION_APPLET=4,
       BOOT_SESSION_APPLET_PROXY=5, BOOT_SESSION_APP_FUNCTIONS=6, BOOT_SESSION_LIBRARY_CREATOR=7,
       BOOT_SESSION_COMMON_STATE=8, BOOT_SESSION_SELF_CONTROL=9, BOOT_SESSION_WINDOW_CONTROL=10,
       BOOT_SESSION_AUDIO_CONTROL=11, BOOT_SESSION_DISPLAY_CONTROL=12, BOOT_SESSION_DEBUG_FUNCTIONS=13,
       BOOT_SESSION_AOC=14, BOOT_SESSION_ERROR_CONTEXT=15, BOOT_SESSION_PCTL=16,
       BOOT_SESSION_PCTL_SERVICE=17, BOOT_SESSION_PCTL_READY=18, BOOT_SESSION_CLOSED=19,
       BOOT_SESSION_FSP=20, BOOT_SESSION_TIME=BOOT_SESSION_FSP+20 };
static inline int boot_create_transfer(BootContext*c,uint64_t pc){
    uint64_t base=c->x[1],size=c->x[2];uint32_t permission=(uint32_t)c->x[3];
    if(size<=UINT64_MAX-base && boot_attributes_overlap(&c->memory_attributes,base,base+size)){
        boot_fault(c,BOOT_SVC,pc,base,size);return 0;
    }
    if(size<=UINT64_MAX-base && c->window_native && (boot_gpu_mapping_cpu_overlap(&((BootWindow*)(uintptr_t)c->window_native)->gpu_mappings,base,base+size) || boot_graphic_cpu_overlap(&((BootWindow*)(uintptr_t)c->window_native)->graphic_buffers,base,base+size))){
        boot_fault(c,BOOT_SVC,pc,base,size);return 0;
    }
    unsigned slot=0;while(slot<16 && c->transfers[slot].handle)++slot;
    if(slot==16 || !size || ((base|size)&4095) || base>UINT64_MAX-size ||
       (permission!=0 && permission!=READ && permission!=(READ|WRITE)) || c->next_session_handle>=0x7ffffffe)goto invalid;
    for(unsigned i=0;i<16;++i){BootTransfer*t=&c->transfers[i];if(t->handle && base<t->base+t->size && t->base<base+size)goto invalid;}
    for(unsigned i=0;i<c->alias_count;++i){BootAlias*a=&c->aliases[i];if(base<a->source+a->size && a->source<base+size)goto invalid;}
    /* Only the owned heap is eligible in this bounded process model. */
    for(unsigned i=0;i<c->region_count;++i){Region*r=&c->regions[i];
        if(r->reserved!=3 || c->memory_types[i]!=5 || r->permissions!=(READ|WRITE) || base<r->base || size>r->size || base-r->base>r->size-size)continue;
        if(c->next_session_handle<0x10000)c->next_session_handle=0x10000;
        uint32_t handle=++c->next_session_handle;
        c->transfers[slot]=(BootTransfer){base,size,handle,permission,0,0};c->exclusive_valid=0;boot_tlb_flush();
        c->x[0]=0;c->x[1]=handle;return 1;
    }
invalid:boot_fault(c,BOOT_SVC,pc,base,size);return 0;
}
static inline uint32_t boot_new_session(BootContext*c,uint64_t pc,unsigned type,uint64_t pid){
    unsigned slot=0;while(slot<BOOT_SESSION_CAPACITY && c->session_handles[slot])++slot;
    if(slot==BOOT_SESSION_CAPACITY || c->next_session_handle>=0x7ffffffe){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
    if(c->next_session_handle<0x10000)c->next_session_handle=0x10000;
    uint32_t handle=++c->next_session_handle;
    c->session_handles[slot]=handle;c->session_process_ids[slot]=pid;c->session_types[slot]=type;
    c->session_domains[slot]=0;
    return handle;
}
/* Connecting creates a real tracked endpoint;
 * it does not imply any IPC request has been implemented or acknowledged. */
static inline void boot_release_nvdrv(BootContext*c,unsigned slot){
    unsigned index=c->nvdrv_transfer_slots[slot];if(!index)return;
    BootTransfer*t=&c->transfers[index-1];
    if(t->driver_references)--t->driver_references;
    if(t->owner_closed && !t->driver_references)memset(t,0,sizeof(*t));
    c->nvdrv_transfer_slots[slot]=0;c->exclusive_valid=0;boot_tlb_flush();
}
static inline int boot_session_service(BootContext*c,uint64_t pc,unsigned svc){
    if(svc==0x13 || svc==0x14){
        uint64_t base=c->x[1],size=c->x[2];
        unsigned time_entry=0;
        while(time_entry<16 && (!c->event_handles[time_entry] ||
              c->event_handles[time_entry]!=c->x[0] || c->event_sources[time_entry]!=700))++time_entry;
        if(time_entry<16){
            if(!base || (base&4095) || size!=0x1000 || base>=c->address_space_end ||
               size>c->address_space_end-base)goto shared_invalid;
            if(svc==0x14){
                for(unsigned i=0;i<c->region_count;++i){Region*r=&c->regions[i];
                    if(r->base==base && r->size==size && r->reserved==2 &&
                       c->memory_types[i]==6 && r->data){
                        if(!UnmapViewOfFile(r->data))goto shared_invalid;
                        --c->region_count;
                        for(unsigned j=i;j<c->region_count;++j){
                            c->regions[j]=c->regions[j+1];c->memory_types[j]=c->memory_types[j+1];
                        }
                        memset(&c->regions[c->region_count],0,sizeof(Region));c->memory_types[c->region_count]=0;
                        c->exclusive_valid=0;boot_tlb_flush();c->x[0]=0;return 1;
                    }
                }
                goto shared_invalid;
            }
            if(c->x[3]!=READ || c->region_count==BOOT_REGION_CAPACITY)goto shared_invalid;
            if(c->layout_enabled){
                uint64_t start=c->layout_info[12],extent=c->layout_info[13];
                if(base<start || base-start>=extent || size>extent-(base-start))goto shared_invalid;
                for(unsigned pair=2;pair<=4;pair+=2){
                    uint64_t reserved=c->layout_info[pair],length=c->layout_info[pair+1];
                    if(base<reserved+length && reserved<base+size)goto shared_invalid;
                }
            }
            for(unsigned i=0;i<c->region_count;++i)
                if(base<c->regions[i].base+c->regions[i].size && c->regions[i].base<base+size)goto shared_invalid;
            uint8_t*view=(uint8_t*)MapViewOfFile((HANDLE)(uintptr_t)c->event_native_handles[time_entry],FILE_MAP_READ,0,0,0x1000);
            if(!view)goto shared_invalid;
            unsigned index=c->region_count++;
            c->regions[index]=(Region){base,size,view,READ,2};c->memory_types[index]=6;boot_tlb_flush();
            c->x[0]=0;return 1;
        }
        BootWindow*w=(BootWindow*)(uintptr_t)c->window_native;
        unsigned entry=0;
        while(entry<16 && (!c->event_handles[entry] || c->event_handles[entry]!=c->x[0] || c->event_sources[entry]!=400))++entry;
        if(entry==16 || !w || !w->hid_memory.data || w->hid_memory.owner!=c->applet_resource_user_id ||
           !base || (base&4095) || size!=BOOT_HID_MEMORY_SIZE || base>=c->address_space_end || size>c->address_space_end-base)goto shared_invalid;
        if(svc==0x14){
            for(unsigned i=0;i<c->region_count;++i){Region*r=&c->regions[i];
                if(r->base==base && r->size==size && r->data==w->hid_memory.data && r->reserved==2 && c->memory_types[i]==6){
                    --c->region_count;
                    for(unsigned j=i;j<c->region_count;++j){c->regions[j]=c->regions[j+1];c->memory_types[j]=c->memory_types[j+1];}
                    memset(&c->regions[c->region_count],0,sizeof(Region));c->memory_types[c->region_count]=0;
                    c->exclusive_valid=0;boot_tlb_flush();c->x[0]=0;return 1;
                }
            }
            goto shared_invalid;
        }
        if(c->x[3]!=READ || c->region_count==BOOT_REGION_CAPACITY)goto shared_invalid;
        if(c->layout_enabled){
            uint64_t start=c->layout_info[12],extent=c->layout_info[13];
            if(base<start || base-start>=extent || size>extent-(base-start))goto shared_invalid;
            for(unsigned pair=2;pair<=4;pair+=2){
                uint64_t reserved=c->layout_info[pair],length=c->layout_info[pair+1];
                if(base<reserved+length && reserved<base+size)goto shared_invalid;
            }
        }
        for(unsigned i=0;i<c->region_count;++i)if(base<c->regions[i].base+c->regions[i].size && c->regions[i].base<base+size)goto shared_invalid;
        unsigned index=c->region_count++;
        c->regions[index]=(Region){base,size,w->hid_memory.data,READ,2};c->memory_types[index]=6;boot_tlb_flush();
        c->x[0]=0;return 1;
shared_invalid:
        boot_fault(c,BOOT_SVC,pc,base,size);return 0;
    }
    if(svc==0x1f){
        uint8_t*name=boot_access(c,c->x[1],4,READ,pc);if(!name)return 0;
        if(memcmp(name,"sm:\0",4)){boot_fault(c,BOOT_SVC,pc,c->x[1],4);return 0;}
        uint32_t handle=boot_new_session(c,pc,BOOT_SESSION_SM,0);if(!handle)return 0;
        c->x[1]=handle;c->x[0]=0;++c->port_connect_count;return 1;
    }
    for(unsigned i=0;i<16;++i)if(c->transfers[i].handle && !c->transfers[i].owner_closed && c->transfers[i].handle==(uint32_t)c->x[0]){
        c->transfers[i].owner_closed=1;
        if(!c->transfers[i].driver_references)memset(&c->transfers[i],0,sizeof(c->transfers[i]));
        c->exclusive_valid=0;boot_tlb_flush();c->x[0]=0;++c->handle_close_count;return 1;
    }
    for(unsigned i=0;i<16;++i)if(c->event_handles[i] && c->event_handles[i]==c->x[0]){
        if(!CloseHandle((HANDLE)(uintptr_t)c->event_native_handles[i])){boot_fault(c,BOOT_SVC,pc,c->x[0],0);return 0;}
        c->event_handles[i]=c->event_sources[i]=0;c->event_native_handles[i]=0;
        c->x[0]=0;++c->handle_close_count;return 1;
    }
    for(unsigned i=0;i<BOOT_SESSION_CAPACITY;++i)if(c->session_handles[i] && c->session_handles[i]==c->x[0]){
        boot_release_nvdrv(c,i);
        if(c->session_types[i]==BOOT_SESSION_FSP+6)boot_window_display_release((BootWindow*)(uintptr_t)c->window_native,i);
        unsigned domain=c->session_domains[i];
        if(domain && --c->domain_refs[domain-1]==0)memset(c->domain_types[domain-1],0,sizeof(c->domain_types[0]));
        c->session_domains[i]=0;
        c->session_handles[i]=0;c->session_process_ids[i]=0;c->session_types[i]=0;c->x[0]=0;++c->handle_close_count;return 1;
    }
    /* Thread handle (nn::os::DestroyThread after join): closing a handle never affects the
     * thread itself. An exited thread's handle is released; a live thread keeps running. */
    for(unsigned i=1;i<c->thread_count && i<BOOT_THREAD_CAPACITY;++i)if(c->threads[i].handle && c->threads[i].handle==c->x[0]){
        if(c->threads[i].state==3)c->threads[i].handle=0;
        c->x[0]=0;++c->handle_close_count;return 1;
    }
    boot_fault(c,BOOT_SVC,pc,c->x[0],0);return 0;
}
static inline int boot_cmif_control(BootContext*c,uint64_t pc,unsigned slot){
    uint8_t*data=boot_access(c,c->tpidrro,44,READ|WRITE,pc);if(!data)return 0;
    uint32_t type=(uint32_t)little_read(data,4),words=(uint32_t)little_read(data+4,4);
    uint32_t version=(uint32_t)little_read(data+20,4),command=(uint32_t)little_read(data+24,4);
    uint32_t token=(uint32_t)little_read(data+28,4),value=0;
    if(c->session_types[slot]==BOOT_SESSION_FSP+4 && (command==0 || ((command==2 || command==4) && !c->nvdrv_transfer_slots[slot]))){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
    if((type!=5 && type!=7) || words!=(command==4?9u:8u) || little_read(data+16,4)!=UINT64_C(0x49434653) ||
       version!=(type==7?1u:0u) || (command!=0 && command!=2 && command!=3 && command!=4)){
        boot_fault(c,BOOT_SVC,pc,c->tpidrro,44);return 0;
    }
    if(command==4){
        data=boot_access(c,c->tpidrro,52,READ|WRITE,pc);if(!data)return 0;
        if(c->session_types[slot]!=BOOT_SESSION_FSP+4 || little_read(data+32,4)!=1){boot_fault(c,BOOT_SVC,pc,c->tpidrro,52);return 0;}
    }
    if(command==2 || command==4){
        if(c->session_types[slot]==BOOT_SESSION_FSP+6 && !c->window_native){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
        /* Clone the endpoint; domain objects remain shared until the last
         * referring session closes. Validate the entire reply before allocation. */
        data=boot_access(c,c->tpidrro,48,READ|WRITE,pc);if(!data)return 0;
        uint32_t handle=boot_new_session(c,pc,c->session_types[slot],c->session_process_ids[slot]);
        if(!handle)return 0;
        unsigned clone=0;while(c->session_handles[clone]!=handle)++clone;
        if(c->nvdrv_transfer_slots[slot]){
            c->nvdrv_transfer_slots[clone]=c->nvdrv_transfer_slots[slot];
            ++c->transfers[c->nvdrv_transfer_slots[slot]-1].driver_references;
        }
        if(c->session_types[slot]==BOOT_SESSION_FSP+6)boot_window_display_clone((BootWindow*)(uintptr_t)c->window_native,slot,clone);
        unsigned domain=c->session_domains[slot];c->session_domains[clone]=domain;
        if(domain)++c->domain_refs[domain-1];
        memset(data,0,48);little_write(data+4,UINT32_C(0x80000008),4);
        little_write(data+8,0x20,4);little_write(data+12,handle,4);
        little_write(data+16,UINT32_C(0x4f434653),4);little_write(data+20,version,4);little_write(data+28,token,4);
        ++c->cmif_control_count;c->x[0]=0;return 1;
    }
    if(command==0){
        if(c->session_types[slot]>=BOOT_SESSION_FSP+5 && c->session_types[slot]!=BOOT_SESSION_FSP+8 && c->session_types[slot]!=BOOT_SESSION_FSP+11 && c->session_types[slot]!=BOOT_SESSION_FSP+17 && c->session_types[slot]!=120 /* prepo:u */ && c->session_types[slot]!=122 /* nfp:user */ && c->session_types[slot]!=BOOT_SESSION_TIME){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
        if(c->session_domains[slot]){boot_fault(c,BOOT_SVC,pc,c->tpidrro,44);return 0;}
        unsigned domain=0;while(domain<16 && c->domain_refs[domain])++domain;
        if(domain==16){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
        c->domain_refs[domain]=1;c->domain_types[domain][0]=c->session_types[slot]+1;
        c->session_domains[slot]=domain+1;value=1;
    }
    if(command==3){
        if(c->session_types[slot]==BOOT_SESSION_FSP+11)value=BOOT_HID_POINTER_CAPACITY;
        else if(c->session_types[slot]==BOOT_SESSION_FSP)value=0x1000;
        /* nfp:user: a zero size made the SDK abort with hipc PointerBufferTooSmall
         * (0x11a0b) after IUser::AttachAvailabilityChangeEvent. */
        else if(c->session_types[slot]==122)value=0x8000; /* suyu reports 0x8000 for every session */
        /* prepo:u reports pass pointer (X) buffers; zero aborted the same way. */
        else if(c->session_types[slot]==120)value=0x8000;
        /* time:u: nn::time::ClockSnapshot::CreateWithAdjustableSystemClock receives its
         * snapshot through a pointer buffer; zero aborted with hipc PointerBufferTooSmall. */
        else if(c->session_types[slot]==BOOT_SESSION_TIME)value=0x8000;
        FILE*log=fopen("pointer-queries.txt","a");
        if(log){fprintf(log,"QueryPointerBufferSize session_type=%u handle=0x%x value=0x%x step=%llu""\n",c->session_types[slot],
                        (unsigned)c->session_handles[slot],(unsigned)value,(unsigned long long)c->steps);fclose(log);}
    }
    /* fsp-srv needs enough pointer-buffer space for a full 0x301-byte path.
     * HID has its separate bounded storage. Other endpoints retain zero.
     * Domain conversion creates root object ID 1. */
    memset(data,0,44);little_write(data+4,9,4);little_write(data+16,UINT64_C(0x4f434653),4);
    little_write(data+20,version,4);little_write(data+28,token,4);little_write(data+32,value,4);
    ++c->cmif_control_count;c->x[0]=0;return 1;
}
static inline int boot_cmif_domain(BootContext*c,uint64_t pc,unsigned slot){
    enum { FSP_READY=BOOT_SESSION_FSP+1, ROM_STORAGE=BOOT_SESSION_FSP+2,
           SAVE_FS=BOOT_SESSION_FSP+19 };
    static unsigned nfp_initialized; /* nfp IUser state: 0 NonInitialized, 1 Initialized */
    /* Generic save-data IFile objects (SAVE_FS+9): host path per domain object. Mirrored to
     * startup\save-data\open-files.txt so an open file survives a resume DLL swap. */
    static struct { uint32_t domain,object; wchar_t path[300]; } save_files[32];
    #define SAVE_FILE_PATH(d,o) ({ const wchar_t*_p=NULL;uint32_t _d=(d),_o=(o); \
        for(unsigned _i=0;_i<32 && !_p;++_i)if(save_files[_i].domain==_d && save_files[_i].object==_o && save_files[_i].path[0])_p=save_files[_i].path; \
        if(!_p){FILE*_f=_wfopen(L"startup\\save-data\\open-files.txt",L"r");unsigned _fd,_fo;wchar_t _fp[300],_last[300];int _hit=0; \
            if(_f){while(fwscanf(_f,L"%u %u %299[^\n]",&_fd,&_fo,_fp)==3)if(_fd==_d && _fo==_o){wcscpy(_last,_fp);_hit=1;} \
                fclose(_f);} \
            if(_hit){unsigned _e=0;while(_e<31 && save_files[_e].path[0])++_e; /* the latest open of this object wins */ \
                save_files[_e].domain=_d;save_files[_e].object=_o;wcscpy(save_files[_e].path,_last);_p=save_files[_e].path;}} _p; })
    uint8_t*data=boot_access(c,c->tpidrro,8,READ,pc);if(!data)return 0;
    uint32_t type=(uint32_t)little_read(data,4),header=(uint32_t)little_read(data+4,4);
    unsigned domain=c->session_domains[slot];
    if(domain && c->session_types[slot]==BOOT_SESSION_FSP+17 && ((type&0xffff)==4 || (type&0xffff)==6) &&
       c->session_process_ids[slot]==1){
        /* hwopus: IHardwareOpusDecoderManager (domain root, object type FSP+18) and the
         * IHardwareOpusDecoder objects it opens (command lists and result codes per
         * switchbrew / suyu hwopus). Decoding is real libopus (vendored 1.5.2, built natively by
         * third_party/build_opus.py and linked as a whole archive; referenced weakly so
         * runtime builds without it still link and stop here instead). Decoder state lives in this host
         * table keyed by domain object. The work-buffer transfer memory is retained (driver
         * reference) while the decoder lives, and its first bytes describe the decoder so it can
         * be re-created (fresh state) after a resume DLL swap empties this table. */
        enum { HWOPUS_SINGLE=150, HWOPUS_MULTI=152, HWOPUS_MAGIC=0x504f4b54 /* "TKOP" */ };
        extern __attribute__((weak)) int opus_decoder_get_size(int);
        extern __attribute__((weak)) void*opus_decoder_create(int32_t,int,int*);
        extern __attribute__((weak)) int opus_decode(void*,const unsigned char*,int32_t,int16_t*,int,int);
        extern __attribute__((weak)) int opus_decoder_ctl(void*,int,...);
        extern __attribute__((weak)) void opus_decoder_destroy(void*);
        extern __attribute__((weak)) int32_t opus_multistream_decoder_get_size(int,int);
        extern __attribute__((weak)) void*opus_multistream_decoder_create(int32_t,int,int,int,const unsigned char*,int*);
        extern __attribute__((weak)) int opus_multistream_decode(void*,const unsigned char*,int32_t,int16_t*,int,int);
        extern __attribute__((weak)) int opus_multistream_decoder_ctl(void*,int,...);
        extern __attribute__((weak)) void opus_multistream_decoder_destroy(void*);
        static struct { uint32_t domain,object,kind,sample_rate,channels,large,streams,stereo,transfer,in_size,frames;
                        void*state;int16_t*pcm; } hwo[16];
        /* Service-side view of guest memory (the ADSP maps the work buffer regardless of the
         * owner's transfer-memory permission). */
        #define HWOPUS_HOST(address,length) ({ uint8_t*_h=NULL;uint64_t _a=(address),_l=(length); \
            for(unsigned _i=0;_i<c->region_count && !_h;++_i){Region*_r=&c->regions[_i]; \
                if(_r->data && _a>=_r->base && _a-_r->base<=_r->size && _l<=_r->size-(_a-_r->base))_h=_r->data+(_a-_r->base);} _h; })
        #define HWOPUS_RELEASE(e) do{ unsigned _e=(e); \
            if(hwo[_e].state){if(hwo[_e].kind==HWOPUS_MULTI)opus_multistream_decoder_destroy(hwo[_e].state);else opus_decoder_destroy(hwo[_e].state);} \
            free(hwo[_e].pcm); \
            if(hwo[_e].transfer && hwo[_e].transfer<=16){BootTransfer*_t=&c->transfers[hwo[_e].transfer-1]; \
                if(_t->handle){uint8_t*_g=HWOPUS_HOST(_t->base,4);if(_g && little_read(_g,4)==HWOPUS_MAGIC)little_write(_g,0,4);} \
                if(_t->driver_references)--_t->driver_references; \
                if(_t->owner_closed && !_t->driver_references)memset(_t,0,sizeof(*_t)); boot_tlb_flush(); \
                c->exclusive_valid=0;} \
            memset(&hwo[_e],0,sizeof(hwo[_e])); }while(0)
        /* HwOpus (module 111) results; libopus error codes map as in the system decoder. */
        #define HWOPUS_RESULT(desc) ((uint32_t)111u|((uint32_t)(desc)<<9))
        #define HWOPUS_LIBOPUS(err) ((err)==-1?HWOPUS_RESULT(2):(err)==-2?HWOPUS_RESULT(3):(err)==-4?HWOPUS_RESULT(17): \
                                     (err)==-5?HWOPUS_RESULT(5):(err)==-6?HWOPUS_RESULT(6):(err)==-7?HWOPUS_RESULT(7):HWOPUS_RESULT(4))
        /* Drop decoders whose domain object was closed or whose session's domain was released. */
        for(unsigned e=0;e<16;++e)if(hwo[e].kind && (hwo[e].domain-1>=16u || hwo[e].object-1>=16u || !c->domain_refs[hwo[e].domain-1] ||
                                                     c->domain_types[hwo[e].domain-1][hwo[e].object-1]!=hwo[e].kind+1))HWOPUS_RELEASE(e);
        uint8_t*m=boot_access(c,c->tpidrro,0x100,READ|WRITE,pc);if(!m)return 0;
        unsigned xs=(type>>16)&15,as=(type>>20)&15,bs=(type>>24)&15,ws=(type>>28)&15,words=header&1023,cmode=(header>>10)&15;
        unsigned special=header>>31,pid=0,copies=0,moves=0,off=8;
        if(special){uint32_t sh=(uint32_t)little_read(m+8,4);pid=sh&1;copies=(sh>>1)&15;moves=(sh>>5)&15;off=12+pid*8+(copies+moves)*4;}
        unsigned xoff=off;off+=xs*8;unsigned aoff=off;off+=(as+bs+ws)*12;
        unsigned raw=(off+15)&~15u,end=off+words*4;
        unsigned dcommand=end<=0x100 && raw+8<=end?m[raw]:0;
        uint32_t object=dcommand?(uint32_t)little_read(m+raw+4,4):0;
        unsigned parent=object && object<=16?c->domain_types[domain-1][object-1]:0;
        if(dcommand==2 && !special && words==8 && little_read(m+raw,4)==2 && little_read(m+raw+8,4)==0 &&
           (parent==HWOPUS_SINGLE+1 || parent==HWOPUS_MULTI+1)){
            /* Domain Close of a decoder: release its libopus state and transfer reference. */
            for(unsigned e=0;e<16;++e)if(hwo[e].kind && hwo[e].domain==domain && hwo[e].object==object)HWOPUS_RELEASE(e);
            c->domain_types[domain-1][object-1]=0;
            memset(m,0,8);c->x[0]=0;return 1;
        }
        int ours=dcommand==1 && raw+32<=end && (parent==BOOT_SESSION_FSP+18 || parent==HWOPUS_SINGLE+1 || parent==HWOPUS_MULTI+1);
        if(ours){
            /* libopus is linked (whole archive) into the runtime DLL; builds without it stop here. */
            if(!opus_decoder_get_size || !opus_decoder_create || !opus_decode || !opus_decoder_ctl || !opus_decoder_destroy ||
               !opus_multistream_decoder_get_size || !opus_multistream_decoder_create || !opus_multistream_decode ||
               !opus_multistream_decoder_ctl || !opus_multistream_decoder_destroy){
                boot_fault(c,BOOT_SVC,pc,c->tpidrro,0x100);return 0;
            }
            uint32_t version=(type&0xffff)==6?1:0,command=(uint32_t)little_read(m+raw+24,4),token=(uint32_t)little_read(m+raw+28,4);
            unsigned data_size=(unsigned)little_read(m+raw+2,2);
            if(m[raw+1] || cmode || ws || little_read(m+raw+8,4) || little_read(m+raw+16,4)!=UINT32_C(0x49434653) ||
               little_read(m+raw+20,4)!=version || data_size<16 || raw+16+data_size>end || moves || pid || command>9){
                boot_fault(c,BOOT_SVC,pc,c->tpidrro,0x100);return 0;
            }
            unsigned payload=data_size-16;const uint8_t*in=m+raw+32;
            uint64_t x_address=0,x_size=0,a_address=0,a_size=0,b_address=0,b_size=0;
            if(xs){uint32_t w0=(uint32_t)little_read(m+xoff,4);x_size=w0>>16;
                x_address=little_read(m+xoff+4,4)|((uint64_t)((w0>>12)&15)<<32)|((uint64_t)((w0>>6)&7)<<36);}
            for(unsigned k=0;k<as+bs;++k){uint8_t*d=m+aoff+k*12;uint32_t w2=(uint32_t)little_read(d+8,4);
                uint64_t size=little_read(d,4)|((uint64_t)((w2>>24)&15)<<32);
                uint64_t address=little_read(d+4,4)|((uint64_t)(w2>>28)<<32)|((uint64_t)((w2>>2)&7)<<36);
                if(k==0 && as){a_address=address;a_size=size;}else if(k==as){b_address=address;b_size=size;}}
            uint32_t result=0;uint8_t reply[24];unsigned reply_bytes=0;int new_object=-1;
            if(parent==BOOT_SESSION_FSP+18){
                /* Manager. Commands 0/2/4/6 open a decoder (one copied transfer-memory handle),
                 * 1/3/5/7/8/9 report the work-buffer size. Multistream parameters arrive in a
                 * pointer (X) buffer. */
                int open=command==0 || command==2 || command==4 || command==6;
                int multi=command==2 || command==3 || command==6 || command==7 || command==9;
                int ex=command>=4;
                unsigned want_payload=multi?(open?4:0):(ex?16:8)+(open?4:0);
                unsigned params_size=multi?(ex?0x118:0x110):0;
                if(copies!=(open?1u:0u) || as || bs || xs!=(multi?1u:0u) || payload<want_payload ||
                   (multi && x_size<params_size)){
                    boot_fault(c,BOOT_SVC,pc,c->tpidrro,0x100);return 0;
                }
                uint8_t params[0x118];memset(params,0,sizeof params);
                if(multi){const uint8_t*p=boot_access(c,x_address,params_size,READ,pc);if(!p)return 0;memcpy(params,p,params_size);
                    m=boot_access(c,c->tpidrro,0x100,READ|WRITE,pc);if(!m)return 0;in=m+raw+32;}
                else memcpy(params,in,ex?16:8);
                uint32_t sample_rate=(uint32_t)little_read(params,4),channels=(uint32_t)little_read(params+4,4);
                uint32_t streams=multi?(uint32_t)little_read(params+8,4):0,stereo=multi?(uint32_t)little_read(params+12,4):0;
                uint32_t large=ex?(multi?params[16]:params[8])!=0:0;
                const uint8_t*mapping=multi?params+(ex?24:16):NULL;
                uint32_t tmem_size=open?(uint32_t)little_read(in+(multi?0:ex?16:8),4):0;
                uint32_t frame=large?5760:1920,work=0;
                int rate_ok=sample_rate==8000 || sample_rate==12000 || sample_rate==16000 || sample_rate==24000 || sample_rate==48000;
                if(multi?(!channels || channels>255):(channels!=1 && channels!=2))result=HWOPUS_RESULT(1002);
                else if(!rate_ok)result=HWOPUS_RESULT(1001);
                else if(multi && (!streams || stereo>streams || streams+stereo>channels))result=HWOPUS_RESULT(1001);
                else {
                    uint32_t out_frames=frame*channels/(48000/sample_rate);
                    if(multi)work=32+(uint32_t)opus_multistream_decoder_get_size((int)streams,(int)stereo)+
                                  ((1500*streams+63)&~63u)+((out_frames+63)&~63u);
                    else work=32+(uint32_t)opus_decoder_get_size((int)channels)+((out_frames+63)&~63u)+0x600;
                }
                if(!result && !open){little_write(reply,work,4);reply_bytes=4;}
                if(!result && open){
                    uint32_t transfer_handle=(uint32_t)little_read(m+12,4);
                    unsigned index=0;while(index<16 && (!c->transfers[index].handle || c->transfers[index].owner_closed ||
                                                        c->transfers[index].handle!=transfer_handle))++index;
                    int busy=0;for(unsigned e=0;e<16;++e)if(hwo[e].kind && hwo[e].transfer==index+1)busy=1;
                    unsigned child=0;while(child<16 && c->domain_types[domain-1][child])++child;
                    unsigned e=0;while(e<16 && hwo[e].kind)++e;
                    uint8_t*guest=index<16?HWOPUS_HOST(c->transfers[index].base,c->transfers[index].size):NULL;
                    /* The work buffer must hold the size reported above; it is never smaller
                     * than the descriptor written at its start. */
                    if(index==16 || busy || !guest || tmem_size<work || tmem_size>c->transfers[index].size || child==16 || e==16){
                        boot_fault(c,BOOT_SVC,pc,c->tpidrro,0x100);return 0;
                    }
                    int error=0;void*state=multi?opus_multistream_decoder_create((int32_t)sample_rate,(int)channels,(int)streams,(int)stereo,mapping,&error):
                                                 opus_decoder_create((int32_t)sample_rate,(int)channels,&error);
                    uint32_t frames=frame/(48000/sample_rate);
                    int16_t*pcm=state?(int16_t*)malloc((size_t)frames*channels*sizeof(int16_t)):NULL;
                    if(!state || error){
                        if(state){if(multi)opus_multistream_decoder_destroy(state);else opus_decoder_destroy(state);}
                        result=HWOPUS_LIBOPUS(error?error:-7);
                    }else if(!pcm){
                        if(multi)opus_multistream_decoder_destroy(state);else opus_decoder_destroy(state);
                        boot_fault(c,BOOT_SVC,pc,0,0);return 0;
                    }else {
                        hwo[e].domain=domain;hwo[e].object=child+1;hwo[e].kind=multi?HWOPUS_MULTI:HWOPUS_SINGLE;
                        hwo[e].sample_rate=sample_rate;hwo[e].channels=channels;hwo[e].large=large;
                        hwo[e].streams=streams;hwo[e].stereo=stereo;hwo[e].transfer=index+1;
                        hwo[e].in_size=multi?((1500*streams+63)&~63u):0x600;hwo[e].frames=frames;
                        hwo[e].state=state;hwo[e].pcm=pcm;
                        ++c->transfers[index].driver_references;
                        memset(guest,0,36+256);little_write(guest,HWOPUS_MAGIC,4);little_write(guest+4,domain,4);
                        little_write(guest+8,child+1,4);little_write(guest+12,hwo[e].kind,4);little_write(guest+16,sample_rate,4);
                        little_write(guest+20,channels,4);little_write(guest+24,large,4);little_write(guest+28,streams,4);
                        little_write(guest+32,stereo,4);if(multi)memcpy(guest+36,mapping,channels);
                        c->exclusive_valid=0;
                        c->domain_types[domain-1][child]=hwo[e].kind+1;new_object=(int)child+1;
                        FILE*log=fopen("hwopus-requests.txt","a");
                        if(log){fprintf(log,"open command=%u object=%u rate=%u channels=%u streams=%u stereo=%u large=%u work=%u tmem=0x%x/0x%llx step=%llu",
                                        command,child+1,sample_rate,channels,streams,stereo,large,work,tmem_size,
                                        (unsigned long long)c->transfers[index].size,(unsigned long long)c->steps);
                                fputc(10,log);fclose(log);}
                    }
                }
            }else {
                /* IHardwareOpusDecoder. Input A buffer: OpusPacketHeader {u32 size BE,
                 * u32 final_range BE} + packet; output B buffer: interleaved s16 PCM. */
                int multi=parent==HWOPUS_MULTI+1;
                unsigned e=0;while(e<16 && !(hwo[e].kind && hwo[e].domain==domain && hwo[e].object==object))++e;
                if(e==16){
                    /* Re-create after a resume: find the retained work buffer describing this object. */
                    for(unsigned i=0;i<16 && e==16;++i){BootTransfer*t=&c->transfers[i];
                        if(!t->handle || !t->driver_references || t->size<36+256)continue;
                        const uint8_t*g=HWOPUS_HOST(t->base,36+256);
                        if(!g || little_read(g,4)!=HWOPUS_MAGIC || little_read(g+4,4)!=domain || little_read(g+8,4)!=object ||
                           little_read(g+12,4)+1!=parent)continue;
                        unsigned slot_e=0;while(slot_e<16 && hwo[slot_e].kind)++slot_e;
                        if(slot_e==16)break;
                        uint32_t rate=(uint32_t)little_read(g+16,4),channels=(uint32_t)little_read(g+20,4),large=(uint32_t)little_read(g+24,4);
                        uint32_t streams=(uint32_t)little_read(g+28,4),stereo=(uint32_t)little_read(g+32,4);
                        if(!rate || rate>48000 || 48000%rate || !channels || channels>255)break;
                        int error=0;void*state=multi?opus_multistream_decoder_create((int32_t)rate,(int)channels,(int)streams,(int)stereo,g+36,&error):
                                                     opus_decoder_create((int32_t)rate,(int)channels,&error);
                        uint32_t frames=(large?5760:1920)/(48000/rate);
                        int16_t*pcm=state && !error?(int16_t*)malloc((size_t)frames*channels*sizeof(int16_t)):NULL;
                        if(!pcm){if(state){if(multi)opus_multistream_decoder_destroy(state);else opus_decoder_destroy(state);}break;}
                        hwo[slot_e].domain=domain;hwo[slot_e].object=object;hwo[slot_e].kind=multi?HWOPUS_MULTI:HWOPUS_SINGLE;
                        hwo[slot_e].sample_rate=rate;hwo[slot_e].channels=channels;hwo[slot_e].large=large;
                        hwo[slot_e].streams=streams;hwo[slot_e].stereo=stereo;hwo[slot_e].transfer=i+1;
                        hwo[slot_e].in_size=multi?((1500*streams+63)&~63u):0x600;hwo[slot_e].frames=frames;
                        hwo[slot_e].state=state;hwo[slot_e].pcm=pcm;e=slot_e;
                    }
                    if(e==16){boot_fault(c,BOOT_SVC,pc,c->tpidrro,0x100);return 0;}
                }
                int set_context=command==1 || command==3;
                int command_multi=command==2 || command==3 || command==5 || command==7 || command==9;
                int perf=command>=4,reset_arg=command>=6;
                if(copies || xs || as!=1 || bs!=(set_context?0u:1u) || (!set_context && command_multi!=multi) ||
                   payload<(reset_arg?1u:0u)){
                    boot_fault(c,BOOT_SVC,pc,c->tpidrro,0x100);return 0;
                }
                if(set_context){
                    /* The decoder context is re-established by the service; the buffer is not
                     * interpreted (as the system service maps the work buffer here). */
                    if(a_size && !boot_access(c,a_address,(unsigned)(a_size<0x1000?a_size:0x1000),READ,pc))return 0;
                }else {
                    int reset=reset_arg && in[0];
                    if(a_size<=8)result=HWOPUS_RESULT(8);
                    else {
                        const uint8_t*h=boot_access(c,a_address,8,READ,pc);if(!h)return 0;
                        uint32_t size=((uint32_t)h[0]<<24)|((uint32_t)h[1]<<16)|((uint32_t)h[2]<<8)|h[3];
                        uint32_t final_range=((uint32_t)h[4]<<24)|((uint32_t)h[5]<<16)|((uint32_t)h[6]<<8)|h[7];
                        if(size>hwo[e].in_size || (uint64_t)size+8>a_size)result=HWOPUS_RESULT(3);
                        else {
                            const uint8_t*packet=size?boot_access(c,a_address+8,size,READ,pc):NULL;
                            if(size && !packet)return 0;
                            uint64_t limit=b_size/(2u*hwo[e].channels);
                            int frames=(int)(limit<hwo[e].frames?limit:hwo[e].frames);
                            LARGE_INTEGER t0,t1,frequency;QueryPerformanceCounter(&t0);
                            int error=0;
                            if(reset)error=multi?opus_multistream_decoder_ctl(hwo[e].state,4028 /* OPUS_RESET_STATE */):
                                                 opus_decoder_ctl(hwo[e].state,4028);
                            int samples=error?error:multi?opus_multistream_decode(hwo[e].state,packet,(int32_t)size,hwo[e].pcm,frames,0):
                                                           opus_decode(hwo[e].state,packet,(int32_t)size,hwo[e].pcm,frames,0);
                            uint32_t range=0;
                            if(samples>=0){error=multi?opus_multistream_decoder_ctl(hwo[e].state,4031 /* OPUS_GET_FINAL_RANGE */,&range):
                                                       opus_decoder_ctl(hwo[e].state,4031,&range);
                                if(!error && final_range && range!=final_range)error=-4;}
                            QueryPerformanceCounter(&t1);QueryPerformanceFrequency(&frequency);
                            if(samples<0)result=HWOPUS_LIBOPUS(samples);
                            else if(error)result=HWOPUS_LIBOPUS(error);
                            else {
                                unsigned bytes=(unsigned)samples*hwo[e].channels*2;
                                if(bytes){uint8_t*out=boot_access(c,b_address,bytes,WRITE,pc);if(!out)return 0;
                                    memcpy(out,hwo[e].pcm,bytes);c->exclusive_valid=0;}
                                uint64_t micros=frequency.QuadPart>0?(uint64_t)(t1.QuadPart-t0.QuadPart)*UINT64_C(1000000)/(uint64_t)frequency.QuadPart:0;
                                little_write(reply,size+8,4);little_write(reply+4,(uint32_t)samples,4);
                                if(perf){little_write(reply+8,micros,8);reply_bytes=16;}else reply_bytes=8;
                            }
                        }
                    }
                }
                m=boot_access(c,c->tpidrro,0x100,READ|WRITE,pc);if(!m)return 0;
            }
            if(result){reply_bytes=0;new_object=-1;}
            memset(m,0,48+reply_bytes+4);
            little_write(m+4,12+(reply_bytes+(new_object>0?4:0))/4,4);
            if(new_object>0)little_write(m+16,1,4);
            little_write(m+32,UINT32_C(0x4f434653),4);little_write(m+36,version,4);
            little_write(m+40,result,4);little_write(m+44,token,4);
            memcpy(m+48,reply,reply_bytes);
            if(new_object>0)little_write(m+48+reply_bytes,(uint32_t)new_object,4);
            c->x[0]=0;return 1;
        }
        #undef HWOPUS_HOST
        #undef HWOPUS_RELEASE
        #undef HWOPUS_RESULT
        #undef HWOPUS_LIBOPUS
    }
    if(domain && c->session_types[slot]==BOOT_SESSION_APPLET && ((type&0xffff)==4 || (type&0xffff)==6) &&
       !(header>>31) && c->session_process_ids[slot]==1){
        /* Library applets (ILibraryAppletCreator / ILibraryAppletAccessor / IStorage /
         * IStorageAccessor, command lists per suyu am/service). An applet completes as soon
         * as it starts; the Controller applet (0x0c) reports one player on npad No1. Storage
         * and accessor state live in this host table, keyed by domain object. */
        enum { LA_ACCESSOR=126, LA_STORAGE=128, LA_STORAGE_ACCESSOR=130, LA_SOURCE_STATE=703, LA_SOURCE_POP=704 };
        static struct { uint32_t domain,object,kind,applet,started,link;uint64_t size;uint8_t*data;uint32_t out_ready; } la[32];
        unsigned xs=(type>>16)&15,as=(type>>20)&15,bs=(type>>24)&15,ws=(type>>28)&15,words=header&1023,cmode=(header>>10)&15;
        unsigned descriptors=8+xs*8+(as+bs+ws)*12,raw=(descriptors+15)&~15u;
        uint8_t*m=boot_access(c,c->tpidrro,0x100,READ|WRITE,pc);if(!m)return 0;
        uint32_t object=(uint32_t)little_read(m+raw+4,4);
        unsigned parent=object && object<=16?c->domain_types[domain-1][object-1]:0;
        unsigned dcommand=m[raw];
        uint32_t command=(uint32_t)little_read(m+raw+24,4),token=(uint32_t)little_read(m+raw+28,4);
        int ours=dcommand==1 && little_read(m+raw+16,4)==UINT32_C(0x49434653) &&
                 ((parent==BOOT_SESSION_LIBRARY_CREATOR+1 && (command==0 || command==10)) ||
                  parent==LA_ACCESSOR+1 || parent==LA_STORAGE+1 || parent==LA_STORAGE_ACCESSOR+1);
        if(ours){
            uint32_t version=(type&0xffff)==6?1:0;
            uint64_t a_address=0,a_size=0,b_address=0,b_size=0,x_address=0,x_size=0,c_address=0,c_size=0;
            if(xs){uint32_t w0=(uint32_t)little_read(m+8,4);x_size=w0>>16;
                x_address=little_read(m+12,4)|((uint64_t)((w0>>12)&15)<<32)|((uint64_t)((w0>>6)&7)<<36);}
            for(unsigned k=0;k<as+bs;++k){uint8_t*d=m+8+xs*8+k*12;uint32_t w2=(uint32_t)little_read(d+8,4);
                uint64_t size=little_read(d,4)|((uint64_t)((w2>>24)&15)<<32);
                uint64_t address=little_read(d+4,4)|((uint64_t)(w2>>28)<<32)|((uint64_t)((w2>>2)&7)<<36);
                if(k<as){if(!k){a_address=address;a_size=size;}}else if(k==as){b_address=address;b_size=size;}}
            if(cmode>=2){uint8_t*d=m+descriptors+words*4;uint32_t hi=(uint32_t)little_read(d+4,4);
                c_address=little_read(d,4)|((uint64_t)(hi&0xffff)<<32);c_size=hi>>16;}
            uint32_t result=0,handle=0;int64_t value=0;int new_object=-1;unsigned value_bytes=0;
            unsigned self=0;while(self<32 && !(la[self].kind && la[self].domain==domain && la[self].object==object))++self;
            unsigned make_kind=0;
            if(parent==BOOT_SESSION_LIBRARY_CREATOR+1)make_kind=command==0?LA_ACCESSOR:LA_STORAGE;
            else if(parent==LA_STORAGE+1 && command==0)make_kind=LA_STORAGE_ACCESSOR;
            else if(parent==LA_ACCESSOR+1 && (command==101 || command==104) && self<32 && la[self].out_ready)make_kind=LA_STORAGE;
            if(parent!=BOOT_SESSION_LIBRARY_CREATOR+1 && self==32){boot_fault(c,BOOT_SVC,pc,c->tpidrro,0x100);return 0;}
            if(make_kind){
                /* A new domain object and its host table entry (an entry left by a closed object is reused). */
                unsigned child=0;while(child<16 && c->domain_types[domain-1][child])++child;
                unsigned e=0;while(e<32 && la[e].kind && !(la[e].domain==domain && la[e].object==child+1))++e;
                if(child==16 || e==32){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
                if(la[e].kind==LA_STORAGE)free(la[e].data);
                memset(&la[e],0,sizeof la[e]);la[e].domain=domain;la[e].object=child+1;la[e].kind=make_kind;
                c->domain_types[domain-1][child]=make_kind+1;new_object=(int)e;
            }
            if(parent==BOOT_SESSION_LIBRARY_CREATOR+1 && command==0){
                la[new_object].applet=(uint32_t)little_read(m+raw+32,4);
            }else if(parent==BOOT_SESSION_LIBRARY_CREATOR+1 && command==10){
                int64_t size=(int64_t)little_read(m+raw+32,8);
                if(size<0 || size>((int64_t)16<<20)){boot_fault(c,BOOT_SVC,pc,c->tpidrro,0x100);return 0;}
                la[new_object].size=(uint64_t)size;la[new_object].data=calloc(1,(size_t)size+1);
            }else if(parent==LA_STORAGE+1 && command==0){
                la[new_object].link=self;
            }else if(parent==LA_STORAGE_ACCESSOR+1 && (command==0 || command==10 || command==11)){
                unsigned st=la[self].link;
                if(st>=32 || la[st].kind!=LA_STORAGE){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
                int64_t offset=(int64_t)little_read(m+raw+32,8);
                if(command==0){value=(int64_t)la[st].size;value_bytes=8;}
                else {uint64_t address=command==10?(a_size?a_address:x_address):(b_size?b_address:c_address);
                    uint64_t size=command==10?(a_size?a_size:x_size):(b_size?b_size:c_size);
                    if(offset<0 || (uint64_t)offset>la[st].size || size>la[st].size-(uint64_t)offset)result=UINT32_C(0x3ee80);  /* AM OutOfBounds */
                    else if(size){uint8_t*g=boot_access(c,address,(unsigned)size,command==10?READ:WRITE,pc);if(!g)return 0;
                        if(command==10)memcpy(la[st].data+offset,g,(size_t)size);else memcpy(g,la[st].data+offset,(size_t)size);
                        m=boot_access(c,c->tpidrro,0x100,READ|WRITE,pc);if(!m)return 0;}}
            }else if(parent==LA_ACCESSOR+1){
                switch(command){
                case 0: case 105:{ /* state-changed / pop-out-data events, signalled once the applet has finished */
                    unsigned source=command==0?LA_SOURCE_STATE:LA_SOURCE_POP,event_slot=0;
                    while(event_slot<16 && c->event_handles[event_slot])++event_slot;
                    HANDLE event=CreateEventW(NULL,TRUE,la[self].started?TRUE:FALSE,NULL);
                    if(event_slot==16 || !event || c->next_session_handle>=0x7ffffffe){if(event)CloseHandle(event);boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
                    if(c->next_session_handle<0x10000)c->next_session_handle=0x10000;
                    handle=++c->next_session_handle;c->event_handles[event_slot]=handle;c->event_sources[event_slot]=source;
                    c->event_native_handles[event_slot]=(uint64_t)(uintptr_t)event;++c->event_create_count;break;}
                case 1:value=la[self].started?1:0;value_bytes=1;break;
                case 10:
                    la[self].started=1;la[self].out_ready=la[self].applet==0x0c;
                    for(unsigned i=0;i<16;++i)if(c->event_handles[i] && (c->event_sources[i]==LA_SOURCE_STATE || c->event_sources[i]==LA_SOURCE_POP))
                        SetEvent((HANDLE)(uintptr_t)c->event_native_handles[i]);
                    break;
                case 20: case 25: case 30: case 60: case 100: case 103: break;
                case 101: case 104:
                    if(new_object<0){result=UINT32_C(0x480);break;}   /* AM NoDataInChannel */
                    la[self].out_ready=0;la[new_object].size=0xc;la[new_object].data=calloc(1,0x10);
                    if(la[new_object].data)la[new_object].data[0]=1;   /* ControllerSupportResultInfo: 1 player, npad No1 (id 0), Success */
                    break;
                default:boot_fault(c,BOOT_SVC,pc,c->tpidrro,0x100);return 0;
                }
            }else {boot_fault(c,BOOT_SVC,pc,c->tpidrro,0x100);return 0;}
            if(new_object>=0 && la[new_object].kind==LA_STORAGE && !la[new_object].data){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
            memset(m,0,64);
            if(handle){little_write(m+4,UINT32_C(0x8000000c),4);little_write(m+8,2,4);little_write(m+12,handle,4);}
            else little_write(m+4,new_object>=0?13:value_bytes==8?14:value_bytes?13:12,4);
            if(new_object>=0){little_write(m+16,1,4);little_write(m+48,la[new_object].object,4);}
            little_write(m+32,UINT64_C(0x4f434653),4);little_write(m+36,version,4);little_write(m+40,result,4);little_write(m+44,token,4);
            if(value_bytes && new_object<0)little_write(m+48,(uint64_t)value,value_bytes);
            c->x[0]=0;return 1;
        }
    }
    /* C-descriptor mode 2 = one auto receive static, 3 = one explicit descriptor. */
    if(type==6 && !(header>>31) && (((header>>10)&15)==2 || ((header>>10)&15)==3) && !(header&~UINT32_C(0x3fff)) && domain &&
       c->session_types[slot]==BOOT_SESSION_TIME){
        /* time:u IStaticService::GetClockSnapshot(u8 type) -> ClockSnapshot (0xd0)
         * through one C (receive pointer) buffer whose descriptor follows the data words. */
        unsigned words=header&1023;
        data=boot_access(c,c->tpidrro,8+words*4+8,READ|WRITE,pc);if(!data)return 0;
        uint32_t object=(uint32_t)little_read(data+20,4),command=(uint32_t)little_read(data+40,4);
        uint64_t c_low=little_read(data+8+words*4,4),c_high=little_read(data+12+words*4,4);
        uint64_t address=c_low|((c_high&0xffff)<<32),size=c_high>>16;
        if(words<13 || (little_read(data+16,4)&0xff)!=1 || !object || object>16 ||
           c->domain_types[domain-1][object-1]!=BOOT_SESSION_TIME+1 || command!=400 ||
           little_read(data+32,4)!=UINT32_C(0x49434653) || size<0xd0 || c->session_process_ids[slot]!=1){
            boot_fault(c,BOOT_SVC,pc,c->tpidrro,8+words*4+8);return 0;
        }
        unsigned kind=(unsigned)data[48];
        uint32_t token=(uint32_t)little_read(data+44,4);
        uint8_t*out=boot_access(c,address,0xd0,WRITE,pc);if(!out)return 0;
        /* Guest steady clock and calendar (guest_time.h). */
        uint64_t steady_seconds;int64_t unix_seconds;
        if(!boot_guest_steady_unix(c,&steady_seconds,&unix_seconds)){
            boot_fault(c,BOOT_SVC,pc,0,0);return 0;
        }
        static const uint8_t source_id[16]={0x54,0x4f,0x54,0x4b,0x2d,0x57,0x69,0x6e,
                                            0x2d,0x54,0x69,0x6d,0x65,0x2d,0x30,0x31};
        /* UTC civil date (Howard Hinnant's days-to-civil). */
        int64_t days=unix_seconds/86400,rem=unix_seconds%86400;if(rem<0){rem+=86400;--days;}
        int64_t z=days+719468,era=(z>=0?z:z-146096)/146097;unsigned doe=(unsigned)(z-era*146097);
        unsigned yoe=(doe-doe/1460+doe/36524-doe/146096)/365;int64_t year=(int64_t)yoe+era*400;
        unsigned doy=doe-(365*yoe+yoe/4-yoe/100),mp=(5*doy+2)/153,day=doy-(153*mp+2)/5+1,month=mp<10?mp+3:mp-9;
        if(month<=2)++year;
        int leap=(year%4==0 && year%100!=0) || year%400==0;
        static const unsigned start[12]={0,31,59,90,120,151,181,212,243,273,304,334};
        unsigned yday=start[month-1]+day-1+(leap && month>2);
        unsigned wday=(unsigned)(((days%7)+11)%7); /* 1970-01-01 was a Thursday (4) */
        memset(out,0,0xd0);
        for(unsigned k=0;k<2;++k){
            little_write(out+k*0x20,(uint64_t)(unix_seconds-(int64_t)steady_seconds),8);
            little_write(out+k*0x20+8,steady_seconds,8);memcpy(out+k*0x20+16,source_id,16);
            little_write(out+0x40+k*8,(uint64_t)unix_seconds,8);
            uint8_t*cal=out+0x50+k*8;
            little_write(cal,(uint64_t)year,2);cal[2]=(uint8_t)month;cal[3]=(uint8_t)day;
            cal[4]=(uint8_t)(rem/3600);cal[5]=(uint8_t)((rem/60)%60);cal[6]=(uint8_t)(rem%60);
            uint8_t*info=out+0x60+k*0x18;
            little_write(info,wday,4);little_write(info+4,yday,4);memcpy(info+8,"UTC",3);
        }
        little_write(out+0x90,steady_seconds,8);memcpy(out+0x98,source_id,16);
        memcpy(out+0xa8,"UTC",3);out[0xcc]=0;out[0xcd]=(uint8_t)kind;
        memset(data,0,64);little_write(data+4,12,4);
        little_write(data+32,UINT64_C(0x4f434653),4);little_write(data+36,1,4);
        little_write(data+44,token,4);c->x[0]=0;return 1;
    }
    if(type==UINT32_C(0x00110006) && (header>>31) && domain && c->session_types[slot]==120){
        /* prepo:u SaveReport family (PID, one X event-id buffer, one A report
         * buffer): 8 + 12 special + 8 X + 12 A = 40, data aligned to +48.
         * Play reports are telemetry; accepted and discarded on this host. */
        data=boot_access(c,c->tpidrro,96,READ|WRITE,pc);if(!data)return 0;
        uint32_t object=(uint32_t)little_read(data+52,4),command=(uint32_t)little_read(data+72,4);
        if(little_read(data+8,4)!=1 || (little_read(data+48,4)&0xff)!=1 || !object || object>16 ||
           c->domain_types[domain-1][object-1]!=121 || little_read(data+64,4)!=UINT32_C(0x49434653) ||
           command<10100 || command>10107 || c->session_process_ids[slot]!=1){
            boot_fault(c,BOOT_SVC,pc,c->tpidrro,96);return 0;
        }
        uint32_t token=(uint32_t)little_read(data+76,4);
        memset(data,0,64);little_write(data+4,12,4);
        little_write(data+32,UINT64_C(0x4f434653),4);little_write(data+36,1,4);
        little_write(data+44,token,4);c->x[0]=0;return 1;
    }
    if(type==UINT32_C(0x00100006) && header==18 && domain){
        /* IFile::Write with one mapped input buffer. A bounded staging copy
         * keeps guest data intact when TLS is overwritten by the response. Size is bounded by the
         * save volume (0x33b0000), not by a fixed chunk: the prologue autosave writes 0x2335e0 bytes
         * in one request. */
        data=boot_access(c,c->tpidrro,96,READ|WRITE,pc);if(!data)return 0;
        uint32_t descriptor=(uint32_t)little_read(data+16,4);
        uint64_t capacity=little_read(data+8,4)|((uint64_t)((descriptor>>24)&15)<<32);
        uint64_t address=little_read(data+12,4)|((uint64_t)(descriptor>>28)<<32)|
                         ((uint64_t)((descriptor>>2)&0x3fffff)<<36);
        uint32_t object=(uint32_t)little_read(data+36,4);
        uint64_t offset=little_read(data+72,8),size=little_read(data+80,8);
        if(object && object<=16 && c->domain_types[domain-1][object-1]==SAVE_FS+8 &&
           little_read(data+48,4)==UINT32_C(0x49434653) && little_read(data+56,4)==1){
            uint32_t token=(uint32_t)little_read(data+60,4);
            memset(data,0,56);little_write(data+4,12,4);
            little_write(data+32,UINT32_C(0x4f434653),4);little_write(data+36,1,4);
            little_write(data+40,UINT32_C(0x307602),4);little_write(data+44,token,4);c->x[0]=0;return 1;
        }
        if((descriptor&3)!=1 || !object || object>16 ||
           (c->domain_types[domain-1][object-1]!=SAVE_FS+5 &&
            c->domain_types[domain-1][object-1]!=SAVE_FS+6 &&
            !(c->domain_types[domain-1][object-1]==SAVE_FS+9 && SAVE_FILE_PATH(domain,object))) ||
           c->session_process_ids[slot]!=1 ||
           little_read(data+32,4)!=UINT32_C(0x280001) || little_read(data+40,4)!=0 ||
           little_read(data+48,4)!=UINT32_C(0x49434653) || little_read(data+52,4)!=1 ||
           little_read(data+56,4)!=1 || little_read(data+64,8)!=0 ||
           size>capacity ||
           offset>UINT64_C(0x33b0000) || size>UINT64_C(0x33b0000)-offset ||
           address>UINT64_MAX-size ||
           (address<c->tpidrro+96 && c->tpidrro<address+size)){
            boot_fault(c,BOOT_SVC,pc,c->tpidrro,96);return 0;
        }
        uint8_t*staged=size?(uint8_t*)malloc((size_t)size):NULL;
        if(size && !staged){boot_fault(c,BOOT_SVC,pc,address,size);return 0;}
        /* Copy page by page: the buffer may span guest-adjacent regions with separate host
         * buffers (observed: 0x205a bytes across a 4 KiB and an 8 KiB region). */
        for(uint64_t done=0;done<size;){
            uint64_t at=address+done,chunk=UINT64_C(0x1000)-(at&0xfff);
            if(chunk>size-done)chunk=size-done;
            const uint8_t*source=boot_access(c,at,(unsigned)chunk,READ,pc);
            if(!source){free(staged);return 0;}
            memcpy(staged+done,source,(size_t)chunk);done+=chunk;
        }
        const wchar_t*file_path=c->domain_types[domain-1][object-1]==SAVE_FS+9?SAVE_FILE_PATH(domain,object):
            c->domain_types[domain-1][object-1]==SAVE_FS+5?
            L"startup\\save-data\\cache\\0100F2C0115B6000\\0\\storage\\CacheStorageInfo.dat":
            L"startup\\save-data\\0100F2C0115B6000\\common\\storage\\CacheStorageKey.dat";
        HANDLE file=CreateFileW(file_path,
                                GENERIC_WRITE,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
        LARGE_INTEGER position;position.QuadPart=(LONGLONG)offset;
        DWORD written=0;
        int okay=file!=INVALID_HANDLE_VALUE &&
                 SetFilePointerEx(file,position,NULL,FILE_BEGIN) &&
                 (!size || (WriteFile(file,staged,(DWORD)size,&written,NULL) && written==size)) &&
                 FlushFileBuffers(file);
        if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);
        free(staged);
        if(!okay){boot_fault(c,BOOT_SVC,pc,address,size);return 0;}
        uint32_t token=(uint32_t)little_read(data+60,4);
        memset(data,0,56);little_write(data+4,12,4);
        little_write(data+32,UINT32_C(0x4f434653),4);little_write(data+36,1,4);
        little_write(data+44,token,4);c->x[0]=0;return 1;
    }
    if(type==UINT32_C(0x00010006) && header==16 && domain){
        /* IFileSystem::CreateFile(path, s64 size, u32 option) in a mounted save container. A new file
         * gets the requested length (zero-filled, within the 0x33b0000 volume bound); option bit 0
         * (big file) needs nothing on the host. */
        data=boot_access(c,c->tpidrro,80,READ|WRITE,pc);if(!data)return 0;
        uint32_t descriptor=(uint32_t)little_read(data+8,4),object=(uint32_t)little_read(data+20,4);
        uint64_t address=little_read(data+12,4)|((uint64_t)((descriptor>>12)&15)<<32)|
                         ((uint64_t)((descriptor>>6)&7)<<36);
        unsigned length=descriptor>>16;
        if((descriptor&UINT32_C(0x00000e30)) || (descriptor&15) || length!=0x301 ||
           !object || object>16 ||
           (c->domain_types[domain-1][object-1]!=SAVE_FS+1 &&
            c->domain_types[domain-1][object-1]!=SAVE_FS+2 &&
            c->domain_types[domain-1][object-1]!=SAVE_FS+4) ||
           c->session_process_ids[slot]!=1 ||
           little_read(data+16,4)!=UINT32_C(0x200001) || little_read(data+24,8)!=0 ||
           little_read(data+32,4)!=UINT32_C(0x49434653) || little_read(data+36,4)!=1 ||
           little_read(data+40,4)!=0 || little_read(data+48,8)>UINT64_C(0x33b0000) ||
           little_read(data+56,8)>1 || little_read(data+64,8)!=0 ||
           little_read(data+72,8)!=0 ||
           address>UINT64_MAX-length ||
           (address<c->tpidrro+80 && c->tpidrro<address+length)){
            boot_fault(c,BOOT_SVC,pc,c->tpidrro,80);return 0;
        }
        const uint8_t*path=boot_access(c,address,length,READ,pc);if(!path)return 0;
        const wchar_t*root=c->domain_types[domain-1][object-1]==SAVE_FS+4?
            L"startup\\save-data\\cache\\0100F2C0115B6000\\0":
            c->domain_types[domain-1][object-1]==SAVE_FS+1?
            L"startup\\save-data\\0100F2C0115B6000\\common":
            L"startup\\save-data\\0100F2C0115B6000\\temporary";
        wchar_t host[1024];size_t used=0,count=0;
        while(root[used]){host[used]=root[used];++used;}
        if(used+length>=sizeof(host)/sizeof(host[0]) || path[0]!='/'){
            boot_fault(c,BOOT_SVC,pc,address,length);return 0;
        }
        while(count<length && path[count]){
            uint8_t ch=path[count];
            if(ch<0x20 || ch>=0x7f || ch=='\\' || ch==':' || ch=='*' ||
               ch=='?' || ch=='"' || ch=='<' || ch=='>' || ch=='|' ||
               (ch=='.' && count+1<length && path[count+1]=='.' &&
                (count==0 || path[count-1]=='/') &&
                (count+2==length || path[count+2]==0 || path[count+2]=='/'))){
                boot_fault(c,BOOT_SVC,pc,address,length);return 0;
            }
            host[used++]=ch=='/'?L'\\':(wchar_t)ch;++count;
        }
        if(count==length || count<2 || host[used-1]==L'\\'){
            boot_fault(c,BOOT_SVC,pc,address,length);return 0;
        }
        host[used]=0;
        HANDLE file=CreateFileW(host,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
        DWORD error=file==INVALID_HANDLE_VALUE?GetLastError():ERROR_SUCCESS;
        uint64_t initial=little_read(data+48,8);
        if(file!=INVALID_HANDLE_VALUE && initial){
            LARGE_INTEGER end;end.QuadPart=(LONGLONG)initial;
            if(!SetFilePointerEx(file,end,NULL,FILE_BEGIN) || !SetEndOfFile(file)){
                CloseHandle(file);DeleteFileW(host);boot_fault(c,BOOT_SVC,pc,address,length);return 0;
            }
        }
        if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);
        if(error!=ERROR_SUCCESS && error!=ERROR_FILE_EXISTS &&
           error!=ERROR_ALREADY_EXISTS && error!=ERROR_PATH_NOT_FOUND){
            boot_fault(c,BOOT_SVC,pc,address,length);return 0;
        }
        uint32_t result=error==ERROR_SUCCESS?0:
            error==ERROR_PATH_NOT_FOUND?UINT32_C(0x202):UINT32_C(0x402);
        uint32_t token=(uint32_t)little_read(data+44,4);
        memset(data,0,56);little_write(data+4,12,4);
        little_write(data+32,UINT32_C(0x4f434653),4);little_write(data+36,1,4);
        little_write(data+40,result,4);little_write(data+44,token,4);
        c->x[0]=0;return 1;
    }
    if(type==UINT32_C(0x00010006) && header==12 && domain){
        /* IFileSystem::GetEntryType / CreateDirectory for the observed save
         * containers. Resolve within the mounted volume. */
        data=boot_access(c,c->tpidrro,64,READ|WRITE,pc);if(!data)return 0;
        uint32_t descriptor=(uint32_t)little_read(data+8,4),object=(uint32_t)little_read(data+20,4);
        uint64_t address=little_read(data+12,4)|((uint64_t)((descriptor>>12)&15)<<32)|
                         ((uint64_t)((descriptor>>6)&7)<<36);
        unsigned length=descriptor>>16;
        if((descriptor&UINT32_C(0x00000e30)) || (descriptor&15) || length!=0x301 ||
           !object || object>16 ||
           (c->domain_types[domain-1][object-1]!=SAVE_FS+1 &&
            c->domain_types[domain-1][object-1]!=SAVE_FS+2 &&
            c->domain_types[domain-1][object-1]!=SAVE_FS+4) ||
           c->session_process_ids[slot]!=1 ||
           little_read(data+16,4)!=UINT32_C(0x100001) || little_read(data+24,8)!=0 ||
           little_read(data+32,4)!=UINT32_C(0x49434653) || little_read(data+36,4)!=1 ||
           !((little_read(data+40,4)==7) ||
             (little_read(data+40,4)==2 && little_read(data+48,8)==0)) ||
           address>UINT64_MAX-length ||
           (address<c->tpidrro+64 && c->tpidrro<address+length)){
            boot_fault(c,BOOT_SVC,pc,c->tpidrro,64);return 0;
        }
        const uint8_t*path=boot_access(c,address,length,READ,pc);if(!path)return 0;
        const wchar_t*root=c->domain_types[domain-1][object-1]==SAVE_FS+4?
            L"startup\\save-data\\cache\\0100F2C0115B6000\\0":
            c->domain_types[domain-1][object-1]==SAVE_FS+1?
            L"startup\\save-data\\0100F2C0115B6000\\common":
            L"startup\\save-data\\0100F2C0115B6000\\temporary";
        wchar_t host[1024];size_t used=0,count=0;
        while(root[used]){host[used]=root[used];++used;}
        if(used+length>=sizeof(host)/sizeof(host[0]) || path[0]!='/'){
            boot_fault(c,BOOT_SVC,pc,address,length);return 0;
        }
        while(count<length && path[count]){
            uint8_t ch=path[count];
            if(ch<0x20 || ch>=0x7f || ch=='\\' || ch==':' || ch=='*' ||
               ch=='?' || ch=='"' || ch=='<' || ch=='>' || ch=='|' ||
               (ch=='.' && count+1<length && path[count+1]=='.' &&
                (count==0 || path[count-1]=='/') &&
                (count+2==length || path[count+2]==0 || path[count+2]=='/'))){
                boot_fault(c,BOOT_SVC,pc,address,length);return 0;
            }
            host[used++]=ch=='/'?L'\\':(wchar_t)ch;
            ++count;
        }
        if(count==length || !count || (count>1 && host[used-1]==L'\\')){
            boot_fault(c,BOOT_SVC,pc,address,length);return 0;
        }
        host[used]=0;
        int create=little_read(data+40,4)==2;
        DWORD attributes=INVALID_FILE_ATTRIBUTES,path_error=ERROR_SUCCESS;
        if(create){
            if(!CreateDirectoryW(host,NULL))path_error=GetLastError();
        }else{
            attributes=GetFileAttributesW(host);
            if(attributes==INVALID_FILE_ATTRIBUTES)path_error=GetLastError();
        }
        if(path_error!=ERROR_SUCCESS && path_error!=ERROR_FILE_NOT_FOUND &&
           path_error!=ERROR_PATH_NOT_FOUND && path_error!=ERROR_ALREADY_EXISTS){
            boot_fault(c,BOOT_SVC,pc,address,length);return 0;
        }
        uint32_t result=path_error==ERROR_SUCCESS?0:
            path_error==ERROR_ALREADY_EXISTS?UINT32_C(0x402):UINT32_C(0x202);
        uint32_t token=(uint32_t)little_read(data+44,4);
        memset(data,0,(create || result)?56:60);
        little_write(data+4,(create || result)?12:13,4);
        little_write(data+32,UINT32_C(0x4f434653),4);little_write(data+36,1,4);
        little_write(data+40,result,4);
        little_write(data+44,token,4);
        if(!create && !result)
            little_write(data+48,(attributes&FILE_ATTRIBUTE_DIRECTORY)?0:1,4);
        c->x[0]=0;return 1;
    }
    if(type==UINT32_C(0x00010006) && header==13 && domain){
        /* IFileSystem::OpenFile with one input pointer descriptor. Resolve the
         * requested path against its own save container. The observed cache
         * information file gets a host-backed IFile object. */
        data=boot_access(c,c->tpidrro,68,READ|WRITE,pc);if(!data)return 0;
        uint32_t descriptor=(uint32_t)little_read(data+8,4),object=(uint32_t)little_read(data+20,4);
        uint64_t address=little_read(data+12,4)|((uint64_t)((descriptor>>12)&15)<<32)|
                         ((uint64_t)((descriptor>>6)&7)<<36);
        unsigned length=descriptor>>16;
        if((descriptor&UINT32_C(0x00000e30)) || (descriptor&15) || length!=0x301 ||
           !object || object>16 ||
           (c->domain_types[domain-1][object-1]!=SAVE_FS+1 &&
            c->domain_types[domain-1][object-1]!=SAVE_FS+2 &&
            c->domain_types[domain-1][object-1]!=SAVE_FS+4) ||
           c->session_process_ids[slot]!=1 ||
           little_read(data+16,4)!=UINT64_C(0x140001) ||
           little_read(data+24,8)!=0 || little_read(data+32,4)!=UINT64_C(0x49434653) ||
           little_read(data+36,4)!=1 || little_read(data+40,4)!=8 ||
           !((c->domain_types[domain-1][object-1]==SAVE_FS+4 &&
              (little_read(data+48,4)==1 || little_read(data+48,4)==6)) ||
             (c->domain_types[domain-1][object-1]!=SAVE_FS+4 &&
              (little_read(data+48,4)==1 || little_read(data+48,4)==6))) ||
           little_read(data+52,8)!=0 ||
           address>UINT64_MAX-length ||
           (address<c->tpidrro+68 && c->tpidrro<address+length)){
            boot_fault(c,BOOT_SVC,pc,c->tpidrro,68);return 0;
        }
        const uint8_t*path=boot_access(c,address,length,READ,pc);if(!path)return 0;
        wchar_t host[1024];
        int cache=c->domain_types[domain-1][object-1]==SAVE_FS+4;
        const wchar_t*root=cache?
            L"startup\\save-data\\cache\\0100F2C0115B6000\\0":
            c->domain_types[domain-1][object-1]==SAVE_FS+1?
            L"startup\\save-data\\0100F2C0115B6000\\common":
            L"startup\\save-data\\0100F2C0115B6000\\temporary";
        size_t used=0,count=0;
        while(root[used]){host[used]=root[used];++used;}
        if(used+length>=sizeof(host)/sizeof(host[0]) || path[0]!='/'){
            boot_fault(c,BOOT_SVC,pc,address,length);return 0;
        }
        while(count<length && path[count]){
            uint8_t ch=path[count];
            if(ch<0x20 || ch>=0x7f || ch=='\\' || ch==':' || ch=='*' ||
               ch=='?' || ch=='"' || ch=='<' || ch=='>' || ch=='|' ||
               (ch=='.' && count+1<length && path[count+1]=='.' &&
                (count==0 || path[count-1]=='/') &&
                (count+2==length || path[count+2]==0 || path[count+2]=='/'))){
                boot_fault(c,BOOT_SVC,pc,address,length);return 0;
            }
            host[used++]=ch=='/'?L'\\':(wchar_t)ch;
            ++count;
        }
        if(count==length || count<2 || host[used-1]==L'\\'){
            boot_fault(c,BOOT_SVC,pc,address,length);return 0;
        }
        host[used]=0;
        DWORD attributes=GetFileAttributesW(host);
        DWORD path_error=attributes==INVALID_FILE_ATTRIBUTES?GetLastError():ERROR_SUCCESS;
        int cache_info=cache &&
            wcscmp(host,L"startup\\save-data\\cache\\0100F2C0115B6000\\0\\storage\\CacheStorageInfo.dat")==0;
        int common_key=c->domain_types[domain-1][object-1]==SAVE_FS+1 &&
            wcscmp(host,L"startup\\save-data\\0100F2C0115B6000\\common\\storage\\CacheStorageKey.dat")==0;
        if(!cache_info && !common_key && attributes!=INVALID_FILE_ATTRIBUTES && !(attributes&FILE_ATTRIBUTE_DIRECTORY) &&
           used<300){
            unsigned child=0;while(child<16 && c->domain_types[domain-1][child])++child;
            if(child==16){boot_fault(c,BOOT_SVC,pc,address,length);return 0;}
            unsigned e=0;while(e<32 && save_files[e].path[0] && !(save_files[e].domain==domain && save_files[e].object==child+1))++e;
            if(e==32)e=child;   /* table full: reuse by object number */
            save_files[e].domain=domain;save_files[e].object=child+1;wcscpy(save_files[e].path,host);
            FILE*sidecar=_wfopen(L"startup\\save-data\\open-files.txt",L"a");
            if(sidecar){fwprintf(sidecar,L"%u %u %ls\n",domain,child+1,host);fclose(sidecar);}
            uint32_t token=(uint32_t)little_read(data+44,4);
            c->domain_types[domain-1][child]=SAVE_FS+9;
            memset(data,0,60);little_write(data+4,13,4);little_write(data+16,1,4);
            little_write(data+32,UINT32_C(0x4f434653),4);little_write(data+36,1,4);
            little_write(data+44,token,4);little_write(data+48,child+1,4);
            c->x[0]=0;return 1;
        }
        if((cache_info || common_key) && attributes!=INVALID_FILE_ATTRIBUTES &&
           !(attributes&FILE_ATTRIBUTE_DIRECTORY)){
            unsigned child=0;while(child<16 && c->domain_types[domain-1][child])++child;
            if(child==16){boot_fault(c,BOOT_SVC,pc,address,length);return 0;}
            uint32_t token=(uint32_t)little_read(data+44,4);
            c->domain_types[domain-1][child]=cache_info?
                (little_read(data+48,4)==1?SAVE_FS+8:SAVE_FS+5):SAVE_FS+6;
            memset(data,0,60);little_write(data+4,13,4);little_write(data+16,1,4);
            little_write(data+32,UINT32_C(0x4f434653),4);little_write(data+36,1,4);
            little_write(data+44,token,4);little_write(data+48,child+1,4);
            c->x[0]=0;return 1;
        }
        if(attributes!=INVALID_FILE_ATTRIBUTES ||
           (path_error!=ERROR_FILE_NOT_FOUND && path_error!=ERROR_PATH_NOT_FOUND)){
            FILE*log=fopen("save-open-rejected.txt","a");
            if(log){fprintf(log,"OpenFile mode %u type %u attributes 0x%lx error %lu path %ls",(unsigned)little_read(data+48,4),
                            c->domain_types[domain-1][object-1],(unsigned long)attributes,(unsigned long)path_error,host);fputc(10,log);fclose(log);}
            boot_fault(c,BOOT_SVC,pc,address,length);return 0;
        }
        uint32_t token=(uint32_t)little_read(data+44,4);
        memset(data,0,56);little_write(data+4,12,4);
        little_write(data+32,UINT32_C(0x4f434653),4);little_write(data+36,1,4);
        little_write(data+40,UINT32_C(0x202),4);little_write(data+44,token,4);
        c->x[0]=0;return 1;
    }
    if(type==6 && header==12 && domain){
        /* IFileSystem::Commit for the mounted save volumes. The supported
         * file in each volume is reopened and flushed before success. */
        data=boot_access(c,c->tpidrro,56,READ|WRITE,pc);if(!data)return 0;
        uint32_t object=(uint32_t)little_read(data+20,4);
        if(object && object<=16 &&
           (c->domain_types[domain-1][object-1]==SAVE_FS+1 ||
            c->domain_types[domain-1][object-1]==SAVE_FS+4) &&
           c->session_process_ids[slot]==1 &&
           little_read(data+16,4)==UINT32_C(0x100001) && little_read(data+24,8)==0 &&
           little_read(data+32,4)==UINT32_C(0x49434653) && little_read(data+36,4)==1 &&
           little_read(data+40,4)==10){
            const wchar_t*file_path=c->domain_types[domain-1][object-1]==SAVE_FS+4?
                L"startup\\save-data\\cache\\0100F2C0115B6000\\0\\storage\\CacheStorageInfo.dat":
                L"startup\\save-data\\0100F2C0115B6000\\common\\storage\\CacheStorageKey.dat";
            HANDLE file=CreateFileW(file_path,
                                    GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,NULL);
            if(file==INVALID_HANDLE_VALUE){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
            int okay=FlushFileBuffers(file);CloseHandle(file);
            if(!okay){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
            uint32_t token=(uint32_t)little_read(data+44,4);
            memset(data,0,56);little_write(data+4,12,4);
            little_write(data+32,UINT32_C(0x4f434653),4);little_write(data+36,1,4);
            little_write(data+44,token,4);c->x[0]=0;return 1;
        }
    }
    uint32_t sized_object=0;
    if(type==6 && header==14 && domain){uint8_t*peek=boot_access(c,c->tpidrro,24,READ,pc);if(!peek)return 0;sized_object=(uint32_t)little_read(peek+20,4);}
    /* Only the object types handled below claim this packet length; others (e.g.
     * ITimeZoneService::ToCalendarTimeWithMyRule) use the general domain dispatcher. */
    /* Domain object type numbers overlap between save-filesystem objects (SAVE_FS+k) and
     * time-service objects (BOOT_SESSION_TIME+k, e.g. ITimeZoneService == SAVE_FS+6), so the
     * owning session's type decides which family an object belongs to. */
    if(type==6 && header==14 && domain && sized_object && sized_object<=16 && c->session_types[slot]!=BOOT_SESSION_TIME &&
       (c->domain_types[domain-1][sized_object-1]==BOOT_SESSION_AUDIO_CONTROL+1 ||
        c->domain_types[domain-1][sized_object-1]==SAVE_FS+5 || c->domain_types[domain-1][sized_object-1]==SAVE_FS+6 ||
        c->domain_types[domain-1][sized_object-1]==SAVE_FS+8 || c->domain_types[domain-1][sized_object-1]==SAVE_FS+9)){
        /* IFile::SetSize for the save files (SAVE_FS+8, opened read-only, gets WriteNotPermitted). */
        data=boot_access(c,c->tpidrro,64,READ|WRITE,pc);if(!data)return 0;
        uint32_t object=(uint32_t)little_read(data+20,4);
        if(object && object<=16 && c->domain_types[domain-1][object-1]==BOOT_SESSION_AUDIO_CONTROL+1){
            /* IAudioController::SetExpectedMasterVolume shares this packet
             * length with IFile::SetSize, but has two binary32 arguments. */
            uint32_t main_gain=(uint32_t)little_read(data+48,4);
            uint32_t library_gain=(uint32_t)little_read(data+52,4);
            if(c->session_process_ids[slot]!=1 || !c->applet_resource_user_id ||
               little_read(data+16,4)!=UINT32_C(0x180001) || little_read(data+24,8)!=0 ||
               little_read(data+32,4)!=UINT32_C(0x49434653) || little_read(data+36,4)!=1 ||
               little_read(data+40,4)!=0 || little_read(data+56,8)!=0 ||
               (main_gain && main_gain!=UINT32_C(0x3f800000)) ||
               (library_gain && library_gain!=UINT32_C(0x3f800000))){
                boot_fault(c,BOOT_SVC,pc,c->tpidrro,64);return 0;
            }
            uint32_t token=(uint32_t)little_read(data+44,4);
            c->audio_device_volume_valid=(c->audio_device_volume_valid&~UINT32_C(0x300))|
                (main_gain?0:UINT32_C(0x100))|(library_gain?0:UINT32_C(0x200));
            memset(data,0,56);little_write(data+4,12,4);
            little_write(data+32,UINT32_C(0x4f434653),4);
            little_write(data+36,1,4);little_write(data+44,token,4);
            c->x[0]=0;return 1;
        }
        uint64_t size=little_read(data+48,8);
        if(object && object<=16 && c->domain_types[domain-1][object-1]==SAVE_FS+8 &&
           c->session_process_ids[slot]==1 && little_read(data+32,4)==UINT32_C(0x49434653) &&
           little_read(data+40,4)==3){
            /* File opened read-only: the server's IFile::DrySetSize returns
             * ResultWriteNotPermitted (FS 6203) without touching the file. */
            uint32_t token=(uint32_t)little_read(data+44,4);
            memset(data,0,56);little_write(data+4,12,4);
            little_write(data+32,UINT32_C(0x4f434653),4);little_write(data+36,1,4);
            little_write(data+40,UINT32_C(0x307602),4);little_write(data+44,token,4);c->x[0]=0;return 1;
        }
        if(!object || object>16 ||
           (c->domain_types[domain-1][object-1]!=SAVE_FS+5 &&
            c->domain_types[domain-1][object-1]!=SAVE_FS+6 &&
            !(c->domain_types[domain-1][object-1]==SAVE_FS+9 && SAVE_FILE_PATH(domain,object))) ||
           c->session_process_ids[slot]!=1 ||
           little_read(data+16,4)!=UINT32_C(0x180001) || little_read(data+24,8)!=0 ||
           little_read(data+32,4)!=UINT32_C(0x49434653) || little_read(data+36,4)!=1 ||
           little_read(data+40,4)!=3 || little_read(data+56,8)!=0 ||
           size>UINT64_C(0x33b0000)){
            boot_fault(c,BOOT_SVC,pc,c->tpidrro,64);return 0;
        }
        const wchar_t*file_path=c->domain_types[domain-1][object-1]==SAVE_FS+9?SAVE_FILE_PATH(domain,object):
            c->domain_types[domain-1][object-1]==SAVE_FS+5?
            L"startup\\save-data\\cache\\0100F2C0115B6000\\0\\storage\\CacheStorageInfo.dat":
            L"startup\\save-data\\0100F2C0115B6000\\common\\storage\\CacheStorageKey.dat";
        HANDLE file=CreateFileW(file_path,GENERIC_WRITE,FILE_SHARE_READ,NULL,OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL,NULL);
        LARGE_INTEGER position;position.QuadPart=(LONGLONG)size;
        int okay=file!=INVALID_HANDLE_VALUE && SetFilePointerEx(file,position,NULL,FILE_BEGIN) &&
                 SetEndOfFile(file) && FlushFileBuffers(file);
        if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);
        if(!okay){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
        uint32_t token=(uint32_t)little_read(data+44,4);
        memset(data,0,56);little_write(data+4,12,4);
        little_write(data+32,UINT32_C(0x4f434653),4);little_write(data+36,1,4);
        little_write(data+44,token,4);c->x[0]=0;return 1;
    }
    if(type==6 && header==12 && domain){
        /* IFile::Flush / GetSize for the two observed save files. */
        data=boot_access(c,c->tpidrro,64,READ|WRITE,pc);if(!data)return 0;
        uint32_t object=(uint32_t)little_read(data+20,4);
        if(object && object<=16 &&
           (c->domain_types[domain-1][object-1]==SAVE_FS+5 ||
            c->domain_types[domain-1][object-1]==SAVE_FS+6 ||
            c->domain_types[domain-1][object-1]==SAVE_FS+8 ||
            (c->domain_types[domain-1][object-1]==SAVE_FS+9 && SAVE_FILE_PATH(domain,object))) &&
           c->session_process_ids[slot]==1 &&
           little_read(data+16,4)==UINT32_C(0x100001) && little_read(data+24,8)==0 &&
           little_read(data+32,4)==UINT32_C(0x49434653) && little_read(data+36,4)==1 &&
           (little_read(data+40,4)==2 || little_read(data+40,4)==4)){
            int flush=little_read(data+40,4)==2;
            if(flush && c->domain_types[domain-1][object-1]==SAVE_FS+8){
                uint32_t token=(uint32_t)little_read(data+44,4);
                memset(data,0,56);little_write(data+4,12,4);
                little_write(data+32,UINT32_C(0x4f434653),4);little_write(data+36,1,4);
                little_write(data+44,token,4);c->x[0]=0;return 1;
            }
            const wchar_t*file_path=c->domain_types[domain-1][object-1]==SAVE_FS+9?SAVE_FILE_PATH(domain,object):
                c->domain_types[domain-1][object-1]!=SAVE_FS+6?
                L"startup\\save-data\\cache\\0100F2C0115B6000\\0\\storage\\CacheStorageInfo.dat":
                L"startup\\save-data\\0100F2C0115B6000\\common\\storage\\CacheStorageKey.dat";
            HANDLE file=CreateFileW(file_path,
                                    flush?GENERIC_WRITE:GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,NULL);
            LARGE_INTEGER size;
            if(file==INVALID_HANDLE_VALUE){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
            int okay=flush?FlushFileBuffers(file):GetFileSizeEx(file,&size);CloseHandle(file);
            if(!okay || (!flush && size.QuadPart<0)){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
            uint32_t token=(uint32_t)little_read(data+44,4);
            memset(data,0,flush?56:64);little_write(data+4,flush?12:14,4);
            little_write(data+32,UINT32_C(0x4f434653),4);little_write(data+36,1,4);
            little_write(data+44,token,4);
            if(!flush)little_write(data+48,(uint64_t)size.QuadPart,8);
            c->x[0]=0;return 1;
        }
    }
    if(type==UINT32_C(0x01000006) && header==18 && domain){
        /* IFile::Read from the two persisted save files. Stage host bytes so
         * an IO failure never leaves a partially modified guest buffer. */
        data=boot_access(c,c->tpidrro,96,READ|WRITE,pc);if(!data)return 0;
        uint32_t descriptor=(uint32_t)little_read(data+16,4),object=(uint32_t)little_read(data+36,4);
        uint64_t address=little_read(data+12,4)|((uint64_t)(descriptor>>28)<<32)|
                         ((uint64_t)((descriptor>>2)&0x3fffff)<<36);
        uint64_t capacity=little_read(data+8,4)|((uint64_t)((descriptor>>24)&15)<<32);
        uint64_t offset=little_read(data+72,8),size=little_read(data+80,8);
        unsigned file_type=object && object<=16?c->domain_types[domain-1][object-1]:0;
        if((descriptor&3)!=1 || !object || object>16 ||
           (file_type!=SAVE_FS+5 && file_type!=SAVE_FS+6 && file_type!=SAVE_FS+8 &&
            !(file_type==SAVE_FS+9 && SAVE_FILE_PATH(domain,object))) ||
           c->session_process_ids[slot]!=1 ||
           little_read(data+32,4)!=UINT32_C(0x280001) || little_read(data+40,4)!=0 ||
           little_read(data+48,4)!=UINT32_C(0x49434653) || little_read(data+52,4)!=1 ||
           little_read(data+56,4)!=0 || little_read(data+64,8)!=0 ||
           size>capacity || size>UINT64_C(0x33b0000) || offset>UINT64_C(0x33b0000) ||
           address>UINT64_MAX-size ||
           (size && address<c->tpidrro+96 && c->tpidrro<address+size)){
            boot_fault(c,BOOT_SVC,pc,c->tpidrro,96);return 0;
        }
        const wchar_t*file_path=file_type==SAVE_FS+9?SAVE_FILE_PATH(domain,object):file_type!=SAVE_FS+6?
            L"startup\\save-data\\cache\\0100F2C0115B6000\\0\\storage\\CacheStorageInfo.dat":
            L"startup\\save-data\\0100F2C0115B6000\\common\\storage\\CacheStorageKey.dat";
        HANDLE file=CreateFileW(file_path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL,NULL);
        LARGE_INTEGER length,position;position.QuadPart=(LONGLONG)offset;
        if(file==INVALID_HANDLE_VALUE || !GetFileSizeEx(file,&length) || length.QuadPart<0 ||
           !SetFilePointerEx(file,position,NULL,FILE_BEGIN)){
            if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);
            boot_fault(c,BOOT_SVC,pc,0,0);return 0;
        }
        uint64_t available=offset<(uint64_t)length.QuadPart?(uint64_t)length.QuadPart-offset:0;
        uint32_t count=(uint32_t)(size<available?size:available);
        uint8_t*staged=count?(uint8_t*)malloc(count):NULL;
        DWORD got=0;
        int okay=!count || (staged && ReadFile(file,staged,count,&got,NULL) && got==count);
        CloseHandle(file);
        if(!okay){free(staged);boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
        /* The output may span guest-adjacent regions with separate host buffers: check every page
         * for write access first (a fault leaves the buffer untouched), then copy page by page. */
        for(uint64_t done=0;done<count;){
            uint64_t at=address+done,chunk=UINT64_C(0x1000)-(at&0xfff);if(chunk>count-done)chunk=count-done;
            if(!boot_access(c,at,(unsigned)chunk,WRITE,pc)){free(staged);return 0;}
            done+=chunk;
        }
        for(uint64_t done=0;done<count;){
            uint64_t at=address+done,chunk=UINT64_C(0x1000)-(at&0xfff);if(chunk>count-done)chunk=count-done;
            memcpy(boot_access(c,at,(unsigned)chunk,WRITE,pc),staged+done,(size_t)chunk);done+=chunk;
        }
        if(count)c->exclusive_valid=0;
        free(staged);
        uint32_t token=(uint32_t)little_read(data+60,4);
        memset(data,0,64);little_write(data+4,14,4);
        little_write(data+32,UINT32_C(0x4f434653),4);little_write(data+36,1,4);
        little_write(data+44,token,4);little_write(data+48,count,8);c->x[0]=0;return 1;
    }
    if(type==UINT32_C(0x01000006) && header==12 && domain){
        /* ISaveDataInfoReader::Read with one mapped output array. This host
         * has at most the cache volume created for the pinned application. */
        data=boot_access(c,c->tpidrro,64,READ|WRITE,pc);if(!data)return 0;
        uint32_t descriptor=(uint32_t)little_read(data+16,4),object=(uint32_t)little_read(data+36,4);
        uint64_t address=little_read(data+12,4)|((uint64_t)(descriptor>>28)<<32)|((uint64_t)((descriptor>>2)&0x3fffff)<<36);
        uint64_t capacity=little_read(data+8,4)|((uint64_t)((descriptor>>24)&15)<<32);
        uint32_t token=(uint32_t)little_read(data+60,4);
        unsigned reader_type=object && object<=16?c->domain_types[domain-1][object-1]:0;
        if((descriptor&3)==2 || !object || object>16 ||
           (reader_type!=SAVE_FS+3 && reader_type!=SAVE_FS+7) ||
           little_read(data+32,4)!=UINT64_C(0x100001) || little_read(data+40,4)!=0 ||
           little_read(data+48,4)!=UINT64_C(0x49434653) || little_read(data+52,4)!=1 ||
           little_read(data+56,4)!=0 || capacity==0 || capacity%0x60 ||
           capacity>16u*1024u*1024u || address>UINT64_MAX-capacity ||
           (address<c->tpidrro+64 && c->tpidrro<address+capacity) ||
           !boot_access(c,address,(unsigned)capacity,WRITE,pc)){
            boot_fault(c,BOOT_SVC,pc,c->tpidrro,64);return 0;
        }
        WIN32_FIND_DATAW entry;
        HANDLE listing=FindFirstFileW(L"startup\\save-data\\cache\\*",&entry);
        int present=0;
        if(listing!=INVALID_HANDLE_VALUE){
            do{
                if(!(entry.cFileName[0]==L'.' &&
                     (entry.cFileName[1]==0 ||
                      (entry.cFileName[1]==L'.' && entry.cFileName[2]==0)))){
                    if(wcscmp(entry.cFileName,L"0100F2C0115B6000")!=0 ||
                       !(entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)){
                        FindClose(listing);boot_fault(c,BOOT_SVC,pc,0,0);return 0;
                    }
                    present=1;
                }
            }while(FindNextFileW(listing,&entry));
            FindClose(listing);
        }else if(GetLastError()!=ERROR_FILE_NOT_FOUND){
            boot_fault(c,BOOT_SVC,pc,0,0);return 0;
        }
        if(present){
            WIN32_FILE_ATTRIBUTE_DATA reserve;
            DWORD volume=GetFileAttributesW(L"startup\\save-data\\cache\\0100F2C0115B6000\\0");
            if(volume==INVALID_FILE_ATTRIBUTES || !(volume&FILE_ATTRIBUTE_DIRECTORY) ||
               !GetFileAttributesExW(L"startup\\save-data\\cache-reserve-0100F2C0115B6000-0.bin",
                                     GetFileExInfoStandard,&reserve) ||
               (reserve.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) ||
               reserve.nFileSizeHigh || reserve.nFileSizeLow!=UINT32_C(0x33dc000)){
                boot_fault(c,BOOT_SVC,pc,0,0);return 0;
            }
        }
        uint64_t entries=present && reader_type==SAVE_FS+3?1:0;
        if(entries){
            /* FsSaveDataInfo is 0x60 bytes. The host-created volume has a
             * stable local ID, User space, Cache type, application and size. */
            uint8_t*output=boot_access(c,address,0x60,WRITE,pc);if(!output)return 0;
            memset(output,0,0x60);
            little_write(output,1,8);
            output[8]=1;output[9]=5;
            little_write(output+0x28,UINT64_C(0x0100f2c0115b6000),8);
            little_write(output+0x30,UINT64_C(0x33dc000),8);
            c->domain_types[domain-1][object-1]=SAVE_FS+7;
        }
        memset(data,0,64);little_write(data+4,14,4);
        little_write(data+32,UINT32_C(0x4f434653),4);little_write(data+36,1,4);
        little_write(data+44,token,4);little_write(data+48,entries,8);c->x[0]=0;return 1;
    }
    if((type==UINT32_C(0x01000004) || type==UINT32_C(0x01000006)) && header==16 && domain){
        /* IStorage::Read: one mapped receive buffer, no handles/pointers. */
        data=boot_access(c,c->tpidrro,84,READ|WRITE,pc);if(!data)return 0;
        uint32_t descriptor=(uint32_t)little_read(data+16,4),object=(uint32_t)little_read(data+36,4);
        uint64_t address=little_read(data+12,4)|((uint64_t)(descriptor>>28)<<32)|((uint64_t)((descriptor>>2)&0x3fffff)<<36);
        uint64_t capacity=little_read(data+8,4)|((uint64_t)((descriptor>>24)&15)<<32);
        uint64_t offset=little_read(data+64,8),size=little_read(data+72,8);
        uint32_t version=(type&0xffff)==6?1:0,token=(uint32_t)little_read(data+60,4);
        if((descriptor&3)==2 || !object || object>16 || c->domain_types[domain-1][object-1]!=BOOT_SESSION_FSP+3 ||
           little_read(data+32,4)!=UINT64_C(0x200001) || little_read(data+40,4)!=0 ||
           little_read(data+48,4)!=UINT64_C(0x49434653) || little_read(data+52,4)!=version ||
           little_read(data+56,4)!=0 || !c->storage_native || size>capacity || size>64u*1024u*1024u ||
           address>UINT64_MAX-size || (size && address<c->tpidrro+84 && c->tpidrro<address+size)){
            boot_fault(c,BOOT_SVC,pc,c->tpidrro,84);return 0;
        }
        uint8_t*output=size?boot_access(c,address,(unsigned)size,WRITE,pc):NULL;
        /* A receive buffer may span adjacent guest regions with separate host blocks
         * (e.g. neighbouring MapPhysicalMemory mappings). Preflight every piece for
         * write access, read into a staging buffer, then copy piecewise. */
        int split=0;
        if(size && !output){
            if(c->status!=CH_ADDRESS || c->fault_pc!=pc)return 0;
            c->status=0;c->fault_pc=c->fault_address=c->fault_width=0;
            for(uint64_t done=0;done<size;){uint64_t at=address+done,piece=0;
                for(unsigned i=0;i<c->region_count;++i){Region*r=&c->regions[i];
                    if(at>=r->base && at-r->base<r->size){piece=r->base+r->size-at;break;}}
                if(!piece){boot_fault(c,CH_ADDRESS,pc,at,size-done);return 0;}
                if(piece>size-done)piece=size-done;
                if(!boot_access(c,at,(unsigned)piece,WRITE,pc))return 0;
                done+=piece;
            }
            split=1;
        }
        BootStorage*storage=(BootStorage*)(uintptr_t)c->storage_native;
        int read_ok=0;
        if(split && offset<=storage->size && size<=storage->size-offset){
            uint8_t*staged=malloc((size_t)size);
            if(staged){
                uint64_t done=0;
                while(done<size){
                    uint64_t chunk=size-done;if(chunk>BOOT_STORAGE_READ_LIMIT)chunk=BOOT_STORAGE_READ_LIMIT;
                    if(!boot_storage_read(storage,offset+done,chunk,staged+(size_t)done))break;
                    done+=chunk;
                }
                if(done==size){
                    for(uint64_t copied=0;copied<size;){uint64_t at=address+copied,piece=0;
                        for(unsigned i=0;i<c->region_count;++i){Region*r=&c->regions[i];
                            if(at>=r->base && at-r->base<r->size){piece=r->base+r->size-at;break;}}
                        if(piece>size-copied)piece=size-copied;
                        memcpy(boot_access(c,at,(unsigned)piece,WRITE,pc),staged+(size_t)copied,(size_t)piece);
                        copied+=piece;
                    }
                    read_ok=1;
                }
                free(staged);
            }
        }else if(offset<=storage->size && size<=storage->size-offset){
            if(size<=BOOT_STORAGE_READ_LIMIT)read_ok=boot_storage_read(storage,offset,size,output);
            else {
                uint8_t*staged=malloc((size_t)size);
                if(staged){
                    uint64_t done=0;
                    while(done<size){
                        uint64_t chunk=size-done;if(chunk>BOOT_STORAGE_READ_LIMIT)chunk=BOOT_STORAGE_READ_LIMIT;
                        if(!boot_storage_read(storage,offset+done,chunk,staged+(size_t)done))break;
                        done+=chunk;
                    }
                    if(done==size){memcpy(output,staged,(size_t)size);read_ok=1;}
                    free(staged);
                }
            }
        }
        if(!read_ok){
            boot_fault(c,BOOT_SVC,pc,address,size);return 0;
        }
        if(size)c->exclusive_valid=0;
        ++c->storage_read_count;c->storage_bytes_read+=size;
        {   /* romfs read log (offset size), mapped to file names by debug/romfs_index.py; TOTK_ROMFS_READ_LOG=0 disables */
            static FILE*read_log;static int read_log_tried;
            if(!read_log_tried){read_log_tried=1;const char*e=getenv("TOTK_ROMFS_READ_LOG");if(!e || *e!='0')read_log=fopen("romfs-reads.log","a");}
            if(read_log){fprintf(read_log,"%#llx %#llx\n",(unsigned long long)offset,(unsigned long long)size);fflush(read_log);}
        }
        memset(data,0,56);little_write(data+4,12,4);little_write(data+32,UINT32_C(0x4f434653),4);
        little_write(data+36,version,4);little_write(data+44,token,4);c->x[0]=0;return 1;
    }
    /* One A (send) descriptor after a PID-only special header leaves the domain
     * header at +32, as without a buffer; only nfp IUser::Initialize accepts it. */
    unsigned send_buffers=0;
    if(type==UINT32_C(0x00100006) && (header>>31) && domain){send_buffers=1;type=6;}
    if((type!=4 && type!=6) || !domain || (header&~UINT32_C(0x800003ff))){
        boot_fault(c,BOOT_SVC,pc,c->tpidrro,8);return 0;
    }
    unsigned special=header>>31,words=header&1023,raw=special?32:16;
    unsigned special_header=0;
    if(special){
        data=boot_access(c,c->tpidrro,12,READ,pc);if(!data)return 0;
        special_header=(unsigned)little_read(data+8,4);
        if(special_header!=1 && special_header!=3){boot_fault(c,BOOT_SVC,pc,c->tpidrro,12);return 0;}
    }
    unsigned total=8+special*12+(special_header==3?4:0)+words*4;
    if(total>256 || total<raw+16){boot_fault(c,BOOT_SVC,pc,c->tpidrro,total);return 0;}
    data=boot_access(c,c->tpidrro,total,READ|WRITE,pc);if(!data)return 0;
    uint32_t object=(uint32_t)little_read(data+raw+4,4);
    if(!special && words==8 && little_read(data+raw,4)==2 && little_read(data+raw+8,4)==0){
        /* Domain Close has no SFCI payload. Unregister only this object;
         * the session and other objects retain their independent lifetimes.
         * Repeated close is harmless, matching the upstream domain dispatcher. */
        if(!object || object>16){boot_fault(c,BOOT_SVC,pc,c->tpidrro,total);return 0;}
        c->domain_types[domain-1][object-1]=0;
        memset(data,0,8);c->x[0]=0;return 1;
    }
    if(!object || object>16 || !c->domain_types[domain-1][object-1]){
        boot_fault(c,BOOT_SVC,pc,c->tpidrro,total);return 0;
    }
    if(total<raw+32 || little_read(data+raw+8,4)!=0 ||
       little_read(data+raw+16,4)!=UINT64_C(0x49434653) ||
       little_read(data+raw+20,4)!=(type==6?1u:0u)){
        boot_fault(c,BOOT_SVC,pc,c->tpidrro,total);return 0;
    }
    unsigned parent_type=c->domain_types[domain-1][object-1],child_type;
    unsigned command=(unsigned)little_read(data+raw+24,4);
    if(parent_type==122+1 || parent_type==124+1){
        FILE*log=fopen("nfp-requests.txt","a");
        if(log){fprintf(log,"object_type=%u command=%u type=0x%x header=0x%x step=%llu",parent_type,command,(unsigned)little_read(data,4),header,(unsigned long long)c->steps);
                fputc(10,log);fclose(log);}
    }
    if(send_buffers && (parent_type!=124+1 || command!=0 || special_header!=1)){
        boot_fault(c,BOOT_SVC,pc,c->tpidrro,total);return 0;
    }
    /* The observed applet profile uses only exact 0.0f/1.0f expected gains.
     * Keep their zero-state flags in unused upper bits of the retained audio
     * state word, separate from audio-device validity in bits 0..2. */
    enum { BOOT_APPLET_MAIN_GAIN_ZERO=1u<<8,BOOT_APPLET_LIBRARY_GAIN_ZERO=1u<<9 };
    if(type==6 && !special && words==12 &&
       little_read(data+raw,4)==UINT32_C(0x100001) &&
       parent_type==BOOT_SESSION_AUDIO_CONTROL+1 &&
       (command==1 || command==2) && c->session_process_ids[slot]==1 &&
       c->applet_resource_user_id){
        /* This local host starts both applet volumes at neutral gain; it does
         * not claim to observe a physical Switch's current volume state. */
        uint32_t token=(uint32_t)little_read(data+raw+28,4);
        uint32_t zero=command==1?BOOT_APPLET_MAIN_GAIN_ZERO:BOOT_APPLET_LIBRARY_GAIN_ZERO;
        data=boot_access(c,c->tpidrro,60,READ|WRITE,pc);if(!data)return 0;
        memset(data,0,60);little_write(data+4,13,4);
        little_write(data+32,UINT32_C(0x4f434653),4);
        little_write(data+36,1,4);little_write(data+44,token,4);
        little_write(data+48,(c->audio_device_volume_valid&zero)?0:UINT32_C(0x3f800000),4);
        c->x[0]=0;return 1;
    }
    if(c->session_types[slot]==BOOT_SESSION_TIME && parent_type==BOOT_SESSION_TIME+1 && command==20 && type==6 && !special &&
       words==12 && little_read(data+raw,4)==UINT32_C(0x100001) && object==1 &&
       c->session_process_ids[slot]==1){
        /* time:u GetSharedMemoryNativeHandle. The shared page has the same
         * lock-free double-buffer layout consumed by Nintendo's time client. */
        unsigned entry=0;while(entry<16 && c->event_handles[entry])++entry;
        if(entry==16 || c->next_session_handle>=0x7ffffffe){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
        HANDLE memory=CreateFileMappingW(INVALID_HANDLE_VALUE,NULL,PAGE_READWRITE,0,0x1000,NULL);
        if(!memory){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
        uint8_t*page=(uint8_t*)MapViewOfFile(memory,FILE_MAP_WRITE,0,0,0x1000);
        /* The time client extrapolates steady time from the guest tick (internal offset 0), so
         * the context time point and offset come from the same guest clock (guest_time.h). */
        uint64_t steady_seconds;int64_t unix_signed;
        if(!page || !boot_guest_steady_unix(c,&steady_seconds,&unix_signed)){
            if(page)UnmapViewOfFile(page);CloseHandle(memory);boot_fault(c,BOOT_SVC,pc,0,0);return 0;
        }
        uint64_t unix_seconds=(uint64_t)unix_signed;
        uint64_t offset=unix_seconds-steady_seconds;
        static const uint8_t source_id[16]={0x54,0x4f,0x54,0x4b,0x2d,0x57,0x69,0x6e,
                                            0x2d,0x54,0x69,0x6d,0x65,0x2d,0x30,0x31};
        memset(page,0,0x1000);
        for(unsigned copy=0;copy<2;++copy){
            unsigned steady=8+copy*0x18;
            memcpy(page+steady+8,source_id,sizeof(source_id));
            for(unsigned context=0;context<2;++context){
                unsigned at=(context?0x80:0x38)+8+copy*0x20;
                little_write(page+at,offset,8);
                little_write(page+at+8,steady_seconds,8);
                memcpy(page+at+16,source_id,sizeof(source_id));
            }
        }
        if(!UnmapViewOfFile(page)){CloseHandle(memory);boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
        if(c->next_session_handle<0x10000)c->next_session_handle=0x10000;
        uint32_t handle=++c->next_session_handle,token=(uint32_t)little_read(data+raw+28,4);
        c->event_handles[entry]=handle;c->event_sources[entry]=700;
        c->event_native_handles[entry]=(uint64_t)(uintptr_t)memory;
        data=boot_access(c,c->tpidrro,64,READ|WRITE,pc);if(!data)return 0;
        memset(data,0,64);little_write(data+4,UINT32_C(0x8000000c),4);
        little_write(data+8,2,4);little_write(data+12,handle,4);
        little_write(data+32,UINT32_C(0x4f434653),4);little_write(data+36,1,4);
        little_write(data+44,token,4);c->x[0]=0;return 1;
    }
    if(parent_type==BOOT_SESSION_APP_FUNCTIONS+1 && command==1 && !special && words==13 &&
       little_read(data+raw,4)==UINT64_C(0x140001) && c->applet_resource_user_id && c->session_process_ids[slot]==1 &&
       (little_read(data+raw+32,4)==1 || little_read(data+raw+32,4)==2)){
        /* No user/general launch parameter was supplied by this host launch.
         * The pinned SDK's TryPop explicitly recognizes AM result 0x480. */
        uint32_t token=(uint32_t)little_read(data+raw+28,4);
        memset(data,0,56);little_write(data+4,12,4);little_write(data+32,UINT32_C(0x4f434653),4);
        little_write(data+36,type==6?1:0,4);little_write(data+40,UINT32_C(0x480),4);little_write(data+44,token,4);
        c->x[0]=0;return 1;
    }
    if(parent_type==BOOT_SESSION_FSP+10 && command==150 && !special && words==12 &&
       little_read(data+raw,4)==UINT64_C(0x100001) && c->session_process_ids[slot]==1){
        uint32_t token=(uint32_t)little_read(data+raw+28,4);
        data=boot_access(c,c->tpidrro,60,READ|WRITE,pc);if(!data)return 0;
        memset(data,0,60);little_write(data+4,13,4);little_write(data+32,UINT32_C(0x4f434653),4);
        little_write(data+36,type==6?1:0,4);little_write(data+44,token,4);data[48]=BOOT_ACCOUNT_SWITCH_LOCK;
        c->x[0]=0;return 1;
    }
    if(parent_type==BOOT_SESSION_FSP+9 && command==160){
        uint8_t reply[56];
        if(!boot_account_init_plan(data,total,object,c->session_process_ids[slot],reply)){
            boot_fault(c,BOOT_SVC,pc,c->tpidrro,total);return 0;
        }
        /* Domain state records the binding to this pinned local application's
         * trusted process. No user selection or online authentication occurs. */
        c->domain_types[domain-1][object-1]=BOOT_SESSION_FSP+10;
        memcpy(data,reply,sizeof(reply));c->x[0]=0;return 1;
    }
    if(parent_type==ROM_STORAGE+1 && command==4 && !special && words==12 &&
       little_read(data+raw,4)==UINT64_C(0x100001) && c->storage_native){
        uint64_t size=((BootStorage*)(uintptr_t)c->storage_native)->size;
        uint32_t token=(uint32_t)little_read(data+raw+28,4);
        data=boot_access(c,c->tpidrro,64,READ|WRITE,pc);if(!data)return 0;
        memset(data,0,64);little_write(data+4,14,4);
        little_write(data+32,UINT32_C(0x4f434653),4);little_write(data+36,type==6?1:0,4);
        little_write(data+44,token,4);little_write(data+48,size,8);c->x[0]=0;return 1;
    }
    if(c->session_types[slot]==BOOT_SESSION_TIME && parent_type==BOOT_SESSION_TIME+1 && command<=4 && type==6 && !special && words==12 &&
       little_read(data+raw,4)==UINT64_C(0x100001) && object==1 &&
       c->session_process_ids[slot]==1){
        /* time:u IStaticService returns a distinct clock/timezone object for
         * commands 0..4. The objects' methods remain independently checked. */
        unsigned child=0;while(child<16 && c->domain_types[domain-1][child])++child;
        if(child==16){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
        uint32_t token=(uint32_t)little_read(data+raw+28,4);
        data=boot_access(c,c->tpidrro,60,READ|WRITE,pc);if(!data)return 0;
        c->domain_types[domain-1][child]=BOOT_SESSION_TIME+2+command;
        memset(data,0,60);little_write(data+4,13,4);little_write(data+16,1,4);
        little_write(data+32,UINT32_C(0x4f434653),4);little_write(data+36,1,4);
        little_write(data+44,token,4);little_write(data+48,child+1,4);
        c->x[0]=0;return 1;
    }
    if(c->session_types[slot]==BOOT_SESSION_TIME && parent_type==BOOT_SESSION_TIME+5 && command==101 && type==6 && !special && words==14 &&
       little_read(data+raw,4)==UINT64_C(0x180001) && c->session_process_ids[slot]==1){
        /* ITimeZoneService::ToCalendarTimeWithMyRule(PosixTime) -> CalendarTime, CalendarAdditionalInfo.
         * The device rule is UTC, so the host time zone never reaches the guest. Civil date from
         * days since the epoch (proleptic Gregorian, H. Hinnant's days_from_civil inverse). */
        int64_t t=(int64_t)little_read(data+raw+32,8);
        int64_t days=t>=0?t/86400:-((-t+86399)/86400),secs=t-days*86400;
        int64_t z=days+719468,era=(z>=0?z:z-146096)/146097,doe=z-era*146097;
        int64_t yoe=(doe-doe/1460+doe/36524-doe/146096)/365,y=yoe+era*400,doy=doe-(365*yoe+yoe/4-yoe/100);
        int64_t mp=(5*doy+2)/153,d=doy-(153*mp+2)/5+1,m=mp<10?mp+3:mp-9;y+=m<=2;
        int leap=(y%4==0 && y%100!=0) || y%400==0;
        static const int start[12]={0,31,59,90,120,151,181,212,243,273,304,334};
        int64_t yday=start[m-1]+d-1+(leap && m>2),wday=((days%7)+11)%7;   /* 1970-01-01 was a Thursday */
        if(y<INT16_MIN || y>INT16_MAX){boot_fault(c,BOOT_SVC,pc,c->tpidrro,64);return 0;}
        uint32_t token=(uint32_t)little_read(data+raw+28,4);
        data=boot_access(c,c->tpidrro,80,READ|WRITE,pc);if(!data)return 0;
        memset(data,0,80);little_write(data+4,20,4);
        little_write(data+32,UINT32_C(0x4f434653),4);little_write(data+36,1,4);little_write(data+44,token,4);
        little_write(data+48,(uint64_t)(uint16_t)y,2);data[50]=(uint8_t)m;data[51]=(uint8_t)d;
        data[52]=(uint8_t)(secs/3600);data[53]=(uint8_t)(secs/60%60);data[54]=(uint8_t)(secs%60);
        little_write(data+56,(uint64_t)wday,4);little_write(data+60,(uint64_t)yday,4);memcpy(data+64,"UTC",3);
        c->x[0]=0;return 1;
    }
    if(parent_type==BOOT_SESSION_FSP+1 || parent_type==FSP_READY+1){
        if(parent_type==FSP_READY+1 && command==51 && type==6 && !special && words==30 &&
           little_read(data+raw,4)==UINT32_C(0x580001) && object==1 &&
           c->session_process_ids[slot]==1 &&
           little_read(data+raw+32,8)==1 &&
           little_read(data+raw+40,8)==UINT64_C(0x01007ef00011e000) &&
           little_read(data+raw+48,8)==0 && little_read(data+raw+56,8)==0 &&
           little_read(data+raw+64,8)==0 && little_read(data+raw+72,8)==1 &&
           little_read(data+raw+80,8)==0 && little_read(data+raw+88,8)==0 &&
           little_read(data+raw+96,8)==0){
            DWORD attributes=GetFileAttributesW(L"startup\\save-data\\01007EF00011E000");
            DWORD error=attributes==INVALID_FILE_ATTRIBUTES?GetLastError():ERROR_SUCCESS;
            if(attributes!=INVALID_FILE_ATTRIBUTES ||
               (error!=ERROR_FILE_NOT_FOUND && error!=ERROR_PATH_NOT_FOUND)){
                boot_fault(c,BOOT_SVC,pc,0,0);return 0;
            }
            uint32_t token=(uint32_t)little_read(data+raw+28,4);
            memset(data,0,56);little_write(data+4,12,4);
            little_write(data+32,UINT32_C(0x4f434653),4);little_write(data+36,1,4);
            little_write(data+40,UINT32_C(0x7d402),4);little_write(data+44,token,4);
            c->x[0]=0;return 1;
        }
        if(parent_type==FSP_READY+1 && command==62 && type==6 && !special && words==12 &&
           little_read(data+raw,4)==UINT64_C(0x100001) && object==1 &&
           c->session_process_ids[slot]==1){
            /* OpenSaveDataInfoReaderOnlyCacheStorage. This host starts with
             * an empty cache-storage inventory; reads on the returned object
             * are checked independently before reporting any entries. */
            DWORD attributes=GetFileAttributesW(L"startup\\save-data\\cache");
            if(attributes==INVALID_FILE_ATTRIBUTES || !(attributes&FILE_ATTRIBUTE_DIRECTORY)){
                boot_fault(c,BOOT_SVC,pc,0,0);return 0;
            }
            unsigned child=0;while(child<16 && c->domain_types[domain-1][child])++child;
            if(child==16){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
            uint32_t token=(uint32_t)little_read(data+raw+28,4);
            data=boot_access(c,c->tpidrro,60,READ|WRITE,pc);if(!data)return 0;
            c->domain_types[domain-1][child]=SAVE_FS+3;
            memset(data,0,60);little_write(data+4,13,4);little_write(data+16,1,4);
            little_write(data+32,UINT32_C(0x4f434653),4);little_write(data+36,1,4);
            little_write(data+44,token,4);little_write(data+48,child+1,4);
            c->x[0]=0;return 1;
        }
        if(parent_type==FSP_READY+1 && command==51 && type==6 && !special && words==30 &&
           little_read(data+raw,4)==UINT64_C(0x580001) && object==1 &&
           c->session_process_ids[slot]==1 &&
           (little_read(data+raw+32,8)==1 || little_read(data+raw+32,8)==3) &&
           little_read(data+raw+40,8)==0 && /* Current application ID. */
           little_read(data+raw+48,8)==0 && little_read(data+raw+56,8)==0 && /* Common account UID. */
           little_read(data+raw+64,8)==0 && /* No system save-data ID. */
           ((little_read(data+raw+32,8)==1 &&
             (little_read(data+raw+72,8)==1 || little_read(data+raw+72,8)==5)) ||
            (little_read(data+raw+32,8)==3 && little_read(data+raw+72,8)==4)) &&
           little_read(data+raw+80,8)==0 && little_read(data+raw+88,8)==0 &&
           little_read(data+raw+96,8)==0){
            /* Each observed save type has its own host container. A cache
             * volume can be opened only after CreateCacheStorage made it. */
            int temporary=little_read(data+raw+32,8)==3;
            int cache=little_read(data+raw+72,8)==5;
            DWORD attributes=GetFileAttributesW(cache?
                L"startup\\save-data\\cache\\0100F2C0115B6000\\0":
                temporary?L"startup\\save-data\\0100F2C0115B6000\\temporary":
                L"startup\\save-data\\0100F2C0115B6000\\common");
            if(attributes==INVALID_FILE_ATTRIBUTES || !(attributes&FILE_ATTRIBUTE_DIRECTORY)){
                boot_fault(c,BOOT_SVC,pc,0,0);return 0;
            }
            if(cache){
                WIN32_FILE_ATTRIBUTE_DATA reserve;
                if(!GetFileAttributesExW(L"startup\\save-data\\cache-reserve-0100F2C0115B6000-0.bin",
                                         GetFileExInfoStandard,&reserve) ||
                   (reserve.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) ||
                   reserve.nFileSizeHigh!=0 ||
                   reserve.nFileSizeLow!=UINT32_C(0x33dc000)){
                    boot_fault(c,BOOT_SVC,pc,0,0);return 0;
                }
            }
            unsigned child=0;while(child<16 && c->domain_types[domain-1][child])++child;
            if(child==16){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
            uint32_t token=(uint32_t)little_read(data+raw+28,4);
            data=boot_access(c,c->tpidrro,60,READ|WRITE,pc);if(!data)return 0;
            c->domain_types[domain-1][child]=cache?SAVE_FS+4:SAVE_FS+1+temporary;
            memset(data,0,60);little_write(data+4,13,4);little_write(data+16,1,4);
            little_write(data+32,UINT32_C(0x4f434653),4);little_write(data+36,1,4);
            little_write(data+44,token,4);little_write(data+48,child+1,4);
            c->x[0]=0;return 1;
        }
        if(parent_type==FSP_READY+1 && command==200 && !special && words==12 &&
           little_read(data+raw,4)==UINT64_C(0x100001) && c->storage_native){
            unsigned child=0;while(child<16 && c->domain_types[domain-1][child])++child;
            if(child==16){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
            uint32_t token=(uint32_t)little_read(data+raw+28,4);
            data=boot_access(c,c->tpidrro,60,READ|WRITE,pc);if(!data)return 0;
            c->domain_types[domain-1][child]=ROM_STORAGE+1;
            memset(data,0,60);little_write(data+4,13,4);little_write(data+16,1,4);
            little_write(data+32,UINT32_C(0x4f434653),4);little_write(data+36,type==6?1:0,4);
            little_write(data+44,token,4);little_write(data+48,child+1,4);c->x[0]=0;return 1;
        }
        if(parent_type==FSP_READY+1 && command==33 && !special && words==13 &&
           little_read(data+raw,4)==UINT64_C(0x120001) && c->session_process_ids[slot]==1){
            /* IFileSystemProxy::DeleteCacheStorage(u16 index). A fresh save state (empty common
             * save) makes the game delete and re-create its cache storage. Only index 0 is ever
             * created (IApplicationFunctions::CreateCacheStorage): its host volume (directory +
             * reserve file) is moved to startup\save-data\deleted\<time>-cache-0 (recoverable), and
             * the cache list/open paths then report it absent. No volume: TargetNotFound. A volume
             * still open in a domain (SAVE_FS+4 object) is a stop, not a silent delete. */
            static const wchar_t volume_dir[]=L"startup\\save-data\\cache\\0100F2C0115B6000\\0";
            static const wchar_t reserve_path[]=L"startup\\save-data\\cache-reserve-0100F2C0115B6000-0.bin";
            uint32_t token=(uint32_t)little_read(data+raw+28,4),index=(uint32_t)little_read(data+raw+32,2);
            data=boot_access(c,c->tpidrro,56,READ|WRITE,pc);if(!data)return 0;
            /* Only fsp-srv domains: time:u object types overlap SAVE_FS+k numerically. */
            if(index==0)for(unsigned i=0;i<BOOT_SESSION_CAPACITY;++i){unsigned d=c->session_domains[i];
                if(!c->session_handles[i] || c->session_types[i]!=BOOT_SESSION_FSP || !d || d>16 || !c->domain_refs[d-1])continue;
                for(unsigned o=0;o<16;++o)if(c->domain_types[d-1][o]==SAVE_FS+4){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}}
            DWORD volume=index?INVALID_FILE_ATTRIBUTES:GetFileAttributesW(volume_dir);
            DWORD reserve=index?INVALID_FILE_ATTRIBUTES:GetFileAttributesW(reserve_path);
            uint32_t result=UINT32_C(0x7d402);   /* fs TargetNotFound */
            if(volume!=INVALID_FILE_ATTRIBUTES || reserve!=INVALID_FILE_ATTRIBUTES){
                wchar_t target[160],moved[220];SYSTEMTIME now;GetLocalTime(&now);
                swprintf(target,sizeof target/sizeof target[0],L"startup\\save-data\\deleted\\%04u%02u%02u-%02u%02u%02u-%03u-cache-0",
                         now.wYear,now.wMonth,now.wDay,now.wHour,now.wMinute,now.wSecond,now.wMilliseconds);
                CreateDirectoryW(L"startup\\save-data\\deleted",NULL);
                if(!CreateDirectoryW(target,NULL)){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
                if(volume!=INVALID_FILE_ATTRIBUTES){
                    swprintf(moved,sizeof moved/sizeof moved[0],L"%ls\\0",target);
                    if(!MoveFileExW(volume_dir,moved,0)){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
                }
                if(reserve!=INVALID_FILE_ATTRIBUTES){
                    swprintf(moved,sizeof moved/sizeof moved[0],L"%ls\\cache-reserve-0100F2C0115B6000-0.bin",target);
                    if(!MoveFileExW(reserve_path,moved,0)){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
                }
                result=0;
            }
            memset(data,0,56);little_write(data+4,12,4);
            little_write(data+32,UINT32_C(0x4f434653),4);little_write(data+36,type==6?1:0,4);
            little_write(data+40,result,4);little_write(data+44,token,4);c->x[0]=0;return 1;
        }
        if(parent_type==FSP_READY+1 && command==1005 && !special && words==12 &&
           little_read(data+raw,4)==UINT64_C(0x100001)){
            /* This read-only host does not write filesystem access logs. */
            uint32_t token=(uint32_t)little_read(data+raw+28,4);
            data=boot_access(c,c->tpidrro,60,READ|WRITE,pc);if(!data)return 0;
            memset(data,0,60);little_write(data+4,13,4);
            little_write(data+32,UINT32_C(0x4f434653),4);little_write(data+36,type==6?1:0,4);
            little_write(data+44,token,4);c->x[0]=0;return 1;
        }
        if(command==1 && special_header==1 && words==14 &&
           little_read(data+raw,4)==UINT64_C(0x180001) &&
           little_read(data+raw+32,8)==0 && c->session_process_ids[slot]==1){
            uint32_t token=(uint32_t)little_read(data+raw+28,4);
            data=boot_access(c,c->tpidrro,56,READ|WRITE,pc);if(!data)return 0;
            memset(data,0,56);little_write(data+4,12,4);
            little_write(data+32,UINT32_C(0x4f434653),4);little_write(data+36,type==6?1:0,4);
            little_write(data+44,token,4);c->domain_types[domain-1][object-1]=FSP_READY+1;
            c->x[0]=0;return 1;
        }
        boot_fault(c,BOOT_SVC,pc,c->tpidrro,total);return 0;
    }
    if(little_read(data+raw,4)==UINT64_C(0x140001) && !special && words==13 && command==66 &&
       parent_type==BOOT_SESSION_COMMON_STATE+1 && c->applet_resource_user_id){
        uint32_t token=(uint32_t)little_read(data+raw+28,4),mode=(uint32_t)little_read(data+raw+32,4);
        data=boot_access(c,c->tpidrro,56,READ|WRITE,pc);if(!data)return 0;
        /* TOTK_CORES>1: kernel work runs on whichever worker entered the kernel; boost every worker
         * thread (the window adapter accepts one execution thread). */
        if(boot_mt.active && c->window_native && !boot_window_is_closed((BootWindow*)(uintptr_t)c->window_native) && mode<=1)
            boot_mt_boost_workers(mode);
        else if(!c->window_native || boot_window_is_closed((BootWindow*)(uintptr_t)c->window_native) ||
           !boot_window_set_cpu_boost((BootWindow*)(uintptr_t)c->window_native,mode)){
            boot_fault(c,BOOT_SVC,pc,0,0);return 0;
        }
        memset(data,0,56);little_write(data+4,12,4);little_write(data+32,UINT64_C(0x4f434653),4);
        little_write(data+36,type==6?1:0,4);little_write(data+44,token,4);c->x[0]=0;return 1;
    }
    if(little_read(data+raw,4)==UINT64_C(0x100001) && !special && words==12 && command==1 &&
       (parent_type==BOOT_SESSION_PCTL_SERVICE+1 || parent_type==BOOT_SESSION_PCTL_READY+1)){
        /* Local client lifecycle only. No restriction/permission queries are
         * answered here; those require a separately implemented policy. */
        uint32_t token=(uint32_t)little_read(data+raw+28,4);
        data=boot_access(c,c->tpidrro,56,READ|WRITE,pc);if(!data)return 0;
        memset(data,0,56);little_write(data+4,12,4);little_write(data+32,UINT64_C(0x4f434653),4);
        little_write(data+36,type==6?1:0,4);little_write(data+44,token,4);
        c->domain_types[domain-1][object-1]=BOOT_SESSION_PCTL_READY+1;c->x[0]=0;return 1;
    }
    if(!special && words==12 && parent_type==BOOT_SESSION_SELF_CONTROL+1 && command==9 &&
       little_read(data+raw,4)==UINT64_C(0x100001) && c->session_process_ids[slot]==1){
        /* GetLibraryAppletLaunchableEvent: signalled from the start (no library applet is
         * running on this host), one shared manual-reset event (source 702). */
        unsigned event_slot=0;while(event_slot<16 && c->event_handles[event_slot])++event_slot;
        if(event_slot==16 || c->next_session_handle>=0x7ffffffe){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
        HANDLE event=NULL;int existing=0;
        for(unsigned i=0;i<16;++i)if(c->event_handles[i] && c->event_sources[i]==702){
            existing=1;DuplicateHandle(GetCurrentProcess(),(HANDLE)(uintptr_t)c->event_native_handles[i],GetCurrentProcess(),&event,0,FALSE,DUPLICATE_SAME_ACCESS);break;}
        if(!existing)event=CreateEventW(NULL,TRUE,TRUE,NULL);
        if(!event){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
        uint32_t token=(uint32_t)little_read(data+raw+28,4);
        data=boot_access(c,c->tpidrro,64,READ|WRITE,pc);if(!data){CloseHandle(event);return 0;}
        if(c->next_session_handle<0x10000)c->next_session_handle=0x10000;
        uint32_t handle=++c->next_session_handle;
        c->event_handles[event_slot]=handle;c->event_sources[event_slot]=702;
        c->event_native_handles[event_slot]=(uint64_t)(uintptr_t)event;++c->event_create_count;
        memset(data,0,64);little_write(data+4,UINT64_C(0x8000000c),4);little_write(data+8,2,4);little_write(data+12,handle,4);
        little_write(data+32,UINT64_C(0x4f434653),4);little_write(data+36,type==6?1:0,4);little_write(data+44,token,4);
        c->x[0]=0;return 1;
    }
    if(!special && (little_read(data+raw,4)&0xff)==1 && c->session_process_ids[slot]==1 &&
       ((parent_type==BOOT_SESSION_SELF_CONTROL+1 &&
         (command==1 || command==2 || command==3 || command==4 || command==10 || command==17 || command==18 || command==19 ||
          command==50 || command==62 || command==63 || command==65 || command==68 || command==69 || command==100)) ||
        (parent_type==BOOT_SESSION_APP_FUNCTIONS+1 &&
         (command==40 || command==67 || command==68 || command==90 || command==102 || command==1001)))){
        /* appletOE calls that only record a preference (command list per suyu am/service):
         * acknowledged. Getters: GetIdleTimeDetectionExtension (63) 0, IsAutoSleepDisabled (69)
         * false, NotifyRunning (40) false = not resumed from a suspension. */
        int getter=(parent_type==BOOT_SESSION_SELF_CONTROL+1 && (command==63 || command==69)) ||
                   (parent_type==BOOT_SESSION_APP_FUNCTIONS+1 && command==40);
        uint32_t token=(uint32_t)little_read(data+raw+28,4);
        data=boot_access(c,c->tpidrro,64,READ|WRITE,pc);if(!data)return 0;
        memset(data,0,64);little_write(data+4,getter?13:12,4);
        little_write(data+32,UINT64_C(0x4f434653),4);little_write(data+36,type==6?1:0,4);little_write(data+44,token,4);
        c->x[0]=0;return 1;
    }
    if(!special && words==12 && parent_type==BOOT_SESSION_SELF_CONTROL+1 && command==44 &&
       little_read(data+raw,4)==UINT64_C(0x100001)){
        uint32_t token=(uint32_t)little_read(data+raw+28,4);uint64_t ids[2];
        data=boot_access(c,c->tpidrro,72,READ|WRITE,pc);if(!data)return 0;
        if(c->session_process_ids[slot]!=1 || !c->applet_resource_user_id ||
           !boot_window_create_layers((BootWindow*)(uintptr_t)c->window_native,c->applet_resource_user_id,ids)){
            boot_fault(c,BOOT_SVC,pc,0,0);return 0;
        }
        memset(data,0,72);little_write(data+4,16,4);
        little_write(data+32,UINT32_C(0x4f434653),4);little_write(data+36,type==6?1:0,4);little_write(data+44,token,4);
        little_write(data+48,ids[0],8);little_write(data+56,ids[1],8);c->x[0]=0;return 1;
    }
    if(!special && words==13 && parent_type==BOOT_SESSION_SELF_CONTROL+1 &&
       (command==11 || command==12 || command==13 || command==14 || command==16) && little_read(data+raw,4)==(command==13?UINT64_C(0x130001):UINT64_C(0x110001))){
        uint32_t token=(uint32_t)little_read(data+raw+28,4);
        uint8_t values[3]={data[raw+32],data[raw+33],data[raw+34]};
        data=boot_access(c,c->tpidrro,56,READ|WRITE,pc);if(!data)return 0;
        BootWindow*w=(BootWindow*)(uintptr_t)c->window_native;
        if(!w || !((command==11 || command==12)?boot_window_set_mode_notification(w,command,values[0]):command==14?boot_window_set_resume_notifications(w,values[0]):boot_window_set_focus_policy(w,command,values))){
            boot_fault(c,BOOT_SVC,pc,0,0);return 0;
        }
        memset(data,0,56);little_write(data+4,12,4);little_write(data+32,UINT64_C(0x4f434653),4);
        little_write(data+36,type==6?1:0,4);little_write(data+44,token,4);c->x[0]=0;return 1;
    }
    if(little_read(data+raw,4)==UINT64_C(0x100001) && !special && words==12 &&
       ((parent_type==BOOT_SESSION_COMMON_STATE+1 && command==0) ||
        (parent_type==BOOT_SESSION_SELF_CONTROL+1 && command==91) ||
        (parent_type==BOOT_SESSION_APP_FUNCTIONS+1 && command==130)) && c->applet_resource_user_id){
        uint32_t token=(uint32_t)little_read(data+raw+28,4);
        data=boot_access(c,c->tpidrro,64,READ|WRITE,pc);if(!data)return 0;
        unsigned source=parent_type==BOOT_SESSION_COMMON_STATE+1?1:parent_type==BOOT_SESSION_SELF_CONTROL+1?0:2;
        uint32_t handle=boot_new_event(c,pc,source);if(!handle)return 0;
        memset(data,0,64);little_write(data+4,UINT64_C(0x8000000c),4);
        little_write(data+8,2,4);little_write(data+12,handle,4);
        little_write(data+32,UINT64_C(0x4f434653),4);little_write(data+36,type==6?1:0,4);
        little_write(data+44,token,4);c->x[0]=0;return 1;
    }
    if(little_read(data+raw,4)==UINT64_C(0x100001) && !special && words==12 &&
       parent_type==BOOT_SESSION_COMMON_STATE+1 && (command==1 || command==5 || command==6 || command==9) && c->applet_resource_user_id){
        if(!c->window_native && command!=1){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
        uint32_t token=(uint32_t)little_read(data+raw+28,4),value=0,result=0;
        data=boot_access(c,c->tpidrro,60,READ|WRITE,pc);if(!data)return 0;
        BootWindow*w=(BootWindow*)(uintptr_t)c->window_native;
        if(command==9)value=boot_window_get_focus(w);
        else if(command==1){int available=w?boot_window_receive(w,&value):0;
            if(available<0){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
            if(!available)result=UINT32_C(0x680);
        }
        memset(data,0,60);little_write(data+4,13,4);little_write(data+32,UINT64_C(0x4f434653),4);
        little_write(data+36,type==6?1:0,4);little_write(data+40,result,4);little_write(data+44,token,4);
        little_write(data+48,value,(command==5 || command==9)?1:4);c->x[0]=0;return 1;
    }
    if(little_read(data+raw,4)==UINT64_C(0x100001) && !special && words==12 &&
       parent_type==BOOT_SESSION_COMMON_STATE+1 && command==900 && c->applet_resource_user_id){
        uint32_t token=(uint32_t)little_read(data+raw+28,4);
        data=boot_access(c,c->tpidrro,56,READ|WRITE,pc);if(!data)return 0;
        memset(data,0,56);little_write(data+4,12,4);little_write(data+32,UINT64_C(0x4f434653),4);
        little_write(data+36,type==6?1:0,4);little_write(data+44,token,4);
        c->exit_to_library_applet_on_next_program=1;c->x[0]=0;return 1;
    }
    if(little_read(data+raw,4)==UINT64_C(0x100001) && !special && words==12 &&
       parent_type==BOOT_SESSION_WINDOW_CONTROL+1 && command==10 && c->applet_resource_user_id){
        uint32_t token=(uint32_t)little_read(data+raw+28,4);
        data=boot_access(c,c->tpidrro,56,READ|WRITE,pc);if(!data)return 0;
        if(!c->window_native || !boot_window_acquire_foreground((BootWindow*)(uintptr_t)c->window_native,c->applet_resource_user_id)){
            boot_fault(c,BOOT_SVC,pc,0,0);return 0;
        }
        memset(data,0,56);little_write(data+4,12,4);little_write(data+32,UINT64_C(0x4f434653),4);
        little_write(data+36,type==6?1:0,4);little_write(data+44,token,4);c->x[0]=0;return 1;
    }
    if(little_read(data+raw,4)==UINT64_C(0x100001) && !special && words==12 &&
       parent_type==BOOT_SESSION_WINDOW_CONTROL+1 && command==1 && c->applet_resource_user_id){
        uint32_t token=(uint32_t)little_read(data+raw+28,4);
        data=boot_access(c,c->tpidrro,64,READ|WRITE,pc);if(!data)return 0;
        memset(data,0,64);little_write(data+4,14,4);little_write(data+32,UINT64_C(0x4f434653),4);
        little_write(data+36,type==6?1:0,4);little_write(data+44,token,4);
        little_write(data+48,c->applet_resource_user_id,8);c->x[0]=0;return 1;
    }
    if(parent_type==BOOT_SESSION_APP_FUNCTIONS+1 && command==27 && type==6 && !special &&
       words==18 && little_read(data+raw,4)==UINT64_C(0x280001) && object==3 &&
       c->session_process_ids[slot]==1 && c->applet_resource_user_id &&
       little_read(data+raw+32,8)==0 && /* Cache index 0; padding zero. */
       little_read(data+raw+40,8)==UINT64_C(0x33b0000) &&
       little_read(data+raw+48,8)==UINT64_C(0x2c000) &&
       little_read(data+raw+56,8)==0){
        /* Reserve the observed cache volume on the host before exposing it to
         * the guest. Guest file operations on the volume remain separately
         * checked; a directory alone is not treated as implemented IFile IO. */
        static const wchar_t app_dir[]=L"startup\\save-data\\cache\\0100F2C0115B6000";
        static const wchar_t volume_dir[]=L"startup\\save-data\\cache\\0100F2C0115B6000\\0";
        static const wchar_t reserve_path[]=L"startup\\save-data\\cache-reserve-0100F2C0115B6000-0.bin";
        uint32_t token=(uint32_t)little_read(data+raw+28,4);
        data=boot_access(c,c->tpidrro,72,READ|WRITE,pc);if(!data)return 0;
        DWORD volume_attributes=GetFileAttributesW(volume_dir);
        DWORD reserve_attributes=GetFileAttributesW(reserve_path);
        if(volume_attributes!=INVALID_FILE_ATTRIBUTES || reserve_attributes!=INVALID_FILE_ATTRIBUTES){
            if(volume_attributes==INVALID_FILE_ATTRIBUTES || reserve_attributes==INVALID_FILE_ATTRIBUTES ||
               !(volume_attributes&FILE_ATTRIBUTE_DIRECTORY) ||
               (reserve_attributes&FILE_ATTRIBUTE_DIRECTORY)){
                boot_fault(c,BOOT_SVC,pc,0,0);return 0;
            }
            memset(data,0,56);little_write(data+4,12,4);
            little_write(data+32,UINT32_C(0x4f434653),4);little_write(data+36,1,4);
            little_write(data+40,UINT32_C(0x402),4);little_write(data+44,token,4);
            c->x[0]=0;return 1;
        }
        HANDLE reserve=CreateFileW(reserve_path,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
        if(reserve==INVALID_HANDLE_VALUE){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
        LARGE_INTEGER quota;quota.QuadPart=UINT64_C(0x33b0000)+UINT64_C(0x2c000);
        int reserved=SetFilePointerEx(reserve,quota,NULL,FILE_BEGIN) && SetEndOfFile(reserve) && FlushFileBuffers(reserve);
        CloseHandle(reserve);
        if(!reserved){DeleteFileW(reserve_path);boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
        DWORD app_attributes=GetFileAttributesW(app_dir);
        int made_app=0;
        if(app_attributes==INVALID_FILE_ATTRIBUTES){
            if(!CreateDirectoryW(app_dir,NULL)){
                DeleteFileW(reserve_path);boot_fault(c,BOOT_SVC,pc,0,0);return 0;
            }
            made_app=1;
        }else if(!(app_attributes&FILE_ATTRIBUTE_DIRECTORY)){
            DeleteFileW(reserve_path);boot_fault(c,BOOT_SVC,pc,0,0);return 0;
        }
        if(!CreateDirectoryW(volume_dir,NULL)){
            if(made_app)RemoveDirectoryW(app_dir);
            DeleteFileW(reserve_path);boot_fault(c,BOOT_SVC,pc,0,0);return 0;
        }
        memset(data,0,72);little_write(data+4,16,4);
        little_write(data+32,UINT32_C(0x4f434653),4);little_write(data+36,1,4);
        little_write(data+44,token,4);little_write(data+48,1,4); /* NAND target media. */
        c->x[0]=0;return 1;
    }
    if(little_read(data+raw,4)==UINT64_C(0x100001) && !special && words==12 &&
       parent_type==BOOT_SESSION_APP_FUNCTIONS+1 && command==21 && c->applet_resource_user_id && c->session_process_ids[slot]==1){
        uint32_t token=(uint32_t)little_read(data+raw+28,4);
        data=boot_access(c,c->tpidrro,64,READ|WRITE,pc);if(!data)return 0;
        memset(data,0,64);little_write(data+4,14,4);little_write(data+32,UINT64_C(0x4f434653),4);
        little_write(data+36,type==6?1:0,4);little_write(data+44,token,4);
        /* Pinned English startup profile; not the Windows system locale. */
        memcpy(data+48,"en-US",5);c->x[0]=0;return 1;
    }
    if(send_buffers==1 && special && special_header==1 && words==16 &&
       little_read(data+raw,4)==UINT64_C(0x200001) && parent_type==124+1 /* nfp IUser */ &&
       command==0 && c->session_process_ids[slot]==1){
        /* IUser::Initialize(ARUID, reserved, PID, A-buffer of MCU versions):
         * accepted; there is no NFC reader, so ListDevices reports none. */
        data=boot_access(c,c->tpidrro,64,READ|WRITE,pc);if(!data)return 0;
        uint32_t token=(uint32_t)little_read(data+raw+28,4);
        memset(data,0,64);little_write(data+4,12,4);
        little_write(data+32,UINT64_C(0x4f434653),4);little_write(data+36,1,4);little_write(data+44,token,4);
        nfp_initialized=1;c->x[0]=0;return 1;
    }
    if(little_read(data+raw,4)==UINT64_C(0x140001) && !special && words==13 && type==6 &&
       parent_type==BOOT_SESSION_APP_FUNCTIONS+1 && command==22 && c->applet_resource_user_id){
        /* SetTerminateResult(u32): recorded for diagnosis; the process continues. */
        data=boot_access(c,c->tpidrro,64,READ|WRITE,pc);if(!data)return 0;
        uint32_t token=(uint32_t)little_read(data+raw+28,4),result=(uint32_t)little_read(data+raw+32,4);
        FILE*log=fopen("terminate-result.txt","a");
        if(log){fprintf(log,"SetTerminateResult 0x%08x (module %u, description %u) at step %llu\n",
                        result,result&0x1ff,(result>>9)&0x1fff,(unsigned long long)c->steps);
                /* Guest frame-pointer backtrace of the caller (x29 chain). */
                fprintf(log,"  pc=0x%llx lr=0x%llx",(unsigned long long)pc,(unsigned long long)c->x[30]);
                uint64_t fp=c->x[29];
                for(unsigned depth=0;depth<24 && fp && !(fp&7);++depth){
                    uint8_t*frame=boot_access(c,fp,16,READ,pc);if(!frame){c->status=0;break;}
                    uint64_t next=little_read(frame,8),ret=little_read(frame+8,8);
                    fprintf(log," 0x%llx",(unsigned long long)ret);
                    if(next<=fp)break;fp=next;
                }
                fputc(10,log);fclose(log);}
        memset(data,0,64);little_write(data+4,12,4);
        little_write(data+32,UINT64_C(0x4f434653),4);little_write(data+36,1,4);
        little_write(data+44,token,4);c->x[0]=0;return 1;
    }
    if(little_read(data+raw,4)==UINT64_C(0x100001) && !special && words==12 && type==6 &&
       parent_type==124+1 /* nfp IUser */ && command==23 && c->session_process_ids[slot]==1){
        /* AttachAvailabilityChangeEvent: one shared manual-reset event (source 701; 700 is the time shared memory)
         * that is never signalled; no NFC reader ever becomes available here. */
        unsigned event_slot=0;while(event_slot<16 && c->event_handles[event_slot])++event_slot;
        if(event_slot==16 || c->next_session_handle>=0x7ffffffe){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
        HANDLE event=NULL;int existing=0;
        for(unsigned i=0;i<16;++i)if(c->event_handles[i] && c->event_sources[i]==701){
            existing=1;
            DuplicateHandle(GetCurrentProcess(),(HANDLE)(uintptr_t)c->event_native_handles[i],GetCurrentProcess(),&event,0,FALSE,DUPLICATE_SAME_ACCESS);
            break;
        }
        if(!existing)event=CreateEventW(NULL,TRUE,FALSE,NULL);
        if(!event){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
        data=boot_access(c,c->tpidrro,64,READ|WRITE,pc);if(!data){CloseHandle(event);return 0;}
        uint32_t token=(uint32_t)little_read(data+raw+28,4);
        if(c->next_session_handle<0x10000)c->next_session_handle=0x10000;
        uint32_t handle=++c->next_session_handle;
        c->event_handles[event_slot]=handle;c->event_sources[event_slot]=701;
        c->event_native_handles[event_slot]=(uint64_t)(uintptr_t)event;++c->event_create_count;
        memset(data,0,64);little_write(data+4,UINT64_C(0x8000000c),4);
        little_write(data+8,2,4);little_write(data+12,handle,4);
        little_write(data+32,UINT64_C(0x4f434653),4);little_write(data+36,1,4);
        little_write(data+44,token,4);c->x[0]=0;return 1;
    }
    if(little_read(data+raw,4)==UINT64_C(0x100001) && !special && words==12 && type==6 &&
       parent_type==124+1 /* nfp IUser */ && (command==1 || command==19) && c->session_process_ids[slot]==1){
        /* No NFC reader on this host: Finalize succeeds; GetState reports
         * Initialized (1) once Initialize has run, otherwise NonInitialized (0). */
        if(command==1)nfp_initialized=0;
        data=boot_access(c,c->tpidrro,64,READ|WRITE,pc);if(!data)return 0;
        uint32_t token=(uint32_t)little_read(data+raw+28,4);
        memset(data,0,64);little_write(data+4,command==19?13:12,4);
        little_write(data+32,UINT64_C(0x4f434653),4);little_write(data+36,1,4);little_write(data+44,token,4);
        if(command==19)little_write(data+48,nfp_initialized,4);
        c->x[0]=0;return 1;
    }
    if(little_read(data+raw,4)==UINT64_C(0x180001) && special_header==1 && words==14 && total>=raw+40 &&
       command==1 && parent_type==BOOT_SESSION_PCTL+1 &&
       little_read(data+raw+32,8)==0 && c->session_process_ids[slot]==1){
        child_type=BOOT_SESSION_PCTL_SERVICE;
    }else if(little_read(data+raw,4)==UINT64_C(0x180001) && special && words==14 && total>=raw+40 && command==0){
        if(parent_type==BOOT_SESSION_LM+1 && special_header==1)child_type=BOOT_SESSION_LOGGER;
        else if(parent_type==BOOT_SESSION_APPLET+1 && special_header==3 &&
                little_read(data+20,4)==UINT64_C(0xffff8001) && little_read(data+raw+32,8)==0 &&
                c->session_process_ids[slot]==1)child_type=BOOT_SESSION_APPLET_PROXY;
        else {boot_fault(c,BOOT_SVC,pc,c->tpidrro,total);return 0;}
    }else if(little_read(data+raw,4)==UINT64_C(0x100001) && !special && words==12 &&
             parent_type==122+1 /* nfp:user IUserManager */ && command==0 && type==6 &&
             c->session_process_ids[slot]==1){
        child_type=124; /* CreateUserInterface -> nfp IUser */
    }else if(little_read(data+raw,4)==UINT64_C(0x100001) && !special && words==12 &&
             parent_type==BOOT_SESSION_APPLET_PROXY+1){
        switch(command){
            case 20:child_type=BOOT_SESSION_APP_FUNCTIONS;break;
            case 11:child_type=BOOT_SESSION_LIBRARY_CREATOR;break;
            case 0:child_type=BOOT_SESSION_COMMON_STATE;break;
            case 1:child_type=BOOT_SESSION_SELF_CONTROL;break;
            case 2:child_type=BOOT_SESSION_WINDOW_CONTROL;break;
            case 3:child_type=BOOT_SESSION_AUDIO_CONTROL;break;
            case 4:child_type=BOOT_SESSION_DISPLAY_CONTROL;break;
            case 1000:child_type=BOOT_SESSION_DEBUG_FUNCTIONS;break;
            default:boot_fault(c,BOOT_SVC,pc,c->tpidrro,total);return 0;
        }
    }
    else {boot_fault(c,BOOT_SVC,pc,c->tpidrro,total);return 0;}
    data=boot_access(c,c->tpidrro,60,READ|WRITE,pc);if(!data)return 0;
    unsigned child=0;while(child<16 && c->domain_types[domain-1][child])++child;
    if(child==16){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
    uint32_t token=(uint32_t)little_read(data+raw+28,4);
    c->domain_types[domain-1][child]=child_type+1;
    memset(data,0,60);little_write(data+4,13,4);little_write(data+16,1,4);
    little_write(data+32,UINT64_C(0x4f434653),4);little_write(data+36,type==6?1:0,4);
    little_write(data+44,token,4);little_write(data+48,child+1,4);
    if(child_type==BOOT_SESSION_LOGGER)++c->logger_open_count;
    else if(child_type==BOOT_SESSION_APPLET_PROXY){
        ++c->applet_proxy_open_count;
        /* One application in this laboratory host; retained for its lifetime. */
        if(!c->applet_resource_user_id)c->applet_resource_user_id=1;
    }
    c->x[0]=0;return 1;
}
static inline DWORD WINAPI boot_audio_completion_worker(void* opaque){
    HANDLE*event=(HANDLE*)opaque;
    Sleep(5);
    SetEvent(*event);
    CloseHandle(*event);
    free(event);
    return 0;
}
static inline int boot_schedule_audio_completion(HANDLE source){
    if(boot_guest_dilated()){
        /* The renderer completes one 5 ms audio frame of guest time later (guest_clock.h). */
        HANDLE event=NULL;
        if(!DuplicateHandle(GetCurrentProcess(),source,GetCurrentProcess(),&event,0,FALSE,DUPLICATE_SAME_ACCESS))return 0;
        if(!boot_guest_timer_add(event,BOOT_GUEST_AUDIO_TICKS)){CloseHandle(event);return 0;}
        return 1;
    }
    HANDLE*copy=(HANDLE*)malloc(sizeof(*copy));if(!copy)return 0;
    if(!DuplicateHandle(GetCurrentProcess(),source,GetCurrentProcess(),copy,0,FALSE,DUPLICATE_SAME_ACCESS)){
        free(copy);return 0;
    }
    if(!QueueUserWorkItem(boot_audio_completion_worker,copy,WT_EXECUTEDEFAULT)){
        CloseHandle(*copy);free(copy);return 0;
    }
    return 1;
}
/* Host gamepads (SDL 3, runtime_support only: TOTK_DRAW_PATH keeps it out of the generated page header).
 * host_input_sdl.h: snapshot type, weak shim API (third_party/build_sdl.py links host_input_sdl.c + libSDL3.a),
 * six-axis LIFO and vibration helpers. A DLL linked without the shim keeps the keyboard. */
#ifdef TOTK_DRAW_PATH
#include "host_input_sdl.h"
#endif
/* Npad backend: once the game has activated npad and set its supported style set, a
     * wired Pro Controller (Fullkey; Handheld if Fullkey is unsupported) is connected and
     * sampled, following suyu NPad::InitNewlyAddedController / OnUpdate and the
     * SharedMemoryFormat layout (npad entries at 0x9a00, 0x5000 each; NpadCondition at
     * 0x3e200). With every style tag zero the game re-runs ShowControllerSupport forever
     * and never leaves the loading screen. Keyboard (window focused): K/J/I/U = A/B/X/Y,
     * Q/E = L/R, 1/3 = ZL/ZR, Enter/Tab = Plus/Minus, arrows = d-pad, WASD = left stick,
     * numpad 8/4/5/6 = right stick. TOTK_NO_CONTROLLER=1 keeps the disconnected view.
     * Host gamepad (TOTK_DRAW_PATH builds, TOTK_INPUT=sdl default, =keyboard to disable): any SDL gamepad
     * mapped by position to the Pro Controller (input-work/README.md), merged with the keyboard (buttons
     * OR'ed, a non-zero keyboard stick wins), works without window focus; its gyro/accel feed the
     * six-axis LIFO at 200 Hz while the game has a six-axis sensor started (at-rest default otherwise). */
/* Sampling runs on the guest clock (guest_time.h: every clock update, dilated or wall mode), at most
 * once per millisecond of guest time (wall time in legacy mode), independent of IPC traffic: before,
 * it piggy-backed on SendSyncRequest and stopped with it. Several host threads (TOTK_CORES>1) may
 * update the clock; one wins the millisecond. */
static inline void boot_hid_npad_tick(BootContext*c,uint64_t now){
    BootWindow*hw=(BootWindow*)(uintptr_t)c->window_native;
    static volatile LONG64 pad_tick=-1;static int pad_disabled=-1;
    if(pad_disabled<0)pad_disabled=getenv("TOTK_NO_CONTROLLER")!=NULL;
    if(pad_disabled || !hw || hw->closed || !hw->hid_memory.data || !hw->hid_memory.npad_active || !(hw->hid_memory.npad_styles&3))return;
    LONG64 last=pad_tick;
    if((uint64_t)last==now || InterlockedCompareExchange64(&pad_tick,(LONG64)now,last)!=last)return;
    uint8_t*shm=hw->hid_memory.data;int fullkey=(hw->hid_memory.npad_styles&1)!=0;
    uint8_t*e=shm+0x9a00+(fullkey?0:8)*0x5000;
    if(!little_read(e,4)){   /* connect (InitNewlyAddedController) */
        little_write(e,fullkey?1:2,4);little_write(e+4,0,4);                       /* style tag, Dual assignment */
        little_write(e+8,0,4);little_write(e+12,UINT32_C(0xff2d2d2d),4);little_write(e+16,UINT32_C(0xffe6e6e6),4);
        little_write(e+20,fullkey?2:0,4);                                         /* joycon colour attribute */
        if(!fullkey){little_write(e+24,UINT32_C(0xff828282),4);little_write(e+28,UINT32_C(0xff0f0f0f),4);
            little_write(e+32,UINT32_C(0xff828282),4);little_write(e+36,UINT32_C(0xff0f0f0f),4);}
        for(unsigned l=0;l<7;++l){uint8_t*lifo=e+0x28+l*0x350;little_write(lifo+8,17,8);}
        for(unsigned l=0;l<6;++l){uint8_t*lifo=e+0x1758+l*0x708;little_write(lifo+8,17,8);}
        little_write(e+0x4188,fullkey?1:0xc,4);                                   /* device type */
        little_write(e+0x4190,(1u<<11)|(1u<<13)|(1u<<14)|(fullkey?0:(1u<<15)),8); /* vertical, +, -, (d-pad) */
        little_write(e+0x419c,4,4);                                               /* battery dual: Full */
        e[0x41ac]=fullkey?12:4;                                                   /* footer: Pro Controller / handheld L+R */
        uint8_t*cond=shm+0x3e200;little_write(cond+4,1,4);little_write(cond+8,1,4);little_write(cond+12,1,4);
    }
    uint64_t buttons=0;int32_t lx=0,ly=0,rx=0,ry=0;
    HWND fg=GetForegroundWindow();DWORD fpid=0;if(fg)GetWindowThreadProcessId(fg,&fpid);
    if(fpid==GetCurrentProcessId()){
        static const struct {int key;unsigned bit;} map[]={{'K',0},{'J',1},{'I',2},{'U',3},{'Q',6},{'E',7},{'1',8},{'3',9},
            {VK_RETURN,10},{VK_TAB,11},{VK_LEFT,12},{VK_UP,13},{VK_RIGHT,14},{VK_DOWN,15}};
        for(unsigned i=0;i<sizeof map/sizeof map[0];++i)if(GetAsyncKeyState(map[i].key)&0x8000)buttons|=UINT64_C(1)<<map[i].bit;
        if(GetAsyncKeyState('A')&0x8000)lx-=32767;if(GetAsyncKeyState('D')&0x8000)lx+=32767;
        if(GetAsyncKeyState('W')&0x8000)ly+=32767;if(GetAsyncKeyState('S')&0x8000)ly-=32767;
        if(GetAsyncKeyState(VK_NUMPAD4)&0x8000)rx-=32767;if(GetAsyncKeyState(VK_NUMPAD6)&0x8000)rx+=32767;
        if(GetAsyncKeyState(VK_NUMPAD8)&0x8000)ry+=32767;if(GetAsyncKeyState(VK_NUMPAD5)&0x8000)ry-=32767;
    }
#ifdef TOTK_DRAW_PATH
    TotkPadState pad;int pad_live=0;memset(&pad,0,sizeof pad);
    if(totk_input_sdl_start && totk_input_sdl_read && totk_input_sdl_start() && totk_input_sdl_read(&pad)){
        pad_live=1;buttons|=pad.buttons;
        if(!lx && !ly){lx=pad.lx;ly=pad.ly;}
        if(!rx && !ry){rx=pad.rx;ry=pad.ry;}
    }
#endif
    /* stick direction buttons at half deflection (keyboard sticks are 0 or full) */
    if(lx<=-16384)buttons|=1u<<16;if(ly>=16384)buttons|=1u<<17;if(lx>=16384)buttons|=1u<<18;if(ly<=-16384)buttons|=1u<<19;
    if(rx<=-16384)buttons|=1u<<20;if(ry>=16384)buttons|=1u<<21;if(rx>=16384)buttons|=1u<<22;if(ry<=-16384)buttons|=1u<<23;
    uint32_t attributes=fullkey?3:(1|2|4|8|16|32);       /* connected+wired (+left/right connected+wired) */
    /* WriteNextEntry on the style's LIFO (fullkey 0 / handheld 1) and system_ext (6) */
    unsigned lifos[2]={fullkey?0u:1u,6u};
    for(unsigned k=0;k<2;++k){uint8_t*lifo=e+0x28+lifos[k]*0x350;
        uint64_t tail=little_read(lifo+16,8),count=little_read(lifo+24,8);
        uint64_t sampling=little_read(lifo+32+tail*0x30+8,8)+1;
        if(count<16)little_write(lifo+24,count+1,8);
        tail=(tail+1)%17;uint8_t*slot_entry=lifo+32+tail*0x30;
        little_write(slot_entry,sampling<<1,8);little_write(slot_entry+8,sampling,8);little_write(slot_entry+16,buttons,8);
        little_write(slot_entry+24,(uint32_t)lx,4);little_write(slot_entry+28,(uint32_t)ly,4);
        little_write(slot_entry+32,(uint32_t)rx,4);little_write(slot_entry+36,(uint32_t)ry,4);
        little_write(slot_entry+40,attributes,4);little_write(slot_entry+44,0,4);
        little_write(lifo+16,tail,8);little_write(lifo,(uint64_t)now,8);
    }
#ifdef TOTK_DRAW_PATH
    {   /* six-axis (fullkey LIFO; handheld LIFO for the handheld entry, as suyu SixAxis::OnUpdate) at the
         * hardware's 200 Hz, so the 17-entry LIFO spans 85 ms of samples */
        static uint64_t sixaxis_last;
        if(!sixaxis_last || now<sixaxis_last || now-sixaxis_last>=5){
            uint64_t delta=sixaxis_last && now>sixaxis_last && now-sixaxis_last<100?(now-sixaxis_last)*1000000:5000000;
            sixaxis_last=now;
            totk_input_sixaxis_write(TOTK_SIXAXIS_LIFO(e,fullkey?0:1),now,delta,&pad,pad_live && pad.motion && totk_input_sixaxis_started);
        }
    }
#endif
}
#ifdef TOTK_DRAW_PATH
/* Packs the guest regions address space `as_fd` maps for the GPU into GPU arenas, so D3D12 opens one
 * heap per arena (the renderer's region table holds BOOT_RENDER_MAX_REGIONS). Used by the GPU channel
 * submit path and by native NVN submissions (host_nvn_exec.h). Returns 0 if no arena could be made.
 *
 * TOTK_CORES>1. Guest memory of MapPhysicalMemory regions comes from the GPU pool (host_gpu_draw_exec.h),
 * so it is GPU-shareable where it lives and is never moved here. Regions that still need a move (memory
 * from before the pool, checkpoint-restored, pool exhausted) are moved only while every other core is
 * parked outside guest code (boot_mt_quiesce_begin); if they cannot be parked in time the move is skipped
 * for this submission (those mappings stay unshared) instead of racing their lock-free accesses.
 * A native NVN submission runs on a core context (a non-kernel host function), whose region table is a
 * stale snapshot: boot_mt_gpu_pack (boot_mt_late.h) takes the kernel lock, packs the master's regions and
 * refreshes the core's copy, which the caller then resolves its mappings with. */
static int boot_gpu_pack_regions_locked(BootContext*c,BootWindow*w,uint32_t as_fd);
static inline int boot_mt_gpu_pack(BootContext*c,BootWindow*w,uint32_t as_fd);
static inline int boot_mt_quiesce_begin(void);
static inline void boot_mt_quiesce_end(void);
/* t2-A: steady-state fast path. A pack's outcome is a function of (1) the guest CPU address of every GPU mapping of
 * as_fd, (2) the region table (base, size, data, reserved, memory types, count), (3) GPU arena membership of region
 * data. When the last full pack of the same context and as_fd had nothing to move (the steady state) and none of
 * these changed since, a full pack has nothing to move again, so the call returns at once:
 *   (1) compared by value, one pass over the mapping table (every writer of w->gpu_mappings holds w->lock, and so
 *       does every caller of the pack: the native NVN submit and the GPU channel submit);
 *   (2) the region generation boot_gpu_pack_rgen(): boot_tlb.flushes (every change of regions, memory types, aliases
 *       or transfers calls boot_tlb_flush() in the same kernel-locked section, and boot_valid flushes at every run
 *       entry, which covers checkpoint restores and host-side table edits) plus boot_fm.adopted (fastmem adoption
 *       moves region data without a flush). Both only grow;
 *   (3) boot_gpu_arena_generation (bumped whenever an arena is created or released).
 * A core context (boot_mt_gpu_pack) also skips the kernel lock and its region-table refresh when its copy was taken
 * at the current region generation. TOTK_PACK_VERIFY=1 runs the full pack on every call and logs (draw-path.log,
 * "gpu pack verify: MISMATCH") every call a fast path would have skipped wrongly and every difference between the
 * indexed region lookup and the original linear scan. */
typedef struct BootGpuPackCache {
    const BootContext*c;uint32_t as_fd;int valid;uint64_t rgen,arena_generation;
    uint64_t cpu[BOOT_GPU_MAPPING_CAPACITY];   /* per mapping slot: cpu of a mapping of as_fd, else 0 */
} BootGpuPackCache;
static BootGpuPackCache boot_gpu_pack_cache;
typedef struct BootGpuPackStats { uint64_t skipped_unlocked,skipped_locked,full,moves,index_builds,full_qpc,verify_mismatches; } BootGpuPackStats;
static BootGpuPackStats boot_gpu_pack_stats;   /* updated under w->lock; nvn native periodic line */
static int boot_gpu_pack_last_work;            /* the last full pack had regions to move */
static inline uint64_t boot_gpu_pack_rgen(void){
    uint64_t g=__atomic_load_n(&boot_tlb.flushes,__ATOMIC_ACQUIRE);
#if TOTK_FASTMEM
    g+=__atomic_load_n(&boot_fm.adopted,__ATOMIC_ACQUIRE);
#endif
    return g;
}
static inline int boot_gpu_pack_verify(void){
    static int mode=-1;
    if(mode<0){char v[8];DWORD n=GetEnvironmentVariableA("TOTK_PACK_VERIFY",v,sizeof v);mode=n>0 && n<sizeof v && v[0]=='1';}
    return mode;
}
/* Caller holds w->lock. 1: a full pack of (c, as_fd) now has nothing to move (see above). */
static inline int boot_gpu_pack_unchanged(const BootContext*c,const BootWindow*w,uint32_t as_fd,uint64_t rgen){
    const BootGpuPackCache*k=&boot_gpu_pack_cache;
    if(!k->valid || k->c!=c || k->as_fd!=as_fd || k->rgen!=rgen ||
       k->arena_generation!=__atomic_load_n(&boot_gpu_arena_generation,__ATOMIC_ACQUIRE))return 0;
    for(unsigned i=0;i<BOOT_GPU_MAPPING_CAPACITY;++i){const BootGpuMapping*m=&w->gpu_mappings.entries[i];
        if((m->size && m->as_fd==as_fd?m->cpu:0)!=k->cpu[i])return 0;}
    return 1;
}
static inline void boot_gpu_pack_mismatch(const char*what,uint64_t a,uint64_t b){
    if(boot_gpu_pack_stats.verify_mismatches++<64)boot_draw_log("gpu pack verify: MISMATCH %s %llu %llu",what,a,b);
}
/* Region index sorted by base for the mapping -> region lookup, rebuilt when the region generation, the context or
 * the count changes. Regions never overlap (boot_valid; every service that adds or grows one checks), so at most one
 * region contains an address and the binary search returns exactly what the original first-match linear scan
 * returned. A table that does overlap disables the index for that generation (linear scans as before). */
typedef struct BootGpuPackKey { uint64_t base;uint32_t index,pad; } BootGpuPackKey;
static struct { const BootContext*c;uint64_t rgen;unsigned count,usable;BootGpuPackKey key[BOOT_REGION_CAPACITY],tmp[BOOT_REGION_CAPACITY]; } boot_gpu_pack_index;
static inline int boot_gpu_pack_index_ready(const BootContext*c,uint64_t rgen){
    if(boot_gpu_pack_index.c==c && boot_gpu_pack_index.rgen==rgen && boot_gpu_pack_index.count==c->region_count)return (int)boot_gpu_pack_index.usable;
    unsigned n=c->region_count<BOOT_REGION_CAPACITY?c->region_count:BOOT_REGION_CAPACITY;
    BootGpuPackKey*a=boot_gpu_pack_index.key,*t=boot_gpu_pack_index.tmp;
    for(unsigned i=0;i<n;++i){a[i].base=c->regions[i].base;a[i].index=i;a[i].pad=0;}
    /* LSD radix sort (stable), 11-bit digits; a digit all keys share (low page bits, high bits) costs no pass */
    static uint32_t hist[6][2048];memset(hist,0,sizeof hist);
    for(unsigned i=0;i<n;++i)for(unsigned d=0;d<6;++d)++hist[d][(a[i].base>>(11*d))&2047];
    for(unsigned d=0;d<6;++d){uint32_t*h=hist[d];
        if(n && h[(a[0].base>>(11*d))&2047]==n)continue;
        uint32_t sum=0;for(unsigned v=0;v<2048;++v){uint32_t x=h[v];h[v]=sum;sum+=x;}
        for(unsigned i=0;i<n;++i)t[h[(a[i].base>>(11*d))&2047]++]=a[i];
        BootGpuPackKey*s=a;a=t;t=s;
    }
    if(a!=boot_gpu_pack_index.key)memcpy(boot_gpu_pack_index.key,a,n*sizeof(BootGpuPackKey));
    unsigned usable=1;   /* sorted by base: pairwise disjoint iff every region ends at or before the next one starts */
    for(unsigned i=1;i<n && usable;++i){const Region*p=&c->regions[boot_gpu_pack_index.key[i-1].index];
        usable=p->size<=boot_gpu_pack_index.key[i].base-p->base;}
    boot_gpu_pack_index.c=c;boot_gpu_pack_index.rgen=rgen;boot_gpu_pack_index.count=n;boot_gpu_pack_index.usable=usable;
    ++boot_gpu_pack_stats.index_builds;
    return (int)usable;
}
static inline unsigned boot_gpu_pack_find(const BootContext*c,uint64_t cpu){   /* region containing cpu, else c->region_count */
    const BootGpuPackKey*a=boot_gpu_pack_index.key;unsigned lo=0,hi=boot_gpu_pack_index.count;
    while(lo<hi){unsigned mid=lo+(hi-lo)/2;if(a[mid].base<=cpu)lo=mid+1;else hi=mid;}   /* lo: first base above cpu */
    if(!lo)return c->region_count;
    const Region*r=&c->regions[a[lo-1].index];
    return cpu-r->base<r->size?a[lo-1].index:c->region_count;
}
/* The regions the mappings at cpu[0..n) use that a pack may move (physical, type 5), in guest-address order, so
 * guest-adjacent regions packed together stay adjacent. picked: all zero on entry and on return. */
static inline unsigned boot_gpu_pack_picks_linear(const BootContext*c,const uint64_t*cpu,unsigned n,uint32_t*picks,uint8_t*picked){
    unsigned count=0;   /* the original lookup: first region in table order containing the address */
    for(unsigned i=0;i<n;++i)for(unsigned k=0;k<c->region_count;++k){const Region*r=&c->regions[k];
        if(cpu[i]<r->base || cpu[i]-r->base>=r->size)continue;
        if(r->reserved==1 && c->memory_types[k]==5 && !picked[k]){picked[k]=1;picks[count++]=k;}
        break;
    }
    for(unsigned a=1;a<count;++a){uint32_t v=picks[a];unsigned b=a;
        while(b && c->regions[picks[b-1]].base>c->regions[v].base){picks[b]=picks[b-1];--b;}picks[b]=v;}
    for(unsigned t=0;t<count;++t)picked[picks[t]]=0;
    return count;
}
static inline unsigned boot_gpu_pack_picks_indexed(const BootContext*c,const uint64_t*cpu,unsigned n,uint32_t*picks,uint8_t*picked){
    unsigned marked=0,count=0;   /* index ready and usable */
    for(unsigned i=0;i<n;++i){unsigned k=boot_gpu_pack_find(c,cpu[i]);
        if(k<c->region_count && c->regions[k].reserved==1 && c->memory_types[k]==5 && !picked[k]){picked[k]=1;++marked;}}
    for(unsigned j=0;j<boot_gpu_pack_index.count && count<marked;++j){unsigned k=boot_gpu_pack_index.key[j].index;
        if(picked[k]){picked[k]=0;picks[count++]=k;}}
    return count;
}
static int boot_gpu_pack_regions(BootContext*c,BootWindow*w,uint32_t as_fd){
    if(boot_mt.active && boot_mt_core_ctx(c))return boot_mt_gpu_pack(c,w,as_fd);
    return boot_gpu_pack_regions_locked(c,w,as_fd);
}
static int boot_gpu_pack_regions_locked(BootContext*c,BootWindow*w,uint32_t as_fd){
    /* D3D12 opens guest memory zero-copy only from whole committed VirtualAlloc blocks.
     * MapPhysicalMemory regions come from calloc: move each one this channel maps into
     * such a block (aliases into it follow). Moved blocks are never freed while a
     * D3D12 heap may reference them; see boot_physical_memory/boot_release_memory. */
    /* Physical regions this channel maps. Every one lives in a GPU arena
     * (host_gpu_draw_exec.h) so D3D12 opens one heap per arena, not per region: new
     * ones are packed together into a fresh arena; when a submission spans more than
     * 64 blocks, all of its regions are repacked into one. Host placement is
     * independent of guest addresses. Arenas no region uses any more are released. */
    static uint32_t picks[BOOT_REGION_CAPACITY];static uint8_t picked[BOOT_REGION_CAPACITY];
    static uint8_t*blocks[BOOT_REGION_CAPACITY];
    static uint64_t cpus[BOOT_GPU_MAPPING_CAPACITY];
    /* t2-A: nothing changed since a full pack found nothing to move: nothing to move now (see boot_gpu_pack_unchanged) */
    const int verify=boot_gpu_pack_verify();const uint64_t rgen=boot_gpu_pack_rgen();
    const int unchanged=boot_gpu_pack_unchanged(c,w,as_fd,rgen);
    if(unchanged && !verify){++boot_gpu_pack_stats.skipped_locked;return 1;}
    LARGE_INTEGER pack_t0;QueryPerformanceCounter(&pack_t0);
    ++boot_gpu_pack_stats.full;
    BootGpuPackCache*cache=&boot_gpu_pack_cache;cache->valid=0;
    unsigned cpu_count=0;
    for(unsigned i=0;i<BOOT_GPU_MAPPING_CAPACITY;++i){
        const BootGpuMapping*m=&w->gpu_mappings.entries[i];
        cache->cpu[i]=m->size && m->as_fd==as_fd?m->cpu:0;
        if(!m->size || m->as_fd!=as_fd || !m->cpu)continue;
        cpus[cpu_count++]=m->cpu;
    }
    /* t2-A: binary search in the sorted region index (was a linear scan of every region per mapping) */
    unsigned block_count=0,loose=0;
    const int indexed=cpu_count>8 && boot_gpu_pack_index_ready(c,rgen);
    unsigned pick_count=indexed?boot_gpu_pack_picks_indexed(c,cpus,cpu_count,picks,picked):boot_gpu_pack_picks_linear(c,cpus,cpu_count,picks,picked);
    if(verify && indexed){static uint32_t linear[BOOT_REGION_CAPACITY];
        unsigned n=boot_gpu_pack_picks_linear(c,cpus,cpu_count,linear,picked);
        if(n!=pick_count || memcmp(linear,picks,n*sizeof(uint32_t))){
            boot_gpu_pack_mismatch("indexed region lookup differs from the linear scan (indexed picks, linear picks)",pick_count,n);
            memcpy(picks,linear,n*sizeof(uint32_t));pick_count=n;}   /* the original result is used */
    }
    for(unsigned t=0;t<pick_count;++t){uint8_t*data=c->regions[picks[t]].data,*block=boot_gpu_arena_base(data);
        if(!block && boot_draw_is_alloc_base(data))block=data;
        if(!block){++loose;continue;}
        unsigned b=0;while(b<block_count && blocks[b]!=block)++b;
        if(b==block_count)blocks[block_count++]=block;
    }
    int repack=block_count+(loose?1:0)>64;
    uint64_t total=0;
    for(unsigned t=0;t<pick_count;++t){Region*r=&c->regions[picks[t]];
        if(boot_gpu_pool_find(r->data))continue;   /* pool memory is never moved */
        if(repack || (!boot_gpu_arena_base(r->data) && !boot_draw_is_alloc_base(r->data)))total+=(r->size+4095)&~UINT64_C(4095);}
    {LARGE_INTEGER pack_t1;QueryPerformanceCounter(&pack_t1);boot_gpu_pack_stats.full_qpc+=(uint64_t)(pack_t1.QuadPart-pack_t0.QuadPart);}
    boot_gpu_pack_last_work=total!=0;
    if(verify && unchanged && total)boot_gpu_pack_mismatch("generation check said unchanged but the full pack has regions to move (bytes, picks)",total,pick_count);
    if(!total){   /* t2-A: steady state: remember it (the mapping cpus were stored by the scan above) */
        cache->c=c;cache->as_fd=as_fd;cache->rgen=boot_gpu_pack_rgen();cache->arena_generation=boot_gpu_arena_generation;cache->valid=1;
        return 1;
    }
    ++boot_gpu_pack_stats.moves;
    {
        /* TOTK_CORES>1: other cores access these regions lock-free; move only while they are parked */
        if(boot_mt.active && !boot_mt_quiesce_begin()){
            static unsigned skipped;if(skipped++<16)boot_draw_log("gpu pack: move of %s skipped (cores not parked) %llu %llu","regions",total,pick_count);
            return 1;}
        uint8_t*block=boot_gpu_arena_new(total);
        if(!block){if(boot_mt.active)boot_mt_quiesce_end();return 0;}
        uint64_t at=BOOT_GPU_ARENA_HEADER;
        static uint8_t*released[BOOT_REGION_CAPACITY];unsigned released_count=0;
#if TOTK_FASTMEM
        static uint8_t*owners[BOOT_REGION_CAPACITY];unsigned owner_count=0;   /* sections that pieces moved out of */
#endif
        for(unsigned t=0;block && t<pick_count;++t){Region*r=&c->regions[picks[t]];uint8_t*old=r->data;
            if(boot_gpu_pool_find(old))continue;
            int in_arena=boot_gpu_arena_base(old)!=NULL,standalone=!in_arena && boot_draw_is_alloc_base(old);
            if(!repack && (in_arena || standalone))continue;
            uint8_t*moved=block+at;at+=(r->size+4095)&~UINT64_C(4095);
            memcpy(moved,old,(size_t)r->size);
            for(unsigned j=0;j<c->region_count;++j){Region*d=&c->regions[j];
                if(d->reserved==2 && d->data>=old && d->data<old+r->size)d->data=moved+(d->data-old);}
            r->data=moved;boot_tlb_flush();
            if(in_arena){uint8_t*a=boot_gpu_arena_base(old);unsigned q=0;while(q<released_count && released[q]!=a)++q;
                if(q==released_count && released_count<BOOT_REGION_CAPACITY)released[released_count++]=a;}
#if TOTK_FASTMEM
            else if(standalone && boot_fm_owns(old)){boot_render_forget(w->render,old);boot_gpu_arena_forget();boot_fm_section_free(old);}
            else if(boot_fm_owns(old)){   /* interior of a section (an unmap split): released below if unused */
                uint8_t*owner=boot_fm.allocs[boot_fm_alloc_find(old)].host;unsigned q=0;
                while(q<owner_count && owners[q]!=owner)++q;
                if(q==owner_count && owner_count<BOOT_REGION_CAPACITY)owners[owner_count++]=owner;}
#endif
            else if(standalone){boot_render_forget(w->render,old);boot_gpu_arena_forget();VirtualFree(old,0,MEM_RELEASE);}
            else free(old);
        }
#if TOTK_FASTMEM
        for(unsigned q=0;q<owner_count;++q){int index=boot_fm_alloc_find(owners[q]);if(index<0)continue;
            uint8_t*owner=owners[q];uint64_t owner_size=boot_fm.allocs[index].size;int used=0;
            for(unsigned j=0;j<c->region_count && !used;++j)used=c->regions[j].data>=owner && c->regions[j].data<owner+owner_size;
            if(!used){boot_render_forget(w->render,owner);boot_gpu_arena_forget();boot_fm_section_free(owner);}
        }
#endif
        /* An older arena is released once no region (or alias) points into it. */
        for(unsigned q=0;q<released_count;++q){uint8_t*a=released[q];uint64_t payload;memcpy(&payload,a+16,8);
            int used=0;for(unsigned j=0;j<c->region_count && !used;++j)
                used=c->regions[j].data>=a && c->regions[j].data<a+BOOT_GPU_ARENA_HEADER+payload;
            if(!used){boot_render_forget(w->render,a);boot_gpu_arena_forget();
#if TOTK_FASTMEM
                if(!boot_fm_section_free(a))
#endif
                VirtualFree(a,0,MEM_RELEASE);}
        }
        c->exclusive_valid=0;
        if(boot_mt.active)boot_mt_quiesce_end();
    }
    return 1;
}
#endif
/* nvmap and nvhost-as mapping ioctls on the guest's nvdrv file descriptors (ioctl codes and request layouts of
 * the guest NVIDIA kernel driver). Called by the nvdrv IPC handler below (boot_send_sync) and in-process by
 * host_sdk_nvdrv.h (NvOsDrvIoctl), so both reach the same window state. in / out: the request and result bytes
 * (they may be the same buffer). Returns 1 when done, 2 when the guest driver itself answers the request with
 * the NvError in *fa (nvmap requests on an unknown handle, exact mode; nothing changed, out untouched), 0 when
 * the request is invalid (*fa / *fb: fault details; c->status may already be set), -1 after a guest memory
 * access fault. */
#define BOOT_NV_BAD_VALUE UINT32_C(0xb)
#define BOOT_NV_ACCESS_DENIED UINT32_C(0x30010)
/* Return address saved in the guest frame at x29 (the caller of the function making this request), read
 * without faulting; 0 when x29 does not point into mapped guest memory. */
static inline uint64_t boot_guest_frame_return(const BootContext*c){
    uint64_t fp=c->x[29];if(!fp || (fp&7))return 0;
    for(unsigned r=0;r<c->region_count;++r){const Region*g=&c->regions[r];
        if(g->data && fp>=g->base && fp-g->base+16<=g->size)return little_read(g->data+(size_t)(fp-g->base)+8,8);}
    return 0;
}
/* nvmap operation trail entry (host_nvmap.h; nvdrv-reject.log). w->lock held exclusively. */
static inline void boot_nvmap_note(BootContext*c,BootWindow*w,uint32_t handle,uint32_t fd,uint32_t op,uint32_t result,uint32_t value){
    if(!handle)return;
    const BootNvMapObject*o=boot_nvmap_find_any(&w->nvmap,handle);
    BootNvMapEvent e={c->steps,c->x[30],boot_guest_frame_return(c),c->current_thread,fd,op,result,o?o->refs:0,value};
    boot_nvmap_trail_add(handle,&e);
}
/* Retained nvmap objects (handle freed while still mapped or displayed) go once nothing uses them. */
static inline void boot_nvmap_reap(BootContext*c,BootWindow*w){
    for(unsigned i=0;i<w->nvmap.capacity;++i){BootNvMapObject*o=&w->nvmap.objects[i];
        if(!o->handle || !boot_nvmap_retained(o) || boot_gpu_mapping_object_used(&w->gpu_mappings,o->owner_fd,o->handle) ||
           boot_graphic_object_used(&w->graphic_buffers,o->owner_fd,o->handle))continue;
        uint32_t handle=o->handle,owner=o->owner_fd;memset(o,0,sizeof(*o));boot_nvmap_note(c,w,handle,owner,BOOT_NVMAP_OP_DROP,0,0);
    }
}
static int boot_nvdrv_mapping_ioctl(BootContext*c,uint64_t pc,uint32_t fd,uint32_t ioctl,const uint8_t*in,uint8_t*out,uint64_t*fa,uint64_t*fb){
    BootWindow*w=(BootWindow*)(uintptr_t)c->window_native;*fa=*fb=0;
    if(!w || !fd)return 0;
    if(ioctl==UINT32_C(0xc0284106) || ioctl==UINT32_C(0xc0084105)){
        int unmapping=ioctl==UINT32_C(0xc0084105);unsigned length=unmapping?8:40;
                uint8_t reply[40]={0};memcpy(reply,in,length);uint64_t gpu=0,diagnostic=0;
                AcquireSRWLockExclusive(&w->lock);unsigned device=0;while(device<16 && w->nv_device_fds[device]!=fd)++device;
                int valid=!w->closed && !w->error && device<16 && w->nv_device_kinds[device]==3 && w->nv_address_spaces[device].initialized;
                if(valid && unmapping){valid=boot_gpu_mapping_remove(&w->gpu_mappings,&w->nv_address_spaces[device],fd,little_read(reply,8));
                    if(valid && boot_nvmap_exact())boot_nvmap_reap(c,w);}
                else if(valid && little_read(reply,4)==UINT32_C(0x100)){
                    /* MapBufferEx remaps a subrange of its existing GPU
                     * allocation. The observed request carries no new NVMap
                     * handle; preserve its registered CPU backing. */
                    gpu=little_read(reply+32,8);
                    valid=little_read(reply+8,8)==0 && little_read(reply+16,8)==0 &&
                          boot_gpu_mapping_rebind_subrange(&w->gpu_mappings,&w->nv_address_spaces[device],fd,
                              gpu,little_read(reply+24,8),(uint32_t)little_read(reply+4,4));
                    if(!valid){
                        diagnostic=1;
                        for(unsigned j=0;j<BOOT_GPU_MAPPING_CAPACITY;++j){const BootGpuMapping*r=&w->gpu_mappings.entries[j];
                            if(r->size && r->as_fd==fd && gpu>=r->gpu && gpu-r->gpu<r->size){
                                diagnostic=(r->size<<32)|boot_gpu_mapping_backing_size(r);
                                break;
                            }
                        }
                    }
                }
                else if(valid){
                    uint32_t handle=(uint32_t)little_read(reply+8,4),page=(uint32_t)little_read(reply+12,4);BootNvMapObject*object=NULL;
                    object=boot_nvmap_find_handle(&w->nvmap,handle);
                    valid=object && object->allocated && (page==0 || page==4096 || page==w->nv_address_spaces[device].page_size);
                    uint64_t offset=little_read(reply+16,8),size=little_read(reply+24,8),cpu=0,backed=0;
                    if(valid){valid=offset<=object->size;if(!size && valid)size=object->size-offset;
                        if(valid)backed=size<object->size-offset?size:object->size-offset;
                        valid=valid && size && backed && object->address<=UINT64_MAX-offset-backed;
                        if(valid)cpu=object->address+offset;}
                    if(valid){unsigned region=0;for(;region<c->region_count;++region){Region*r=&c->regions[region];
                            if((r->reserved==1 || r->reserved==3) && c->memory_types[region]==5 && r->permissions==(READ|WRITE) && cpu>=r->base && backed<=r->size && cpu-r->base<=r->size-backed)break;}
                        valid=region<c->region_count;}
                    if(valid && !boot_access(c,cpu,backed,READ,pc)){ReleaseSRWLockExclusive(&w->lock);return -1;}
                    if(valid)valid=boot_gpu_mapping_add(&w->gpu_mappings,&w->nv_address_spaces[device],fd,object,
                        (uint32_t)little_read(reply,4),(uint32_t)little_read(reply+4,4),offset,size,little_read(reply+32,8),&gpu);
                    boot_nvmap_note(c,w,handle,fd,BOOT_NVMAP_OP_MAP,valid?0:UINT32_MAX,(uint32_t)(gpu>>12));
                }
                ReleaseSRWLockExclusive(&w->lock);if(!valid){*fa=gpu;*fb=diagnostic;return 0;}
                if(!unmapping){if(little_read(reply+4,4)==UINT32_MAX)little_write(reply+4,0,4);little_write(reply+32,gpu,8);}memcpy(out,reply,length);
        return 1;
    }
    if(ioctl==UINT32_C(0xc008010e) || ioctl==UINT32_C(0xc0080101) || ioctl==UINT32_C(0xc0180105) || ioctl==UINT32_C(0xc0200104) || ioctl==UINT32_C(0xc00c0109) || ioctl==UINT32_C(0xc0080103)){
        int identifying=ioctl==UINT32_C(0xc008010e),importing=ioctl==UINT32_C(0xc0080103);
        int freeing=ioctl==UINT32_C(0xc0180105),allocating=ioctl==UINT32_C(0xc0200104);int querying=ioctl==UINT32_C(0xc00c0109);unsigned length=querying?12:allocating?32:freeing?24:8;
        /* Exact nvmap semantics (host_nvmap.h; off: TOTK_NVMAP_LEGACY=1): one handle table for all nvmap fds,
         * reference-counted handles (FROM_ID adds, FREE drops; a handle freed while mapped / displayed keeps its
         * object until unused), and a request on an unknown handle gets the driver's own answer (returned 2) */
        int exact=boot_nvmap_exact();if(importing && !exact)return 0;
        if((freeing || allocating) && little_read(in+4,4)!=0)return 0;
                uint32_t value=(uint32_t)little_read(in,4),handle=0;uint8_t reply[32]={0};little_write(reply,value,4);BootNvMapObject freed={0};
                uint32_t subject=identifying?(uint32_t)little_read(in+4,4):value,error=0,outcome=0;int driver=0;
                if(allocating || querying || identifying || importing)memcpy(reply,in,length);
                AcquireSRWLockExclusive(&w->lock);unsigned device=0;while(device<16 && w->nv_device_fds[device]!=fd)++device;
                int valid=!w->closed && !w->error && device<16 && w->nv_device_kinds[device]==2;
                if(valid){
                    if(identifying){
                        BootNvMapObject*object=boot_nvmap_lookup(&w->nvmap,fd,subject);valid=object!=NULL;
                        /* IDs name the window's monotonically allocated objects;
                         * exporting identity neither copies nor completes data. */
                        if(valid)little_write(reply,object->handle,4);
                        else if(exact){driver=1;error=subject?BOOT_NV_ACCESS_DENIED:BOOT_NV_BAD_VALUE;}
                    }else if(importing){
                        handle=boot_nvmap_import(&w->nvmap,fd,value);valid=handle!=0;
                        if(valid)little_write(reply+4,handle,4);
                        else if(!boot_nvmap_lookup(&w->nvmap,fd,value)){driver=1;error=BOOT_NV_BAD_VALUE;}
                    }else if(querying){
                        BootNvMapObject*object=boot_nvmap_lookup(&w->nvmap,fd,value);uint32_t parameter=(uint32_t)little_read(reply+4,4);
                        valid=object && (parameter==1 || (object->allocated && (parameter==2 || parameter==5)));
                        if(valid)little_write(reply+8,parameter==1?object->size:parameter==2?object->alignment:object->kind,4);
                        else if(exact && !object){driver=1;error=BOOT_NV_BAD_VALUE;}
                    }else if(allocating){
                        BootNvMapObject*object=boot_nvmap_lookup(&w->nvmap,fd,value);uint32_t flags=(uint32_t)little_read(reply+8,4),alignment=(uint32_t)little_read(reply+12,4);
                        uint64_t address=little_read(reply+24,8);
                        valid=object && !object->allocated && flags<=1 && alignment>=4096 && !(alignment&(alignment-1)) &&
                              little_read(reply+16,8)==0 && address && !(address&4095) && address<=UINT64_MAX-object->size;
                        if(valid && !boot_access(c,address,object->size,flags?READ|WRITE:READ,pc)){ReleaseSRWLockExclusive(&w->lock);return -1;}
                        if(valid){object->address=address;object->alignment=alignment;object->flags=flags;object->kind=0;object->allocated=1;}
                        else if(exact && !object){driver=1;error=BOOT_NV_BAD_VALUE;}
                    }else if(freeing && exact){
                        /* an unknown handle is a successful no-op for the driver (outputs untouched) */
                        BootNvMapObject*object=boot_nvmap_lookup(&w->nvmap,fd,value);
                        if(!object){valid=0;driver=1;error=0;}
                        else outcome=boot_nvmap_release(object,boot_graphic_object_used(&w->graphic_buffers,object->owner_fd,value) ||
                                                               boot_gpu_mapping_object_used(&w->gpu_mappings,object->owner_fd,value),&freed);
                    }else if(freeing){valid=!boot_graphic_object_used(&w->graphic_buffers,fd,value) && !boot_gpu_mapping_object_used(&w->gpu_mappings,fd,value) && boot_nvmap_free(&w->nvmap,fd,value,&freed);outcome=valid?2:0;}
                    else {if(exact)boot_nvmap_reap(c,w);handle=boot_nvmap_create(&w->nvmap,fd,value);valid=handle!=0;}
                }
                {uint32_t op=identifying?BOOT_NVMAP_OP_GET_ID:importing?BOOT_NVMAP_OP_FROM_ID:querying?BOOT_NVMAP_OP_PARAM:allocating?BOOT_NVMAP_OP_ALLOC:freeing?BOOT_NVMAP_OP_FREE:BOOT_NVMAP_OP_CREATE;
                 boot_nvmap_note(c,w,op==BOOT_NVMAP_OP_CREATE?handle:subject,fd,op,valid?0:driver?error:UINT32_MAX,
                     op==BOOT_NVMAP_OP_CREATE?value:op==BOOT_NVMAP_OP_PARAM?(uint32_t)little_read(in+4,4):op==BOOT_NVMAP_OP_ALLOC?(uint32_t)little_read(in+8,4):outcome);}
                ReleaseSRWLockExclusive(&w->lock);if(!valid){if(driver){*fa=error;return 2;}return 0;}
                if(freeing){if(outcome==1)little_write(reply+20,1,4);else little_write(reply+8,freed.address,8);little_write(reply+16,freed.size,4);}
                else if(!allocating && !querying && !identifying && !importing)little_write(reply+4,handle,4);
                memcpy(out,reply,length);
        return 1;
    }
    if((ioctl&UINT32_C(0xc000ffff))==UINT32_C(0xc0004114)){
        uint32_t length=(ioctl>>16)&0x3fff;if(!length || length%20 || length>10240)return 0;
                AcquireSRWLockExclusive(&w->lock);unsigned device=0;while(device<16 && w->nv_device_fds[device]!=fd)++device;
                int valid=!w->closed && !w->error && device<16 && w->nv_device_kinds[device]==3;
                static BootGpuMappings next;next=w->gpu_mappings;   /* static: 1 MiB, w->lock is held exclusively */
                for(unsigned off=0;valid && off<length;off+=20){const uint8_t*entry=in+off;
                    uint32_t flags=(uint32_t)little_read(entry,2),kind=(uint32_t)little_read(entry+2,2),handle=(uint32_t)little_read(entry+4,4);
                    uint64_t offset=little_read(entry+8,4)<<16,gpu=little_read(entry+12,4)<<16,size=little_read(entry+16,4)<<16;
                    BootNvMapObject*object=NULL;
                    if(handle){object=boot_nvmap_find_handle(&w->nvmap,handle);
                        valid=object && object->allocated && offset<=object->size && size && size<=object->size-offset && object->address<=UINT64_MAX-offset-size;
                        if(valid){uint64_t cpu=object->address+offset;unsigned region=0;
                            for(;region<c->region_count;++region){Region*r=&c->regions[region];if((r->reserved==1 || r->reserved==3) && c->memory_types[region]==5 && r->permissions==(READ|WRITE) && cpu>=r->base && size<=r->size && cpu-r->base<=r->size-size)break;}
                            valid=region<c->region_count && boot_access(c,cpu,size,READ,pc)!=NULL;
                        }
                    }
                    if(valid)valid=boot_gpu_mapping_remap(&next,&w->nv_address_spaces[device],fd,object,flags,kind,offset,gpu,size);
                }
                if(valid){w->gpu_mappings=next;if(boot_nvmap_exact())boot_nvmap_reap(c,w);}ReleaseSRWLockExclusive(&w->lock);
                if(!valid)return 0;
                memmove(out,in,length);
        return 1;
    }
    if(ioctl==UINT32_C(0xc0184102) || ioctl==UINT32_C(0xc0104103)){
        int freeing=ioctl==UINT32_C(0xc0104103);unsigned length=freeing?16:24;
                uint8_t reply[24]={0};memcpy(reply,in,length);uint64_t offset=0;
                if(!freeing && little_read(reply+12,4)!=0)return 0;
                AcquireSRWLockExclusive(&w->lock);unsigned device=0;while(device<16 && w->nv_device_fds[device]!=fd)++device;
                int valid=!w->closed && !w->error && device<16 && w->nv_device_kinds[device]==3;
                if(valid && freeing){uint64_t begin=little_read(reply,8),size=little_read(reply+8,4)*little_read(reply+12,4);
                    valid=begin<=UINT64_MAX-size && !boot_gpu_mapping_va_overlap(&w->gpu_mappings,fd,begin,begin+size);}
                if(valid)valid=freeing?boot_gpu_va_release(&w->nv_address_spaces[device],little_read(reply,8),(uint32_t)little_read(reply+8,4),(uint32_t)little_read(reply+12,4)):
                    boot_gpu_va_reserve(&w->nv_address_spaces[device],(uint32_t)little_read(reply,4),(uint32_t)little_read(reply+4,4),(uint32_t)little_read(reply+8,4),little_read(reply+16,8),&offset);
                ReleaseSRWLockExclusive(&w->lock);if(!valid)return 0;
                if(!freeing)little_write(reply+16,offset,8);memcpy(out,reply,length);
        return 1;
    }
    return 0;
}
static inline int boot_send_sync(BootContext*c,uint64_t pc){
    {   /* IPC trace (diagnostics, off by default): creating frames\ipc-trace.request (optionally
         * holding a request count, default 4000) logs that many SendSyncRequests to
         * frames\ipc-trace.txt: guest thread, fp backtrace, session slot/type, domain object type,
         * command, svc result, CMIF result and the first X/A buffer as text (paths). The
         * request is served by a nested call of this handler, so its behaviour is unchanged. */
        static unsigned trace_left,trace_depth;static ULONGLONG trace_poll;static FILE*trace;
        if(!trace_depth){
            if(!trace_left){ULONGLONG now=GetTickCount64();
                if(now-trace_poll>=1000){trace_poll=now;
                    if(GetFileAttributesA("frames\\ipc-trace.request")!=INVALID_FILE_ATTRIBUTES){
                        unsigned n=4000;FILE*q=fopen("frames\\ipc-trace.request","r");
                        if(q){if(fscanf(q,"%u",&n)!=1)n=4000;fclose(q);}
                        DeleteFileA("frames\\ipc-trace.request");
                        trace=fopen("frames\\ipc-trace.txt","w");if(trace && n)trace_left=n;}}}
            const uint8_t*src=trace_left?boot_access(c,c->tpidrro,0x100,READ,pc):NULL;
            if(src){
                uint8_t in[0x100];memcpy(in,src,sizeof in);
                uint64_t handle=c->x[0];unsigned thread=c->current_thread;
                unsigned slot=0;while(slot<BOOT_SESSION_CAPACITY && (!c->session_handles[slot] || c->session_handles[slot]!=handle))++slot;
                unsigned stype=slot<BOOT_SESSION_CAPACITY?c->session_types[slot]:999,domain=slot<BOOT_SESSION_CAPACITY?c->session_domains[slot]:0;
                uint32_t w0=(uint32_t)little_read(in,4),w1=(uint32_t)little_read(in+4,4);
                unsigned xs=(w0>>16)&15,as=(w0>>20)&15,bs=(w0>>24)&15,ws=(w0>>28)&15,off=8;
                if(w1>>31){uint32_t hw=(uint32_t)little_read(in+8,4);off+=4+((hw&1)?8:0)+4*(((hw>>1)&15)+((hw>>5)&15));}
                unsigned xoff=off,aoff=off+xs*8;off+=xs*8+(as+bs+ws)*12;unsigned raw=(off+15)&~15u;
                unsigned magic=0;for(unsigned o=raw;o+16<=sizeof in;o+=16)if(little_read(in+o,4)==UINT32_C(0x49434653)){magic=o;break;}
                uint32_t cmd=magic?(uint32_t)little_read(in+magic+8,4):0xffffffffu,object=0,otype=0,dcmd=0;
                if(domain && raw+8<=sizeof in){dcmd=in[raw];object=(uint32_t)little_read(in+raw+4,4);
                    if(object && object<=16)otype=c->domain_types[domain-1][object-1];}
                char text[100];text[0]=0;
                for(unsigned k=0;k<2 && !text[0];++k){uint64_t address=0,size=0;
                    if(k==0 && xs){uint32_t d0=(uint32_t)little_read(in+xoff,4);size=d0>>16;
                        address=little_read(in+xoff+4,4)|((uint64_t)((d0>>12)&15)<<32)|((uint64_t)((d0>>6)&7)<<36);}
                    if(k==1 && as){const uint8_t*d=in+aoff;uint32_t d2=(uint32_t)little_read(d+8,4);
                        size=little_read(d,4)|((uint64_t)((d2>>24)&15)<<32);
                        address=little_read(d+4,4)|((uint64_t)(d2>>28)<<32)|((uint64_t)((d2>>2)&7)<<36);}
                    if(!size || !address)continue;
                    for(unsigned r=0;r<c->region_count;++r){Region*g=&c->regions[r];
                        if(g->data && address>=g->base && address-g->base<g->size){
                            uint64_t avail=g->size-(address-g->base);unsigned n=0;const uint8_t*p=g->data+(size_t)(address-g->base);
                            while(n<sizeof text-1 && n<size && n<avail && p[n]>=32 && p[n]<127){text[n]=(char)p[n];++n;}
                            text[n<2?0:n]=0;break;}}}
                char bt[80];unsigned bl=0;bt[0]=0;
                {uint64_t fp=c->x[29];for(unsigned depth=0;depth<4 && fp && !(fp&7);++depth){const uint8_t*frame=NULL;
                    for(unsigned r=0;r<c->region_count;++r){Region*g=&c->regions[r];
                        if(g->data && fp>=g->base && fp-g->base+16<=g->size){frame=g->data+(size_t)(fp-g->base);break;}}
                    if(!frame)break;uint64_t next=little_read(frame,8),ret=little_read(frame+8,8);
                    bl+=(unsigned)snprintf(bt+bl,sizeof bt-bl,"%llx,",(unsigned long long)ret);if(bl>=sizeof bt-1 || next<=fp)break;fp=next;}}
                uint64_t steps=c->steps,tid=thread<c->thread_count?c->threads[thread].id:0;
                trace_depth=1;int rc=boot_send_sync(c,pc);trace_depth=0;
                uint32_t result=0xffffffffu,ow0=0,ow1=0;
                const uint8_t*out=rc?boot_access(c,c->tpidrro,0x100,READ,pc):NULL;
                if(out){ow0=(uint32_t)little_read(out,4);ow1=(uint32_t)little_read(out+4,4);
                    for(unsigned o=16;o+16<=0x100;o+=16)if(little_read(out+o,4)==UINT32_C(0x4f434653)){result=(uint32_t)little_read(out+o+8,4);break;}}
                if(trace){fprintf(trace,"%llu t=%llu h=%llx slot=%u st=%u dom=%u dc=%u obj=%u ot=%u type=%x cmd=%d rc=%d x0=%llx res=%x ow=%x/%x%s bt=%s str=\"%s\"\n",
                    (unsigned long long)steps,(unsigned long long)tid,(unsigned long long)handle,slot,stype,domain,dcmd,object,otype,w0&0xffff,(int)cmd,rc,
                    (unsigned long long)c->x[0],result,ow0,ow1,c->current_thread!=thread?" SWITCHED":"",bt,text);
                    if(--trace_left==0){fclose(trace);trace=NULL;}else if(!(trace_left&63))fflush(trace);}
                else trace_left=0;
                return rc;
            }
        }
    }
    {   /* diagnostics: guest thread table every 30 s of wall time (presents may stall) */
        static ULONGLONG last_dump;ULONGLONG now=GetTickCount64();
        /* A window procedure from an older DLL may have reported desktop deactivation as
         * OutOfFocus; restore InFocus (FocusStateChanged) unless focus testing is requested. */
        BootWindow*fw=(BootWindow*)(uintptr_t)c->window_native;
        if(fw && fw->focus==3 && !getenv("TOTK_FOCUS_FOLLOWS_WINDOW")){
            AcquireSRWLockExclusive(&fw->lock);
            if(fw->focus==3 && !fw->closed && fw->count<32){fw->focus=1;fw->messages[(fw->head+fw->count)%32]=15;++fw->count;SetEvent(fw->notification);}
            ReleaseSRWLockExclusive(&fw->lock);
        }
        if(now-last_dump>=30000){last_dump=now;FILE*f=fopen("threads-periodic.txt","w");
            if(f){fprintf(f,"steps %llu current %u switches %u\n",(unsigned long long)c->steps,c->current_thread,c->thread_switch_count);
                for(unsigned i=0;i<c->thread_count;++i){BootThread*t=&c->threads[i];
                    fprintf(f,"%2u h=%x id=%llu state=%u prio=%u core=%u pc=%llx sp=%llx wait_addr=%llx owner=%x waits=%u cancel=%u runnable=%u active=%u last=%llu lr=%llx\n",
                            i,t->handle,(unsigned long long)t->id,t->state,t->priority,t->core,(unsigned long long)t->pc,(unsigned long long)t->sp,
                            (unsigned long long)t->wait_address,t->wait_owner,t->wait_count,t->wait_cancel,t->schedule.runnable,t->schedule.active,
                            (unsigned long long)t->schedule.last_scheduled,(unsigned long long)t->x[30]);
                    for(unsigned k=0;k<t->wait_count && k<4;++k)fprintf(f,"     wait[%u] native=%llx thread=%x\n",k,(unsigned long long)t->wait_native[k],t->wait_threads[k]);
                    /* guest frame-pointer backtrace (x29 chain), read without faulting */
                    fprintf(f,"     bt");uint64_t fp=i==c->current_thread?c->x[29]:t->x[29];
                    for(unsigned depth=0;depth<16 && fp && !(fp&7);++depth){const uint8_t*frame=NULL;
                        for(unsigned r=0;r<c->region_count;++r){Region*g=&c->regions[r];
                            if(g->data && fp>=g->base && fp-g->base+16<=g->size){frame=g->data+(size_t)(fp-g->base);break;}}
                        if(!frame)break;uint64_t next=little_read(frame,8),ret=little_read(frame+8,8);
                        fprintf(f," %llx",(unsigned long long)ret);if(next<=fp)break;fp=next;}
                    fputc(10,f);
                }
                fclose(f);}}
    }
    unsigned slot=0;while(slot<BOOT_SESSION_CAPACITY && (!c->session_handles[slot] || c->session_handles[slot]!=c->x[0]))++slot;
    if(slot==BOOT_SESSION_CAPACITY){boot_fault(c,BOOT_SVC,pc,c->x[0],0);return 0;}
    if(c->session_types[slot]==BOOT_SESSION_CLOSED){c->x[0]=UINT32_C(0xf601);return 1;}
    uint8_t*request=boot_access(c,c->tpidrro,8,READ|WRITE,pc);if(!request)return 0;
    if(c->session_types[slot]!=BOOT_SESSION_SM){
        unsigned type=(unsigned)little_read(request,4);
        if(c->session_types[slot]==BOOT_SESSION_FSP+13 && type==6 && little_read(request+4,4)==UINT32_C(0x8000001a)){
            /* audren:u command 0, OpenAudioRenderer. The two copy handles are
             * the work-buffer transfer memory and the current process. Retain
             * the transfer while the resulting renderer session is open. */
            request=boot_access(c,c->tpidrro,128,READ|WRITE,pc);if(!request)return 0;
            uint32_t transfer_handle=(uint32_t)little_read(request+20,4);
            unsigned index=0;while(index<16 && (!c->transfers[index].handle || c->transfers[index].owner_closed || c->transfers[index].handle!=transfer_handle))++index;
            uint32_t token=(uint32_t)little_read(request+44,4);
            if(c->session_domains[slot] || c->session_process_ids[slot]!=1 ||
               little_read(request+8,4)!=5 || little_read(request+12,8)==0 ||
               little_read(request+24,4)!=UINT32_C(0xffff8001) ||
               little_read(request+32,4)!=UINT32_C(0x49434653) ||
               little_read(request+36,4)!=1 || little_read(request+40,4)!=0 ||
               little_read(request+48,4)!=48000 || little_read(request+52,4)!=240 ||
               little_read(request+56,4)!=72 || little_read(request+60,4)!=11 ||
               little_read(request+64,4)!=96 || little_read(request+68,4)!=2 ||
               little_read(request+72,4)!=16 || little_read(request+76,4)!=0 ||
               little_read(request+80,4)!=1 || little_read(request+84,4)!=96 ||
               little_read(request+88,4)!=384 || little_read(request+92,4)!=0 ||
               little_read(request+96,4)!=UINT32_C(0x3b564552) ||
               little_read(request+104,8)!=UINT64_C(0x2b1000) ||
               !c->applet_resource_user_id || little_read(request+112,8)!=c->applet_resource_user_id ||
               index==16 || c->transfers[index].size!=UINT64_C(0x2b1000) ||
               c->transfers[index].permissions!=0){
                boot_fault(c,BOOT_SVC,pc,c->tpidrro,128);return 0;
            }
            uint32_t handle=boot_new_session(c,pc,BOOT_SESSION_FSP+14,c->session_process_ids[slot]);if(!handle)return 0;
            unsigned renderer=0;while(c->session_handles[renderer]!=handle)++renderer;
            /* This per-session transfer slot also retains audio renderer work
             * memory; graphics and audio endpoints never share a session. */
            ++c->transfers[index].driver_references;c->nvdrv_transfer_slots[renderer]=index+1;
            memset(request,0,48);little_write(request+4,UINT32_C(0x80000008),4);
            little_write(request+8,0x20,4);little_write(request+12,handle,4);
            little_write(request+16,UINT32_C(0x4f434653),4);
            little_write(request+20,1,4);little_write(request+28,token,4);
            c->x[0]=0;return 1;
        }
        if(c->session_types[slot]==BOOT_SESSION_FSP+13 && type==6 && little_read(request+4,4)==12){
            /* audren:u command 4: one audio-device service for this applet's
             * observed REV11 profile. Device commands are checked separately. */
            request=boot_access(c,c->tpidrro,64,READ|WRITE,pc);if(!request)return 0;
            uint32_t token=(uint32_t)little_read(request+28,4);
            if(c->session_domains[slot] || c->session_process_ids[slot]!=1 ||
               little_read(request+16,4)!=UINT32_C(0x49434653) ||
               little_read(request+20,4)!=1 || little_read(request+24,4)!=4 ||
               little_read(request+32,4)!=UINT32_C(0x3b564552) ||
               little_read(request+36,4) || !c->applet_resource_user_id ||
               little_read(request+40,8)!=c->applet_resource_user_id){
                boot_fault(c,BOOT_SVC,pc,c->tpidrro,64);return 0;
            }
            uint32_t handle=boot_new_session(c,pc,BOOT_SESSION_FSP+16,c->session_process_ids[slot]);if(!handle)return 0;
            memset(request,0,48);little_write(request+4,UINT32_C(0x80000008),4);
            little_write(request+8,0x20,4);little_write(request+12,handle,4);
            little_write(request+16,UINT32_C(0x4f434653),4);
            little_write(request+20,1,4);little_write(request+28,token,4);
            c->x[0]=0;return 1;
        }
        if(c->session_types[slot]==BOOT_SESSION_FSP+13 && type==6 && little_read(request+4,4)==21){
            /* audren:u command 1, GetWorkBufferSize, for the observed REV11
             * renderer profile. Sizes follow the renderer's workbuffer layout;
             * unknown revisions and parameter shapes remain explicit stops. */
            request=boot_access(c,c->tpidrro,96,READ|WRITE,pc);if(!request)return 0;
            uint32_t version=(uint32_t)little_read(request+20,4);
            uint32_t command=(uint32_t)little_read(request+24,4);
            uint32_t token=(uint32_t)little_read(request+28,4);
            uint64_t rate=little_read(request+32,4),samples=little_read(request+36,4);
            uint64_t mixes=little_read(request+40,4),sub=little_read(request+44,4);
            uint64_t voices=little_read(request+48,4),sinks=little_read(request+52,4);
            uint64_t effects=little_read(request+56,4),perf=little_read(request+60,4);
            uint64_t splitters=little_read(request+68,4),destinations=little_read(request+72,4);
            if(c->session_domains[slot] || c->session_process_ids[slot]!=1 ||
               little_read(request+8,4)!=32 || little_read(request+16,4)!=UINT32_C(0x49434653) ||
               version!=1 || command!=1 || rate!=48000 || samples!=240 ||
               !mixes || mixes>256 || sub>256 || !voices || voices>1024 ||
               !sinks || sinks>256 || effects>256 || splitters>1024 || destinations>4096 ||
               perf || request[64]>1 || request[65] || request[66] || request[67] ||
               little_read(request+76,4) || little_read(request+80,4)!=UINT32_C(0x3b564552)){
                boot_fault(c,BOOT_SVC,pc,c->tpidrro,96);return 0;
            }
            #define BOOT_AUDIO_ALIGN(value,alignment) (((value)+(uint64_t)(alignment)-1)&~((uint64_t)(alignment)-1))
            uint64_t count=sub+1,mem_pools=effects+voices*4;
            uint64_t node=BOOT_AUDIO_ALIGN(count,64)/8*2+count*32+count*(count*4);
            uint64_t edges=BOOT_AUDIO_ALIGN(count*count,64)/8;
            uint64_t bytes=BOOT_AUDIO_ALIGN(mixes*4,64)+sub*256*4+count*2368+
                voices*(512+200+296)+BOOT_AUDIO_ALIGN(count*8,16)+
                BOOT_AUDIO_ALIGN(voices*8,16)+
                BOOT_AUDIO_ALIGN(((sinks+sub)*240*4+samples*4)*(mixes+6),64)+
                BOOT_AUDIO_ALIGN(node+edges,16)+destinations*216+splitters*32+
                BOOT_AUDIO_ALIGN(destinations*4,16)+mem_pools*32+effects*128+0x50;
            bytes=BOOT_AUDIO_ALIGN(bytes,64);
            bytes+=(sinks+sub)*616+effects*1592+BOOT_AUDIO_ALIGN(voices*296,64)+
                sinks*352+effects*128;
            uint64_t voice_command=296+56*2+40+328;
            uint64_t commands=effects*1592+
                voices*(splitters*96+voice_command)+
                (48+40*24*24)+(48+40*24)+destinations*48*24+
                sinks*320+(effects+voices+sinks+sub+1+100)*64;
            bytes=BOOT_AUDIO_ALIGN(bytes+commands+126,4096);
            #undef BOOT_AUDIO_ALIGN
            memset(request,0,48);little_write(request+4,10,4);
            little_write(request+16,UINT32_C(0x4f434653),4);
            little_write(request+20,version,4);little_write(request+28,token,4);
            little_write(request+32,bytes,8);c->x[0]=0;return 1;
        }
        if((c->session_types[slot]==BOOT_SESSION_FSP+14 || c->session_types[slot]==BOOT_SESSION_FSP+15) &&
           type==6 && little_read(request+4,4)==8 && little_read(request+24,4)!=7){
            /* Observed renderer uses the fixed REV11 profile opened above.
             * Start/Stop only transition its service state; actual audio
             * processing remains an explicit boundary at RequestUpdate. */
            request=boot_access(c,c->tpidrro,48,READ|WRITE,pc);if(!request)return 0;
            unsigned transfer=c->nvdrv_transfer_slots[slot];
            uint32_t command=(uint32_t)little_read(request+24,4);
            uint32_t token=(uint32_t)little_read(request+28,4);
            if(c->session_domains[slot] || c->session_process_ids[slot]!=1 || !transfer ||
               little_read(request+16,4)!=UINT32_C(0x49434653) ||
               little_read(request+20,4)!=1 ||
               (command!=0 && command!=1 && command!=2 && command!=3 && command!=5 && command!=6)){
                boot_fault(c,BOOT_SVC,pc,c->tpidrro,48);return 0;
            }
            if(command==5 || command==6){
                unsigned source=500+transfer;
                for(unsigned i=0;i<16;++i)if(c->event_handles[i] && c->event_sources[i]==source){
                    HANDLE event=(HANDLE)(uintptr_t)c->event_native_handles[i];
                    if(!(command==5?SetEvent(event):ResetEvent(event))){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
                    break;
                }
                c->session_types[slot]=BOOT_SESSION_FSP+(command==5?15:14);
            }
            uint32_t value=command==0?48000:command==1?240:command==2?72:
                           c->session_types[slot]==BOOT_SESSION_FSP+15?0:1;
            unsigned reply_size=command==5 || command==6?40:44;
            memset(request,0,reply_size);little_write(request+4,command==5 || command==6?8:9,4);
            little_write(request+16,UINT32_C(0x4f434653),4);
            little_write(request+20,1,4);little_write(request+28,token,4);
            if(reply_size==44)little_write(request+32,value,4);
            c->x[0]=0;return 1;
        }
        if(c->session_types[slot]==BOOT_SESSION_FSP+15 && type==UINT32_C(0x02100006) && little_read(request+4,4)==8){
            /* Audio renderer command 4, RequestUpdate. This is the observed
             * REV11 startup profile. Return a silent renderer update with the
             * exact section layout, while retaining explicit stops for other
             * profiles and descriptor shapes. */
            request=boot_access(c,c->tpidrro,64,READ|WRITE,pc);if(!request)return 0;
            uint32_t in_descriptor=(uint32_t)little_read(request+16,4);
            uint32_t out_descriptor=(uint32_t)little_read(request+28,4);
            uint32_t perf_descriptor=(uint32_t)little_read(request+40,4);
            uint64_t in_address=little_read(request+12,4)|((uint64_t)(in_descriptor>>28)<<32)|((uint64_t)((in_descriptor>>2)&0x3fffff)<<36);
            uint64_t out_address=little_read(request+24,4)|((uint64_t)(out_descriptor>>28)<<32)|((uint64_t)((out_descriptor>>2)&0x3fffff)<<36);
            uint64_t in_size=little_read(request+8,4)|((uint64_t)((in_descriptor>>24)&15)<<32);
            uint64_t out_size=little_read(request+20,4)|((uint64_t)((out_descriptor>>24)&15)<<32);
            uint32_t token=(uint32_t)little_read(request+60,4);
            /* Mix and splitter sections vary with the game's mixer setup (0x20/0x20, 0x950/0xa820
             * and others observed); neither has reply output. Every section that shapes the
             * reply stays fixed, and the header total must equal the sum of its sections. */
            if(c->session_domains[slot] || c->session_process_ids[slot]!=1 || !c->nvdrv_transfer_slots[slot] ||
               (in_descriptor&3) || (out_descriptor&3) || (perf_descriptor&3) ||
               in_size<UINT64_C(0xf520) || in_size>UINT64_C(0x100000) || out_size!=UINT64_C(0x2950) ||
               little_read(request+32,8)!=0 || perf_descriptor ||
               in_address>UINT64_MAX-in_size || out_address>UINT64_MAX-out_size ||
               little_read(request+48,4)!=UINT32_C(0x49434653) ||
               little_read(request+52,4)!=1 || little_read(request+56,4)!=4){
                boot_fault(c,BOOT_SVC,pc,c->tpidrro,64);return 0;
            }
            const uint8_t*input=boot_access(c,in_address,(size_t)in_size,READ,pc);
            uint8_t*output=boot_access(c,out_address,(size_t)out_size,WRITE,pc);
            if(!input || !output)return 0;
            if(little_read(input,4)!=UINT32_C(0x3b564552) ||
               little_read(input+4,4)!=0x10 || little_read(input+8,4)!=0x3200 ||
               little_read(input+12,4)!=0x8a00 || little_read(input+16,4)!=0x2a00 ||
               little_read(input+20,4)!=0xc00 || (little_read(input+24,4)&15) || little_read(input+24,4)<0x20 ||
               little_read(input+28,4)!=0x280 || little_read(input+32,4)!=0x10 ||
               (little_read(input+36,4)&15) || little_read(input+36,4)<0x20 || little_read(input+60,4)!=in_size ||
               0x40+0x10+0x3200+0x8a00+0x2a00+0xc00+little_read(input+24,4)+0x280+0x10+little_read(input+36,4)!=in_size ||
               little_read(input+64,4)!=UINT32_C(0x3b564552)){
                boot_fault(c,BOOT_SVC,pc,in_address,0x50);return 0;
            }
            memset(output,0,(size_t)out_size);
            little_write(output,UINT32_C(0x3b564552),4);
            little_write(output+4,0xb0,4);
            little_write(output+8,0x1900,4);
            little_write(output+12,0x600,4);
            little_write(output+20,0x900,4);
            little_write(output+28,0x40,4);
            little_write(output+32,0x10,4);
            little_write(output+40,0x10,4);
            little_write(output+60,out_size,4);
            for(unsigned i=0;i<400;++i){
                const uint8_t*pool=input+0x50+i*0x20;
                uint32_t state=(uint32_t)little_read(pool+16,4);
                if(state==4){
                    uint64_t address=little_read(pool,8),size=little_read(pool+8,8);
                    if(!address || !size || ((address|size)&0xfff)){
                        boot_fault(c,BOOT_SVC,pc,in_address+0x50+i*0x20,0x20);return 0;
                    }
                    little_write(output+0x40+i*0x10,5,4);
                }else if(state==2){
                    little_write(output+0x40+i*0x10,3,4);
                }else if(state>6){
                    boot_fault(c,BOOT_SVC,pc,in_address+0x50+i*0x20,0x20);return 0;
                }
            }
            {   /* Voice progress (host_audren_voices.h): one 240-sample frame per update, state kept in
                 * the renderer work buffer (server-only transfer memory, carried by checkpoints). */
                BootTransfer*work=&c->transfers[c->nvdrv_transfer_slots[slot]-1];uint8_t*host=NULL;
                for(unsigned i=0;i<c->region_count && !host;++i){Region*r=&c->regions[i];
                    if(r->data && work->base>=r->base && work->base-r->base<=r->size && work->size<=r->size-(work->base-r->base))
                        host=r->data+(work->base-r->base);}
                boot_audren_update(host,host?(size_t)work->size:0,work->handle,input,output,
                                   c->session_types[slot]==BOOT_SESSION_FSP+15,240,48000);
            }
            memset(request,0,40);little_write(request+4,8,4);
            little_write(request+16,UINT32_C(0x4f434653),4);
            little_write(request+20,1,4);little_write(request+28,token,4);
            unsigned source=500+c->nvdrv_transfer_slots[slot];
            for(unsigned i=0;i<16;++i)if(c->event_handles[i] && c->event_sources[i]==source){
                HANDLE event=(HANDLE)(uintptr_t)c->event_native_handles[i];
                if(!ResetEvent(event) || !boot_schedule_audio_completion(event)){
                    boot_fault(c,BOOT_SVC,pc,0,0);return 0;
                }
            }
            c->x[0]=0;return 1;
        }
        if((c->session_types[slot]==BOOT_SESSION_FSP+14 || c->session_types[slot]==BOOT_SESSION_FSP+15) && type==6 && little_read(request+4,4)==8){
            /* Audio renderer command 7 returns a readable completion event.
             * The synchronous silent RequestUpdate signals it on completion. */
            request=boot_access(c,c->tpidrro,48,READ|WRITE,pc);if(!request)return 0;
            unsigned transfer=c->nvdrv_transfer_slots[slot];
            if(c->session_domains[slot] || c->session_process_ids[slot]!=1 || !transfer ||
               little_read(request+16,4)!=UINT32_C(0x49434653) ||
               little_read(request+20,4)!=1 || little_read(request+24,4)!=7){
                boot_fault(c,BOOT_SVC,pc,c->tpidrro,48);return 0;
            }
            unsigned event_slot=0;while(event_slot<16 && c->event_handles[event_slot])++event_slot;
            if(event_slot==16 || c->next_session_handle>=0x7ffffffe){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
            unsigned source=500+transfer;
            HANDLE event=NULL;int existing=0;
            for(unsigned i=0;i<16;++i)if(c->event_handles[i] && c->event_sources[i]==source){
                existing=1;
                DuplicateHandle(GetCurrentProcess(),(HANDLE)(uintptr_t)c->event_native_handles[i],GetCurrentProcess(),&event,0,FALSE,DUPLICATE_SAME_ACCESS);
                break;
            }
            if(!existing)event=CreateEventW(NULL,TRUE,c->session_types[slot]==BOOT_SESSION_FSP+15,NULL);
            if(!event){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
            if(c->next_session_handle<0x10000)c->next_session_handle=0x10000;
            uint32_t handle=++c->next_session_handle;
            c->event_handles[event_slot]=handle;c->event_sources[event_slot]=source;
            c->event_native_handles[event_slot]=(uint64_t)(uintptr_t)event;++c->event_create_count;
            uint32_t token=(uint32_t)little_read(request+28,4);
            memset(request,0,48);little_write(request+4,UINT32_C(0x80000008),4);
            little_write(request+8,2,4);little_write(request+12,handle,4);
            little_write(request+16,UINT32_C(0x4f434653),4);
            little_write(request+20,1,4);little_write(request+28,token,4);
            c->x[0]=0;return 1;
        }
        if(c->session_types[slot]==BOOT_SESSION_FSP+16 && type==6 && little_read(request+4,4)==8 &&
           little_read(request+24,4)==5){
            /* IAudioDevice command 5: deterministic stereo host fixture. */
            request=boot_access(c,c->tpidrro,48,READ|WRITE,pc);if(!request)return 0;
            if(c->session_domains[slot] || c->session_process_ids[slot]!=1 ||
               little_read(request+16,4)!=UINT32_C(0x49434653) || little_read(request+20,4)!=1){
                boot_fault(c,BOOT_SVC,pc,c->tpidrro,48);return 0;
            }
            uint32_t token=(uint32_t)little_read(request+28,4);
            memset(request,0,40);little_write(request+4,9,4);
            little_write(request+16,UINT32_C(0x4f434653),4);
            little_write(request+20,1,4);little_write(request+28,token,4);
            little_write(request+32,2,4);c->x[0]=0;return 1;
        }
        if(c->session_types[slot]==BOOT_SESSION_FSP+16 && type==6 && little_read(request+4,4)==8){
            /* IAudioDevice command 12 queries the output-device change event.
             * It is initially unsignaled and shared by duplicate copy handles. */
            request=boot_access(c,c->tpidrro,48,READ|WRITE,pc);if(!request)return 0;
            unsigned event_slot=0;while(event_slot<16 && c->event_handles[event_slot])++event_slot;
            if(c->session_domains[slot] || c->session_process_ids[slot]!=1 ||
               event_slot==16 || c->next_session_handle>=0x7ffffffe ||
               little_read(request+16,4)!=UINT32_C(0x49434653) ||
               little_read(request+20,4)!=1 || little_read(request+24,4)!=12){
                boot_fault(c,BOOT_SVC,pc,c->tpidrro,48);return 0;
            }
            HANDLE event=NULL;int existing=0;
            for(unsigned i=0;i<16;++i)if(c->event_handles[i] && c->event_sources[i]==600){
                existing=1;
                DuplicateHandle(GetCurrentProcess(),(HANDLE)(uintptr_t)c->event_native_handles[i],GetCurrentProcess(),&event,0,FALSE,DUPLICATE_SAME_ACCESS);
                break;
            }
            if(!existing)event=CreateEventW(NULL,TRUE,FALSE,NULL);
            if(!event){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
            if(c->next_session_handle<0x10000)c->next_session_handle=0x10000;
            uint32_t handle=++c->next_session_handle;
            c->event_handles[event_slot]=handle;c->event_sources[event_slot]=600;
            c->event_native_handles[event_slot]=(uint64_t)(uintptr_t)event;++c->event_create_count;
            uint32_t token=(uint32_t)little_read(request+28,4);
            memset(request,0,48);little_write(request+4,UINT32_C(0x80000008),4);
            little_write(request+8,2,4);little_write(request+12,handle,4);
            little_write(request+16,UINT32_C(0x4f434653),4);
            little_write(request+20,1,4);little_write(request+28,token,4);
            c->x[0]=0;return 1;
        }
        if(c->session_types[slot]==BOOT_SESSION_FSP+16 && type==UINT32_C(0x01000006) &&
           little_read(request+4,4)==8 && little_read(request+40,4)==13){
            /* IAudioDevice command 13: the active output name for the
             * deterministic handheld-speaker host fixture. */
            request=boot_access(c,c->tpidrro,48,READ|WRITE,pc);if(!request)return 0;
            uint32_t descriptor=(uint32_t)little_read(request+16,4);
            uint64_t address=little_read(request+12,4)|((uint64_t)(descriptor>>28)<<32)|((uint64_t)((descriptor>>2)&0x3fffff)<<36);
            uint64_t capacity=little_read(request+8,4)|((uint64_t)((descriptor>>24)&15)<<32);
            uint32_t token=(uint32_t)little_read(request+44,4);
            if(c->session_domains[slot] || c->session_process_ids[slot]!=1 ||
               (descriptor&3) || capacity!=256 || address>UINT64_MAX-256 ||
               (address<c->tpidrro+48 && c->tpidrro<address+256) ||
               little_read(request+32,4)!=UINT32_C(0x49434653) ||
               little_read(request+36,4)!=1){
                boot_fault(c,BOOT_SVC,pc,c->tpidrro,48);return 0;
            }
            uint8_t*name=boot_access(c,address,256,WRITE,pc);if(!name)return 0;
            memset(name,0,256);
            memcpy(name,"AudioBuiltInSpeakerOutput",sizeof("AudioBuiltInSpeakerOutput"));
            memset(request,0,44);little_write(request+4,8,4);
            little_write(request+16,UINT32_C(0x4f434653),4);
            little_write(request+20,1,4);little_write(request+28,token,4);
            c->x[0]=0;return 1;
        }
        if(c->session_types[slot]==BOOT_SESSION_FSP+16 && type==UINT32_C(0x01000006) && little_read(request+4,4)==8){
            /* IAudioDevice command 14: three Switch output-device names, each
             * 0x100 bytes. Reject any other output-buffer shape. */
            request=boot_access(c,c->tpidrro,48,READ|WRITE,pc);if(!request)return 0;
            uint32_t descriptor=(uint32_t)little_read(request+16,4);
            uint64_t address=little_read(request+12,4)|((uint64_t)(descriptor>>28)<<32)|((uint64_t)((descriptor>>2)&0x3fffff)<<36);
            uint64_t capacity=little_read(request+8,4)|((uint64_t)((descriptor>>24)&15)<<32);
            uint32_t token=(uint32_t)little_read(request+44,4);
            if(c->session_domains[slot] || c->session_process_ids[slot]!=1 ||
               (descriptor&3)!=0 || capacity!=768 || address>UINT64_MAX-768 ||
               (address<c->tpidrro+48 && c->tpidrro<address+768) ||
               little_read(request+32,4)!=UINT32_C(0x49434653) ||
               little_read(request+36,4)!=1 || little_read(request+40,4)!=14){
                boot_fault(c,BOOT_SVC,pc,c->tpidrro,48);return 0;
            }
            uint8_t*names=boot_access(c,address,768,WRITE,pc);if(!names)return 0;
            memset(names,0,768);
            memcpy(names,"AudioBuiltInSpeakerOutput",sizeof("AudioBuiltInSpeakerOutput"));
            memcpy(names+256,"AudioTvOutput",sizeof("AudioTvOutput"));
            memcpy(names+512,"AudioExternalOutput",sizeof("AudioExternalOutput"));
            memset(request,0,44);little_write(request+4,9,4);
            little_write(request+16,UINT32_C(0x4f434653),4);
            little_write(request+20,1,4);little_write(request+28,token,4);
            little_write(request+32,3,4);c->x[0]=0;return 1;
        }
        if(c->session_types[slot]==BOOT_SESSION_FSP+16 && type==UINT32_C(0x00110006) &&
           (little_read(request+4,4)==8 || little_read(request+4,4)==9)){
            /* IAudioDevice commands 7/8 use the same 0x100-byte input name.
             * Preserve each accepted volume across native DLL resumes. The
             * renderer is still silent; this is device-control state only. */
            uint32_t data_words=(uint32_t)little_read(request+4,4);
            unsigned input_bytes=data_words==9?52:48;
            request=boot_access(c,c->tpidrro,input_bytes,READ|WRITE,pc);if(!request)return 0;
            uint32_t descriptor=(uint32_t)little_read(request+24,4);
            uint64_t address=little_read(request+20,4)|((uint64_t)(descriptor>>28)<<32)|((uint64_t)((descriptor>>2)&0x3fffff)<<36);
            uint64_t capacity=little_read(request+16,4)|((uint64_t)((descriptor>>24)&15)<<32);
            uint32_t token=(uint32_t)little_read(request+44,4);
            if(c->session_domains[slot] || c->session_process_ids[slot]!=1 ||
               little_read(request+8,8) || little_read(request+28,4) ||
               (descriptor&3) || capacity!=256 || address>UINT64_MAX-256 ||
               (address<c->tpidrro+input_bytes && c->tpidrro<address+256) ||
               little_read(request+32,4)!=UINT32_C(0x49434653) ||
               little_read(request+36,4)!=1 || little_read(request+40,4)!=(data_words==9?7:8)){
                boot_fault(c,BOOT_SVC,pc,c->tpidrro,input_bytes);return 0;
            }
            const uint8_t*name=boot_access(c,address,256,READ,pc);if(!name)return 0;
            static const char*device_names[3]={"AudioBuiltInSpeakerOutput","AudioTvOutput","AudioExternalOutput"};
            unsigned device=3;
            for(unsigned i=0;i<3;++i){
                size_t length=strlen(device_names[i])+1;
                if(memcmp(name,device_names[i],length))continue;
                unsigned tail=length;
                while(tail<256 && !name[tail])++tail;
                if(tail==256){device=i;break;}
            }
            if(device==3){boot_fault(c,BOOT_SVC,pc,address,256);return 0;}
            uint32_t volume_bits=0;
            if(data_words==9){
                volume_bits=(uint32_t)little_read(request+48,4);
                float volume;memcpy(&volume,&volume_bits,sizeof(volume));
                if(!(volume>=0.0f && volume<=2.0f)){
                    boot_fault(c,BOOT_SVC,pc,c->tpidrro,input_bytes);return 0;
                }
                c->audio_device_volume_bits[device]=volume_bits;
                c->audio_device_volume_valid|=1u<<device;
            }else{
                if(!(c->audio_device_volume_valid&(1u<<device))){boot_fault(c,BOOT_SVC,pc,c->tpidrro,input_bytes);return 0;}
                volume_bits=c->audio_device_volume_bits[device];
            }
            memset(request,0,data_words==9?40:44);little_write(request+4,data_words==9?8:9,4);
            little_write(request+16,UINT32_C(0x4f434653),4);
            little_write(request+20,1,4);little_write(request+28,token,4);
            if(data_words==8)little_write(request+32,volume_bits,4);
            c->x[0]=0;return 1;
        }
        if(c->session_types[slot]==BOOT_SESSION_FSP+11 && (type==UINT32_C(0x10004) || type==UINT32_C(0x10006)) && little_read(request+4,4)==UINT32_C(0x8000000a)){
            request=boot_access(c,c->tpidrro,68,READ|WRITE,pc);if(!request)return 0;
            uint32_t descriptor=(uint32_t)little_read(request+20,4),size=descriptor>>16;
            uint64_t address=little_read(request+24,4)|((uint64_t)((descriptor>>12)&15)<<32)|((uint64_t)((descriptor>>6)&63)<<36);
            uint32_t version=(type&0xffff)==6?1:0,token=(uint32_t)little_read(request+44,4);
            BootWindow*w=(BootWindow*)(uintptr_t)c->window_native;
            if(c->session_domains[slot] || c->session_process_ids[slot]!=1 || !c->applet_resource_user_id ||
               !w || w->closed || w->error || !w->hid_memory.npad_active || w->hid_memory.owner!=c->applet_resource_user_id ||
               (descriptor&63) || !size || size>40 || (size&3) || little_read(request+8,4)!=1 ||
               little_read(request+32,4)!=UINT32_C(0x49434653) || little_read(request+36,4)!=version ||
               little_read(request+40,4)!=102 || little_read(request+48,8)!=c->applet_resource_user_id ||
               address>UINT64_MAX-size || (address<c->tpidrro+68 && c->tpidrro<address+size)){
                boot_fault(c,BOOT_SVC,pc,c->tpidrro,68);return 0;
            }
            uint8_t*source=boot_access(c,address,size,READ,pc);if(!source)return 0;
            uint32_t ids[10]={0};unsigned count=size/4;
            for(unsigned i=0;i<count;++i){ids[i]=(uint32_t)little_read(source+4*i,4);
                if(ids[i]>7 && ids[i]!=16 && ids[i]!=32){boot_fault(c,BOOT_SVC,pc,address,size);return 0;}
                for(unsigned j=0;j<i;++j)if(ids[i]==ids[j]){boot_fault(c,BOOT_SVC,pc,address,size);return 0;}
            }
            memcpy(w->hid_memory.pointer_buffer,source,size);memcpy(w->hid_memory.npad_ids,ids,sizeof(ids));w->hid_memory.npad_id_count=count;
            memset(request,0,40);little_write(request+4,8,4);little_write(request+16,UINT32_C(0x4f434653),4);
            little_write(request+20,version,4);little_write(request+28,token,4);c->x[0]=0;return 1;
        }
        if(c->session_types[slot]==BOOT_SESSION_FSP+11 && (type==UINT32_C(0x00020004) || type==UINT32_C(0x00020006)) &&
           (request=boot_access(c,c->tpidrro,64,READ|WRITE,pc)) && !(little_read(request+4,4)&UINT32_C(0xfffffc00)) &&
           little_read(request+32,4)==UINT32_C(0x49434653) && little_read(request+40,4)==206){
            /* SendVibrationValues(two pointer buffers: handles, values). Accepted; TOTK_DRAW_PATH builds drive
             * the host gamepad's motors (host_input_sdl.h). */
            uint32_t version=type==UINT32_C(0x00020006)?1:0,token=(uint32_t)little_read(request+44,4);
            if(c->session_domains[slot] || c->session_process_ids[slot]!=1 || little_read(request+36,4)!=version){
                boot_fault(c,BOOT_SVC,pc,c->tpidrro,64);return 0;
            }
#ifdef TOTK_DRAW_PATH
            if(totk_input_sdl_vibrate){   /* X[0] VibrationDeviceHandle[n] (4 bytes), X[1] VibrationValue[n] (16 bytes) */
                uint32_t d0=(uint32_t)little_read(request+8,4),d1=(uint32_t)little_read(request+16,4);
                uint64_t a0=little_read(request+12,4)|((uint64_t)((d0>>12)&15)<<32)|((uint64_t)((d0>>6)&63)<<36);
                uint64_t a1=little_read(request+20,4)|((uint64_t)((d1>>12)&15)<<32)|((uint64_t)((d1>>6)&63)<<36);
                uint32_t n=(d0>>16)/4;
                if(n && n<=8 && (d0>>16)==n*4 && (d1>>16)==n*16 && a0 && a1){
                    const uint8_t*handles=boot_access(c,a0,n*4,READ,pc);if(!handles)return 0;
                    const uint8_t*values=boot_access(c,a1,n*16,READ,pc);if(!values)return 0;
                    for(uint32_t i=0;i<n;++i)totk_input_vibration_forward((uint32_t)little_read(handles+4*i,4),values+16*i);
                    request=boot_access(c,c->tpidrro,64,READ|WRITE,pc);if(!request)return 0;
                }
            }
#endif
            memset(request,0,40);little_write(request+4,8,4);little_write(request+16,UINT32_C(0x4f434653),4);
            little_write(request+20,version,4);little_write(request+28,token,4);c->x[0]=0;return 1;
        }
        if(c->session_types[slot]==140 && (type==4 || type==6)){
            /* IActiveVibrationDeviceList: 0 ActivateVibrationDevice(VibrationDeviceHandle). Accepted. */
            request=boot_access(c,c->tpidrro,64,READ|WRITE,pc);if(!request)return 0;
            uint32_t header=(uint32_t)little_read(request+4,4);unsigned base=(header>>31)?32:16;
            uint32_t version=type==6?1:0,token=(uint32_t)little_read(request+base+12,4);
            if(c->session_domains[slot] || (header&UINT32_C(0x7ffffc00)) || little_read(request+base,4)!=UINT32_C(0x49434653) ||
               little_read(request+base+4,4)!=version || little_read(request+base+8,4)!=0){
                boot_fault(c,BOOT_SVC,pc,c->tpidrro,64);return 0;
            }
            memset(request,0,40);little_write(request+4,8,4);little_write(request+16,UINT32_C(0x4f434653),4);
            little_write(request+20,version,4);little_write(request+28,token,4);c->x[0]=0;return 1;
        }
        if(c->session_types[slot]==BOOT_SESSION_FSP+11 && (type==4 || type==6) &&
           ((little_read(request+4,4)&UINT32_C(0xfffffc00))==UINT32_C(0x80000000) || !(little_read(request+4,4)&UINT32_C(0xfffffc00))) &&
           (request=boot_access(c,c->tpidrro,80,READ|WRITE,pc))){
            /* hid setters and simple getters with no buffers or handles (command list per
             * suyu hid_server.cpp). No controller is connected on this host: setters are
             * accepted; getters report the defaults a disconnected console reports. */
            static const uint16_t setters[]={66,67,69,70,72,79,81,84,89,91,92,104,107,122,123,124,125,126,127,128,130,
                                             132,133,134,135,136,201,204,209,210,300,301,302};
            uint32_t header=(uint32_t)little_read(request+4,4);unsigned pid=header>>31,base=pid?32:16;
            uint32_t command=(uint32_t)little_read(request+base+8,4);
            if((command==200 || command==202) && little_read(request+base,4)==UINT32_C(0x49434653) && (!pid || little_read(request+8,4)==1)){
                /* 200 GetVibrationDeviceInfo(VibrationDeviceHandle): linear resonant actuator, left/right by the
                 * handle's device index (byte 2). 202 GetActualVibrationValue: the default value suyu reports
                 * {0, 160 Hz, 0, 320 Hz}; no vibration hardware is driven. */
                uint32_t version=type==6?1:0,token=(uint32_t)little_read(request+base+12,4);
                if(c->session_domains[slot] || c->session_process_ids[slot]!=1 || little_read(request+base+4,4)!=version){
                    boot_fault(c,BOOT_SVC,pc,c->tpidrro,80);return 0;
                }
                uint32_t device_index=(uint32_t)(little_read(request+base+16,4)>>16)&0xff;
                memset(request,0,64);little_write(request+16,UINT32_C(0x4f434653),4);little_write(request+20,version,4);little_write(request+28,token,4);
                if(command==200){little_write(request+4,10,4);little_write(request+32,1,4);little_write(request+36,device_index==0?1:device_index==1?2:0,4);}
                else{little_write(request+4,12,4);little_write(request+32,0,4);little_write(request+36,UINT32_C(0x43200000),4);
                     little_write(request+40,0,4);little_write(request+44,UINT32_C(0x43a00000),4);}
                c->x[0]=0;return 1;
            }
            if(command==203 && !pid && little_read(request+base,4)==UINT32_C(0x49434653)){
                /* CreateActiveVibrationDeviceList: a new IActiveVibrationDeviceList session (type 140)
                 * whose ActivateVibrationDevice requests are accepted (no vibration hardware). */
                uint32_t version=type==6?1:0,token=(uint32_t)little_read(request+base+12,4);
                if(c->session_domains[slot] || c->session_process_ids[slot]!=1 || little_read(request+base+4,4)!=version){
                    boot_fault(c,BOOT_SVC,pc,c->tpidrro,80);return 0;
                }
                uint32_t handle=boot_new_session(c,pc,140,c->session_process_ids[slot]);if(!handle)return 0;
                memset(request,0,48);little_write(request+4,UINT32_C(0x80000008),4);little_write(request+8,0x20,4);little_write(request+12,handle,4);
                little_write(request+16,UINT32_C(0x4f434653),4);little_write(request+20,version,4);little_write(request+28,token,4);
                c->x[0]=0;return 1;
            }
            int setter=0;for(unsigned i=0;i<sizeof(setters)/sizeof(setters[0]);++i)setter|=setters[i]==command;
            /* getters: 121 hold type horizontal, 129 handheld activation dual, 131/68/80/85/211 false or 0, 205 vibration permitted */
            /* 101 the style set the game supplied (SetSupportedNpadStyleSet); 108 player LED pattern of npad id */
            int getter=command==121 || command==129 || command==131 || command==68 || command==80 || command==85 ||
                       command==205 || command==211 || command==101 || command==108;
            if((setter || getter) && (!pid || little_read(request+8,4)==1) &&
               little_read(request+base,4)==UINT32_C(0x49434653)){
                uint32_t version=type==6?1:0,token=(uint32_t)little_read(request+base+12,4);
                if(c->session_domains[slot] || c->session_process_ids[slot]!=1 || little_read(request+base+4,4)!=version){
                    boot_fault(c,BOOT_SVC,pc,c->tpidrro,80);return 0;
                }
#ifdef TOTK_DRAW_PATH
                /* 66/67 Start/StopSixAxisSensor(handle, ARUID); 201 SendVibrationValue(handle, VibrationValue, ARUID) */
                if(command==66 || command==67)totk_input_sixaxis_handle((uint32_t)little_read(request+base+16,4),command==66);
                if(command==201)totk_input_vibration_forward((uint32_t)little_read(request+base+16,4),request+base+20);
#endif
                uint64_t value=command==121?1:command==205?1:0;
                if(command==101){BootWindow*w=(BootWindow*)(uintptr_t)c->window_native;value=w?w->hid_memory.npad_styles:0;}
                if(command==108){static const uint8_t leds[8]={1,3,7,15,9,5,13,11};uint32_t id=(uint32_t)little_read(request+base+16,4);
                    value=id<8?leds[id]:0;}
                int wide=command==121 || command==108;
                memset(request,0,48);little_write(request+4,getter?(wide?10:9):8,4);
                little_write(request+16,UINT32_C(0x4f434653),4);little_write(request+20,version,4);little_write(request+28,token,4);
                if(getter)little_write(request+32,value,wide?8:4);
                c->x[0]=0;return 1;
            }
            request=boot_access(c,c->tpidrro,8,READ|WRITE,pc);if(!request)return 0;
        }
        if(c->session_types[slot]==BOOT_SESSION_FSP+11 && (type==4 || type==6) && little_read(request+4,4)==UINT32_C(0x8000000c) &&
           (request=boot_access(c,c->tpidrro,68,READ|WRITE,pc)) && little_read(request+40,4)==120){
            /* SetNpadJoyHoldType(ARUID, u64 hold type: 0 vertical, 1 horizontal). Accepted;
             * no controller is connected, so it changes no shared-memory sample. */
            uint32_t version=type==6?1:0,token=(uint32_t)little_read(request+44,4);
            if(c->session_domains[slot] || c->session_process_ids[slot]!=1 || !c->applet_resource_user_id ||
               little_read(request+8,4)!=1 || little_read(request+32,4)!=UINT32_C(0x49434653) || little_read(request+36,4)!=version ||
               little_read(request+48,8)!=c->applet_resource_user_id || little_read(request+56,8)>1){
                boot_fault(c,BOOT_SVC,pc,c->tpidrro,68);return 0;
            }
            memset(request,0,40);little_write(request+4,8,4);little_write(request+16,UINT32_C(0x4f434653),4);
            little_write(request+20,version,4);little_write(request+28,token,4);c->x[0]=0;return 1;
        }
        if(c->session_types[slot]==BOOT_SESSION_FSP+11 && (type==4 || type==6) && little_read(request+4,4)==UINT32_C(0x8000000c)){
            request=boot_access(c,c->tpidrro,68,READ|WRITE,pc);if(!request)return 0;
            uint32_t version=type==6?1:0,token=(uint32_t)little_read(request+44,4);
            uint32_t command=(uint32_t)little_read(request+40,4),value=(uint32_t)little_read(request+48,4);
            BootWindow*w=(BootWindow*)(uintptr_t)c->window_native;
            if(c->session_domains[slot] || c->session_process_ids[slot]!=1 || !c->applet_resource_user_id ||
               !w || w->closed || w->error || !w->hid_memory.data || w->hid_memory.owner!=c->applet_resource_user_id ||
               little_read(request+8,4)!=1 || little_read(request+32,4)!=UINT32_C(0x49434653) || little_read(request+36,4)!=version ||
               !((command==109 && value==3) || (command==100 && !(value&~31u) && w->hid_memory.npad_active)) || little_read(request+52,4)!=0 ||
               little_read(request+56,8)!=c->applet_resource_user_id){boot_fault(c,BOOT_SVC,pc,c->tpidrro,68);return 0;}
            /* Activation enables this resource's disconnected-controller view.
             * No controller style bits or samples are manufactured. */
            if(command==109){w->hid_memory.npad_active=1;w->hid_memory.npad_revision=3;}
            else w->hid_memory.npad_styles=value;
            memset(request,0,40);little_write(request+4,8,4);little_write(request+16,UINT32_C(0x4f434653),4);
            little_write(request+20,version,4);little_write(request+28,token,4);c->x[0]=0;return 1;
        }
        if(c->session_types[slot]==BOOT_SESSION_FSP+12 && (type==4 || type==6) && little_read(request+4,4)==8){
            request=boot_access(c,c->tpidrro,48,READ|WRITE,pc);if(!request)return 0;
            uint32_t version=type==6?1:0,token=(uint32_t)little_read(request+28,4);
            BootWindow*w=(BootWindow*)(uintptr_t)c->window_native;
            unsigned entry=0;while(entry<16 && c->event_handles[entry])++entry;
            if(c->session_domains[slot] || c->session_process_ids[slot]!=1 || !c->applet_resource_user_id ||
               !w || w->closed || w->error || entry==16 || c->next_session_handle>=0x7ffffffe ||
               little_read(request+16,4)!=UINT32_C(0x49434653) || little_read(request+20,4)!=version || little_read(request+24,4)!=0){
                boot_fault(c,BOOT_SVC,pc,c->tpidrro,48);return 0;
            }
            HANDLE memory=boot_hid_memory_copy(&w->hid_memory,c->applet_resource_user_id);
            if(!memory){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
            if(c->next_session_handle<0x10000)c->next_session_handle=0x10000;
            uint32_t handle=++c->next_session_handle;
            /* Native handle registry also owns copied shared-memory handles. */
            c->event_handles[entry]=handle;c->event_sources[entry]=400;c->event_native_handles[entry]=(uint64_t)(uintptr_t)memory;
            memset(request,0,48);little_write(request+4,UINT32_C(0x80000008),4);little_write(request+8,2,4);little_write(request+12,handle,4);
            little_write(request+16,UINT32_C(0x4f434653),4);little_write(request+20,version,4);little_write(request+28,token,4);
            c->x[0]=0;return 1;
        }
        if(c->session_types[slot]==BOOT_SESSION_FSP+11 && (type==4 || type==6) && little_read(request+4,4)==UINT32_C(0x8000000a)){
            request=boot_access(c,c->tpidrro,60,READ|WRITE,pc);if(!request)return 0;
            uint32_t version=type==6?1:0,token=(uint32_t)little_read(request+44,4);
            uint32_t command=(uint32_t)little_read(request+40,4);
            if(c->session_domains[slot] || c->session_process_ids[slot]!=1 || !c->applet_resource_user_id ||
               little_read(request+8,4)!=1 || little_read(request+32,4)!=UINT32_C(0x49434653) ||
               little_read(request+36,4)!=version || (command!=0 && command!=11 && command!=21 && command!=31) || little_read(request+48,8)!=c->applet_resource_user_id){
                boot_fault(c,BOOT_SVC,pc,c->tpidrro,60);return 0;
            }
            if(command){
                BootWindow*w=(BootWindow*)(uintptr_t)c->window_native;
                if(!w || w->closed || w->error || !w->hid_memory.mapping || !w->hid_memory.data ||
                   w->hid_memory.owner!=c->applet_resource_user_id){boot_fault(c,BOOT_SVC,pc,c->tpidrro,60);return 0;}
                /* Track touch/mouse/keyboard activation for this resource.
                 * The disconnected backend leaves its sample LIFOs empty. */
                w->hid_memory.peripheral_active|=1u<<((command-11)/10);
                memset(request,0,40);little_write(request+4,8,4);little_write(request+16,UINT32_C(0x4f434653),4);
                little_write(request+20,version,4);little_write(request+28,token,4);c->x[0]=0;return 1;
            }
            uint32_t handle=boot_new_session(c,pc,BOOT_SESSION_FSP+12,c->session_process_ids[slot]);if(!handle)return 0;
            memset(request,0,48);little_write(request+4,UINT32_C(0x80000008),4);little_write(request+8,0x20,4);little_write(request+12,handle,4);
            little_write(request+16,UINT32_C(0x4f434653),4);little_write(request+20,version,4);little_write(request+28,token,4);
            c->x[0]=0;return 1;
        }
        if(c->session_types[slot]==BOOT_SESSION_FSP+4){
            unsigned need=(type==UINT32_C(0x02110004) || type==UINT32_C(0x02110006))?112:
                          (type==UINT32_C(0x01110004) || type==UINT32_C(0x01110006))?92:(type==4 || type==6)?32:8;
            request=boot_access(c,c->tpidrro,need,READ|WRITE,pc);if(!request)return 0;
        }
        if(c->session_types[slot]==BOOT_SESSION_FSP+6 && (type==UINT32_C(0x01000004) || type==UINT32_C(0x01000006))){
            request=boot_access(c,c->tpidrro,140,READ|WRITE,pc);if(!request)return 0;
            uint32_t descriptor=(uint32_t)little_read(request+28,4),version=(type&0xffff)==6?1:0;
            uint64_t address=little_read(request+24,4)|((uint64_t)(descriptor>>28)<<32)|((uint64_t)((descriptor>>2)&0x3fffff)<<36);
            uint64_t capacity=little_read(request+20,4)|((uint64_t)((descriptor>>24)&15)<<32);
            uint64_t layer=little_read(request+112,8),owner=little_read(request+120,8);
            uint32_t token=(uint32_t)little_read(request+44,4);static const uint8_t name[64]="Default";
            if(c->session_domains[slot] || c->session_process_ids[slot]!=1 || owner!=c->applet_resource_user_id || !owner ||
               little_read(request+4,4)!=UINT32_C(0x8000001c) || little_read(request+8,4)!=1 || (descriptor&3)!=0 ||
               little_read(request+32,4)!=UINT32_C(0x49434653) || little_read(request+36,4)!=version ||
               little_read(request+40,4)!=2020 || memcmp(request+48,name,64) || capacity<60 ||
               address>UINT64_MAX-capacity || (address<c->tpidrro+140 && c->tpidrro<address+capacity)){
                boot_fault(c,BOOT_SVC,pc,c->tpidrro,140);return 0;
            }
            uint8_t*output=boot_access(c,address,60,WRITE,pc);if(!output)return 0;
            uint32_t producer=0;
            if(!boot_window_layer((BootWindow*)(uintptr_t)c->window_native,slot,owner,layer,1,&producer)){
                boot_fault(c,BOOT_SVC,pc,0,0);return 0;
            }
            uint8_t parcel[60]={0};little_write(parcel,40,4);little_write(parcel+4,16,4);
            little_write(parcel+8,4,4);little_write(parcel+12,56,4);little_write(parcel+16,2,4);
            little_write(parcel+24,producer,4);memcpy(parcel+40,"dispdrv",7);memcpy(output,parcel,60);
            memset(request,0,48);little_write(request+4,10,4);little_write(request+16,UINT32_C(0x4f434653),4);
            little_write(request+20,version,4);little_write(request+28,token,4);little_write(request+32,60,8);c->x[0]=0;return 1;
        }
        if(c->session_types[slot]==BOOT_SESSION_FSP+7 && (type==UINT32_C(0x01110004) || type==UINT32_C(0x01110006))){
            request=boot_access(c,c->tpidrro,96,READ|WRITE,pc);if(!request)return 0;
            uint32_t version=(type&0xffff)==6,token=(uint32_t)little_read(request+60,4),id=(uint32_t)little_read(request+64,4);
            BootWindow*w=(BootWindow*)(uintptr_t)c->window_native;
            if(c->session_domains[slot] || c->session_process_ids[slot]!=1 || !w || !id || little_read(request+4,4)!=0xc0c || little_read(request+8,8) ||
                little_read(request+94,2) || little_read(request+48,4)!=UINT32_C(0x49434653) || little_read(request+52,4)!=version ||
                little_read(request+56,4)!=3 || (little_read(request+68,4)!=10 && little_read(request+68,4)!=9 && little_read(request+68,4)!=14 && little_read(request+68,4)!=2 && little_read(request+68,4)!=3 && little_read(request+68,4)!=1
#ifdef TOTK_DRAW_PATH
                && little_read(request+68,4)!=7
#endif
                ) || little_read(request+72,4)){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
            int query=little_read(request+68,4)==9,preallocated=little_read(request+68,4)==14,set_count=little_read(request+68,4)==2;
            int dequeue=little_read(request+68,4)==3,request_buffer=little_read(request+68,4)==1;
            int queue=little_read(request+68,4)==7;   /* QUEUE_BUFFER (draw path only) */
            unsigned input_size=queue?192u:preallocated?476u:dequeue?116u:(query || set_count || request_buffer)?100u:108u;
            unsigned output_size=queue?36u:request_buffer?396u:dequeue?72u:(preallocated || set_count)?20u:query?24u:36u;
            uint64_t addresses[2],sizes[2];uint8_t*buffers[2];
            for(unsigned j=0;j<2;++j){unsigned off=16+j*12;uint32_t desc=(uint32_t)little_read(request+off+8,4);
                addresses[j]=little_read(request+off+4,4)|((uint64_t)(desc>>28)<<32)|((uint64_t)((desc>>2)&0x3fffff)<<36);
                sizes[j]=little_read(request+off,4)|((uint64_t)((desc>>24)&15)<<32);
                if((desc&3) || (j==0?sizes[j]!=input_size:(sizes[j]<output_size || sizes[j]>1048576)) || addresses[j]>UINT64_MAX-sizes[j] ||
                    (addresses[j]<c->tpidrro+96 && c->tpidrro<addresses[j]+sizes[j])){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
                buffers[j]=boot_access(c,addresses[j],sizes[j],j?WRITE:READ,pc);if(!buffers[j])return 0;
            }
            if(addresses[0]<addresses[1]+sizes[1] && addresses[1]<addresses[0]+sizes[0]){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
            AcquireSRWLockExclusive(&w->lock);unsigned index=0;while(index<2 && w->producer_ids[index]!=id)++index;RECT rect={0};
            int valid=!w->closed && !w->error && w->hwnd && index<2 && w->managed_layer_owner==c->applet_resource_user_id &&
                w->producer_refs[index][0] && w->producer_refs[index][1] && GetClientRect(w->hwnd,&rect);
            /* A minimised (or zero-sized) host window has an empty client area; the guest's
             * display keeps its own size, so fall back to the 1280x720 handheld resolution. */
            if(valid && (rect.right<=rect.left || rect.bottom<=rect.top)){rect.left=rect.top=0;rect.right=1280;rect.bottom=720;}
            if(valid && dequeue){uint8_t reply[72];unsigned selected=64;
                boot_present_before_dequeue(w,index);   /* held buffers whose vsync has passed (host_present_hold.h) */
                valid=boot_graphic_dequeue_plan(&w->graphic_buffers,index,&w->buffer_queues[index],buffers[0],(size_t)sizes[0],reply,sizeof(reply),&selected);
                if(valid && selected<64){if(w->producer_events[index])valid=ResetEvent(w->producer_events[index])!=0;
                    if(valid)w->graphic_buffers.slots[index][selected].state=1;}
                else if(valid)valid=boot_present_would_block(w,index);
                if(valid)memcpy(buffers[1],reply,sizeof(reply));
            }
#ifdef TOTK_DRAW_PATH
            else if(valid && queue){
                /* slot follows the interface token; the 84-byte input carries timestamp, crop,
                 * scaling, transform and the GPU fence. Submission is synchronous, so the fence
                 * has already passed when the buffer is queued. */
                static const char token[]="android.gui.IGraphicBufferProducer";
                const uint8_t*in=buffers[0];uint32_t qslot=boot_parcel_u32(in+96);
                valid=boot_parcel_u32(in+4)==16 && boot_parcel_u32(in+20)==34 && qslot<64 && boot_parcel_u32(in+100)==84;
                for(unsigned k=0;valid && k<sizeof(token);++k)if(in[24+k*2]!=(uint8_t)token[k] || in[25+k*2])valid=0;
                if(valid)valid=w->graphic_buffers.slots[index][qslot].registered && w->graphic_buffers.slots[index][qslot].state==1;
                if(valid)valid=boot_draw_present(w,&w->graphic_buffers.slots[index][qslot]);
                /* Release: at once (wall clock), or held until its guest vsync x swap interval
                 * (dilated clock; QueueBufferInput.swap_interval at data+44, host_present_hold.h). */
                if(valid)valid=boot_present_queued(w,index,qslot,boot_parcel_u32(in+152));
                if(valid){   /* diagnostics: guest thread table every 600 presents; rarer frame dumps */
                    static uint64_t presents;
                    if(w->present && w->present->dump_every!=300)w->present->dump_every=300;   /* ~10 s of frames at 30 fps */
                    if(++presents%600==1){FILE*f=fopen("threads-dump.txt","w");
                        if(f){fprintf(f,"presents %llu steps %llu current %u switches %u\n",(unsigned long long)presents,(unsigned long long)c->steps,c->current_thread,c->thread_switch_count);
                            for(unsigned i=0;i<c->thread_count;++i){BootThread*t=&c->threads[i];
                                fprintf(f,"%2u h=%x id=%llu state=%u prio=%u core=%u pc=%llx sp=%llx wait_addr=%llx owner=%x waits=%u cancel=%u runnable=%u active=%u last=%llu lr=%llx\n",
                                        i,t->handle,(unsigned long long)t->id,t->state,t->priority,t->core,(unsigned long long)t->pc,(unsigned long long)t->sp,
                                        (unsigned long long)t->wait_address,t->wait_owner,t->wait_count,t->wait_cancel,t->schedule.runnable,t->schedule.active,
                                        (unsigned long long)t->schedule.last_scheduled,(unsigned long long)t->x[30]);
                                for(unsigned k=0;k<t->wait_count && k<4;++k)fprintf(f,"     wait[%u] native=%llx thread=%x\n",k,(unsigned long long)t->wait_native[k],t->wait_threads[k]);
                            }
                            fclose(f);}}
                }
                if(valid){uint8_t reply[36]={0};boot_parcel_put(reply,20);boot_parcel_put(reply+4,16);boot_parcel_put(reply+12,36);
                    boot_parcel_put(reply+16,w->buffer_queues[index].width);boot_parcel_put(reply+20,w->buffer_queues[index].height);
                    memcpy(buffers[1],reply,36);}
            }
#endif
            else if(valid && request_buffer)valid=boot_graphic_request_buffer(&w->graphic_buffers,index,&w->buffer_queues[index],buffers[0],(size_t)sizes[0],buffers[1],(size_t)sizes[1]);
            else if(valid && set_count && (boot_present_release(w,index,1),1))valid=boot_graphic_set_count(&w->graphic_buffers,index,&w->buffer_queues[index],buffers[0],(size_t)sizes[0],buffers[1],(size_t)sizes[1]);
            else if(valid && preallocated){
                BootGraphicBuffer buffer;BootGraphicSlot bound;BootNvMapObject*object=NULL;
                valid=w->buffer_queues[index].connected_api && boot_graphic_buffer_parse(buffers[0],(size_t)sizes[0],&buffer);
                if(valid){valid=!w->graphic_buffers.slots[index][buffer.slot].registered && (!w->buffer_queues[index].buffer_count || buffer.slot<w->buffer_queues[index].buffer_count);
                    object=boot_nvmap_find_handle(&w->nvmap,buffer.object_id);}
                if(valid)valid=boot_graphic_bind(&buffer,object,&bound);
                if(valid)valid=!boot_graphic_cpu_overlap(&w->graphic_buffers,bound.cpu,bound.cpu+bound.buffer.size);
                if(valid)valid=boot_access(c,bound.cpu,bound.buffer.size,READ|WRITE,pc)!=NULL;
                if(valid && w->producer_events[index]){int busy=0;for(unsigned k=0;k<64;++k)busy|=w->graphic_buffers.slots[index][k].registered && w->graphic_buffers.slots[index][k].state==1;
                    valid=(busy?ResetEvent(w->producer_events[index]):SetEvent(w->producer_events[index]))!=0;}
                if(valid){w->graphic_buffers.slots[index][buffer.slot]=bound;uint8_t reply[20]={0};
                    boot_parcel_put(reply,4);boot_parcel_put(reply+4,16);boot_parcel_put(reply+12,20);memcpy(buffers[1],reply,20);}
            }else if(valid)valid=query?boot_buffer_queue_query(&w->buffer_queues[index],buffers[0],(size_t)sizes[0],buffers[1],(size_t)sizes[1]):
                boot_buffer_queue_connect(&w->buffer_queues[index],buffers[0],(size_t)sizes[0],(uint32_t)(rect.right-rect.left),(uint32_t)(rect.bottom-rect.top),buffers[1],(size_t)sizes[1]);
            if(!valid){FILE*f=fopen("binder-reject.txt","a");
                if(f){const uint8_t*in=buffers[0];uint32_t qs=queue?boot_parcel_u32(in+96):0;
                    fprintf(f,"steps %llu code %u index %u closed %d error %d hwnd %p rect %ld,%ld owner %llx/%llx refs %u,%u in4 %u in20 %u qslot %u in100 %u reg %u state %u present %p sizes %llu,%llu\n",
                        (unsigned long long)c->steps,(unsigned)little_read(request+68,4),index,(int)w->closed,(int)w->error,(void*)w->hwnd,
                        (long)(rect.right-rect.left),(long)(rect.bottom-rect.top),(unsigned long long)w->managed_layer_owner,(unsigned long long)c->applet_resource_user_id,
                        index<2?(unsigned)w->producer_refs[index][0]:0u,index<2?(unsigned)w->producer_refs[index][1]:0u,
                        boot_parcel_u32(in+4),boot_parcel_u32(in+20),qs,boot_parcel_u32(in+100),
                        (index<2 && qs<64)?(unsigned)w->graphic_buffers.slots[index][qs].registered:0u,(index<2 && qs<64)?(unsigned)w->graphic_buffers.slots[index][qs].state:0u,
                        (void*)w->hwnd,(unsigned long long)sizes[0],(unsigned long long)sizes[1]);fclose(f);}}
            ReleaseSRWLockExclusive(&w->lock);if(!valid){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
            memset(request,0,40);little_write(request+4,8,4);little_write(request+16,UINT32_C(0x4f434653),4);little_write(request+20,version,4);little_write(request+28,token,4);c->x[0]=0;return 1;
        }
        if(c->session_types[slot]==BOOT_SESSION_FSP+7 && (type==4 || type==6) && little_read(request+4,4)==10){
            request=boot_access(c,c->tpidrro,56,READ|WRITE,pc);if(!request)return 0;
            uint32_t version=type==6?1:0,token=(uint32_t)little_read(request+28,4),id=(uint32_t)little_read(request+32,4);
            BootWindow*w=(BootWindow*)(uintptr_t)c->window_native;
            if(c->session_domains[slot] || c->session_process_ids[slot]!=1 || !w || !id ||
               little_read(request+16,4)!=UINT32_C(0x49434653) || little_read(request+20,4)!=version ||
               little_read(request+24,4)!=2 || little_read(request+36,4)!=15){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
            AcquireSRWLockShared(&w->lock);unsigned index=0;while(index<2 && w->producer_ids[index]!=id)++index;
            int valid=!w->closed && !w->error && w->managed_layer_owner==c->applet_resource_user_id && index<2 && w->producer_refs[index][0] && w->producer_refs[index][1];
            ReleaseSRWLockShared(&w->lock);
            unsigned event_slot=0;while(event_slot<16 && c->event_handles[event_slot])++event_slot;
            if(!valid || event_slot==16 || c->next_session_handle>=0x7ffffffe){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
            HANDLE event=NULL;AcquireSRWLockExclusive(&w->lock);int created=!w->producer_events[index];
            if(created){int available=boot_graphic_available(&w->graphic_buffers,index);
                w->producer_events[index]=CreateEventW(NULL,TRUE,available,NULL);}
            if(w->producer_events[index] && !DuplicateHandle(GetCurrentProcess(),w->producer_events[index],GetCurrentProcess(),&event,0,FALSE,DUPLICATE_SAME_ACCESS))event=NULL;
            if(!event && created && w->producer_events[index]){CloseHandle(w->producer_events[index]);w->producer_events[index]=NULL;}
            ReleaseSRWLockExclusive(&w->lock);
            if(!event){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
            if(c->next_session_handle<0x10000)c->next_session_handle=0x10000;
            uint32_t handle=++c->next_session_handle;
            c->event_handles[event_slot]=handle;c->event_sources[event_slot]=100+index;c->event_native_handles[event_slot]=(uint64_t)(uintptr_t)event;++c->event_create_count;
            memset(request,0,48);little_write(request+4,UINT32_C(0x80000008),4);little_write(request+8,2,4);little_write(request+12,handle,4);
            little_write(request+16,UINT32_C(0x4f434653),4);little_write(request+20,version,4);little_write(request+28,token,4);c->x[0]=0;return 1;
        }
        if(c->session_types[slot]==BOOT_SESSION_FSP+7 && (type==4 || type==6)){
            request=boot_access(c,c->tpidrro,60,READ|WRITE,pc);if(!request)return 0;
            uint32_t version=type==6?1:0,token=(uint32_t)little_read(request+28,4);
            uint32_t id=(uint32_t)little_read(request+32,4),delta=(uint32_t)little_read(request+36,4),kind=(uint32_t)little_read(request+40,4);
            BootWindow*w=(BootWindow*)(uintptr_t)c->window_native;
            if(c->session_domains[slot] || c->session_process_ids[slot]!=1 || !w ||
               little_read(request+4,4)!=11 || little_read(request+16,4)!=UINT32_C(0x49434653) ||
               little_read(request+20,4)!=version || little_read(request+24,4)!=1 ||
               !id || kind>1 || (delta!=1 && delta!=UINT32_MAX)){
                boot_fault(c,BOOT_SVC,pc,c->tpidrro,60);return 0;
            }
            AcquireSRWLockExclusive(&w->lock);unsigned index=0;while(index<2 && w->producer_ids[index]!=id)++index;
            int ok=!w->closed && !w->error && w->managed_layer_owner==c->applet_resource_user_id && index<2;
            if(ok){uint32_t count=w->producer_refs[index][kind];
                ok=delta==1?count<INT32_MAX:count!=0;
                if(ok)w->producer_refs[index][kind]=delta==1?count+1:count-1;
            }
            ReleaseSRWLockExclusive(&w->lock);
            if(!ok){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
            memset(request,0,40);little_write(request+4,8,4);little_write(request+16,UINT32_C(0x4f434653),4);
            little_write(request+20,version,4);little_write(request+28,token,4);c->x[0]=0;return 1;
        }
        if(c->session_types[slot]==BOOT_SESSION_FSP+5 && (type==4 || type==6)){
            request=boot_access(c,c->tpidrro,52,READ|WRITE,pc);if(!request)return 0;
            uint32_t version=type==6?1:0,token=(uint32_t)little_read(request+28,4);
            if(c->session_domains[slot] || c->session_process_ids[slot]!=1 ||
               little_read(request+4,4)!=9 || little_read(request+16,4)!=UINT32_C(0x49434653) ||
               little_read(request+20,4)!=version || little_read(request+24,4)!=1 || little_read(request+32,4)!=0){
                boot_fault(c,BOOT_SVC,pc,c->tpidrro,52);return 0;
            }
            uint32_t handle=boot_new_session(c,pc,BOOT_SESSION_FSP+6,c->session_process_ids[slot]);if(!handle)return 0;
            memset(request,0,48);little_write(request+4,UINT32_C(0x80000008),4);
            little_write(request+8,0x20,4);little_write(request+12,handle,4);
            little_write(request+16,UINT32_C(0x4f434653),4);little_write(request+20,version,4);
            little_write(request+28,token,4);c->x[0]=0;return 1;
        }
        if(c->session_types[slot]==BOOT_SESSION_FSP+6 && (type==4 || type==6) &&
           (little_read(request+4,4)==24 || little_read(request+4,4)==10)){
            uint32_t words=(uint32_t)little_read(request+4,4),version=type==6?1:0;
            unsigned total=words==24?112:56;
            request=boot_access(c,c->tpidrro,total,READ|WRITE,pc);if(!request)return 0;
            uint32_t command=(uint32_t)little_read(request+24,4),token=(uint32_t)little_read(request+28,4);
            static const uint8_t name[64]="Default";
            int opening=words==24;
            if(!opening && command==5202){
                BootWindow*w=(BootWindow*)(uintptr_t)c->window_native;
                unsigned event_slot=0;while(event_slot<16 && c->event_handles[event_slot])++event_slot;
                if(c->session_domains[slot] || c->session_process_ids[slot]!=1 || !w || event_slot==16 || c->next_session_handle>=0x7ffffffe ||
                   little_read(request+16,4)!=UINT32_C(0x49434653) || little_read(request+20,4)!=version || little_read(request+32,8)!=1){
                    boot_fault(c,BOOT_SVC,pc,0,0);return 0;
                }
                HANDLE event=NULL;AcquireSRWLockExclusive(&w->lock);unsigned group=w->display_groups[slot];
                int valid=!w->closed && !w->error && group && w->display_opens[group-1];
                if(valid && boot_guest_dilated()){
                    /* Guest vsync: guest_time.h signals every event of source 200 at 60 Hz of
                     * guest time. Host compositor timing stays with host presentation. */
                    event=CreateEventW(NULL,TRUE,FALSE,NULL);valid=event!=NULL;
                }else{
                    BootVsync*source=w->vsync;int created=source==NULL;
                    if(valid && !source)source=boot_vsync_open();
                    if(valid && source && !InterlockedCompareExchange(&source->failed,0,0))
                        valid=DuplicateHandle(GetCurrentProcess(),source->event,GetCurrentProcess(),&event,0,FALSE,DUPLICATE_SAME_ACCESS);
                    else valid=0;
                    if(valid)w->vsync=source;else if(created && source)boot_vsync_close(source);
                }
                ReleaseSRWLockExclusive(&w->lock);
                if(!valid){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
                if(c->next_session_handle<0x10000)c->next_session_handle=0x10000;
                uint32_t handle=++c->next_session_handle;c->event_handles[event_slot]=handle;c->event_sources[event_slot]=200;
                c->event_native_handles[event_slot]=(uint64_t)(uintptr_t)event;++c->event_create_count;
                memset(request,0,48);little_write(request+4,UINT32_C(0x80000008),4);little_write(request+8,2,4);little_write(request+12,handle,4);
                little_write(request+16,UINT32_C(0x4f434653),4);little_write(request+20,version,4);little_write(request+28,token,4);c->x[0]=0;return 1;
            }
            if(!opening && command==2021){
                uint32_t unused=0;
                if(c->session_domains[slot] || c->session_process_ids[slot]!=1 ||
                   little_read(request+16,4)!=UINT32_C(0x49434653) || little_read(request+20,4)!=version ||
                   !boot_window_layer((BootWindow*)(uintptr_t)c->window_native,slot,c->applet_resource_user_id,little_read(request+32,8),0,&unused)){
                    boot_fault(c,BOOT_SVC,pc,0,0);return 0;
                }
                memset(request,0,40);little_write(request+4,8,4);little_write(request+16,UINT32_C(0x4f434653),4);
                little_write(request+20,version,4);little_write(request+28,token,4);c->x[0]=0;return 1;
            }
            if(c->session_domains[slot] || c->session_process_ids[slot]!=1 ||
               little_read(request+16,4)!=UINT32_C(0x49434653) || little_read(request+20,4)!=version ||
               command!=(opening?1010u:1020u) || (opening && memcmp(request+32,name,64)) ||
               !boot_window_display((BootWindow*)(uintptr_t)c->window_native,slot,opening,opening?0:little_read(request+32,8))){
                boot_fault(c,BOOT_SVC,pc,c->tpidrro,total);return 0;
            }
            memset(request,0,opening?48:40);little_write(request+4,opening?10:8,4);
            little_write(request+16,UINT32_C(0x4f434653),4);little_write(request+20,version,4);little_write(request+28,token,4);
            if(opening)little_write(request+32,1,8);
            c->x[0]=0;return 1;
        }
        if(c->session_types[slot]==BOOT_SESSION_FSP+6 && (type==4 || type==6)){
            request=boot_access(c,c->tpidrro,48,READ|WRITE,pc);if(!request)return 0;
            uint32_t version=type==6?1:0,token=(uint32_t)little_read(request+28,4);
            if(c->session_domains[slot] || c->session_process_ids[slot]!=1 ||
               little_read(request+4,4)!=8 || little_read(request+16,4)!=UINT32_C(0x49434653) ||
               little_read(request+20,4)!=version || little_read(request+24,4)!=100){
                boot_fault(c,BOOT_SVC,pc,c->tpidrro,48);return 0;
            }
            uint32_t handle=boot_new_session(c,pc,BOOT_SESSION_FSP+7,c->session_process_ids[slot]);if(!handle)return 0;
            memset(request,0,48);little_write(request+4,UINT32_C(0x80000008),4);
            little_write(request+8,0x20,4);little_write(request+12,handle,4);
            little_write(request+16,UINT32_C(0x4f434653),4);little_write(request+20,version,4);
            little_write(request+28,token,4);c->x[0]=0;return 1;
        }
        if(c->session_types[slot]==BOOT_SESSION_FSP+4 && (type==UINT32_C(0x00100004) || type==UINT32_C(0x00100006))){
            request=boot_access(c,c->tpidrro,60,READ|WRITE,pc);if(!request)return 0;
            uint32_t descriptor=(uint32_t)little_read(request+16,4),version=(type&0xffff)==6?1:0;
            uint64_t address=little_read(request+12,4)|((uint64_t)(descriptor>>28)<<32)|((uint64_t)((descriptor>>2)&0x3fffff)<<36);
            uint64_t length=little_read(request+8,4)|((uint64_t)((descriptor>>24)&15)<<32);
            uint32_t token=(uint32_t)little_read(request+44,4);BootWindow*w=(BootWindow*)(uintptr_t)c->window_native;
            if(c->session_domains[slot] || c->session_process_ids[slot]!=1 || !c->nvdrv_transfer_slots[slot] || !w ||
               little_read(request+4,4)!=8 || (descriptor&3)!=0 || (length!=20 && length!=10 && length!=18 && length!=15) || address>UINT64_MAX-length ||
               little_read(request+32,4)!=UINT32_C(0x49434653) || little_read(request+36,4)!=version || little_read(request+40,4)!=0){
                boot_fault(c,BOOT_SVC,pc,0,0);return 0;
            }
            uint8_t*path=boot_access(c,address,length,READ,pc);if(!path)return 0;
            uint32_t device_kind=length==20 && !memcmp(path,"/dev/nvhost-ctrl-gpu",20)?1:
                                 length==10 && !memcmp(path,"/dev/nvmap",10)?2:
                                 length==18 && !memcmp(path,"/dev/nvhost-as-gpu",18)?3:
                                 length==15 && !memcmp(path,"/dev/nvhost-gpu",15)?4:0;
            if(!device_kind){boot_fault(c,BOOT_SVC,pc,address,length);return 0;}
            AcquireSRWLockExclusive(&w->lock);unsigned device=0;while(device<16 && w->nv_device_fds[device])++device;
            int valid=!w->closed && !w->error && device<16 && w->nv_device_next<INT32_MAX;uint32_t fd=0;
            if(valid){fd=++w->nv_device_next;w->nv_device_fds[device]=fd;w->nv_device_kinds[device]=device_kind;}
            ReleaseSRWLockExclusive(&w->lock);
            if(!valid){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
            memset(request,0,48);little_write(request+4,10,4);little_write(request+16,UINT32_C(0x4f434653),4);
            little_write(request+20,version,4);little_write(request+28,token,4);little_write(request+32,fd,4);c->x[0]=0;return 1;
        }
        if(c->session_types[slot]==BOOT_SESSION_FSP+4 && (type==UINT32_C(0x01220004) || type==UINT32_C(0x01220006))){
            /* Validate and stage an entire source submission before any GPU or
             * guest-memory side effect. Unsupported actions remain a stop. */
            request=boot_access(c,c->tpidrro,112,READ|WRITE,pc);if(!request)return 0;
            uint32_t version=(type&0xffff)==6?1:0,fd=(uint32_t)little_read(request+80,4);
            BootWindow*w=(BootWindow*)(uintptr_t)c->window_native;uint64_t addresses[3],sizes[3];uint8_t*buffers[3];
            if(c->session_domains[slot] || c->session_process_ids[slot]!=1 || !c->nvdrv_transfer_slots[slot] || !w || !fd ||
               little_read(request+4,4)!=0xc0b || little_read(request+8,8)!=0 || little_read(request+16,8)!=1 || little_read(request+110,2)!=0 ||
               little_read(request+64,4)!=UINT32_C(0x49434653) || little_read(request+68,4)!=version || little_read(request+72,4)!=11 ||
               little_read(request+84,4)!=UINT32_C(0xc018481b)){
                FILE*log=fopen("submit-reject.txt","a");
                if(log){unsigned m=(c->session_domains[slot]?1:0)|(c->session_process_ids[slot]!=1?2:0)|(!c->nvdrv_transfer_slots[slot]?4:0)|
                         (!w?8:0)|(!fd?16:0)|(little_read(request+4,4)!=0xc0b?32:0)|(little_read(request+8,8)!=0?64:0)|
                         (little_read(request+16,8)!=1?128:0)|(little_read(request+110,2)!=0?256:0)|
                         (little_read(request+68,4)!=version?512:0)|(little_read(request+72,4)!=11?1024:0);
                        fprintf(log,"precondition mask=0x%x slot=%u fd=%u\n",m,slot,fd);fclose(log);}
                boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
            for(unsigned j=0;j<3;++j){unsigned off=24+j*12;uint32_t desc=(uint32_t)little_read(request+off+8,4);
                addresses[j]=little_read(request+off+4,4)|((uint64_t)(desc>>28)<<32)|((uint64_t)((desc>>2)&0x3fffff)<<36);
                sizes[j]=little_read(request+off,4)|((uint64_t)((desc>>24)&15)<<32);
                if((desc&3)!=0 || (j!=1 && sizes[j]!=24) || !sizes[j] || sizes[j]>524288 || addresses[j]>UINT64_MAX-sizes[j] ||
                   (addresses[j]<c->tpidrro+112 && c->tpidrro<addresses[j]+sizes[j])){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
                buffers[j]=boot_access(c,addresses[j],sizes[j],j==2?WRITE:READ,pc);if(!buffers[j])return 0;
            }
            if(addresses[0]!=addresses[2] && addresses[0]<addresses[2]+24 && addresses[2]<addresses[0]+24){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
            if((addresses[1]<addresses[0]+24 && addresses[0]<addresses[1]+sizes[1]) ||
               (addresses[1]<addresses[2]+24 && addresses[2]<addresses[1]+sizes[1])){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
            uint32_t count=(uint32_t)little_read(buffers[0]+8,4),flags=(uint32_t)little_read(buffers[0]+12,4);
            uint32_t increment=(uint32_t)little_read(buffers[0]+20,4);
            if(!count || count>65536 || sizes[1]!=(uint64_t)count*8 || (flags!=4 && flags!=0x104) ||
               little_read(buffers[0]+16,4)!=0 || (flags==4 && increment)){
                FILE*log=fopen("submit-reject.txt","a");
                if(log){fprintf(log,"header count=%u flags=0x%x w16=0x%x incr=%u sizes=%llu/%llu/%llu w0=0x%llx\n",count,flags,
                                (unsigned)little_read(buffers[0]+16,4),increment,(unsigned long long)sizes[0],(unsigned long long)sizes[1],
                                (unsigned long long)sizes[2],(unsigned long long)little_read(buffers[0],8));fclose(log);}
                boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
            AcquireSRWLockExclusive(&w->lock);unsigned device=0;while(device<16 && w->nv_device_fds[device]!=fd)++device;
            int valid=!w->closed && !w->error && device<16 && w->nv_device_kinds[device]==4 &&
                w->nv_channels[device].allocator && !w->nv_channels[device].pending && count<=w->nv_channels[device].capacity;
            BootGpuStage*stage=NULL;BootGpuMethod*methods=NULL;size_t total=0;
            size_t method_capacity=262144;
            for(unsigned j=0;j<count;++j){size_t words=(size_t)((little_read(buffers[1]+j*8+4,4)>>10)&0x1fffff);method_capacity+=words;}
            if(valid){stage=calloc(1,sizeof(*stage));methods=calloc(method_capacity,sizeof(*methods));valid=stage && methods;}
            if(valid && w->nv_channels[device].state){
                memcpy(stage,w->nv_channels[device].state,sizeof(*stage));
#ifdef TOTK_DRAW_PATH
                stage->draw.events=NULL;stage->draw.count=stage->draw.capacity=0;   /* events belong to one submission */
#endif
                stage->write_count=stage->barriers=stage->staged=stage->macro_calls=0;
                stage->error=stage->error_class=stage->error_method=stage->error_value=stage->error_packet=0;
            }
            if(!valid){FILE*log=fopen("submit-reject.txt","a");
                if(log){fprintf(log,"channel device=%u kind=%u allocator=%d pending=%d count=%u capacity=%u closed=%d error=%d\n",device,
                                device<16?w->nv_device_kinds[device]:0,device<16?!!w->nv_channels[device].allocator:-1,
                                device<16?!!w->nv_channels[device].pending:-1,count,device<16?(unsigned)w->nv_channels[device].capacity:0,
                                !!w->closed,!!w->error);fclose(log);}}
            unsigned as_device=0;if(valid)while(as_device<16 && w->nv_device_fds[as_device]!=w->nv_channels[device].as_fd)++as_device;
            if(valid){valid=as_device<16 && w->nv_device_kinds[as_device]==3;if(!valid){FILE*log=fopen("submit-reject.txt","a");if(log){fprintf(log,"address space device=%u kind=%u\n",as_device,as_device<16?w->nv_device_kinds[as_device]:0);fclose(log);}}}
            /* One index of this channel's mappings for every entry (mappings cannot change under the lock). */
            BootGpuSegmentIndex segments;memset(&segments,0,sizeof segments);
            if(valid)boot_gpu_segment_index_build(&segments,&w->gpu_mappings,&w->nv_address_spaces[as_device],w->nv_channels[device].as_fd);
            for(unsigned j=0;valid && j<count;++j){
                uint32_t lo=(uint32_t)little_read(buffers[1]+j*8,4),hi=(uint32_t)little_read(buffers[1]+j*8+4,4);
                uint64_t gpu=(lo&UINT32_C(0xfffffffc))|((uint64_t)(hi&255)<<32),bytes=(uint64_t)((hi>>10)&0x1fffff)*4,cpu=0;
                if((lo&3) || (hi&0x100) || !bytes || bytes>1048576){valid=0;{FILE*log=fopen("submit-reject.txt","a");if(log){fprintf(log,"entry %u/%u lo=0x%x hi=0x%x bytes=%llu\n",j,count,lo,hi,(unsigned long long)bytes);fclose(log);}}break;}
                uint8_t*data=calloc(1,(size_t)bytes);if(!data){valid=0;break;}
                for(uint64_t done=0;valid && done<bytes;){uint64_t length=0;
                    valid=boot_gpu_memory_segment_indexed(&segments,w->nv_channels[device].nvmap_fd,gpu+done,bytes-done,&cpu,&length);
                    if(!valid){FILE*log=fopen("submit-reject.txt","a");if(log){fprintf(log,"segment entry %u/%u gpu=0x%llx done=%llu bytes=%llu\n",j,count,(unsigned long long)gpu,(unsigned long long)done,(unsigned long long)bytes);fclose(log);}}
                    if(valid && cpu){uint8_t*source=boot_access(c,cpu,length,READ,pc);if(!source)valid=0;else memcpy(data+done,source,(size_t)length);}
                    if(valid)done+=length;
                }
                size_t added=0,fault=0;if(valid)valid=boot_gpu_decode_packets((const uint32_t*)data,(size_t)bytes/4,methods+total,method_capacity-total,&added,&fault);
                if(!valid){FILE*log=fopen("submit-reject.txt","a");
                    if(log){fprintf(log,"decode entry %u/%u bytes %llu fault word %zu header 0x%08x total %zu capacity %zu\n",j,count,(unsigned long long)bytes,fault,
                                    fault<bytes/4?(unsigned)little_read(data+fault*4,4):0u,total,method_capacity);fclose(log);}}
                free(data);
                if(valid)total+=added;
            }
            boot_gpu_segment_index_free(&segments);
            uint64_t diagnostic=0;uint32_t value=0;
            if(valid){valid=boot_gpu_stage_methods(stage,methods,total);if(!valid){diagnostic=((uint64_t)stage->error_class<<32)|stage->error_method;value=stage->error_value;
                /* Diagnostic: why the stager stopped, and the decoded method stream of the submission. */
                FILE*dump=fopen("stage-fail.txt","w");
                if(dump){fprintf(dump,"error %u class %x method %x value %x packet %u writes %u methods %zu dma_remaining %u\n",stage->error,stage->error_class,
                                 stage->error_method,stage->error_value,stage->error_packet,stage->write_count,total,stage->dma_remaining);
                    for(size_t k=0;k<total;++k)fprintf(dump,"%zu sub%u %04x %08x\n",k,methods[k].subchannel,methods[k].method,methods[k].value);fclose(dump);}
            }}
#ifdef TOTK_DRAW_PATH
            /* Draws and clears run after the whole submission validated, in order, each seeing
             * the staged writes that precede it; guest memory writes commit afterwards as before. */
            if(valid && stage->draw.count && !boot_gpu_pack_regions(c,w,w->nv_channels[device].as_fd)){
                valid=0;diagnostic=UINT64_C(0xd2a40001);value=0;}
            if(valid && stage->draw.count){valid=boot_draw_execute(c->regions,c->region_count,w,&w->nv_channels[device],stage);if(!valid){diagnostic=UINT64_C(0xd2a40000);value=0;
                /* Diagnostic: the decoded method stream of the failing submission. */
                FILE*dump=fopen("draw-methods-dump.txt","w");
                if(dump){for(size_t k=0;k<total;++k)fprintf(dump,"%zu sub%u %04x %08x\n",k,methods[k].subchannel,methods[k].method,methods[k].value);fclose(dump);}
            }}
#endif
            BootGpuWritePlan plan={0};BootGpuMemorySpan spans[16]={0};
            if(valid)valid=boot_gpu_write_plan_sparse(&w->gpu_mappings,&w->nv_address_spaces[as_device],w->nv_channels[device].as_fd,w->nv_channels[device].nvmap_fd,boot_gpu_stage_writes(stage),stage->write_count,&plan);
            for(unsigned j=0;valid && j<plan.count;++j){
                BootGpuMapping*m=&w->gpu_mappings.entries[plan.spans[j].mapping];
                BootNvMapObject*object=boot_nvmap_find(&w->nvmap,m->owner_fd,m->handle);
                uint64_t cpu=plan.spans[j].cpu,size=plan.spans[j].size;
                valid=object && object->allocated && cpu>=object->address && size<=object->size && cpu-object->address<=object->size-size;
                if(valid && cpu<c->tpidrro+112 && c->tpidrro<cpu+size)valid=0;
                for(unsigned k=0;valid && k<3;++k)if(cpu<addresses[k]+sizes[k] && addresses[k]<cpu+size)valid=0;
                if(!valid)break;
                spans[j].destination=boot_access(c,cpu,size,READ|WRITE,pc);spans[j].size=(size_t)size;
                uint8_t*contents=malloc((size_t)size);spans[j].contents=contents;
                if(!spans[j].destination || !contents){valid=0;break;}
                memcpy(contents,spans[j].destination,(size_t)size);
            }
            /* Reports execute in source order into private backing. Source
             * state and the fence commit only after verified queue completion. */
            int planned=valid;
            if(valid)valid=boot_gpu_sync_memory_execute(w->gpu,&plan,spans,boot_gpu_stage_writes(stage),stage->write_count,
                w->nv_channels[device].syncpoint,increment,&w->nv_channels[device].pending);
            if(!valid && !diagnostic){FILE*log=fopen("submit-reject.txt","a");   /* diagnostic: post-draw commit failure */
                if(log){fprintf(log,"commit: planned=%d plan_count=%u writes=%u draws=%llu\n",planned,plan.count,stage?stage->write_count:0,
                                (unsigned long long)0);
                    for(unsigned j=0;j<plan.count;++j){fprintf(log,"  span %u gpu 0x%llx cpu 0x%llx size 0x%llx\n",j,(unsigned long long)plan.spans[j].gpu,
                        (unsigned long long)plan.spans[j].cpu,(unsigned long long)plan.spans[j].size);}
                    fclose(log);}}
            for(unsigned j=0;j<16;++j)free((void*)spans[j].contents);
            if(valid){
#ifdef TOTK_DRAW_PATH
                free(stage->draw.events);stage->draw.events=NULL;stage->draw.count=stage->draw.capacity=0;
#endif
                BootGpuChannel*channel=&w->nv_channels[device];free(channel->state);channel->state=stage;stage=NULL;++channel->verified_submissions;
                channel->syncpoint_value+=increment;
                uint8_t reply[24];memcpy(reply,buffers[0],24);little_write(reply+16,channel->syncpoint,4);little_write(reply+20,channel->syncpoint_value,4);
                uint32_t token=(uint32_t)little_read(request+76,4);memcpy(buffers[2],reply,24);
                memset(request,0,44);little_write(request+4,9,4);little_write(request+16,UINT32_C(0x4f434653),4);
                little_write(request+20,version,4);little_write(request+28,token,4);c->x[0]=0;
            }
#ifdef TOTK_DRAW_PATH
            if(stage)free(stage->draw.events);
#endif
            free(methods);free(stage);ReleaseSRWLockExclusive(&w->lock);
            if(valid)return 1;
            if(!c->status)boot_fault(c,BOOT_SVC,pc,diagnostic,value);return 0;
        }
        if(c->session_types[slot]==BOOT_SESSION_FSP+4 && (type==UINT32_C(0x01110004) || type==UINT32_C(0x01110006) ||
           ((type==UINT32_C(0x02110004) || type==UINT32_C(0x02110006)) && little_read(request+84,4)==UINT32_C(0xc0404108)))){
            /* Initialize an empty source GPU virtual-address domain. This is
             * bookkeeping only; it does not map RAM or submit GPU commands. */
            int inline_regions=(type>>24)==2;unsigned cmif=inline_regions?64:48,total=inline_regions?112:92;
            request=boot_access(c,c->tpidrro,total,READ|WRITE,pc);if(!request)return 0;
            if(!inline_regions && (little_read(request+68,4)==UINT32_C(0xc020481a) || little_read(request+68,4)==UINT32_C(0xc0104809) || little_read(request+68,4)==UINT32_C(0xc018480c) || little_read(request+68,4)==UINT32_C(0xc004481d))){
                int context=little_read(request+68,4)==UINT32_C(0xc0104809),notifier=little_read(request+68,4)==UINT32_C(0xc018480c);
                int timeslice=little_read(request+68,4)==UINT32_C(0xc004481d);
                unsigned length=timeslice?4:notifier?24:context?16:32;
                uint32_t version=(type&0xffff)==6?1:0,token=(uint32_t)little_read(request+60,4),fd=(uint32_t)little_read(request+64,4);
                BootWindow*w=(BootWindow*)(uintptr_t)c->window_native;uint64_t addresses[2];uint8_t*buffers[2];
                if(c->session_domains[slot] || c->session_process_ids[slot]!=1 || !c->nvdrv_transfer_slots[slot] || !w || !fd ||
                   little_read(request+4,4)!=0xc0b || little_read(request+8,8)!=0 || little_read(request+90,2)!=0 ||
                   little_read(request+48,4)!=UINT32_C(0x49434653) || little_read(request+52,4)!=version || little_read(request+56,4)!=1){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
                for(unsigned i=0;i<2;++i){unsigned off=16+i*12;uint32_t desc=(uint32_t)little_read(request+off+8,4);
                    addresses[i]=little_read(request+off+4,4)|((uint64_t)(desc>>28)<<32)|((uint64_t)((desc>>2)&0x3fffff)<<36);
                    if(little_read(request+off,4)!=length || ((desc>>24)&15)!=0 || (desc&3)!=0 || addresses[i]>UINT64_MAX-length ||
                       (addresses[i]<c->tpidrro+92 && c->tpidrro<addresses[i]+length)){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
                    buffers[i]=boot_access(c,addresses[i],length,i?WRITE:READ,pc);if(!buffers[i])return 0;
                }
                if(addresses[0]!=addresses[1] && addresses[0]<addresses[1]+length && addresses[1]<addresses[0]+length){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
                uint8_t reply[32]={0};memcpy(reply,buffers[0],length);
                if(!context && !notifier && !timeslice && (little_read(reply+4,4)!=1 || little_read(reply+8,4)!=0 || little_read(reply+20,8)!=0 || little_read(reply+28,4)!=0)){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
                AcquireSRWLockExclusive(&w->lock);unsigned device=0;while(device<16 && w->nv_device_fds[device]!=fd)++device;
                int valid=!w->closed && !w->error && device<16 && w->nv_device_kinds[device]==4;
                if(valid && !timeslice)valid=notifier?boot_gpu_channel_notifier(&w->nv_channels[device],(uint32_t)little_read(reply+16,4)):
                    context?boot_gpu_channel_context(&w->nv_channels[device],w->gpu,(uint32_t)little_read(reply,4),(uint32_t)little_read(reply+4,4)):
                    boot_gpu_channel_allocate(&w->nv_channels[device],&w->gpu,&w->nv_syncpoint_next,(uint32_t)little_read(reply,4));
                if(valid && !notifier && !timeslice){if(context)little_write(reply+8,fd,8);else{little_write(reply+12,w->nv_channels[device].syncpoint,4);little_write(reply+16,0,4);}}
                ReleaseSRWLockExclusive(&w->lock);if(!valid){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
                memcpy(buffers[1],reply,length);memset(request,0,44);little_write(request+4,9,4);little_write(request+16,UINT32_C(0x4f434653),4);
                little_write(request+20,version,4);little_write(request+28,token,4);
                /* Exact hardware scheduling slices are unavailable through D3D12. */
                little_write(request+32,timeslice?2:0,4);c->x[0]=0;return 1;
            }
            if(!inline_regions && (little_read(request+68,4)==UINT32_C(0x40084714) || little_read(request+68,4)==UINT32_C(0x40044803))){
                int timeout=little_read(request+68,4)==UINT32_C(0x40044803);unsigned length=timeout?4:8;
                uint32_t version=(type&0xffff)==6?1:0,token=(uint32_t)little_read(request+60,4),fd=(uint32_t)little_read(request+64,4);
                uint32_t desc=(uint32_t)little_read(request+24,4);BootWindow*w=(BootWindow*)(uintptr_t)c->window_native;
                uint64_t address=little_read(request+20,4)|((uint64_t)(desc>>28)<<32)|((uint64_t)((desc>>2)&0x3fffff)<<36);
                if(c->session_domains[slot] || c->session_process_ids[slot]!=1 || !c->nvdrv_transfer_slots[slot] || !w || !fd ||
                   little_read(request+4,4)!=0xc0b || little_read(request+8,8)!=0 || little_read(request+90,2)!=0 ||
                   little_read(request+16,4)!=length || ((desc>>24)&15)!=0 || (desc&3)!=0 || little_read(request+28,8)!=0 || little_read(request+36,4)!=0 ||
                   address>UINT64_MAX-length || (address<c->tpidrro+92 && c->tpidrro<address+length) ||
                   little_read(request+48,4)!=UINT32_C(0x49434653) || little_read(request+52,4)!=version || little_read(request+56,4)!=1){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
                uint8_t*input=boot_access(c,address,length,READ,pc);if(!input)return 0;uint64_t value=little_read(input,length);
                AcquireSRWLockExclusive(&w->lock);unsigned device=0;while(device<16 && w->nv_device_fds[device]!=fd)++device;
                int valid=!w->closed && !w->error && device<16 && w->nv_device_kinds[device]==4;
                if(valid){if(timeout)w->nv_channels[device].timeout_ms=(uint32_t)value;else w->nv_channels[device].user_data=value;}
                ReleaseSRWLockExclusive(&w->lock);if(!valid){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
                memset(request,0,44);little_write(request+4,9,4);little_write(request+16,UINT32_C(0x4f434653),4);
                little_write(request+20,version,4);little_write(request+28,token,4);c->x[0]=0;return 1;
            }
            if(!inline_regions && (little_read(request+68,4)==UINT32_C(0x40044101) || little_read(request+68,4)==UINT32_C(0x40044801))){
                int bind_map=little_read(request+68,4)==UINT32_C(0x40044801);
                uint32_t version=(type&0xffff)==6?1:0,token=(uint32_t)little_read(request+60,4),fd=(uint32_t)little_read(request+64,4);
                uint32_t desc=(uint32_t)little_read(request+24,4);BootWindow*w=(BootWindow*)(uintptr_t)c->window_native;
                uint64_t address=little_read(request+20,4)|((uint64_t)(desc>>28)<<32)|((uint64_t)((desc>>2)&0x3fffff)<<36);
                if(c->session_domains[slot] || c->session_process_ids[slot]!=1 || !c->nvdrv_transfer_slots[slot] || !w || !fd ||
                   little_read(request+4,4)!=0xc0b || little_read(request+8,8)!=0 || little_read(request+90,2)!=0 ||
                   little_read(request+16,4)!=4 || ((desc>>24)&15)!=0 || (desc&3)!=0 || little_read(request+28,8)!=0 || little_read(request+36,4)!=0 ||
                   address>UINT64_MAX-4 || (address<c->tpidrro+92 && c->tpidrro<address+4) ||
                   little_read(request+48,4)!=UINT32_C(0x49434653) || little_read(request+52,4)!=version || little_read(request+56,4)!=1){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
                uint8_t*input=boot_access(c,address,4,READ,pc);if(!input)return 0;uint32_t channel_fd=(uint32_t)little_read(input,4);
                AcquireSRWLockExclusive(&w->lock);unsigned domain=0,channel=0;
                while(domain<16 && w->nv_device_fds[domain]!=fd)++domain;
                while(channel<16 && w->nv_device_fds[channel]!=channel_fd)++channel;
                int valid=!w->closed && !w->error && domain<16 && channel<16 && channel_fd &&
                    (bind_map?(w->nv_device_kinds[domain]==4 && w->nv_device_kinds[channel]==2 && !w->nv_channels[domain].nvmap_fd):
                    (w->nv_device_kinds[domain]==3 && w->nv_address_spaces[domain].initialized && w->nv_device_kinds[channel]==4 && !w->nv_channels[channel].as_fd));
                if(valid){if(bind_map)w->nv_channels[domain].nvmap_fd=channel_fd;else w->nv_channels[channel].as_fd=fd;}
                ReleaseSRWLockExclusive(&w->lock);if(!valid){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
                memset(request,0,44);little_write(request+4,9,4);little_write(request+16,UINT32_C(0x4f434653),4);
                little_write(request+20,version,4);little_write(request+28,token,4);c->x[0]=0;return 1;
            }
            if(!inline_regions && (little_read(request+68,4)==UINT32_C(0xc0284106) || little_read(request+68,4)==UINT32_C(0xc0084105))){
                int unmapping=little_read(request+68,4)==UINT32_C(0xc0084105);unsigned length=unmapping?8:40;
                uint32_t version=(type&0xffff)==6?1:0,token=(uint32_t)little_read(request+60,4),fd=(uint32_t)little_read(request+64,4);
                BootWindow*w=(BootWindow*)(uintptr_t)c->window_native;uint64_t addresses[2];uint8_t*buffers[2];
                if(c->session_domains[slot] || c->session_process_ids[slot]!=1 || !c->nvdrv_transfer_slots[slot] || !w || !fd ||
                   little_read(request+4,4)!=0xc0b || little_read(request+8,8)!=0 || little_read(request+90,2)!=0 ||
                   little_read(request+48,4)!=UINT32_C(0x49434653) || little_read(request+52,4)!=version || little_read(request+56,4)!=1){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
                for(unsigned i=0;i<2;++i){unsigned off=16+i*12;uint32_t desc=(uint32_t)little_read(request+off+8,4);
                    addresses[i]=little_read(request+off+4,4)|((uint64_t)(desc>>28)<<32)|((uint64_t)((desc>>2)&0x3fffff)<<36);
                    if(little_read(request+off,4)!=length || ((desc>>24)&15)!=0 || (desc&3)!=0 || addresses[i]>UINT64_MAX-length ||
                       (addresses[i]<c->tpidrro+92 && c->tpidrro<addresses[i]+length)){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
                    buffers[i]=boot_access(c,addresses[i],length,i?WRITE:READ,pc);if(!buffers[i])return 0;
                }
                if(addresses[0]!=addresses[1] && addresses[0]<addresses[1]+length && addresses[1]<addresses[0]+length){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
                {uint64_t fa=0,fb=0;int rc=boot_nvdrv_mapping_ioctl(c,pc,fd,(uint32_t)little_read(request+68,4),buffers[0],buffers[1],&fa,&fb);
                 if(rc<0)return 0;if(!rc){if(!c->status)boot_fault(c,BOOT_SVC,pc,fa,fb);return 0;}}
                memset(request,0,44);little_write(request+4,9,4);little_write(request+16,UINT32_C(0x4f434653),4);
                little_write(request+20,version,4);little_write(request+28,token,4);c->x[0]=0;return 1;
            }
            if(!inline_regions && (little_read(request+68,4)==UINT32_C(0xc008010e) || little_read(request+68,4)==UINT32_C(0xc0080101) || little_read(request+68,4)==UINT32_C(0xc0180105) || little_read(request+68,4)==UINT32_C(0xc0200104) || little_read(request+68,4)==UINT32_C(0xc00c0109) || little_read(request+68,4)==UINT32_C(0xc0080103))){
                int identifying=little_read(request+68,4)==UINT32_C(0xc008010e);(void)identifying;
                int freeing=little_read(request+68,4)==UINT32_C(0xc0180105),allocating=little_read(request+68,4)==UINT32_C(0xc0200104);int querying=little_read(request+68,4)==UINT32_C(0xc00c0109);unsigned length=querying?12:allocating?32:freeing?24:8;
                uint32_t version=(type&0xffff)==6?1:0,token=(uint32_t)little_read(request+60,4),fd=(uint32_t)little_read(request+64,4);
                BootWindow*w=(BootWindow*)(uintptr_t)c->window_native;uint64_t addresses[2];uint8_t*buffers[2];
                if(c->session_domains[slot] || c->session_process_ids[slot]!=1 || !c->nvdrv_transfer_slots[slot] || !w || !fd ||
                   little_read(request+4,4)!=0xc0b || little_read(request+8,8)!=0 || little_read(request+90,2)!=0 ||
                   little_read(request+48,4)!=UINT32_C(0x49434653) || little_read(request+52,4)!=version || little_read(request+56,4)!=1){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
                for(unsigned i=0;i<2;++i){unsigned off=16+i*12;uint32_t desc=(uint32_t)little_read(request+off+8,4);
                    addresses[i]=little_read(request+off+4,4)|((uint64_t)(desc>>28)<<32)|((uint64_t)((desc>>2)&0x3fffff)<<36);
                    if(little_read(request+off,4)!=length || ((desc>>24)&15)!=0 || (desc&3)!=0 || addresses[i]>UINT64_MAX-length ||
                       (addresses[i]<c->tpidrro+92 && c->tpidrro<addresses[i]+length)){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
                    buffers[i]=boot_access(c,addresses[i],length,i?WRITE:READ,pc);if(!buffers[i])return 0;
                }
                if(addresses[0]!=addresses[1] && addresses[0]<addresses[1]+length && addresses[1]<addresses[0]+length){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
                {uint64_t fa=0,fb=0;int rc=boot_nvdrv_mapping_ioctl(c,pc,fd,(uint32_t)little_read(request+68,4),buffers[0],buffers[1],&fa,&fb);
                 if(rc<0)return 0;if(!rc){if(!c->status)boot_fault(c,BOOT_SVC,pc,fa,fb);return 0;}
                 if(rc==2){memset(request,0,44);little_write(request+4,9,4);little_write(request+16,UINT32_C(0x4f434653),4);   /* driver's NvResult */
                     little_write(request+20,version,4);little_write(request+28,token,4);little_write(request+32,fa,4);c->x[0]=0;return 1;}}
                memset(request,0,44);little_write(request+4,9,4);little_write(request+16,UINT32_C(0x4f434653),4);
                little_write(request+20,version,4);little_write(request+28,token,4);c->x[0]=0;return 1;
            }
            if(!inline_regions && (little_read(request+68,4)&UINT32_C(0xc000ffff))==UINT32_C(0xc0004114)){
                uint32_t ioctl=(uint32_t)little_read(request+68,4),length=(ioctl>>16)&0x3fff;
                uint32_t version=(type&0xffff)==6?1:0,token=(uint32_t)little_read(request+60,4),fd=(uint32_t)little_read(request+64,4);
                BootWindow*w=(BootWindow*)(uintptr_t)c->window_native;uint64_t addresses[2];uint8_t*buffers[2];
                if(!length || length%20 || length>10240 || c->session_domains[slot] || c->session_process_ids[slot]!=1 || !c->nvdrv_transfer_slots[slot] || !w || !fd ||
                   little_read(request+4,4)!=0xc0b || little_read(request+8,8)!=0 || little_read(request+90,2)!=0 ||
                   little_read(request+48,4)!=UINT32_C(0x49434653) || little_read(request+52,4)!=version || little_read(request+56,4)!=1){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
                for(unsigned i=0;i<2;++i){unsigned off=16+i*12;uint32_t desc=(uint32_t)little_read(request+off+8,4);
                    addresses[i]=little_read(request+off+4,4)|((uint64_t)(desc>>28)<<32)|((uint64_t)((desc>>2)&0x3fffff)<<36);
                    if(little_read(request+off,4)!=length || ((desc>>24)&15)!=0 || (desc&3)!=0 || addresses[i]>UINT64_MAX-length ||
                       (addresses[i]<c->tpidrro+92 && c->tpidrro<addresses[i]+length)){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
                    buffers[i]=boot_access(c,addresses[i],length,i?WRITE:READ,pc);if(!buffers[i])return 0;
                }
                if(addresses[0]!=addresses[1] && addresses[0]<addresses[1]+length && addresses[1]<addresses[0]+length){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
                {uint64_t fa=0,fb=0;int rc=boot_nvdrv_mapping_ioctl(c,pc,fd,(uint32_t)little_read(request+68,4),buffers[0],buffers[1],&fa,&fb);
                 if(rc<0)return 0;if(!rc){if(!c->status)boot_fault(c,BOOT_SVC,pc,fa,fb);return 0;}}
                memset(request,0,44);little_write(request+4,9,4);little_write(request+16,UINT32_C(0x4f434653),4);
                little_write(request+20,version,4);little_write(request+28,token,4);c->x[0]=0;return 1;
            }
            if(!inline_regions && (little_read(request+68,4)==UINT32_C(0xc0184102) || little_read(request+68,4)==UINT32_C(0xc0104103))){
                int freeing=little_read(request+68,4)==UINT32_C(0xc0104103);unsigned length=freeing?16:24;
                uint32_t version=(type&0xffff)==6?1:0,token=(uint32_t)little_read(request+60,4),fd=(uint32_t)little_read(request+64,4);
                BootWindow*w=(BootWindow*)(uintptr_t)c->window_native;uint64_t addresses[2];uint8_t*buffers[2];
                if(c->session_domains[slot] || c->session_process_ids[slot]!=1 || !c->nvdrv_transfer_slots[slot] || !w || !fd ||
                   little_read(request+4,4)!=0xc0b || little_read(request+8,8)!=0 || little_read(request+90,2)!=0 ||
                   little_read(request+48,4)!=UINT32_C(0x49434653) || little_read(request+52,4)!=version || little_read(request+56,4)!=1){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
                for(unsigned i=0;i<2;++i){unsigned off=16+i*12;uint32_t desc=(uint32_t)little_read(request+off+8,4);
                    addresses[i]=little_read(request+off+4,4)|((uint64_t)(desc>>28)<<32)|((uint64_t)((desc>>2)&0x3fffff)<<36);
                    if(little_read(request+off,4)!=length || ((desc>>24)&15)!=0 || (desc&3)!=0 || addresses[i]>UINT64_MAX-length ||
                       (addresses[i]<c->tpidrro+92 && c->tpidrro<addresses[i]+length)){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
                    buffers[i]=boot_access(c,addresses[i],length,i?WRITE:READ,pc);if(!buffers[i])return 0;
                }
                if(addresses[0]!=addresses[1] && addresses[0]<addresses[1]+length && addresses[1]<addresses[0]+length){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
                {uint64_t fa=0,fb=0;int rc=boot_nvdrv_mapping_ioctl(c,pc,fd,(uint32_t)little_read(request+68,4),buffers[0],buffers[1],&fa,&fb);
                 if(rc<0)return 0;if(!rc){if(!c->status)boot_fault(c,BOOT_SVC,pc,fa,fb);return 0;}}
                memset(request,0,44);little_write(request+4,9,4);little_write(request+16,UINT32_C(0x4f434653),4);
                little_write(request+20,version,4);little_write(request+28,token,4);c->x[0]=0;return 1;
            }
            if(little_read(request+cmif+20,4)==UINT32_C(0xc0404108)){
                uint32_t version=(type&0xffff)==6?1:0,token=(uint32_t)little_read(request+cmif+12,4),fd=(uint32_t)little_read(request+cmif+16,4);
                BootWindow*w=(BootWindow*)(uintptr_t)c->window_native;uint64_t addresses[3];uint8_t*buffers[3];
                if(c->session_domains[slot] || c->session_process_ids[slot]!=1 || !c->nvdrv_transfer_slots[slot] || !w || !fd ||
                   little_read(request+4,4)!=(inline_regions?0x100b:0xc0b) || little_read(request+8,8)!=0 ||
                   (inline_regions?(little_read(request+96,8)!=0 || little_read(request+104,8)!=0):little_read(request+90,2)!=0) ||
                   little_read(request+cmif,4)!=UINT32_C(0x49434653) || little_read(request+cmif+4,4)!=version || little_read(request+cmif+8,4)!=(inline_regions?12:1)){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
                for(unsigned i=0;i<(inline_regions?3u:2u);++i){unsigned off=16+i*12;uint32_t desc=(uint32_t)little_read(request+off+8,4);uint64_t length=i==2?48:64;
                    addresses[i]=little_read(request+off+4,4)|((uint64_t)(desc>>28)<<32)|((uint64_t)((desc>>2)&0x3fffff)<<36);
                    if(little_read(request+off,4)!=length || ((desc>>24)&15)!=0 || (desc&3)!=0 || addresses[i]>UINT64_MAX-length ||
                       (addresses[i]<c->tpidrro+total && c->tpidrro<addresses[i]+length)){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
                    buffers[i]=boot_access(c,addresses[i],length,i?WRITE:READ,pc);if(!buffers[i])return 0;
                }
                if(inline_regions && ((addresses[2]<addresses[0]+64 && addresses[0]<addresses[2]+48) ||
                   (addresses[2]<addresses[1]+64 && addresses[1]<addresses[2]+48))){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
                uint32_t size=(uint32_t)little_read(buffers[0]+8,4);
                if(little_read(buffers[0],8)!=0 || little_read(buffers[0]+12,4)!=0 || (size!=0 && size!=48) ||
                   (addresses[0]!=addresses[1] && addresses[0]<addresses[1]+64 && addresses[1]<addresses[0]+64)){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
                uint8_t reply[64]={0};little_write(reply+8,48,4);
                AcquireSRWLockShared(&w->lock);unsigned device=0;while(device<16 && w->nv_device_fds[device]!=fd)++device;
                int valid=!w->closed && !w->error && device<16 && w->nv_device_kinds[device]==3 && w->nv_address_spaces[device].initialized;
                if(valid){uint64_t start=w->nv_address_spaces[device].start,split=w->nv_address_spaces[device].split,end=w->nv_address_spaces[device].end;
                    uint32_t page=w->nv_address_spaces[device].page_size;
                    little_write(reply+16,start,8);little_write(reply+24,4096,4);little_write(reply+32,(split-start)/4096,8);
                    little_write(reply+40,split,8);little_write(reply+48,page,4);little_write(reply+56,(end-split)/page,8);}
                ReleaseSRWLockShared(&w->lock);if(!valid){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
                memcpy(buffers[1],reply,64);if(inline_regions)memcpy(buffers[2],reply+16,48);
                memset(request,0,44);little_write(request+4,9,4);little_write(request+16,UINT32_C(0x4f434653),4);
                little_write(request+20,version,4);little_write(request+28,token,4);c->x[0]=0;return 1;
            }
            if(little_read(request+68,4)==UINT32_C(0x40284109)){
                uint32_t version=(type&0xffff)==6?1:0,token=(uint32_t)little_read(request+60,4);
                uint32_t fd=(uint32_t)little_read(request+64,4),desc=(uint32_t)little_read(request+24,4);
                uint64_t address=little_read(request+20,4)|((uint64_t)(desc>>28)<<32)|((uint64_t)((desc>>2)&0x3fffff)<<36);
                BootWindow*w=(BootWindow*)(uintptr_t)c->window_native;
                if(c->session_domains[slot] || c->session_process_ids[slot]!=1 || !c->nvdrv_transfer_slots[slot] || !w || !fd ||
                   little_read(request+4,4)!=0xc0b || little_read(request+8,8)!=0 || little_read(request+16,4)!=40 ||
                   (desc&3)!=0 || ((desc>>24)&15)!=0 || little_read(request+28,8)!=0 || little_read(request+36,4)!=0 ||
                   little_read(request+90,2)!=0 || address>UINT64_MAX-40 ||
                   (address<c->tpidrro+92 && c->tpidrro<address+40) ||
                   little_read(request+48,4)!=UINT32_C(0x49434653) || little_read(request+52,4)!=version || little_read(request+56,4)!=1){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
                uint8_t*input=boot_access(c,address,40,READ,pc);if(!input)return 0;
                uint32_t page=(uint32_t)little_read(input+8,4);
                uint64_t start=little_read(input+16,8),end=little_read(input+24,8),split=little_read(input+32,8);
                if(little_read(input,8)!=1 || little_read(input+12,4)!=0 || page!=65536 || !start ||
                   start>=split || split>=end || end>(UINT64_C(1)<<40) || ((start|end|split)&(page-1))){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
                AcquireSRWLockExclusive(&w->lock);unsigned device=0;while(device<16 && w->nv_device_fds[device]!=fd)++device;
                int valid=!w->closed && !w->error && device<16 && w->nv_device_kinds[device]==3 && !w->nv_address_spaces[device].initialized;
                if(valid){w->nv_address_spaces[device].start=start;w->nv_address_spaces[device].end=end;w->nv_address_spaces[device].split=split;
                    w->nv_address_spaces[device].page_size=page;w->nv_address_spaces[device].initialized=1;}
                ReleaseSRWLockExclusive(&w->lock);if(!valid){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
                memset(request,0,44);little_write(request+4,9,4);little_write(request+16,UINT32_C(0x4f434653),4);
                little_write(request+20,version,4);little_write(request+28,token,4);c->x[0]=0;return 1;
            }
            if(little_read(request+68,4)==UINT32_C(0x402c4703) || little_read(request+68,4)==UINT32_C(0xc0344704)){
                int query=little_read(request+68,4)==UINT32_C(0xc0344704);
                uint32_t length=query?52:44,version=(type&0xffff)==6?1:0,token=(uint32_t)little_read(request+60,4);
                uint32_t fd=(uint32_t)little_read(request+64,4);BootWindow*w=(BootWindow*)(uintptr_t)c->window_native;
                if(c->session_domains[slot] || c->session_process_ids[slot]!=1 || !c->nvdrv_transfer_slots[slot] || !w || !fd ||
                   little_read(request+4,4)!=0xc0b || little_read(request+8,8)!=0 || little_read(request+90,2)!=0 ||
                   little_read(request+48,4)!=UINT32_C(0x49434653) || little_read(request+52,4)!=version || little_read(request+56,4)!=1){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
                uint64_t addresses[2];uint8_t*buffers[2]={0};
                for(unsigned i=0;i<2;++i){unsigned off=16+i*12;uint32_t desc=(uint32_t)little_read(request+off+8,4),size=i&&!query?0:length;
                    addresses[i]=little_read(request+off+4,4)|((uint64_t)(desc>>28)<<32)|((uint64_t)((desc>>2)&0x3fffff)<<36);
                    if(little_read(request+off,4)!=size || (desc&3)!=0 || ((desc>>24)&15)!=0 || addresses[i]>UINT64_MAX-size ||
                       (size && addresses[i]<c->tpidrro+92 && c->tpidrro<addresses[i]+size) || (!size && addresses[i])){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
                    if(size){buffers[i]=boot_access(c,addresses[i],size,i?WRITE:READ,pc);if(!buffers[i])return 0;}
                }
                if(query && addresses[0]!=addresses[1] && addresses[0]<addresses[1]+length && addresses[1]<addresses[0]+length){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
                uint32_t values[13]={0};for(unsigned i=0;i<length/4;++i)values[i]=(uint32_t)little_read(buffers[0]+i*4,4);
                AcquireSRWLockExclusive(&w->lock);unsigned device=0;while(device<16 && w->nv_device_fds[device]!=fd)++device;
                int valid=!w->closed && !w->error && device<16 && w->nv_device_kinds[device]==1;
                if(valid)valid=query?boot_zbc_query(&w->zbc,values):boot_zbc_set(&w->zbc,values);
                ReleaseSRWLockExclusive(&w->lock);if(!valid){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
                if(query)for(unsigned i=0;i<13;++i)little_write(buffers[1]+i*4,values[i],4);
                memset(request,0,44);little_write(request+4,9,4);little_write(request+16,UINT32_C(0x4f434653),4);
                little_write(request+20,version,4);little_write(request+28,token,4);c->x[0]=0;return 1;
            }
            /* Z-cull images and ZBC hardware slot-mask queries are not exposed.
             * Return the NV driver's explicit NotSupported result, not a made-up
             * context size. The observed SDK disables this optional feature. */
            request=boot_access(c,c->tpidrro,92,READ|WRITE,pc);if(!request)return 0;
            uint32_t version=(type&0xffff)==6?1:0,token=(uint32_t)little_read(request+60,4);
            uint32_t fd=(uint32_t)little_read(request+64,4);BootWindow*w=(BootWindow*)(uintptr_t)c->window_native;
            uint32_t ioctl=(uint32_t)little_read(request+68,4),length=ioctl==UINT32_C(0x80084714)?8:4;
            uint32_t in_desc=(uint32_t)little_read(request+24,4),out_desc=(uint32_t)little_read(request+36,4);
            uint64_t address=little_read(request+32,4)|((uint64_t)(out_desc>>28)<<32)|((uint64_t)((out_desc>>2)&0x3fffff)<<36);
            if(c->session_domains[slot] || c->session_process_ids[slot]!=1 || !c->nvdrv_transfer_slots[slot] || !w || !fd ||
               little_read(request+4,4)!=0xc0b || little_read(request+8,8)!=0 || little_read(request+16,4)!=0 ||
               (in_desc&3)!=0 || ((in_desc>>24)&15)!=0 || little_read(request+28,4)!=length ||
               (out_desc&3)!=0 || ((out_desc>>24)&15)!=0 || address>UINT64_MAX-length ||
               (address<c->tpidrro+92 && c->tpidrro<address+length) || little_read(request+84,8)!=0 ||
               little_read(request+48,4)!=UINT32_C(0x49434653) || little_read(request+52,4)!=version ||
               little_read(request+56,4)!=1 || (ioctl!=UINT32_C(0x80044701) && ioctl!=UINT32_C(0x80084714))){
                boot_fault(c,BOOT_SVC,pc,0,0);return 0;
            }
            if(!boot_access(c,address,length,WRITE,pc))return 0;
            AcquireSRWLockShared(&w->lock);unsigned device=0;while(device<16 && w->nv_device_fds[device]!=fd)++device;
            int valid=!w->closed && !w->error && device<16 && w->nv_device_kinds[device]==1;
            ReleaseSRWLockShared(&w->lock);if(!valid){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
            memset(request,0,44);little_write(request+4,9,4);little_write(request+16,UINT32_C(0x4f434653),4);
            little_write(request+20,version,4);little_write(request+28,token,4);little_write(request+32,2,4);c->x[0]=0;return 1;
        }
        if(c->session_types[slot]==BOOT_SESSION_FSP+4 && (type==UINT32_C(0x02110004) || type==UINT32_C(0x02110006))){
            /* Observed Ioctl3 map-alias layout. This identifies the source GPU
             * ABI, not the DXGI adapter or implemented host GPU operations.
             * Unknown device operations still fail closed below. */
            request=boot_access(c,c->tpidrro,112,READ|WRITE,pc);if(!request)return 0;
            uint32_t version=(type&0xffff)==6?1:0,token=(uint32_t)little_read(request+76,4);
            uint32_t fd=(uint32_t)little_read(request+80,4);BootWindow*w=(BootWindow*)(uintptr_t)c->window_native;
            uint32_t ioctl=(uint32_t)little_read(request+84,4);int tpc=ioctl==UINT32_C(0xc0184706);
            uint64_t main_size=tpc?24:176,inline_size=tpc?4:160;
            if(c->session_domains[slot] || c->session_process_ids[slot]!=1 || !c->nvdrv_transfer_slots[slot] || !w || !fd ||
               little_read(request+4,4)!=0x100b || little_read(request+8,8)!=0 ||
               little_read(request+64,4)!=UINT32_C(0x49434653) || little_read(request+68,4)!=version ||
               little_read(request+72,4)!=12 || (!tpc && ioctl!=UINT32_C(0xc0b04705)) ||
               little_read(request+96,8)!=0 || little_read(request+104,8)!=0){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
            uint64_t addresses[3],lengths[3];uint8_t*buffers[3];
            for(unsigned i=0;i<3;++i){
                unsigned off=16+i*12;uint32_t desc=(uint32_t)little_read(request+off+8,4);
                addresses[i]=little_read(request+off+4,4)|((uint64_t)(desc>>28)<<32)|((uint64_t)((desc>>2)&0x3fffff)<<36);
                lengths[i]=little_read(request+off,4)|((uint64_t)((desc>>24)&15)<<32);
                if((desc&3)!=0 || lengths[i]!=(i==2?inline_size:main_size) || addresses[i]>UINT64_MAX-lengths[i] ||
                   (addresses[i]<c->tpidrro+112 && c->tpidrro<addresses[i]+lengths[i])){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
                buffers[i]=boot_access(c,addresses[i],lengths[i],i?WRITE:READ,pc);if(!buffers[i])return 0;
            }
            /* The two main buffers must be identical or disjoint. Inline output
             * must be disjoint from both, so commit order cannot corrupt data. */
            if((addresses[0]!=addresses[1] && addresses[0]<addresses[1]+main_size && addresses[1]<addresses[0]+main_size) ||
               (addresses[2]<addresses[0]+main_size && addresses[0]<addresses[2]+inline_size) ||
               (addresses[2]<addresses[1]+main_size && addresses[1]<addresses[2]+inline_size) ||
               little_read(buffers[0],8)!=inline_size || little_read(buffers[0]+8,8)!=0){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
            AcquireSRWLockShared(&w->lock);unsigned device=0;while(device<16 && w->nv_device_fds[device]!=fd)++device;
            int valid=!w->closed && !w->error && device<16 && w->nv_device_kinds[device]==1;
            ReleaseSRWLockShared(&w->lock);if(!valid){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
            /* GM20B source ABI; layout documented by libnx nvidia/ioctl.h.
             * No command submission, memory mapping, or fence is acknowledged. */
            static const uint32_t characteristics[40]={
                0x120,0xb,0xa1,1,0x40000,0,0,0,2,0x20,0x20000,0x20000,
                0x1b,0x30000,1,0x503,0x503,0x80,0x28,0,0x55,0,
                0x902d,0xb197,0xb1c0,0xb06f,0xa140,0xb0b5,1,0,2,1,0,1,
                0x21d70,0,0x30326d67,0x62,0,0};
            uint8_t reply[176]={0};little_write(reply,inline_size,8);
            if(tpc)little_write(reply+16,3,4); /* One GPC with both source TPCs. */
            else for(unsigned i=0;i<40;++i)little_write(reply+16+i*4,characteristics[i],4);
            memcpy(buffers[1],reply,(size_t)main_size);memcpy(buffers[2],reply+16,(size_t)inline_size);
            memset(request,0,44);little_write(request+4,9,4);little_write(request+16,UINT32_C(0x4f434653),4);
            little_write(request+20,version,4);little_write(request+28,token,4);c->x[0]=0;return 1;
        }
        if(c->session_types[slot]==BOOT_SESSION_FSP+4 && (type==4 || type==6) && little_read(request+4,4)==9){
            request=boot_access(c,c->tpidrro,52,READ|WRITE,pc);if(!request)return 0;
            uint32_t version=type==6?1:0,token=(uint32_t)little_read(request+28,4),fd=(uint32_t)little_read(request+32,4);
            BootWindow*w=(BootWindow*)(uintptr_t)c->window_native;
            if(c->session_domains[slot] || c->session_process_ids[slot]!=1 || !c->nvdrv_transfer_slots[slot] || !w || !fd ||
               little_read(request+16,4)!=UINT32_C(0x49434653) || little_read(request+20,4)!=version || little_read(request+24,4)!=2){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
            AcquireSRWLockExclusive(&w->lock);unsigned device=0;while(device<16 && w->nv_device_fds[device]!=fd)++device;
            int valid=!w->closed && !w->error && device<16;
            if(valid && w->nv_channels[device].pending)valid=0;
            if(valid && w->nv_device_kinds[device]==2 && (boot_gpu_mapping_object_used(&w->gpu_mappings,fd,0) || boot_graphic_object_used(&w->graphic_buffers,fd,0)))valid=0;
            if(valid)for(unsigned i=0;i<16;++i)if(w->nv_channels[i].as_fd==fd || w->nv_channels[i].nvmap_fd==fd){valid=0;break;}
            if(valid){if(w->nv_device_kinds[device]==2)boot_nvmap_close(&w->nvmap,fd);boot_gpu_mapping_close(&w->gpu_mappings,fd);w->nv_device_fds[device]=0;w->nv_device_kinds[device]=0;memset(&w->nv_address_spaces[device],0,sizeof(w->nv_address_spaces[device]));boot_gpu_channel_close(&w->nv_channels[device]);}
            ReleaseSRWLockExclusive(&w->lock);if(!valid){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
            memset(request,0,44);little_write(request+4,9,4);little_write(request+16,UINT32_C(0x4f434653),4);
            little_write(request+20,version,4);little_write(request+28,token,4);c->x[0]=0;return 1;
        }
        if(c->session_types[slot]==BOOT_SESSION_FSP+4 && (type==4 || type==6) && little_read(request+4,4)==10 && little_read(request+24,4)==4){
            request=boot_access(c,c->tpidrro,56,READ|WRITE,pc);if(!request)return 0;
            uint32_t version=type==6?1:0,token=(uint32_t)little_read(request+28,4);
            uint32_t fd=(uint32_t)little_read(request+32,4),id=(uint32_t)little_read(request+36,4);
            BootWindow*w=(BootWindow*)(uintptr_t)c->window_native;
            unsigned event_slot=0;while(event_slot<16 && c->event_handles[event_slot])++event_slot;
            if(c->session_domains[slot] || c->session_process_ids[slot]!=1 || !c->nvdrv_transfer_slots[slot] || !w || !fd ||
               id<1 || id>3 || event_slot==16 || c->next_session_handle>=0x7ffffffe ||
               little_read(request+16,4)!=UINT32_C(0x49434653) || little_read(request+20,4)!=version){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
            AcquireSRWLockExclusive(&w->lock);unsigned device=0;while(device<16 && w->nv_device_fds[device]!=fd)++device;
            int valid=!w->closed && !w->error && device<16 &&
                ((w->nv_device_kinds[device]==1 && id<=2) || (w->nv_device_kinds[device]==4 && id==3));
            HANDLE*owned=valid?(id==3?&w->nv_channels[device].error_event:&w->nv_control_events[id-1]):NULL;
            HANDLE event=NULL,owner=owned?*owned:NULL;int created=0;
            if(valid && !owner){owner=CreateEventW(NULL,TRUE,FALSE,NULL);created=1;}
            if(owner && DuplicateHandle(GetCurrentProcess(),owner,GetCurrentProcess(),&event,0,FALSE,DUPLICATE_SAME_ACCESS))*owned=owner;
            else if(created && owner)CloseHandle(owner);
            ReleaseSRWLockExclusive(&w->lock);
            if(!event){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
            if(c->next_session_handle<0x10000)c->next_session_handle=0x10000;
            uint32_t handle=++c->next_session_handle;
            c->event_handles[event_slot]=handle;c->event_sources[event_slot]=300+id;
            c->event_native_handles[event_slot]=(uint64_t)(uintptr_t)event;++c->event_create_count;
            /* Starts unsignaled: no GPU submission/completion is fabricated. */
            memset(request,0,52);little_write(request+4,UINT32_C(0x80000009),4);little_write(request+8,2,4);little_write(request+12,handle,4);
            little_write(request+16,UINT32_C(0x4f434653),4);little_write(request+20,version,4);little_write(request+28,token,4);c->x[0]=0;return 1;
        }
        if(c->session_types[slot]==BOOT_SESSION_FSP+4 && (type==4 || type==6) && little_read(request+4,4)==10){
            request=boot_access(c,c->tpidrro,56,READ|WRITE,pc);if(!request)return 0;
            uint32_t version=type==6?1:0,token=(uint32_t)little_read(request+28,4);
            if(c->session_domains[slot] || c->session_process_ids[slot]!=1 || !c->nvdrv_transfer_slots[slot] ||
               little_read(request+16,4)!=UINT32_C(0x49434653) || little_read(request+20,4)!=version ||
               little_read(request+24,4)!=13 || little_read(request+32,8)>1){
                boot_fault(c,BOOT_SVC,pc,c->tpidrro,56);return 0;
            }
            /* Windows profile: nv_graphics_firmware_memory_margin is disabled.
             * Effective margin stays zero for either boolean request. No
             * Tegra firmware allocation exists in this native host. */
            memset(request,0,40);little_write(request+4,8,4);little_write(request+16,UINT32_C(0x4f434653),4);
            little_write(request+20,version,4);little_write(request+28,token,4);c->x[0]=0;return 1;
        }
        if(c->session_types[slot]==BOOT_SESSION_FSP+4 && (type==4 || type==6) && little_read(request+4,4)==UINT32_C(0x8000000a)){
            request=boot_access(c,c->tpidrro,72,READ|WRITE,pc);if(!request)return 0;
            uint32_t version=type==6?1:0,token=(uint32_t)little_read(request+44,4);
            if(c->session_domains[slot] || c->session_process_ids[slot]!=1 || !c->nvdrv_transfer_slots[slot] ||
               !c->applet_resource_user_id || little_read(request+8,4)!=1 || little_read(request+32,4)!=UINT32_C(0x49434653) ||
               little_read(request+36,4)!=version || little_read(request+40,4)!=8 || little_read(request+48,8)!=c->applet_resource_user_id){
                boot_fault(c,BOOT_SVC,pc,c->tpidrro,72);return 0;
            }
            /* The initialized session already belongs to process 1. Validate
             * its application-resource identity; kernel PID is host supplied. */
            memset(request,0,40);little_write(request+4,8,4);little_write(request+16,UINT32_C(0x4f434653),4);
            little_write(request+20,version,4);little_write(request+28,token,4);c->x[0]=0;return 1;
        }
        if(c->session_types[slot]==BOOT_SESSION_FSP+4 && (type==4 || type==6)){
            request=boot_access(c,c->tpidrro,68,READ|WRITE,pc);if(!request)return 0;
            uint32_t version=type==6?1:0,token=(uint32_t)little_read(request+44,4);
            uint32_t transfer_handle=(uint32_t)little_read(request+16,4),size=(uint32_t)little_read(request+48,4);
            unsigned index=0;while(index<16 && (!c->transfers[index].handle || c->transfers[index].owner_closed || c->transfers[index].handle!=transfer_handle))++index;
            if(c->session_domains[slot] || c->session_process_ids[slot]!=1 || c->nvdrv_transfer_slots[slot] ||
               little_read(request+4,4)!=UINT32_C(0x80000009) || little_read(request+8,4)!=4 ||
               little_read(request+12,4)!=UINT32_C(0xffff8001) || little_read(request+32,4)!=UINT32_C(0x49434653) ||
               little_read(request+36,4)!=version || little_read(request+40,4)!=3 || index==16 ||
               c->transfers[index].size!=size || c->transfers[index].permissions!=0){
                boot_fault(c,BOOT_SVC,pc,c->tpidrro,68);return 0;
            }
            ++c->transfers[index].driver_references;c->nvdrv_transfer_slots[slot]=index+1;
            memset(request,0,40);little_write(request+4,8,4);little_write(request+16,UINT32_C(0x4f434653),4);
            little_write(request+20,version,4);little_write(request+28,token,4);c->x[0]=0;return 1;
        }
        if(c->session_types[slot]==BOOT_SESSION_FSP+3 && !c->session_domains[slot] && (type==4 || type==6)){
            request=boot_access(c,c->tpidrro,32,READ|WRITE,pc);if(!request)return 0;
            uint32_t version=type==6?1:0,token=(uint32_t)little_read(request+28,4);
            uint32_t words=(uint32_t)little_read(request+4,4),command=(uint32_t)little_read(request+24,4);
            if(little_read(request+16,4)!=UINT64_C(0x49434653) || little_read(request+20,4)!=version ||
               c->session_process_ids[slot]!=1 || !((command==0 && words==10) || (command==1 && words==9))){
                boot_fault(c,BOOT_SVC,pc,c->tpidrro,32);return 0;
            }
            request=boot_access(c,c->tpidrro,command==0?48:44,READ|WRITE,pc);if(!request)return 0;
            uint32_t mode=(uint32_t)little_read(request+32,4),configuration=0;
            BootPerformance*performance=(BootPerformance*)(uintptr_t)c->performance_native;
            if(command==0){
                configuration=(uint32_t)little_read(request+36,4);
                if(!boot_performance_set(&performance,mode,configuration)){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
                c->performance_native=(uint64_t)(uintptr_t)performance;
            }else {
                if(mode!=0 || !performance){boot_fault(c,BOOT_SVC,pc,0,0);return 0;}
                configuration=performance->configuration;
            }
            memset(request,0,command==0?40:44);little_write(request+4,command==0?8:9,4);
            little_write(request+16,UINT32_C(0x4f434653),4);little_write(request+20,version,4);little_write(request+28,token,4);
            if(command==1)little_write(request+32,configuration,4);
            c->x[0]=0;return 1;
        }
        if(c->session_types[slot]==BOOT_SESSION_APM && !c->session_domains[slot] && (type==4 || type==6)){
            request=boot_access(c,c->tpidrro,48,READ|WRITE,pc);if(!request)return 0;
            uint32_t version=type==6?1:0,token=(uint32_t)little_read(request+28,4);
            if(little_read(request+4,4)!=8 || little_read(request+16,4)!=UINT64_C(0x49434653) ||
               little_read(request+20,4)!=version || little_read(request+24,4)!=0 || c->session_process_ids[slot]!=1){
                boot_fault(c,BOOT_SVC,pc,c->tpidrro,48);return 0;
            }
            /* APM ISession follows the filesystem service tags (20..22). */
            uint32_t handle=boot_new_session(c,pc,BOOT_SESSION_FSP+3,c->session_process_ids[slot]);if(!handle)return 0;
            memset(request,0,48);little_write(request+4,UINT32_C(0x80000008),4);
            little_write(request+8,0x20,4);little_write(request+12,handle,4);
            little_write(request+16,UINT32_C(0x4f434653),4);little_write(request+20,version,4);
            little_write(request+28,token,4);c->x[0]=0;return 1;
        }
        if(type==2 && little_read(request+4,4)==0){
            boot_release_nvdrv(c,slot);
            if(c->session_types[slot]==BOOT_SESSION_FSP+6)boot_window_display_release((BootWindow*)(uintptr_t)c->window_native,slot);
            /* The server closes its endpoint without replying. The pending
             * synchronous caller receives ResultSessionClosed (module 1,123).
             * Its client handle remains valid until svcCloseHandle. */
            unsigned domain=c->session_domains[slot];
            if(domain && --c->domain_refs[domain-1]==0)memset(c->domain_types[domain-1],0,sizeof(c->domain_types[0]));
            c->session_domains[slot]=0;c->session_types[slot]=BOOT_SESSION_CLOSED;
            c->x[0]=UINT32_C(0xf601);return 1;
        }
        return (type==5 || type==7)?boot_cmif_control(c,pc,slot):boot_cmif_domain(c,pc,slot);
    }
    request=boot_access(c,c->tpidrro,20,READ|WRITE,pc);if(!request)return 0;
    if(little_read(request,4)==17 && little_read(request+4,4)==2 && c->session_process_ids[slot]){
        if(!boot_service_allowed(request+8)){
            unsigned terminated=0,nonempty=0;
            for(unsigned i=0;i<8;++i){unsigned ch=request[8+i];
                if(!ch){terminated=1;continue;}
                if(terminated || ch<33 || ch>126){boot_fault(c,BOOT_SVC,pc,c->tpidrro,20);return 0;}
                nonempty=1;
            }
            if(!nonempty){boot_fault(c,BOOT_SVC,pc,c->tpidrro,20);return 0;}
            /* Preserve GetService's fixed output layout even on failure.
             * Its moved-handle slot is null (no handle is allocated); the
             * original optimized SDK reads the result at byte 16. */
            memset(request,0,20);little_write(request+4,UINT32_C(0x80000001),4);
            little_write(request+8,0x20,4);little_write(request+16,UINT32_C(0x1015),4);
            c->x[0]=0;return 1;
        }
        static const struct {char name[9];unsigned kind;} endpoints[]={
            {"lm",BOOT_SESSION_LM},{"apm",BOOT_SESSION_APM},{"appletOE",BOOT_SESSION_APPLET},
            {"aoc:u",BOOT_SESSION_AOC},{"ectx:aw",BOOT_SESSION_ERROR_CONTEXT},
            {"pctl",BOOT_SESSION_PCTL},{"fsp-srv",BOOT_SESSION_FSP},
            {"nvdrv",BOOT_SESSION_FSP+4}, /* 24: tracked graphics-driver endpoint */
            {"time:u",BOOT_SESSION_TIME},
            {"vi:u",BOOT_SESSION_FSP+5}, /* 25: application display endpoint */
            {"acc:u0",BOOT_SESSION_FSP+8}, /* 28: local account endpoint; commands checked separately */
            {"hid",BOOT_SESSION_FSP+11}, /* 31: tracked input service endpoint */
            {"audren:u",BOOT_SESSION_FSP+13}, /* 33: audio renderer manager */
            {"hwopus",BOOT_SESSION_FSP+17}, /* 37: hardware Opus decoder manager */
            {"prepo:u",120}, /* play reports: accepted and discarded on this host (see BOOT_SESSION_PREPO) */
            {"nfp:user",122} /* amiibo (NFP) user manager: no NFC hardware on this host */
        };
        unsigned kind=UINT32_MAX;
        for(unsigned i=0;i<sizeof(endpoints)/sizeof(endpoints[0]);++i)
            if(!memcmp(request+8,endpoints[i].name,8)){kind=endpoints[i].kind;break;}
        if(kind==UINT32_MAX){boot_fault(c,BOOT_SVC,pc,c->tpidrro,20);return 0;}
        uint32_t handle=boot_new_session(c,pc,kind,c->session_process_ids[slot]);if(!handle)return 0;
        little_write(request,0,4);little_write(request+4,UINT64_C(0x80000001),4);
        little_write(request+8,0x20,4);little_write(request+12,handle,4);little_write(request+16,0,4);
        ++c->service_lookup_count;c->x[0]=0;return 1;
    }
    /* Observed TIPC RegisterClient: command 16, no data/descriptors, PID
     * special header. The kernel supplies identity, never the caller's PID.
     * This isolated host has one synthetic process, ID 1. */
    if(little_read(request,4)!=16 || little_read(request+4,4)!=UINT64_C(0x80000000) ||
       little_read(request+8,4)!=1 || c->session_process_ids[slot]){
        boot_fault(c,BOOT_SVC,pc,c->tpidrro,20);return 0;
    }
    little_write(request,0,4);little_write(request+4,1,4);little_write(request+8,0,4);
    c->session_process_ids[slot]=1;++c->sm_register_count;c->x[0]=0;return 1;
}
static inline void boot_invalidate_exclusive(BootContext*c,uint64_t a,unsigned w){
    if(c->exclusive_valid && a<=UINT64_MAX-w && a<c->exclusive_address+c->exclusive_width &&
       c->exclusive_address<a+w)c->exclusive_valid=0;
}
/* A guest pair may span adjacent mappings whose host buffers are separate.
 * The ordinary contiguous lookup stays fast; this rare fallback preflights
 * every byte before a store so a later permission fault has no partial write. */
static inline int boot_pair_split(BootContext*c,uint64_t a,unsigned width,unsigned access,uint64_t pc,uint8_t*pointers[16]){
    if(c->status!=CH_ADDRESS || c->fault_pc!=pc || c->fault_address!=a ||
       c->fault_width!=width || width>16 || a>UINT64_MAX-width)return 0;
    c->status=0;c->fault_pc=c->fault_address=c->fault_width=0;
    for(unsigned i=0;i<width;++i){
        pointers[i]=boot_access(c,a+i,1,access,pc);
        if(!pointers[i])return 0;
    }
    return 1;
}
/* Stores of up to 64 bytes. A store spanning guest-adjacent regions with separate host buffers
 * takes the out-of-line split path: every byte's destination is checked before any is written,
 * so a permission fault leaves memory unchanged (as boot_store_pair). Kept out of line so the
 * millions of translated store sites inline only the contiguous fast path. */
static inline __attribute__((noinline,cold)) int boot_store_split(BootContext*c,uint64_t a,unsigned w,uint64_t pc,const uint8_t*source) {
    if(c->status!=CH_ADDRESS || c->fault_pc!=pc || c->fault_address!=a || c->fault_width!=w || w>64 || a>UINT64_MAX-w)return 0;
    c->status=0;c->fault_pc=c->fault_address=c->fault_width=0;
    uint8_t*pointers[64];
    for(unsigned i=0;i<w;++i){pointers[i]=boot_access(c,a+i,1,WRITE,pc);if(!pointers[i])return 0;}
    boot_invalidate_exclusive(c,a,w);
    for(unsigned i=0;i<w;++i)*pointers[i]=source[i];
    return 1;
}
#if TOTK_FASTMEM
static inline __attribute__((noinline)) int boot_store_bytes_legacy(BootContext*c,uint64_t a,unsigned w,uint64_t pc,const uint8_t*source);
static inline __attribute__((always_inline)) int boot_store_bytes(BootContext*c,uint64_t a,unsigned w,uint64_t pc,const uint8_t*source) {
    /* Translator batch: w==32 (STP Q, ST1 two registers) goes direct too, as two 16-byte stores in address
     * order, under the same direct-view rule as w==16 (a page crossing is the fixed view's business). */
    if(__builtin_expect(a<=BOOT_FM_DIRECT_LIMIT-(w==32?16:0) && (w==1 || w==2 || w==4 || w==8 || w==16 || w==32) && BOOT_FM_DIRECT(c,pc),1)){
        uint8_t*m=BOOT_FM_VIEW+a;
        if(w==32){boot_fm_v128 v0,v1;memcpy(&v0,source,16);memcpy(&v1,source+16,16);*(volatile boot_fm_v128*)m=v0;*(volatile boot_fm_v128*)(m+16)=v1;}
        else if(w==16){boot_fm_v128 v;memcpy(&v,source,16);*(volatile boot_fm_v128*)m=v;}
        else {uint64_t v=0;memcpy(&v,source,w);boot_fm_write(m,w,v);}
        return 1;
    }
    return boot_store_bytes_legacy(c,a,w,pc,source);
}
static inline __attribute__((noinline)) int boot_store_bytes_legacy(BootContext*c,uint64_t a,unsigned w,uint64_t pc,const uint8_t*source) {
#else
static inline int boot_store_bytes(BootContext*c,uint64_t a,unsigned w,uint64_t pc,const uint8_t*source) {
#endif
    uint8_t*p=boot_access(c,a,w,WRITE,pc);
    if(__builtin_expect(p!=NULL,1)){boot_invalidate_exclusive(c,a,w);memcpy(p,source,w);return 1;}
    return boot_store_split(c,a,w,pc,source);
}
#if TOTK_FASTMEM
static inline __attribute__((noinline)) int boot_store_legacy(BootContext*c,uint64_t a,unsigned w,uint64_t pc,uint64_t value);
static inline __attribute__((noinline)) int boot_store_fm_slow(BootContext*c,uint64_t a,unsigned w,uint64_t pc,uint64_t value) {
    if(BOOT_FM_TABLE(c)){uint8_t*h=boot_fm_table_ptr(c,a,w,WRITE);if(__builtin_expect(h!=NULL,1)){boot_fm_write(h,w,value);return 1;}}
    return boot_store_legacy(c,a,w,pc,value);
}
static inline __attribute__((always_inline)) int boot_store(BootContext*c,uint64_t a,unsigned w,uint64_t pc,uint64_t value) {
    /* No per-store monitor clearing: store-exclusive is a compare-exchange against the loaded value. */
    if(__builtin_expect(a<=BOOT_FM_DIRECT_LIMIT && BOOT_FM_DIRECT(c,pc),1)){boot_fm_write(BOOT_FM_VIEW+a,w,value);return 1;}
    return boot_store_fm_slow(c,a,w,pc,value);
}
static inline __attribute__((noinline)) int boot_store_legacy(BootContext*c,uint64_t a,unsigned w,uint64_t pc,uint64_t value) {
#else
static inline int boot_store(BootContext*c,uint64_t a,unsigned w,uint64_t pc,uint64_t value) {
#endif
    uint8_t*p=boot_access(c,a,w,WRITE,pc);
    if(__builtin_expect(p!=NULL,1)){boot_invalidate_exclusive(c,a,w);little_write(p,value,w);return 1;}
    uint8_t bytes[8];little_write(bytes,value,w);return boot_store_split(c,a,w,pc,bytes);
}
/* The split fallbacks are out of line (as boot_store_split) so the contiguous pair paths inline. */
static inline __attribute__((noinline,cold)) int boot_load_pair_split(BootContext*c,uint64_t a,unsigned w,uint64_t pc,uint64_t*v0,uint64_t*v1) {
    uint8_t*pointers[16],bytes[16];
    if(!boot_pair_split(c,a,w*2,READ,pc,pointers))return 0;
    for(unsigned i=0;i<w*2;++i)bytes[i]=*pointers[i];
    *v0=little_read(bytes,w);*v1=little_read(bytes+w,w);return 1;
}
#if TOTK_FASTMEM
static inline __attribute__((noinline)) int boot_load_pair_legacy(BootContext*c,uint64_t a,unsigned w,uint64_t pc,uint64_t*v0,uint64_t*v1);
static inline __attribute__((always_inline)) int boot_load_pair(BootContext*c,uint64_t a,unsigned w,uint64_t pc,uint64_t*v0,uint64_t*v1) {
    if(__builtin_expect(a<=BOOT_FM_DIRECT_LIMIT && (w==4 || w==8) && BOOT_FM_DIRECT(c,pc),1)){
        const uint8_t*m=BOOT_FM_VIEW+a;
        if(w==8){boot_fm_v128 v=*(volatile const boot_fm_v128*)m;*v0=(uint64_t)v[0];*v1=(uint64_t)v[1];}
        else {uint64_t v=*(volatile const boot_fm_u64*)m;*v0=(uint32_t)v;*v1=v>>32;}
        return 1;
    }
    return boot_load_pair_legacy(c,a,w,pc,v0,v1);
}
static inline __attribute__((noinline)) int boot_load_pair_legacy(BootContext*c,uint64_t a,unsigned w,uint64_t pc,uint64_t*v0,uint64_t*v1) {
#else
static inline int boot_load_pair(BootContext*c,uint64_t a,unsigned w,uint64_t pc,uint64_t*v0,uint64_t*v1) {
#endif
    uint8_t*p=boot_access(c,a,w*2,READ,pc);
    if(__builtin_expect(p!=NULL,1)){*v0=little_read(p,w);*v1=little_read(p+w,w);return 1;}
    return boot_load_pair_split(c,a,w,pc,v0,v1);
}
static inline __attribute__((noinline,cold)) int boot_store_pair_split(BootContext*c,uint64_t a,unsigned w,uint64_t pc,uint64_t v0,uint64_t v1) {
    uint8_t*pointers[16],bytes[16];
    if(!boot_pair_split(c,a,w*2,WRITE,pc,pointers))return 0;
    little_write(bytes,v0,w);little_write(bytes+w,v1,w);
    boot_invalidate_exclusive(c,a,w*2);
    for(unsigned i=0;i<w*2;++i)*pointers[i]=bytes[i];
    return 1;
}
#if TOTK_FASTMEM
static inline __attribute__((noinline)) int boot_store_pair_legacy(BootContext*c,uint64_t a,unsigned w,uint64_t pc,uint64_t v0,uint64_t v1);
static inline __attribute__((always_inline)) int boot_store_pair(BootContext*c,uint64_t a,unsigned w,uint64_t pc,uint64_t v0,uint64_t v1) {
    if(__builtin_expect(a<=BOOT_FM_DIRECT_LIMIT && (w==4 || w==8) && BOOT_FM_DIRECT(c,pc),1)){
        uint8_t*m=BOOT_FM_VIEW+a;
        if(w==8){boot_fm_v128 v={(long long)v0,(long long)v1};*(volatile boot_fm_v128*)m=v;}
        else *(volatile boot_fm_u64*)m=(v0&0xffffffffu)|(v1<<32);
        return 1;
    }
    return boot_store_pair_legacy(c,a,w,pc,v0,v1);
}
static inline __attribute__((noinline)) int boot_store_pair_legacy(BootContext*c,uint64_t a,unsigned w,uint64_t pc,uint64_t v0,uint64_t v1) {
#else
static inline int boot_store_pair(BootContext*c,uint64_t a,unsigned w,uint64_t pc,uint64_t v0,uint64_t v1) {
#endif
    uint8_t*p=boot_access(c,a,w*2,WRITE,pc);
    if(__builtin_expect(p!=NULL,1)){boot_invalidate_exclusive(c,a,w*2);little_write(p,v0,w);little_write(p+w,v1,w);return 1;}
    return boot_store_pair_split(c,a,w,pc,v0,v1);
}
/* One guest thread only. Multiple host/guest threads must not share this
 * context; a future scheduler must supply cross-thread monitor invalidation. */
static inline int boot_load_exclusive(BootContext*c,uint64_t a,unsigned w,uint64_t pc,uint64_t*out){
    if(a&(w-1)){boot_fault(c,CH_ARGUMENT,pc,a,w);return 0;}
    if(!boot_load(c,a,w,pc,out))return 0;
    atomic_thread_fence(memory_order_acquire);
    c->exclusive_address=a;c->exclusive_value=*out;c->exclusive_width=w;c->exclusive_valid=1;return 1;
}
static inline int boot_load_exclusive_pair(BootContext*c,uint64_t a,unsigned w,uint64_t pc,int acquire,uint64_t*low,uint64_t*high){
    if((w!=4 && w!=8) || (a&((w*2)-1))){boot_fault(c,CH_ARGUMENT,pc,a,w*2);return 0;}
    if(!boot_load_pair(c,a,w,pc,low,high))return 0;
    if(acquire)atomic_thread_fence(memory_order_acquire);
    c->exclusive_address=a;c->exclusive_value=*low;c->exclusive_value_hi=*high;
    c->exclusive_width=w*2;c->exclusive_valid=1;return 1;
}
static inline int boot_store_exclusive(BootContext*c,uint64_t a,unsigned w,uint64_t pc,uint64_t value,uint64_t*out){
    if(a&(w-1)){boot_fault(c,CH_ARGUMENT,pc,a,w);return 0;}
    uint8_t*p=boot_access(c,a,w,WRITE,pc);if(!p)return 0;
#if TOTK_FASTMEM
    /* One host compare-exchange against the value LDXR observed (atomic for other host threads). */
    {int success=c->exclusive_valid && c->exclusive_address==a && c->exclusive_width==w && boot_fm_cas(p,w,c->exclusive_value,value);
     c->exclusive_valid=0;*out=success?0:1;return 1;}
#endif
    int success=c->exclusive_valid && c->exclusive_address==a && c->exclusive_width==w && little_read(p,w)==c->exclusive_value;
    c->exclusive_valid=0;
    if(success){atomic_thread_fence(memory_order_release);little_write(p,value,w);}
    *out=success?0:1;return 1;
}
static inline int boot_store_exclusive_pair(BootContext*c,uint64_t a,unsigned w,uint64_t pc,int release,uint64_t low,uint64_t high,uint64_t*out){
    if((w!=4 && w!=8) || (a&((w*2)-1))){boot_fault(c,CH_ARGUMENT,pc,a,w*2);return 0;}
#if TOTK_FASTMEM
    {uint8_t*p=boot_access(c,a,w*2,WRITE,pc);
     if(p && !((uintptr_t)p&(w*2-1))){
        int armed=c->exclusive_valid && c->exclusive_address==a && c->exclusive_width==w*2,success=0;
        if(armed && w==8)success=boot_fm_cas16(p,c->exclusive_value,c->exclusive_value_hi,low,high);
        else if(armed)success=boot_fm_cas(p,8,(c->exclusive_value&0xffffffffu)|(c->exclusive_value_hi<<32),(low&0xffffffffu)|(high<<32));
        (void)release;c->exclusive_valid=0;*out=success?0:1;return 1;
     }
     if(!p && c->status!=CH_ADDRESS)return 0;}
#endif
    if(!boot_access(c,a,w*2,WRITE,pc)){
        uint8_t*pointers[16];if(!boot_pair_split(c,a,w*2,WRITE,pc,pointers))return 0;
    }
    uint64_t observed_low,observed_high;
    if(!boot_load_pair(c,a,w,pc,&observed_low,&observed_high))return 0;
    int success=c->exclusive_valid && c->exclusive_address==a && c->exclusive_width==w*2 &&
                observed_low==c->exclusive_value && observed_high==c->exclusive_value_hi;
    c->exclusive_valid=0;
    if(success){
        if(release)atomic_thread_fence(memory_order_release);
        if(!boot_store_pair(c,a,w,pc,low,high))return 0;
    }
    *out=success?0:1;return 1;
}
/* Bounded guest-memory service. This is a synthetic process map, not a host
 * VirtualQuery adapter or a complete Horizon kernel implementation.
 * MemoryInfo ABI: libnx nx/include/switch/kernel/svc.h. The SVC returns
 * PageInfo in w1; the original ARM wrapper writes it through its saved pointer.
 * Out-of-profile queries trap instead of inventing kernel error semantics. */
static inline int boot_set_memory_attribute(BootContext*c,uint64_t pc){
    uint64_t base=c->x[0],size=c->x[1];uint32_t mask=(uint32_t)c->x[2],value=(uint32_t)c->x[3];
    if(!size || ((base|size)&4095) || base>UINT64_MAX-size || mask!=8 || (value&~mask))goto invalid;
    /* A mapped GPU buffer may be marked uncached again. Keep rejecting a
     * transition back to cached memory while the device still refers to it. */
    if(!value && c->window_native && (boot_gpu_mapping_cpu_overlap(&((BootWindow*)(uintptr_t)c->window_native)->gpu_mappings,base,base+size) || boot_graphic_cpu_overlap(&((BootWindow*)(uintptr_t)c->window_native)->graphic_buffers,base,base+size)))goto invalid;
    for(unsigned i=0;i<c->alias_count;++i){BootAlias*a=&c->aliases[i];
        if(base<a->source+a->size && a->source<base+size)goto invalid;}
    for(unsigned i=0;i<16;++i){BootTransfer*t=&c->transfers[i];
        if(t->handle && base<t->base+t->size && t->base<base+size)goto invalid;}
    /* Check every byte is backed by committed owned normal memory, including
     * adjacent physical mappings. CPU coherence does not imply GPU coherence. */
    uint64_t cursor=base,end=base+size;
    while(cursor<end){
        unsigned i=0;for(;i<c->region_count;++i){Region*r=&c->regions[i];
            if((r->reserved!=1 && r->reserved!=3) || c->memory_types[i]!=5 || r->permissions!=(READ|WRITE) ||
               cursor<r->base || cursor-r->base>=r->size)continue;
            uint64_t available=r->size-(cursor-r->base);cursor+=available<end-cursor?available:end-cursor;break;
        }
        if(i==c->region_count)goto invalid;
    }
    atomic_thread_fence(memory_order_seq_cst);
    if(!boot_attributes_update(&c->memory_attributes,base,end,value!=0))goto invalid;
    boot_tlb_flush();c->x[0]=0;return 1;
invalid:boot_fault(c,BOOT_SVC,pc,base,size);return 0;
}
static inline int boot_query_memory(BootContext*c,uint64_t pc) {
    uint64_t query=c->x[2],start=0,end=c->address_space_end;
    uint32_t type=0,permissions=0,attributes=0,device_refs=0;
    if(!end || (query>=end && !c->terminal_query_enabled)){
        boot_fault(c,BOOT_QUERY_SCOPE,pc,query,0);return 0;
    }
    for(unsigned i=0;i<c->region_count;++i){
        Region*r=&c->regions[i];
        if(r->base>=end || r->size>end-r->base){
            boot_fault(c,BOOT_QUERY_SCOPE,pc,r->base,r->size);return 0;
        }
    }
    if(query>=end){start=end;end=0;type=0x10;}
    else for(unsigned i=0;i<c->region_count;++i){
        Region*r=&c->regions[i];uint64_t last=r->base+r->size;
        if(query>=r->base && query<last){
            start=r->base;end=last;type=c->memory_types[i];permissions=r->permissions;
            break;
        }
        if(last<=query && last>start)start=last;
        if(r->base>query && r->base<end)end=r->base;
    }
    if(query<c->address_space_end)for(unsigned i=0;i<c->alias_count;++i){BootAlias*m=&c->aliases[i];
        uint64_t last=m->source+m->size;
        if(query>=m->source && query<last){start=m->source;end=last;permissions=0;attributes=1;break;}
        if(last<=query && last>start)start=last;
        if(m->source>query && m->source<end)end=m->source;
    }
    if(query<c->address_space_end)for(unsigned i=0;i<16;++i){BootTransfer*t=&c->transfers[i];if(!t->handle)continue;
        uint64_t last=t->base+t->size;
        if(query>=t->base && query<last){start=t->base;end=last;permissions=t->permissions;attributes=1;break;}
        if(last<=query && last>start)start=last;
        if(t->base>query && t->base<end)end=t->base;
    }
    if(query<c->address_space_end)for(unsigned i=0;i<c->memory_attributes.count;++i){
        BootUncachedRange*r=&c->memory_attributes.ranges[i];
        if(query>=r->begin && query<r->end){if(r->begin>start)start=r->begin;if(r->end<end)end=r->end;attributes|=8;break;}
        if(r->end<=query && r->end>start)start=r->end;
        if(r->begin>query && r->begin<end)end=r->begin;
    }
    if(c->window_native && query<c->address_space_end){BootWindow*w=(BootWindow*)(uintptr_t)c->window_native;
        for(unsigned i=0;i<BOOT_GPU_MAPPING_CAPACITY;++i){BootGpuMapping*r=&w->gpu_mappings.entries[i];if(!r->size)continue;
            uint64_t backed=boot_gpu_mapping_backing_size(r);if(!backed)continue;
            uint64_t last=r->cpu+backed;
            if(query>=r->cpu && query<last){if(r->cpu>start)start=r->cpu;if(last<end)end=last;attributes|=4;++device_refs;}
            if(last<=query && last>start)start=last;
            if(r->cpu>query && r->cpu<end)end=r->cpu;
        }
    }
    uint8_t*out=boot_access(c,c->x[0],40,WRITE,pc);
    if(!out)return 0;
    uint8_t info[40]={0};
    little_write(info,start,8);little_write(info+8,end-start,8);
    little_write(info+16,type,4);little_write(info+20,attributes,4);little_write(info+24,permissions,4);
    little_write(info+32,device_refs,4);
    boot_invalidate_exclusive(c,c->x[0],40);memcpy(out,info,sizeof(info));c->x[0]=0;c->x[1]=0;++c->query_count;
    return 1;
}
static inline uint64_t boot_umulh(uint64_t a,uint64_t b){return (uint64_t)(((unsigned __int128)a*b)>>64);}   /* one MUL */
/* RBIT / CLZ of the low `width` bits (1..64): bit-parallel, no per-bit loop (RBIT+CLZ is how Arm code
 * counts trailing zeros, so these sit in hot loops). */
static inline uint64_t boot_rbit(uint64_t a,unsigned width){
    a=((a>>1)&UINT64_C(0x5555555555555555))|((a&UINT64_C(0x5555555555555555))<<1);
    a=((a>>2)&UINT64_C(0x3333333333333333))|((a&UINT64_C(0x3333333333333333))<<2);
    a=((a>>4)&UINT64_C(0x0f0f0f0f0f0f0f0f))|((a&UINT64_C(0x0f0f0f0f0f0f0f0f))<<4);
    a=__builtin_bswap64(a);
    return width>=64?a:a>>(64-width);
}
static inline uint64_t boot_clz(uint64_t a,unsigned width){
    if(width<64)a&=(UINT64_C(1)<<width)-1;
    return a?(uint64_t)(__builtin_clzll(a)-(64-(int)width)):width;
}
static inline uint64_t boot_sign_extend(uint64_t a,unsigned width){
    uint64_t sign=UINT64_C(1)<<(width-1),mask=width==64?UINT64_MAX:(UINT64_C(1)<<width)-1;
    return ((a&mask)^sign)-sign;
}
static inline __attribute__((always_inline)) uint64_t boot_add_flags(BootContext*c,uint64_t a,uint64_t b,unsigned width) {
    uint64_t mask=width==64?UINT64_MAX:UINT64_C(0xffffffff),sign=UINT64_C(1)<<(width-1);
    a&=mask;b&=mask;uint64_t r=(a+b)&mask;
    c->n=(r&sign)!=0;c->z=r==0;c->carry=r<a;c->v=(~(a^b)&(a^r)&sign)!=0;return r;
}
static inline __attribute__((always_inline)) uint64_t boot_sub_flags(BootContext*c,uint64_t a,uint64_t b,unsigned width) {
    uint64_t mask=width==64?UINT64_MAX:UINT64_C(0xffffffff),sign=UINT64_C(1)<<(width-1);
    a&=mask;b&=mask;uint64_t r=(a-b)&mask;
    c->n=(r&sign)!=0;c->z=r==0;c->carry=a>=b;c->v=((a^b)&(a^r)&sign)!=0;return r;
}
/* SleepThread (svc 0x0b) and WaitForAddress/SignalToAddress (0x34/0x35), formerly emitted inline
 * by sleep_emit.py / address_arbiter_emit.py into every translated SVC site. Return -1: stop
 * (c->status set), 0: continue at *target (set by the body). */
static inline int boot_svc_sleep(BootContext*c,uint64_t pc,uint64_t next,uint64_t*target){

{
int64_t sleep_ns=(int64_t)c->x[0];
if(sleep_ns < -1 || sleep_ns>INT64_C(1000000000) || c->thread_count>BOOT_THREAD_CAPACITY || (c->thread_count && c->current_thread>=c->thread_count)){
    boot_fault(c,BOOT_SVC,pc,0,0);return -1;
}
if(sleep_ns<=0){
    *target=next;
    /* Distinct per-core queues; -1 additionally permits yield migration. */
    for(unsigned j=0;j<c->thread_count;++j){
        BootThread*t=&c->threads[j];
        if(t->state==5){int ready=boot_poll_wait(c,t,pc);if(ready==-2)return -1;if(ready>=0)boot_finish_wait(t,0,ready);}
    }
    unsigned previous=c->current_thread;
    if(!boot_reschedule(c,pc,next,sleep_ns<0?BOOT_SCHEDULE_MIGRATE:BOOT_SCHEDULE_YIELD,target))return -1;
    if(previous==c->current_thread)SwitchToThread();
}else if(!c->thread_count){
    HANDLE sleep_timer=CreateWaitableTimerW(NULL,TRUE,NULL);
    if(!sleep_timer){boot_fault(c,BOOT_SVC,pc,0,0);return -1;}
    LARGE_INTEGER sleep_due;sleep_due.QuadPart=-((sleep_ns+99)/100);
    if(!SetWaitableTimer(sleep_timer,&sleep_due,0,NULL,NULL,FALSE)){
        CloseHandle(sleep_timer);boot_fault(c,BOOT_SVC,pc,0,0);return -1;
    }
    DWORD sleep_result=WaitForSingleObject(sleep_timer,INFINITE);
    CloseHandle(sleep_timer);
    if(sleep_result!=WAIT_OBJECT_0){boot_fault(c,BOOT_SVC,pc,0,0);return -1;}
    *target=next;
}else{
    uint64_t now=0;
    if(!boot_condition_clock(c,&now)){boot_fault(c,BOOT_SVC,pc,0,0);return -1;}
    uint64_t ticks=((uint64_t)sleep_ns*UINT64_C(19200000)+UINT64_C(999999999))/UINT64_C(1000000000);
    if(!ticks || now>UINT64_MAX-ticks){boot_fault(c,BOOT_SVC,pc,0,0);return -1;}
    for(unsigned j=0;j<c->thread_count;++j)if(j!=c->current_thread){
        BootThread*t=&c->threads[j];
        if(t->state==5){int ready=boot_poll_wait(c,t,pc);if(ready==-2)return -1;if(ready>=0)boot_finish_wait(t,0,ready);}
    }
    BootThread*self=&c->threads[c->current_thread];
    self->state=8;self->wait_native[2]=now+ticks;
    unsigned chosen=BOOT_THREAD_CAPACITY;
    if(!boot_scheduler_choose(c,BOOT_SCHEDULE_CHECK,CORE_SCHEDULE_NONE,&chosen)){
        self->state=2;self->wait_native[2]=0;boot_fault(c,BOOT_SVC,pc,0,0);return -1;
    }
    *target=next;
    if(chosen!=c->current_thread){boot_save_thread(c,next);boot_load_thread(c,chosen,target);}
}
}
return 0;
}
static inline int boot_svc_arbiter(BootContext*c,uint64_t pc,uint64_t next,unsigned svc,uint64_t*target){

{
uint64_t arb_address=c->x[0];uint32_t arb_type=(uint32_t)c->x[1],arb_value=(uint32_t)c->x[2];
if(!c->thread_count || c->thread_count>BOOT_THREAD_CAPACITY || c->current_thread>=c->thread_count || (arb_address&3) || arb_type>2){boot_fault(c,BOOT_SVC,pc,0,0);return -1;}
*target=next;
if(svc==0x34){
    uint8_t*arb_word=boot_access(c,arb_address,4,arb_type==1?READ|WRITE:READ,pc);if(!arb_word)return -1;
    uint32_t arb_old;int arb_matches;
    for(;;){   /* type 1 (DecrementAndWaitIfLessThan): test and decrement as one compare-exchange */
        arb_old=boot_kernel_load32(arb_word);
        arb_matches=arb_type==2?arb_old==arb_value:(int32_t)arb_old<(int32_t)arb_value;
        if(!arb_matches || arb_type!=1 || boot_kernel_cas32(arb_word,arb_old,arb_old-1))break;
    }
    if(!arb_matches)c->x[0]=UINT32_C(0xfa01);
    else if(c->x[3]==0)c->x[0]=UINT32_C(0xea01);
    else {
        int64_t arb_timeout=(int64_t)c->x[3];uint64_t arb_deadline=0;
        /* a rejected call leaves the word as it was: take the type-1 decrement back */
#define BOOT_ARB_UNDO() do{if(arb_type==1)__atomic_fetch_add((uint32_t*)(void*)arb_word,1,__ATOMIC_SEQ_CST);}while(0)
        if(arb_timeout< -1){BOOT_ARB_UNDO();boot_fault(c,BOOT_SVC,pc,0,0);return -1;}
        if(arb_timeout>0){
            uint64_t arb_now=0;
            __uint128_t arb_delta=((__uint128_t)(uint64_t)arb_timeout*UINT64_C(19200000)+UINT64_C(999999999))/UINT64_C(1000000000);
            if(!boot_condition_clock(c,&arb_now) || !arb_delta || arb_delta>UINT64_MAX-arb_now){BOOT_ARB_UNDO();boot_fault(c,BOOT_SVC,pc,0,0);return -1;}
            arb_deadline=arb_now+(uint64_t)arb_delta;
        }
        if(!boot_address_wait_block(c,pc,next,arb_word,arb_type==1?0:arb_type,arb_old,arb_address,arb_deadline,target)){
            BOOT_ARB_UNDO();return -1;
        }
#undef BOOT_ARB_UNDO
    }
}else{
    if(!boot_condition_expire(c)){boot_fault(c,BOOT_SVC,pc,0,0);return -1;}
    int32_t arb_count=(int32_t)c->x[3];unsigned arb_waiters=0;
    for(unsigned j=0;j<c->thread_count;++j)if(c->threads[j].state==6 && c->threads[j].wait_address==arb_address)++arb_waiters;
    int arb_signal=1;
    if(arb_type){
        uint8_t*arb_word=boot_access(c,arb_address,4,READ|WRITE,pc);if(!arb_word)return -1;
        uint32_t arb_new=arb_value;   /* the word changes only if it still holds arb_value (one compare-exchange) */
        if(arb_type==1 || !arb_waiters)++arb_new;
        else if(arb_count<=0 || arb_waiters<=(uint32_t)arb_count)--arb_new;
        if(!boot_kernel_cas32(arb_word,arb_value,arb_new)){c->x[0]=UINT32_C(0xfa01);arb_signal=0;}
    }
    if(arb_signal){
        unsigned arb_limit=arb_count<=0?arb_waiters:(unsigned)arb_count;
        for(unsigned k=0;k<arb_limit && k<arb_waiters;++k){
            unsigned arb_chosen=BOOT_THREAD_CAPACITY;
            for(unsigned j=0;j<c->thread_count;++j)if(c->threads[j].state==6 && c->threads[j].wait_address==arb_address && (arb_chosen==BOOT_THREAD_CAPACITY || c->threads[j].priority<c->threads[arb_chosen].priority))arb_chosen=j;
            if(arb_chosen==BOOT_THREAD_CAPACITY)break;
            BootThread*t=&c->threads[arb_chosen];t->state=2;t->wait_address=0;t->wait_native[2]=0;t->x[0]=0;
        }
        c->x[0]=0;
        if(!boot_reschedule(c,pc,next,BOOT_SCHEDULE_CHECK,target))return -1;
    }
}
}
return 0;
}
/* The single kernel entry of translated code (one call per SVC site). Returns -1: stop with
 * c->status; 1: continue at the next instruction; 2: continue at *target (indirect); 0: not
 * handled here (the caller raises the unsupported-SVC stop). Bodies equal the former inline
 * emission of translate_startup.py for each SVC. */
#define BOOT_SVC_HOSTCALL 0x100u   /* pseudo-SVC: kernel-mode host function (host_fn.h) */
static inline int boot_host_fn_kernel(BootContext*c,uint64_t pc,uint64_t next,uint64_t*target);
static inline int boot_svc_kernel(BootContext*c,uint64_t pc,uint64_t next,unsigned svc,uint64_t*target){
    (void)pc;(void)next;(void)target;
    switch(svc){
case BOOT_SVC_HOSTCALL:return boot_host_fn_kernel(c,pc,next,target);
case 0x1:
if(c->layout_enabled){
    c->svc_id=1;
    if(!boot_set_heap_size(c,pc))return -1;
    c->svc_id=0;
    return 1;
}
    return 0;
case 0x3:
if(c->layout_enabled){
    c->svc_id=3;
    if(!boot_set_memory_attribute(c,pc))return -1;
    c->svc_id=0;
    return 1;
}
    return 0;
case 0x4:
if(c->layout_enabled){
    c->svc_id=4;
    if(!boot_alias_memory(c,pc,4))return -1;
    c->svc_id=0;
    return 1;
}
    return 0;
case 0x5:
if(c->layout_enabled){
    c->svc_id=5;
    if(!boot_alias_memory(c,pc,5))return -1;
    c->svc_id=0;
    return 1;
}
    return 0;
case 0x6:
if(c->query_enabled){
    c->svc_id=6;
    if(!boot_query_memory(c,pc))return -1;
    c->svc_id=0;
    return 1;
}
    return 0;
case 0x8:
if(c->thread_enabled && c->layout_enabled){
    c->svc_id=8;
    uint64_t saved_core=c->x[5];uint32_t core=(uint32_t)saved_core;c->x[5]=core==UINT32_C(0xfffffffe)?0:core;
    int created=boot_create_thread(c,pc);c->x[5]=saved_core;if(!created)return -1;
    c->svc_id=0;
    return 1;
}
    return 0;
case 0x9:
if(c->thread_enabled){
    c->svc_id=9;
    if(!boot_schedule_thread(c,pc,next,9,target))return -1;
    c->svc_id=0;return 2;
}
    return 0;
case 0xa:
if(c->thread_enabled){
    c->svc_id=10;
    if(!boot_schedule_thread(c,pc,next,10,target))return -1;
    c->svc_id=0;return 2;
}
    return 0;
case 0xb:
if(c->thread_enabled){
c->svc_id=0xb;
if(boot_svc_sleep(c,pc,next,target)<0)return -1;
c->svc_id=0;return 2;
}
    return 0;
case 0xc:
if(c->thread_enabled){
    c->svc_id=12;
    if(!boot_thread_info(c,pc,12))return -1;
    c->svc_id=0;
    return 1;
}
    return 0;
case 0xd:
if(c->thread_enabled && c->layout_enabled){
    c->svc_id=0xd;
    if(!boot_set_thread_priority(c,pc,next,target))return -1;
    c->svc_id=0;return 2;
}
    return 0;
case 0xe:
if(c->thread_enabled){
    c->svc_id=14;
    if(!boot_thread_affinity(c,pc,14))return -1;
    if(!boot_reschedule(c,pc,next,BOOT_SCHEDULE_CHECK,target))return -1;
    c->svc_id=0;return 2;
}
    return 0;
case 0xf:
if(c->thread_enabled){
    c->svc_id=15;
    if(!boot_thread_affinity(c,pc,15))return -1;
    if(!boot_reschedule(c,pc,next,BOOT_SCHEDULE_CHECK,target))return -1;
    c->svc_id=0;return 2;
}
    return 0;
case 0x10:
if(c->thread_enabled && c->layout_enabled){
    uint64_t mask=c->thread_count?c->threads[c->current_thread].core_mask:c->layout_info[0];
    uint32_t core=c->thread_count?(c->threads[c->current_thread].schedule.affinity?c->threads[c->current_thread].schedule.active:c->threads[c->current_thread].core):0;
    if(mask && !(mask&~c->layout_info[0])){
        if(core>=64 || !(mask&(UINT64_C(1)<<core))){core=0;while(!(mask&(UINT64_C(1)<<core)))++core;}
        c->x[0]=core;c->svc_id=0;
        return 1;
    }
}
    return 0;
case 0x13:
if(c->ipc_enabled){
    c->svc_id=19;
    if(!boot_session_service(c,pc,19))return -1;
    c->svc_id=0;
    return 1;
}
    return 0;
case 0x14:
if(c->ipc_enabled){
    c->svc_id=20;
    if(!boot_session_service(c,pc,20))return -1;
    c->svc_id=0;
    return 1;
}
    return 0;
case 0x15:
if(c->layout_enabled && c->ipc_enabled){
    c->svc_id=0x15;
    if(!boot_create_transfer(c,pc))return -1;
    c->svc_id=0;
    return 1;
}
    return 0;
case 0x16:
if(c->ipc_enabled){
    c->svc_id=22;
    if(!boot_session_service(c,pc,22))return -1;
    c->svc_id=0;
    return 1;
}
    return 0;
case 0x17:
if(c->ipc_enabled){
    c->svc_id=0x17;
    if(!boot_reset_signal(c,pc))return -1;
    c->svc_id=0;
    return 1;
}
    return 0;
case 0x18:
if(c->thread_enabled){
    c->svc_id=24;
    if(!boot_wait_synchronization(c,pc,next,target))return -1;
    c->svc_id=0;return 2;
}
    return 0;
case 0x19:
if(c->thread_enabled){
    c->svc_id=25;
    if(!boot_cancel_synchronization(c,pc,next,target))return -1;
    c->svc_id=0;return 2;
}
    return 0;
case 0x1a:
if(c->thread_enabled){
    c->svc_id=26;
    if(!boot_mutex(c,pc,next,26,target))return -1;
    c->svc_id=0;return 2;
}
    return 0;
case 0x1b:
if(c->thread_enabled){
    c->svc_id=27;
    if(!boot_mutex(c,pc,next,27,target))return -1;
    c->svc_id=0;return 2;
}
    return 0;
case 0x1c:
if(c->thread_enabled){
    c->svc_id=28;
    if(!boot_condition_variable(c,pc,next,28,target))return -1;
    c->svc_id=0;return 2;
}
    return 0;
case 0x1d:
if(c->thread_enabled){
    c->svc_id=29;
    if(!boot_condition_variable(c,pc,next,29,target))return -1;
    c->svc_id=0;return 2;
}
    return 0;
case 0x1f:
if(c->ipc_enabled){
    c->svc_id=31;
    if(!boot_session_service(c,pc,31))return -1;
    c->svc_id=0;
    return 1;
}
    return 0;
case 0x21:
if(c->ipc_enabled){
    c->svc_id=0x21;
    if(!boot_send_sync(c,pc))return -1;
    c->svc_id=0;
    return 1;
}
    return 0;
case 0x25:
if(c->thread_enabled){
    c->svc_id=37;
    if(!boot_thread_info(c,pc,37))return -1;
    c->svc_id=0;
    return 1;
}
    return 0;
case 0x29:
if(c->info_enabled){
    c->svc_id=0x29;
    if(!boot_get_info(c,pc))return -1;
    c->svc_id=0;
    return 1;
}
    return 0;
case 0x2c:
if(c->layout_enabled){
    c->svc_id=44;
    if(!boot_physical_memory(c,pc,44))return -1;
    c->svc_id=0;
    return 1;
}
    return 0;
case 0x2d:
if(c->layout_enabled){
    c->svc_id=45;
    if(!boot_physical_memory(c,pc,45))return -1;
    c->svc_id=0;
    return 1;
}
    return 0;
case 0x34:
if(c->thread_enabled){
c->svc_id=52;
if(boot_svc_arbiter(c,pc,next,52,target)<0)return -1;
c->svc_id=0;return 2;
}
    return 0;
case 0x35:
if(c->thread_enabled){
c->svc_id=53;
if(boot_svc_arbiter(c,pc,next,53,target)<0)return -1;
c->svc_id=0;return 2;
}
    return 0;
    default:return 0;
    }
}
#include "boot_mt_late.h"
/* Kernel entry of translated code. A core context (TOTK_CORES>1) enters the kernel under the kernel
 * lock; single-core execution runs the handler directly and, once a thread table exists with
 * TOTK_CORES>1 requested, hijacks the limit so boot_mt_execute can start the workers. */
static inline int boot_svc(BootContext*c,uint64_t pc,uint64_t next,unsigned svc,uint64_t*target){
    if(c->tlb_epoch==BOOT_TLB_CORE)return boot_mt_svc(c,pc,next,svc,target);
    int r=boot_svc_kernel(c,pc,next,svc,target);
#if TOTK_FASTMEM && TOTK_FM_FIXED
    boot_fm_sync_pending();
#endif
    if(boot_mt.requested>1 && !boot_mt.active && !boot_mt.hijacked && r>0 && c->thread_count>=2){
        boot_mt.saved_limit=c->limit;c->limit=0;boot_mt.hijacked=1;
    }
    return r;
}
/* Host functions (host_fn.h) and their subsystems: NVN API (host_nvn.h), native file system
 * (host_svc_fs.h, svc-work stage 1). boot_host_fn_dispatch is a runtime handler (split_runtime.py):
 * pages and the dispatch unit only see its declaration. host_svc_fs.h is runtime_support-only
 * (TOTK_DRAW_PATH, the runtime_support define), so the generated page header does not change. */
#include "host_fn.h"
#include "host_nvn.h"
#ifdef TOTK_DRAW_PATH
#include "host_svc_fs.h"
#include "host_sdk.h"   /* native SDK layer: nn::os locks, nn::fs directories (play-work/SDK.md) */
#include "host_movie.h" /* native movie:: (subsdk0): cutscene movies via libwebm + libvpx (play-work/movie-work) */
#endif
#ifndef TOTK_HOST_FN_SUBSYSTEMS
static inline void boot_host_fn_subsystems(void){
    boot_nvn_register();
#ifdef TOTK_DRAW_PATH
    (void)boot_svc_fs_register();
    (void)boot_sdk_register();
    (void)boot_movie_register();
#endif
}
#endif
static inline int boot_host_fn_dispatch(BootContext*c,uint64_t*next,uint32_t*direct){
    int r=boot_host_fn_call(c,next,direct);
#if TOTK_FASTMEM && TOTK_FM_FIXED
    boot_fm_sync_pending();
#endif
    return r;
}
__declspec(dllexport) uint32_t boot_run(BootContext*c,uint64_t entry_offset);
/* Expose the flag primitive for independent arithmetic-boundary tests. */
__declspec(dllexport) uint32_t boot_test_subflags(uint64_t a,uint64_t b,uint32_t width);
__declspec(dllexport) uint64_t boot_context_size(void);
#endif
