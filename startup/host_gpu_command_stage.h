#ifndef TOTK_HOST_GPU_COMMAND_STAGE_H
#define TOTK_HOST_GPU_COMMAND_STAGE_H
#include "host_gpu_packets.h"
#include "generated_gpu_macros.h"
#include "host_gpu_register_profile.h"
#include "host_gpu_initial_state.h"
#include "host_gpu_sync.h"
#ifdef TOTK_DRAW_PATH
#include "host_gpu_stage_draw.h"
#endif
#include <string.h>
#include <stdlib.h>
/* kind occupies existing ABI padding: 0 store, 1/2 timestamp low/high,
 * 3 syncpoint increment, 4/5 ZPASS counter low/high (value of kind 4 = report type; both are
 * replaced by plain stores of the host GPU count before commit, host_gpu_draw_exec.h).
 * Retained stage size and member offsets stay stable. */
typedef struct BootGpuMemoryWrite {uint64_t gpu;uint32_t value,kind;} BootGpuMemoryWrite;
typedef struct BootGpuStage {
    uint32_t classes[8],registers[8][4096];
    uint8_t defined[8][4096];
    uint32_t code[2048],bindings[128],code_pos,bind_pos;
    uint8_t code_valid[2048],bind_valid[128];
    uint32_t code_pos_valid,bind_pos_valid,macro_sub,staged,macro_calls;
    uint32_t error,error_class,error_method,error_value,error_packet;
    BootGpuRegisterProfile profile;
    BootGpuMemoryWrite writes[8192];
    uint32_t write_count,dma_remaining,barriers,shadow_mode;
    uint64_t dma_address;
#ifdef TOTK_DRAW_PATH
    BootDrawRecorder draw;   /* draw/clear events and latched constant-buffer bindings */
#endif
} BootGpuStage;
static inline int boot_gpu_stage_fail(BootGpuStage*s,uint32_t code,uint32_t cls,uint32_t method,uint32_t value){
    s->error=code;s->error_class=cls;s->error_method=method;s->error_value=value;return 0;
}
/* Ordered staged writes. The first 8192 live in the stage (its retained layout is fixed); a
 * submission with more (large inline uploads) continues in a module-owned log that then holds
 * all of them contiguously. Stages are staged one at a time (window lock); the log belongs to
 * the stage that grew it, and boot_gpu_stage_writes() is the only way to read the list. */
#define BOOT_GPU_STAGE_MAX_WRITES (1u<<21)
static struct {const BootGpuStage*owner;BootGpuMemoryWrite*log;uint32_t capacity;} boot_gpu_write_log;
static inline BootGpuMemoryWrite*boot_gpu_stage_writes(BootGpuStage*s){
    return s->write_count>8192 && boot_gpu_write_log.owner==s?boot_gpu_write_log.log:s->writes;
}
static inline int boot_gpu_stage_push(BootGpuStage*s,BootGpuMemoryWrite w){
    if(s->write_count<8192){s->writes[s->write_count++]=w;return 1;}
    if(s->write_count>=BOOT_GPU_STAGE_MAX_WRITES)return 0;
    if(s->write_count==8192 || boot_gpu_write_log.owner!=s){
        if(s->write_count!=8192)return 0;   /* the log was taken over by another stage */
        boot_gpu_write_log.owner=s;
        if(!boot_gpu_write_log.log){boot_gpu_write_log.capacity=65536;boot_gpu_write_log.log=malloc(65536*sizeof(BootGpuMemoryWrite));
            if(!boot_gpu_write_log.log){boot_gpu_write_log.owner=NULL;return 0;}}
        memcpy(boot_gpu_write_log.log,s->writes,8192*sizeof(BootGpuMemoryWrite));
    }
    if(s->write_count==boot_gpu_write_log.capacity){
        BootGpuMemoryWrite*g=realloc(boot_gpu_write_log.log,(size_t)boot_gpu_write_log.capacity*2*sizeof(*g));
        if(!g)return 0;boot_gpu_write_log.log=g;boot_gpu_write_log.capacity*=2;
    }
    boot_gpu_write_log.log[s->write_count++]=w;return 1;
}
static inline int boot_gpu_stage_memory(BootGpuStage*s,uint64_t gpu,uint32_t value){
    if(gpu>UINT64_C(0xfffffffffc) || (gpu&3) || !boot_gpu_stage_push(s,(BootGpuMemoryWrite){gpu,value,0}))return boot_gpu_stage_fail(s,11,0xb197,0x1b4,value);
    return 1;
}
static inline int boot_gpu_stage_semaphore(BootGpuStage*s,const BootGpuSemaphore*op){
    int ok=boot_gpu_stage_push(s,(BootGpuMemoryWrite){op->address,op->payload,0});
    if(ok && op->words==4)
        ok=boot_gpu_stage_push(s,(BootGpuMemoryWrite){op->address+4,0,0}) && boot_gpu_stage_push(s,(BootGpuMemoryWrite){op->address+8,0,1}) &&
           boot_gpu_stage_push(s,(BootGpuMemoryWrite){op->address+12,0,2});
    if(!ok)return boot_gpu_stage_fail(s,11,0xb197,0x1c,0);
    ++s->barriers;return 1;
}
#ifdef TOTK_DRAW_PATH
/* NVB197 report semaphore of ZPASS_PIXEL_CNT (2) or ZPASS_PIXEL_CNT64 (0x15), operation RELEASE or
 * REPORT_ONLY at pipeline location ALL. The value is the channel's sample counter: samples passing the
 * depth/stencil tests in draws with SET_ZPASS_PIXEL_COUNT enabled since the last CLEAR_REPORT_VALUE
 * ZPASS_PIXEL_CNT (suyu Maxwell3D::ProcessQueryGet / ProcessCounterReset, query_cache CounterReport).
 * It is known only after the preceding draws ran, so an ordered report event marks the point and the
 * value words are staged as kinds 4/5, resolved from D3D12 occlusion queries before commit. Four-word
 * form: 64-bit value, then the 64-bit timestamp (kinds 1/2) like every other report; one word: the
 * low 32 bits. Sub-reports, reductions, awaken and the second-dword select stop here. */
static inline int boot_gpu_stage_zpass_report(BootGpuStage*s,uint32_t sub,uint32_t control){
    const uint32_t*regs=s->registers[sub];
    uint32_t op=control&3,report=(control>>23)&31;int four=!((control>>28)&1);
    if((control&~UINT32_C(0x1f80f017)) || (op!=0 && op!=2) || ((control>>12)&15)!=15 || (report!=2 && report!=0x15))return 0;
    uint64_t address=((uint64_t)regs[0x1b00/4]<<32)|regs[0x1b04/4];
    if(regs[0x1b00/4]>255 || (address&3) || address>UINT64_C(0xffffffffff)-(four?15:3))return 0;
    if(!boot_draw_counter_point(&s->draw,s->write_count,6))return 0;
    int ok=boot_gpu_stage_push(s,(BootGpuMemoryWrite){address,report,4});
    if(ok && four)ok=boot_gpu_stage_push(s,(BootGpuMemoryWrite){address+4,0,5}) && boot_gpu_stage_push(s,(BootGpuMemoryWrite){address+8,0,1}) &&
                     boot_gpu_stage_push(s,(BootGpuMemoryWrite){address+12,0,2});
    if(!ok)return 0;
    ++s->barriers;return 1;
}
/* Resolve the ZPASS reports of an executed submission: walk its events in order with the counter
 * carried from the previous submission (*counter): resets zero it, counted draws add their query's
 * samples (counts[slot], slot = clear_regs[1] - 1, below `slots`), reports turn their kind 4/5 words into
 * plain stores of the value. *counter ends as the value after the submission. 0 on inconsistency. */
static inline int boot_gpu_stage_zpass_resolve(BootGpuStage*s,const uint64_t*counts,uint32_t slots,uint64_t*counter,uint32_t*reports){
    BootGpuMemoryWrite*w=boot_gpu_stage_writes(s);uint64_t value=*counter;*reports=0;
    for(uint32_t i=0;i<s->draw.count;++i){BootDrawEvent*e=&s->draw.events[i];
        if(e->kind==5)value=0;
        else if(e->kind==1 && e->clear_regs[1]){if(e->clear_regs[1]>slots || !counts)return 0;value+=counts[e->clear_regs[1]-1];}
        else if(e->kind==6){uint32_t k=e->write_index;
            if(k>=s->write_count || w[k].kind!=4)return 0;
            uint64_t v=w[k].value==2?(uint32_t)value:value;
            w[k].value=(uint32_t)v;w[k].kind=0;
            if(k+1<s->write_count && w[k+1].kind==5){w[k+1].value=(uint32_t)(v>>32);w[k+1].kind=0;}
            ++*reports;
        }
    }
    *counter=value;return 1;
}
#endif
static inline int boot_gpu_stage_state_classify(uint32_t cls,uint32_t method);
static inline int boot_gpu_stage_state_method(uint32_t cls,uint32_t method,uint32_t value){
    if(cls==0xb197 || cls==0xb1c0){
        switch(method){
            case 0x155c:case 0x1574:case 0x790:return value<=255;
            case 0x1560:case 0x1578:case 0x794:return 1;
            case 0x1564:return value<=0xfffff;
            case 0x157c:return value<=0x3fffff;
            default:break;
        }
    }
    /* An exact (class, method, value) entry of the initial-state table is accepted; otherwise the
     * classification below decides. The classification does not depend on the table, so it runs
     * first and the table (a linear list) is searched only for methods it would refuse. */
    if(boot_gpu_stage_state_classify(cls,method))return 1;
    for(size_t i=0;i<sizeof(boot_gpu_initial_state)/sizeof(boot_gpu_initial_state[0]);++i){const uint32_t*t=boot_gpu_initial_state[i];if(t[0]==cls && t[1]==method && t[2]==value)return 1;}
    return 0;
}
static inline int boot_gpu_stage_state_classify(uint32_t cls,uint32_t method){
    if(cls==0x902d)return method==0x2ac || method==0x290 || method==0x884;
    if(cls!=0xb197)return 0;
    /* Classified from the complete NVB197 method list (gpu-reference/b197-methods.txt).
     * Actions must be implemented explicitly; until then they stop here. Cache
     * invalidates, flushes, ZCULL hints and PIPE_NOP have no effect on coherent host
     * memory. Every other method only records state for the next action to read. */
    switch(method){
        case 0x10c:case 0x13c:case 0x140:case 0x144:case 0x200:case 0x2e0:case 0x2f0:case 0x304:
        case 0xdf4:case 0xf18:case 0x1114:case 0x11fc:case 0x1214:case 0x1218:case 0x123c:
        case 0x1304:case 0x15e8:case 0x15f0:
#ifndef TOTK_DRAW_PATH
        case 0x1530:   /* CLEAR_REPORT_VALUE: counters are not modelled; reports write semaphores only */
#endif
#ifndef TOTK_DRAW_PATH
        case 0xd78:case 0x1614:case 0x1618:case 0x17e0:case 0x19d0:
#endif
        case 0x17e4:case 0x17e8:case 0x17ec:case 0x17f0:case 0x17f4:case 0x17f8:
            return 0;                                   /* draw/clear/report/decompress actions */
        case 0x21c:case 0x2d4:case 0x2fc:case 0x11f4:case 0x120c:case 0x1210:case 0x1330:case 0x1334:
        case 0x1338:case 0x1500:case 0x1958:case 0x1978:case 0x1a2c:
            return 1;                                   /* cache/ZCULL maintenance: no host effect */
        default:
            /* Latching methods: they capture other registers at write time, so storing
             * the value alone would lose information. Stop until implemented. */
            if(method>=0x390 && method<0x390+4*32 && !((method-0x390)&31))return 0;   /* SET_STREAM_OUT_BUFFER_LOAD_WRITE_POINTER(j) */
#ifndef TOTK_DRAW_PATH
            if(method>=0x2410 && method<0x2410+5*32 && !((method-0x2410)&31))return 0;   /* BIND_GROUP_CONSTANT_BUFFER(j) */
#endif
            if(method<0x3800)return 1;                  /* state */
            break;
    }
    if((method>=0x3400 && method<0x3600) || (method>=0xd00 && method<0xd40) ||
       (method>=0xe00 && method<=0xef0 && !(method&15)) || (method>=0x1880 && method<=0x18bc))return 1;
    /* Reviewed state-only registers in the initial submission. Actions such as
     * FALCON04, DMA launch, draw and clear are deliberately absent. */
    switch(method){
        case 0x1534:case 0x15b4:case 0x15d0:case 0x153c:case 0x10cc:case 0x10e0:case 0x10e4:case 0x10ec:case 0x10f0:
        case 0x74c:case 0x10f8:case 0x16a8:case 0x1794:case 0xde8:case 0x165c:case 0x1234:case 0x1684:case 0x1528:
        case 0x1000:case 0x218:case 0x10fc:case 0x1290:case 0x12d8:case 0x12dc:case 0xd64:case 0x1140:case 0x1610:
        case 0x164c:case 0x151c:case 0x20c:case 0x30c:case 0x300:case 0xfdc:case 0x19c0:case 0xf8c:case 0x1594:
        case 0x2608:case 0x310:case 0x77c:case 0x1130:case 0x374:case 0x15cc:case 0x1220:case 0x1450:return 1;
        default:return 0;
    }
}
/* MME shadow RAM, as used by NVN/deko3d: PASSTHROUGH writes temporary state, REPLAY then
 * re-sends the same methods and the hardware substitutes the shadowed (pre-passthrough)
 * values. The stage keeps one register file, so the first passthrough write of each
 * register records the value it replaces; that value is the shadow, because tracking
 * was on until passthrough began. The record lives in the host module, not the stage, and
 * is reset at each switch to passthrough; replay without a recorded passthrough stops. */
static struct { uint8_t saved[8][4096],had[8][4096];uint32_t value[8][4096];int active; } boot_gpu_shadow;
static inline int boot_gpu_shadow_filter(BootGpuStage*s,uint32_t sub,uint32_t method,uint32_t*value){
    unsigned m=method/4;
    if(s->shadow_mode==2){
        if(!boot_gpu_shadow.active)return boot_gpu_stage_fail(s,4,0xb197,0x124,2);
        if(!boot_gpu_shadow.saved[sub][m]){boot_gpu_shadow.saved[sub][m]=1;boot_gpu_shadow.had[sub][m]=s->defined[sub][m];boot_gpu_shadow.value[sub][m]=s->registers[sub][m];}
    }else if(s->shadow_mode==3){
        if(!boot_gpu_shadow.active)return boot_gpu_stage_fail(s,4,0xb197,0x124,3);
        if(boot_gpu_shadow.saved[sub][m]){
            if(!boot_gpu_shadow.had[sub][m])return boot_gpu_stage_fail(s,4,0xb197,method,*value);
            *value=boot_gpu_shadow.value[sub][m];
        }else{
            if(!s->defined[sub][m])return boot_gpu_stage_fail(s,4,0xb197,method,*value);
            *value=s->registers[sub][m];
        }
    }
    return 1;
}
/* NVB1C0 (Maxwell compute) methods, classified from the complete class list (gpu-reference/clb1c0.h).
 * Returns 1 when the value is to be stored in the register file. Actions: inline upload (I2M),
 * report semaphores, and SEND_SIGNALING_PCAS_B with SCHEDULE, which records an ordered compute
 * dispatch (QMD at SEND_PCAS_A << 8, read when the event executes; host_gpu_compute.h). Anything
 * whose effect is not implemented (falcon firmware calls, dependent-QMD scheduling, conditional
 * render enable, trap semaphores, reductions) stops here. */
static inline int boot_gpu_stage_compute(BootGpuStage*s,uint32_t sub,uint32_t method,uint32_t value){
    const uint32_t*regs=s->registers[sub];const uint8_t*def=s->defined[sub];
    switch(method){
        /* inline upload (same UPLOAD block as the 3D class): data words become ordered guest writes */
        case 0x180:case 0x184:case 0x188:case 0x18c:case 0x190:case 0x194:case 0x198:case 0x19c:
        case 0x1a0:case 0x1a4:case 0x1a8:case 0x1ac:return 1;
        case 0x1b0:
            /* pitch destination (bit 0), flush-only completion (bits 4-5 = 1); bit 6 (sysmembar disable) and
             * bit 12 (semaphore struct size) have no effect without a semaphore release. */
            if((value&~0x1040u)!=0x11 || s->dma_remaining || !def[0x180/4] || !def[0x184/4] || !def[0x188/4] || !def[0x18c/4] ||
               regs[0x184/4]!=1 || regs[0x180/4]>32768 || regs[0x180/4]%4)return 0;
            s->dma_remaining=regs[0x180/4]/4;s->dma_address=((uint64_t)regs[0x188/4]<<32)|regs[0x18c/4];return 1;
        case 0x1b4:
            if(!s->dma_remaining || !boot_gpu_stage_memory(s,s->dma_address,value)){s->error=11;return 0;}
            --s->dma_remaining;s->dma_address+=4;return 1;
        /* report semaphore: RELEASE only, no reduction/awaken; four-word form carries a timestamp */
        case 0x1b00:return value<=255;
        case 0x1b04:return !(value&3);
        case 0x1b08:return 1;
        case 0x1b0c:{
            if((value&~UINT32_C(0x10000004)) || !def[0x1b00/4] || !def[0x1b04/4] || !def[0x1b08/4]){s->error=13;return 0;}
            BootGpuSemaphore op={((uint64_t)regs[0x1b00/4]<<32)|regs[0x1b04/4],regs[0x1b08/4],(value>>28)&1?1u:4u,(value>>28)&1?0u:1u,1};
            if(!boot_gpu_stage_semaphore(s,&op)){s->error=13;return 0;}
            return 1;}
        /* local memory sizes */
        case 0x2e4:case 0x2f0:return value<=255;
        case 0x2ec:case 0x2f8:return value<=511;
        case 0x2e8:case 0x2f4:return 1;
        /* pools, program region */
        case 0x155c:case 0x1574:return value<=255;
        case 0x1560:case 0x1578:case 0x160c:return 1;
        case 0x1564:return value<=0xfffff;
        case 0x157c:return value<=0x3fffff;
        case 0x1608:return value<=255;
        /* state read by later actions or without host effect */
        case 0x104:case 0x108:case 0x130:case 0x134:case 0x150:case 0x154:case 0x1dc:case 0x1e0:case 0x1e4:
        case 0x1f0:case 0x1f4:case 0x1f8:case 0x1fc:case 0x200:case 0x204:case 0x208:case 0x20c:case 0x214:
        case 0x240:case 0x248:case 0x26c:case 0x270:case 0x274:case 0x278:case 0x280:case 0x288:case 0x28c:
        case 0x29c:case 0x2b0:case 0x2b4:case 0x310:case 0x77c:case 0x790:case 0x794:case 0xd94:case 0xde4:
        case 0x1528:case 0x1550:case 0x1554:case 0x1a30:case 0x1a34:case 0x1a38:case 0x1a3c:case 0x2608:case 0x260c:
        case 0x33dc:
            return 1;
        case 0x218:return value==1;                     /* Maxwell texture headers (the only layout host_texture_desc.h decodes) */
        case 0x138:case 0x1558:return value==1;        /* render enable: only TRUE; FALSE/conditional suppress launches (not implemented) */
        case 0x1944:return value==0;                    /* render enable override: USE_RENDER_ENABLE */
        /* cache maintenance, idle waits, checks and no-ops: no host effect (coherent host memory, in-order execution) */
        case 0x100:case 0x13c:case 0x140:case 0x144:case 0x210:case 0x21c:case 0x244:case 0x27c:case 0x284:case 0x290:
        case 0x298:case 0xf44:case 0xf48:case 0xf4c:case 0xf50:case 0x120c:case 0x1210:case 0x1288:case 0x12a8:
        case 0x1330:case 0x1334:case 0x1338:case 0x1424:case 0x1698:case 0x1a2c:case 0x33e0:case 0x33e4:
            return 1;
        case 0x110:case 0x294:if(value)return 0;++s->barriers;return 1;
        case 0x2bc:   /* SEND_SIGNALING_PCAS_B: bit 0 invalidate (no host effect), bit 1 schedule = launch */
            if(value&~3u)return 0;
            if(value&2){
#ifdef TOTK_DRAW_PATH
                if(!def[0x2b4/4] || !def[0x1608/4] || !def[0x160c/4]){s->error=14;return 0;}
                if(!boot_draw_compute(&s->draw,regs,s->write_count)){s->error=14;return 0;}
                ++s->barriers;
#else
                return 0;
#endif
            }
            return 1;
        default:
            if(method>=0x220 && method<=0x268 && method!=0x240 && method!=0x244 && method!=0x248)return 1;   /* reserved SW methods */
            if(method>=0x1040 && method<=0x106c)return 1;   /* spare no-ops */
            if(method>=0x333c && method<0x33dc)return 1;    /* performance counter configuration */
            /* initial-state values the driver writes once */
            for(size_t i=0;i<sizeof(boot_gpu_initial_state)/sizeof(boot_gpu_initial_state[0]);++i){const uint32_t*t=boot_gpu_initial_state[i];if(t[0]==0xb1c0 && t[1]==method && t[2]==value)return 1;}
            return 0;   /* 0x10c NOTIFY, 0x2b8 SEND_PCAS_B, 0x500.. FALCON, unknown */
    }
}
static inline int boot_gpu_stage_write(BootGpuStage*s,uint32_t sub,uint32_t method,uint32_t value){
    if(sub>=8 || method>=16384 || (method&3))return boot_gpu_stage_fail(s,1,0,method,value);
    uint32_t cls=s->classes[sub];
    if(method==0x10 || method==0x14 || method==0x18 || method==0x1c){
        if(method==0x1c){BootGpuSemaphore op;
            if(!s->defined[0][4] || !s->defined[0][5] || !s->defined[0][6] ||
               !boot_gpu_semaphore_fifo(s->registers[0][4],s->registers[0][5],s->registers[0][6],value,&op) ||
               !boot_gpu_stage_semaphore(s,&op))return boot_gpu_stage_fail(s,13,cls,method,value);
        }
        s->registers[0][method/4]=value;s->defined[0][method/4]=1;++s->staged;return 1;
    }
    if(method==8){++s->staged;return 1;}
    /* NVB06F_SET_REFERENCE: the channel's reference count, published in USERD once preceding methods
     * complete. Guests read it only through USERD, which this host does not map, so it is recorded
     * with the other host-method registers and has no further effect. */
    if(method==0x50){s->registers[0][0x50/4]=value;s->defined[0][0x50/4]=1;++s->staged;return 1;}
    if(method==0x2c && (value==0x80000000 || value==0x70000000)){++s->barriers;++s->staged;return 1;}
    if(!method){
        if(value!=0xb197 && value!=0xb1c0 && value!=0xb0b5 && value!=0xa140 && value!=0x902d)return boot_gpu_stage_fail(s,2,value,0,0);
        if(cls && cls!=value)return boot_gpu_stage_fail(s,2,value,0,0);
        s->classes[sub]=value;++s->staged;return 1;
    }
    if(cls==0xb197 && method<0x3800 && method!=0x124 && s->shadow_mode>=2 && !boot_gpu_shadow_filter(s,sub,method,&value))return 0;
    if(cls==0xb197){
#ifdef TOTK_DRAW_PATH
        /* Draw path: latching binds, BEGIN/END, draws and clears are recorded in order. */
        if(method>=0x2410 && method<0x2410+5*32 && !((method-0x2410)&31)){
            if(!boot_draw_bind_cbuf(&s->draw,s->registers[sub],method,value))return boot_gpu_stage_fail(s,4,cls,method,value);
            s->registers[sub][method/4]=value;s->defined[sub][method/4]=1;++s->staged;return 1;
        }
        if(method==0x110 || method==0x21c || method==0xda4 || method==0x1144 || method==0x1288)boot_draw_note_barrier();
        switch(method){
            case 0x1618:if(!boot_draw_begin(&s->draw,value))return boot_gpu_stage_fail(s,14,cls,method,value);break;
            case 0x1614:if(!boot_draw_end(&s->draw))return boot_gpu_stage_fail(s,14,cls,method,value);break;
            case 0xd78:if(!boot_draw_record(&s->draw,s->registers[sub],s->write_count,0,value))return boot_gpu_stage_fail(s,14,cls,method,value);break;
            case 0x17e0:if(!boot_draw_record(&s->draw,s->registers[sub],s->write_count,1,value))return boot_gpu_stage_fail(s,14,cls,method,value);break;
            case 0x19d0:if(!boot_draw_clear(&s->draw,s->registers[sub],s->write_count,value))return boot_gpu_stage_fail(s,14,cls,method,value);break;
            /* CLEAR_REPORT_VALUE: ZPASS_PIXEL_CNT resets the occlusion counter in order with the draws. Other
             * counters are not modelled (their reports stop at SET_REPORT_SEMAPHORE_D), so their resets have
             * no observable effect. */
            case 0x1530:if(value==1 && !boot_draw_counter_point(&s->draw,s->write_count,5))return boot_gpu_stage_fail(s,14,cls,method,value);break;
            default:break;
        }
#endif
        switch(method){
            case 0x798:if(value>63)return boot_gpu_stage_fail(s,4,cls,method,value);break;
            case 0x79c:break;
            case 0x1144:if(value)return boot_gpu_stage_fail(s,4,cls,method,value);++s->barriers;break;
            case 0x1b00:case 0x1b04:case 0x1b08:break;
            case 0x1b0c:{BootGpuSemaphore op;
#ifdef TOTK_DRAW_PATH
                if(((value>>23)&31)==2 || ((value>>23)&31)==0x15){
                    if(!s->defined[sub][0x1b00/4] || !s->defined[sub][0x1b04/4] || !boot_gpu_stage_zpass_report(s,sub,value))return boot_gpu_stage_fail(s,13,cls,method,value);
                    break;}
#endif
                if(!s->defined[sub][0x1b00/4] || !s->defined[sub][0x1b04/4] || !s->defined[sub][0x1b08/4] ||
                   !boot_gpu_semaphore_report(s->registers[sub][0x1b00/4],s->registers[sub][0x1b04/4],s->registers[sub][0x1b08/4],value,&op) ||
                   !boot_gpu_stage_semaphore(s,&op))return boot_gpu_stage_fail(s,13,cls,method,value);
                break;}
            case 0x2c8:{uint32_t index,clean,rop;
                if(!boot_gpu_syncpoint_decode(value,&index,&clean,&rop) || !boot_gpu_stage_push(s,(BootGpuMemoryWrite){index,1,3}))return boot_gpu_stage_fail(s,13,cls,method,value);
                ++s->barriers;break;}
            case 0xf80:if(value)return boot_gpu_stage_fail(s,4,cls,method,value);++s->barriers;break;
            case 0x2310:
                if(!s->defined[sub][0x3400/4] || !s->defined[sub][0x3404/4] || !s->defined[sub][0x3408/4] || s->registers[sub][0x3400/4]!=0 ||
                   !boot_gpu_profile_write(&s->profile,value,s->registers[sub][0x3404/4],s->registers[sub][0x3408/4]))return boot_gpu_stage_fail(s,4,cls,method,value);
                s->registers[sub][0x3400/4]=1;break;
            /* SET_MME_SHADOW_RAM_CONTROL (see boot_gpu_shadow above). */
            case 0x124:
                if(value>3 || (value==3 && !boot_gpu_shadow.active))return boot_gpu_stage_fail(s,4,cls,method,value);
                if(value==2){memset(&boot_gpu_shadow,0,sizeof boot_gpu_shadow);boot_gpu_shadow.active=1;}
                s->shadow_mode=value;break;
            case 0xda4:if(value!=0x1011)return boot_gpu_stage_fail(s,4,cls,method,value);++s->barriers;break;
            case 0x1288:case 0x1424:case 0x1428:if(value)return boot_gpu_stage_fail(s,4,cls,method,value);++s->barriers;break;
            case 0xdd8:case 0x180:case 0x184:case 0x188:case 0x18c:case 0x2380:case 0x2384:case 0x2388:case 0x238c:break;
            case 0x1b0:
                if(value!=0x1011 || s->dma_remaining || !s->defined[sub][0x180/4] || !s->defined[sub][0x184/4] || !s->defined[sub][0x188/4] || !s->defined[sub][0x18c/4] ||
                   s->registers[sub][0x184/4]!=1 || s->registers[sub][0x180/4]>32768 || s->registers[sub][0x180/4]%4)return boot_gpu_stage_fail(s,4,cls,method,value);
                s->dma_remaining=s->registers[sub][0x180/4]/4;s->dma_address=((uint64_t)s->registers[sub][0x188/4]<<32)|s->registers[sub][0x18c/4];break;
            case 0x1b4:
                if(!s->dma_remaining || !boot_gpu_stage_memory(s,s->dma_address,value))return boot_gpu_stage_fail(s,11,cls,method,value);
                --s->dma_remaining;s->dma_address+=4;break;
            case 0x114:if(value>=2048)return boot_gpu_stage_fail(s,3,cls,method,value);s->code_pos=value;s->code_pos_valid=1;break;
            case 0x118:if(!s->code_pos_valid || s->code_pos>=2048)return boot_gpu_stage_fail(s,3,cls,method,value);s->code_valid[s->code_pos]=1;s->code[s->code_pos++]=value;break;
            case 0x11c:if(value>=128)return boot_gpu_stage_fail(s,3,cls,method,value);s->bind_pos=value;s->bind_pos_valid=1;break;
            case 0x120:if(!s->bind_pos_valid || s->bind_pos>=128 || value>=2048)return boot_gpu_stage_fail(s,3,cls,method,value);s->bind_valid[s->bind_pos]=1;s->bindings[s->bind_pos++]=value;break;
            case 0x100:break; /* NO_OPERATION. */
            case 0x110:if(value)return boot_gpu_stage_fail(s,4,cls,method,value);break; /* No accepted GPU work exists in this stage. */
            default:
                if(method>=0x2390 && method<0x23d0){
                    if(!s->defined[sub][0x2380/4] || !s->defined[sub][0x2384/4] || !s->defined[sub][0x2388/4] || !s->defined[sub][0x238c/4])return boot_gpu_stage_fail(s,11,cls,method,value);
                    uint32_t offset=s->registers[sub][0x238c/4],size=s->registers[sub][0x2380/4];
                    uint64_t base=((uint64_t)s->registers[sub][0x2384/4]<<32)|s->registers[sub][0x2388/4];
                    if(size<4 || offset>size-4 || base>UINT64_MAX-offset || !boot_gpu_stage_memory(s,base+offset,value))return boot_gpu_stage_fail(s,11,cls,method,value);
                    s->registers[sub][0x238c/4]=offset+4;
                }else if(!boot_gpu_stage_state_method(cls,method,value))return boot_gpu_stage_fail(s,4,cls,method,value);
        }
    }
#ifdef TOTK_DRAW_PATH
    else if(cls==0xb0b5){   /* copy engine: registers are state; LAUNCH_DMA records an ordered copy event */
        s->registers[sub][method/4]=value;s->defined[sub][method/4]=1;
        if(method==0x300 && !boot_draw_copy(&s->draw,s->registers[sub],s->write_count,value))return boot_gpu_stage_fail(s,14,cls,method,value);
        ++s->staged;return 1;
    }
#endif
    else if(cls==0xb1c0){
        if(!boot_gpu_stage_compute(s,sub,method,value))return boot_gpu_stage_fail(s,s->error?s->error:4,cls,method,value);
    }else if(!boot_gpu_stage_state_method(cls,method,value))return boot_gpu_stage_fail(s,4,cls,method,value);
    s->registers[sub][method/4]=value;s->defined[sub][method/4]=1;++s->staged;return 1;
}
static inline int boot_gpu_stage_macro_read(void*opaque,uint32_t method,uint32_t*out){
    BootGpuStage*s=opaque;unsigned sub=s->macro_sub;
    if(method>=16384 || method%4 || !s->defined[sub][method/4])return boot_gpu_stage_fail(s,5,s->classes[sub],method,0);
    *out=s->registers[sub][method/4];return 1;
}
static inline int boot_gpu_stage_macro_write(void*opaque,uint32_t method,uint32_t value){
    BootGpuStage*s=opaque;return boot_gpu_stage_write(s,s->macro_sub,method,value);
}
static inline int boot_gpu_stage_macro(BootGpuStage*s,unsigned sub,unsigned index,const uint32_t*params,size_t count){
    if(sub>=8 || s->classes[sub]!=0xb197 || index>=128 || !s->bind_valid[index])return boot_gpu_stage_fail(s,6,0xb197,0x3800+index*8,0);
    /* The bound code must equal a translated program word-for-word. Uploads
     * are contiguous, so take the longest program the slot's code begins with. */
    unsigned start=s->bindings[index];int(*run)(BootGpuMacroIo*)=NULL;uint32_t best=0;
    for(size_t p=0;p<sizeof(boot_gpu_macro_programs)/sizeof(boot_gpu_macro_programs[0]);++p){
        const BootGpuMacroProgram*m=&boot_gpu_macro_programs[p];size_t i=0;
        if(m->count<=best || start>2048-m->count)continue;
        while(i<m->count && s->code_valid[start+i] && s->code[start+i]==m->words[i])++i;
        if(i==m->count){run=m->run;best=m->count;}
    }
    if(!run)return boot_gpu_stage_fail(s,7,0xb197,0x3800+index*8,0);
    s->macro_sub=sub;BootGpuMacroIo io={s,boot_gpu_stage_macro_read,boot_gpu_stage_macro_write,params,count,0,0,0};
    if(!run(&io)){if(!s->error)boot_gpu_stage_fail(s,8,0xb197,0x3800+index*8,io.error);return 0;}
    ++s->macro_calls;return 1;
}
static inline int boot_gpu_stage_methods(BootGpuStage*s,const BootGpuMethod*methods,size_t count){
    for(size_t i=0;i<count;++i){const BootGpuMethod*m=&methods[i];s->error_packet=m->packet_word;
        if(m->method>=0x3800 && m->method<0x3c00 && !(m->method&7)){
            uint32_t params[8192];size_t n=1;params[0]=m->value;
            while(i+1<count && methods[i+1].subchannel==m->subchannel && methods[i+1].method==m->method+4){if(n==8192)return boot_gpu_stage_fail(s,9,0xb197,m->method,0);params[n++]=methods[++i].value;}
            if(!boot_gpu_stage_macro(s,m->subchannel,(m->method-0x3800)/8,params,n))return 0;
        }else if(!boot_gpu_stage_write(s,m->subchannel,m->method,m->value))return 0;
    }
    if(s->dma_remaining)return boot_gpu_stage_fail(s,12,0xb197,0x1b4,s->dma_remaining);
    return 1;
}
#endif
