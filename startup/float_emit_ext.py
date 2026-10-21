"""Additional exact ARM64 floating-point forms for the AOT translator.

emit_float_ext(i, op, args, pc, out) appends C statements for:
  fabd s|d (scalar), frintm v.4s|v.2d, frecpe s|v.2s|v.4s, frecps s|v.2s|v.4s,
  fcvt s,h | h,s, fcvtl v.4s,v.4h | v.2d,v.2s, fcvtn v.4h,v.4s, fcvtns s,s,
  facge/facgt s|d|v.2s|v.4s|v.2d, fmaxnm/fminnm v.2s|v.4s|v.2d,
  frintx s|d, frinta/n/p/z/i/x v.2s|v.4s|v.2d.
It returns False for any mnemonic/form it does not own so emit() can continue.
All arithmetic is integer-only generated C (no host FP environment), following
the Arm ARM pseudocode (FPSub/FPAbs, FPRoundInt, FPRecipEstimate,
FPRecipStepFused, FPConvert/FPConvertNaN, FPToFixed). FPCR.AHP and every other
bit not listed in each form's profile faults with BOOT_FP_SCOPE.
"""
import re
from capstone import arm64 as A64
from capstone.arm64 import ARM64_OP_REG

_V = r'v(?:[0-9]|[12][0-9]|3[01])'
def _rx(prefix):return r'['+prefix+r'](?:[0-9]|[12][0-9]|3[01])'

# FPCR profiles (bits accepted; anything else faults).
#  RMode=22..23, FZ=24, DN=25, FZ16=19, AHP=26 (never accepted).
_ROUND_PROFILE='0x3c00000'      # RMode|FZ|DN
_CONVERT_PROFILE='0x3c80000'    # RMode|FZ|DN|FZ16 (FPConvert forces FZ16=0)

def _fmt(width):
    if width==16:return 10,15
    if width==32:return 23,127
    return 52,1023

def round_pack(width,fz,result='r'):
    """Arm FPRound of the exact nonzero value (-1)^negative * sig * 2^exp2.
    Expects C locals: int negative; uint64_t sig (nonzero); int exp2; unsigned mode;
    assigns `result`. `fz` is a C expression enabling output flush-to-zero."""
    F,bias=_fmt(width);emin=1-bias
    sign=1<<(width-1);inf=(2*bias+1)<<F
    return (f'''
{{uint64_t output_sign=negative?UINT64_C({sign}):0;int top=63-__builtin_clzll(sig),exponent=top+exp2;
if(({fz}) && exponent<{emin}){{R=output_sign;c->fpsr|=8;}}
else {{
    int shift=exponent<{emin}?{emin-F}-exp2:top-{F};
    unsigned __int128 mant,rem=0,half=0;
    if(shift<=0)mant=(unsigned __int128)sig<<(-shift);
    else {{if(shift>65)shift=65;mant=(unsigned __int128)sig>>shift;rem=(unsigned __int128)sig&((((unsigned __int128)1)<<shift)-1);half=((unsigned __int128)1)<<(shift-1);}}
    int lost=rem!=0;
    if(lost){{c->fpsr|=16;if(exponent<{emin})c->fpsr|=8;}}
    if((mode==0 && (rem>half || (rem==half && (mant&1)))) || (mode==1 && !negative && lost) || (mode==2 && negative && lost))++mant;
    int e=exponent<{emin}?{emin}:exponent;
    if(mant>=((unsigned __int128)1<<{F+1})){{mant>>=1;++e;}}
    if(e>{bias}){{int infinite=mode==0 || (mode==1 && !negative) || (mode==2 && negative);R=output_sign|(infinite?UINT64_C({inf}):UINT64_C({inf-1}));c->fpsr|=20;}}
    else if(mant<((unsigned __int128)1<<{F}))R=output_sign|(uint64_t)mant;
    else R=output_sign|((uint64_t)(e+{bias})<<{F})|((uint64_t)mant&UINT64_C({(1<<F)-1}));
}}}}
''').replace('R=',result+'=')

def absolute_difference(width):
    """FPAbs(FPSub(a,b)) with FPCR.AH=0. Locals a,b,r of uint{width}_t."""
    from float_emit import flush_inputs
    F,bias=_fmt(width);sign=1<<(width-1);inf=(2*bias+1)<<F;quiet=1<<(F-1)
    return flush_inputs(width,'a','b')+f'''
{{unsigned mode=(c->fpcr>>22)&3;
uint64_t am=a&UINT64_C({sign-1}),bm=b&UINT64_C({sign-1});int an=am>UINT64_C({inf}),bn=bm>UINT64_C({inf});
if(an || bn){{
    int as=an && !(a&UINT64_C({quiet})),bs=bn && !(b&UINT64_C({quiet}));
    if(as || bs)c->fpsr|=1;
    uint64_t nan=as?a:bs?b:an?a:b;
    r=(c->fpcr&0x2000000)?UINT64_C({inf|quiet}):(nan|UINT64_C({quiet}));
}}else {{
    uint64_t x=a,y=b^UINT64_C({sign});
    if(am==UINT64_C({inf}) && bm==UINT64_C({inf})){{if((x^y)&UINT64_C({sign})){{r=UINT64_C({inf|quiet});c->fpsr|=1;}}else r=x;}}
    else if(am==UINT64_C({inf}))r=x;
    else if(bm==UINT64_C({inf}))r=y;
    else {{
        uint64_t xm=am,ym=bm;
        if(xm<ym){{uint64_t t=x;x=y;y=t;t=xm;xm=ym;ym=t;}}
        int xe=(int)(xm>>{F}),ye=(int)(ym>>{F});
        uint64_t xv=xm&UINT64_C({(1<<F)-1}),yv=ym&UINT64_C({(1<<F)-1});
        if(xe)xv|=UINT64_C({1<<F});else xe=1;
        if(ye)yv|=UINT64_C({1<<F});else ye=1;
        xv<<=3;yv=boot_shift_jam(yv<<3,(unsigned)(xe-ye));
        uint64_t sig=((x^y)&UINT64_C({sign}))?xv-yv:xv+yv;
        if(!sig)r=((x^y)&UINT64_C({sign}))?(mode==2?UINT64_C({sign}):0):(x&UINT64_C({sign}));
        else {{int negative=(x&UINT64_C({sign}))!=0,exp2=xe-{bias+F+3};{round_pack(width,'c->fpcr&0x1000000')}}}
    }}
}}
r&=UINT64_C({sign-1});}}
'''

def reciprocal_estimate(width):
    """Arm FPRecipEstimate (FPCR.AH=0, no FEAT_RPRES increased precision).
    Locals a,r of uint{width}_t."""
    F,bias=_fmt(width);sign=1<<(width-1);inf=(2*bias+1)<<F;quiet=1<<(F-1)
    return f'''
{{unsigned mode=(c->fpcr>>22)&3;uint64_t magnitude=a&UINT64_C({sign-1});
if((c->fpcr&0x1000000) && magnitude && magnitude<UINT64_C({1<<F})){{a&=UINT64_C({sign});magnitude=0;c->fpsr|=128;}}
int negative=(a&UINT64_C({sign}))!=0;
if(magnitude>UINT64_C({inf})){{
    if(!(a&UINT64_C({quiet})))c->fpsr|=1;
    r=(c->fpcr&0x2000000)?UINT64_C({inf|quiet}):(a|UINT64_C({quiet}));
}}else if(magnitude==UINT64_C({inf}))r=a&UINT64_C({sign});
else if(!magnitude){{r=(a&UINT64_C({sign}))|UINT64_C({inf});c->fpsr|=2;}}
else if(magnitude<UINT64_C({1<<(F-2)})){{
    int infinite=mode==0 || (mode==1 && !negative) || (mode==2 && negative);
    r=(a&UINT64_C({sign}))|(infinite?UINT64_C({inf}):UINT64_C({inf-1}));c->fpsr|=20;
}}else if((c->fpcr&0x1000000) && magnitude>=UINT64_C({(2*bias-1)<<F})){{r=a&UINT64_C({sign});c->fpsr|=8;}}
else {{
    int exponent=(int)(magnitude>>{F});uint64_t fraction=(magnitude&UINT64_C({(1<<F)-1}))<<{52-F};
    if(!exponent){{
        if(!(fraction>>51)){{exponent=-1;fraction=(fraction<<2)&UINT64_C({(1<<52)-1});}}
        else fraction=(fraction<<1)&UINT64_C({(1<<52)-1});
    }}
    unsigned scaled=256u|(unsigned)(fraction>>44);
    int result_exp={2*bias-1}-exponent;
    unsigned estimate=boot_fp_recip_table[scaled-256];
    fraction=(uint64_t)(estimate&255)<<44;
    if(result_exp==0)fraction=(UINT64_C(1)<<51)|(fraction>>1);
    else if(result_exp==-1){{fraction=(UINT64_C(1)<<50)|(fraction>>2);result_exp=0;}}
    r=(a&UINT64_C({sign}))|((uint64_t)result_exp<<{F})|(fraction>>{52-F});
}}}}
'''

def reciprocal_step(width):
    """Arm FPRecipStepFused: exact 2+(-a)*b over bounded limbs, one rounding.
    Locals a,b,r of uint{width}_t. Structure mirrors float_emit.reciprocal_sqrt_step."""
    from float_emit import flush_inputs
    F,bias=_fmt(width);fraction=F
    sign=1<<(width-1);inf=(2*bias+1)<<F;quiet=1<<(F-1);two=(bias+1)<<F
    limbs=(2*(bias+fraction-1)+2+63)//64
    return f'''
a^=UINT64_C({sign});
''' + flush_inputs(width,'a','b') + f'''
{{uint64_t am=a&UINT64_C({sign-1}),bm=b&UINT64_C({sign-1});
unsigned mode=(c->fpcr>>22)&3;
int an=am>UINT64_C({inf}),bn=bm>UINT64_C({inf});
if(an || bn){{
    int as=an && !(a&UINT64_C({quiet})),bs=bn && !(b&UINT64_C({quiet}));
    if(as || bs)c->fpsr|=1;
    uint64_t nan=as?a:bs?b:an?a:b;
    r=(c->fpcr&0x2000000)?UINT64_C({inf|quiet}):(nan|UINT64_C({quiet}));
}}else if((am==UINT64_C({inf}) && !bm) || (!am && bm==UINT64_C({inf})))r=UINT64_C({two});
else if(am==UINT64_C({inf}) || bm==UINT64_C({inf}))r=((a^b)&UINT64_C({sign}))|UINT64_C({inf});
else if(!am || !bm)r=UINT64_C({two});
else {{
    int ae=(int)(am>>{F}),be=(int)(bm>>{F});
    uint64_t av=am&UINT64_C({(1<<F)-1}),bv=bm&UINT64_C({(1<<F)-1});
    if(ae)av|=UINT64_C({1<<F});else ae=1;
    if(be)bv|=UINT64_C({1<<F});else be=1;
    int power=ae+be-{2*(bias+F)},common=power<0?power:0;
    unsigned __int128 product=(unsigned __int128)av*bv;
    uint64_t terms[2][{limbs}]={{{{0}}}},sum[{limbs}]={{0}};
    unsigned cb=(unsigned)(-common),cw=cb/64,cs=cb%64;
    terms[0][cw]=UINT64_C(2)<<cs;
    if(cs>62 && cw+1<{limbs})terms[0][cw+1]=UINT64_C(2)>>(64-cs);
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
        int exponent=top+common;
        if((c->fpcr&0x1000000) && exponent<{1-bias}){{r=output_sign;c->fpsr|=8;}}
        else {{
            int shift=top-{F};if(exponent<{1-bias})shift={1-bias-F}-common;
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
            if(mant>=UINT64_C({1<<(F+1)})){{mant>>=1;++exponent;}}
            if(exponent>{bias}){{int infinite=mode==0 || (mode==1 && !negative) || (mode==2 && negative);r=output_sign|(infinite?UINT64_C({inf}):UINT64_C({inf-1}));c->fpsr|=20;}}
            else if(mant<UINT64_C({1<<F}))r=output_sign|mant;
            else r=output_sign|((uint64_t)(exponent+{bias})<<{F})|(mant&UINT64_C({(1<<F)-1}));
        }}
    }}
}}}}
'''

def widen(src_width):
    """FPConvert to the next wider format (exact). Locals a (uint{src}_t), r (uint{2*src}_t).
    Half input: FPUnpackCV forces FZ16=0 (no flush). Single input: FZ flushes (IDC)."""
    from float_emit import flush_inputs
    F,bias=_fmt(src_width);G,gbias=_fmt(src_width*2)
    sign=1<<(src_width-1);inf=(2*bias+1)<<F;quiet=1<<(F-1)
    dw=src_width*2;dsign=1<<(dw-1);dinf=(2*gbias+1)<<G;dquiet=1<<(G-1)
    flush=flush_inputs(32,'a') if src_width==32 else ''
    return flush+f'''
{{uint64_t magnitude=a&UINT64_C({sign-1}),out_sign=(a&UINT64_C({sign}))?UINT64_C({dsign}):0;
if(magnitude>UINT64_C({inf})){{
    if(!(a&UINT64_C({quiet})))c->fpsr|=1;
    r=(c->fpcr&0x2000000)?UINT64_C({dinf|dquiet}):(out_sign|UINT64_C({dinf|dquiet})|((magnitude&UINT64_C({quiet-1}))<<{G-F}));
}}else if(magnitude==UINT64_C({inf}))r=out_sign|UINT64_C({dinf});
else if(!magnitude)r=out_sign;
else {{
    int exponent=(int)(magnitude>>{F});uint64_t mantissa=magnitude&UINT64_C({(1<<F)-1});
    if(!exponent){{exponent=1;while(!(mantissa&UINT64_C({1<<F}))){{mantissa<<=1;--exponent;}}mantissa&=UINT64_C({(1<<F)-1});}}
    r=out_sign|((uint64_t)(exponent-{bias}+{gbias})<<{G})|(mantissa<<{G-F});
}}}}
'''

def narrow_to_half():
    """FPConvert binary32 -> IEEE binary16 (AHP=0). Locals a (uint32_t), r (uint16_t).
    Input FZ flush applies (N=32); output never flushes (FPRoundCV forces FZ16=0)."""
    from float_emit import flush_inputs
    return flush_inputs(32,'a')+'''
{uint64_t magnitude=a&0x7fffffff,out_sign=(a&0x80000000)?0x8000:0;
if(magnitude>0x7f800000){
    if(!(a&0x400000))c->fpsr|=1;
    r=(c->fpcr&0x2000000)?0x7e00:(uint16_t)(out_sign|0x7e00|((magnitude>>13)&0x1ff));
}else if(magnitude==0x7f800000)r=(uint16_t)(out_sign|0x7c00);
else if(!magnitude)r=(uint16_t)out_sign;
else {
    unsigned mode=(c->fpcr>>22)&3;int exponent=(int)(magnitude>>23),negative=out_sign!=0;uint64_t sig=magnitude&0x7fffff,rr;
    if(exponent)sig|=0x800000;else exponent=1;
    int exp2=exponent-150;
''' + round_pack(16,'0','rr') + '''
    r=(uint16_t)rr;
}}
'''

def to_signed32_nearest():
    """FPToFixed(op,0,unsigned=FALSE,TIEEVEN) binary32 -> int32. Locals a (uint32_t), r (uint32_t)."""
    from float_emit import flush_inputs
    return flush_inputs(32,'a')+'''
{uint32_t magnitude=a&0x7fffffff;int negative=(a>>31)!=0;uint64_t limit=negative?UINT64_C(0x80000000):UINT64_C(0x7fffffff),value=0;
if(magnitude>0x7f800000){c->fpsr|=1;r=0;}
else if(magnitude==0x7f800000){c->fpsr|=1;r=(uint32_t)(negative?0x80000000u:0x7fffffffu);}
else if(!magnitude)r=0;
else {
    int exponent=(int)(magnitude>>23);uint64_t sig=magnitude&0x7fffff;
    if(exponent)sig|=0x800000;else exponent=1;
    int scale=exponent-150;int inexact=0,overflow=0;
    if(scale>=0){if(scale>=9)overflow=1;else value=sig<<scale;}
    else {
        unsigned shift=(unsigned)(-scale);if(shift>40)shift=40;
        uint64_t rem=sig&((UINT64_C(1)<<shift)-1),half=UINT64_C(1)<<(shift-1);
        value=sig>>shift;inexact=rem!=0;
        if(rem>half || (rem==half && (value&1)))++value;
    }
    if(overflow || value>limit){c->fpsr|=1;value=limit;}
    else if(inexact)c->fpsr|=16;
    r=(uint32_t)(negative?UINT64_C(0)-value:value);
}}
'''

def max_min_num(width,maximum):
    """Arm FPMaxNum/FPMinNum (FPCR.AH=0) on already FZ-flushed locals a,b -> r: a single
    quiet NaN loses to a number; any SNaN -> FPProcessNaNs (quieted or default NaN, IOC);
    two quiet NaNs -> FPProcessNaNs (first operand, or default NaN with DN); +-0 by sign."""
    F,bias=_fmt(width);sign=1<<(width-1);inf=(2*bias+1)<<F;quiet=1<<(F-1)
    M,I,Q,S,D=(f'UINT64_C({v})' for v in (sign-1,inf,quiet,sign,inf|quiet))
    return (f'{{uint64_t am=a&{M},bm=b&{M};int an=am>{I},bn=bm>{I};int as=an && !(a&{Q}),bs=bn && !(b&{Q});'
            f'if(as || bs){{r=(c->fpcr&0x2000000)?{D}:((as?a:b)|{Q});c->fpsr|=1;}}'
            f'else if(an && bn)r=(c->fpcr&0x2000000)?{D}:a;else if(an)r=b;else if(bn)r=a;'
            f'else if(!am && !bm)r=(a{"&" if maximum else "|"}b)&{S};'
            f'else {{int less=((a^b)&{S})?((a&{S})!=0):((a&{S})?am>bm:am<bm);r=less?{"b:a" if maximum else "a:b"};}}}}')

def absolute_compare(width,greater):
    """FPCompareGE/GT(FPAbs(a),FPAbs(b)) (FPCR.AH=0): FZ flushes the magnitudes (IDC); any NaN
    (quiet or signalling) compares false and raises IOC. Locals a,b -> r (all ones / zero)."""
    from float_emit import flush_inputs
    F,bias=_fmt(width);sign=1<<(width-1);inf=(2*bias+1)<<F
    return (f'a&=UINT64_C({sign-1});b&=UINT64_C({sign-1});'+flush_inputs(width,'a','b')+
            f'if(a>UINT64_C({inf}) || b>UINT64_C({inf})){{r=0;c->fpsr|=1;}}else r=(a{">" if greater else ">="}b)?UINT64_C({(1<<width)-1}):0;')

def _names(i,args):
    if any(o.type!=ARM64_OP_REG or o.vector_index!=-1 for o in args):return None
    return [i.reg_name(o.reg) for o in args]

def emit_float_ext(i,op,args,pc,out,Unsupported=None):
    if Unsupported is None:
        from translate_startup import Unsupported
    from float_emit import round_integral
    VAS={A64.ARM64_VAS_2S:(2,4),A64.ARM64_VAS_4S:(4,4),A64.ARM64_VAS_2D:(2,8)}
    def fault(profile,size):
        return f'if(c->fpcr&~UINT32_C({profile})){{boot_fault(c,BOOT_FP_SCOPE,{pc},0,{size});return c->status;}}'

    if op=='fabd':
        names=_names(i,args)
        if not names or len(names)!=3 or not all(re.fullmatch(_rx('sd'),n) for n in names):return False
        if len({n[0] for n in names})!=1:raise Unsupported('FABD scalar matching S/D registers')
        width=32 if names[0][0]=='s' else 64;size=width//8;dst,left,right=[int(n[1:]) for n in names]
        out.append(f'{{uint{width}_t a,b,r;memcpy(&a,c->q[{left}],{size});memcpy(&b,c->q[{right}],{size});')
        out.append(fault(_ROUND_PROFILE,size))
        out.append(absolute_difference(width))
        out.append(f'BOOT_QZ(c->q[{dst}],r,{size});}}')
        return True

    if op in ('frinta','frintn','frintm','frintp','frintz','frinti','frintx'):
        names=_names(i,args)
        if not names or len(names)!=2:return False
        if op=='frintx' and all(re.fullmatch(_rx('sd'),n) for n in names):
            if names[0][0]!=names[1][0]:raise Unsupported('FRINTX matching scalar S/D registers')
            width=32 if names[0][0]=='s' else 64;size=width//8;dst,src=[int(n[1:]) for n in names]
            out.append(f'{{uint{width}_t a,r;memcpy(&a,c->q[{src}],{size});{fault(_ROUND_PROFILE,size)}')
            out.append(round_integral(width,'frintx'))
            out.append(f'BOOT_QZ(c->q[{dst}],r,{size});}}')
            return True
        if not all(re.fullmatch(_V,n) for n in names):return False
        if args[0].vas!=args[1].vas or args[0].vas not in VAS:raise Unsupported(f'{op.upper()} vector arrangement')
        lanes,size=VAS[args[0].vas];width=size*8;dst,src=[int(n[1:]) for n in names]
        out.append(f'{{{fault(_ROUND_PROFILE,size)}uint8_t result[16]={{0}};for(unsigned lane=0;lane<{lanes};++lane){{uint{width}_t a,r;memcpy(&a,c->q[{src}]+lane*{size},{size});')
        out.append(round_integral(width,op))
        out.append(f'memcpy(result+lane*{size},&r,{size});}}memcpy(c->q[{dst}],result,16);}}')
        return True

    if op in ('fmaxnm','fminnm','facge','facgt'):
        names=_names(i,args)
        if not names or len(names)!=3:return False
        if all(re.fullmatch(_V,n) for n in names):
            if len({o.vas for o in args})!=1 or args[0].vas not in VAS:raise Unsupported(f'{op.upper()} vector arrangement')
            lanes,size=VAS[args[0].vas]
        elif op in ('facge','facgt') and all(re.fullmatch(_rx('sd'),n) for n in names):
            if len({n[0] for n in names})!=1:raise Unsupported(f'{op.upper()} matching scalar S/D registers')
            lanes,size=1,(4 if names[0][0]=="s" else 8)
        else:return False   # scalar FMAXNM/FMINNM: owned by emit()
        width=size*8;dst,left,right=[int(n[1:]) for n in names]
        from float_emit import flush_inputs
        body=(flush_inputs(width,'a','b')+max_min_num(width,op=='fmaxnm')) if op in ('fmaxnm','fminnm') else absolute_compare(width,op=='facgt')
        # Neither operation rounds, so FPCR.RMode is admitted along with FZ/DN.
        out.append(f'{{{fault(_ROUND_PROFILE,size)}uint8_t result[16]={{0}};for(unsigned lane=0;lane<{lanes};++lane){{uint{width}_t a,b,r;memcpy(&a,c->q[{left}]+lane*{size},{size});memcpy(&b,c->q[{right}]+lane*{size},{size});')
        out.append(body)
        out.append(f'memcpy(result+lane*{size},&r,{size});}}memcpy(c->q[{dst}],result,16);}}')
        return True

    if op in ('frecpe','frecps'):
        count=2 if op=='frecpe' else 3
        names=_names(i,args)
        if not names or len(names)!=count:raise Unsupported(f'{op.upper()} operands')
        if all(re.fullmatch(_V,n) for n in names) and all(o.vas==args[0].vas for o in args) and args[0].vas in (A64.ARM64_VAS_2S,A64.ARM64_VAS_4S):
            lanes,size=VAS[args[0].vas]
        elif all(re.fullmatch(_rx('s'),n) for n in names):lanes,size=1,4
        else:raise Unsupported(f'{op.upper()} arrangement')
        regs=[int(n[1:]) for n in names]
        if op=='frecpe':
            dst,src=regs
            out.append(f'{{{fault(_ROUND_PROFILE,size)}uint8_t result[16]={{0}};for(unsigned lane=0;lane<{lanes};++lane){{uint32_t a,r;memcpy(&a,c->q[{src}]+lane*4,4);')
            out.append(reciprocal_estimate(32))
        else:
            dst,left,right=regs
            # translator batch: host FMA first (boot_vfn_step32), the exact loop as the fallback
            out.append(f'{{{fault(_ROUND_PROFILE,size)}if(!BOOT_TBN || !boot_vfn_step32(c,0,{lanes},{dst},{left},{right})){{uint8_t result[16]={{0}};for(unsigned lane=0;lane<{lanes};++lane){{uint32_t a,b,r;memcpy(&a,c->q[{left}]+lane*4,4);memcpy(&b,c->q[{right}]+lane*4,4);')
            out.append(reciprocal_step(32))
        out.append(f'memcpy(result+lane*4,&r,4);}}memcpy(c->q[{dst}],result,16);'+('}' if op=='frecps' else '')+'}')
        return True

    if op=='fcvt':
        names=_names(i,args)
        if not names or len(names)!=2:return False
        if re.fullmatch(_rx('s'),names[0]) and re.fullmatch(_rx('h'),names[1]):
            dst,src=[int(n[1:]) for n in names]
            out.append(f'{{uint16_t a;uint32_t r;memcpy(&a,c->q[{src}],2);{fault(_CONVERT_PROFILE,4)}')
            out.append(widen(16))
            out.append(f'BOOT_QZ(c->q[{dst}],r,4);}}')
            return True
        if re.fullmatch(_rx('h'),names[0]) and re.fullmatch(_rx('s'),names[1]):
            dst,src=[int(n[1:]) for n in names]
            out.append(f'{{uint32_t a;uint16_t r;memcpy(&a,c->q[{src}],4);{fault(_CONVERT_PROFILE,2)}')
            out.append(narrow_to_half())
            out.append(f'BOOT_QZ(c->q[{dst}],r,2);}}')
            return True
        return False

    if op=='fcvtl':
        names=_names(i,args)
        if not names or len(names)!=2 or not all(re.fullmatch(_V,n) for n in names):raise Unsupported('FCVTL operands')
        shapes={(A64.ARM64_VAS_4S,A64.ARM64_VAS_4H):(4,16),(A64.ARM64_VAS_2D,A64.ARM64_VAS_2S):(2,32)}
        shape=shapes.get((args[0].vas,args[1].vas))
        if not shape:raise Unsupported('FCVTL arrangement')
        lanes,sw=shape;dw=2*sw;dst,src=[int(n[1:]) for n in names]
        out.append(f'{{{fault(_CONVERT_PROFILE,dw//8)}uint8_t result[16]={{0}};for(unsigned lane=0;lane<{lanes};++lane){{uint{sw}_t a;uint{dw}_t r;memcpy(&a,c->q[{src}]+lane*{sw//8},{sw//8});')
        out.append(widen(sw))
        out.append(f'memcpy(result+lane*{dw//8},&r,{dw//8});}}memcpy(c->q[{dst}],result,16);}}')
        return True

    if op=='fcvtn':
        names=_names(i,args)
        if not names or len(names)!=2 or not all(re.fullmatch(_V,n) for n in names):raise Unsupported('FCVTN operands')
        if args[0].vas!=A64.ARM64_VAS_4H or args[1].vas!=A64.ARM64_VAS_4S:raise Unsupported('FCVTN arrangement')
        dst,src=[int(n[1:]) for n in names]
        out.append(f'{{{fault(_CONVERT_PROFILE,2)}uint8_t result[16]={{0}};for(unsigned lane=0;lane<4;++lane){{uint32_t a;uint16_t r;memcpy(&a,c->q[{src}]+lane*4,4);')
        out.append(narrow_to_half())
        out.append(f'memcpy(result+lane*2,&r,2);}}memcpy(c->q[{dst}],result,16);}}')
        return True

    if op=='fcvtns':
        names=_names(i,args)
        if not names or len(names)!=2 or not all(re.fullmatch(_rx('s'),n) for n in names):return False
        dst,src=[int(n[1:]) for n in names]
        out.append(f'{{uint32_t a,r;memcpy(&a,c->q[{src}],4);{fault(_ROUND_PROFILE,4)}')
        out.append(to_signed32_nearest())
        out.append(f'BOOT_QZ(c->q[{dst}],r,4);}}')
        return True

    return False
