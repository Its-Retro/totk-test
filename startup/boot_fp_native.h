#ifndef TOTK_BOOT_FP_NATIVE_H
#define TOTK_BOOT_FP_NATIVE_H
/* Patch 58 (play-work/fp-work, BORROW-PLAN C1-C3): inline native FP fast paths. Included by boot_runtime.h
 * after the patch 57 FMA helpers; boot_fp_paths.h (generated) follows with the cold exact helpers and the
 * per-form wrappers the translator emits.
 *
 * Contract (unchanged from the existing native FADD/FMUL paths): guest code runs with the host MXCSR control
 * bits at the Windows default (round to nearest, all exceptions masked, FTZ=DAZ=0). Windows threads start that
 * way and the x64 ABI makes MXCSR control bits callee-saved, and nothing in the runtime changes them (patch 57's
 * helper restores its temporary setting). Host status bits are never read here.
 *
 * A fast path keeps the host IEEE result only when it is provably the Arm result AND leaves FPSR unchanged:
 *   - FPSR.IXC is already set (sticky): the Arm inexact flag adds nothing. Otherwise the cold path runs and
 *     sets IXC exactly (cumulative IXC stays exact for MRS FPSR / fetestexcept readers).
 *   - no input is a nonzero subnormal (FZ would flush it and set IDC; without FZ the cold path is still exact);
 *   - the result is finite (no NaN: no IOC/DN/payload rules; no infinity: no OFC/DZC);
 *   - the result is not tiny: Arm detects tininess before rounding and flushes (FZ) or sets UFC; x86 detects it
 *     after rounding. Add/sub: an exact sum below the minimum normal is representable, so only results that
 *     are themselves subnormal are refused. Mul/div/fma: every nonzero result <= minimum normal is refused, and a
 *     zero result only stays when a factor (dividend) is zero, which makes it exact.
 *   - FPCR.RMode is round to nearest (the non-fused forms already fault on any other mode).
 * Under these conditions FPCR.FZ and FPCR.DN cannot change the Arm result, so they are not tested.
 * Vector forms evaluate all lanes with one host operation and accept the whole instruction or none of it; the
 * cold exact helper then recomputes every lane from the unchanged source registers (aliasing-safe). */
#ifndef BOOT_FPN_TAKEN
#define BOOT_FPN_TAKEN() ((void)0)   /* test hook: counts accepted fast paths (test_fp58.py) */
#endif
#define BOOT_FPOP_ADD 0
#define BOOT_FPOP_SUB 1
#define BOOT_FPOP_ABD 2
#define BOOT_FPOP_MUL 3
#define BOOT_FPOP_NMUL 4
#define BOOT_FPOP_DIV 5
#define BOOT_FPOP_ADDP 6
#define BOOT_FPOP_SQRT 7
typedef float boot_fpn_v4f __attribute__((vector_size(16)));
typedef int32_t boot_fpn_v4i __attribute__((vector_size(16)));
typedef double boot_fpn_v2d __attribute__((vector_size(16)));
typedef int64_t boot_fpn_v2l __attribute__((vector_size(16)));
/* Nonzero subnormal encodings (either sign). */
static inline __attribute__((always_inline)) int boot_fpn_subnormal32(uint32_t x){return (uint32_t)((x&UINT32_C(0x7fffffff))-1u)<UINT32_C(0x7fffff);}
static inline __attribute__((always_inline)) int boot_fpn_subnormal64(uint64_t x){return (uint64_t)((x&UINT64_C(0x7fffffffffffffff))-1u)<UINT64_C(0xfffffffffffff);}
static inline __attribute__((always_inline)) boot_fpn_v4i boot_fpn_vsubnormal32(boot_fpn_v4i v){boot_fpn_v4i m=v&0x7fffffff;return (m>0)&(m<0x800000);}
static inline __attribute__((always_inline)) boot_fpn_v2l boot_fpn_vsubnormal64(boot_fpn_v2l v){
    boot_fpn_v2l m=v&INT64_C(0x7fffffffffffffff);return (m>0)&(m<INT64_C(0x10000000000000));}

/* Scalar binary32/binary64 ADD/SUB/MUL/NMUL/DIV/SQRT (b unused for SQRT). 1: *out is the Arm result and FPSR
 * needs no update; 0: use the exact path. `op` is a constant at every call site. */
static inline __attribute__((always_inline)) int boot_fpn32(BootContext*c,unsigned op,uint32_t a,uint32_t b,uint32_t*out){
    float x,y,z;memcpy(&x,&a,4);memcpy(&y,&b,4);
    switch(op){
    case BOOT_FPOP_ADD:z=x+y;break;
    case BOOT_FPOP_SUB:z=x-y;break;
    case BOOT_FPOP_MUL:z=x*y;break;
    case BOOT_FPOP_NMUL:z=-(x*y);break;
    case BOOT_FPOP_DIV:z=x/y;break;
    default:z=_mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(x)));break;
    }
    uint32_t r;memcpy(&r,&z,4);
    uint32_t m=r&UINT32_C(0x7fffffff),am=a&UINT32_C(0x7fffffff),bm=b&UINT32_C(0x7fffffff);
    int bad=(m>=UINT32_C(0x7f800000))|boot_fpn_subnormal32(a);
    if(op!=BOOT_FPOP_SQRT)bad|=boot_fpn_subnormal32(b);
    if(op==BOOT_FPOP_ADD || op==BOOT_FPOP_SUB)bad|=boot_fpn_subnormal32(r);
    else if(op==BOOT_FPOP_MUL || op==BOOT_FPOP_NMUL)bad|=(m<=UINT32_C(0x800000))&(am!=0)&(bm!=0);
    else if(op==BOOT_FPOP_DIV)bad|=(m<=UINT32_C(0x800000))&(am!=0);
    if(__builtin_expect(bad || !(c->fpsr&16),0))return 0;
    *out=r;BOOT_FPN_TAKEN();return 1;
}
static inline __attribute__((always_inline)) int boot_fpn64(BootContext*c,unsigned op,uint64_t a,uint64_t b,uint64_t*out){
    double x,y,z;memcpy(&x,&a,8);memcpy(&y,&b,8);
    switch(op){
    case BOOT_FPOP_ADD:z=x+y;break;
    case BOOT_FPOP_SUB:z=x-y;break;
    case BOOT_FPOP_MUL:z=x*y;break;
    case BOOT_FPOP_NMUL:z=-(x*y);break;
    case BOOT_FPOP_DIV:z=x/y;break;
    default:z=_mm_cvtsd_f64(_mm_sqrt_sd(_mm_setzero_pd(),_mm_set_sd(x)));break;
    }
    uint64_t r;memcpy(&r,&z,8);
    uint64_t m=r&UINT64_C(0x7fffffffffffffff),am=a&UINT64_C(0x7fffffffffffffff),bm=b&UINT64_C(0x7fffffffffffffff);
    int bad=(m>=UINT64_C(0x7ff0000000000000))|boot_fpn_subnormal64(a);
    if(op!=BOOT_FPOP_SQRT)bad|=boot_fpn_subnormal64(b);
    if(op==BOOT_FPOP_ADD || op==BOOT_FPOP_SUB)bad|=boot_fpn_subnormal64(r);
    else if(op==BOOT_FPOP_MUL || op==BOOT_FPOP_NMUL)bad|=(m<=UINT64_C(0x10000000000000))&(am!=0)&(bm!=0);
    else if(op==BOOT_FPOP_DIV)bad|=(m<=UINT64_C(0x10000000000000))&(am!=0);
    if(__builtin_expect(bad || !(c->fpsr&16),0))return 0;
    *out=r;BOOT_FPN_TAKEN();return 1;
}

/* C1: scalar fused multiply-add acc+a*b, one host FMA (pages are built with -mfma/-march=x86-64-v3 and
 * -ffp-contract=off). Without FMA in the target, 0 (the caller then takes patch 57's helper). */
static inline __attribute__((always_inline)) int boot_fp_fma32_inline(BootContext*c,uint32_t acc,uint32_t a,uint32_t b,uint32_t*out){
#if defined(__FMA__)
    float x,y,z;memcpy(&x,&a,4);memcpy(&y,&b,4);memcpy(&z,&acc,4);
    float f=__builtin_fmaf(x,y,z);uint32_t r;memcpy(&r,&f,4);
    uint32_t m=r&UINT32_C(0x7fffffff),am=a&UINT32_C(0x7fffffff),bm=b&UINT32_C(0x7fffffff);
    int bad=(m>=UINT32_C(0x7f800000))|boot_fpn_subnormal32(acc)|boot_fpn_subnormal32(a)|boot_fpn_subnormal32(b)|
            ((m<=UINT32_C(0x800000))&(am!=0)&(bm!=0));
    if(__builtin_expect(bad || (c->fpcr&UINT32_C(0xc00000)) || !(c->fpsr&16),0))return 0;
    *out=r;BOOT_FPN_TAKEN();return 1;
#else
    (void)c;(void)acc;(void)a;(void)b;(void)out;return 0;
#endif
}
static inline __attribute__((always_inline)) int boot_fp_fma64_inline(BootContext*c,uint64_t acc,uint64_t a,uint64_t b,uint64_t*out){
#if defined(__FMA__)
    double x,y,z;memcpy(&x,&a,8);memcpy(&y,&b,8);memcpy(&z,&acc,8);
    double f=__builtin_fma(x,y,z);uint64_t r;memcpy(&r,&f,8);
    uint64_t m=r&UINT64_C(0x7fffffffffffffff),am=a&UINT64_C(0x7fffffffffffffff),bm=b&UINT64_C(0x7fffffffffffffff);
    int bad=(m>=UINT64_C(0x7ff0000000000000))|boot_fpn_subnormal64(acc)|boot_fpn_subnormal64(a)|boot_fpn_subnormal64(b)|
            ((m<=UINT64_C(0x10000000000000))&(am!=0)&(bm!=0));
    if(__builtin_expect(bad || (c->fpcr&UINT32_C(0xc00000)) || !(c->fpsr&16),0))return 0;
    *out=r;BOOT_FPN_TAKEN();return 1;
#else
    (void)c;(void)acc;(void)a;(void)b;(void)out;return 0;
#endif
}
/* The patch 57 interface the emitted FMA sites call: 1 = *out set (FPSR updated); 0 = run the exact code.
 * Inline FMA first; when it declines (IXC still clear, special operands, directed rounding), patch 57's
 * MXCSR-isolated helper, which keeps IXC exact. */
static inline __attribute__((always_inline)) int boot_fp_fma32(BootContext*c,uint32_t acc,uint32_t a,uint32_t b,uint32_t*out){
    return boot_fp_fma32_inline(c,acc,a,b,out) || boot_fp_fma32_mxcsr(c,acc,a,b,out);
}
static inline __attribute__((always_inline)) int boot_fp_fma64(BootContext*c,uint64_t acc,uint64_t a,uint64_t b,uint64_t*out){
    return boot_fp_fma64_inline(c,acc,a,b,out) || boot_fp_fma64_mxcsr(c,acc,a,b,out);
}

/* C2: packed vector forms. lanes 2 (.2S, upper half of the destination zeroed) or 4 (.4S); e>=0 selects the
 * indexed element of m (FMUL/FMLA/FMLS by element). ADDP is FADDP (pairs of n then pairs of m). */
static inline __attribute__((always_inline)) boot_fpn_v4f boot_fpn_load4(BootContext*c,unsigned r,unsigned lanes){
    boot_fpn_v4f v={0,0,0,0};memcpy(&v,c->q[r],lanes*4);return v;
}
/* Translator batch: lane-granular fallback. With FPSR.IXC already set, a vector instruction where only some lanes
 * are refused keeps the host result of the good lanes (each is the Arm result and raises nothing new) and
 * recomputes only the refused lanes with the scalar exact helper of the same operation (boot_vfl32/64, defined
 * after boot_fp_paths.h). All-refused vectors and IXC clear still take the vector cold helper. */
#ifndef BOOT_TB_NATIVE
#define BOOT_TB_NATIVE 1
#endif
#define BOOT_TBN (BOOT_FP_NATIVE && BOOT_TB_NATIVE)
#define BOOT_FPOP_FMA 8
static inline __attribute__((noinline,cold)) void boot_vfl32(BootContext*c,unsigned op,unsigned mask,const uint32_t*x,const uint32_t*y,const uint32_t*z,uint32_t*r);
static inline __attribute__((noinline,cold)) void boot_vfl64(BootContext*c,unsigned op,unsigned mask,const uint64_t*x,const uint64_t*y,const uint64_t*z,uint64_t*r);
/* 1: keep the host lanes (fix the lanes in *mask first when it is nonzero); 0: the vector cold helper. */
static inline __attribute__((always_inline)) int boot_vfn_lanes_ok(BootContext*c,unsigned mask,unsigned lanes){
    return __builtin_expect((c->fpsr&16) && (!mask || (BOOT_TB_NATIVE && mask!=(1u<<lanes)-1)),1);
}
static inline __attribute__((always_inline)) int boot_vfn32_check(BootContext*c,boot_fpn_v4i bad,unsigned lanes){
    unsigned mask=(unsigned)_mm_movemask_ps((__m128)bad)&((1u<<lanes)-1);
    return __builtin_expect(!mask && (c->fpsr&16),1);
}
static inline __attribute__((always_inline)) int boot_vfn32(BootContext*c,unsigned op,unsigned lanes,unsigned d,unsigned n,unsigned m,int e){
    boot_fpn_v4f x=boot_fpn_load4(c,n,lanes),y;
    if(e>=0){float t;memcpy(&t,c->q[m]+4*(unsigned)e,4);y=(boot_fpn_v4f){t,t,t,t};}
    else y=boot_fpn_load4(c,m,lanes);
    if(op==BOOT_FPOP_ADDP){
        boot_fpn_v4f p,q;
        if(lanes==4){p=__builtin_shufflevector(x,y,0,2,4,6);q=__builtin_shufflevector(x,y,1,3,5,7);}
        else{p=__builtin_shufflevector(x,y,0,4,2,6);q=__builtin_shufflevector(x,y,1,5,3,7);}
        x=p;y=q;
    }
    boot_fpn_v4f z;
    switch(op){
    case BOOT_FPOP_ADD:case BOOT_FPOP_ADDP:z=x+y;break;
    case BOOT_FPOP_SUB:case BOOT_FPOP_ABD:z=x-y;break;
    case BOOT_FPOP_MUL:z=x*y;break;
    case BOOT_FPOP_DIV:z=x/y;break;
    default:z=(boot_fpn_v4f)_mm_sqrt_ps((__m128)x);break;
    }
    boot_fpn_v4i a=(boot_fpn_v4i)x,b=(boot_fpn_v4i)y,r=(boot_fpn_v4i)z;
    boot_fpn_v4i rm=r&0x7fffffff,am=a&0x7fffffff,bm=b&0x7fffffff;
    boot_fpn_v4i bad=(rm>=0x7f800000)|boot_fpn_vsubnormal32(a);
    if(op!=BOOT_FPOP_SQRT)bad|=boot_fpn_vsubnormal32(b);
    if(op==BOOT_FPOP_ADD || op==BOOT_FPOP_ADDP || op==BOOT_FPOP_SUB || op==BOOT_FPOP_ABD)bad|=(rm>0)&(rm<0x800000);
    else if(op==BOOT_FPOP_MUL)bad|=(rm<=0x800000)&(am!=0)&(bm!=0);
    else if(op==BOOT_FPOP_DIV)bad|=(rm<=0x800000)&(am!=0);
    unsigned mask=(unsigned)_mm_movemask_ps((__m128)bad)&((1u<<lanes)-1);
    if(!boot_vfn_lanes_ok(c,mask,lanes))return 0;
    if(__builtin_expect(mask!=0,0)){
        uint32_t xs[4],ys[4],rs[4];memcpy(xs,&a,16);memcpy(ys,&b,16);memcpy(rs,&r,16);
        boot_vfl32(c,op,mask,xs,ys,NULL,rs);memcpy(&r,rs,16);
    }
    (void)rm;if(op==BOOT_FPOP_ABD)r&=0x7fffffff;
    if(lanes==2)r&=(boot_fpn_v4i){-1,-1,0,0};
    memcpy(c->q[d],&r,16);BOOT_FPN_TAKEN();return 1;
}
static inline __attribute__((always_inline)) int boot_vfn64(BootContext*c,unsigned op,unsigned lanes,unsigned d,unsigned n,unsigned m,int e){
    (void)lanes;   /* .2D only */
    boot_fpn_v2d x,y;memcpy(&x,c->q[n],16);
    if(e>=0){double t;memcpy(&t,c->q[m]+8*(unsigned)e,8);y=(boot_fpn_v2d){t,t};}
    else memcpy(&y,c->q[m],16);
    if(op==BOOT_FPOP_ADDP){boot_fpn_v2d p=__builtin_shufflevector(x,y,0,2),q=__builtin_shufflevector(x,y,1,3);x=p;y=q;}
    boot_fpn_v2d z;
    switch(op){
    case BOOT_FPOP_ADD:case BOOT_FPOP_ADDP:z=x+y;break;
    case BOOT_FPOP_SUB:case BOOT_FPOP_ABD:z=x-y;break;
    case BOOT_FPOP_MUL:z=x*y;break;
    case BOOT_FPOP_DIV:z=x/y;break;
    default:z=(boot_fpn_v2d)_mm_sqrt_pd((__m128d)x);break;
    }
    boot_fpn_v2l a=(boot_fpn_v2l)x,b=(boot_fpn_v2l)y,r=(boot_fpn_v2l)z;
    boot_fpn_v2l rm=r&INT64_C(0x7fffffffffffffff),am=a&INT64_C(0x7fffffffffffffff),bm=b&INT64_C(0x7fffffffffffffff);
    boot_fpn_v2l bad=(rm>=INT64_C(0x7ff0000000000000))|boot_fpn_vsubnormal64(a);
    if(op!=BOOT_FPOP_SQRT)bad|=boot_fpn_vsubnormal64(b);
    if(op==BOOT_FPOP_ADD || op==BOOT_FPOP_ADDP || op==BOOT_FPOP_SUB || op==BOOT_FPOP_ABD)bad|=(rm>0)&(rm<INT64_C(0x10000000000000));
    else if(op==BOOT_FPOP_MUL)bad|=(rm<=INT64_C(0x10000000000000))&(am!=0)&(bm!=0);
    else if(op==BOOT_FPOP_DIV)bad|=(rm<=INT64_C(0x10000000000000))&(am!=0);
    unsigned mask=(unsigned)_mm_movemask_pd((__m128d)bad);
    if(!boot_vfn_lanes_ok(c,mask,2))return 0;
    if(__builtin_expect(mask!=0,0)){
        uint64_t xs[2],ys[2],rs[2];memcpy(xs,&a,16);memcpy(ys,&b,16);memcpy(rs,&r,16);
        boot_vfl64(c,op,mask,xs,ys,NULL,rs);memcpy(&r,rs,16);
    }
    (void)rm;if(op==BOOT_FPOP_ABD)r&=INT64_C(0x7fffffffffffffff);
    memcpy(c->q[d],&r,16);BOOT_FPN_TAKEN();return 1;
}
/* FMLA (neg=0) / FMLS (neg=1): d = d + (+-n)*m, one packed host FMA. */
static inline __attribute__((always_inline)) int boot_vfn32_fma(BootContext*c,unsigned neg,unsigned lanes,unsigned d,unsigned n,unsigned m,int e){
#if defined(__FMA__)
    if(c->fpcr&UINT32_C(0xc00000))return 0;
    boot_fpn_v4f x=boot_fpn_load4(c,n,lanes),y,z=boot_fpn_load4(c,d,lanes);
    if(e>=0){float t;memcpy(&t,c->q[m]+4*(unsigned)e,4);y=(boot_fpn_v4f){t,t,t,t};}
    else y=boot_fpn_load4(c,m,lanes);
    if(neg)x=-x;
    boot_fpn_v4i r=(boot_fpn_v4i)__builtin_elementwise_fma(x,y,z);
    boot_fpn_v4i a=(boot_fpn_v4i)x,b=(boot_fpn_v4i)y,s=(boot_fpn_v4i)z;
    boot_fpn_v4i rm=r&0x7fffffff,am=a&0x7fffffff,bm=b&0x7fffffff;
    boot_fpn_v4i bad=(rm>=0x7f800000)|boot_fpn_vsubnormal32(a)|boot_fpn_vsubnormal32(b)|boot_fpn_vsubnormal32(s)|
                     ((rm<=0x800000)&(am!=0)&(bm!=0));
    unsigned mask=(unsigned)_mm_movemask_ps((__m128)bad)&((1u<<lanes)-1);
    if(!boot_vfn_lanes_ok(c,mask,lanes))return 0;
    if(__builtin_expect(mask!=0,0)){
        uint32_t xs[4],ys[4],zs[4],rs[4];memcpy(xs,&a,16);memcpy(ys,&b,16);memcpy(zs,&s,16);memcpy(rs,&r,16);
        boot_vfl32(c,BOOT_FPOP_FMA,mask,xs,ys,zs,rs);memcpy(&r,rs,16);
    }
    if(lanes==2)r&=(boot_fpn_v4i){-1,-1,0,0};
    memcpy(c->q[d],&r,16);BOOT_FPN_TAKEN();return 1;
#else
    (void)c;(void)neg;(void)lanes;(void)d;(void)n;(void)m;(void)e;return 0;
#endif
}
static inline __attribute__((always_inline)) int boot_vfn64_fma(BootContext*c,unsigned neg,unsigned lanes,unsigned d,unsigned n,unsigned m,int e){
    (void)lanes;
#if defined(__FMA__)
    if(c->fpcr&UINT32_C(0xc00000))return 0;
    boot_fpn_v2d x,y,z;memcpy(&x,c->q[n],16);memcpy(&z,c->q[d],16);
    if(e>=0){double t;memcpy(&t,c->q[m]+8*(unsigned)e,8);y=(boot_fpn_v2d){t,t};}
    else memcpy(&y,c->q[m],16);
    if(neg)x=-x;
    boot_fpn_v2l r=(boot_fpn_v2l)__builtin_elementwise_fma(x,y,z);
    boot_fpn_v2l a=(boot_fpn_v2l)x,b=(boot_fpn_v2l)y,s=(boot_fpn_v2l)z;
    boot_fpn_v2l rm=r&INT64_C(0x7fffffffffffffff),am=a&INT64_C(0x7fffffffffffffff),bm=b&INT64_C(0x7fffffffffffffff);
    boot_fpn_v2l bad=(rm>=INT64_C(0x7ff0000000000000))|boot_fpn_vsubnormal64(a)|boot_fpn_vsubnormal64(b)|boot_fpn_vsubnormal64(s)|
                     ((rm<=INT64_C(0x10000000000000))&(am!=0)&(bm!=0));
    unsigned mask=(unsigned)_mm_movemask_pd((__m128d)bad);
    if(!boot_vfn_lanes_ok(c,mask,2))return 0;
    if(__builtin_expect(mask!=0,0)){
        uint64_t xs[2],ys[2],zs[2],rs[2];memcpy(xs,&a,16);memcpy(ys,&b,16);memcpy(zs,&s,16);memcpy(rs,&r,16);
        boot_vfl64(c,BOOT_FPOP_FMA,mask,xs,ys,zs,rs);memcpy(&r,rs,16);
    }
    memcpy(c->q[d],&r,16);BOOT_FPN_TAKEN();return 1;
#else
    (void)c;(void)neg;(void)d;(void)n;(void)m;(void)e;return 0;
#endif
}

/* Translator batch (play-work/translator-batch/PLAN-RUN.md): more inline native fast paths, same contract as above
 * (host MXCSR control bits at the Windows default; a fast path keeps the host result only when it is provably the
 * Arm result and FPSR is updated exactly; otherwise the exact code the translator emits next to the call runs).
 * BOOT_TBN gates the call sites the translator emits; -DBOOT_TB_NATIVE=0 (page recompile, no regeneration) turns
 * every batch fast path off, including the lane-granular vector fallback above. */

/* FCMP/FCCMP S/D: host UCOMISS/UCOMISD gives the Arm NZCV for ordered operands (+0 == -0; with DAZ off subnormals
 * compare by value, which is the Arm result unless FPCR.FZ flushes them). An ordered FCMP sets no FPSR flag; an
 * unordered one (IOC for a signalling NaN) and FZ with a subnormal input (IDC) take the exact code. */
static inline __attribute__((always_inline)) int boot_fpn_cmp32(BootContext*c,uint32_t a,uint32_t b){
    if(__builtin_expect((c->fpcr&0x1000000) && (boot_fpn_subnormal32(a)|boot_fpn_subnormal32(b)),0))return 0;
    float x,y;memcpy(&x,&a,4);memcpy(&y,&b,4);
    if(__builtin_expect(__builtin_isunordered(x,y),0))return 0;
    int lt=x<y;c->n=(uint32_t)lt;c->z=(uint32_t)(x==y);c->carry=(uint32_t)!lt;c->v=0;BOOT_FPN_TAKEN();return 1;
}
static inline __attribute__((always_inline)) int boot_fpn_cmp64(BootContext*c,uint64_t a,uint64_t b){
    if(__builtin_expect((c->fpcr&0x1000000) && (boot_fpn_subnormal64(a)|boot_fpn_subnormal64(b)),0))return 0;
    double x,y;memcpy(&x,&a,8);memcpy(&y,&b,8);
    if(__builtin_expect(__builtin_isunordered(x,y),0))return 0;
    int lt=x<y;c->n=(uint32_t)lt;c->z=(uint32_t)(x==y);c->carry=(uint32_t)!lt;c->v=0;BOOT_FPN_TAKEN();return 1;
}

/* FCMEQ/FCMGE/FCMGT, register or #0.0 (m<0), .2S/.4S/.2D. cmp: 0 EQ, 1 GE, 2 GT. Host packed compare when no lane
 * holds a NaN (FCMGE/FCMGT set IOC for any NaN, FCMEQ for a signalling one) and FZ cannot flush an input; ordered
 * compares then raise nothing and give all ones / zero per lane. Upper half zeroed for .2S. */
static inline __attribute__((always_inline)) int boot_vfn_cmp32(BootContext*c,unsigned cmp,unsigned lanes,unsigned d,unsigned n,int m){
    boot_fpn_v4f x=boot_fpn_load4(c,n,lanes),y=m>=0?boot_fpn_load4(c,(unsigned)m,lanes):(boot_fpn_v4f){0,0,0,0};
    boot_fpn_v4i a=(boot_fpn_v4i)x,b=(boot_fpn_v4i)y;
    boot_fpn_v4i bad=((a&0x7fffffff)>0x7f800000)|((b&0x7fffffff)>0x7f800000);
    if(c->fpcr&0x1000000)bad|=boot_fpn_vsubnormal32(a)|boot_fpn_vsubnormal32(b);
    if(__builtin_expect(_mm_movemask_ps((__m128)bad)!=0,0))return 0;
    boot_fpn_v4i r=cmp==0?(boot_fpn_v4i)(x==y):cmp==1?(boot_fpn_v4i)(x>=y):(boot_fpn_v4i)(x>y);
    if(lanes==2)r&=(boot_fpn_v4i){-1,-1,0,0};
    memcpy(c->q[d],&r,16);BOOT_FPN_TAKEN();return 1;
}
static inline __attribute__((always_inline)) int boot_vfn_cmp64(BootContext*c,unsigned cmp,unsigned lanes,unsigned d,unsigned n,int m){
    (void)lanes;   /* .2D only */
    boot_fpn_v2d x,y={0,0};memcpy(&x,c->q[n],16);if(m>=0)memcpy(&y,c->q[m],16);
    boot_fpn_v2l a=(boot_fpn_v2l)x,b=(boot_fpn_v2l)y;
    boot_fpn_v2l bad=((a&INT64_C(0x7fffffffffffffff))>INT64_C(0x7ff0000000000000))|((b&INT64_C(0x7fffffffffffffff))>INT64_C(0x7ff0000000000000));
    if(c->fpcr&0x1000000)bad|=boot_fpn_vsubnormal64(a)|boot_fpn_vsubnormal64(b);
    if(__builtin_expect(_mm_movemask_pd((__m128d)bad)!=0,0))return 0;
    boot_fpn_v2l r=cmp==0?(boot_fpn_v2l)(x==y):cmp==1?(boot_fpn_v2l)(x>=y):(boot_fpn_v2l)(x>y);
    memcpy(c->q[d],&r,16);BOOT_FPN_TAKEN();return 1;
}

/* FRSQRTS (half=1: (3-a*b)/2) and FRECPS (half=0: 2-a*b), fused with one rounding: one host FMA, then an exact
 * halving (the FMA result is finite and normal there, so halving commutes with the rounding). Kept when FPSR.IXC is
 * already set, FPCR.RMode is nearest, no input is NaN/infinite/subnormal and the result is finite and not tiny.
 * Zero inputs give exactly 1.5 / 2.0 (the Arm special result); exact cancellation gives +0 like Arm's nearest
 * mode. lanes 1 (scalar; rest of the register zeroed), 2 or 4. */
static inline __attribute__((always_inline)) int boot_vfn_step32(BootContext*c,unsigned half,unsigned lanes,unsigned d,unsigned n,unsigned m){
#if defined(__FMA__)
    if((c->fpcr&UINT32_C(0xc00000)) || !(c->fpsr&16))return 0;
    boot_fpn_v4f x=boot_fpn_load4(c,n,lanes),y=boot_fpn_load4(c,m,lanes);
    boot_fpn_v4f z=__builtin_elementwise_fma(-x,y,half?(boot_fpn_v4f){3,3,3,3}:(boot_fpn_v4f){2,2,2,2});
    if(half)z=z*(boot_fpn_v4f){0.5f,0.5f,0.5f,0.5f};
    boot_fpn_v4i a=(boot_fpn_v4i)x,b=(boot_fpn_v4i)y,r=(boot_fpn_v4i)z,rm=r&0x7fffffff;
    boot_fpn_v4i bad=((a&0x7fffffff)>=0x7f800000)|((b&0x7fffffff)>=0x7f800000)|boot_fpn_vsubnormal32(a)|boot_fpn_vsubnormal32(b)|
                     (rm>=0x7f800000)|((rm!=0)&(rm<=0x800000));
    if(__builtin_expect(((unsigned)_mm_movemask_ps((__m128)bad)&((1u<<lanes)-1))!=0,0))return 0;
    r&=lanes==4?(boot_fpn_v4i){-1,-1,-1,-1}:lanes==2?(boot_fpn_v4i){-1,-1,0,0}:(boot_fpn_v4i){-1,0,0,0};
    memcpy(c->q[d],&r,16);BOOT_FPN_TAKEN();return 1;
#else
    (void)c;(void)half;(void)lanes;(void)d;(void)n;(void)m;return 0;
#endif
}
static inline __attribute__((always_inline)) int boot_vfn_step64(BootContext*c,unsigned half,unsigned lanes,unsigned d,unsigned n,unsigned m){
#if defined(__FMA__)
    if((c->fpcr&UINT32_C(0xc00000)) || !(c->fpsr&16))return 0;
    boot_fpn_v2d x={0,0},y={0,0};memcpy(&x,c->q[n],lanes*8);memcpy(&y,c->q[m],lanes*8);
    boot_fpn_v2d z=__builtin_elementwise_fma(-x,y,half?(boot_fpn_v2d){3,3}:(boot_fpn_v2d){2,2});
    if(half)z=z*(boot_fpn_v2d){0.5,0.5};
    boot_fpn_v2l a=(boot_fpn_v2l)x,b=(boot_fpn_v2l)y,r=(boot_fpn_v2l)z,rm=r&INT64_C(0x7fffffffffffffff);
    boot_fpn_v2l bad=((a&INT64_C(0x7fffffffffffffff))>=INT64_C(0x7ff0000000000000))|((b&INT64_C(0x7fffffffffffffff))>=INT64_C(0x7ff0000000000000))|
                     boot_fpn_vsubnormal64(a)|boot_fpn_vsubnormal64(b)|(rm>=INT64_C(0x7ff0000000000000))|((rm!=0)&(rm<=INT64_C(0x10000000000000)));
    if(__builtin_expect(((unsigned)_mm_movemask_pd((__m128d)bad)&((1u<<lanes)-1))!=0,0))return 0;
    if(lanes==1)r&=(boot_fpn_v2l){-1,0};
    memcpy(c->q[d],&r,16);BOOT_FPN_TAKEN();return 1;
#else
    (void)c;(void)half;(void)lanes;(void)d;(void)n;(void)m;return 0;
#endif
}

/* Integer -> FP (SCVTF/UCVTF, no fixed-point scale) under round to nearest: the host conversion is the correctly
 * rounded Arm result (an integer never gives a subnormal, NaN or infinity, so FZ/DN do not matter); IXC is set
 * exactly: a value is inexact iff it has more significant bits than the format's precision p (24 or 53) and any
 * of the bits below the top p is set. */
static inline __attribute__((always_inline)) int boot_fpn_int_inexact(uint64_t u,unsigned p){
    if(!u)return 0;
    unsigned bits=64u-(unsigned)__builtin_clzll(u);
    return bits>p && (u&((UINT64_C(1)<<(bits-p))-1))!=0;
}
/* One value: iw source bits (32/64, sgn: two's complement), fw result bits (32/64). Returns the result bits. */
static inline __attribute__((always_inline)) uint64_t boot_fpn_cvtf(BootContext*c,unsigned sgn,unsigned iw,unsigned fw,uint64_t v){
    if(iw==32)v=sgn?(uint64_t)(int64_t)(int32_t)(uint32_t)v:(uint64_t)(uint32_t)v;
    int negative=sgn && (int64_t)v<0;uint64_t u=negative?UINT64_C(0)-v:v;
    if(boot_fpn_int_inexact(u,fw==32?24:53))c->fpsr|=16;
    if(fw==32){float f=sgn?(float)(int64_t)v:(float)v;uint32_t b;memcpy(&b,&f,4);return b;}
    double f=sgn?(double)(int64_t)v:(double)v;uint64_t b;memcpy(&b,&f,8);return b;
}
/* Vector form: lanes 2 or 4 of 32-bit integers (result .2S/.4S, upper half zeroed for .2S) or 2 of 64-bit (.2D).
 * Signed .2S/.4S use CVTDQ2PS and its exact round trip (CVTTPS2DQ gives the same integer iff the conversion was
 * exact; 2^31 converts back to 0x80000000, which is never the source of a rounded-up 2^31). */
static inline __attribute__((always_inline)) int boot_vcvtf(BootContext*c,unsigned sgn,unsigned w,unsigned lanes,unsigned d,unsigned n){
    if(c->fpcr&UINT32_C(0xc00000))return 0;
    if(w==32 && sgn){
        uint8_t in[16]={0};memcpy(in,c->q[n],lanes*4);
        __m128i a=_mm_loadu_si128((const __m128i*)in);__m128 f=_mm_cvtepi32_ps(a);
        if(_mm_movemask_epi8(_mm_cmpeq_epi32(_mm_cvttps_epi32(f),a))!=0xffff)c->fpsr|=16;
        __m128i r=_mm_castps_si128(f);if(lanes==2)r=_mm_move_epi64(r);
        _mm_storeu_si128((__m128i*)c->q[d],r);BOOT_FPN_TAKEN();return 1;
    }
    uint64_t r[2]={0,0};uint8_t out[16];
    if(w==32){
        uint32_t in[4]={0,0,0,0},o[4]={0,0,0,0};memcpy(in,c->q[n],lanes*4);
        for(unsigned lane=0;lane<lanes;++lane)o[lane]=(uint32_t)boot_fpn_cvtf(c,sgn,32,32,in[lane]);
        memcpy(out,o,16);
    }else{
        uint64_t in[2];memcpy(in,c->q[n],16);
        for(unsigned lane=0;lane<2;++lane)r[lane]=boot_fpn_cvtf(c,sgn,64,64,in[lane]);
        memcpy(out,r,16);
    }
    memcpy(c->q[d],out,16);BOOT_FPN_TAKEN();return 1;
}

/* FCVTZS/FCVTZU (round toward zero, no fixed-point scale) to w = 32/64 integer bits from a binary32/binary64 input:
 * the host truncating conversion when the input is zero or normal and its truncation fits (signed |x| < 2^(w-1),
 * unsigned 0 <= x < 2^w; -0 gives 0). Then no saturation (IOC) or FZ flush (IDC) can happen, and IXC is set exactly:
 * the truncation converted back is exact (an integer below 2^p, or the input itself when it is >= 2^p), so it
 * differs from the input iff the input had a fraction. *out: the result zero-extended from w bits like the exact
 * code's (uint64_t) two's complement value truncated by the caller. */
static inline __attribute__((always_inline)) int boot_fpn_cvtz32(BootContext*c,unsigned sgn,unsigned w,uint32_t a,uint64_t*out){
    uint32_t m=a&UINT32_C(0x7fffffff);
    uint32_t limit=sgn?(w==32?UINT32_C(0x4f000000):UINT32_C(0x5f000000)):(w==32?UINT32_C(0x4f800000):UINT32_C(0x5f800000));
    if(__builtin_expect((m!=0 && m<UINT32_C(0x800000)) || m>=limit || (!sgn && m && (a>>31)),0))return 0;
    float x;memcpy(&x,&a,4);uint64_t r;float back;
    if(sgn){if(w==32){int32_t t=(int32_t)x;back=(float)t;r=(uint64_t)(int64_t)t;}else{int64_t t=(int64_t)x;back=(float)t;r=(uint64_t)t;}}
    else{if(w==32){uint32_t t=(uint32_t)x;back=(float)t;r=t;}else{uint64_t t=(uint64_t)x;back=(float)t;r=t;}}
    if(back!=x)c->fpsr|=16;
    *out=r;BOOT_FPN_TAKEN();return 1;
}
static inline __attribute__((always_inline)) int boot_fpn_cvtz64(BootContext*c,unsigned sgn,unsigned w,uint64_t a,uint64_t*out){
    uint64_t m=a&UINT64_C(0x7fffffffffffffff);
    uint64_t limit=sgn?(w==32?UINT64_C(0x41e0000000000000):UINT64_C(0x43e0000000000000)):(w==32?UINT64_C(0x41f0000000000000):UINT64_C(0x43f0000000000000));
    if(__builtin_expect((m!=0 && m<UINT64_C(0x10000000000000)) || m>=limit || (!sgn && m && (a>>63)),0))return 0;
    double x;memcpy(&x,&a,8);uint64_t r;double back;
    if(sgn){if(w==32){int32_t t=(int32_t)x;back=(double)t;r=(uint64_t)(int64_t)t;}else{int64_t t=(int64_t)x;back=(double)t;r=(uint64_t)t;}}
    else{if(w==32){uint32_t t=(uint32_t)x;back=(double)t;r=t;}else{uint64_t t=(uint64_t)x;back=(double)t;r=t;}}
    if(back!=x)c->fpsr|=16;
    *out=r;BOOT_FPN_TAKEN();return 1;
}

/* Whole-register writes. boot_q16 stores 16 bytes to a guest register as one store, except where that store would
 * split a 64-byte cache line: the hot game threads' contexts are 64-byte aligned (BootCore, _aligned_malloc 64) and
 * c->q starts 52 bytes into a line, so q0, q4, ..., q28 straddle lines; those get two stores that do not (a split
 * store costs more than the partial stores it replaces). lineoff = the register's offset in its line assuming a
 * 64-aligned context (BOOT_QLINE: constant after inlining); any alignment is correct, only the store shape changes.
 * boot_qz: a scalar result or short load zero-extended to the register (one MOVD/MOVQ + store), not a zeroing
 * store plus a partial one, so a later 16-byte read of the register can forward from a single store. size 1/2/4/8
 * bytes of v (the rest of v is ignored). BOOT_QZ refuses floating-point values at compile time. */
#define BOOT_QLINE(q) ((unsigned)(((uintptr_t)(q)-(uintptr_t)c)&63))
static inline __attribute__((always_inline)) void boot_q16(uint8_t*q,const void*v,unsigned lineoff){
    if(lineoff>48){unsigned a=64-lineoff;memcpy(q,v,a);memcpy(q+a,(const uint8_t*)v+a,16-a);}
    else memcpy(q,v,16);
}
static inline __attribute__((always_inline)) void boot_qz(uint8_t*q,uint64_t v,unsigned size,unsigned lineoff){
    typedef uint64_t boot_qz_v_ __attribute__((vector_size(16)));
    if(size<8)v&=(UINT64_C(1)<<(size*8))-1;
    boot_qz_v_ x={v,0};boot_q16(q,&x,lineoff);
}
#define BOOT_QZ(q,v,size) boot_qz((q),_Generic((v),float:(void)0,double:(void)0,default:(v)),(size),BOOT_QLINE(q))
static inline __attribute__((always_inline)) void boot_qzp(uint8_t*q,const uint8_t*p,unsigned size,unsigned lineoff){
    if(size==16){memcpy(q,p,16);return;}
    uint64_t v=0;memcpy(&v,p,size);boot_qz(q,v,size,lineoff);
}
#define BOOT_QZP(q,p,size) boot_qzp((q),(p),(size),BOOT_QLINE(q))

/* Pages built for x86-64-v3 (compile_cached_fast.py: PAGE_ARCH_FLAGS) need AVX2/FMA/BMI1/BMI2/LZCNT/MOVBE/F16C.
 * runtime_support is built for the baseline target with -DBOOT_REQUIRE_X86_64_V3 and checks at DLL load, before
 * any page or dispatch code can run, so a host without them gets a clear message instead of an illegal
 * instruction somewhere in guest code. */
#if defined(BOOT_REQUIRE_X86_64_V3)
static void __attribute__((constructor)) boot_fp_require_host_isa(void){
    uint32_t a,b,cx,dx,ok=1;
    a=1;cx=0;__asm__ volatile("cpuid":"+a"(a),"=b"(b),"+c"(cx),"=d"(dx));
    /* SSE3 SSSE3 FMA SSE4.1 SSE4.2 MOVBE POPCNT OSXSAVE AVX F16C */
    const uint32_t need1=(1u<<0)|(1u<<9)|(1u<<12)|(1u<<19)|(1u<<20)|(1u<<22)|(1u<<23)|(1u<<27)|(1u<<28)|(1u<<29);
    ok&=(cx&need1)==need1;
    if(ok){uint32_t lo,hi;__asm__ volatile("xgetbv":"=a"(lo),"=d"(hi):"c"(0));ok&=(lo&6)==6;}
    a=7;cx=0;__asm__ volatile("cpuid":"+a"(a),"=b"(b),"+c"(cx),"=d"(dx));
    ok&=(b&((1u<<3)|(1u<<5)|(1u<<8)))==((1u<<3)|(1u<<5)|(1u<<8));   /* BMI1 AVX2 BMI2 */
    a=0x80000001;cx=0;__asm__ volatile("cpuid":"+a"(a),"=b"(b),"+c"(cx),"=d"(dx));
    ok&=(cx>>5)&1;   /* LZCNT */
    if(ok)return;
    static const char message[]="TOTK native: this build needs an x86-64-v3 CPU (AVX2, FMA3, BMI1/BMI2, LZCNT, MOVBE, F16C; "
        "Intel Haswell / AMD Excavator or newer) with AVX enabled by the OS. Rebuild the pages without "
        "PAGE_ARCH_FLAGS (compile_cached_fast.py) for an older CPU.\n";
    fputs(message,stderr);fflush(stderr);OutputDebugStringA(message);
    MessageBoxA(NULL,message,"TOTK native",MB_OK|MB_ICONERROR|MB_SYSTEMMODAL);
    TerminateProcess(GetCurrentProcess(),0xC000001Du);   /* STATUS_ILLEGAL_INSTRUCTION */
}
#endif
#include "boot_fp_paths.h"
/* Translator batch: lane-granular vector fallback, the cold half (see boot_vfn32). For every lane in mask, the
 * scalar exact helper of the same operation on the lane's inputs (x, y; z = FMLA/FMLS accumulator, x already
 * negated for FMLS), as the vector cold helpers compute each lane: Arm applies the scalar FP operation per element
 * with the same FPCR, and its FPSR flags are cumulative ORs, so lane order does not matter. FMA lanes take patch
 * 57's MXCSR-isolated FMA, then the exact code (boot_fps_*), like the vector cold helper of the native build. */
static inline __attribute__((noinline,cold)) void boot_vfl32(BootContext*c,unsigned op,unsigned mask,const uint32_t*x,const uint32_t*y,const uint32_t*z,uint32_t*r){
    for(unsigned i=0;i<4;++i){
        if(!((mask>>i)&1))continue;
        switch(op){
        case BOOT_FPOP_ADD:case BOOT_FPOP_ADDP:r[i]=boot_fpx_fadd_s(c,x[i],y[i]);break;
        case BOOT_FPOP_SUB:case BOOT_FPOP_ABD:r[i]=boot_fpx_fsub_s(c,x[i],y[i]);break;
        case BOOT_FPOP_MUL:r[i]=boot_fpx_fmul_s(c,x[i],y[i]);break;
        case BOOT_FPOP_DIV:r[i]=boot_fpx_fdiv_s(c,x[i],y[i]);break;
        case BOOT_FPOP_SQRT:r[i]=boot_fpx_fsqrt_s(c,x[i]);break;
        default:r[i]=boot_fps_fma_s(c,z[i],x[i],y[i]);break;
        }
    }
}
static inline __attribute__((noinline,cold)) void boot_vfl64(BootContext*c,unsigned op,unsigned mask,const uint64_t*x,const uint64_t*y,const uint64_t*z,uint64_t*r){
    for(unsigned i=0;i<2;++i){
        if(!((mask>>i)&1))continue;
        switch(op){
        case BOOT_FPOP_ADD:case BOOT_FPOP_ADDP:r[i]=boot_fpx_fadd_d(c,x[i],y[i]);break;
        case BOOT_FPOP_SUB:case BOOT_FPOP_ABD:r[i]=boot_fpx_fsub_d(c,x[i],y[i]);break;
        case BOOT_FPOP_MUL:r[i]=boot_fpx_fmul_d(c,x[i],y[i]);break;
        case BOOT_FPOP_DIV:r[i]=boot_fpx_fdiv_d(c,x[i],y[i]);break;
        case BOOT_FPOP_SQRT:r[i]=boot_fpx_fsqrt_d(c,x[i]);break;
        default:r[i]=boot_fps_fma_d(c,z[i],x[i],y[i]);break;
        }
    }
}
#endif
