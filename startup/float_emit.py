"""Instruction-local integer implementations; no host FP environment dependency."""

def round_integral(width,op):
    """Scalar FRINT rounding (Arm FPRoundInt). The non-X forms do not raise inexact;
    FRINTX uses the FPCR rounding mode like FRINTI and raises IXC when the result differs."""
    fraction,bias=(23,127) if width==32 else (52,1023)
    sign=1<<(width-1);inf=(sign-1)^((1<<fraction)-1);quiet=1<<(fraction-1)
    exact=op=='frintx'
    if op in ('frinti','frintx'):
        small=f'(mode==0 ? mag>UINT64_C({(bias-1)<<fraction}) : mode==1 ? !negative : mode==2 ? negative : 0)'
        increment='(mode==0 ? (remainder>half || (remainder==half && ((mag>>shift)&1))) : mode==1 ? (remainder && !negative) : mode==2 ? (remainder && negative) : 0)'
    else:
        small={'frinta':f'mag>=UINT64_C({(bias-1)<<fraction})',
               'frintn':f'mag>UINT64_C({(bias-1)<<fraction})',
               'frintm':'negative','frintp':'!negative','frintz':'0'}[op]
        increment={'frinta':'remainder>=half','frintn':'remainder>half || (remainder==half && ((mag>>shift)&1))',
                   'frintm':'remainder && negative','frintp':'remainder && !negative','frintz':'0'}[op]
    mode_decl='unsigned mode=(c->fpcr>>22)&3;' if op in ('frinti','frintx') else ''
    ixc_small='c->fpsr|=16;' if exact else ''
    open_,close_=('{','}') if exact else ('','')
    ixc_part='if(remainder)c->fpsr|=16;' if exact else ''
    return f'''
uint64_t mag=a&UINT64_C({sign-1}),negative=a&UINT64_C({sign});{mode_decl}r=a;
if(mag>UINT64_C({inf})){{
    if(!(a&UINT64_C({quiet})))c->fpsr|=1;
    r=(c->fpcr&0x2000000)?UINT64_C({inf|quiet}):(a|UINT64_C({quiet}));
}}else if(mag && mag<UINT64_C({inf})){{
    if((c->fpcr&0x1000000) && mag<UINT64_C({1<<fraction})){{r=negative;c->fpsr|=128;}}
    else {{int exponent=(int)(mag>>{fraction})-{bias};
        if(exponent<0){open_}r=negative|(({small})?UINT64_C({bias<<fraction}):0);{ixc_small}{close_}
        else if(exponent<{fraction}){{
            unsigned shift={fraction}-exponent;uint64_t unit=UINT64_C(1)<<shift,mask=unit-1;
            uint64_t remainder=mag&mask,half=unit>>1;(void)remainder;(void)half;{ixc_part}
            uint64_t rounded=mag&~mask;if({increment})rounded+=unit;r=negative|rounded;
        }}
    }}
}}
'''

def fused_multiply_add_exact(width):
    """Exact acc+a*b in integer subnormal-squared units, rounded once."""
    fraction,bias=(23,127) if width==32 else (52,1023)
    sign=1<<(width-1);inf=(2*bias+1)<<fraction;quiet=1<<(fraction-1)
    q=bias+fraction-1;limbs=(4*bias+2*fraction+2+63)//64
    return flush_inputs(width,'acc','a','b')+f'''
uint64_t values[3]={{acc,a,b}},magnitudes[3]={{acc&UINT64_C({sign-1}),a&UINT64_C({sign-1}),b&UINT64_C({sign-1})}};
unsigned mode=(c->fpcr>>22)&3;int nan=-1,snan=-1;
for(unsigned j=0;j<3;++j)if(magnitudes[j]>UINT64_C({inf})){{if(nan<0)nan=(int)j;if(!(values[j]&UINT64_C({quiet})) && snan<0)snan=(int)j;}}
int invalid_product=(magnitudes[1]==UINT64_C({inf}) && !magnitudes[2]) || (!magnitudes[1] && magnitudes[2]==UINT64_C({inf}));
if(snan>=0){{c->fpsr|=1;r=(c->fpcr&0x2000000)?UINT64_C({inf|quiet}):(values[snan]|UINT64_C({quiet}));}}
else if(invalid_product){{c->fpsr|=1;r=UINT64_C({inf|quiet});}}
else if(nan>=0)r=(c->fpcr&0x2000000)?UINT64_C({inf|quiet}):(values[nan]|UINT64_C({quiet}));
else if(magnitudes[1]==UINT64_C({inf}) || magnitudes[2]==UINT64_C({inf})){{
    if(magnitudes[0]==UINT64_C({inf}) && ((acc^a^b)&UINT64_C({sign}))){{c->fpsr|=1;r=UINT64_C({inf|quiet});}}
    else r=((a^b)&UINT64_C({sign}))|UINT64_C({inf});
}}else if(magnitudes[0]==UINT64_C({inf}))r=acc;
else {{
    uint64_t mantissas[3];unsigned powers[3];
    for(unsigned j=0;j<3;++j){{
        unsigned exponent=(unsigned)(magnitudes[j]>>{fraction});
        mantissas[j]=magnitudes[j]&UINT64_C({(1<<fraction)-1});
        if(exponent)mantissas[j]|=UINT64_C({1<<fraction});
        powers[j]=exponent?exponent-1:0;
    }}
    unsigned __int128 terms_value[2]={{mantissas[0],(unsigned __int128)mantissas[1]*mantissas[2]}};
    unsigned offsets[2]={{powers[0]+{q},powers[1]+powers[2]}};
    uint64_t terms[2][{limbs}]={{0}},sum[{limbs}]={{0}};
    for(unsigned term=0;term<2;++term)for(unsigned part=0;part<2;++part){{
        uint64_t chunk=(uint64_t)(terms_value[term]>>(part*64));
        unsigned bit=offsets[term]+part*64,word=bit/64,shift=bit%64;
        if(chunk){{terms[term][word]|=chunk<<shift;if(shift && word+1<{limbs})terms[term][word+1]|=chunk>>(64-shift);}}
    }}
    int signs[2]={{(acc&UINT64_C({sign}))!=0,((a^b)&UINT64_C({sign}))!=0}},negative=signs[0];
    if(signs[0]==signs[1]){{
        unsigned __int128 carry=0;
        for(unsigned j=0;j<{limbs};++j){{carry+=(unsigned __int128)terms[0][j]+terms[1][j];sum[j]=(uint64_t)carry;carry>>=64;}}
    }}else{{
        int compare=0;
        for(int j={limbs-1};j>=0;--j)if(terms[0][j]!=terms[1][j]){{compare=terms[0][j]>terms[1][j]?1:-1;break;}}
        unsigned larger=compare<0;negative=signs[larger];
        const uint64_t*x=terms[larger],*y=terms[1-larger];uint64_t borrow=0;
        for(unsigned j=0;j<{limbs};++j){{uint64_t sub=y[j]+borrow;int overflow=sub<y[j];sum[j]=x[j]-sub;borrow=overflow || x[j]<sub;}}
    }}
    int top=-1;
    for(int j={limbs-1};j>=0;--j)if(sum[j]){{top=j*64+63-__builtin_clzll(sum[j]);break;}}
    uint64_t output_sign=negative?UINT64_C({sign}):0;
    if(top<0)r=(signs[0]==signs[1]?signs[0]:mode==2)?UINT64_C({sign}):0;
    else {{
        int exponent=top-{2*q};
        if((c->fpcr&0x1000000) && exponent<{1-bias}){{r=output_sign;c->fpsr|=8;}}
        else {{
            int shift=top-{fraction};if(exponent<{1-bias})shift={q};
            uint64_t mant=0;int guard=0,sticky=0;
            if(shift<=0)mant=sum[0]<<(-shift);
            else {{
                unsigned word=(unsigned)shift/64,bit=(unsigned)shift%64;
                if(word<{limbs}){{mant=sum[word]>>bit;if(bit && word+1<{limbs})mant|=sum[word+1]<<(64-bit);}}
                unsigned g=(unsigned)shift-1;
                if(g/64<{limbs})guard=(int)((sum[g/64]>>(g%64))&1);
                for(unsigned j=0;j<{limbs} && j*64<g;++j){{unsigned n=g-j*64;sticky|=n>=64?sum[j]!=0:(sum[j]&((UINT64_C(1)<<n)-1))!=0;}}
            }}
            int lost=guard || sticky;
            if(lost){{c->fpsr|=16;if(exponent<{1-bias})c->fpsr|=8;}}
            if((mode==0 && guard && (sticky || (mant&1))) || (mode==1 && !negative && lost) || (mode==2 && negative && lost))++mant;
            if(exponent<{1-bias})exponent={1-bias};
            if(mant>=UINT64_C({1<<(fraction+1)})){{mant>>=1;++exponent;}}
            if(exponent>{bias}){{int infinite=mode==0 || (mode==1 && !negative) || (mode==2 && negative);r=output_sign|(infinite?UINT64_C({inf}):UINT64_C({inf-1}));c->fpsr|=20;}}
            else if(mant<UINT64_C({1<<fraction}))r=output_sign|mant;
            else r=output_sign|((uint64_t)(exponent+{bias})<<{fraction})|(mant&UINT64_C({(1<<fraction)-1}));
        }}
    }}
}}
'''

def fused_multiply_add(width):
    exact = fused_multiply_add_exact(width)
    return (f'if(!BOOT_FP_NATIVE || !boot_fp_fma{width}(c,acc,a,b,&r)){{\n'
            + exact + '\n}\n')

def reciprocal_sqrt_step(width):
    """Exact fused (3-a*b)/2 using bounded limbs; one final ARM rounding."""
    fraction,bias=(23,127) if width==32 else (52,1023)
    sign=1<<(width-1);inf=(2*bias+1)<<fraction;quiet=1<<(fraction-1)
    # Worst-case alignment is the constant 3 against two minimum subnormals.
    limbs=(2*(bias+fraction-1)+2+63)//64
    return f'''
a^=UINT64_C({sign});
''' + flush_inputs(width,'a','b') + f'''
uint64_t am=a&UINT64_C({sign-1}),bm=b&UINT64_C({sign-1});
unsigned mode=(c->fpcr>>22)&3;
int an=am>UINT64_C({inf}),bn=bm>UINT64_C({inf});
if(an || bn){{
    int as=an && !(a&UINT64_C({quiet})),bs=bn && !(b&UINT64_C({quiet}));
    if(as || bs)c->fpsr|=1;
    uint64_t nan=as?a:bs?b:an?a:b;
    r=(c->fpcr&0x2000000)?UINT64_C({inf|quiet}):(nan|UINT64_C({quiet}));
}}else if((am==UINT64_C({inf}) && !bm) || (!am && bm==UINT64_C({inf})))r=UINT64_C({(bias<<fraction)|(1<<(fraction-1))});
else if(am==UINT64_C({inf}) || bm==UINT64_C({inf}))r=((a^b)&UINT64_C({sign}))|UINT64_C({inf});
else if(!am || !bm)r=UINT64_C({(bias<<fraction)|(1<<(fraction-1))});
else {{
    int ae=(int)(am>>{fraction}),be=(int)(bm>>{fraction});
    uint64_t av=am&UINT64_C({(1<<fraction)-1}),bv=bm&UINT64_C({(1<<fraction)-1});
    if(ae)av|=UINT64_C({1<<fraction});else ae=1;
    if(be)bv|=UINT64_C({1<<fraction});else be=1;
    int power=ae+be-{2*(bias+fraction)},common=power<0?power:0;
    unsigned __int128 product=(unsigned __int128)av*bv;
    uint64_t terms[2][{limbs}]={{0}},sum[{limbs}]={{0}};
    unsigned cb=(unsigned)(-common),cw=cb/64,cs=cb%64;
    terms[0][cw]=UINT64_C(3)<<cs;
    if(cs>62 && cw+1<{limbs})terms[0][cw+1]=UINT64_C(3)>>(64-cs);
    for(unsigned part=0;part<2;++part){{
        uint64_t chunk=(uint64_t)(product>>(part*64));
        unsigned bit=(unsigned)(power-common)+part*64,word=bit/64,shift=bit%64;
        if(chunk){{terms[1][word]|=chunk<<shift;if(shift && word+1<{limbs})terms[1][word+1]|=chunk>>(64-shift);}}
    }}
    int negative=0;
    if(!((a^b)&UINT64_C({sign}))){{
        unsigned __int128 carry=0;
        for(unsigned j=0;j<{limbs};++j){{carry+=(unsigned __int128)terms[0][j]+terms[1][j];sum[j]=(uint64_t)carry;carry>>=64;}}
    }}else{{
        int compare=0;
        for(int j={limbs-1};j>=0;--j)if(terms[0][j]!=terms[1][j]){{compare=terms[0][j]>terms[1][j]?1:-1;break;}}
        negative=compare<0;
        const uint64_t*x=terms[negative?1:0],*y=terms[negative?0:1];uint64_t borrow=0;
        for(unsigned j=0;j<{limbs};++j){{uint64_t sub=y[j]+borrow;int overflow=sub<y[j];sum[j]=x[j]-sub;borrow=overflow || x[j]<sub;}}
    }}
    int top=-1;
    for(int j={limbs-1};j>=0;--j)if(sum[j]){{top=j*64+63-__builtin_clzll(sum[j]);break;}}
    uint64_t output_sign=negative?UINT64_C({sign}):0;
    if(top<0)r=mode==2?UINT64_C({sign}):0;
    else {{
        int exponent=top+common-1;
        if((c->fpcr&0x1000000) && exponent<{1-bias}){{r=output_sign;c->fpsr|=8;}}
        else {{
            int shift=top-{fraction};if(exponent<{1-bias})shift={2-bias-fraction}-common;
            uint64_t mant=0;int guard=0,sticky=0;
            if(shift<=0)mant=sum[0]<<(-shift);
            else {{
                unsigned word=(unsigned)shift/64,bit=(unsigned)shift%64;
                if(word<{limbs}){{mant=sum[word]>>bit;if(bit && word+1<{limbs})mant|=sum[word+1]<<(64-bit);}}
                unsigned g=(unsigned)shift-1;
                if(g/64<{limbs})guard=(int)((sum[g/64]>>(g%64))&1);
                for(unsigned j=0;j<{limbs} && j*64<g;++j){{unsigned n=g-j*64;sticky|=n>=64?sum[j]!=0:(sum[j]&((UINT64_C(1)<<n)-1))!=0;}}
            }}
            int lost=guard || sticky;
            if(lost){{c->fpsr|=16;if(exponent<{1-bias})c->fpsr|=8;}}
            if((mode==0 && guard && (sticky || (mant&1))) || (mode==1 && !negative && lost) || (mode==2 && negative && lost))++mant;
            if(exponent<{1-bias})exponent={1-bias};
            if(mant>=UINT64_C({1<<(fraction+1)})){{mant>>=1;++exponent;}}
            if(exponent>{bias}){{int infinite=mode==0 || (mode==1 && !negative) || (mode==2 && negative);r=output_sign|(infinite?UINT64_C({inf}):UINT64_C({inf-1}));c->fpsr|=20;}}
            else if(mant<UINT64_C({1<<fraction}))r=output_sign|mant;
            else r=output_sign|((uint64_t)(exponent+{bias})<<{fraction})|(mant&UINT64_C({(1<<fraction)-1}));
        }}
    }}
}}
'''

def reciprocal_sqrt_estimate(width):
    """ARM's original eight-bit estimate; no host floating-point approximation."""
    fraction,bias=(23,127) if width==32 else (52,1023)
    sign=1<<(width-1);inf=(2*bias+1)<<fraction;quiet=1<<(fraction-1)
    return f'''
uint64_t magnitude=a&UINT64_C({sign-1});
if((c->fpcr&0x1000000) && magnitude && magnitude<UINT64_C({1<<fraction})){{a&=UINT64_C({sign});magnitude=0;c->fpsr|=128;}}
if(magnitude>UINT64_C({inf})){{
    if(!(a&UINT64_C({quiet})))c->fpsr|=1;
    r=(c->fpcr&0x2000000)?UINT64_C({inf|quiet}):(a|UINT64_C({quiet}));
}}else if(!magnitude){{r=(a&UINT64_C({sign}))|UINT64_C({inf});c->fpsr|=2;}}
else if(a&UINT64_C({sign})){{r=UINT64_C({inf|quiet});c->fpsr|=1;}}
else if(magnitude==UINT64_C({inf}))r=0;
else {{
    int exponent=(int)(magnitude>>{fraction});uint64_t mantissa=magnitude&UINT64_C({(1<<fraction)-1});
    if(!exponent){{
        while(!(mantissa&UINT64_C({1<<(fraction-1)}))){{mantissa<<=1;--exponent;}}
        mantissa=(mantissa&UINT64_C({(1<<(fraction-1))-1}))<<1;
    }}
    unsigned bucket=(exponent%2)?(128u|(unsigned)(mantissa>>{fraction-7})):(256u|(unsigned)(mantissa>>{fraction-8}));
    unsigned estimate=boot_fp_rsqrt_table[bucket-128];
    r=((uint64_t)(({3*bias-1}-exponent)/2)<<{fraction})|((uint64_t)(estimate&255)<<{fraction-8});
}}
'''

# Finite inputs, nearest-even, optional FZ. The caller checks the profile and
# flushes subnormal inputs before these instruction-local arithmetic blocks.
MULTIPLY = r'''
uint32_t sign=(a^b)&0x80000000, am=a&0x7fffff,bm=b&0x7fffff;
int ae=(a>>23)&255,be=(b>>23)&255;
if(ae)am|=0x800000;else ae=1;
if(be)bm|=0x800000;else be=1;
uint64_t product=(uint64_t)am*bm;
r=sign;
if(product){
    int power=ae+be-300,top=63-__builtin_clzll(product);
    int exponent=top+power;
    if((c->fpcr&0x1000000) && exponent < -126)c->fpsr|=8;
    else {
    int shift=top-23;if(exponent < -126)shift=-149-power;
    uint64_t mant,rem=0,half=0;
    if(shift<=0)mant=product<<(-shift);
    else if(shift>=64){mant=0;rem=product;half=UINT64_MAX;}
    else {mant=product>>shift;rem=product&((UINT64_C(1)<<shift)-1);half=UINT64_C(1)<<(shift-1);}
    if(rem)c->fpsr|=16;
    if(exponent < -126 && rem)c->fpsr|=8;
    if(rem && (rem>half || (rem==half && (mant&1))))++mant;
    if(exponent < -126)exponent=-126;
    if(mant>=0x1000000){mant>>=1;++exponent;}
    if(exponent>127){r|=0x7f800000;c->fpsr|=20;}
    else if(mant<0x800000)r|=(uint32_t)mant;
    else r|=((uint32_t)(exponent+127)<<23)|((uint32_t)mant&0x7fffff);
    }
}
'''

SQUARE_ROOT = r'''
uint32_t magnitude=a&0x7fffffff;
r=a;
if(magnitude && magnitude!=0x7f800000){
    uint32_t mant=a&0x7fffff;int exponent=(int)(a>>23)-127;
    if(a>>23)mant|=0x800000;
    else {exponent=-126;while(!(mant&0x800000)){mant<<=1;--exponent;}}
    if(exponent%2){mant<<=1;--exponent;}
    uint64_t radicand=(uint64_t)mant<<23;
    uint32_t low=0,high=0x1000000;
    while(low+1<high){uint32_t mid=low+(high-low)/2;if((uint64_t)mid*mid<=radicand)low=mid;else high=mid;}
    uint64_t remainder=radicand-(uint64_t)low*low;
    if(remainder)c->fpsr|=16;
    if(remainder>low)++low;
    exponent/=2;
    if(low==0x1000000){low>>=1;++exponent;}
    r=((uint32_t)(exponent+127)<<23)|(low&0x7fffff);
}
'''

SQUARE_ROOT64 = r'''
uint64_t magnitude=a&UINT64_C(0x7fffffffffffffff);
r=a;
if(magnitude && magnitude!=UINT64_C(0x7ff0000000000000)){
    uint64_t mant=a&UINT64_C(0xfffffffffffff);int exponent=(int)((a>>52)&2047)-1023;
    if((a>>52)&2047)mant|=UINT64_C(0x10000000000000);
    else {exponent=-1022;while(!(mant&UINT64_C(0x10000000000000))){mant<<=1;--exponent;}}
    if(exponent%2){mant<<=1;--exponent;}
    unsigned __int128 radicand=(unsigned __int128)mant<<52;
    uint64_t low=0,high=UINT64_C(0x20000000000000);
    while(low+1<high){uint64_t mid=low+(high-low)/2;
        if((unsigned __int128)mid*mid<=radicand)low=mid;else high=mid;}
    unsigned __int128 remainder=radicand-(unsigned __int128)low*low;
    if(remainder)c->fpsr|=16;
    if(remainder>low)++low;
    exponent/=2;
    if(low==UINT64_C(0x20000000000000)){low>>=1;++exponent;}
    r=((uint64_t)(exponent+1023)<<52)|(low&UINT64_C(0xfffffffffffff));
}
'''

DIVIDE = r'''
uint32_t sign=(a^b)&0x80000000,am=a&0x7fffff,bm=b&0x7fffff;
int ae=(int)((a>>23)&255)-127,be=(int)((b>>23)&255)-127;
r=sign;
if(!(b&0x7fffffff)){
    if(!(a&0x7fffffff)){r=0x7fc00000;c->fpsr|=1;}
    else {r=sign|0x7f800000;c->fpsr|=2;}
}
else if(a&0x7fffffff){
    if((a>>23)&255)am|=0x800000;else {ae=-126;while(!(am&0x800000)){am<<=1;--ae;}}
    if((b>>23)&255)bm|=0x800000;else {be=-126;while(!(bm&0x800000)){bm<<=1;--be;}}
    int exponent=ae-be;
    if(am<bm){am<<=1;--exponent;}
    int subshift=exponent < -126 ? -126-exponent : 0;
    if(subshift && (c->fpcr&0x1000000))c->fpsr|=8;
    else if(subshift>=26)c->fpsr|=24;
    else {
        uint64_t numerator=(uint64_t)am<<23,denominator=(uint64_t)bm<<subshift;
        uint64_t mant=numerator/denominator,remainder=numerator%denominator;
        if(remainder)c->fpsr|=16;
        if(subshift && remainder)c->fpsr|=8;
        if(remainder*2>denominator || (remainder*2==denominator && (mant&1)))++mant;
        if(exponent < -126)exponent=-126;
        if(mant>=0x1000000){mant>>=1;++exponent;}
        if(exponent>127){r|=0x7f800000;c->fpsr|=20;}
        else if(mant<0x800000)r|=(uint32_t)mant;
        else r|=((uint32_t)(exponent+127)<<23)|((uint32_t)mant&0x7fffff);
    }
}
'''

DIVIDE64 = r'''
uint64_t sign=(a^b)&UINT64_C(0x8000000000000000);
uint64_t am=a&UINT64_C(0xfffffffffffff),bm=b&UINT64_C(0xfffffffffffff);
int ae=(int)((a>>52)&2047)-1023,be=(int)((b>>52)&2047)-1023;
r=sign;
if(!(b&UINT64_C(0x7fffffffffffffff))){
    if(!(a&UINT64_C(0x7fffffffffffffff))){r=UINT64_C(0x7ff8000000000000);c->fpsr|=1;}
    else {r=sign|UINT64_C(0x7ff0000000000000);c->fpsr|=2;}
}
else if(a&UINT64_C(0x7fffffffffffffff)){
    if((a>>52)&2047)am|=UINT64_C(0x10000000000000);
    else {ae=-1022;while(!(am&UINT64_C(0x10000000000000))){am<<=1;--ae;}}
    if((b>>52)&2047)bm|=UINT64_C(0x10000000000000);
    else {be=-1022;while(!(bm&UINT64_C(0x10000000000000))){bm<<=1;--be;}}
    int exponent=ae-be;
    if(am<bm){am<<=1;--exponent;}
    int subshift=exponent < -1022 ? -1022-exponent : 0;
    /* FPRound: FZ flushes on the unrounded exponent (UFC only); otherwise a tiny inexact
       quotient raises UFC even when it rounds up to the smallest normal. */
    if(subshift && (c->fpcr&UINT32_C(0x1000000)))c->fpsr|=8;
    else if(subshift>=54){c->fpsr|=24;}
    else {
        unsigned __int128 numerator=(unsigned __int128)am<<52;
        unsigned __int128 denominator=(unsigned __int128)bm<<subshift;
        uint64_t mant=(uint64_t)(numerator/denominator);
        unsigned __int128 remainder=numerator%denominator;
        if(remainder)c->fpsr|=16;
        if(remainder*2>denominator || (remainder*2==denominator && (mant&1)))++mant;
        if(subshift){
            if(remainder)c->fpsr|=8;
            r|=mant>=UINT64_C(0x10000000000000)?UINT64_C(0x10000000000000):mant;
        }else{
            if(mant>=UINT64_C(0x20000000000000)){mant>>=1;++exponent;}
            if(exponent>1023){r|=UINT64_C(0x7ff0000000000000);c->fpsr|=20;}
            else r|=((uint64_t)(exponent+1023)<<52)|(mant&UINT64_C(0xfffffffffffff));
        }
    }
}
'''

ADD64 = r'''
unsigned sign=(unsigned)(a>>63);
uint64_t am=a&UINT64_C(0x7fffffffffffffff),bm=b&UINT64_C(0x7fffffffffffffff);
if(am<bm){uint64_t tmp=a;a=b;b=tmp;tmp=am;am=bm;bm=tmp;sign=(unsigned)(a>>63);}
unsigned ae=(unsigned)(am>>52),be=(unsigned)(bm>>52);
uint64_t av=am&UINT64_C(0xfffffffffffff),bv=bm&UINT64_C(0xfffffffffffff);
if(ae)av|=UINT64_C(0x10000000000000);else ae=1;
if(be)bv|=UINT64_C(0x10000000000000);else be=1;
av<<=3;bv=boot_shift_jam(bv<<3,ae-be);
uint64_t sum=((a^b)>>63)?av-bv:av+bv;
if(!sum)r=(a&b)&UINT64_C(0x8000000000000000);
else {
    if(sum>=UINT64_C(0x100000000000000)){sum=boot_shift_jam(sum,1);++ae;}
    while(sum<UINT64_C(0x80000000000000) && ae>1){sum<<=1;--ae;}
    if((c->fpcr&0x1000000) && ae==1 && sum<UINT64_C(0x80000000000000)){r=(uint64_t)sign<<63;c->fpsr|=8;}
    else {
    uint64_t mant=sum>>3;unsigned tail=(unsigned)(sum&7);
    if(tail)c->fpsr|=16;
    if(tail>4 || (tail==4 && (mant&1)))++mant;
    if(mant>=UINT64_C(0x20000000000000)){mant>>=1;++ae;}
    if(ae>=2047){c->fpsr|=20;r=((uint64_t)sign<<63)|UINT64_C(0x7ff0000000000000);}
    else {if(ae==1 && mant<UINT64_C(0x10000000000000)){ae=0;if(tail)c->fpsr|=8;}
        r=((uint64_t)sign<<63)|((uint64_t)ae<<52)|(mant&UINT64_C(0xfffffffffffff));}
    }
}
'''

# Reuse the exact integer rounding procedure, with the binary64 significand
# and power in place of a binary32 product. This is generated text, not a
# runtime conversion through host floating-point arithmetic.
NARROW = MULTIPLY[MULTIPLY.index('r=sign;'):].replace(
    'int power=ae+be-300,top=63-__builtin_clzll(product);', 'int top=63-__builtin_clzll(product);')
NARROW = r'''
uint32_t sign=(uint32_t)(a>>32)&0x80000000;
uint64_t product=a&UINT64_C(0xfffffffffffff);
int encoded=(int)((a>>52)&2047),power=encoded ? encoded-1075 : -1074;
if(encoded)product|=UINT64_C(0x10000000000000);
''' + NARROW

MULTIPLY64 = r'''
uint64_t sign=(a^b)&UINT64_C(0x8000000000000000),am=a&UINT64_C(0xfffffffffffff),bm=b&UINT64_C(0xfffffffffffff);
int ae=(int)((a>>52)&2047),be=(int)((b>>52)&2047);
if(ae)am|=UINT64_C(0x10000000000000);else ae=1;
if(be)bm|=UINT64_C(0x10000000000000);else be=1;
unsigned __int128 product=(unsigned __int128)am*bm;
r=sign;
if(product){
    int power=ae+be-2150,top=(uint64_t)(product>>64)?127-__builtin_clzll((uint64_t)(product>>64)):63-__builtin_clzll((uint64_t)product);
    int exponent=top+power;
    if((c->fpcr&0x1000000) && exponent < -1022)c->fpsr|=8;
    else {
    int shift=top-52;if(exponent < -1022)shift=-1074-power;
    unsigned __int128 mant,rem=0,half=0;
    if(shift<=0)mant=product<<(-shift);
    else if(shift>=128){mant=0;rem=product;half=~(unsigned __int128)0;}
    else {mant=product>>shift;rem=product&(((unsigned __int128)1<<shift)-1);half=(unsigned __int128)1<<(shift-1);}
    if(rem)c->fpsr|=16;
    if(exponent < -1022 && rem)c->fpsr|=8;
    if(rem && (rem>half || (rem==half && (mant&1))))++mant;
    if(exponent < -1022)exponent=-1022;
    if(mant>=UINT64_C(0x20000000000000)){mant>>=1;++exponent;}
    if(exponent>1023){r|=UINT64_C(0x7ff0000000000000);c->fpsr|=20;}
    else if(mant<UINT64_C(0x10000000000000))r|=(uint64_t)mant;
    else r|=((uint64_t)(exponent+1023)<<52)|((uint64_t)mant&UINT64_C(0xfffffffffffff));
    }
}
'''

def flush_inputs(width,*names):
    sign='UINT64_C(0x8000000000000000)' if width==64 else '0x80000000'
    magnitude='UINT64_C(0x7fffffffffffffff)' if width==64 else '0x7fffffff'
    minimum='UINT64_C(0x10000000000000)' if width==64 else '0x800000'
    return ''.join(f'if((c->fpcr&0x1000000) && ({n}&{magnitude}) && ({n}&{magnitude})<{minimum}){{{n}&={sign};c->fpsr|=128;}}' for n in names)

def _k(width):
    """C constants for Inf/NaN handling: sign, magnitude, infinity, quiet bit, default NaN, 2.0."""
    if width==32:
        return dict(S='UINT32_C(0x80000000)',M='UINT32_C(0x7fffffff)',I='UINT32_C(0x7f800000)',
                    Q='UINT32_C(0x400000)',D='UINT32_C(0x7fc00000)',T='UINT32_C(0x40000000)')
    return dict(S='UINT64_C(0x8000000000000000)',M='UINT64_C(0x7fffffffffffffff)',I='UINT64_C(0x7ff0000000000000)',
                Q='UINT64_C(0x8000000000000)',D='UINT64_C(0x7ff8000000000000)',T='UINT64_C(0x4000000000000000)')

def process_nans(width,a='a',b='b',r='r'):
    """Arm FPProcessNaNs (FPCR.AH=0) for C locals sa_/sb_ (magnitudes) when one is a NaN:
    first SNaN (a then b) quieted with IOC, else first QNaN; FPCR.DN gives the default NaN."""
    k=_k(width)
    return (f'int as_=sa_>{k["I"]} && !({a}&{k["Q"]}),bs_=sb_>{k["I"]} && !({b}&{k["Q"]});if(as_ || bs_)c->fpsr|=1;'
            f'{r}=as_?({a}|{k["Q"]}):bs_?({b}|{k["Q"]}):sa_>{k["I"]}?{a}:{b};if(c->fpcr&0x2000000){r}={k["D"]};')

def special_binary(width,kind,finite,a='a',b='b',r='r'):
    """Arm FPAdd/FPSub/FPMul/FPMulX/FPDiv with infinity/NaN operands, then `finite` otherwise.
    Inputs must already be FZ-flushed (FPUnpack). For 'sub' the operand b is passed unnegated:
    NaN propagation uses the original operand, infinities use -b. Invalid operations
    (inf-inf, inf*0, inf/inf) give FPDefaultNaN and IOC whatever FPCR.DN is. inf/0 gives a
    signed infinity without DZC. FPMulX gives 2.0 with the product sign for inf*0."""
    k=_k(width)
    if kind in ('add','sub'):
        y=b if kind=='add' else f'({b}^{k["S"]})'
        infinite=f'if(ai_ && bi_ && (({a}^{y})&{k["S"]})){{{r}={k["D"]};c->fpsr|=1;}}else {r}=ai_?{a}:{y};'
    elif kind in ('mul','mulx'):
        zero=f'{{{r}={k["D"]};c->fpsr|=1;}}' if kind=='mul' else f'{r}=(({a}^{b})&{k["S"]})|{k["T"]};'
        infinite=f'if((ai_ && !sb_) || (bi_ && !sa_)){zero}else {r}=(({a}^{b})&{k["S"]})|{k["I"]};'
    elif kind=='div':
        infinite=f'if(ai_ && bi_){{{r}={k["D"]};c->fpsr|=1;}}else {r}=(({a}^{b})&{k["S"]})|(ai_?{k["I"]}:0);'
    else:raise ValueError(kind)
    return (f'{{uint64_t sa_=(uint64_t)({a}&{k["M"]}),sb_=(uint64_t)({b}&{k["M"]});'
            f'if(sa_>={k["I"]} || sb_>={k["I"]}){{'
            f'if(sa_>{k["I"]} || sb_>{k["I"]}){{{process_nans(width,a,b,r)}}}'
            f'else {{int ai_=sa_=={k["I"]},bi_=sb_=={k["I"]};{infinite}}}'
            f'}}else {{\n{finite}\n}}}}')

def special_sqrt(width,finite,a='a',r='r'):
    """Arm FPSqrt special cases (input already FZ-flushed): NaN via FPProcessNaN, negative
    nonzero (including -inf) -> default NaN with IOC; +-0 and +inf fall through to `finite`
    (which returns them unchanged)."""
    k=_k(width)
    return (f'{{uint64_t sa_=(uint64_t)({a}&{k["M"]});'
            f'if(sa_>{k["I"]}){{if(!({a}&{k["Q"]}))c->fpsr|=1;{r}=(c->fpcr&0x2000000)?{k["D"]}:({a}|{k["Q"]});}}'
            f'else if(({a}&{k["S"]}) && sa_){{{r}={k["D"]};c->fpsr|=1;}}'
            f'else {{\n{finite}\n}}}}')

# ---------------------------------------------------------------------------------------------------------
# Patch 58 (play-work/fp-work, BORROW-PLAN C1-C3): shared cold exact paths behind inline native fast paths.
#
# The translator no longer expands the exact Arm code at every FP instruction. A site emits (fp_call below)
#   if(!BOOT_FP_NATIVE || !<engine>(...)) <cold>(...)
# The engine (inline, boot_fp_native.h) computes the host IEEE result and keeps it only when it is provably
# the Arm result with FPSR unchanged. The cold helper (noinline, cold, defined once in runtime_support via the
# page-header split) is boot_fpx_*: the complete exact implementation, i.e. the text the emitters used to
# inline, produced by the same functions as before, wrapped in a function with the register numbers as
# parameters; or, for the fused forms on the native build, boot_fps_*: patch 57's MXCSR-isolated FMA per lane
# (exact IXC), then the exact code (the old BOOT_FP_NATIVE=1 behaviour of those sites).
# fp_paths_header() is boot_fp_paths.h (written by gen_fp_paths.py); fp_page_roots() feeds PAGE_ROOTS.
# ---------------------------------------------------------------------------------------------------------
import re as _re

def _scalar32_exact(op):
    """Old emitter text for fadd/fsub/fmul/fnmul/fdiv S (translate_startup.py), as a function body:
    the special-case `store; goto next` became `return r;`."""
    body=[flush_inputs(32,'a','b')]
    if op in ('fadd','fsub'):
        same='((a^b)&0x80000000)' if op=='fadd' else '!((a^b)&0x80000000)'
        infinite=f'if(ai && bi && {same}){{r=0x7fc00000;c->fpsr|=1;}}else r=ai?a:{"b" if op=="fadd" else "(b^0x80000000)"};'
    elif op in ('fmul','fnmul'):
        infinite='if((ai && !(b&0x7fffffff)) || (bi && !(a&0x7fffffff))){r=0x7fc00000;c->fpsr|=1;}else r=((a^b)&0x80000000)|0x7f800000;'
    else:
        infinite='if(ai && bi){r=0x7fc00000;c->fpsr|=1;}else r=((a^b)&0x80000000)|(ai?0x7f800000:0);'
    body.append('if((a&0x7f800000)==0x7f800000 || (b&0x7f800000)==0x7f800000){'
                'int an=(a&0x7fffffff)>0x7f800000,bn=(b&0x7fffffff)>0x7f800000,ai=(a&0x7fffffff)==0x7f800000,bi=(b&0x7fffffff)==0x7f800000;(void)ai;(void)bi;'
                'if(an || bn){int as=an && !(a&0x400000),bs=bn && !(b&0x400000);if(as || bs)c->fpsr|=1;'
                'r=as?(a|0x400000):bs?(b|0x400000):an?a:b;if(c->fpcr&0x2000000)r=0x7fc00000;}'
                'else{'+infinite+'}'+('r^=UINT32_C(0x80000000);' if op=='fnmul' else '')+'return r;}')
    if op in ('fadd','fsub'):
        if op=='fsub':body.append('b^=0x80000000;')
        body.append('r=boot_add_finite32(a,b,&c->fpsr);if((c->fpcr&0x1000000) && (r&0x7fffffff) && (r&0x7fffffff)<0x800000){r&=0x80000000;c->fpsr|=8;}')
    elif op in ('fmul','fnmul'):
        body.append(MULTIPLY)
        if op=='fnmul':body.append('r^=UINT32_C(0x80000000);')
    else:body.append(DIVIDE)
    return '\n'.join(body)

def _scalar64_exact(op):
    """Old emitter text for fadd/fsub/fmul/fnmul/fdiv D (its BOOT_FP_NATIVE=0 branch)."""
    finite=DIVIDE64 if op=='fdiv' else MULTIPLY64 if op in ('fmul','fnmul') else ADD64
    if op=='fsub':finite='b^=UINT64_C(0x8000000000000000);'+finite
    text=flush_inputs(64,'a','b')+special_binary(64,{'fadd':'add','fsub':'sub','fmul':'mul','fnmul':'mul','fdiv':'div'}[op],finite)
    if op=='fnmul':text+='r^=UINT64_C(0x8000000000000000);'
    return text

def _vector_finite(op,width):
    if op=='fmul':finite=MULTIPLY64 if width==64 else MULTIPLY
    elif width==64:finite=ADD64
    else:finite='r=boot_add_finite32(a,b,&c->fpsr);if((c->fpcr&0x1000000) && (r&0x7fffffff) && (r&0x7fffffff)<0x800000){r&=0x80000000;c->fpsr|=8;}'
    if op in ('fsub','fabd'):finite=f'b^=UINT{width}_C({1<<(width-1)});'+finite
    return finite

def _vector_loads(op,lanes,size,indexed):
    width=size*8
    if op=='faddp':
        return f'for(unsigned lane=0;lane<{lanes};++lane){{uint{width}_t a,b,r;const uint8_t*p=c->q[lane<{lanes//2}?n:m]+(lane%{lanes//2})*{size*2};memcpy(&a,p,{size});memcpy(&b,p+{size},{size});'
    right=f'e*{size}' if indexed else f'lane*{size}'
    return f'for(unsigned lane=0;lane<{lanes};++lane){{uint{width}_t a,b,r;memcpy(&a,c->q[n]+lane*{size},{size});memcpy(&b,c->q[m]+{right},{size});'

def _vector_exact(op,lanes,size,indexed):
    """Old vector FADD/FSUB/FABD/FMUL/FADDP emitter loop (its BOOT_FP_NATIVE=0 branch), registers as parameters."""
    width=size*8
    kind='mul' if op=='fmul' else 'sub' if op in ('fsub','fabd') else 'add'
    text=['uint8_t result[16]={0};',_vector_loads(op,lanes,size,indexed),'r=0;',flush_inputs(width,'a','b'),
          special_binary(width,kind,_vector_finite(op,width))]
    if op=='fabd':text.append(f'r&=UINT{width}_C({(1<<(width-1))-1});')
    text.append(f'memcpy(result+lane*{size},&r,{size});}}memcpy(c->q[d],result,16);')
    return '\n'.join(text)

def _vector_fused(op,lanes,size,indexed,native):
    """Old vector FMLA/FMLS loop. native=False: the exact code only (BOOT_FP_NATIVE=0 branch); True: patch 57's
    MXCSR-isolated FMA per lane, then the exact code (the old BOOT_FP_NATIVE=1 behaviour)."""
    width=size*8
    text=['uint8_t result[16]={0};',_vector_loads(op,lanes,size,indexed),
          f'uint{width}_t acc;memcpy(&acc,c->q[d]+lane*{size},{size});']
    if op=='fmls':text.append(f'a^=UINT{width}_C({1<<(width-1)});')
    text.append(f'if(!boot_fp_fma{width}_mxcsr(c,acc,a,b,&r)){{\n'+fused_multiply_add_exact(width)+'\n}' if native else fused_multiply_add_exact(width))
    text.append(f'memcpy(result+lane*{size},&r,{size});}}memcpy(c->q[d],result,16);')
    return '\n'.join(text)

def _vector_divsqrt_exact(op,lanes):
    """Old vector FDIV/FSQRT .2S/.4S emitter loop (no native path existed)."""
    text=[f'uint8_t result[16]={{0}};for(unsigned lane=0;lane<{lanes};++lane){{uint32_t a,r;memcpy(&a,c->q[n]+lane*4,4);']
    if op=='fdiv':text.append('uint32_t b;memcpy(&b,c->q[m]+lane*4,4);')
    else:text.insert(0,'(void)m;')
    text.append(flush_inputs(32,*(('a','b') if op=='fdiv' else ('a',))))
    text.append(special_binary(32,'div',DIVIDE) if op=='fdiv' else special_sqrt(32,SQUARE_ROOT))
    text.append('memcpy(result+lane*4,&r,4);}memcpy(c->q[d],result,16);')
    return '\n'.join(text)

FP_VECTOR_SHAPES={'2s':(2,4),'4s':(4,4),'2d':(2,8)}

def fp_path_forms():
    """[(name, return type, parameters, body)] of every generated cold helper."""
    forms=[]
    def cold(name,ret,params,body):forms.append((name,ret,params,body))
    for op in ('fadd','fsub','fmul','fnmul','fdiv'):
        cold(f'boot_fpx_{op}_s','uint32_t','BootContext*c,uint32_t a,uint32_t b','uint32_t r=0;\n'+_scalar32_exact(op)+'\nreturn r;')
        cold(f'boot_fpx_{op}_d','uint64_t','BootContext*c,uint64_t a,uint64_t b','uint64_t r=0;\n'+_scalar64_exact(op)+'\nreturn r;')
    # FMUL Sd, Sn, Vm.S[i] (old indexed-scalar emitter text); its D form is the D scalar FMUL text.
    cold('boot_fpx_fmul_se','uint32_t','BootContext*c,uint32_t a,uint32_t b','uint32_t r=0;\n'+flush_inputs(32,'a','b')+special_binary(32,'mul',MULTIPLY)+'\nreturn r;')
    for width,root in ((32,SQUARE_ROOT),(64,SQUARE_ROOT64)):
        t=f'uint{width}_t';s='s' if width==32 else 'd'
        cold(f'boot_fpx_fsqrt_{s}',t,f'BootContext*c,{t} a',f'{t} r=0;\n'+flush_inputs(width,'a')+special_sqrt(width,root)+'\nreturn r;')
        cold(f'boot_fpx_fma_{s}',t,f'BootContext*c,{t} acc,{t} a,{t} b',f'{t} r=0;\n'+fused_multiply_add_exact(width)+'\nreturn r;')
        cold(f'boot_fps_fma_{s}',t,f'BootContext*c,{t} acc,{t} a,{t} b',
             f'{t} r=0;\nif(!boot_fp_fma{width}_mxcsr(c,acc,a,b,&r)){{\n'+fused_multiply_add_exact(width)+'\n}\nreturn r;')
    for shape,(lanes,size) in FP_VECTOR_SHAPES.items():
        for op in ('fadd','fsub','fabd','fmul','faddp'):
            cold(f'boot_fpx_{op}_{shape}','void','BootContext*c,unsigned d,unsigned n,unsigned m',_vector_exact(op,lanes,size,False))
        cold(f'boot_fpx_fmul_{shape}_e','void','BootContext*c,unsigned d,unsigned n,unsigned m,unsigned e',_vector_exact('fmul',lanes,size,True))
        for op in ('fmla','fmls'):
            for indexed in (False,True):
                suffix='_e' if indexed else ''
                params='BootContext*c,unsigned d,unsigned n,unsigned m'+(',unsigned e' if indexed else '')
                cold(f'boot_fpx_{op}_{shape}{suffix}','void',params,_vector_fused(op,lanes,size,indexed,False))
                cold(f'boot_fps_{op}_{shape}{suffix}','void',params,_vector_fused(op,lanes,size,indexed,True))
    for shape,lanes in (('2s',2),('4s',4)):
        cold(f'boot_fpx_fdiv_{shape}','void','BootContext*c,unsigned d,unsigned n,unsigned m',_vector_divsqrt_exact('fdiv',lanes))
        cold(f'boot_fpx_fsqrt_{shape}','void','BootContext*c,unsigned d,unsigned n,unsigned m',_vector_divsqrt_exact('fsqrt',lanes))
    return forms

FP_ENGINES=('boot_fpn32','boot_fpn64','boot_vfn32','boot_vfn64','boot_vfn32_fma','boot_vfn64_fma','boot_fp_fma32_inline','boot_fp_fma64_inline',
            # translator batch (boot_fp_native.h)
            'BOOT_TBN','BOOT_TB_NATIVE','boot_fpn_cmp32','boot_fpn_cmp64','boot_vfn_cmp32','boot_vfn_cmp64','boot_vfn_step32','boot_vfn_step64',
            'boot_fpn_cvtf','boot_vcvtf','boot_fpn_cvtz32','boot_fpn_cvtz64','BOOT_QZ','boot_qz','boot_qzp','BOOT_QZP','BOOT_QLINE','boot_q16')
FP_OPS={'fadd':'ADD','fsub':'SUB','fabd':'ABD','fmul':'MUL','fnmul':'NMUL','fdiv':'DIV','faddp':'ADDP','fsqrt':'SQRT'}

def fp_scalar_call(op,width,cold=None):
    """C statement r=<op>(a,b) for a scalar site (locals a, b, r; fsqrt: a only)."""
    s='s' if width==32 else 'd';cold=cold or f'boot_fpx_{op}_{s}'
    args='c,a' if op=='fsqrt' else 'c,a,b'
    return f'if(!BOOT_FP_NATIVE || !boot_fpn{width}(c,BOOT_FPOP_{FP_OPS[op]},a,{"0" if op=="fsqrt" else "b"},&r))r={cold}({args});'

def fp_fma_call(width):
    """C statement r=acc+a*b for a scalar fused site (locals acc, a, b, r)."""
    s='s' if width==32 else 'd'
    return (f'if(!BOOT_FP_NATIVE)r=boot_fpx_fma_{s}(c,acc,a,b);'
            f'else if(!boot_fp_fma{width}_inline(c,acc,a,b,&r))r=boot_fps_fma_{s}(c,acc,a,b);')

def fp_vector_call(op,lanes,size,d,n,m,e=None):
    """C statement for a vector site: registers d, n, m (and element e for the by-element forms)."""
    width=size*8;shape='2d' if size==8 else f'{lanes}s';suffix='_e' if e is not None else ''
    regs=f'c,{d},{n},{m}'+(f',{e}' if e is not None else '');index=-1 if e is None else e
    if op in ('fmla','fmls'):
        return (f'if(!BOOT_FP_NATIVE)boot_fpx_{op}_{shape}{suffix}({regs});'
                f'else if(!boot_vfn{width}_fma(c,{int(op=="fmls")},{lanes},{d},{n},{m},{index}))boot_fps_{op}_{shape}{suffix}({regs});')
    return f'if(!BOOT_FP_NATIVE || !boot_vfn{width}(c,BOOT_FPOP_{FP_OPS[op]},{lanes},{d},{n},{m},{index}))boot_fpx_{op}_{shape}{suffix}({regs});'

def fp_paths_header():
    """Text of boot_fp_paths.h (written by gen_fp_paths.py; included at the end of boot_fp_native.h)."""
    parts=['/* GENERATED by startup/gen_fp_paths.py from float_emit.fp_path_forms(): do not edit.',
           ' * Cold exact FP paths: boot_fpx_* is the exact Arm code the translator used to inline at every site;',
           ' * boot_fps_* is patch 57 MXCSR-isolated FMA per lane, then exact (fused forms, native build). */',
           '#ifndef TOTK_BOOT_FP_PATHS_H','#define TOTK_BOOT_FP_PATHS_H']
    for name,ret,params,body in fp_path_forms():
        parts.append(f'static inline __attribute__((noinline,cold)) {ret} {name}({params}){{\n{body}\n}}')
    parts.append('#endif')
    return '\n'.join(parts)+'\n'

def fp_page_roots():
    """Header names translated pages reference directly (split_page_header.PAGE_ROOTS)."""
    return frozenset({name for name,_,_,_ in fp_path_forms()}|set(FP_ENGINES)|{'BOOT_FPOP_'+v for v in FP_OPS.values()})
