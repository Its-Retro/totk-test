#ifndef TOTK_HOST_LIGHT_INPUT_PROBE_CORE_H
#define TOTK_HOST_LIGHT_INPUT_PROBE_CORE_H
/*88: bounded CPU observations of actual owned-ring descriptors. Direct guest
 * resources never provide payload. No GPU or guest-memory operation is here. */
#include "host_gpu_cbv_tap.h"
#include <limits.h>
#define BOOT_L88_SUBS 41u
#define BOOT_L88_PER_SUB 8u
#define BOOT_L88_RECORDS (BOOT_L88_SUBS*BOOT_L88_PER_SUB)
#define BOOT_L88_BANKS 3u
#define BOOT_L88_BYTES 256u
enum { BOOT_L88_UNUSED=0,BOOT_L88_PENDING=1,BOOT_L88_DONE=2,BOOT_L88_ABORTED=3 };
enum { BOOT_L88_CONTEXT=1,BOOT_L88_BINDING=2,BOOT_L88_TABLE=3,BOOT_L88_UNISSUED=4,BOOT_L88_STOP=5,BOOT_L88_TIMEOUT=6 };
typedef struct BootLight88Config { uint64_t first,last,address0,address1; } BootLight88Config;
typedef struct BootLight88Record {
    volatile LONG state;
    unsigned slot,ordinal,tid,abort_reason,bindings,payload_mask,table_seen,resolver_closed,issued;
    uint64_t qpc_begin,qpc_table,qpc_draw,loss_begin,loss_end;
    BootCbvTapBegin begin;
    BootCbvTapBinding binding[BOOT_L88_BANKS]; /* owned pointer erased before publication */
    BootCbvTapTable table;
    uint8_t payload[BOOT_L88_BYTES];
} BootLight88Record;
static inline int boot_light88_number(const char **at,unsigned radix,uint64_t *out,char delimiter) {
    const char *p=*at; uint64_t n=0; unsigned digits=0;
    for(;*p && *p!=delimiter;++p) {
        unsigned d=*p>='0'&&*p<='9'?(unsigned)(*p-'0'):*p>='a'&&*p<='f'?(unsigned)(*p-'a'+10):*p>='A'&&*p<='F'?(unsigned)(*p-'A'+10):99u;
        if(d>=radix || n>(UINT64_MAX-d)/radix) return 0;
        n=n*radix+d; ++digits;
    }
    if(!digits || *p!=delimiter) return 0;
    *out=n; *at=p+(delimiter!=0); return 1;
}
static inline int boot_light88_parse(const char *text,BootLight88Config *out) {
    const char *p=text; BootLight88Config c={0};
    if(!p || !*p) return 0;
    if(!boot_light88_number(&p,10,&c.first,':') || !boot_light88_number(&p,10,&c.last,':') ||
       !boot_light88_number(&p,16,&c.address0,':') || !boot_light88_number(&p,16,&c.address1,0) ||
       !c.first || c.last<c.first || c.last-c.first>=BOOT_L88_SUBS || !c.address0 || !c.address1 || c.address0==c.address1) return -1;
    *out=c; return 1;
}
static inline unsigned boot_light88_slot(unsigned bank) { return bank==0?4u:bank==1?3u:1u; }
static inline unsigned boot_light88_offset(unsigned bank) { return bank==1?160u:0u; }
static inline unsigned boot_light88_bytes(unsigned bank) { return bank==0?128u:bank==1?96u:32u; }
static inline unsigned boot_light88_payload_at(unsigned bank) { return bank==0?0u:bank==1?128u:224u; }
static inline int boot_light88_hashes(const uint8_t vs[32],const uint8_t ps[32]) {
    static const uint8_t v[32]={0xee,0x7a,0x38,0x09,0x36,0x96,0xd2,0x45,0x14,0xf0,0x9c,0x9a,0x2d,0xfc,0x4f,0xd7,0x28,0x99,0xb6,0x10,0x14,0x84,0xf9,0x41,0xf3,0x40,0x21,0x05,0xb9,0x33,0x7e,0x9f};
    static const uint8_t p[32]={0x96,0xe4,0x4e,0xaa,0xa1,0x9a,0x7f,0xab,0x38,0x4c,0x2d,0xff,0x81,0x71,0xd3,0xa5,0x22,0xf2,0xf6,0x00,0xd6,0x1c,0x1a,0x65,0x7e,0xf5,0x1b,0xbd,0xfb,0x41,0x61,0x09};
    return !memcmp(v,vs,32) && !memcmp(p,ps,32);
}
static inline int boot_light88_select(const BootLight88Config *c,const BootCbvTapBegin *b) {
    if(b->sub<c->first || b->sub>c->last || b->rt_address[0]!=c->address0 || b->rt_address[1]!=c->address1 ||
       !boot_light88_hashes(b->vs,b->ps) || b->rt_count!=3 || b->count!=864 || b->instances!=1 || b->indexed!=1 || b->topology!=4 || b->samples!=1 ||
       !b->renderer || !b->draw || !b->pso || !b->token) return 0;
    for(unsigned j=0;j<3;++j) {
        const BootCbvTapTarget *t=&b->target[j];
        if(!b->rt_resource[j] || !b->rt_address[j] || t->width!=640 || t->height!=360 || t->format!=(j==2?28u:26u) || t->state!=4 || t->layer || t->layers!=1 || t->pitch || t->map>7) return 0;
    }
    return b->rt_resource[0]!=b->rt_resource[1] && b->rt_resource[0]!=b->rt_resource[2] && b->rt_resource[1]!=b->rt_resource[2];
}
/* Caller owns the nonblocking producer lease and only mutates PENDING records.
 * Actual descriptor bounds, not the original requested length, admit bytes. */
static inline int boot_light88_binding(BootLight88Record *r,const BootCbvTapBinding *b) {
    if(b->stage!=1 || b->group!=4) return 1;
    unsigned bank=0; while(bank<BOOT_L88_BANKS && boot_light88_slot(bank)!=b->slot) ++bank;
    if(bank==BOOT_L88_BANKS) return 1;
    if(r->resolver_closed || (r->bindings&(1u<<bank)) || b->token!=r->begin.token || b->renderer!=r->begin.renderer || b->draw!=r->begin.draw ||
       !b->resource || !b->location || (b->location&255) || (b->offset&255) || !b->size || (b->size&255) || b->size>65536 || b->valid>2 || b->flags>3 ||
       b->location<b->offset || b->location>UINT64_MAX-b->size ||
       b->offset>b->resource_bytes || b->size>b->resource_bytes-b->offset || !b->scope || !b->list ||
       boot_light88_offset(bank)>b->size || boot_light88_bytes(bank)>b->size-boot_light88_offset(bank)) return 0;
    if(b->mode==BOOT_CBV_TAP_DIRECT_REGION) {
        if(b->valid!=2 || b->owned || b->owned_bytes) return 0;
    } else {
        for(unsigned j=0;j<BOOT_L88_BANKS;++j) if((r->bindings&(1u<<j)) && r->binding[j].mode!=BOOT_CBV_TAP_DIRECT_REGION &&
            (r->binding[j].resource!=b->resource || r->binding[j].resource_bytes!=b->resource_bytes ||
             r->binding[j].location<r->binding[j].offset || b->location<b->offset ||
             r->binding[j].location-r->binding[j].offset!=b->location-b->offset)) return 0;
        if(b->mode<BOOT_CBV_TAP_RING_FRESH || b->mode>BOOT_CBV_TAP_RING_PATCHED ||
           !boot_cbv_tap_copy_owned(b,boot_light88_offset(bank),boot_light88_bytes(bank),r->payload+boot_light88_payload_at(bank))) return 0;
        r->payload_mask|=1u<<bank;
    }
    r->binding[bank]=*b; r->binding[bank].owned=NULL; r->binding[bank].owned_bytes=0;
    r->bindings|=1u<<bank; return 1;
}
static inline int boot_light88_table(BootLight88Record *r,const BootCbvTapTable *t,const BootCbvTapCv actual[19]) {
    if(t->stage!=1 || t->group!=4) return 1;
    if(r->table_seen || r->resolver_closed || r->bindings!=7 || t->token!=r->begin.token || t->renderer!=r->begin.renderer || t->draw!=r->begin.draw || t->reused>1 || t->base>UINT32_MAX-18) return 0;
    for(unsigned j=0;j<BOOT_L88_BANKS;++j) {
        const BootCbvTapBinding *b=&r->binding[j];
        if(actual[b->slot].location!=b->location || actual[b->slot].size!=b->size || t->scope!=b->scope || t->list!=b->list || t->map_generation!=b->map_generation) return 0;
    }
    r->table=*t; r->table_seen=1; return 1;
}
#endif
