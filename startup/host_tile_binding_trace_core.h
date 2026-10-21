#ifndef TOTK_HOST_TILE_BINDING_TRACE_CORE_H
#define TOTK_HOST_TILE_BINDING_TRACE_CORE_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define BOOT_TILE89_VALUES 224u
#define BOOT_TILE89_RAW 32u
#define BOOT_TILE89_RING 256u
#define BOOT_TILE89_MAX_DRAWS 4096u
#define BOOT_TILE89_MAX_ROWS 8192u
#define BOOT_TILE89_BIND_BASE 64u
#define BOOT_TILE89_BIND_STRIDE 14u
#define BOOT_TILE89_RT_BASE 148u
#define BOOT_TILE89_RT_STRIDE 6u
enum { BOOT_TILE89_DRAW=1, BOOT_TILE89_SIGNAL=2 };
enum { BOOT_TILE89_STOP_TIME=1,BOOT_TILE89_STOP_CAP=2,BOOT_TILE89_STOP_IO=3,
       BOOT_TILE89_STOP_UNARMED=4,BOOT_TILE89_STOP_BAD_ARM=5,BOOT_TILE89_STOP_DEVICE=6,BOOT_TILE89_STOP_CLOCK=7 };
enum { BOOT_TILE89_BAD_TABLE=1u,BOOT_TILE89_BAD_BINDING=2u,BOOT_TILE89_BAD_ABORT=4u,
       BOOT_TILE89_BAD_NEST=8u,BOOT_TILE89_BAD_ISSUE=16u,BOOT_TILE89_BAD_LEDGER=32u };
enum { BOOT_TILE89_VS_C0=1u,BOOT_TILE89_PS_C0=2u,BOOT_TILE89_VS_C4=4u,
       BOOT_TILE89_PS_C4=8u,BOOT_TILE89_HOST_VS=16u,BOOT_TILE89_HOST_PS=32u };
typedef struct BootTile89Config { uint32_t duration_ms,max_draws; } BootTile89Config;
typedef struct BootTile89Record {
    uint64_t qpc,serial,thread,kind; uint8_t vs[32],ps[32];
    uint64_t v[BOOT_TILE89_VALUES]; uint32_t raw[BOOT_TILE89_RAW];
} BootTile89Record;
static const uint8_t boot_tile89_vs[32]={0xc0,0x81,0xf4,0x7a,0x27,0x58,0x66,0x0a,0xa0,0xc1,0x83,0x50,0xa2,0x82,0x1d,0xb9,0x47,0x11,0x90,0x42,0x39,0xa2,0x73,0xac,0xdc,0x54,0xb2,0xd6,0xdf,0x0a,0xcf,0xde};
static const uint8_t boot_tile89_ps[32]={0xc7,0x5c,0x17,0x3e,0x64,0x54,0xbc,0x68,0x3a,0x9c,0x26,0x54,0x86,0x42,0x50,0x4c,0x7f,0xf9,0x58,0x18,0x4f,0x93,0x1e,0x6d,0xb8,0x29,0x8b,0x02,0x61,0x50,0x30,0xfd};
static inline int boot_tile89_pair(const uint8_t vs[32],const uint8_t ps[32]) {
    return !memcmp(vs,boot_tile89_vs,32) && !memcmp(ps,boot_tile89_ps,32);
}
static inline int boot_tile89_uint(const char **at,uint64_t *value) {
    const char *p=*at; uint64_t n=0;
    if(*p<'0' || *p>'9' || (*p=='0' && p[1]>='0' && p[1]<='9')) return 0;
    do { unsigned digit=(unsigned)(*p-'0');if(n>(UINT64_MAX-digit)/10) return 0;n=n*10+digit;++p; } while(*p>='0' && *p<='9');
    *at=p;*value=n;return 1;
}
static inline int boot_tile89_config(const char *text,BootTile89Config *out) {
    uint64_t duration,draws; const char *p=text;
    if(!p || !boot_tile89_uint(&p,&duration) || *p++!=':' || !boot_tile89_uint(&p,&draws) || *p ||
       !duration || duration>120000 || !draws || draws>BOOT_TILE89_MAX_DRAWS) return 0;
    out->duration_ms=(uint32_t)duration;out->max_draws=(uint32_t)draws;return 1;
}
static inline int boot_tile89_literal(const char **at,const char *literal) {
    size_t n=strlen(literal);if(strncmp(*at,literal,n))return 0;*at+=n;return 1;
}
/* Canonical, bounded arm JSON. Fixed field order and no duplicate/extra keys. */
static inline int boot_tile89_arm(const char *text,size_t bytes,uint64_t pid,uint64_t creation,uint64_t start) {
    if(!text || !bytes || bytes>512 || memchr(text,0,bytes))return 0;
    char bounded[513];memcpy(bounded,text,bytes);bounded[bytes]=0;
    const char *p=bounded;uint64_t got_pid,got_creation,got_start;
    if(!boot_tile89_literal(&p,"{\"version\":89,\"command\":\"start\",\"pid\":") || !boot_tile89_uint(&p,&got_pid) ||
       !boot_tile89_literal(&p,",\"creation\":") || !boot_tile89_uint(&p,&got_creation) ||
       !boot_tile89_literal(&p,",\"start_qpc\":") || !boot_tile89_uint(&p,&got_start) || !boot_tile89_literal(&p,"}"))return 0;
    if(p<bounded+bytes && *p=='\r')++p;if(p<bounded+bytes && *p=='\n')++p;
    return p==bounded+bytes && got_pid==pid && got_creation==creation && got_start==start && pid && creation && start;
}
static inline unsigned boot_tile89_bind_index(unsigned stage,unsigned slot) {
    if(stage>1)return UINT32_MAX;
    unsigned j=slot==0?0:slot==4?1:slot==18?2:UINT32_MAX;
    return j==UINT32_MAX?j:stage*3+j;
}
/* Field dictionary is deliberately fixed, numeric and shader-family specific.
 * v0 token,1 sub,2 event,3 renderer,4 device,5 queue,6 CL,7 allocator,8 segment,
 * 9 fenceobject,10 PSO,11 VSsignature,12/13 VSdxil/bytes,14/15 PSdxil/bytes,
 * 16 count,17 instances,18 indexed,19 topology,20 RTcount,21 samples,
 * 22 first,23 basevertex,24 baseinstance,25 nativeknown,26..31 nativecontext,
 * 32..39 intended CPU HostState,40 stores,41 pendingBefore,42 guestbarrier,43 serialize,
 * 44 oldStorePS,45 serialCondition,46 emitted,47/48 UAVcounterBefore/After,
 * 49/50 settleBefore/After,51 rawbarrierKnown,52 rawflags,53 recordAddress,
 * 54 barrierSequence,55 executor,56 resolverErrors,57 life,58 Reset,
 * 59 workSerial,60 ordinal,61 workTicketKnown,62 rawKnownMask,63 tableMask.
 * Binding j at64+14*j: requestVA,requestSize,valid,CVlocation,CVsize,
 * resource,offset,effectiveBound,mode,region,scope,list,mapGeneration,flags.
 * RT j at148+6*j: resource,VA,WHpack,formatStatepack,layerLayerspack,pitchMap.
 * v196/197 actualVS/PStableBase,198/199 tableReused,200/201 tableConsistent,
 * 202/203 tableScope,204/205 tableList,206/207 tableMapGeneration,
 * 208 nativeOrdinal,209 nativeExpanded,210 nativeInstances,211 infoKnown,
 * 212 pendingBeforeKnown,213 settleResourceCount,214..221 first8settleResources,
 * 222 omittedSettleResources,223 gpuWriteAllUnknown.
 * Raw0..3 VS c0[21],4..7 PS c0[85],8..15 VS c4[146/147],16..23 PS same,
 * 24..31 owned VS HostState bytes (PS copy must agree to set PS-known bit).
 * Unknown raw slots stay zero and MUST use mask.
 * Signal rows use v3..9 owner pointers,57 life,58 Reset,59 Execute,
 * 60 firstWork,61 lastWork,62 ticketKnown,63 originalSignalHRESULT,
 * 64 actualSignalValue,65 ledgerTaintMask (lostLock=1/missing=2/eviction=4).
 * A known ticket requires zero observed monotone82 taint at work/Execute/Signal;
 * later metadata loss cannot retroactively establish a generation identity.
 * Signal is not a GPU completion observation. */
#endif
