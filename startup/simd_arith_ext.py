"""Additional exact ARM64 SIMD integer / crypto / misc forms for the AOT translator.

emit_simd_arith_ext(i, op, args, pc, out, Unsupported) appends C statements for:
  ORR/BIC (vector, immediate)        .2s/.4s (LSL #0/8/16/24), .4h/.8h (LSL #0/8)
  CLZ/CLS (vector)                   .8b/.16b/.4h/.8h/.2s/.4s
  SMULL/UMULL/SMLAL/UMLAL/SMLSL/UMLSL[2] (vector)      .8h<-8b/16b, .4s<-4h/8h, .2d<-2s/4s
  SMULL/UMULL/SMLAL/UMLAL/SMLSL/UMLSL[2] (by element)  .4s<-4h/8h,h[0..7] (Vm<16), .2d<-2s/4s,s[0..3]
  SQSHRN/SQRSHRN/UQSHRN/UQRSHRN/SQSHRUN/SQRSHRUN/RSHRN[2] (vector), and the scalar
    saturating forms (b<-h, h<-s, s<-d); saturation sets FPSR.QC (bit 27)
  REV16 (.8b/.16b), REV32 (.8b/.16b/.4h/.8h)
  AESE/AESD/AESMC/AESIMC, SHA1C/P/M/H/SU0/SU1, SHA256H/H2/SU0/SU1
  LDAXRH/LDXRH/STLXRH/STXRH (existing boot_load_exclusive/boot_store_exclusive, width 2)
  CSDB (no architectural effect: no statements)
  SQDMULH/SQRDMULH (vector .4h/.8h/.2s/.4s, scalar h/s, by element vector and scalar)
  MLA/MLS (vector .8b..4s), UZP1/UZP2 (all arrangements), SMOV (w<-b/h, x<-b/h/s)
  SADDL/UADDL/SSUBL/USUBL/SABDL/UABDL/SABAL/UABAL[2], PMULL[2] (.8h<-8b/16b, .1q<-1d/2d)
  SQADD/UQADD/SQSUB/UQSUB (scalar b/h/s/d), SQSHL/UQSHL/SQRSHL/UQRSHL (register, vector and scalar)
  SQABS/SQNEG (vector and scalar), SQXTN/UQXTN/SQXTUN (scalar), UQXTN[2] (vector)
  SADDLV (.8b/.16b/.4h/.8h/.4s), UADDLV (.4h/.8h/.4s), SRSRA/URSRA (vector and scalar d)
Ownership is decided from the raw encoding fields (Arm ARM A64 encoding classes);
capstone's mnemonic and operands are then cross-checked against the decode, and any
disagreement raises Unsupported. Returns False for everything else (including SHRN
and REV64, which emit() already owns), so emit() can continue.
Generated C is integer-only and follows the Arm ARM pseudocode.
"""
import re
from capstone import arm64 as A64
from capstone.arm64 import ARM64_OP_REG,ARM64_OP_IMM,ARM64_OP_MEM,ARM64_SFT_INVALID,ARM64_SFT_LSL

QC='c->fpsr|=UINT32_C(0x8000000);'
VAS={'8b':A64.ARM64_VAS_8B,'16b':A64.ARM64_VAS_16B,'4h':A64.ARM64_VAS_4H,'8h':A64.ARM64_VAS_8H,
     '2s':A64.ARM64_VAS_2S,'4s':A64.ARM64_VAS_4S,'2d':A64.ARM64_VAS_2D,'h':A64.ARM64_VAS_1H,'s':A64.ARM64_VAS_1S,
     'b':A64.ARM64_VAS_1B,'d':A64.ARM64_VAS_1D,'1d':A64.ARM64_VAS_1D,'1q':A64.ARM64_VAS_1Q}

def _arr(size,Q):return ('8b','16b','4h','8h','2s','4s','1d','2d')[size*2+Q]

# ---- AES tables (computed, FIPS-197 5.1.1) ---------------------------------
def _gmul(a,b):
    r=0
    while b:
        if b&1:r^=a
        a=((a<<1)^0x11b) if a&0x80 else a<<1;b>>=1
    return r
def _sbox():
    inv=[0]*256
    for a in range(1,256):
        for b in range(1,256):
            if _gmul(a,b)==1:inv[a]=b;break
    box=[]
    for a in range(256):
        x=inv[a];s=x
        for k in range(1,5):s^=((x<<k)|(x>>(8-k)))&0xff
        box.append(s^0x63)
    return box
SBOX=_sbox();INV_SBOX=[0]*256
for _k,_v in enumerate(SBOX):INV_SBOX[_v]=_k
def _table(name,values):return f'static const uint8_t {name}[256]={{'+','.join(str(v) for v in values)+'};'

# ---- helpers -----------------------------------------------------------------
def _sx(v,bits):return f'((int64_t)((({v})^UINT64_C({1<<(bits-1)}))-UINT64_C({1<<(bits-1)})))'
def _rol(x,n):return f'((uint32_t)(({x})<<{n})|(uint32_t)(({x})>>{32-n}))'
def _ror(x,n):return _rol(x,32-n)

def _check(i,args,mnemonic,expected,Unsupported):
    """expected: list of ('v',n,arr) | ('r',name) | ('e',n,lanechar,index) | ('imm',value,lsl)."""
    if i.mnemonic!=mnemonic or len(args)!=len(expected):raise Unsupported(f'Decoder disagreement: {i.mnemonic} {i.op_str} vs {mnemonic}')
    for o,e in zip(args,expected):
        if e[0]=='imm':
            shift=0 if o.shift.type==ARM64_SFT_INVALID else (o.shift.value if o.shift.type==ARM64_SFT_LSL else -1)
            ok=o.type==ARM64_OP_IMM and o.imm==e[1] and shift==e[2]
        else:
            ok=o.type==ARM64_OP_REG and o.shift.type==ARM64_SFT_INVALID
            if ok:
                name=i.reg_name(o.reg)
                if e[0]=='r':ok=name==e[1] and o.vector_index==-1
                elif e[0]=='v':ok=name==f'v{e[1]}' and o.vas==VAS[e[2]] and o.vector_index==-1
                else:ok=name==f'v{e[1]}' and o.vas==VAS[e[2]] and o.vector_index==e[3]
        if not ok:raise Unsupported(f'Decoder disagreement: {i.mnemonic} {i.op_str}')

def _store(dst):return f'memcpy(c->q[{dst}],result,16);}}'

# ---- emitter -------------------------------------------------------------------
def emit_simd_arith_ext(i,op,args,pc,out,Unsupported):
    w=int.from_bytes(bytes(i.bytes),'little')
    rd,rn,rm=w&31,(w>>5)&31,(w>>16)&31
    Q,U=(w>>30)&1,(w>>29)&1

    if w==0xd503229f:   # CSDB: speculation barrier, no architectural effect (cf. NOP).
        _check(i,args,'csdb',[],Unsupported);return True

    # Exclusive halfword: same helpers/idiom as emit() uses for word/byte, width 2.
    if (w&0xfffffc00) in (0x485ffc00,0x485f7c00) or (w&0xffe0fc00) in (0x4800fc00,0x48007c00):
        from translate_startup import regname
        load=(w>>22)&1;mnemonic=('ld' if load else 'st')+('a' if (w>>15)&1 and load else 'l' if (w>>15)&1 else '')+'xrh'
        if i.mnemonic!=mnemonic:raise Unsupported(f'Decoder disagreement: {i.mnemonic} {i.op_str}')
        mi=1 if load else 2
        if len(args)!=mi+1 or args[mi].type!=ARM64_OP_MEM or args[mi].mem.index or args[mi].mem.disp:raise Unsupported('Exclusive address form')
        base,bw=regname(i.reg_name(args[mi].mem.base))
        if bw!=64:raise Unsupported('Exclusive base width')
        names=[i.reg_name(o.reg) if o.type==ARM64_OP_REG else None for o in args[:mi]]
        if any(n is None or not re.fullmatch(r'w(?:[0-9]|[12][0-9]|30)|wzr',n) for n in names):raise Unsupported('Exclusive halfword registers')
        def reg(n):return ('discard0' if n=='wzr' else regname(n)[0])
        if load:
            out.append(f'if(!boot_load_exclusive(c,{base},2,{pc},&{reg(names[0])}))return c->status;')
        else:
            if rm!=31 and (rm==(w&31) or (rm==rn and rn!=31)):raise Unsupported('Exclusive store status overlaps (CONSTRAINED UNPREDICTABLE)')
            value='UINT64_C(0)' if names[1]=='wzr' else f'(uint32_t){regname(names[1])[0]}'
            out.append(f'if(!boot_store_exclusive(c,{base},2,{pc},{value},&{reg(names[0])}))return c->status;')
        return True

    # AdvSIMD modified immediate: ORR/BIC (vector, immediate).
    if (w&0x9ff80c00)==0x0f000400:
        cmode=(w>>12)&15
        if cmode not in (1,3,5,7,9,11):return False   # MOVI/MVNI/FMOV
        imm8=(((w>>16)&7)<<5)|((w>>5)&31)
        if cmode<8:shift=((cmode>>1)&3)*8;element=(imm8<<shift)&0xffffffff;imm=element|(element<<32);arr='4s' if Q else '2s'
        else:shift=((cmode>>1)&1)*8;element=(imm8<<shift)&0xffff;imm=element*0x0001000100010001;arr='8h' if Q else '4h'
        mnemonic='bic' if U else 'orr'
        _check(i,args,mnemonic,[('v',rd,arr),('imm',imm8,shift)],Unsupported)
        expr=f'&UINT64_C(0x{(~imm)&(2**64-1):x})' if U else f'|UINT64_C(0x{imm:x})'
        out.append(f'{{uint64_t lo=little_read(c->q[{rd}],8)'+(f',hi=little_read(c->q[{rd}]+8,8){expr};' if Q else ',hi=0;')+
                   f'lo=lo{expr};little_write(c->q[{rd}],lo,8);little_write(c->q[{rd}]+8,hi,8);}}')
        return True

    # AdvSIMD two-register miscellaneous: CLS/CLZ, REV16/REV32.
    if (w&0x9f3e0c00)==0x0e200800:
        opcode=(w>>12)&31;size=(w>>22)&3
        if opcode==4:
            if size==3:raise Unsupported('CLZ/CLS size')
            mnemonic='clz' if U else 'cls';arr=_arr(size,Q)
            _check(i,args,mnemonic,[('v',rd,arr),('v',rn,arr)],Unsupported)
            element=1<<size;bits=8*element;total=16 if Q else 8
            if U:expr=f'(a?(uint64_t)(__builtin_clzll(a)-{64-bits}):UINT64_C({bits}))'
            else:expr=f'((((a>>1)^a)&UINT64_C({(1<<(bits-1))-1}))?(uint64_t)(__builtin_clzll(((a>>1)^a)&UINT64_C({(1<<(bits-1))-1}))-{65-bits}):UINT64_C({bits-1}))'
            out.append(f'{{uint8_t result[16]={{0}};for(unsigned off=0;off<{total};off+={element}){{uint64_t a=little_read(c->q[{rn}]+off,{element});little_write(result+off,{expr},{element});}}'+_store(rd))
            return True
        if opcode in (0,1):
            if opcode==0 and U==0:return False   # REV64: owned by emit()
            if opcode==1 and U==1:return False
            container=2 if opcode==1 else 4
            if (1<<size)>=container:raise Unsupported('REV16/REV32 element size')
            mnemonic='rev16' if opcode==1 else 'rev32';arr=_arr(size,Q)
            _check(i,args,mnemonic,[('v',rd,arr),('v',rn,arr)],Unsupported)
            element=1<<size;total=16 if Q else 8
            out.append(f'{{uint8_t result[16]={{0}};for(unsigned off=0;off<{total};off+={element})memcpy(result+off,c->q[{rn}]+(off^{container-element}),{element});'+_store(rd))
            return True
        return _rare_integer(i,w,args,out,Unsupported)

    # AdvSIMD three different: SMULL/UMULL/SMLAL/UMLAL/SMLSL/UMLSL[2].
    if (w&0x9f200c00)==0x0e200000 and ((w>>12)&15) in (8,10,12):
        opcode=(w>>12)&15;size=(w>>22)&3
        if size==3:raise Unsupported('Widening multiply size')
        kind={8:'mlal',10:'mlsl',12:'mull'}[opcode]
        mnemonic=('u' if U else 's')+kind+('2' if Q else '')
        element=1<<size;src=_arr(size,Q);dst=_arr(size+1,1)
        _check(i,args,mnemonic,[('v',rd,dst),('v',rn,src),('v',rm,src)],Unsupported)
        _widen(out,U,kind,element,Q,rd,rn,f'c->q[{rm}]+{8*Q}+lane*{element}')
        return True

    # AdvSIMD vector x indexed element: SMULL/UMULL/SMLAL/UMLAL/SMLSL/UMLSL[2] (by element).
    if (w&0x9f000400)==0x0f000000 and ((w>>12)&15) in (2,6,10):
        opcode=(w>>12)&15;size=(w>>22)&3;L,M,H=(w>>21)&1,(w>>20)&1,(w>>11)&1
        if size==1:index=(H<<2)|(L<<1)|M;m=rm&15;lane='h'
        elif size==2:index=(H<<1)|L;m=rm;lane='s'
        else:raise Unsupported('By-element widening multiply size')
        kind={2:'mlal',6:'mlsl',10:'mull'}[opcode]
        mnemonic=('u' if U else 's')+kind+('2' if Q else '')
        element=1<<size
        _check(i,args,mnemonic,[('v',rd,_arr(size+1,1)),('v',rn,_arr(size,Q)),('e',m,lane,index)],Unsupported)
        _widen(out,U,kind,element,Q,rd,rn,f'c->q[{m}]+{index*element}')
        return True

    # AdvSIMD shift by immediate (vector) and scalar: narrowing right shifts.
    vector=(w&0x9f800400)==0x0f000400;scalar=(w&0xdf800400)==0x5f000400
    if (vector or scalar) and (w>>19)&15 and ((w>>11)&31) in (16,17,18,19):
        opcode=(w>>11)&31;immh=(w>>19)&15;immb=(w>>16)&7
        if vector and opcode==16 and not U:return False   # SHRN: owned by emit()
        if scalar and opcode in (16,17) and not U:return False   # unallocated
        if immh&8:raise Unsupported('Narrowing shift immh')
        size=immh.bit_length()-1;element=1<<size;bits=8*element;shift=2*bits-((immh<<3)|immb)
        rounding=opcode&1
        if opcode in (16,17):name={(0,0):'shrn',(0,1):'rshrn',(1,0):'sqshrun',(1,1):'sqrshrun'}[(U,rounding)]
        else:name={(0,0):'sqshrn',(0,1):'sqrshrn',(1,0):'uqshrn',(1,1):'uqrshrn'}[(U,rounding)]
        signed_source=name.startswith('sq')
        saturate=None if name=='rshrn' else ('u' if (name.startswith('uq') or 'shrun' in name) else 's')
        if vector:
            mnemonic=name+('2' if Q else '')
            _check(i,args,mnemonic,[('v',rd,_arr(size,Q)),('v',rn,_arr(size+1,1)),('imm',shift,0)],Unsupported)
            lanes=8//element;base=8*Q;keep=Q
        else:
            _check(i,args,name,[('r','bhs'[size]+str(rd)),('r','hsd'[size]+str(rn)),('imm',shift,0)],Unsupported)
            lanes=1;base=0;keep=0
        source=2*element
        value=f'(__int128){_sx("raw",2*bits)}' if signed_source else '(__int128)raw'
        rnd=f'v+=(__int128)1<<{shift-1};' if rounding else ''
        if saturate=='s':lo,hi=-(1<<(bits-1)),(1<<(bits-1))-1
        elif saturate=='u':lo,hi=0,(1<<bits)-1
        clamp=(f'if(v<(__int128)INT64_C({lo})){{v=(__int128)INT64_C({lo});{QC}}}else if(v>(__int128)INT64_C({hi})){{v=(__int128)INT64_C({hi});{QC}}}' if saturate else '')
        out.append('{uint8_t result[16];'+(f'memcpy(result,c->q[{rd}],16);' if keep else 'memset(result,0,16);')+
                   f'for(unsigned lane=0;lane<{lanes};++lane){{uint64_t raw=little_read(c->q[{rn}]+lane*{source},{source});__int128 v={value};{rnd}v>>={shift};{clamp}'
                   f'little_write(result+{base}+lane*{element},(uint64_t)v,{element});}}'+_store(rd))
        return True

    # Crypto AES.
    if (w&0xffffcc00)==0x4e284800:
        opcode=(w>>12)&3;mnemonic=('aese','aesd','aesmc','aesimc')[opcode]
        _check(i,args,mnemonic,[('v',rd,'16b'),('v',rn,'16b')],Unsupported)
        if opcode<2:
            table=_table('box',SBOX if opcode==0 else INV_SBOX)
            source='(col+row)&3' if opcode==0 else '(col+4-row)&3'
            out.append(f'{{{table}uint8_t s[16],result[16];for(unsigned k=0;k<16;++k)s[k]=c->q[{rd}][k]^c->q[{rn}][k];'
                       f'for(unsigned col=0;col<4;++col)for(unsigned row=0;row<4;++row)result[row+4*col]=box[s[row+4*({source})]];'+_store(rd))
        else:
            xt='(uint8_t)(((X)<<1)^(((X)>>7)*0x1b))'
            def t(x):return xt.replace('X',x)
            if opcode==2:
                body=(f'uint8_t a=s[4*col+row],b=s[4*col+((row+1)&3)],d2=s[4*col+((row+2)&3)],d3=s[4*col+((row+3)&3)];'
                      f'result[4*col+row]=(uint8_t)({t("a")}^{t("b")}^b^d2^d3);')
            else:
                # InvMixColumns row r: 0e*a[r] ^ 0b*a[r+1] ^ 0d*a[r+2] ^ 09*a[r+3]
                body=('uint8_t m[4][4];for(unsigned k=0;k<4;++k){uint8_t x=s[4*col+((row+k)&3)],x2='+t('x')+',x4='+t('x2')+',x8='+t('x4')+';'
                      'm[k][0]=(uint8_t)(x8^x4^x2);m[k][1]=(uint8_t)(x8^x2^x);m[k][2]=(uint8_t)(x8^x4^x);m[k][3]=(uint8_t)(x8^x);}'
                      'result[4*col+row]=(uint8_t)(m[0][0]^m[1][1]^m[2][2]^m[3][3]);')
            out.append(f'{{uint8_t s[16],result[16];memcpy(s,c->q[{rn}],16);for(unsigned col=0;col<4;++col)for(unsigned row=0;row<4;++row){{{body}}}'+_store(rd))
        return True

    # Crypto three-register SHA.
    if (w&0xffe08c00)==0x5e000000 and ((w>>12)&7)!=7:
        opcode=(w>>12)&7
        mnemonic=('sha1c','sha1p','sha1m','sha1su0','sha256h','sha256h2','sha256su1')[opcode]
        load='uint32_t x[4],y[4],z[4];for(unsigned k=0;k<4;++k){'+f'x[k]=(uint32_t)little_read(c->q[{rd}]+4*k,4);y[k]=(uint32_t)little_read(c->q[{rn}]+4*k,4);z[k]=(uint32_t)little_read(c->q[{rm}]+4*k,4);}}'
        if opcode<3:
            _check(i,args,mnemonic,[('r',f'q{rd}'),('r',f's{rn}'),('v',rm,'4s')],Unsupported)
            f={0:'(((x[2]^x[3])&x[1])^x[3])',1:'(x[1]^x[2]^x[3])',2:'((x[1]&x[2])|((x[1]|x[2])&x[3]))'}[opcode]
            body=('uint32_t h=y[0];for(unsigned e=0;e<4;++e){uint32_t t='+f+';h=h+'+_rol('x[0]',5)+'+t+z[e];x[1]='+_rol('x[1]',30)+';'
                  'uint32_t top=x[3];x[3]=x[2];x[2]=x[1];x[1]=x[0];x[0]=h;h=top;}')
            result='x'
        elif opcode==3:
            _check(i,args,mnemonic,[('v',rd,'4s'),('v',rn,'4s'),('v',rm,'4s')],Unsupported)
            body='uint32_t r[4]={x[2],x[3],y[0],y[1]};for(unsigned k=0;k<4;++k)r[k]^=x[k]^z[k];'
            result='r'
        elif opcode in (4,5):
            _check(i,args,mnemonic,[('r',f'q{rd}'),('r',f'q{rn}'),('v',rm,'4s')],Unsupported)
            # SHA256hash(X,Y,W,part1): H uses X=Vd,Y=Vn -> X; H2 uses X=Vn,Y=Vd -> Y.
            X,Y=('x','y') if opcode==4 else ('y','x')
            body=('uint32_t*X='+X+',*Y='+Y+';for(unsigned e=0;e<4;++e){'
                  'uint32_t chs=((Y[1]^Y[2])&Y[0])^Y[2],maj=(X[0]&X[1])|((X[0]|X[1])&X[2]);'
                  'uint32_t t=Y[3]+('+_ror('Y[0]',6)+'^'+_ror('Y[0]',11)+'^'+_ror('Y[0]',25)+')+chs+z[e];'
                  'X[3]=t+X[3];Y[3]=t+('+_ror('X[0]',2)+'^'+_ror('X[0]',13)+'^'+_ror('X[0]',22)+')+maj;'
                  'uint32_t x3=X[3],y3=Y[3];X[3]=X[2];X[2]=X[1];X[1]=X[0];X[0]=y3;Y[3]=Y[2];Y[2]=Y[1];Y[1]=Y[0];Y[0]=x3;}')
            result='x'   # H returns X (=Vd); H2 returns Y (=Vd)
        else:
            _check(i,args,mnemonic,[('v',rd,'4s'),('v',rn,'4s'),('v',rm,'4s')],Unsupported)
            s1=lambda e:'('+_ror(e,17)+'^'+_ror(e,19)+'^(('+e+')>>10))'
            body=('uint32_t t0[4]={y[1],y[2],y[3],z[0]},r[4];'
                  'for(unsigned e=0;e<2;++e){uint32_t v=z[2+e];r[e]='+s1('v')+'+x[e]+t0[e];}'
                  'for(unsigned e=2;e<4;++e){uint32_t v=r[e-2];r[e]='+s1('v')+'+x[e]+t0[e];}')
            result='r'
        out.append('{'+load+body+'uint8_t result[16];for(unsigned k=0;k<4;++k)little_write(result+4*k,'+result+'[k],4);'+_store(rd))
        return True

    # Crypto two-register SHA.
    if (w&0xfffecc00)==0x5e280800 and ((w>>12)&31) in (0,1,2):
        opcode=(w>>12)&3;mnemonic=('sha1h','sha1su1','sha256su0')[opcode]
        if opcode==0:
            _check(i,args,mnemonic,[('r',f's{rd}'),('r',f's{rn}')],Unsupported)
            out.append(f'{{uint32_t a=(uint32_t)little_read(c->q[{rn}],4);uint8_t result[16]={{0}};little_write(result,{_rol("a",30)},4);'+_store(rd))
            return True
        _check(i,args,mnemonic,[('v',rd,'4s'),('v',rn,'4s')],Unsupported)
        load='uint32_t x[4],y[4],r[4];for(unsigned k=0;k<4;++k){'+f'x[k]=(uint32_t)little_read(c->q[{rd}]+4*k,4);y[k]=(uint32_t)little_read(c->q[{rn}]+4*k,4);}}'
        if opcode==1:
            body=('uint32_t t[4]={x[0]^y[1],x[1]^y[2],x[2]^y[3],x[3]};for(unsigned k=0;k<4;++k)r[k]='+_rol('t[k]',1)+';r[3]^='+_rol('t[0]',2)+';')
        else:
            body=('uint32_t t[4]={x[1],x[2],x[3],y[0]};for(unsigned k=0;k<4;++k)r[k]=('+_ror('t[k]',7)+'^'+_ror('t[k]',18)+'^(t[k]>>3))+x[k];')
        out.append('{'+load+body+'uint8_t result[16];for(unsigned k=0;k<4;++k)little_write(result+4*k,r[k],4);'+_store(rd))
        return True
    return _rare_integer(i,w,args,out,Unsupported)

def _widen(out,U,kind,element,Q,rd,rn,b_address):
    """Integer widening multiply (and accumulate) lanes: result lane = acc +/- a*b mod 2^(2*esize)."""
    bits=8*element;lanes=8//element
    product='(a*b)' if U else f'(uint64_t)({_sx("a",bits)}*{_sx("b",bits)})'
    acc={'mull':'','mlal':f'little_read(c->q[{rd}]+lane*{2*element},{2*element})+','mlsl':f'little_read(c->q[{rd}]+lane*{2*element},{2*element})-'}[kind]
    out.append(f'{{uint8_t result[16];for(unsigned lane=0;lane<{lanes};++lane){{uint64_t a=little_read(c->q[{rn}]+{8*Q}+lane*{element},{element}),b=little_read({b_address},{element});'
               f'little_write(result+lane*{2*element},{acc}{product},{2*element});}}'+_store(rd))

# ---- rarer integer forms (Arm ARM shared pseudocode: SatQ, SignedSatQ, UnsignedSatQ) ----
def _sat(value,bits,unsigned):
    """Clamp the __int128 lvalue `value` to an esize range, setting FPSR.QC on saturation."""
    lo,hi=(0,(1<<bits)-1) if unsigned else (-(1<<(bits-1)),(1<<(bits-1))-1)
    L=f'((__int128)INT64_C({lo}))' if lo>-(1<<63) else '(-((__int128)1<<63))'
    H=f'((__int128)UINT64_C({hi}))'
    return f'if({value}<{L}){{{value}={L};{QC}}}else if({value}>{H}){{{value}={H};{QC}}}'

def _ext(v,bits,signed):
    """Int(element, unsigned) as __int128."""
    return f'((__int128){_sx(v,bits)})' if signed else f'((__int128)({v}))'

def _rare_integer(i,w,args,out,Unsupported):
    rd,rn,rm=w&31,(w>>5)&31,(w>>16)&31
    Q,U=(w>>30)&1,(w>>29)&1;size=(w>>22)&3
    vector3=(w&0x9f200400)==0x0e200400;scalar3=(w&0xdf200400)==0x5e200400
    opcode3=(w>>11)&31

    # Three same (vector / scalar): SQDMULH/SQRDMULH, MLA/MLS, saturating add/sub, saturating shifts.
    if (vector3 or scalar3) and (opcode3 in (1,5,9,11) or (opcode3==22 and size in (1,2)) or (vector3 and opcode3==18)):
        if vector3 and opcode3 in (1,5):return False   # vector SQADD/UQADD/SQSUB/UQSUB: owned by emit()
        if opcode3==18 and size==3:raise Unsupported('MLA/MLS size')
        if vector3 and size==3 and not Q:raise Unsupported('Reserved .1d arrangement')
        bits=8<<size;element=bits//8
        name={1:('sqadd','uqadd'),5:('sqsub','uqsub'),9:('sqshl','uqshl'),11:('sqrshl','uqrshl'),22:('sqdmulh','sqrdmulh'),18:('mla','mls')}[opcode3][U]
        if vector3:
            arr=_arr(size,Q);_check(i,args,name,[('v',rd,arr),('v',rn,arr),('v',rm,arr)],Unsupported);total=16 if Q else 8
        else:
            p='bhsd'[size];_check(i,args,name,[('r',f'{p}{rd}'),('r',f'{p}{rn}'),('r',f'{p}{rm}')],Unsupported);total=element
        unsigned=name.startswith('u')
        if opcode3==18:
            op='-' if U else '+'
            body=f'little_write(result+off,little_read(c->q[{rd}]+off,{element}){op}a*b,{element});'
        elif opcode3 in (1,5):
            op='+' if opcode3==1 else '-'
            body=f'__int128 v={_ext("a",bits,not unsigned)}{op}{_ext("b",bits,not unsigned)};{_sat("v",bits,unsigned)}little_write(result+off,(uint64_t)v,{element});'
        elif opcode3==22:
            rounding=f'+((__int128)1<<{bits-1})' if U else ''
            body=(f'__int128 v=(2*{_ext("a",bits,True)}*{_ext("b",bits,True)}{rounding})>>{bits};'
                  f'{_sat("v",bits,False)}little_write(result+off,(uint64_t)v,{element});')
        else:
            rounding=opcode3==11
            # shift = SInt(element2<7:0>); element = (Int(element1) + round_const) << shift; SatQ.
            # A nonzero element shifted left by >= esize always saturates (stand-in magnitude 2^100).
            body=(f'int shift=(int)(int8_t)(uint8_t)b;__int128 x={_ext("a",bits,not unsigned)},v;'
                  f'if(shift>=0)v=(x==0)?0:(shift>={bits}?(x<0?-((__int128)1<<100):((__int128)1<<100)):x*((__int128)1<<shift));'
                  +(f'else if(shift<-100)v=0;else v=(x+((__int128)1<<(-shift-1)))>>(-shift);' if rounding else
                    f'else if(shift<-100)v=x<0?-1:0;else v=x>>(-shift);')+
                  f'{_sat("v",bits,unsigned)}little_write(result+off,(uint64_t)v,{element});')
        out.append(f'{{uint8_t result[16]={{0}};for(unsigned off=0;off<{total};off+={element}){{uint64_t a=little_read(c->q[{rn}]+off,{element}),b=little_read(c->q[{rm}]+off,{element});(void)b;{body}}}'+_store(rd))
        return True

    # By element (vector / scalar): SQDMULH (opcode 1100), SQRDMULH (opcode 1101).
    velem=(w&0x9f000400)==0x0f000000;selem=(w&0xdf000400)==0x5f000000
    if (velem or selem) and not U and ((w>>12)&15) in (12,13):
        L,M,H=(w>>21)&1,(w>>20)&1,(w>>11)&1
        if size==1:index=(H<<2)|(L<<1)|M;m=rm&15
        elif size==2:index=(H<<1)|L;m=rm
        else:raise Unsupported('SQDMULH by element size')
        rounding=((w>>12)&15)==13;name='sqrdmulh' if rounding else 'sqdmulh';bits=8<<size;element=bits//8
        lane='hs'[size-1]
        if velem:
            arr=_arr(size,Q);_check(i,args,name,[('v',rd,arr),('v',rn,arr),('e',m,lane,index)],Unsupported);total=16 if Q else 8
        else:
            _check(i,args,name,[('r',f'{lane}{rd}'),('r',f'{lane}{rn}'),('e',m,lane,index)],Unsupported);total=element
        rc=f'+((__int128)1<<{bits-1})' if rounding else ''
        out.append(f'{{uint8_t result[16]={{0}};uint64_t b=little_read(c->q[{m}]+{index*element},{element});for(unsigned off=0;off<{total};off+={element}){{uint64_t a=little_read(c->q[{rn}]+off,{element});'
                   f'__int128 v=(2*{_ext("a",bits,True)}*{_ext("b",bits,True)}{rc})>>{bits};{_sat("v",bits,False)}little_write(result+off,(uint64_t)v,{element});}}'+_store(rd))
        return True

    # Three different: [SU]ADDL, [SU]SUBL, [SU]ABAL, [SU]ABDL, PMULL (and the "2" upper-half forms).
    if (w&0x9f200c00)==0x0e200000 and ((w>>12)&15) in (0,2,5,7,14):
        opcode=(w>>12)&15
        if opcode==14:
            if U or size not in (0,3):raise Unsupported('PMULL size')
            name='pmull'+('2' if Q else '')
            if size==0:
                _check(i,args,name,[('v',rd,'8h'),('v',rn,_arr(0,Q)),('v',rm,_arr(0,Q))],Unsupported)
                out.append(f'{{uint8_t result[16];for(unsigned lane=0;lane<8;++lane){{uint32_t a=c->q[{rn}][{8*Q}+lane],b=c->q[{rm}][{8*Q}+lane],r=0;'
                           'for(unsigned k=0;k<8;++k)if((b>>k)&1)r^=a<<k;little_write(result+2*lane,r,2);}'+_store(rd))
            else:
                _check(i,args,name,[('v',rd,'1q'),('v',rn,'2d' if Q else '1d'),('v',rm,'2d' if Q else '1d')],Unsupported)
                out.append(f'{{uint8_t result[16];uint64_t a=little_read(c->q[{rn}]+{8*Q},8),b=little_read(c->q[{rm}]+{8*Q},8),lo=0,hi=0;'
                           'for(unsigned k=0;k<64;++k)if((b>>k)&1){lo^=a<<k;if(k)hi^=a>>(64-k);}little_write(result,lo,8);little_write(result+8,hi,8);'+_store(rd))
            return True
        if size==3:raise Unsupported('Widening arithmetic size')
        name=('u' if U else 's')+{0:'addl',2:'subl',5:'abal',7:'abdl'}[opcode]+('2' if Q else '')
        element=1<<size;bits=8*element;src=_arr(size,Q);dst=_arr(size+1,1)
        _check(i,args,name,[('v',rd,dst),('v',rn,src),('v',rm,src)],Unsupported)
        x,y=_ext('a',bits,not U),_ext('b',bits,not U)
        value={0:f'{x}+{y}',2:f'{x}-{y}',5:f'({x}>{y}?{x}-{y}:{y}-{x})',7:f'({x}>{y}?{x}-{y}:{y}-{x})'}[opcode]
        acc=f'little_read(c->q[{rd}]+lane*{2*element},{2*element})+' if opcode==5 else ''
        out.append(f'{{uint8_t result[16];for(unsigned lane=0;lane<{8//element};++lane){{uint64_t a=little_read(c->q[{rn}]+{8*Q}+lane*{element},{element}),b=little_read(c->q[{rm}]+{8*Q}+lane*{element},{element});'
                   f'little_write(result+lane*{2*element},{acc}(uint64_t)({value}),{2*element});}}'+_store(rd))
        return True

    # Permute: UZP1/UZP2.
    if (w&0xbf208c00)==0x0e000800 and ((w>>12)&7) in (1,5):
        if size==3 and not Q:raise Unsupported('Reserved .1d arrangement')
        part=((w>>12)&7)==5;name='uzp2' if part else 'uzp1';arr=_arr(size,Q)
        _check(i,args,name,[('v',rd,arr),('v',rn,arr),('v',rm,arr)],Unsupported)
        element=1<<size;total=16 if Q else 8;lanes=total//element
        out.append(f'{{uint8_t concat[32],result[16]={{0}};memcpy(concat,c->q[{rn}],{total});memcpy(concat+{total},c->q[{rm}],{total});'
                   f'for(unsigned lane=0;lane<{lanes};++lane)memcpy(result+lane*{element},concat+(2*lane+{int(part)})*{element},{element});'+_store(rd))
        return True

    # Copy: SMOV.
    if (w&0xbfe0fc00)==0x0e002c00:
        imm5=(w>>16)&31;element=imm5&-imm5
        if element not in (1,2,4) or (element==4 and not Q):raise Unsupported('SMOV element size')
        index=imm5//(2*element);lane='bhs'[element.bit_length()-1]
        from translate_startup import regname
        if rd==31:raise Unsupported('SMOV zero-register destination')
        dname=('x' if Q else 'w')+str(rd)
        _check(i,args,'smov',[('r',dname),('e',rn,lane,index)],Unsupported)
        dst,width=regname(dname)
        out.append(f'{dst}=(uint{width}_t){_sx(f"little_read(c->q[{rn}]+{index*element},{element})",8*element)};')
        return True

    # Across lanes: SADDLV (all), UADDLV (halfword and word elements; bytes are owned by emit()).
    if (w&0x9f3e0c00)==0x0e300800 and ((w>>12)&31)==3 and (not U or size in (1,2)):
        if size==3 or (size==2 and not Q):raise Unsupported('Long add across lanes arrangement')
        name='uaddlv' if U else 'saddlv';element=1<<size;bits=8*element;total=16 if Q else 8
        _check(i,args,name,[('r','hsd'[size]+str(rd)),('v',rn,_arr(size,Q))],Unsupported)
        out.append(f'{{uint64_t sum=0;for(unsigned off=0;off<{total};off+={element}){{uint64_t a=little_read(c->q[{rn}]+off,{element});sum+=(uint64_t){_ext("a",bits,not U)};}}'
                   f'memset(c->q[{rd}],0,16);little_write(c->q[{rd}],sum,{2*element});}}')
        return True

    # Shift by immediate: SRSRA/URSRA (vector, and scalar D).
    vshift=(w&0x9f800400)==0x0f000400;sshift=(w&0xdf800400)==0x5f000400
    if (vshift or sshift) and (w>>19)&15 and ((w>>11)&31)==6:
        immh=(w>>19)&15;immb=(w>>16)&7
        if sshift and not immh&8:raise Unsupported('Scalar rounding shift-accumulate size')
        if vshift and immh&8 and not Q:raise Unsupported('Reserved .1d arrangement')
        size=immh.bit_length()-1;element=1<<size;bits=8*element;shift=2*bits-((immh<<3)|immb)
        name='ursra' if U else 'srsra'
        if vshift:
            arr=_arr(size,Q);_check(i,args,name,[('v',rd,arr),('v',rn,arr),('imm',shift,0)],Unsupported);total=16 if Q else 8
        else:
            _check(i,args,name,[('r',f'd{rd}'),('r',f'd{rn}'),('imm',shift,0)],Unsupported);total=8
        out.append(f'{{uint8_t result[16]={{0}};for(unsigned off=0;off<{total};off+={element}){{uint64_t a=little_read(c->q[{rn}]+off,{element});'
                   f'__int128 v=({_ext("a",bits,not U)}+((__int128)1<<{shift-1}))>>{shift};'
                   f'little_write(result+off,little_read(c->q[{rd}]+off,{element})+(uint64_t)v,{element});}}'+_store(rd))
        return True

    # Two-register misc (vector / scalar): SQABS/SQNEG, saturating narrows.
    vmisc=(w&0x9f3e0c00)==0x0e200800;smisc=(w&0xdf3e0c00)==0x5e200800;opcode=(w>>12)&31
    if (vmisc or smisc) and (opcode==7 or (smisc and (opcode==20 or (opcode==18 and U))) or (vmisc and opcode==20 and U)):
        if opcode==7:
            if vmisc and size==3 and not Q:raise Unsupported('Reserved .1d arrangement')
            name='sqneg' if U else 'sqabs';bits=8<<size;element=bits//8
            if vmisc:
                arr=_arr(size,Q);_check(i,args,name,[('v',rd,arr),('v',rn,arr)],Unsupported);total=16 if Q else 8
            else:
                p='bhsd'[size];_check(i,args,name,[('r',f'{p}{rd}'),('r',f'{p}{rn}')],Unsupported);total=element
            value='-x' if U else '(x<0?-x:x)'
            out.append(f'{{uint8_t result[16]={{0}};for(unsigned off=0;off<{total};off+={element}){{uint64_t a=little_read(c->q[{rn}]+off,{element});'
                       f'__int128 x={_ext("a",bits,True)},v={value};{_sat("v",bits,False)}little_write(result+off,(uint64_t)v,{element});}}'+_store(rd))
            return True
        if size==3:raise Unsupported('Saturating narrow size')
        name={(20,0):'sqxtn',(20,1):'uqxtn',(18,1):'sqxtun'}[(opcode,U)];bits=8<<size;element=bits//8
        signed_source=name!='uqxtn';unsigned_result=name!='sqxtn'
        if vmisc:
            name+='2' if Q else ''
            _check(i,args,name,[('v',rd,_arr(size,Q)),('v',rn,_arr(size+1,1))],Unsupported);lanes=8//element;base=8*Q;keep=Q
        else:
            _check(i,args,name,[('r','bhs'[size]+str(rd)),('r','hsd'[size]+str(rn))],Unsupported);lanes=1;base=0;keep=0
        out.append('{uint8_t result[16];'+(f'memcpy(result,c->q[{rd}],16);' if keep else 'memset(result,0,16);')+
                   f'for(unsigned lane=0;lane<{lanes};++lane){{uint64_t a=little_read(c->q[{rn}]+lane*{2*element},{2*element});__int128 v={_ext("a",2*bits,signed_source)};'
                   f'{_sat("v",bits,unsigned_result)}little_write(result+{base}+lane*{element},(uint64_t)v,{element});}}'+_store(rd))
        return True
    return False
