#ifndef TOTK_FLOAT32_H
#define TOTK_FLOAT32_H
#include <stdint.h>
/* Binary32 addition, round-to-nearest ties-to-even, gradual underflow.
 * Integer arithmetic deliberately avoids the host floating-point environment.
 * NaNs are handled by the caller until guest payload semantics are supported. */
static inline uint64_t boot_shift_jam(uint64_t x,unsigned shift){
    if(!shift)return x;
    if(shift>=64)return x!=0;
    return (x>>shift)|((x<<(64-shift))!=0);
}
static inline uint32_t boot_add_finite32(uint32_t a,uint32_t b,uint32_t *flags){
    unsigned sign=a>>31;
    uint32_t am=a&0x7fffffff,bm=b&0x7fffffff;
    if(am<bm){uint32_t tmp=a;a=b;b=tmp;tmp=am;am=bm;bm=tmp;sign=a>>31;}
    unsigned ae=am>>23,be=bm>>23;
    uint64_t av=am&0x7fffff,bv=bm&0x7fffff;
    if(ae)av|=0x800000;else ae=1;
    if(be)bv|=0x800000;else be=1;
    av<<=3;bv=boot_shift_jam(bv<<3,ae-be);
    uint64_t sum=((a^b)>>31)?av-bv:av+bv;
    if(!sum)return ((a&b)>>31)<<31;
    if(sum>=UINT64_C(0x8000000)){sum=boot_shift_jam(sum,1);++ae;}
    while(sum<UINT64_C(0x4000000) && ae>1){sum<<=1;--ae;}
    uint32_t mant=(uint32_t)(sum>>3),tail=(uint32_t)(sum&7);
    if(tail)*flags|=16;
    if(tail>4 || (tail==4 && (mant&1)))++mant;
    if(mant>=0x1000000){mant>>=1;++ae;}
    if(ae>=255){*flags|=20;return (sign<<31)|0x7f800000;}
    if(ae==1 && mant<0x800000){ae=0;if(tail)*flags|=8;}
    return (sign<<31)|(ae<<23)|(mant&0x7fffff);
}
#endif
