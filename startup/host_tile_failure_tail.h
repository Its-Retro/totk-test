#ifndef TOTK_HOST_TILE_FAILURE_TAIL_H
#define TOTK_HOST_TILE_FAILURE_TAIL_H
/*97: a separate, bounded copy of already-admitted89 CPU rows. This header
 * performs no guest/GPU/clock/lock operation and never uses the89 writer. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "host_tile_binding_trace_core.h"
#define BOOT_TILE97_ROWS 8u
typedef struct BootTile97Tail { BootTile89Record rows[BOOT_TILE97_ROWS]; } BootTile97Tail;
typedef struct BootTile97Snapshot {
    uint64_t pid,creation,start,frequency,arm,end,last_qpc,stop_qpc;
    uint64_t accepted,draws,signals,loss[8];
    unsigned duration_ms,max_draws,state,reason,arm_consumed,arm_bytes,ledger_taint,count;
    BootTile89Record rows[BOOT_TILE97_ROWS];
} BootTile97Snapshot;
/* Called only after successful89 admission, under its existing producer lock. */
static inline void boot_tile97_commit(BootTile97Tail *tail,const BootTile89Record *row) {
    tail->rows[(row->serial-1)%BOOT_TILE97_ROWS]=*row;
}
/* The caller already owns the89 lock. No pointers into the mutable tail escape. */
static inline int boot_tile97_copy(BootTile97Snapshot *snapshot,const BootTile97Tail *tail) {
    if(snapshot->accepted>BOOT_TILE89_MAX_ROWS)return 0;
    snapshot->count=snapshot->accepted<BOOT_TILE97_ROWS?(unsigned)snapshot->accepted:BOOT_TILE97_ROWS;
    uint64_t first=snapshot->accepted-snapshot->count+1,last=0;
    for(unsigned i=0;i<snapshot->count;++i) {
        const BootTile89Record *row=&tail->rows[(first+i-1)%BOOT_TILE97_ROWS];
        if(row->serial!=first+i || !row->qpc || (i && row->qpc<last) ||
           (row->kind!=BOOT_TILE89_DRAW && row->kind!=BOOT_TILE89_SIGNAL))return 0;
        snapshot->rows[i]=*row;last=row->qpc;
    }
    return !snapshot->count || last==snapshot->last_qpc;
}
/* Existing failure-log FILE only. A successful write is not a flush/close or
 * persistence receipt. Partial output has no usable complete97 framing. */
static inline int boot_tile97_output(FILE *file,const char *line,size_t bytes) {
    return fwrite(line,1,bytes,file)==bytes && !ferror(file);
}
static inline int boot_tile97_unavailable(FILE *file,const char *reason) {
    char line[256];int n=snprintf(line,sizeof line,"TILE97 {\"kind\":\"unavailable\",\"version\":97,\"reason\":\"%s\",\"capture_complete_proven\":false}\n",reason);
    return n>0 && (size_t)n<sizeof line && boot_tile97_output(file,line,(size_t)n);
}
static inline int boot_tile97_format_row(FILE *file,const BootTile89Record *row) {
    char line[12288],vs[65],ps[65];size_t used=0;
    for(unsigned i=0;i<32;++i){snprintf(vs+i*2,3,"%02x",row->vs[i]);snprintf(ps+i*2,3,"%02x",row->ps[i]);}
#define BOOT_TILE97_APP(...) do { int n=snprintf(line+used,sizeof line-used,__VA_ARGS__);if(n<0 || (size_t)n>=sizeof line-used)return 0;used+=(size_t)n; } while(0)
    BOOT_TILE97_APP("TILE97 {\"kind\":\"%s\",\"qpc\":%llu,\"serial\":%llu,\"thread\":%llu,\"vs\":\"%s\",\"ps\":\"%s\",\"v\":[",row->kind==BOOT_TILE89_DRAW?"draw":"signal",(unsigned long long)row->qpc,(unsigned long long)row->serial,(unsigned long long)row->thread,vs,ps);
    for(unsigned i=0;i<BOOT_TILE89_VALUES;++i)BOOT_TILE97_APP("%s%llu",i?",":"",(unsigned long long)row->v[i]);
    BOOT_TILE97_APP("],\"raw\":[");for(unsigned i=0;i<BOOT_TILE89_RAW;++i)BOOT_TILE97_APP("%s%u",i?",":"",row->raw[i]);BOOT_TILE97_APP("]}\n");
#undef BOOT_TILE97_APP
    return boot_tile97_output(file,line,used);
}
static inline int boot_tile97_format(FILE *file,const BootTile97Snapshot *snapshot) {
    char line[3072];const BootTile97Snapshot *s=snapshot;
    int n=snprintf(line,sizeof line,
        "TILE97 {\"kind\":\"header\",\"version\":97,\"scope\":\"partial_accepted_cpu_tail\",\"pid\":%llu,\"creation\":%llu,\"start_qpc\":%llu,\"qpc_frequency\":%llu,\"duration_ms\":%u,\"max_draws\":%u,\"arm_qpc\":%llu,\"end_qpc\":%llu,\"last_qpc\":%llu,\"stop_qpc\":%llu,\"state\":%u,\"reason\":%u,\"arm_consumed\":%u,\"arm_bytes\":%u,\"accepted\":%llu,\"draws\":%llu,\"signals\":%llu,\"tail_rows\":%u,\"tail_limit\":8,\"value_count\":224,\"raw_words\":32,\"loss\":[%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu],\"ledger_taint\":%u,\"capture_complete_proven\":false,\"global_last_writer_proven\":false,\"hung_draw_identity_proven\":false,\"gpu_completion_observed\":false,\"terminal_io_known\":false}\n",
        (unsigned long long)s->pid,(unsigned long long)s->creation,(unsigned long long)s->start,(unsigned long long)s->frequency,s->duration_ms,s->max_draws,
        (unsigned long long)s->arm,(unsigned long long)s->end,(unsigned long long)s->last_qpc,(unsigned long long)s->stop_qpc,s->state,s->reason,s->arm_consumed,s->arm_bytes,
        (unsigned long long)s->accepted,(unsigned long long)s->draws,(unsigned long long)s->signals,s->count,
        (unsigned long long)s->loss[0],(unsigned long long)s->loss[1],(unsigned long long)s->loss[2],(unsigned long long)s->loss[3],(unsigned long long)s->loss[4],(unsigned long long)s->loss[5],(unsigned long long)s->loss[6],(unsigned long long)s->loss[7],s->ledger_taint);
    if(n<0 || (size_t)n>=sizeof line || !boot_tile97_output(file,line,(size_t)n))return 0;
    for(unsigned i=0;i<s->count;++i)if(!boot_tile97_format_row(file,&s->rows[i]))return 0;
    n=snprintf(line,sizeof line,"TILE97 {\"kind\":\"end\",\"version\":97,\"pid\":%llu,\"creation\":%llu,\"start_qpc\":%llu,\"rows\":%u,\"first_serial\":%llu,\"last_serial\":%llu,\"capture_complete_proven\":false,\"terminal_io_known\":false}\n",
        (unsigned long long)s->pid,(unsigned long long)s->creation,(unsigned long long)s->start,s->count,(unsigned long long)(s->count?s->accepted-s->count+1:0),(unsigned long long)s->accepted);
    return n>0 && (size_t)n<sizeof line && boot_tile97_output(file,line,(size_t)n);
}
#endif
