"""Static ARM64 startup-to-SVC translation. No runtime opcode decoder.

Direct edges become C gotos. A finite native label registry implements only
the observed indirect jump-table/RET targets; unknown targets stop explicitly.
"""
import hashlib,json,re,struct
from pathlib import Path
from service_profile import write_service_profile
from partition_native import partition
from capstone import Cs,CS_ARCH_ARM64,CS_MODE_ARM
from capstone import arm64 as A64
from capstone.arm64 import (ARM64_OP_REG,ARM64_OP_IMM,ARM64_OP_MEM,ARM64_SFT_INVALID,
                           ARM64_SFT_LSL,ARM64_SFT_LSR,ARM64_SFT_ASR,ARM64_SFT_ROR,ARM64_EXT_INVALID,ARM64_EXT_UXTB,ARM64_EXT_UXTH,ARM64_EXT_UXTW,ARM64_EXT_SXTW)

ROOT=Path(__file__).resolve().parent
MAIN_EXPECTED='529c01b70ac0ecc4c6c7aed73be7f998d5e3680a5b657712409cb1bb1980133b'
MAIN_RANGES=json.loads((ROOT/'discovered-main-ranges.json').read_text()) if (ROOT/'discovered-main-ranges.json').exists() else []
EXPECTED='8ebd0cfabdaf5221cf0548b6e0ffb619d97bf52a1a8069ef86fedddbd6c860aa'
RANGES=[(0,0xd8),(0xd8,0x10c),(0x4f0,0x7cc),(0x7d0,0x854),
        (0x860,0x13d8),(0x13e0,0x14a0),(0x14b4,0x14d0),
        (0x14d4,0x14e0),(0x14f0,0x15c0),(0x16c0,0x178c),(0x1790,0x17c0)]
if (ROOT/'discovered-rtld-ranges.json').exists():
    RANGES.extend(json.loads((ROOT/'discovered-rtld-ranges.json').read_text()))
LITERALS={4,0x18,0x2c,0x4c,0x60,0x98,0xac,0xc0,0xdc,0x13b8}
SDK_EXPECTED='8501ee7f9d3ce3654171899e25e9b913e4d9001ca43b3c03d0da6b2751b640f0'
SUBSDK_EXPECTED='69d204488f84d269f22d75c8954c2036dca13697b2a8b9085824158e92a56c47'
SUBSDK_RANGES=[]
if (ROOT/'discovered-subsdk0-ranges.json').exists():
    SUBSDK_RANGES.extend(json.loads((ROOT/'discovered-subsdk0-ranges.json').read_text()))
SDK_RANGES=[(0x158334,0x15835c),(0x158304,0x158314),(0x1584cc,0x1584dc),(0x1583ec,0x1583f4),(0x158454,0x15845c),(0xfa060,0xfa0c4),(0xfa0d0,0xfa0d4),(0xfa0e0,0xfa0e4),
            (0x1582dc,0x1582f4),
            (0x158384,0x1583ac),
            (0x1583f4,0x15841c),
            (0x5c3030,0x5c30c0),(0x5bd480,0x5bd4a0),(0x56f470,0x56f474),
            (0x131210,0x13124c),(0x5c4dc0,0x5c4dd0),
            (0x131420,0x131530),(0x1318d8,0x131948),(0x1584ac,0x1584c4),
            (0x5c4e20,0x5c4e40),(0x5c40f0,0x5c4100),(0x131724,0x13172c),
            (0x82198,0x826b4),(0x5c4e90,0x5c4ea0),(0x131530,0x1315f4),
            (0x5bfaf0,0x5bfb00),(0x5c4e40,0x5c4e90),(0x1315f4,0x1316b0),
            (0x12dc24,0x12dcf0),(0x12a9e4,0x12aaa8),(0x12d178,0x12d1e0),
            (0x5c4a00,0x5c4a10),(0x5c4100,0x5c4120),(0x5c4130,0x5c4140),
            (0x12fdc8,0x12fe88),(0x158314,0x15832c),(0x158364,0x15837c),(0x15847c,0x158494),
            (0x5c4db0,0x5c4dc0),(0x12a0c0,0x12a1a8),(0x5bd5a0,0x5bd5b0),
            (0x587580,0x587650),(0x131094,0x13109c),(0x131734,0x13186c),
            (0x15841c,0x15842c),(0x12df88,0x12e00c),(0x5c0650,0x5c0660),
            (0x13124c,0x131274),(0x12ba70,0x12ba74),(0x5c3eb0,0x5c3ec0),(0x5c4a50,0x5c4a60),
            (0x127fa0,0x127fe0),(0x12f410,0x12f4cc),(0x1313c4,0x131420),
            (0x5c4e00,0x5c4e20),(0x12c770,0x12c7d0),(0xfa000,0xfa03c),(0xfa040,0xfa044),(0xfa050,0xfa054),
            (0x56f480,0x56f498),(0x56f4a0,0x56f4a4),(0x5bbc50,0x5bbc68),(0x5bbc90,0x5bbc94),
            (0x56f540,0x56f55c),(0x5c2f90,0x5c3030)]

class Unsupported(ValueError):pass
CONDITIONS={'eq':'c->z','ne':'!c->z','hs':'c->carry','lo':'!c->carry',
            'mi':'c->n','pl':'!c->n','vs':'c->v','vc':'!c->v',
            'hi':'c->carry && !c->z','ls':'!c->carry || c->z',
            'ge':'c->n==c->v','lt':'c->n!=c->v',
            'gt':'!c->z && c->n==c->v','le':'c->z || c->n!=c->v'}

# Auditable ranges discovered from native stopping targets. Every instruction
# still passes the same strict emitter and pinned-input checks.
if (ROOT/'discovered-sdk-ranges.json').exists():
    SDK_RANGES.extend(json.loads((ROOT/'discovered-sdk-ranges.json').read_text()))

def regname(name):
    name={'fp':'x29','lr':'x30'}.get(name,name)
    if name=='sp':return 'c->sp',64
    if name in ('xzr','wzr'):return 'UINT64_C(0)',64 if name[0]=='x' else 32
    m=re.fullmatch(r'([wx])([0-9]|[12][0-9]|30)',name)
    if not m:raise Unsupported(f'Unsupported register {name}')
    return f'c->x[{int(m[2])}]',32 if m[1]=='w' else 64

VECTOR_INT_OPS=frozenset(('smax','smin','umax','umin','cmgt','cmge','cmhs','cmle','cmlt','mul','shadd','shsub','uhadd','uhsub',
    'sqadd','uqadd','sqsub','uqsub','sabd','uabd','smaxp','sminp','umaxp','uminp','neg','abs','sshr','ushr','srshr','urshr',
    'ushll','ushll2','uxtl','uxtl2','sxtl','sxtl2','shll','shll2','uaddw','uaddw2','saddw','saddw2','usubw','usubw2','ssubw','ssubw2',
    'smaxv','sminv','umaxv','uminv'))

def emit(i):
    op=i.mnemonic;args=i.operands;pc=f'(boot_address(c,UINT64_C({i.address})))'
    out=[];next_address=i.address+4;edges=[next_address];indirect=False
    def register(o,dest=False):
        if o.type!=ARM64_OP_REG:raise Unsupported('Expected register')
        expr,width=regname(i.reg_name(o.reg))
        if dest and expr.startswith('UINT'):raise Unsupported('Zero-register destination')
        return (expr if dest or width==64 else f'(uint32_t){expr}'),width
    def operand(o,width,allow_shift=True):
        extensions={A64.ARM64_EXT_UXTB:(8,False),A64.ARM64_EXT_UXTH:(16,False),
                    A64.ARM64_EXT_UXTW:(32,False),A64.ARM64_EXT_UXTX:(64,False),
                    A64.ARM64_EXT_SXTB:(8,True),A64.ARM64_EXT_SXTH:(16,True),
                    A64.ARM64_EXT_SXTW:(32,True),A64.ARM64_EXT_SXTX:(64,True)}
        if o.ext!=ARM64_EXT_INVALID and o.ext not in extensions:raise Unsupported('Extended operand unsupported')
        if o.type==ARM64_OP_IMM:expr=f'UINT64_C(0x{o.imm&((1<<width)-1):x})'
        else:
            expr,w=register(o)
            if o.ext in extensions:
                bits,signed=extensions[o.ext]
                expected_width=64 if width==64 and bits==64 else 32
                if w!=expected_width or o.shift.type not in (ARM64_SFT_INVALID,ARM64_SFT_LSL) or o.shift.value>4:raise Unsupported('Extended arithmetic form')
                bits=min(bits,width)
                expr=f'boot_sign_extend(({expr}),{bits})' if signed else f'(({expr})&UINT64_C({(1<<bits)-1}))'
            elif w!=width:raise Unsupported('Mixed width')
        if o.shift.type!=ARM64_SFT_INVALID:
            if not allow_shift or not 0<=o.shift.value<width:raise Unsupported('Invalid shift')
            if o.shift.type not in (ARM64_SFT_LSL,ARM64_SFT_LSR,ARM64_SFT_ASR,ARM64_SFT_ROR):raise Unsupported('Unsupported shift')
            if o.shift.type==ARM64_SFT_ROR:expr=f'((({expr}) >> {o.shift.value}) | (({expr}) << {(width-o.shift.value)&(width-1)}))'
            elif o.shift.type==ARM64_SFT_ASR:expr=f'boot_sign_extend(({expr}) >> {o.shift.value},{width-o.shift.value})'
            else:
                sign='<<' if o.shift.type==ARM64_SFT_LSL else '>>'
                expr=f'(({expr}) {sign} {o.shift.value})'
        return expr
    def assignment(dst,width,expr):out.append(f'{dst}=(uint{width}_t)({expr});')
    from float_emit_ext import emit_float_ext
    from simd_mem_ext import emit_simd_mem
    from simd_arith_ext import emit_simd_arith_ext
    if emit_simd_mem(i,pc,out,Unsupported):pass
    elif emit_simd_arith_ext(i,op,args,pc,out,Unsupported):pass
    elif emit_float_ext(i,op,args,pc,out,Unsupported):pass
    elif op in ('ldr','ldrsw') and len(args)==2 and args[1].type==ARM64_OP_IMM:
        name=i.reg_name(args[0].reg);delta=(args[1].imm-i.address)&((1<<64)-1)
        address=f'({pc}+UINT64_C(0x{delta:x}))'
        if op=='ldr' and re.fullmatch(r'[sdq](?:[0-9]|[12][0-9]|3[01])',name):
            size={'s':4,'d':8,'q':16}[name[0]];q=f'c->q[{int(name[1:])}]'
            out.append(f'{{uint8_t*p=boot_access_read(c,{address},{size},{pc});if(!p)return c->status;BOOT_QZP({q},p,{size});}}')
        else:
            dst,width=register(args[0],name not in ('xzr','wzr'));size=4 if op=='ldrsw' else width//8
            out.append(f'{{uint64_t value;if(!boot_load(c,{address},{size},{pc},&value))return c->status;')
            if name not in ('xzr','wzr'):out.append(f'{dst}='+('boot_sign_extend(value,32)' if op=='ldrsw' else f'(uint{width}_t)value')+';')
            out.append('}')
    elif op in ('ldr','ldur','ldrb','ldrh','ldurb','ldurh','ldrsb','ldursb','ldrsh','ldursh','ldrsw','ldursw','str','stur','strb','strh','sturb','sturh','ldp','ldpsw','stp'):
        pair=op in ('ldp','ldpsw','stp');load=op.startswith('ld');memindex=2 if pair else 1
        if len(args) not in (memindex+1,memindex+2) or args[memindex].type!=ARM64_OP_MEM:
            raise Unsupported('Memory operand shape')
        memop=args[memindex];mem=memop.mem
        basename=i.reg_name(mem.base);base,bwidth=regname(basename)
        if bwidth!=64 or base.startswith('UINT'):raise Unsupported('Invalid memory base')
        if basename=='sp':out.append(f'if(!boot_sp(c,{pc}))return c->status;')
        offset=f'UINT64_C(0x{mem.disp&((1<<64)-1):x})'
        if mem.index:
            indexexpr,iw=regname(i.reg_name(mem.index))
            if iw==32 and memop.ext==ARM64_EXT_UXTW:indexexpr=f'(uint64_t)(uint32_t){indexexpr}'
            elif iw==32 and memop.ext==ARM64_EXT_SXTW:indexexpr=f'boot_sign_extend({indexexpr},32)'
            elif iw!=64 or memop.ext!=ARM64_EXT_INVALID:raise Unsupported('Unsupported memory index extension')
            if memop.shift.type not in (ARM64_SFT_INVALID,ARM64_SFT_LSL):raise Unsupported('Memory shift')
            if memop.shift.value>=64:raise Unsupported('Invalid memory shift')
            offset=f'({offset}+({indexexpr} << {memop.shift.value}))'
        elif memop.shift.type!=ARM64_SFT_INVALID or memop.ext!=ARM64_EXT_INVALID:
            raise Unsupported('Unexpected memory modifier')
        post=len(args)==memindex+2
        if post and (not i.writeback or args[-1].type!=ARM64_OP_IMM):raise Unsupported('Invalid postindex')
        if i.writeback and mem.index:raise Unsupported('Indexed writeback unsupported')
        names=[i.reg_name(o.reg) for o in args[:memindex]]
        if not pair and op in ('ldr','ldur','str','stur') and re.fullmatch(r'[bhsdq](?:[0-9]|[12][0-9]|3[01])',names[0]):
            size={'b':1,'h':2,'s':4,'d':8,'q':16}[names[0][0]]
            q=f'c->q[{int(names[0][1:])}]'
            out.append(f'address={base}+{offset};')
            if i.writeback:
                change=args[-1].imm if post else mem.disp
                out.append(f'writeback={base}+UINT64_C(0x{change&((1<<64)-1):x});')
            if load:
                out.append(f'{{uint8_t*p=boot_access_read(c,address,{size},{pc});if(!p)return c->status;')
                out.append(f'BOOT_QZP({q},p,{size});')
                out.append('}')
            else:
                out.append(f'if(!boot_store_bytes(c,address,{size},{pc},{q}))return c->status;')
            if i.writeback:out.append(f'{base}=writeback;')
            out.append(f'goto L_{next_address:x};')
            return out,edges,False
        if pair and all(re.fullmatch(r'[sdq](?:[0-9]|[12][0-9]|3[01])',n) for n in names):
            if mem.index:raise Unsupported('Vector pair index unsupported')
            if names[0][0]!=names[1][0]:raise Unsupported('Mixed vector pair width')
            if load and names[0]==names[1]:raise Unsupported('Aliased vector pair destinations')
            size={'s':4,'d':8,'q':16}[names[0][0]]
            out.append(f'address={base}+{offset};')
            if i.writeback:
                change=args[-1].imm if post else mem.disp
                out.append(f'writeback={base}+UINT64_C(0x{change&((1<<64)-1):x});')
            if load:out.append(f'{{uint8_t*p=boot_access_read(c,address,{size*2},{pc});if(!p)return c->status;')
            else:out.append(f'{{uint8_t p[{size*2}];')
            for index,name in enumerate(names):
                q=f'c->q[{int(name[1:])}]';p=f'p+{index*size}'
                if load:out.append(f'memset({q},0,16);')
                out.append(f'memcpy({q if load else p},{p if load else q},{size});')
            if not load:out.append(f'if(!boot_store_bytes(c,address,{size*2},{pc},p))return c->status;')
            out.append('}')
            if i.writeback:out.append(f'{base}=writeback;')
            out.append(f'goto L_{next_address:x};')
            return out,edges,False
        first,width=register(args[0],load and names[0] not in ('xzr','wzr'))
        if load and names[0] in ('xzr','wzr'):first='discard0'
        second=None
        if pair:
            second,w=register(args[1],load and names[1] not in ('xzr','wzr'))
            if load and names[1] in ('xzr','wzr'):second='discard1'
            if w!=width:raise Unsupported('Mixed pair width')
        if op=='ldpsw' and (width!=64 or names[0]==names[1]):raise Unsupported('LDPSW distinct 64-bit destinations')
        widthbytes=1 if op in ('ldrb','ldurb','ldrsb','ldursb','strb','sturb') else 2 if op in ('ldrh','ldurh','ldrsh','ldursh','strh','sturh') else 4 if op in ('ldrsw','ldursw','ldpsw') else width//8
        # Compute address once before register destinations can overwrite a base.
        out.append(f'address={base}+{offset};')
        if i.writeback:
            if load and (base==first or base==second):raise Unsupported('Writeback aliases load destination')
            change=args[-1].imm if post else mem.disp
            out.append(f'writeback={base}+UINT64_C(0x{change&((1<<64)-1):x});')
        if pair:
            fn='boot_load_pair' if load else 'boot_store_pair'
            a='&'+first if load else first;b='&'+second if load else second
            out.append(f'if(!{fn}(c,address,{widthbytes},{pc},{a},{b}))return c->status;')
            if op=='ldpsw':out.append(f'{first}=boot_sign_extend({first},32);{second}=boot_sign_extend({second},32);')
        else:
            fn='boot_load' if load else 'boot_store';arg='&'+first if load else first
            out.append(f'if(!{fn}(c,address,{widthbytes},{pc},{arg}))return c->status;')
            if op in ('ldrsw','ldursw'):out.append(f'{first}=boot_sign_extend({first},32);')
            if op in ('ldrsb','ldursb'):out.append(f'{first}=(uint{width}_t)boot_sign_extend({first},8);')
            if op in ('ldrsh','ldursh'):out.append(f'{first}=(uint{width}_t)boot_sign_extend({first},16);')
        if i.writeback:out.append(f'{base}=writeback;')
    elif i.writeback and op not in ('ld2','st2'):raise Unsupported('Unexpected writeback')
    elif op=='fcsel':
        names=[i.reg_name(o.reg) if o.type==ARM64_OP_REG else '' for o in args]
        condition=i.op_str.rsplit(', ',1)[-1]
        if len(names)!=3 or not all(re.fullmatch(r'[sd](?:[0-9]|[12][0-9]|3[01])',n) for n in names) or len({n[0] for n in names})!=1:
            raise Unsupported('FCSEL matching scalar S/D registers only')
        predicate='1' if condition in ('al','nv') else CONDITIONS.get(condition)
        if predicate is None:raise Unsupported('FCSEL condition')
        dst,left,right=[int(n[1:]) for n in names];size=4 if names[0][0]=='s' else 8
        out.append(f'{{uint8_t selected[{size}];memcpy(selected,({predicate})?c->q[{left}]:c->q[{right}],{size});memset(c->q[{dst}],0,16);memcpy(c->q[{dst}],selected,{size});}}')
    elif op in ('fmaxnmv','fminnmv'):
        # Reduce(op, 4 lanes) = op(op(e0,e1), op(e2,e3)) with the FMAXNM/FMINNM rules below.
        if len(args)!=2 or args[0].type!=ARM64_OP_REG or args[1].type!=ARM64_OP_REG or args[1].vector_index!=-1 or args[1].vas!=A64.ARM64_VAS_4S:raise Unsupported('FMAXNMV/FMINNMV 4S only')
        dname,sname=i.reg_name(args[0].reg),i.reg_name(args[1].reg)
        if not re.fullmatch(r's(?:[0-9]|[12][0-9]|3[01])',dname) or not re.fullmatch(r'v(?:[0-9]|[12][0-9]|3[01])',sname):raise Unsupported('FMAXNMV/FMINNMV registers')
        dst,src=int(dname[1:]),int(sname[1:]);mx=op=='fmaxnmv'
        from float_emit import flush_inputs
        out.append(f'{{if(c->fpcr&~UINT32_C(0x3000000)){{boot_fault(c,BOOT_FP_SCOPE,{pc},0,4);return c->status;}}uint32_t e[4],r;for(unsigned lane=0;lane<4;++lane)memcpy(&e[lane],c->q[{src}]+lane*4,4);')
        out.append('for(unsigned lane=0;lane<4;++lane){uint32_t a=e[lane];'+flush_inputs(32,'a')+'e[lane]=a;}')
        combine=('{uint32_t am=a&0x7fffffff,bm=b&0x7fffffff;int an=am>0x7f800000,bn=bm>0x7f800000;int as=an && !(a&0x400000),bs=bn && !(b&0x400000);'
                 'if(as || bs){r=(c->fpcr&0x2000000)?0x7fc00000:((as?a:b)|0x400000);c->fpsr|=1;}else if(an && bn)r=(c->fpcr&0x2000000)?0x7fc00000:a;else if(an)r=b;else if(bn)r=a;'
                 f'else if(!am && !bm)r=(a{"&" if mx else "|"}b)&0x80000000;'
                 f'else {{int less=((a^b)>>31)?(a>>31):((a>>31)?am>bm:am<bm);r=less?{"b:a" if mx else "a:b"};}}}}')
        out.append('uint32_t lo,hi;{uint32_t a=e[0],b=e[1];'+combine+'lo=r;}{uint32_t a=e[2],b=e[3];'+combine+'hi=r;}{uint32_t a=lo,b=hi;'+combine+'}')
        out.append(f'BOOT_QZ(c->q[{dst}],r,4);}}')
    elif op in ('fmaxnm','fminnm'):
        names=[i.reg_name(o.reg) if o.type==ARM64_OP_REG else '' for o in args]
        if len(names)!=3 or not all(re.fullmatch(r'[sd](?:[0-9]|[12][0-9]|3[01])',n) for n in names) or len({n[0] for n in names})!=1:
            raise Unsupported('FMAXNM/FMINNM matching scalar S/D registers only')
        dst,left,right=[int(n[1:]) for n in names];width=32 if names[0][0]=='s' else 64;size=width//8
        magnitude='UINT64_C(0x7fffffffffffffff)' if width==64 else '0x7fffffff'
        infinity='UINT64_C(0x7ff0000000000000)' if width==64 else '0x7f800000'
        quiet='UINT64_C(0x8000000000000)' if width==64 else '0x400000'
        sign='UINT64_C(0x8000000000000000)' if width==64 else '0x80000000'
        from float_emit import flush_inputs
        out.append(f'{{uint{width}_t a,b,r;memcpy(&a,c->q[{left}],{size});memcpy(&b,c->q[{right}],{size});')
        out.append(f'if(c->fpcr&~UINT32_C(0x3000000)){{boot_fault(c,BOOT_FP_SCOPE,{pc},0,{size});return c->status;}}')
        out.append(flush_inputs(width,'a','b'))
        default_nan='UINT64_C(0x7ff8000000000000)' if width==64 else '0x7fc00000'
        out.extend([f'uint{width}_t am=a&{magnitude},bm=b&{magnitude};int an=am>{infinity},bn=bm>{infinity};',
                    f'int as=an && !(a&{quiet}),bs=bn && !(b&{quiet});',
                    f'if(as || bs){{r=(c->fpcr&0x2000000)?{default_nan}:((as?a:b)|{quiet});c->fpsr|=1;}}',
                    f'else if(an && bn)r=(c->fpcr&0x2000000)?{default_nan}:a;else if(an)r=b;else if(bn)r=a;',
                    f'else if(!am && !bm)r=(a{"&" if op=="fmaxnm" else "|"}b)&{sign};',
                    f'else {{int less=((a^b)>>{width-1})?(a>>{width-1}):((a>>{width-1})?am>bm:am<bm);r=less?{"b:a" if op=="fmaxnm" else "a:b"};}}',
                    f'BOOT_QZ(c->q[{dst}],r,{size});}}'])
    elif op in ('frinta','frintn','frintm','frintp','frintz','frinti'):
        names=[i.reg_name(o.reg) if o.type==ARM64_OP_REG else '' for o in args]
        if len(names)!=2 or not all(re.fullmatch(r'[sd](?:[0-9]|[12][0-9]|3[01])',n) for n in names) or names[0][0]!=names[1][0]:
            raise Unsupported('FRINT matching scalar S/D registers only')
        dst,src=[int(n[1:]) for n in names];width=32 if names[0][0]=='s' else 64;size=width//8
        from float_emit import round_integral
        out.append(f'{{uint{width}_t a,r;memcpy(&a,c->q[{src}],{size});')
        out.append(f'if(c->fpcr&~UINT32_C(0x3c00000)){{boot_fault(c,BOOT_FP_SCOPE,{pc},0,{size});return c->status;}}')
        out.append(round_integral(width,op))
        out.append(f'BOOT_QZ(c->q[{dst}],r,{size});}}')
    elif op in ('fdiv','fsqrt') and args and args[0].type==ARM64_OP_REG and re.fullmatch(r'v(?:[0-9]|[12][0-9]|3[01])',i.reg_name(args[0].reg)):
        count=2 if op=='fsqrt' else 3
        if len(args)!=count or any(o.type!=ARM64_OP_REG or o.vector_index!=-1 or o.vas!=args[0].vas or not re.fullmatch(r'v(?:[0-9]|[12][0-9]|3[01])',i.reg_name(o.reg)) for o in args):raise Unsupported('Floating vector division/root operands')
        lanes={A64.ARM64_VAS_2S:2,A64.ARM64_VAS_4S:4}.get(args[0].vas)
        if not lanes:raise Unsupported('Floating vector division/root arrangement')
        registers=[int(i.reg_name(o.reg)[1:]) for o in args];dst,left=registers[:2]
        out.append(f'{{if(c->fpcr&~UINT32_C(0x3000000)){{boot_fault(c,BOOT_FP_SCOPE,{pc},0,4);return c->status;}}')
        # Patch 58: packed host DIVPS/SQRTPS with one result check (boot_fp_native.h), else the exact per-lane
        # code as a shared cold helper (float_emit.fp_path_forms, boot_fp_paths.h).
        from float_emit import fp_vector_call
        out.append(fp_vector_call(op,lanes,4,dst,left,registers[2] if op=='fdiv' else left)+'}')
    elif op=='fsqrt':
        names=[i.reg_name(o.reg) if o.type==ARM64_OP_REG else '' for o in args]
        if len(names)!=2 or not all(re.fullmatch(r'[sd](?:[0-9]|[12][0-9]|3[01])',n) for n in names) or names[0][0]!=names[1][0]:raise Unsupported('FSQRT matching scalar S/D registers only')
        dst,src=[int(n[1:]) for n in names];width=32 if names[0][0]=='s' else 64;size=width//8
        out.append(f'{{uint{width}_t a,r;memcpy(&a,c->q[{src}],{size});')
        out.append(f'if(c->fpcr&~UINT32_C(0x3000000)){{boot_fault(c,BOOT_FP_SCOPE,{pc},0,{size});return c->status;}}')
        # Play build (BOOT_FP_NATIVE): host SQRTSS/SQRTSD kept when it is provably the Arm result with FPSR
        # unchanged (boot_fp_native.h: IXC already set, no subnormal input, finite result); else the exact
        # code (shared cold helper, boot_fp_paths.h). Patch 58: IXC is now exact on this path too.
        from float_emit import fp_scalar_call
        out.append(fp_scalar_call('fsqrt',width))
        out.append(f'BOOT_QZ(c->q[{dst}],r,{size});}}')
    elif op in ('fcmp','fccmp'):
        names=[i.reg_name(o.reg) if o.type==ARM64_OP_REG else '' for o in args[:2]]
        zero=op=='fcmp' and len(args)==2 and args[1].type==A64.ARM64_OP_FP and args[1].fp==0
        if len(args)!=(3 if op=='fccmp' else 2) or not re.fullmatch(r'[sd](?:[0-9]|[12][0-9]|3[01])',names[0]) or (not zero and (not re.fullmatch(r'[sd](?:[0-9]|[12][0-9]|3[01])',names[1]) or names[0][0]!=names[1][0])):raise Unsupported('FCMP scalar registers or immediate zero only')
        left=int(names[0][1:]);right=None if zero else int(names[1][1:]);width=32 if names[0][0]=='s' else 64;size=width//8
        magnitude='UINT64_C(0x7fffffffffffffff)' if width==64 else '0x7fffffff'
        infinity='UINT64_C(0x7ff0000000000000)' if width==64 else '0x7f800000'
        quiet='UINT64_C(0x8000000000000)' if width==64 else '0x400000'
        if op=='fccmp':
            condition=i.op_str.rsplit(', ',1)[-1]
            if condition not in CONDITIONS or args[2].type!=ARM64_OP_IMM or not 0<=args[2].imm<=15:raise Unsupported('FCCMP condition/flags')
            out.append(f'if({CONDITIONS[condition]}){{')
        out.append(f'{{uint{width}_t a,b=0;memcpy(&a,c->q[{left}],{size});'+('' if zero else f'memcpy(&b,c->q[{right}],{size});'))
        out.append(f'if(c->fpcr&~UINT32_C(0x3000000)){{boot_fault(c,BOOT_FP_SCOPE,{pc},0,{size});return c->status;}}')
        from float_emit import flush_inputs
        out.append(f'if(!BOOT_TBN || !boot_fpn_cmp{width}(c,a,b)){{')   # translator batch: host compare first
        out.append(flush_inputs(width,'a','b'))
        out.extend([f'uint{width}_t am=a&{magnitude},bm=b&{magnitude};int nan=am>{infinity} || bm>{infinity};',
                    f'if(nan){{c->n=0;c->z=0;c->carry=1;c->v=1;if((am>{infinity} && !(a&{quiet})) || (bm>{infinity} && !(b&{quiet})))c->fpsr|=1;}}',
                    f'else {{int equal=a==b || (!am && !bm);int less=!equal && (((a^b)>>{width-1})?(a>>{width-1}):((a>>{width-1})?am>bm:am<bm));c->n=less;c->z=equal;c->carry=!less;c->v=0;}}}}}}'])
        if op=='fccmp':
            nzcv=args[2].imm
            out.append(f'}}else{{c->n={(nzcv>>3)&1};c->z={(nzcv>>2)&1};c->carry={(nzcv>>1)&1};c->v={nzcv&1};}}')
    elif op in ('ucvtf','scvtf','fcvtzs','fcvtzu') and len(args) in (2,3) and args[0].type==ARM64_OP_REG and re.fullmatch(r'v(?:[0-9]|[12][0-9]|3[01])',i.reg_name(args[0].reg)):
        if args[1].type!=ARM64_OP_REG or args[0].vector_index!=-1 or args[1].vector_index!=-1 or args[0].vas!=args[1].vas:raise Unsupported('Vector conversion operands')
        names=[i.reg_name(o.reg) for o in args[:2]]
        if not all(re.fullmatch(r'v(?:[0-9]|[12][0-9]|3[01])',n) for n in names):raise Unsupported('Vector conversion registers')
        shape={A64.ARM64_VAS_2S:(2,32),A64.ARM64_VAS_4S:(4,32),A64.ARM64_VAS_2D:(2,64)}.get(args[0].vas)
        if not shape:raise Unsupported('Vector conversion arrangement')
        lanes,width=shape;size=width//8;fraction,bias=(23,127) if width==32 else (52,1023)
        dst,src=[int(n[1:]) for n in names];signed=op in ('scvtf','fcvtzs')
        # Fixed-point forms (#fbits, 1..esize): FixedToFP scales by 2^-fbits before the one
        # rounding; FPToFixed scales by 2^fbits before truncation and saturation.
        fbits=0
        if len(args)==3:
            if args[2].type!=ARM64_OP_IMM or not 1<=args[2].imm<=width:raise Unsupported('Vector fixed-point scale')
            fbits=args[2].imm
        profile='0x3c00000' if op in ('ucvtf','scvtf') else '0x3000000'
        out.append(f'{{if(c->fpcr&~UINT32_C({profile})){{boot_fault(c,BOOT_FP_SCOPE,{pc},0,{size});return c->status;}}')
        # Translator batch: SCVTF/UCVTF without a fixed-point scale try the host conversion first (boot_vcvtf: round
        # to nearest only, IXC exact); the exact loop below stays as the fallback.
        native_cvt=op in ('ucvtf','scvtf') and not fbits
        if native_cvt:out.append(f'if(!BOOT_TBN || !boot_vcvtf(c,{int(signed)},{width},{lanes},{dst},{src})){{')
        out.append(f'uint8_t result[16]={{0}};for(unsigned lane=0;lane<{lanes};++lane){{uint64_t a=little_read(c->q[{src}]+lane*{size},{size}),r=0;')
        if op in ('ucvtf','scvtf'):
            if signed:out.append(f'if(a&(UINT64_C(1)<<{width-1})){{r=UINT64_C(1)<<{width-1};a=(uint{width}_t)(UINT64_C(0)-a);}}')
            out.append('if(a){unsigned top=63u-(unsigned)__builtin_clzll(a);uint64_t mant;')
            out.append(f'if(top<={fraction})mant=a<<({fraction}-top);else {{unsigned shift=top-{fraction},mode=(c->fpcr>>22)&3;mant=a>>shift;uint64_t rem=a&((UINT64_C(1)<<shift)-1),half=UINT64_C(1)<<(shift-1);if(rem)c->fpsr|=16;if((mode==0 && (rem>half || (rem==half && (mant&1)))) || (mode==1 && !r && rem) || (mode==2 && r && rem))++mant;}}')
            out.append(f'if(mant==UINT64_C({1<<(fraction+1)})){{mant>>=1;++top;}}r|=((uint64_t)(top+{bias-fbits})<<{fraction})|(mant&UINT64_C({(1<<fraction)-1}));}}')
        else:
            magnitude='UINT64_C(0x7fffffffffffffff)' if width==64 else 'UINT32_C(0x7fffffff)'
            hidden=1<<fraction;positive=(1<<(width-int(signed)))-1;negative=(1<<(width-1)) if signed else 0
            out.append(f'uint64_t magnitude=a&{magnitude};int negative=(a>>{width-1})!=0;')
            # FPToFixed: NaN gives 0 with IOC; infinities saturate with IOC through the exponent>=width path.
            out.append(f'if(magnitude>UINT64_C({(2*bias+1)<<fraction}))c->fpsr|=1;else if(magnitude && (c->fpcr&0x1000000) && magnitude<UINT64_C({hidden}))c->fpsr|=128;else if(magnitude){{')
            out.append(f'int exponent=(int)(magnitude>>{fraction})-{bias}{f"+{fbits}" if fbits else ""};uint64_t mant=(a&UINT64_C({hidden-1}))|UINT64_C({hidden});uint64_t limit=negative?UINT64_C({negative}):UINT64_C({positive});')
            out.append(f'if(exponent<0)c->fpsr|=16;else if(exponent>={width}){{c->fpsr|=1;r=limit;}}else {{uint64_t remainder=0;if(exponent>={fraction})r=mant<<(exponent-{fraction});else {{unsigned shift={fraction}-exponent;r=mant>>shift;remainder=mant&((UINT64_C(1)<<shift)-1);}}if(r>limit){{r=limit;c->fpsr|=1;}}else if(remainder)c->fpsr|=16;}}')
            if signed:out.append('if(negative)r=UINT64_C(0)-r;')
            out.append('}')
        out.append(f'little_write(result+lane*{size},r,{size});}}memcpy(c->q[{dst}],result,16);'+('}' if native_cvt else '')+'}')
    elif op in ('ucvtf','scvtf'):
        if len(args) not in (2,3) or args[0].type!=ARM64_OP_REG or args[0].vector_index!=-1 or not re.fullmatch(r'[sd](?:[0-9]|[12][0-9]|3[01])',i.reg_name(args[0].reg)):raise Unsupported('Integer-to-float destination scope')
        result_width=32 if i.reg_name(args[0].reg)[0]=='s' else 64
        fraction,bias=(23,127) if result_width==32 else (52,1023)
        source_name=i.reg_name(args[1].reg) if args[1].type==ARM64_OP_REG else ''
        if len(args)==3 and (args[2].type!=ARM64_OP_IMM or not 1<=args[2].imm<=64):raise Unsupported('Integer-to-float fixed-point scale')
        if len(args) not in (2,3):raise Unsupported('Integer-to-float operands')
        fbits=args[2].imm if len(args)==3 else 0
        if re.fullmatch(r'[sd](?:[0-9]|[12][0-9]|3[01])',source_name):
            width=32 if source_name[0]=='s' else 64
            if width!=result_width or args[1].vector_index!=-1:raise Unsupported('SIMD integer-to-float matching widths')
            value=f'little_read(c->q[{int(source_name[1:])}],{width//8})'
        else:value,width=register(args[1])
        if width not in (32,64) or fbits>width:raise Unsupported('Integer-to-float source scope')
        q=f'c->q[{int(i.reg_name(args[0].reg)[1:])}]'
        out.append(f'if(c->fpcr&~UINT32_C(0x3c00000)){{boot_fault(c,BOOT_FP_SCOPE,{pc},0,{result_width//8});return c->status;}}')
        # Play build (BOOT_FP_NATIVE): the host conversion (correctly rounded to nearest) when FPCR.RMode is
        # round-to-nearest and there is no fixed-point scale; FPSR.IXC is not accumulated on that path.
        if not fbits:
            ftype='float' if result_width==32 else 'double';itype=('int' if op=='scvtf' else 'uint')+f'{width}_t'
            # Translator batch: boot_fpn_cvtf also sets FPSR.IXC exactly (the old native path did not accumulate it).
            (ftype,itype)
            out.append(f'if(BOOT_FP_NATIVE && !(c->fpcr&0xc00000)){{uint{result_width}_t r_=(uint{result_width}_t)boot_fpn_cvtf(c,{int(op=="scvtf")},{width},{result_width},(uint64_t)({value}));'
                       f'BOOT_QZ({q},r_,{result_width//8});}}else')
        out.append(f'{{uint64_t a=(uint{width}_t)({value}),r=0;')
        if op=='scvtf':out.append(f'if(a&(UINT64_C(1)<<{width-1})){{r=UINT64_C({1<<(result_width-1)});a=(uint{width}_t)(UINT64_C(0)-a);}}')
        out.append('if(a){unsigned top=63u-(unsigned)__builtin_clzll(a);uint64_t mant;')
        out.append(f'if(top<={fraction})mant=a<<({fraction}-top);else {{unsigned shift=top-{fraction},mode=(c->fpcr>>22)&3;mant=a>>shift;uint64_t rem=a&((UINT64_C(1)<<shift)-1),half=UINT64_C(1)<<(shift-1);if(rem)c->fpsr|=16;if((mode==0 && (rem>half || (rem==half && (mant&1)))) || (mode==1 && !r && rem) || (mode==2 && r && rem))++mant;}}')
        out.append(f'if(mant==UINT64_C({1<<(fraction+1)})){{mant>>=1;++top;}}r|=((uint64_t)(top+{bias-fbits})<<{fraction})|(mant&UINT64_C({(1<<fraction)-1}));}}')
        out.append(f'BOOT_QZ({q},r,{result_width//8});}}')
    elif op in ('fcvtzu','fcvtzs','fcvtas','fcvtau','fcvtms','fcvtmu','fcvtps','fcvtpu'):
        nearest_away=op in ('fcvtas','fcvtau')
        round_down=op in ('fcvtms','fcvtmu')
        round_up=op in ('fcvtps','fcvtpu')
        if (nearest_away or round_down or round_up) and len(args)!=2:raise Unsupported('Directed scalar conversion has no fixed-point scale')
        if len(args) not in (2,3) or args[1].type!=ARM64_OP_REG or not re.fullmatch(r'[sd](?:[0-9]|[12][0-9]|3[01])',i.reg_name(args[1].reg)):
            raise Unsupported('Float-to-fixed scalar S/D source only')
        destination=i.reg_name(args[0].reg) if args[0].type==ARM64_OP_REG else ''
        simd=bool(re.fullmatch(r'[sd](?:[0-9]|[12][0-9]|3[01])',destination))
        if simd:
            if destination[0]!=i.reg_name(args[1].reg)[0]:raise Unsupported('SIMD conversion element widths')
            dst=int(destination[1:]);width=32 if destination[0]=='s' else 64
        else:dst,width=register(args[0],True)
        fraction=0
        if len(args)==3:
            if args[2].type!=ARM64_OP_IMM or not 1<=args[2].imm<=width:raise Unsupported('Float-to-fixed scale')
            fraction=args[2].imm
        name=i.reg_name(args[1].reg);src=int(name[1:]);bits=32 if name[0]=='s' else 64;size=bits//8
        mantissa=23 if bits==32 else 52;bias=127 if bits==32 else 1023
        sign=1<<(bits-1);magnitude=sign-1;hidden=1<<mantissa;infinity=magnitude^(hidden-1)
        signed=op in ('fcvtzs','fcvtas','fcvtms','fcvtps');positive=(1<<(width-int(signed)))-1;negative=(1<<(width-1)) if signed else 0
        out.append(f'{{uint{bits}_t a;memcpy(&a,c->q[{src}],{size});')
        # DN cannot affect float-to-integer results. FPToFixed: NaN gives 0 with IOC;
        # infinities saturate with IOC through the exponent>=width path below.
        out.append(f'if(c->fpcr&~UINT32_C(0x3000000)){{boot_fault(c,BOOT_FP_SCOPE,{pc},0,{size});return c->status;}}')
        # Translator batch: FCVTZS/FCVTZU without a scale try the host truncation first (boot_fpn_cvtz*, IXC exact).
        native_cvtz=op in ('fcvtzs','fcvtzu') and not fraction
        if native_cvtz:out.append(f'uint64_t r=0;if(!BOOT_TBN || !boot_fpn_cvtz{bits}(c,{int(signed)},{width},a,&r)){{uint64_t magnitude=a&UINT64_C({magnitude});int negative=(a>>{bits-1})!=0;')
        else:out.append(f'uint64_t r=0,magnitude=a&UINT64_C({magnitude});int negative=(a>>{bits-1})!=0;')
        out.append(f'if(magnitude>UINT64_C({infinity}))c->fpsr|=1;else if(magnitude && (c->fpcr&0x1000000) && magnitude<UINT64_C({hidden}))c->fpsr|=128;else if(magnitude){{')
        out.append(f'int exponent=(int)(magnitude>>{mantissa})-{bias}+{fraction};uint64_t mant=(a&UINT64_C({hidden-1}))|UINT64_C({hidden});')
        out.append(f'uint64_t limit=negative?UINT64_C({negative}):UINT64_C({positive});')
        if nearest_away:
            out.append(f'if(exponent<0){{r=exponent==-1;if(r>limit){{r=limit;c->fpsr|=1;}}else c->fpsr|=16;}}else if(exponent>={width}){{c->fpsr|=1;r=limit;}}')
        elif round_down or round_up:
            condition='negative' if round_down else '!negative'
            out.append(f'if(exponent<0){{r={condition};if(r>limit){{r=limit;c->fpsr|=1;}}else c->fpsr|=16;}}else if(exponent>={width}){{c->fpsr|=1;r=limit;}}')
        else:out.append(f'if(exponent<0)c->fpsr|=16;else if(exponent>={width}){{c->fpsr|=1;r=limit;}}')
        rounding='if(remainder>=(UINT64_C(1)<<(shift-1)))++r;' if nearest_away else (
            f'if(remainder && ({"negative" if round_down else "!negative"}))++r;' if round_down or round_up else '')
        out.append(f'else {{uint64_t remainder=0;if(exponent>={mantissa})r=mant<<(exponent-{mantissa});else {{unsigned shift={mantissa}-exponent;r=mant>>shift;remainder=mant&((UINT64_C(1)<<shift)-1);{rounding}}}')
        out.append('if(r>limit){r=limit;c->fpsr|=1;}else if(remainder)c->fpsr|=16;}')
        if signed:out.append('if(negative)r=UINT64_C(0)-r;')
        out.append('}')
        if native_cvtz:out.append('}')
        if simd:out.append(f'BOOT_QZ(c->q[{dst}],r,{width//8});')
        else:assignment(dst,width,'r')
        out.append('}')
    elif op=='fcvt':
        if len(args)!=2 or any(o.type!=ARM64_OP_REG for o in args):raise Unsupported('FCVT scalar register shape')
        names=[i.reg_name(o.reg) for o in args]
        if re.fullmatch(r's(?:[0-9]|[12][0-9]|3[01])',names[0]) and re.fullmatch(r'd(?:[0-9]|[12][0-9]|3[01])',names[1]):
            from float_emit import NARROW,flush_inputs
            dst,src=[int(n[1:]) for n in names]
            out.append(f'{{uint64_t a;uint32_t r;memcpy(&a,c->q[{src}],8);')
            out.append(f'if(c->fpcr&~UINT32_C(0x3000000)){{boot_fault(c,BOOT_FP_SCOPE,{pc},0,4);return c->status;}}')
            out.append(flush_inputs(64,'a'))
            # FPConvert NaN: DN gives the default NaN, else FPConvertNaN keeps sign and top payload bits; SNaN raises IOC.
            out.append('if((a&UINT64_C(0x7fffffffffffffff))>UINT64_C(0x7ff0000000000000)){if(!(a&UINT64_C(0x8000000000000)))c->fpsr|=1;'
                       'r=(c->fpcr&0x2000000)?0x7fc00000:(((uint32_t)(a>>32)&0x80000000)|0x7fc00000|((uint32_t)(a>>29)&0x3fffff));}')
            out.append('else if((a&UINT64_C(0x7fffffffffffffff))==UINT64_C(0x7ff0000000000000))r=((uint32_t)(a>>32)&0x80000000)|0x7f800000;else {')
            out.append(NARROW)
            out.append(f'}}BOOT_QZ(c->q[{dst}],r,4);}}')
            # The common instruction epilogue below still accounts for this operation.
        elif not re.fullmatch(r'd(?:[0-9]|[12][0-9]|3[01])',names[0]) or not re.fullmatch(r's(?:[0-9]|[12][0-9]|3[01])',names[1]):raise Unsupported('FCVT binary32/binary64 only')
        else:
            dst,src=[int(n[1:]) for n in names]
            out.append(f'{{uint32_t a;memcpy(&a,c->q[{src}],4);')
            out.append(f'if(c->fpcr&~UINT32_C(0x3000000)){{boot_fault(c,BOOT_FP_SCOPE,{pc},0,8);return c->status;}}')
            from float_emit import flush_inputs
            out.append(flush_inputs(32,'a'))
            out.extend(['uint32_t exponent=(a>>23)&255,mantissa=a&0x7fffff;uint64_t bits=(uint64_t)(a>>31)<<63;',
                        # FPConvert NaN: DN default NaN, else sign and payload widened and quieted; SNaN raises IOC.
                        'if(exponent==255 && mantissa){if(!(mantissa&0x400000))c->fpsr|=1;bits=(c->fpcr&0x2000000)?UINT64_C(0x7ff8000000000000):(bits|UINT64_C(0x7ff8000000000000)|((uint64_t)mantissa<<29));}',
                        'else if(exponent==255)bits|=UINT64_C(0x7ff0000000000000);',
                        'else if(exponent)bits|=((uint64_t)(exponent+896)<<52)|((uint64_t)mantissa<<29);',
                        'else if(mantissa){int e=-126;while(!(mantissa&0x800000)){mantissa<<=1;--e;}bits|=((uint64_t)(e+1023)<<52)|((uint64_t)(mantissa&0x7fffff)<<29);}',
                        f'BOOT_QZ(c->q[{dst}],bits,8);}}'])
    elif op=='mvn' and args and args[0].type==ARM64_OP_REG and re.fullmatch(r'v(?:[0-9]|[12][0-9]|3[01])',i.reg_name(args[0].reg)):
        if len(args)!=2 or any(o.type!=ARM64_OP_REG or o.vector_index!=-1 or o.vas!=args[0].vas or not re.fullmatch(r'v(?:[0-9]|[12][0-9]|3[01])',i.reg_name(o.reg)) for o in args):raise Unsupported('Vector MVN operands')
        size={A64.ARM64_VAS_8B:8,A64.ARM64_VAS_16B:16}.get(args[0].vas)
        if not size:raise Unsupported('Vector MVN arrangement')
        dst,src=[int(i.reg_name(o.reg)[1:]) for o in args]
        out.append(f'{{uint8_t result[16]={{0}};for(unsigned lane=0;lane<{size};++lane)result[lane]=(uint8_t)~c->q[{src}][lane];memcpy(c->q[{dst}],result,16);}}')
    elif op in ('tbl','tbx'):
        if not 3<=len(args)<=6 or any(o.type!=ARM64_OP_REG or o.vector_index!=-1 for o in args):raise Unsupported('TBL/TBX vector operands')
        names=[i.reg_name(o.reg) for o in args]
        if not all(re.fullmatch(r'v(?:[0-9]|[12][0-9]|3[01])',n) for n in names):raise Unsupported('TBL/TBX vector registers')
        if args[0].vas not in (A64.ARM64_VAS_8B,A64.ARM64_VAS_16B) or args[-1].vas!=args[0].vas or any(o.vas!=A64.ARM64_VAS_16B for o in args[1:-1]):raise Unsupported('TBL/TBX arrangements')
        dst,index=int(names[0][1:]),int(names[-1][1:]);tables=[int(n[1:]) for n in names[1:-1]]
        if len(tables)>4 or any(tables[j]!=(tables[0]+j)%32 for j in range(len(tables))):raise Unsupported('TBL/TBX table register list')
        size=8 if args[0].vas==A64.ARM64_VAS_8B else 16
        table_copy=''.join(f'memcpy(table+{j*16},c->q[{reg}],16);' for j,reg in enumerate(tables))
        initial=(f'memcpy(result,c->q[{dst}],{size});memset(result+{size},0,{16-size});' if size==8 else f'memcpy(result,c->q[{dst}],16);') if op=='tbx' else 'memset(result,0,16);'
        out.append(f'{{uint8_t table[{len(tables)*16}],indices[16],result[16];{table_copy}memcpy(indices,c->q[{index}],16);{initial}for(unsigned lane=0;lane<{size};++lane){{unsigned offset=indices[lane];if(offset<{len(tables)*16})result[lane]=table[offset];}}memcpy(c->q[{dst}],result,16);}}')
    elif op in ('zip1','zip2'):
        if len(args)!=3 or any(o.type!=ARM64_OP_REG or o.vector_index!=-1 for o in args):raise Unsupported('ZIP vector operands')
        names=[i.reg_name(o.reg) for o in args]
        if not all(re.fullmatch(r'v(?:[0-9]|[12][0-9]|3[01])',n) for n in names) or len({o.vas for o in args})!=1:raise Unsupported('ZIP matching arrangements')
        shape={A64.ARM64_VAS_8B:(8,1),A64.ARM64_VAS_16B:(16,1),A64.ARM64_VAS_4H:(8,2),A64.ARM64_VAS_8H:(16,2),A64.ARM64_VAS_2S:(8,4),A64.ARM64_VAS_4S:(16,4),A64.ARM64_VAS_2D:(16,8)}.get(args[0].vas)
        if not shape:raise Unsupported('ZIP vector arrangement')
        size,element=shape;half=size//(2*element);dst,left,right=[int(n[1:]) for n in names]
        start=half if op=='zip2' else 0
        out.append(f'{{uint8_t result[16]={{0}};for(unsigned lane=0;lane<{half};++lane){{memcpy(result+lane*{2*element},c->q[{left}]+(lane+{start})*{element},{element});memcpy(result+lane*{2*element}+{element},c->q[{right}]+(lane+{start})*{element},{element});}}memcpy(c->q[{dst}],result,16);}}')
    elif op in ('trn1','trn2'):
        if len(args)!=3 or any(o.type!=ARM64_OP_REG or o.vector_index!=-1 for o in args):raise Unsupported('TRN vector operands')
        names=[i.reg_name(o.reg) for o in args]
        if not all(re.fullmatch(r'v(?:[0-9]|[12][0-9]|3[01])',n) for n in names) or len({o.vas for o in args})!=1:raise Unsupported('TRN matching arrangements')
        shape={A64.ARM64_VAS_8B:(8,1),A64.ARM64_VAS_16B:(16,1),A64.ARM64_VAS_4H:(8,2),A64.ARM64_VAS_8H:(16,2),A64.ARM64_VAS_2S:(8,4),A64.ARM64_VAS_4S:(16,4),A64.ARM64_VAS_2D:(16,8)}.get(args[0].vas)
        if not shape:raise Unsupported('TRN vector arrangement')
        size,element=shape;half=size//(2*element);dst,left,right=[int(n[1:]) for n in names]
        parity=1 if op=='trn2' else 0
        out.append(f'{{uint8_t result[16]={{0}};for(unsigned lane=0;lane<{half};++lane){{unsigned off=(2*lane+{parity})*{element};memcpy(result+lane*{2*element},c->q[{left}]+off,{element});memcpy(result+lane*{2*element}+{element},c->q[{right}]+off,{element});}}memcpy(c->q[{dst}],result,16);}}')
    elif op in ('bit','bif','bsl'):
        if len(args)!=3 or any(o.type!=ARM64_OP_REG or o.vector_index!=-1 for o in args):raise Unsupported('Bit-select vector operands')
        names=[i.reg_name(o.reg) for o in args]
        if not all(re.fullmatch(r'v(?:[0-9]|[12][0-9]|3[01])',n) for n in names) or len({o.vas for o in args})!=1:raise Unsupported('Bit-select matching arrangements')
        size={A64.ARM64_VAS_8B:8,A64.ARM64_VAS_16B:16}.get(args[0].vas)
        if not size:raise Unsupported('Bit-select vector arrangement')
        dst,left,right=[int(n[1:]) for n in names]
        expression={'bit':'(d&~m)|(a&m)','bif':'(d&m)|(a&~m)','bsl':'(d&a)|(~d&m)'}[op]
        out.append(f'{{uint8_t result[16]={{0}};for(unsigned lane=0;lane<{size};++lane){{uint8_t d=c->q[{dst}][lane],a=c->q[{left}][lane],m=c->q[{right}][lane];result[lane]=(uint8_t)({expression});}}memcpy(c->q[{dst}],result,16);}}')
    elif op=='addp':
        if len(args)!=3 or any(o.type!=ARM64_OP_REG or o.vector_index!=-1 for o in args):raise Unsupported('ADDP vector operands')
        names=[i.reg_name(o.reg) for o in args]
        if not all(re.fullmatch(r'v(?:[0-9]|[12][0-9]|3[01])',n) for n in names) or len({o.vas for o in args})!=1:raise Unsupported('ADDP matching arrangements')
        shape={A64.ARM64_VAS_8B:(8,1),A64.ARM64_VAS_16B:(16,1),A64.ARM64_VAS_4H:(8,2),A64.ARM64_VAS_8H:(16,2),A64.ARM64_VAS_2S:(8,4),A64.ARM64_VAS_4S:(16,4),A64.ARM64_VAS_2D:(16,8)}.get(args[0].vas)
        if not shape:raise Unsupported('ADDP vector arrangement')
        size,element=shape;lanes=size//element;dst,left,right=[int(n[1:]) for n in names]
        out.append(f'{{uint8_t result[16]={{0}};for(unsigned lane=0;lane<{lanes};++lane){{const uint8_t*p=c->q[lane<{lanes//2}?{left}:{right}]+(lane%{lanes//2})*{2*element};uint64_t a=little_read(p,{element}),b=little_read(p+{element},{element});little_write(result+lane*{element},a+b,{element});}}memcpy(c->q[{dst}],result,16);}}')
    elif op in VECTOR_INT_OPS and len(args)>=2 and args[0].type==ARM64_OP_REG and (i.reg_name(args[0].reg).startswith('v') or (op in ('smaxv','sminv','umaxv','uminv') and args[1].type==ARM64_OP_REG and i.reg_name(args[1].reg).startswith('v'))):
        # Integer vector family (lane-wise, pairwise, widening, across-lanes, immediate shifts).
        if any(o.type==ARM64_OP_REG and o.vector_index!=-1 for o in args):raise Unsupported('Integer vector lane operands')
        names=[i.reg_name(o.reg) for o in args if o.type==ARM64_OP_REG]
        if not all(re.fullmatch(r'v(?:[0-9]|[12][0-9]|3[01])',n) for n in (names[1:] if op in ('smaxv','sminv','umaxv','uminv') else names)):raise Unsupported('Integer vector registers')
        regs=[int(n[1:]) for n in names]
        full={A64.ARM64_VAS_8B:(8,1),A64.ARM64_VAS_16B:(16,1),A64.ARM64_VAS_4H:(8,2),A64.ARM64_VAS_8H:(16,2),A64.ARM64_VAS_2S:(8,4),A64.ARM64_VAS_4S:(16,4),A64.ARM64_VAS_2D:(16,8)}
        def sx(v,bits):return f'((int64_t)((({v})^UINT64_C({1<<(bits-1)}))-UINT64_C({1<<(bits-1)})))'
        if op in ('smax','smin','umax','umin','cmgt','cmge','cmhs','mul','shadd','shsub','uhadd','uhsub','sqadd','uqadd','sqsub','uqsub','sabd','uabd','smaxp','sminp','umaxp','uminp') and len(args)==3 and args[2].type==ARM64_OP_REG:
            if len({o.vas for o in args})!=1:raise Unsupported('Integer vector arrangement mismatch')
            shape=full.get(args[0].vas)
            if not shape or (op in ('mul','smaxp','sminp','umaxp','uminp','shadd','shsub','uhadd','uhsub','sabd','uabd') and shape[1]==8):raise Unsupported('Integer vector arrangement')
            size,element=shape;bits=element*8;dst,left,right=regs;lanes=size//element
            smax_=(1<<(bits-1))-1;smin_=-(1<<(bits-1));umax_=(1<<bits)-1
            sa,sb=sx('a',bits),sx('b',bits)
            expression={'smax':f'({sa}>{sb}?a:b)','smin':f'({sa}<{sb}?a:b)','umax':'(a>b?a:b)','umin':'(a<b?a:b)',
                        'cmgt':f'({sa}>{sb}?UINT64_MAX:0)','cmge':f'({sa}>={sb}?UINT64_MAX:0)','cmhs':'(a>=b?UINT64_MAX:0)','mul':'(a*b)',
                        'shadd':f'((uint64_t)(({sa}+{sb})>>1))','shsub':f'((uint64_t)(({sa}-{sb})>>1))','uhadd':'((a+b)>>1)','uhsub':'((a-b)>>1)',
                        'sabd':f'({sa}>{sb}?(uint64_t)({sa}-{sb}):(uint64_t)({sb}-{sa}))','uabd':'(a>b?a-b:b-a)',
                        'smaxp':f'({sa}>{sb}?a:b)','sminp':f'({sa}<{sb}?a:b)','umaxp':'(a>b?a:b)','uminp':'(a<b?a:b)'}
            if op in ('sqadd','sqsub'):
                if bits==64:raise Unsupported('64-bit saturating vector arithmetic')
                operator='+' if op=='sqadd' else '-'
                expr=f'({sa}{operator}{sb}>INT64_C({smax_})?UINT64_C({smax_}):({sa}{operator}{sb}<INT64_C({smin_})?(uint64_t)INT64_C({smin_}):(uint64_t)({sa}{operator}{sb})))'
                sat=f'(({sa}{operator}{sb}>INT64_C({smax_}))||({sa}{operator}{sb}<INT64_C({smin_})))'
            elif op in ('uqadd','uqsub'):
                if bits==64:raise Unsupported('64-bit saturating vector arithmetic')
                expr=f'(a+b>UINT64_C({umax_})?UINT64_C({umax_}):a+b)' if op=='uqadd' else '(a<b?0:a-b)'
                sat=f'(a+b>UINT64_C({umax_}))' if op=='uqadd' else '(a<b)'
            else:expr=expression[op];sat=None
            if op in ('smaxp','sminp','umaxp','uminp'):
                out.append(f'{{uint8_t result[16]={{0}};for(unsigned lane=0;lane<{lanes};++lane){{const uint8_t*p=c->q[lane<{lanes//2}?{left}:{right}]+(lane%{lanes//2})*{2*element};uint64_t a=little_read(p,{element}),b=little_read(p+{element},{element});little_write(result+lane*{element},{expr},{element});}}memcpy(c->q[{dst}],result,16);}}')
            elif op in ('smax','smin','umax','umin','cmgt','cmge','cmhs','mul'):
                # Translator batch: whole-register host vector operation (exact integer semantics; .8B/.4H/.2S
                # arrangements zero the upper half like the lane loop's result buffer).
                signed_op=op in ('smax','smin','cmgt','cmge')
                st=f'int{bits}_t';ut=f'uint{bits}_t';vt='boot_vs_' if signed_op else 'boot_vu_'
                body={'smax':'__builtin_elementwise_max(a_,b_)','smin':'__builtin_elementwise_min(a_,b_)','umax':'__builtin_elementwise_max(a_,b_)',
                      'umin':'__builtin_elementwise_min(a_,b_)','cmgt':'(boot_vs_)(a_>b_)','cmge':'(boot_vs_)(a_>=b_)','cmhs':'(boot_vu_)(a_>=b_)','mul':'a_*b_'}[op]
                upper='' if size==16 else 'w_[1]=0;'
                out.append(f'{{typedef {st if signed_op else ut} {vt} __attribute__((vector_size(16)));typedef uint64_t boot_vw_ __attribute__((vector_size(16)));'
                           f'{vt} a_,b_;memcpy(&a_,c->q[{left}],16);memcpy(&b_,c->q[{right}],16);boot_vw_ w_=(boot_vw_)({body});{upper}memcpy(c->q[{dst}],&w_,16);}}')
            else:
                saturate=f'if({sat})c->fpsr|=UINT32_C(0x8000000);' if sat else ''
                out.append(f'{{uint8_t result[16]={{0}};for(unsigned off=0;off<{size};off+={element}){{uint64_t a=little_read(c->q[{left}]+off,{element}),b=little_read(c->q[{right}]+off,{element});{saturate}little_write(result+off,{expr},{element});}}memcpy(c->q[{dst}],result,16);}}')
        elif op in ('cmgt','cmge','cmle','cmlt','cmeq') and len(args)==3 and args[2].type==ARM64_OP_IMM:
            if args[2].imm!=0 or args[0].vas!=args[1].vas:raise Unsupported('Vector compare-zero operands')
            shape=full.get(args[0].vas)
            if not shape:raise Unsupported('Vector compare-zero arrangement')
            size,element=shape;bits=element*8;dst,src=regs
            test={'cmgt':'>','cmge':'>=','cmle':'<=','cmlt':'<','cmeq':'=='}[op]
            out.append(f'{{uint8_t result[16]={{0}};for(unsigned off=0;off<{size};off+={element}){{uint64_t a=little_read(c->q[{src}]+off,{element});little_write(result+off,{sx("a",bits)}{test}0?UINT64_MAX:0,{element});}}memcpy(c->q[{dst}],result,16);}}')
        elif op in ('neg','abs') and len(args)==2:
            if args[0].vas!=args[1].vas:raise Unsupported('Vector NEG/ABS arrangement')
            shape=full.get(args[0].vas)
            if not shape:raise Unsupported('Vector NEG/ABS arrangement')
            size,element=shape;bits=element*8;dst,src=regs
            expr='0-a' if op=='neg' else f'({sx("a",bits)}<0?0-a:a)'
            out.append(f'{{uint8_t result[16]={{0}};for(unsigned off=0;off<{size};off+={element}){{uint64_t a=little_read(c->q[{src}]+off,{element});little_write(result+off,{expr},{element});}}memcpy(c->q[{dst}],result,16);}}')
        elif op in ('sshr','ushr','srshr','urshr') and len(args)==3 and args[2].type==ARM64_OP_IMM:
            if args[0].vas!=args[1].vas:raise Unsupported('Vector shift-right arrangement')
            shape=full.get(args[0].vas)
            if not shape:raise Unsupported('Vector shift-right arrangement')
            size,element=shape;bits=element*8;shift=args[2].imm;dst,src=regs
            if not 1<=shift<=bits:raise Unsupported('Vector shift-right amount')
            rounding=op in ('srshr','urshr')
            if op in ('sshr','srshr'):
                # arithmetic shift on the sign-extended lane; rounding adds 1<<(shift-1) first (in 65+ bits for 64-bit lanes)
                if rounding and bits==64:raise Unsupported('64-bit rounding shift')
                value=sx('a',bits)
                expr=f'(uint64_t)(({value}{f"+INT64_C({1<<(shift-1)})" if rounding else ""})>>{min(shift,63)})' if shift<64 else f'({value}<0?UINT64_MAX:0)'
            else:
                if rounding and bits==64:raise Unsupported('64-bit rounding shift')
                expr=f'((a{f"+UINT64_C({1<<(shift-1)})" if rounding else ""})>>{shift})' if shift<64 else 'UINT64_C(0)'
            out.append(f'{{uint8_t result[16]={{0}};for(unsigned off=0;off<{size};off+={element}){{uint64_t a=little_read(c->q[{src}]+off,{element});little_write(result+off,{expr},{element});}}memcpy(c->q[{dst}],result,16);}}')
        elif op in ('ushll','ushll2','uxtl','uxtl2','sxtl','sxtl2','shll','shll2'):
            upper=op.endswith('2')
            shapes={A64.ARM64_VAS_8H:(A64.ARM64_VAS_16B if upper else A64.ARM64_VAS_8B,8,1),
                    A64.ARM64_VAS_4S:(A64.ARM64_VAS_8H if upper else A64.ARM64_VAS_4H,4,2),
                    A64.ARM64_VAS_2D:(A64.ARM64_VAS_4S if upper else A64.ARM64_VAS_2S,2,4)}
            shape=shapes.get(args[0].vas)
            if not shape or args[1].vas!=shape[0]:raise Unsupported('Widening shift arrangement')
            _,lanes,source_size=shape;bits=source_size*8;dst,src=regs
            if op.startswith(('uxtl','sxtl')):
                if len(args)!=2:raise Unsupported('Extend-long operands')
                shift=0
            else:
                if len(args)!=3 or args[2].type!=ARM64_OP_IMM:raise Unsupported('Widening shift operands')
                shift=args[2].imm
                if op.startswith('shll') and shift!=bits:raise Unsupported('SHLL shift')
                if op.startswith('ushll') and not 0<=shift<bits:raise Unsupported('USHLL shift')
            value=f'(uint64_t){sx("raw",bits)}' if op.startswith('sxtl') else 'raw'
            out.append(f'{{uint8_t result[16]={{0}};for(unsigned lane=0;lane<{lanes};++lane){{uint64_t raw=little_read(c->q[{src}]+{8 if upper else 0}+lane*{source_size},{source_size});little_write(result+lane*{source_size*2},({value})<<{shift},{source_size*2});}}memcpy(c->q[{dst}],result,16);}}')
        elif op in ('uaddw','uaddw2','saddw','saddw2','usubw','usubw2','ssubw','ssubw2') and len(args)==3:
            upper=op.endswith('2')
            shapes={A64.ARM64_VAS_8H:(A64.ARM64_VAS_16B if upper else A64.ARM64_VAS_8B,8,1),
                    A64.ARM64_VAS_4S:(A64.ARM64_VAS_8H if upper else A64.ARM64_VAS_4H,4,2),
                    A64.ARM64_VAS_2D:(A64.ARM64_VAS_4S if upper else A64.ARM64_VAS_2S,2,4)}
            shape=shapes.get(args[0].vas)
            if not shape or args[1].vas!=args[0].vas or args[2].vas!=shape[0]:raise Unsupported('Wide add arrangement')
            _,lanes,source_size=shape;bits=source_size*8;dst,left,right=regs
            narrow=f'(uint64_t){sx("raw",bits)}' if op.startswith('s') else 'raw'
            operator='+' if 'add' in op else '-'
            out.append(f'{{uint8_t result[16]={{0}};for(unsigned lane=0;lane<{lanes};++lane){{uint64_t raw=little_read(c->q[{right}]+{8 if upper else 0}+lane*{source_size},{source_size});uint64_t a=little_read(c->q[{left}]+lane*{source_size*2},{source_size*2});little_write(result+lane*{source_size*2},a{operator}({narrow}),{source_size*2});}}memcpy(c->q[{dst}],result,16);}}')
        elif op in ('smaxv','sminv','umaxv','uminv') and len(args)==2:
            shape={A64.ARM64_VAS_8B:(8,1),A64.ARM64_VAS_16B:(16,1),A64.ARM64_VAS_4H:(8,2),A64.ARM64_VAS_8H:(16,2),A64.ARM64_VAS_4S:(16,4)}.get(args[1].vas)
            scalar=i.reg_name(args[0].reg)
            if not shape or not re.fullmatch(r'[bhs](?:[0-9]|[12][0-9]|3[01])',scalar) or {'b':1,'h':2,'s':4}[scalar[0]]!=shape[1]:raise Unsupported('Across-lanes arrangement')
            size,element=shape;bits=element*8;dst=int(scalar[1:]);src=regs[-1]
            better={'smaxv':f'{sx("a",bits)}>{sx("best",bits)}','sminv':f'{sx("a",bits)}<{sx("best",bits)}','umaxv':'a>best','uminv':'a<best'}[op]
            out.append(f'{{uint8_t result[16]={{0}};uint64_t best=little_read(c->q[{src}],{element});for(unsigned off={element};off<{size};off+={element}){{uint64_t a=little_read(c->q[{src}]+off,{element});if({better})best=a;}}little_write(result,best,{element});memcpy(c->q[{dst}],result,16);}}')
        else:raise Unsupported(f'Unsupported {op} {i.op_str}')
    elif op in ('add','sub','cmhi','cmeq','and','orr','orn','bic','eor','ushl','sshl','cmtst') and args and args[0].type==ARM64_OP_REG and i.reg_name(args[0].reg).startswith('v') and (op!='cmeq' or (len(args)==3 and args[2].type==ARM64_OP_REG)):
        if len(args)!=3 or any(o.type!=ARM64_OP_REG or o.vector_index!=-1 for o in args):raise Unsupported('Vector arithmetic operands')
        names=[i.reg_name(o.reg) for o in args]
        if not all(re.fullmatch(r'v(?:[0-9]|[12][0-9]|3[01])',n) for n in names) or len({o.vas for o in args})!=1:raise Unsupported('Matching vector arithmetic arrangements')
        layouts={A64.ARM64_VAS_8B:(8,1),A64.ARM64_VAS_16B:(16,1)}
        if op in ('add','sub','cmhi','cmeq','ushl','sshl','cmtst'):layouts.update({A64.ARM64_VAS_4H:(8,2),A64.ARM64_VAS_8H:(16,2),A64.ARM64_VAS_2S:(8,4),A64.ARM64_VAS_4S:(16,4),A64.ARM64_VAS_2D:(16,8)})
        shape=layouts.get(args[0].vas)
        if not shape:raise Unsupported('Vector arithmetic arrangement')
        size,element=shape;dst,left,right=[int(n[1:]) for n in names]
        # Variable shifts use the signed low byte only. Bound both shifts before C
        # evaluation, including the 64-bit lane and negative 128 cases.
        expression={'add':'a+b','sub':'a-b','cmhi':'a>b?UINT64_MAX:0','cmeq':'a==b?UINT64_MAX:0','and':'a&b','orr':'a|b','orn':'a|~b','bic':'a&~b','eor':'a^b','cmtst':'(a&b)?UINT64_MAX:0',
                    'ushl':f'((b&255)<128?((b&255)<{element*8}?a<<(b&255):0):((256-(b&255))<{element*8}?a>>(256-(b&255)):0))',
                    'sshl':f'((b&255)<128?((b&255)<{element*8}?a<<(b&255):0):((256-(b&255))<{element*8}?(a>>(256-(b&255)))|((a>>{element*8-1})?(UINT64_MAX<<({element*8}-(256-(b&255)))):0):((a>>{element*8-1})?UINT64_MAX:0)))'}[op]
        out.append(f'{{uint8_t result[16]={{0}};for(unsigned off=0;off<{size};off+={element}){{uint64_t a=little_read(c->q[{left}]+off,{element}),b=little_read(c->q[{right}]+off,{element});little_write(result+off,{expression},{element});}}memcpy(c->q[{dst}],result,16);}}')
    elif op=='cmeq' and len(args)==3 and args[0].type==ARM64_OP_REG and i.reg_name(args[0].reg).startswith('v'):
        if args[1].type!=ARM64_OP_REG or args[2].type!=ARM64_OP_IMM or args[2].imm!=0 or args[0].vector_index!=-1 or args[1].vector_index!=-1 or args[0].vas!=args[1].vas:raise Unsupported('Vector CMEQ zero operands')
        names=[i.reg_name(o.reg) for o in args[:2]]
        if not all(re.fullmatch(r'v(?:[0-9]|[12][0-9]|3[01])',n) for n in names):raise Unsupported('Vector CMEQ zero registers')
        shape={A64.ARM64_VAS_8B:(8,1),A64.ARM64_VAS_16B:(16,1),A64.ARM64_VAS_4H:(8,2),A64.ARM64_VAS_8H:(16,2),A64.ARM64_VAS_2S:(8,4),A64.ARM64_VAS_4S:(16,4),A64.ARM64_VAS_2D:(16,8)}.get(args[0].vas)
        if not shape:raise Unsupported('Vector CMEQ zero arrangement')
        size,element=shape;dst,src=[int(n[1:]) for n in names]
        out.append(f'{{uint8_t result[16]={{0}};for(unsigned off=0;off<{size};off+={element}){{uint64_t value=little_read(c->q[{src}]+off,{element});little_write(result+off,value?0:UINT64_MAX,{element});}}memcpy(c->q[{dst}],result,16);}}')
    elif op=='shl' and len(args)==3 and args[0].type==ARM64_OP_REG and i.reg_name(args[0].reg).startswith('v'):
        if args[1].type!=ARM64_OP_REG or args[2].type!=ARM64_OP_IMM or any(o.vector_index!=-1 for o in args[:2]):raise Unsupported('Vector SHL operands')
        names=[i.reg_name(o.reg) for o in args[:2]]
        if not all(re.fullmatch(r'v(?:[0-9]|[12][0-9]|3[01])',n) for n in names) or args[0].vas!=args[1].vas:raise Unsupported('Vector SHL matching arrangements')
        shape={A64.ARM64_VAS_8B:(8,1),A64.ARM64_VAS_16B:(16,1),A64.ARM64_VAS_4H:(8,2),A64.ARM64_VAS_8H:(16,2),A64.ARM64_VAS_2S:(8,4),A64.ARM64_VAS_4S:(16,4),A64.ARM64_VAS_2D:(16,8)}.get(args[0].vas)
        if not shape:raise Unsupported('Vector SHL arrangement')
        size,element=shape;shift=args[2].imm;dst,src=[int(n[1:]) for n in names]
        if not 0<=shift<element*8:raise Unsupported('Vector SHL shift')
        out.append(f'{{uint8_t result[16]={{0}};for(unsigned off=0;off<{size};off+={element}){{uint64_t value=little_read(c->q[{src}]+off,{element});little_write(result+off,value<<{shift},{element});}}memcpy(c->q[{dst}],result,16);}}')
    elif op in ('ssra','usra') and len(args)==3 and args[0].type==ARM64_OP_REG and i.reg_name(args[0].reg).startswith('v'):
        if args[1].type!=ARM64_OP_REG or args[2].type!=ARM64_OP_IMM or any(o.vector_index!=-1 for o in args[:2]):raise Unsupported('Vector shift-accumulate operands')
        names=[i.reg_name(o.reg) for o in args[:2]]
        if not all(re.fullmatch(r'v(?:[0-9]|[12][0-9]|3[01])',n) for n in names) or args[0].vas!=args[1].vas:raise Unsupported('Vector shift-accumulate arrangement')
        shape={A64.ARM64_VAS_8B:(8,1),A64.ARM64_VAS_16B:(16,1),A64.ARM64_VAS_4H:(8,2),A64.ARM64_VAS_8H:(16,2),A64.ARM64_VAS_2S:(8,4),A64.ARM64_VAS_4S:(16,4),A64.ARM64_VAS_2D:(16,8)}.get(args[0].vas)
        if not shape:raise Unsupported('Vector shift-accumulate shape')
        size,element=shape;bits=element*8;shift=args[2].imm;dst,src=[int(n[1:]) for n in names]
        if not 1<=shift<=bits:raise Unsupported('Vector shift-accumulate shift')
        if op=='ssra':shifted=f'boot_sign_extend(raw>>{shift},{bits-shift})' if shift<bits else f'((raw>>{bits-1})?UINT64_MAX:0)'
        else:shifted=f'(raw>>{shift})' if shift<bits else 'UINT64_C(0)'
        out.append(f'{{uint8_t result[16]={{0}};for(unsigned off=0;off<{size};off+={element}){{uint64_t raw=little_read(c->q[{src}]+off,{element}),acc=little_read(c->q[{dst}]+off,{element});(void)raw;little_write(result+off,acc+({shifted}),{element});}}memcpy(c->q[{dst}],result,16);}}')
    elif op in ('sshll','sshll2'):
        if len(args)!=3 or any(o.type!=ARM64_OP_REG or o.vector_index!=-1 for o in args[:2]) or args[2].type!=ARM64_OP_IMM:raise Unsupported('SSHLL operands')
        names=[i.reg_name(o.reg) for o in args[:2]]
        if not all(re.fullmatch(r'v(?:[0-9]|[12][0-9]|3[01])',n) for n in names):raise Unsupported('SSHLL registers')
        upper=op=='sshll2'
        shapes={A64.ARM64_VAS_8H:(A64.ARM64_VAS_16B if upper else A64.ARM64_VAS_8B,8,1),
                A64.ARM64_VAS_4S:(A64.ARM64_VAS_8H if upper else A64.ARM64_VAS_4H,4,2),
                A64.ARM64_VAS_2D:(A64.ARM64_VAS_4S if upper else A64.ARM64_VAS_2S,2,4)}
        shape=shapes.get(args[0].vas)
        if not shape or args[1].vas!=shape[0]:raise Unsupported('SSHLL arrangement')
        _,lanes,source_size=shape;shift=args[2].imm;bits=source_size*8
        if not 0<=shift<bits:raise Unsupported('SSHLL shift')
        dst,src=[int(name[1:]) for name in names]
        out.append(f'{{uint8_t result[16]={{0}};for(unsigned lane=0;lane<{lanes};++lane){{uint64_t raw=little_read(c->q[{src}]+{8 if upper else 0}+lane*{source_size},{source_size});uint64_t value=(raw^UINT64_C({1<<(bits-1)}))-UINT64_C({1<<(bits-1)});little_write(result+lane*{source_size*2},value<<{shift},{source_size*2});}}memcpy(c->q[{dst}],result,16);}}')
    elif op in ('xtn','xtn2'):
        if len(args)!=2 or any(o.type!=ARM64_OP_REG or o.vector_index!=-1 for o in args):raise Unsupported('XTN operands')
        names=[i.reg_name(o.reg) for o in args]
        if not all(re.fullmatch(r'v(?:[0-9]|[12][0-9]|3[01])',n) for n in names):raise Unsupported('XTN registers')
        upper=op=='xtn2'
        source_shape={A64.ARM64_VAS_8H:(8,2),A64.ARM64_VAS_4S:(4,4),A64.ARM64_VAS_2D:(2,8)}.get(args[1].vas)
        destination_vas=({A64.ARM64_VAS_8H:A64.ARM64_VAS_16B,A64.ARM64_VAS_4S:A64.ARM64_VAS_8H,A64.ARM64_VAS_2D:A64.ARM64_VAS_4S} if upper else {A64.ARM64_VAS_8H:A64.ARM64_VAS_8B,A64.ARM64_VAS_4S:A64.ARM64_VAS_4H,A64.ARM64_VAS_2D:A64.ARM64_VAS_2S}).get(args[1].vas)
        if not source_shape or args[0].vas!=destination_vas:raise Unsupported('XTN arrangement')
        lanes,source_size=source_shape;target_size=source_size//2;dst,src=[int(name[1:]) for name in names]
        out.append(f'{{uint8_t result[16];'+(f'memcpy(result,c->q[{dst}],16);' if upper else 'memset(result,0,16);'))
        out.append(f'for(unsigned lane=0;lane<{lanes};++lane){{uint64_t value=little_read(c->q[{src}]+lane*{source_size},{source_size});little_write(result+{8 if upper else 0}+lane*{target_size},value,{target_size});}}memcpy(c->q[{dst}],result,16);}}')
    elif op in ('shrn','shrn2'):
        if len(args)!=3 or any(o.type!=ARM64_OP_REG or o.vector_index!=-1 for o in args[:2]) or args[2].type!=ARM64_OP_IMM:raise Unsupported('SHRN operands')
        names=[i.reg_name(o.reg) for o in args[:2]]
        if not all(re.fullmatch(r'v(?:[0-9]|[12][0-9]|3[01])',n) for n in names):raise Unsupported('SHRN registers')
        upper=op=='shrn2'
        source_shape={A64.ARM64_VAS_8H:(8,2),A64.ARM64_VAS_4S:(4,4),A64.ARM64_VAS_2D:(2,8)}.get(args[1].vas)
        destination_vas=({A64.ARM64_VAS_8H:A64.ARM64_VAS_16B,A64.ARM64_VAS_4S:A64.ARM64_VAS_8H,A64.ARM64_VAS_2D:A64.ARM64_VAS_4S} if upper else {A64.ARM64_VAS_8H:A64.ARM64_VAS_8B,A64.ARM64_VAS_4S:A64.ARM64_VAS_4H,A64.ARM64_VAS_2D:A64.ARM64_VAS_2S}).get(args[1].vas)
        if not source_shape or args[0].vas!=destination_vas:raise Unsupported('SHRN arrangement')
        lanes,source_size=source_shape;target_size=source_size//2;shift=args[2].imm;dst,src=[int(name[1:]) for name in names]
        if not 1<=shift<=target_size*8:raise Unsupported('SHRN shift')
        out.append(f'{{uint8_t result[16];'+(f'memcpy(result,c->q[{dst}],16);' if upper else 'memset(result,0,16);'))
        out.append(f'for(unsigned lane=0;lane<{lanes};++lane){{uint64_t value=little_read(c->q[{src}]+lane*{source_size},{source_size});little_write(result+{8 if upper else 0}+lane*{target_size},value>>{shift},{target_size});}}memcpy(c->q[{dst}],result,16);}}')
    elif op in ('sqxtn','sqxtn2','sqxtun','sqxtun2'):
        if len(args)!=2 or any(o.type!=ARM64_OP_REG or o.vector_index!=-1 for o in args):raise Unsupported('Saturating narrow operands')
        names=[i.reg_name(o.reg) for o in args]
        if not all(re.fullmatch(r'v(?:[0-9]|[12][0-9]|3[01])',n) for n in names):raise Unsupported('Saturating narrow registers')
        upper=op.endswith('2');unsigned=op.startswith('sqxtun')
        source_shape={A64.ARM64_VAS_8H:(8,2),A64.ARM64_VAS_4S:(4,4),A64.ARM64_VAS_2D:(2,8)}.get(args[1].vas)
        destination_vas=({A64.ARM64_VAS_8H:A64.ARM64_VAS_16B,A64.ARM64_VAS_4S:A64.ARM64_VAS_8H,A64.ARM64_VAS_2D:A64.ARM64_VAS_4S} if upper else {A64.ARM64_VAS_8H:A64.ARM64_VAS_8B,A64.ARM64_VAS_4S:A64.ARM64_VAS_4H,A64.ARM64_VAS_2D:A64.ARM64_VAS_2S}).get(args[1].vas)
        if not source_shape or args[0].vas!=destination_vas:raise Unsupported('Saturating narrow arrangement')
        lanes,source_size=source_shape;target_size=source_size//2;bits=target_size*8;source_bits=source_size*8
        dst,src=[int(name[1:]) for name in names]
        out.append(f'{{uint8_t result[16];'+(f'memcpy(result,c->q[{dst}],16);' if upper else 'memset(result,0,16);'))
        out.append(f'for(unsigned lane=0;lane<{lanes};++lane){{uint64_t raw=little_read(c->q[{src}]+lane*{source_size},{source_size});int64_t value;')
        if source_bits==64:out.append('memcpy(&value,&raw,8);')
        else:out.append(f'value=(int64_t)raw;if(raw&(UINT64_C(1)<<{source_bits-1}))value-=INT64_C({1<<source_bits});')
        minimum=0 if unsigned else -(1<<(bits-1));maximum=(1<<bits)-1 if unsigned else (1<<(bits-1))-1
        out.append(f'if(value<INT64_C({minimum})){{value=INT64_C({minimum});c->fpsr|=UINT32_C(0x8000000);}}else if(value>INT64_C({maximum})){{value=INT64_C({maximum});c->fpsr|=UINT32_C(0x8000000);}}')
        out.append(f'little_write(result+{8 if upper else 0}+lane*{target_size},(uint64_t)value,{target_size});}}memcpy(c->q[{dst}],result,16);}}')
    elif op=='rev64':
        if len(args)!=2 or any(o.type!=ARM64_OP_REG or o.vector_index!=-1 for o in args):raise Unsupported('REV64 vector operands')
        names=[i.reg_name(o.reg) for o in args]
        if not all(re.fullmatch(r'v(?:[0-9]|[12][0-9]|3[01])',n) for n in names) or args[0].vas!=args[1].vas:raise Unsupported('REV64 matching vectors')
        shape={A64.ARM64_VAS_8B:(8,1),A64.ARM64_VAS_16B:(16,1),A64.ARM64_VAS_4H:(8,2),A64.ARM64_VAS_8H:(16,2),A64.ARM64_VAS_2S:(8,4),A64.ARM64_VAS_4S:(16,4)}.get(args[0].vas)
        if not shape:raise Unsupported('REV64 arrangement')
        size,element=shape;dst,src=[int(n[1:]) for n in names]
        out.append(f'{{uint8_t result[16]={{0}};for(unsigned off=0;off<{size};off+={element})memcpy(result+off,c->q[{src}]+(off^{8-element}),{element});memcpy(c->q[{dst}],result,16);}}')
    elif op in ('fcmlt','fcmle') and len(args)==3 and args[0].type==ARM64_OP_REG and i.reg_name(args[0].reg).startswith('v'):
        if args[1].type!=ARM64_OP_REG or args[2].type!=A64.ARM64_OP_FP or args[2].fp!=0.0 or any(o.vector_index!=-1 for o in args[:2]):raise Unsupported('FCMLT zero operands')
        names=[i.reg_name(o.reg) for o in args[:2]]
        shape={A64.ARM64_VAS_2S:(2,4),A64.ARM64_VAS_4S:(4,4),A64.ARM64_VAS_2D:(2,8)}.get(args[0].vas)
        if not shape or args[1].vas!=args[0].vas or not all(re.fullmatch(r'v(?:[0-9]|[12][0-9]|3[01])',n) for n in names):raise Unsupported('FCMLT zero arrangement')
        lanes,size=shape;bits=size*8;dst,src=[int(n[1:]) for n in names]
        fraction=23 if bits==32 else 52;sign=1<<(bits-1);inf=(sign-1)^((1<<fraction)-1)
        from float_emit import flush_inputs
        out.append(f'{{if(c->fpcr&~UINT32_C(0x3c00000)){{boot_fault(c,BOOT_FP_SCOPE,{pc},0,{size});return c->status;}}uint8_t result[16]={{0}};for(unsigned lane=0;lane<{lanes};++lane){{uint64_t a=little_read(c->q[{src}]+lane*{size},{size});')
        out.append(flush_inputs(bits,'a'))
        out.append(f'uint64_t mag=a&UINT64_C({sign-1});int nan=mag>UINT64_C({inf});if(nan)c->fpsr|=1;little_write(result+lane*{size},!nan && ({"(a&UINT64_C(%d)) && mag" % sign if op=='fcmlt' else "((a&UINT64_C(%d)) || !mag)" % sign})?UINT64_MAX:0,{size});}}memcpy(c->q[{dst}],result,16);}}')
    elif op in ('fcmeq','fcmge','fcmgt'):
        if len(args)!=3 or any(o.type!=ARM64_OP_REG or o.vector_index!=-1 for o in args[:2]):raise Unsupported('Floating vector compare operands')
        names=[i.reg_name(o.reg) for o in args[:2]]
        shape={A64.ARM64_VAS_2S:(2,4),A64.ARM64_VAS_4S:(4,4),A64.ARM64_VAS_2D:(2,8)}.get(args[0].vas)
        if not shape or args[1].vas!=args[0].vas or not all(re.fullmatch(r'v(?:[0-9]|[12][0-9]|3[01])',n) for n in names):raise Unsupported('Floating vector compare arrangement')
        lanes,size=shape;bits=size*8;dst,left=[int(n[1:]) for n in names]
        if args[2].type==A64.ARM64_OP_FP and args[2].fp==0.0:right='0'
        elif args[2].type==ARM64_OP_REG and args[2].vector_index==-1 and args[2].vas==args[0].vas and re.fullmatch(r'v(?:[0-9]|[12][0-9]|3[01])',i.reg_name(args[2].reg)):
            right=f'little_read(c->q[{int(i.reg_name(args[2].reg)[1:])}]+lane*{size},{size})'
        else:raise Unsupported('Floating vector compare right operand')
        fraction=23 if size==4 else 52;sign=1<<(bits-1);inf=(sign-1)^((1<<fraction)-1)
        from float_emit import flush_inputs
        rreg=-1 if right=='0' else int(i.reg_name(args[2].reg)[1:])   # translator batch: host packed compare first
        out.append(f'{{if(c->fpcr&~UINT32_C(0x3c00000)){{boot_fault(c,BOOT_FP_SCOPE,{pc},0,{size});return c->status;}}if(!BOOT_TBN || !boot_vfn_cmp{bits}(c,{("fcmeq","fcmge","fcmgt").index(op)},{lanes},{dst},{left},{rreg})){{uint8_t result[16]={{0}};for(unsigned lane=0;lane<{lanes};++lane){{uint64_t a=little_read(c->q[{left}]+lane*{size},{size}),b={right};')
        out.append(flush_inputs(bits,'a','b'))
        comparison='eq' if op=='fcmeq' else 'eq||gt' if op=='fcmge' else 'gt'
        out.append(f'uint64_t am=a&UINT64_C({sign-1}),bm=b&UINT64_C({sign-1});int an=am>UINT64_C({inf}),bn=bm>UINT64_C({inf});')
        out.append(f'int same_zero=!am&&!bm,eq=a==b||same_zero,gt=!same_zero && (((a^b)&UINT64_C({sign}))?!(a&UINT64_C({sign})):((a&UINT64_C({sign}))?a<b:a>b));')
        if op=='fcmeq':out.append('(void)gt;')
        if op=='fcmgt':out.append('(void)eq;')
        out.append(f'if((an||bn) && ({1 if op!="fcmeq" else 0} || (an && !(a&UINT64_C({1<<(fraction-1)}))) || (bn && !(b&UINT64_C({1<<(fraction-1)})))))c->fpsr|=1;')
        out.append(f'little_write(result+lane*{size},!an && !bn && ({comparison})?UINT64_MAX:0,{size});}}memcpy(c->q[{dst}],result,16);}}}}')
    elif op=='frsqrts':
        if len(args)!=3 or any(o.type!=ARM64_OP_REG or o.vector_index!=-1 for o in args):raise Unsupported('FRSQRTS operands')
        names=[i.reg_name(o.reg) for o in args]
        if all(re.fullmatch(r'v(?:[0-9]|[12][0-9]|3[01])',n) for n in names) and all(o.vas==args[0].vas for o in args):
            shape={A64.ARM64_VAS_2S:(2,4),A64.ARM64_VAS_4S:(4,4),A64.ARM64_VAS_2D:(2,8)}.get(args[0].vas)
        elif all(re.fullmatch(r'[sd](?:[0-9]|[12][0-9]|3[01])',n) for n in names) and len({n[0] for n in names})==1:shape=(1,4 if names[0][0]=='s' else 8)
        else:shape=None
        if not shape:raise Unsupported('FRSQRTS arrangement')
        from float_emit import reciprocal_sqrt_step
        lanes,size=shape;dst,left,right=[int(n[1:]) for n in names]
        out.append(f'{{if(c->fpcr&~UINT32_C(0x3c00000)){{boot_fault(c,BOOT_FP_SCOPE,{pc},0,{size});return c->status;}}if(!BOOT_TBN || !boot_vfn_step{size*8}(c,1,{lanes},{dst},{left},{right})){{uint8_t result[16]={{0}};for(unsigned lane=0;lane<{lanes};++lane){{uint{size*8}_t a,b,r;memcpy(&a,c->q[{left}]+lane*{size},{size});memcpy(&b,c->q[{right}]+lane*{size},{size});')
        out.append(reciprocal_sqrt_step(size*8))
        out.append(f'memcpy(result+lane*{size},&r,{size});}}memcpy(c->q[{dst}],result,16);}}}}')   # translator batch: host FMA first
    elif op=='frsqrte':
        if len(args)!=2 or any(o.type!=ARM64_OP_REG or o.vector_index!=-1 for o in args):raise Unsupported('FRSQRTE operands')
        names=[i.reg_name(o.reg) for o in args]
        if all(re.fullmatch(r'v(?:[0-9]|[12][0-9]|3[01])',n) for n in names) and args[0].vas==args[1].vas:
            shape={A64.ARM64_VAS_2S:(2,4),A64.ARM64_VAS_4S:(4,4),A64.ARM64_VAS_2D:(2,8)}.get(args[0].vas)
        elif all(re.fullmatch(r'[sd](?:[0-9]|[12][0-9]|3[01])',n) for n in names) and names[0][0]==names[1][0]:shape=(1,4 if names[0][0]=='s' else 8)
        else:shape=None
        if not shape:raise Unsupported('FRSQRTE arrangement')
        from float_emit import reciprocal_sqrt_estimate
        lanes,size=shape;dst,src=[int(n[1:]) for n in names]
        out.append(f'{{if(c->fpcr&~UINT32_C(0x3c00000)){{boot_fault(c,BOOT_FP_SCOPE,{pc},0,{size});return c->status;}}uint8_t result[16]={{0}};for(unsigned lane=0;lane<{lanes};++lane){{uint{size*8}_t a,r;memcpy(&a,c->q[{src}]+lane*{size},{size});')
        out.append(reciprocal_sqrt_estimate(size*8))
        out.append(f'memcpy(result+lane*{size},&r,{size});}}memcpy(c->q[{dst}],result,16);}}')
    elif op=='ext':
        if len(args)!=4 or any(o.type!=ARM64_OP_REG or o.vector_index!=-1 for o in args[:3]) or args[3].type!=ARM64_OP_IMM:raise Unsupported('EXT operands')
        names=[i.reg_name(o.reg) for o in args[:3]]
        size={A64.ARM64_VAS_8B:8,A64.ARM64_VAS_16B:16}.get(args[0].vas)
        if not size or any(o.vas!=args[0].vas for o in args[:3]) or not all(re.fullmatch(r'v(?:[0-9]|[12][0-9]|3[01])',n) for n in names):raise Unsupported('EXT vector arrangement')
        index=args[3].imm
        if not 0<=index<size:raise Unsupported('EXT byte index')
        dst,left,right=[int(n[1:]) for n in names]
        out.append(f'{{uint8_t combined[{size*2}],result[16]={{0}};memcpy(combined,c->q[{left}],{size});memcpy(combined+{size},c->q[{right}],{size});memcpy(result,combined+{index},{size});memcpy(c->q[{dst}],result,16);}}')
    elif op=='addv':
        if len(args)!=2 or any(o.type!=ARM64_OP_REG or o.vector_index!=-1 for o in args):raise Unsupported('ADDV operands')
        names=[i.reg_name(o.reg) for o in args]
        shape={A64.ARM64_VAS_8B:(8,1,'b'),A64.ARM64_VAS_16B:(16,1,'b'),
               A64.ARM64_VAS_4H:(8,2,'h'),A64.ARM64_VAS_8H:(16,2,'h'),
               A64.ARM64_VAS_4S:(16,4,'s')}.get(args[1].vas)
        if not shape or not re.fullmatch(r'v(?:[0-9]|[12][0-9]|3[01])',names[1]) or not re.fullmatch(shape[2]+r'(?:[0-9]|[12][0-9]|3[01])',names[0]):
            raise Unsupported('ADDV scalar destination and vector arrangement')
        size,element,_=shape;dst=int(names[0][1:]);src=int(names[1][1:])
        out.append(f'{{uint64_t sum=0;for(unsigned off=0;off<{size};off+={element})sum+=little_read(c->q[{src}]+off,{element});BOOT_QZ(c->q[{dst}],sum,{element});}}')
    elif op in ('cnt','uaddlv'):
        names=[i.reg_name(o.reg) if o.type==ARM64_OP_REG else '' for o in args]
        if len(args)!=2 or any(o.vector_index!=-1 for o in args):raise Unsupported('Vector count/reduction operands')
        if not re.fullmatch(r'v(?:[0-9]|[12][0-9]|3[01])',names[1]):raise Unsupported('Vector count/reduction source')
        size={A64.ARM64_VAS_8B:8,A64.ARM64_VAS_16B:16}.get(args[1].vas)
        if not size:raise Unsupported('Byte count/reduction only')
        dst,src=[int(n[1:]) for n in names]
        if op=='cnt':
            if not names[0].startswith('v') or args[0].vas!=args[1].vas:raise Unsupported('CNT destination')
            out.append(f'{{uint8_t result[16]={{0}};for(unsigned lane=0;lane<{size};++lane)result[lane]=(uint8_t)__builtin_popcount((unsigned)c->q[{src}][lane]);memcpy(c->q[{dst}],result,16);}}')
        else:
            if not re.fullmatch(r'h(?:[0-9]|[12][0-9]|3[01])',names[0]):raise Unsupported('UADDLV byte-to-halfword only')
            out.append(f'{{uint32_t sum=0;for(unsigned lane=0;lane<{size};++lane)sum+=c->q[{src}][lane];BOOT_QZ(c->q[{dst}],sum,2);}}')
    elif op in ('mov','ins') and len(args)==2 and all(o.type==ARM64_OP_REG and o.vector_index>=0 for o in args):
        # INS (element), normally disassembled as MOV. Preserve every other
        # destination lane, including the upper half of the vector.
        names=[i.reg_name(o.reg) for o in args]
        word=int.from_bytes(i.bytes,'little');imm5=(word>>16)&31;imm4=(word>>11)&15
        size=imm5&-imm5
        if word&0xffe08400!=0x6e000400 or size not in (1,2,4,8):raise Unsupported('INS element encoding')
        if not all(re.fullmatch(r'v(?:[0-9]|[12][0-9]|3[01])',n) for n in names):raise Unsupported('INS element registers')
        dst,src=[int(n[1:]) for n in names];dl,sl=[o.vector_index for o in args]
        if dst!=(word&31) or src!=((word>>5)&31) or dl!=imm5//(2*size) or sl!=imm4//size:raise Unsupported('INS element lane encoding mismatch')
        out.append(f'{{uint8_t bits[{size}];memcpy(bits,c->q[{src}]+{sl*size},{size});memcpy(c->q[{dst}]+{dl*size},bits,{size});}}')
    elif op=='mov' and len(args)==2 and args[0].type==ARM64_OP_REG and args[1].type==ARM64_OP_REG and args[1].vector_index>=0 and re.fullmatch(r'[sd](?:[0-9]|[12][0-9]|3[01])',i.reg_name(args[0].reg)):
        destination=i.reg_name(args[0].reg);source=i.reg_name(args[1].reg)
        word=int.from_bytes(i.bytes,'little');imm5=(word>>16)&31;size=imm5&-imm5
        if word&0xffe0fc00!=0x5e000400 or size not in (4,8) or destination[0]!=('s' if size==4 else 'd'):raise Unsupported('Scalar vector-lane MOV encoding')
        lane=args[1].vector_index
        if not re.fullmatch(r'v(?:[0-9]|[12][0-9]|3[01])',source) or lane!=imm5//(2*size) or int(destination[1:])!=(word&31) or int(source[1:])!=((word>>5)&31):raise Unsupported('Scalar vector-lane MOV registers')
        out.append(f'{{uint8_t bits[{size}];memcpy(bits,c->q[{int(source[1:])}]+{lane*size},{size});memset(c->q[{int(destination[1:])}],0,16);memcpy(c->q[{int(destination[1:])}],bits,{size});}}')
    elif op in ('mov','umov') and len(args)==2 and args[1].type==ARM64_OP_REG and args[1].vector_index>=0:
        name=i.reg_name(args[1].reg);destination=i.reg_name(args[0].reg)
        # Capstone omits VAS on the MOV alias. Decode only the fixed UMOV
        # encoding here; this happens during AOT generation, never runtime.
        word=int.from_bytes(i.bytes,'little');imm5=(word>>16)&31
        size=imm5&-imm5
        if word&0xbfe0fc00!=0x0e003c00 or size not in (1,2,4,8) or bool(word&(1<<30))!=(size==8):raise Unsupported('UMOV encoding')
        lane=args[1].vector_index
        if lane!=imm5//(2*size):raise Unsupported('UMOV lane encoding mismatch')
        if not size or not re.fullmatch(r'v(?:[0-9]|[12][0-9]|3[01])',name) or not 0<=lane<16//size:raise Unsupported('UMOV vector lane')
        dst,width=register(args[0],destination not in ('wzr','xzr'))
        if destination in ('sp','wsp') or args[0].vector_index!=-1 or width!=(64 if size==8 else 32):raise Unsupported('UMOV destination width')
        if destination not in ('wzr','xzr'):
            out.append(f'{dst}=little_read(c->q[{int(name[1:])}]+{lane*size},{size});')
    elif op in ('mov','ins') and len(args)==2 and args[0].type==ARM64_OP_REG and args[0].vector_index>=0 and args[1].type==ARM64_OP_REG:
        # INS (general), also shown as MOV. Only the selected vector lane
        # changes; WZR/XZR contributes zero like any other general register.
        destination=i.reg_name(args[0].reg);source=i.reg_name(args[1].reg)
        word=int.from_bytes(i.bytes,'little');imm5=(word>>16)&31;size=imm5&-imm5
        expected_vas={1:A64.ARM64_VAS_1B,2:A64.ARM64_VAS_1H,4:A64.ARM64_VAS_1S,8:A64.ARM64_VAS_1D}
        if word&0xffe0fc00!=0x4e001c00 or size not in expected_vas:raise Unsupported('INS general encoding')
        lane=args[0].vector_index
        if not re.fullmatch(r'v(?:[0-9]|[12][0-9]|3[01])',destination) or lane!=imm5//(2*size) or args[0].vas!=expected_vas[size]:raise Unsupported('INS general vector lane')
        value,width=register(args[1])
        source_number={'wzr':31,'xzr':31,'fp':29,'lr':30}.get(source)
        if source_number is None:
            if not re.fullmatch(r'[wx](?:[0-9]|[12][0-9]|30)',source):raise Unsupported('INS general source register')
            source_number=int(source[1:])
        if width!=(64 if size==8 else 32) or int(destination[1:])!=(word&31) or source_number!=((word>>5)&31):raise Unsupported('INS general register encoding')
        out.append(f'little_write(c->q[{int(destination[1:])}]+{lane*size},{value},{size});')
    elif op=='mov' and args[0].type==ARM64_OP_REG and i.reg_name(args[0].reg).startswith('v'):
        if len(args)!=2 or any(o.type!=ARM64_OP_REG or o.vector_index!=-1 for o in args):raise Unsupported('Whole-vector MOV only')
        names=[i.reg_name(o.reg) for o in args]
        if not all(re.fullmatch(r'v(?:[0-9]|[12][0-9]|3[01])',n) for n in names) or args[0].vas!=args[1].vas:raise Unsupported('Matching vector MOV operands required')
        size={A64.ARM64_VAS_8B:8,A64.ARM64_VAS_16B:16}.get(args[0].vas)
        if not size:raise Unsupported('Vector MOV arrangement')
        dst,src=[int(n[1:]) for n in names]
        out.append(f'{{uint8_t bits[16]={{0}};memcpy(bits,c->q[{src}],{size});memcpy(c->q[{dst}],bits,16);}}')
    elif op in ('fneg','fabs'):
        if len(args)!=2 or any(o.type!=ARM64_OP_REG or o.vector_index!=-1 for o in args):raise Unsupported('Floating sign register shape')
        names=[i.reg_name(o.reg) for o in args]
        if all(re.fullmatch(r'v(?:[0-9]|[12][0-9]|3[01])',n) for n in names) and args[0].vas==args[1].vas:
            shape={A64.ARM64_VAS_2S:(2,4),A64.ARM64_VAS_4S:(4,4),A64.ARM64_VAS_2D:(2,8)}.get(args[0].vas)
        elif all(re.fullmatch(r'[sd](?:[0-9]|[12][0-9]|3[01])',n) for n in names) and names[0][0]==names[1][0]:
            shape=(1,4 if names[0][0]=='s' else 8)
        else:shape=None
        if not shape:raise Unsupported('Floating sign scalar or matching vector form')
        lanes,size=shape;dst,src=[int(n[1:]) for n in names]
        out.append(f'{{uint8_t bits[16]={{0}};memcpy(bits,c->q[{src}],{lanes*size});')
        for lane in range(lanes):out.append(f'bits[{(lane+1)*size-1}]'+('^=128;' if op=='fneg' else '&=127;'))
        out.append(f'memcpy(c->q[{dst}],bits,16);}}')
    elif op=='ld1r':
        if len(args)!=2 or args[0].type!=ARM64_OP_REG or args[0].vector_index!=-1 or args[1].type!=ARM64_OP_MEM or i.writeback:raise Unsupported('LD1R no-offset operands')
        name=i.reg_name(args[0].reg)
        if not re.fullmatch(r'v(?:[0-9]|[12][0-9]|3[01])',name):raise Unsupported('LD1R vector register')
        shape={A64.ARM64_VAS_8B:(8,1),A64.ARM64_VAS_16B:(16,1),A64.ARM64_VAS_4H:(8,2),A64.ARM64_VAS_8H:(16,2),A64.ARM64_VAS_2S:(8,4),A64.ARM64_VAS_4S:(16,4),A64.ARM64_VAS_2D:(16,8)}.get(args[0].vas)
        if not shape:raise Unsupported('LD1R vector arrangement')
        size,element=shape;mem=args[1].mem
        if mem.index or mem.disp:raise Unsupported('LD1R no-offset address')
        basename=i.reg_name(mem.base);base,width=regname(basename)
        if width!=64 or base.startswith('UINT'):raise Unsupported('LD1R base width')
        if basename=='sp':out.append(f'if(!boot_sp(c,{pc}))return c->status;')
        out.append(f'{{uint8_t*p=boot_access_read(c,{base},{element},{pc});if(!p)return c->status;uint8_t result[16]={{0}};for(unsigned lane=0;lane<{size//element};++lane)memcpy(result+lane*{element},p,{element});memcpy(c->q[{int(name[1:])}],result,16);}}')
    elif op=='ld2':
        if len(args)!=3 or any(o.type!=ARM64_OP_REG or o.vector_index!=-1 for o in args[:2]) or args[2].type!=ARM64_OP_MEM:raise Unsupported('LD2 operands')
        names=[i.reg_name(o.reg) for o in args[:2]]
        if not all(re.fullmatch(r'v(?:[0-9]|[12][0-9]|3[01])',n) for n in names) or args[0].vas!=args[1].vas:raise Unsupported('LD2 matching vectors')
        first,second=[int(n[1:]) for n in names]
        if second!=(first+1)%32:raise Unsupported('LD2 consecutive vectors')
        shape={A64.ARM64_VAS_8B:(8,1),A64.ARM64_VAS_16B:(16,1),A64.ARM64_VAS_4H:(8,2),A64.ARM64_VAS_8H:(16,2),A64.ARM64_VAS_2S:(8,4),A64.ARM64_VAS_4S:(16,4),A64.ARM64_VAS_2D:(16,8)}.get(args[0].vas)
        if not shape:raise Unsupported('LD2 vector arrangement')
        size,element=shape;mem=args[2].mem
        if mem.index or mem.disp:raise Unsupported('LD2 no-offset address')
        if i.writeback:
            word=int.from_bytes(i.bytes,'little')
            if not i.post_index or ((word>>16)&31)!=31 or not i.op_str.endswith(f'#{2*size}'):raise Unsupported('LD2 immediate post-index')
        basename=i.reg_name(mem.base);base,width=regname(basename)
        if width!=64 or base.startswith('UINT'):raise Unsupported('LD2 base width')
        if basename=='sp':out.append(f'if(!boot_sp(c,{pc}))return c->status;')
        out.append(f'{{uint8_t*p=boot_access_read(c,{base},{2*size},{pc});if(!p)return c->status;uint8_t a[16]={{0}},b[16]={{0}};')
        out.append(f'for(unsigned lane=0;lane<{size//element};++lane){{memcpy(a+lane*{element},p+lane*{2*element},{element});memcpy(b+lane*{element},p+lane*{2*element}+{element},{element});}}memcpy(c->q[{first}],a,16);memcpy(c->q[{second}],b,16);'+(f'{base}+={2*size};' if i.writeback else '')+'}')
    elif op=='st2':
        # ST2 (multiple structures): interleave lane k of Vt and Vt2 as element pairs.
        if len(args)!=3 or any(o.type!=ARM64_OP_REG or o.vector_index!=-1 for o in args[:2]) or args[2].type!=ARM64_OP_MEM:raise Unsupported('ST2 operands')
        names=[i.reg_name(o.reg) for o in args[:2]]
        if not all(re.fullmatch(r'v(?:[0-9]|[12][0-9]|3[01])',n) for n in names) or args[0].vas!=args[1].vas:raise Unsupported('ST2 matching vectors')
        first,second=[int(n[1:]) for n in names]
        if second!=(first+1)%32:raise Unsupported('ST2 consecutive vectors')
        shape={A64.ARM64_VAS_8B:(8,1),A64.ARM64_VAS_16B:(16,1),A64.ARM64_VAS_4H:(8,2),A64.ARM64_VAS_8H:(16,2),A64.ARM64_VAS_2S:(8,4),A64.ARM64_VAS_4S:(16,4),A64.ARM64_VAS_2D:(16,8)}.get(args[0].vas)
        if not shape:raise Unsupported('ST2 vector arrangement')
        size,element=shape;mem=args[2].mem
        if mem.index or mem.disp:raise Unsupported('ST2 no-offset address')
        if i.writeback:
            word=int.from_bytes(i.bytes,'little')
            if not i.post_index or ((word>>16)&31)!=31 or not i.op_str.endswith(f'#{2*size}'):raise Unsupported('ST2 immediate post-index')
        basename=i.reg_name(mem.base);base,width=regname(basename)
        if width!=64 or base.startswith('UINT'):raise Unsupported('ST2 base width')
        if basename=='sp':out.append(f'if(!boot_sp(c,{pc}))return c->status;')
        out.append(f'{{uint8_t p[{2*size}];')
        out.append(f'for(unsigned lane=0;lane<{size//element};++lane){{memcpy(p+lane*{2*element},c->q[{first}]+lane*{element},{element});memcpy(p+lane*{2*element}+{element},c->q[{second}]+lane*{element},{element});}}'
                   f'if(!boot_store_bytes(c,{base},{2*size},{pc},p))return c->status;'+(f'{base}+={2*size};' if i.writeback else '')+'}')
    elif op in ('st1','ld1'):
        if len(args)!=2 or args[0].type!=ARM64_OP_REG or args[1].type!=ARM64_OP_MEM:raise Unsupported('Single SIMD lane memory form only')
        name=i.reg_name(args[0].reg);mem=args[1].mem
        sizes={A64.ARM64_VAS_1B:1,A64.ARM64_VAS_1H:2,A64.ARM64_VAS_1S:4,A64.ARM64_VAS_1D:8}
        size=sizes.get(args[0].vas);lane=args[0].vector_index
        if not size or not re.fullmatch(r'v(?:[0-9]|[12][0-9]|3[01])',name) or not 0<=lane<16//size:raise Unsupported('Invalid SIMD lane')
        if mem.index or mem.disp:raise Unsupported('SIMD lane base address only')
        basename=i.reg_name(mem.base);base,width=regname(basename)
        if width!=64 or base.startswith('UINT'):raise Unsupported('Invalid SIMD lane base')
        if basename=='sp':out.append(f'if(!boot_sp(c,{pc}))return c->status;')
        load=op=='ld1';q=f'c->q[{int(name[1:])}]+{lane*size}'
        if load:out.append(f'{{uint8_t*p=boot_access_read(c,{base},{size},{pc});if(!p)return c->status;memcpy({q},p,{size});}}')
        else:out.append(f'if(!boot_store_bytes(c,{base},{size},{pc},{q}))return c->status;')
    elif op=='fmov' and len(args)==2 and all(o.type==ARM64_OP_REG for o in args):
        names=[i.reg_name(o.reg) for o in args]
        scalar=re.compile(r'[sd](?:[0-9]|[12][0-9]|3[01])')
        if scalar.fullmatch(names[0]):
            size=4 if names[0][0]=='s' else 8;dst=int(names[0][1:])
            if scalar.fullmatch(names[1]) and names[1][0]==names[0][0]:
                out.append(f'{{uint{size*8}_t bits;memcpy(&bits,c->q[{int(names[1][1:])}],{size});BOOT_QZ(c->q[{dst}],bits,{size});}}')
            else:
                value,width=register(args[1])
                if width!=size*8:raise Unsupported('FMOV matching general register width')
                out.append(f'{{uint{width}_t bits=(uint{width}_t)({value});BOOT_QZ(c->q[{dst}],bits,{size});}}')
        elif scalar.fullmatch(names[1]):
            dst,width=register(args[0],True);size=4 if names[1][0]=='s' else 8
            if width!=size*8:raise Unsupported('FMOV matching general register width')
            out.append(f'{{uint{width}_t bits;memcpy(&bits,c->q[{int(names[1][1:])}],{size});{dst}=bits;}}')
        else:raise Unsupported('FMOV register shape')
    elif op in ('dup','fmov','movi','mvni'):
        if len(args)!=2 or args[0].type!=ARM64_OP_REG:raise Unsupported('SIMD copy operand shape')
        name=i.reg_name(args[0].reg)
        layouts={A64.ARM64_VAS_8B:(8,1),A64.ARM64_VAS_16B:(16,1),A64.ARM64_VAS_4H:(4,2),A64.ARM64_VAS_8H:(8,2),A64.ARM64_VAS_2S:(2,4),A64.ARM64_VAS_4S:(4,4),A64.ARM64_VAS_2D:(2,8)}
        if re.fullmatch(r'v(?:[0-9]|[12][0-9]|3[01])',name) and args[0].vas in layouts:
            lanes,size=layouts[args[0].vas]
        elif (op=='fmov' and re.fullmatch(r'[sd](?:[0-9]|[12][0-9]|3[01])',name)) or (op=='movi' and re.fullmatch(r'd(?:[0-9]|[12][0-9]|3[01])',name)):
            lanes,size=1,{'s':4,'d':8}[name[0]]
        else:raise Unsupported('SIMD copy destination form')
        if op=='dup':
            source=i.reg_name(args[1].reg) if args[1].type==ARM64_OP_REG else ''
            if re.fullmatch(r'v(?:[0-9]|[12][0-9]|3[01])',source):
                element_sizes={A64.ARM64_VAS_1B:1,A64.ARM64_VAS_1H:2,A64.ARM64_VAS_1S:4,A64.ARM64_VAS_1D:8}
                # Capstone omits VAS for D[index]; validate its encoded size.
                word=int.from_bytes(i.bytes,'little');imm5=(word>>16)&31
                source_size=element_sizes.get(args[1].vas,8 if args[1].vas==A64.ARM64_VAS_INVALID and imm5&-imm5==8 else 0)
                if source_size!=size or not 0<=args[1].vector_index<16//size or imm5!=size|(args[1].vector_index*size<<1):raise Unsupported('DUP element shape')
                value=f'little_read(c->q[{int(source[1:])}]+{args[1].vector_index*size},{size})'
            else:
                value,width=register(args[1])
                if width!=(64 if size==8 else 32) or args[1].vector_index!=-1:raise Unsupported('DUP requires general register or element')
        elif op=='mvni':
            if args[1].type!=ARM64_OP_IMM or not 0<=args[1].imm<=255 or size not in (2,4):raise Unsupported('MVNI halfword/word immediate')
            shift=args[1].shift
            if shift.type in (0,A64.ARM64_SFT_LSL) and shift.value in range(0,size*8,8):expanded=args[1].imm<<shift.value
            elif shift.type==A64.ARM64_SFT_MSL and size==4 and shift.value in (8,16):expanded=(args[1].imm<<shift.value)|((1<<shift.value)-1)
            else:raise Unsupported('MVNI immediate shift')
            value=f'UINT32_C({(~expanded)&((1<<(size*8))-1)})'
        elif op=='movi':
            if args[1].type!=ARM64_OP_IMM:raise Unsupported('MOVI immediate required')
            if args[1].shift.type==A64.ARM64_SFT_MSL:
                if size!=4 or not 0<=args[1].imm<=255 or args[1].shift.value not in (8,16):raise Unsupported('MOVI MSL immediate')
                shift=args[1].shift.value;value=f'UINT32_C({(args[1].imm<<shift)|((1<<shift)-1)})'
            else:value=operand(args[1],size*8)
        else:
            if args[1].type!=A64.ARM64_OP_FP or size not in (4,8):raise Unsupported('FMOV immediate only')
            value='UINT64_C(0x'+struct.pack('<f' if size==4 else '<d',args[1].fp)[::-1].hex()+')'
        q=f'c->q[{int(name[1:])}]'
        # Translator batch: one 16-byte register write (the lanes, zeros above them) instead of a zeroing store and a
        # store per lane, so the next 16-byte read of the register forwards from a single store.
        t=f'uint{size*8}_t';elements=','.join(['value']*lanes+['0']*(16//size-lanes))
        out.append(f'{{{t} value=({t})({value});typedef {t} boot_qv_ __attribute__((vector_size(16)));boot_qv_ qv_={{{elements}}};boot_q16({q},&qv_,BOOT_QLINE({q}));}}')
    elif op in ('fmin','fmax') and len(args)==3 and all(o.type==ARM64_OP_REG and re.fullmatch(r'[sd](?:[0-9]|[12][0-9]|3[01])',i.reg_name(o.reg)) for o in args):
        names=[i.reg_name(o.reg) for o in args]
        if len({name[0] for name in names})!=1 or any(o.vector_index!=-1 for o in args):raise Unsupported('FMIN scalar matching operands')
        width=32 if names[0][0]=='s' else 64;size=width//8;dst,left,right=[int(name[1:]) for name in names]
        from float_emit import flush_inputs
        sign='UINT64_C(0x8000000000000000)' if width==64 else 'UINT32_C(0x80000000)'
        magnitude='UINT64_C(0x7fffffffffffffff)' if width==64 else 'UINT32_C(0x7fffffff)'
        infinity='UINT64_C(0x7ff0000000000000)' if width==64 else 'UINT32_C(0x7f800000)'
        quiet='UINT64_C(0x8000000000000)' if width==64 else 'UINT32_C(0x400000)'
        out.append(f'{{uint{width}_t a,b,r;memcpy(&a,c->q[{left}],{size});memcpy(&b,c->q[{right}],{size});')
        out.append(f'if(c->fpcr&~UINT32_C(0x3000000)){{boot_fault(c,BOOT_FP_SCOPE,{pc},0,{size});return c->status;}}')
        out.append(flush_inputs(width,'a','b'))
        out.append(f'uint{width}_t am=a&{magnitude},bm=b&{magnitude};if(am>{infinity} || bm>{infinity}){{')
        out.append(f'if(am>{infinity} && !(a&{quiet})){{r=a|{quiet};c->fpsr|=1;}}else if(bm>{infinity} && !(b&{quiet})){{r=b|{quiet};c->fpsr|=1;}}else r=am>{infinity}?a:b;if(c->fpcr&0x2000000)r={infinity}|{quiet};')
        relation='>' if op=='fmax' else '<'
        out.append(f'}}else{{uint{width}_t ak=(a&{sign})?~a:(a|{sign}),bk=(b&{sign})?~b:(b|{sign});r=ak{relation}bk?a:b;}}BOOT_QZ(c->q[{dst}],r,{size});}}')
    elif op in ('fmin','fmax','fmaxp','fminp') and len(args)==3 and all(o.type==ARM64_OP_REG and re.fullmatch(r'v(?:[0-9]|[12][0-9]|3[01])',i.reg_name(o.reg)) for o in args):
        if any(o.vector_index!=-1 or o.vas!=args[0].vas for o in args):raise Unsupported('FMIN matching vector operands')
        shape={A64.ARM64_VAS_2S:(2,32),A64.ARM64_VAS_4S:(4,32),A64.ARM64_VAS_2D:(2,64)}.get(args[0].vas)
        if not shape:raise Unsupported('FMIN vector arrangement')
        lanes,width=shape;size=width//8;dst,left,right=[int(i.reg_name(o.reg)[1:]) for o in args]
        from float_emit import flush_inputs
        sign='UINT64_C(0x8000000000000000)' if width==64 else 'UINT32_C(0x80000000)'
        magnitude='UINT64_C(0x7fffffffffffffff)' if width==64 else 'UINT32_C(0x7fffffff)'
        infinity='UINT64_C(0x7ff0000000000000)' if width==64 else 'UINT32_C(0x7f800000)'
        quiet='UINT64_C(0x8000000000000)' if width==64 else 'UINT32_C(0x400000)'
        out.append(f'{{if(c->fpcr&~UINT32_C(0x3000000)){{boot_fault(c,BOOT_FP_SCOPE,{pc},0,{size});return c->status;}}uint8_t result[16]={{0}};')
        if op in ('fmaxp','fminp'):
            out.append(f'for(unsigned lane=0;lane<{lanes};++lane){{uint{width}_t a,b,r;const uint8_t*p=c->q[lane<{lanes//2}?{left}:{right}]+(lane%{lanes//2})*{size*2};memcpy(&a,p,{size});memcpy(&b,p+{size},{size});')
        else:
            out.append(f'for(unsigned lane=0;lane<{lanes};++lane){{uint{width}_t a,b,r;memcpy(&a,c->q[{left}]+lane*{size},{size});memcpy(&b,c->q[{right}]+lane*{size},{size});')
        out.append(flush_inputs(width,'a','b'))
        out.append(f'uint{width}_t am=a&{magnitude},bm=b&{magnitude};if(am>{infinity} || bm>{infinity}){{')
        out.append(f'if(am>{infinity} && !(a&{quiet})){{r=a|{quiet};c->fpsr|=1;}}else if(bm>{infinity} && !(b&{quiet})){{r=b|{quiet};c->fpsr|=1;}}else r=am>{infinity}?a:b;if(c->fpcr&0x2000000)r={infinity}|{quiet};')
        relation='>' if op in ('fmax','fmaxp') else '<'
        out.append(f'}}else{{uint{width}_t ak=(a&{sign})?~a:(a|{sign}),bk=(b&{sign})?~b:(b|{sign});r=ak{relation}bk?a:b;}}memcpy(result+lane*{size},&r,{size});}}memcpy(c->q[{dst}],result,16);}}')
    elif op in ('fadd','fsub','fabd','fmul','faddp','fmla','fmls') and len(args)==3 and all(o.type==ARM64_OP_REG and re.fullmatch(r'v(?:[0-9]|[12][0-9]|3[01])',i.reg_name(o.reg)) for o in args):
        shape={A64.ARM64_VAS_2S:(2,4),A64.ARM64_VAS_4S:(4,4),A64.ARM64_VAS_2D:(2,8)}.get(args[0].vas)
        if not shape:raise Unsupported('Floating vector arrangement')
        lanes,size=shape;width=size*8;dst,left,right=[int(i.reg_name(o.reg)[1:]) for o in args]
        indexed=args[2].vector_index!=-1
        if any(o.vector_index!=-1 or o.vas!=args[0].vas for o in args[:2]):raise Unsupported('Matching floating vector arrangements required')
        if indexed:
            allowed_vas=(A64.ARM64_VAS_1S,) if size==4 else (A64.ARM64_VAS_1D,A64.ARM64_VAS_INVALID)
            if op not in ('fmul','fmla','fmls') or args[2].vas not in allowed_vas or not 0<=args[2].vector_index<16//size or ((int.from_bytes(i.bytes,'little')>>22)&1)!=(size==8):raise Unsupported('Floating element shape')
        elif args[2].vas!=args[0].vas:raise Unsupported('Matching floating vector arrangements required')
        fused=op in ('fmla','fmls')
        # FZ, DN (and RMode for the fused forms) are modelled; Inf/NaN lanes follow
        # FPProcessNaNs and the FPAdd/FPSub/FPMul special cases (float_emit.special_binary).
        profile='0x3c00000' if fused else '0x3000000'
        out.append(f'{{if(c->fpcr&~UINT32_C({profile})){{boot_fault(c,BOOT_FP_SCOPE,{pc},0,{size});return c->status;}}')
        # Patch 58: play build (BOOT_FP_NATIVE): one packed host operation (FMA for FMLA/FMLS) on the whole register
        # and one result-side check (boot_fp_native.h); any refused lane sends the whole instruction to a cold
        # out-of-line helper: the exact per-lane code (boot_fpx_*, generated from float_emit.py into boot_fp_paths.h,
        # also the exact build) or, for FMLA/FMLS, patch 57's per-lane MXCSR-isolated FMA then exact (boot_fps_*).
        from float_emit import fp_vector_call
        out.append(fp_vector_call(op,lanes,size,dst,left,right,args[2].vector_index if indexed else None)+'}')
    elif op=='fmul' and len(args)==3 and all(o.type==ARM64_OP_REG for o in args) and re.fullmatch(r'[sd](?:[0-9]|[12][0-9]|3[01])',i.reg_name(args[0].reg)) and re.fullmatch(r'v(?:[0-9]|[12][0-9]|3[01])',i.reg_name(args[2].reg)):
        names=[i.reg_name(o.reg) for o in args]
        if names[1][0]!=names[0][0] or not re.fullmatch(r'[sd](?:[0-9]|[12][0-9]|3[01])',names[1]) or args[0].vector_index!=-1 or args[1].vector_index!=-1:raise Unsupported('Indexed scalar FMUL registers')
        width=32 if names[0][0]=='s' else 64;size=width//8;lane=args[2].vector_index
        allowed=(A64.ARM64_VAS_1S,) if width==32 else (A64.ARM64_VAS_1D,A64.ARM64_VAS_INVALID)
        if args[2].vas not in allowed or not 0<=lane<16//size:raise Unsupported('Indexed scalar FMUL element')
        dst,left,right=[int(name[1:]) for name in names]
        out.append(f'{{uint{width}_t a,b,r;memcpy(&a,c->q[{left}],{size});memcpy(&b,c->q[{right}]+{lane*size},{size});')
        out.append(f'if(c->fpcr&~UINT32_C(0x3000000)){{boot_fault(c,BOOT_FP_SCOPE,{pc},0,{size});return c->status;}}')
        # Patch 58: native host multiply as the scalar FMUL (boot_fp_native.h), else the exact text as a cold helper
        # (the S form keeps its own exact text; the D form's exact text is the scalar FMUL D text).
        from float_emit import fp_scalar_call
        out.append(fp_scalar_call('fmul',width,'boot_fpx_fmul_se' if width==32 else None))
        out.append(f'BOOT_QZ(c->q[{dst}],r,{size});}}')
    elif op in ('fmadd','fmsub','fnmadd','fnmsub'):
        names=[i.reg_name(o.reg) if o.type==ARM64_OP_REG else '' for o in args]
        if len(names)!=4 or not all(re.fullmatch(r'[sd](?:[0-9]|[12][0-9]|3[01])',name) for name in names) or len({name[0] for name in names})!=1 or any(o.vector_index!=-1 for o in args):
            raise Unsupported('Scalar fused multiply-add operands')
        width=32 if names[0][0]=='s' else 64;size=width//8
        dst,left,right,addend=[int(name[1:]) for name in names]
        out.append(f'{{uint{width}_t a,b,acc,r;memcpy(&a,c->q[{left}],{size});memcpy(&b,c->q[{right}],{size});memcpy(&acc,c->q[{addend}],{size});')
        out.append(f'if(c->fpcr&~UINT32_C(0x3c00000)){{boot_fault(c,BOOT_FP_SCOPE,{pc},0,{size});return c->status;}}')
        if op in ('fmsub','fnmadd'):out.append(f'a^=UINT{width}_C({1<<(width-1)});')
        if op in ('fnmadd','fnmsub'):out.append(f'acc^=UINT{width}_C({1<<(width-1)});')
        # Patch 58 (C1): one inline host FMA with a result check (boot_fp_native.h); else patch 57's MXCSR-isolated
        # helper and the exact code, both out of line (boot_fps_fma_*, boot_fp_paths.h).
        from float_emit import fp_fma_call
        out.append(fp_fma_call(width))
        out.append(f'BOOT_QZ(c->q[{dst}],r,{size});}}')
    elif op in ('fadd','fsub','fmul','fnmul','fdiv') and len(args)==3 and all(o.type==ARM64_OP_REG and re.fullmatch(r'd(?:[0-9]|[12][0-9]|3[01])',i.reg_name(o.reg)) for o in args):
        dst,left,right=[int(i.reg_name(o.reg)[1:]) for o in args]
        out.append(f'{{uint64_t a,b,r;memcpy(&a,c->q[{left}],8);memcpy(&b,c->q[{right}],8);')
        out.append(f'if(c->fpcr&~UINT32_C(0x3000000)){{boot_fault(c,BOOT_FP_SCOPE,{pc},0,8);return c->status;}}')
        # Play build (BOOT_FP_NATIVE): the host IEEE operation (MXCSR default: round to nearest, the only mode
        # the FPCR check above admits; no FTZ/DAZ), kept when it is provably the Arm result with FPSR unchanged
        # (boot_fp_native.h: IXC already set, no subnormal input, finite and not tiny result). Otherwise the
        # Arm-exact code, now a shared cold helper (boot_fp_paths.h; FNMUL negates NaN results there too).
        # Patch 58: FPSR IXC/UFC/OFC are exact on the native path as well (it no longer drops IXC).
        from float_emit import fp_scalar_call
        out.append(fp_scalar_call(op,64))
        out.append(f'BOOT_QZ(c->q[{dst}],r,8);}}')
    elif op in ('fadd','fsub','fmul','fnmul','fdiv'):
        names=[i.reg_name(o.reg) if o.type==ARM64_OP_REG else '' for o in args]
        if len(names)!=3 or not all(re.fullmatch(r's(?:[0-9]|[12][0-9]|3[01])',n) for n in names):raise Unsupported('FADD scalar binary32 only')
        dst,left,right=[int(n[1:]) for n in names]
        out.append(f'{{uint32_t a,b,r;memcpy(&a,c->q[{left}],4);memcpy(&b,c->q[{right}],4);')
        out.append(f'if(c->fpcr&~UINT32_C(0x3000000)){{boot_fault(c,BOOT_FP_SCOPE,{pc},0,4);return c->status;}}')
        # Play build (BOOT_FP_NATIVE): the host IEEE operation (MXCSR default: round to nearest, the only mode
        # the FPCR check above admits; no FTZ/DAZ), kept when it is provably the Arm result with FPSR unchanged
        # (boot_fp_native.h). Otherwise the Arm-exact code (FPProcessNaNs, the FPAdd/FPSub/FPMul/FPDiv special
        # cases, FZ, then the integer arithmetic), now a shared cold helper (boot_fp_paths.h, generated from
        # float_emit._scalar32_exact). Patch 58: FPSR IXC/UFC/OFC are exact on the native path as well.
        from float_emit import fp_scalar_call
        out.append(fp_scalar_call(op,32))
        out.append(f'BOOT_QZ(c->q[{dst}],r,4);}}')
    elif op in ('crc32b','crc32h','crc32w','crc32x','crc32cb','crc32ch','crc32cw','crc32cx'):
        if len(args)!=3:raise Unsupported('CRC32 operand shape')
        discard=args[0].type==ARM64_OP_REG and i.reg_name(args[0].reg)=='wzr'
        dst,width=register(args[0],not discard);seed,seedwidth=register(args[1]);value,valuewidth=register(args[2])
        size={'b':1,'h':2,'w':4,'x':8}[op[-1]]
        polynomial=0x82f63b78 if op.startswith('crc32c') else 0xedb88320
        if width!=32 or seedwidth!=32 or valuewidth!=(64 if size==8 else 32):raise Unsupported('CRC32 register widths')
        out.append(f'{{uint32_t crc=(uint32_t)({seed});uint64_t data={value};')
        out.append(f'for(unsigned byte=0;byte<{size};++byte){{crc^=(uint32_t)(data&255);data>>=8;for(unsigned bit=0;bit<8;++bit)crc=(crc>>1)^((crc&1)?UINT32_C(0x{polynomial:x}):0);}}')
        out.append('(void)crc;}' if discard else f'{dst}=crc;}}')
    elif op in ('smull','umull','smaddl','smsubl','umsubl'):
        if len(args)!=(4 if op in ('smaddl','smsubl','umsubl') else 3):raise Unsupported('Long multiply operand shape')
        dst,width=register(args[0],True);left,lwidth=register(args[1]);right,rwidth=register(args[2])
        if (width,lwidth,rwidth)!=(64,32,32):raise Unsupported('Long multiply register widths')
        product=f'(uint64_t)((int64_t)boot_sign_extend({left},32)*(int64_t)boot_sign_extend({right},32))' if op not in ('umull','umsubl') else f'((uint64_t)(uint32_t)({left})*(uint64_t)(uint32_t)({right}))'
        if len(args)==4:
            accumulator,awidth=register(args[3])
            if awidth!=64:raise Unsupported('Long multiply accumulator width')
            product=f'({accumulator})'+('+' if op=='smaddl' else '-')+f'({product})'
        out.append(f'{dst}={product};')
    elif op=='sdiv':
        if len(args)!=3:raise Unsupported('SDIV operand shape')
        dst,width=register(args[0],True);left,lwidth=register(args[1]);right,rwidth=register(args[2])
        if lwidth!=width or rwidth!=width:raise Unsupported('SDIV register widths')
        out.append(f'{{int64_t a=(int64_t)boot_sign_extend({left},{width}),b=(int64_t)boot_sign_extend({right},{width});')
        out.append(f'{dst}=(uint{width}_t)(!b?0:(a==INT64_MIN && b==-1)?INT64_MIN:a/b);}}')
    elif op=='extr':
        dst,width=register(args[0],True);high=operand(args[1],width);low=operand(args[2],width)
        if args[3].type!=ARM64_OP_IMM or not 0<=args[3].imm<width:raise Unsupported('EXTR shift')
        shift=args[3].imm
        assignment(dst,width,low if shift==0 else f'(({low}) >> {shift}) | (({high}) << {width-shift})')
    elif op in ('ldar','ldarb','ldarh','stlr','stlrb','stlrh'):
        load=op.startswith('ld');mem=args[1].mem
        if args[1].type!=ARM64_OP_MEM or mem.index or mem.disp:raise Unsupported('Acquire/release address form')
        zero=i.reg_name(args[0].reg) in ('wzr','xzr')
        base,_=regname(i.reg_name(mem.base));value,width=register(args[0],load and not zero)
        if load and zero:value='discard0'
        size=1 if op.endswith('b') else 2 if op.endswith('h') else width//8
        out.append(f'if(({base})&{size-1}){{boot_fault(c,CH_ARGUMENT,{pc},{base},{size});return c->status;}}')
        if load:
            out.append(f'if(!boot_load(c,{base},{size},{pc},&{value}))return c->status;')
            out.append('atomic_thread_fence(memory_order_acquire);')
        else:
            # ARMv8 store-release is RCsc: a later LDAR may not pass it. x86 needs a locked store.
            out.append(f'if(!boot_store_release(c,{base},{size},{pc},{value}))return c->status;')
    elif op in ('ldaxp','ldxp','stlxp','stxp'):
        load=op.startswith('ld');expected=3 if load else 4
        if len(args)!=expected or args[-1].type!=ARM64_OP_MEM:raise Unsupported('Exclusive pair operand shape')
        mem=args[-1].mem
        if mem.index or mem.disp:raise Unsupported('Exclusive pair address form')
        base,bw=regname(i.reg_name(mem.base))
        if bw!=64:raise Unsupported('Exclusive pair base width')
        if load:
            names=[i.reg_name(args[k].reg) for k in (0,1)]
            if args[0].type!=ARM64_OP_REG or args[1].type!=ARM64_OP_REG or (names[0]==names[1] and names[0] not in ('wzr','xzr')):
                raise Unsupported('Exclusive pair destinations')
            widths=[register(args[k])[1] for k in (0,1)]
            if widths[0]!=widths[1] or widths[0] not in (32,64):raise Unsupported('Exclusive pair widths')
            dest=[f'discard{k}' if names[k] in ('wzr','xzr') else register(args[k],True)[0] for k in (0,1)]
            out.append(f'if(!boot_load_exclusive_pair(c,{base},{widths[0]//8},{pc},{1 if op=="ldaxp" else 0},&{dest[0]},&{dest[1]}))return c->status;')
        else:
            status_name=i.reg_name(args[0].reg)
            status,status_width=register(args[0],status_name not in ('wzr','xzr'))
            if status_name in ('wzr','xzr'):status='discard0'
            low,low_width=register(args[1]);high,high_width=register(args[2])
            if status_width!=32 or low_width!=high_width or low_width not in (32,64):raise Unsupported('Exclusive pair store widths')
            out.append(f'if(!boot_store_exclusive_pair(c,{base},{low_width//8},{pc},{1 if op=="stlxp" else 0},{low},{high},&{status}))return c->status;')
    elif op in ('ldaxr','ldxr','stlxr','stxr','ldaxrb','ldxrb','stlxrb','stxrb'):
        load=op.startswith('ld');mi=1 if load else 2
        mem=args[mi].mem
        if args[mi].type!=ARM64_OP_MEM or mem.index or mem.disp:raise Unsupported('Exclusive address form')
        base,bw=regname(i.reg_name(mem.base))
        if bw!=64:raise Unsupported('Exclusive base width')
        if load:
            zero=i.reg_name(args[0].reg) in ('wzr','xzr')
            dst,width=register(args[0],not zero)
            if zero:dst='discard0'
            out.append(f'if(!boot_load_exclusive(c,{base},{1 if op.endswith("b") else width//8},{pc},&{dst}))return c->status;')
        else:
            zero=i.reg_name(args[0].reg) in ('wzr','xzr')
            dst,dw=register(args[0],not zero);value,width=register(args[1])
            if zero:dst='discard0'
            if dw!=32:raise Unsupported('Exclusive result width')
            out.append(f'if(!boot_store_exclusive(c,{base},{1 if op.endswith("b") else width//8},{pc},{value},&{dst}))return c->status;')
    elif op=='clrex':out.append('c->exclusive_valid=0;')
    elif op=='rev':
        if len(args)!=2:raise Unsupported('REV operand count')
        dst,width=register(args[0],True);src,source_width=register(args[1])
        if width!=source_width or width not in (32,64):raise Unsupported('REV width')
        assignment(dst,width,f'__builtin_bswap{width}((uint{width}_t)({src}))')
    elif op=='rev32':
        if len(args)!=2:raise Unsupported('REV32 operand count')
        dst,width=register(args[0],True);src,source_width=register(args[1])
        if width!=64 or source_width!=64:raise Unsupported('REV32 width')
        assignment(dst,width,f'((uint64_t)__builtin_bswap32((uint32_t)({src}))) | ((uint64_t)__builtin_bswap32((uint32_t)(({src})>>32))<<32)')
    elif op=='rev16':
        if len(args)!=2:raise Unsupported('REV16 operand count')
        dst,width=register(args[0],True);src,source_width=register(args[1])
        if width!=source_width or width not in (32,64):raise Unsupported('REV16 width')
        low=int('00ff'*(width//16),16)
        high=int('ff00'*(width//16),16)
        assignment(dst,width,f'((({src}) & UINT64_C(0x{low:x})) << 8) | ((({src}) & UINT64_C(0x{high:x})) >> 8)')
    elif op in ('adc','adcs','sbc','sbcs','ngc','ngcs') and args and args[0].type==ARM64_OP_REG and not i.reg_name(args[0].reg).startswith('v'):
        # Add/subtract with carry: SBC a,b = ADC a,~b; NGC b = SBC zr,b. Flags as AddWithCarry().
        dst,width=register(args[0],True) if i.reg_name(args[0].reg) not in ('wzr','xzr') else (None,32 if i.reg_name(args[0].reg)=='wzr' else 64)
        if op in ('ngc','ngcs'):
            if len(args)!=2:raise Unsupported('NGC operands')
            a='UINT64_C(0)';b,bw=register(args[1])
        else:
            if len(args)!=3:raise Unsupported('ADC/SBC operands')
            a,aw=register(args[1]);b,bw=register(args[2])
            if aw!=width:raise Unsupported('ADC/SBC mixed width')
        if bw!=width:raise Unsupported('ADC/SBC mixed width')
        mask='UINT64_MAX' if width==64 else 'UINT64_C(0xffffffff)'
        invert=op.startswith(('sbc','ngc'))
        lines=[f'{{uint64_t a_=({a})&{mask},b_=({"~" if invert else ""}({b}))&{mask},ci_=c->carry?1:0;']
        if width==64:lines.append('uint64_t s_=a_+b_,r_=s_+ci_;unsigned co_=(s_<a_)|(r_<s_);')
        else:lines.append('uint64_t full_=a_+b_+ci_,r_=full_&UINT64_C(0xffffffff);unsigned co_=(unsigned)(full_>>32);')
        if op.endswith('s'):
            sign=f'UINT64_C({1<<(width-1)})'
            lines.append(f'c->n=(r_&{sign})!=0;c->z=r_==0;c->carry=co_;c->v=(~(a_^b_)&(a_^r_)&{sign})!=0;')
        else:lines.append('(void)co_;')
        if dst is not None:lines.append(f'{dst}=(uint{width}_t)r_;')
        lines.append('}')
        out.append(''.join(lines))
    elif op=='negs' and args and args[0].type==ARM64_OP_REG and not i.reg_name(args[0].reg).startswith('v'):
        if len(args)!=2:raise Unsupported('NEGS operands')
        name=i.reg_name(args[0].reg)
        width=32 if name.startswith('w') else 64
        b=operand(args[1],width)
        if name in ('wzr','xzr'):out.append(f'(void)boot_sub_flags(c,0,{b},{width});')
        else:
            dst,_=register(args[0],True);assignment(dst,width,f'boot_sub_flags(c,0,{b},{width})')
    elif op in ('mov','mvn','movz','movn','movk','add','adds','sub','subs','and','orr','orn','eor','eon','bic','mul','mneg','neg','sxtw','bfi','bfc','bfxil',
                'lsl','lsr','asr','ror','rbit','clz','udiv','umulh','smulh','madd','msub','umaddl','sxtb','sxth','sbfiz','ubfiz','sbfx','ubfx'):
        dst,width=register(args[0],True)
        if i.update_flags and op not in ('adds','subs'):raise Unsupported('Unexpected arithmetic flags')
        if op in ('movz','movn'):
            if len(args)!=2 or args[1].type!=ARM64_OP_IMM or args[1].shift.type not in (ARM64_SFT_INVALID,ARM64_SFT_LSL):raise Unsupported('Wide move form')
            shift=args[1].shift.value
            if shift%16 or shift+16>width or not 0<=args[1].imm<=65535:raise Unsupported('Wide move range')
            value=args[1].imm<<shift
            if op=='movn':value=(~value)&((1<<width)-1)
            assignment(dst,width,f'UINT64_C(0x{value:x})')
        elif op=='movk':
            if len(args)!=2 or args[1].type!=ARM64_OP_IMM or args[1].shift.type not in (ARM64_SFT_INVALID,ARM64_SFT_LSL):raise Unsupported('MOVK form')
            shift=args[1].shift.value
            if shift%16 or shift+16>width or not 0<=args[1].imm<=65535:raise Unsupported('MOVK range')
            mask=((1<<width)-1)^(0xffff<<shift)
            assignment(dst,width,f'({dst}&UINT64_C(0x{mask:x}))|UINT64_C(0x{args[1].imm<<shift:x})')
        elif op=='sxtw':
            src,w=register(args[1])
            if width!=64 or w!=32:raise Unsupported('SXTW form')
            assignment(dst,width,f'(uint64_t)({src}) | ((({src})&UINT32_C(0x80000000))?UINT64_C(0xffffffff00000000):0)')
        elif op=='bfxil':
            if len(args)!=4 or any(o.type!=ARM64_OP_IMM for o in args[2:]):raise Unsupported('BFXIL form')
            shift,size=args[2].imm,args[3].imm
            if not 0<=shift<width or not 1<=size<=width-shift:raise Unsupported('BFXIL width')
            src=operand(args[1],width);low=(1<<size)-1;keep=((1<<width)-1)^low
            assignment(dst,width,f'({dst}&UINT64_C(0x{keep:x})) | ((({src}) >> {shift})&UINT64_C(0x{low:x}))')
        elif op=='bfc':
            if len(args)!=3 or any(o.type!=ARM64_OP_IMM for o in args[1:]):raise Unsupported('BFC form')
            shift,size=args[1].imm,args[2].imm
            if not 0<=shift<width or not 1<=size<=width-shift:raise Unsupported('BFC width')
            keep=((1<<width)-1)^(((1<<size)-1)<<shift)
            assignment(dst,width,f'({dst}&UINT64_C(0x{keep:x}))')
        elif op=='bfi':
            if len(args)!=4 or any(o.type!=ARM64_OP_IMM for o in args[2:]):raise Unsupported('BFI form')
            shift,size=args[2].imm,args[3].imm
            if not 0<=shift<width or not 1<=size<=width-shift:raise Unsupported('BFI width')
            src=operand(args[1],width);low=(1<<size)-1;keep=((1<<width)-1)^(low<<shift)
            assignment(dst,width,f'({dst}&UINT64_C(0x{keep:x})) | ((({src})&UINT64_C(0x{low:x})) << {shift})')
        elif op in ('sbfiz','ubfiz','sbfx','ubfx'):
            a=operand(args[1],width);shift=args[2].imm;bits=args[3].imm
            if not 0<=shift<width or not 1<=bits<=width-shift:raise Unsupported('Bitfield range')
            if op=='sbfiz':expr=f'boot_sign_extend(({a}),{bits}) << {shift}'
            elif op=='ubfiz':expr=f'(({a}) & UINT64_C({(1<<bits)-1})) << {shift}'
            elif op=='sbfx':expr=f'boot_sign_extend(({a}) >> {shift},{bits})'
            else:expr=f'(({a}) >> {shift}) & UINT64_C({(1<<bits)-1})'
            assignment(dst,width,expr)
        elif op in ('sxtb','sxth'):
            a,source_width=register(args[1])
            if source_width!=32:raise Unsupported('Small signed extension requires word source')
            assignment(dst,width,f'boot_sign_extend({a},{8 if op=="sxtb" else 16})')
        elif op=='umaddl':
            a,wa=register(args[1]);b,wb=register(args[2]);d,wd=register(args[3])
            if (width,wa,wb,wd)!=(64,32,32,64):raise Unsupported('UMADDL widths')
            assignment(dst,64,f'(uint64_t)({a})*(uint64_t)({b})+({d})')
        else:
            a=operand(args[1],width)
            if op=='mov':expr=a
            elif op=='mvn':expr=f'~({a})'
            elif op=='neg':expr=f'UINT64_C(0)-({a})'
            elif op in ('rbit','clz'):expr=f'boot_{op}({a},{width})'
            else:
                b=operand(args[2],width)
                if op=='subs':expr=f'boot_sub_flags(c,{a},{b},{width})'
                elif op=='adds':expr=f'boot_add_flags(c,{a},{b},{width})'
                elif op in ('lsl','lsr'):expr=f'({a}) '+('<<' if op=='lsl' else '>>')+f' (({b})&{width-1})'
                elif op=='ror':expr=f'(({a}) >> (({b})&{width-1})) | (({a}) << (({width}-(({b})&{width-1}))&{width-1}))'
                elif op=='asr':expr=f'boot_sign_extend(({a}) >> (({b})&{width-1}),{width}-(({b})&{width-1}))'
                elif op=='udiv':expr=f'(({b})?({a})/({b}):0)'
                elif op=='umulh':expr=f'boot_umulh({a},{b})'
                elif op=='smulh':
                    if width!=64:raise Unsupported('SMULH width')
                    expr=f'(boot_umulh({a},{b})-((({a})>>63)?({b}):0)-((({b})>>63)?({a}):0))'
                elif op in ('madd','msub'):
                    d=operand(args[3],width);expr=f'({d}) '+('+' if op=='madd' else '-')+f' (({a})*({b}))'
                elif op=='mneg':expr=f'0-(({a})*({b}))'
                elif op=='bic':expr=f'({a})&~({b})'
                elif op=='orn':expr=f'({a})|~({b})'
                elif op=='eon':expr=f'({a})^~({b})'
                else:expr=f'({a}) '+{'add':'+','sub':'-','and':'&','orr':'|','eor':'^','mul':'*'}[op]+f' ({b})'
            assignment(dst,width,expr)
    elif op in ('cmp','cmn'):
        a,width=register(args[0]);b=operand(args[1],width)
        out.append(f'(void)boot_{"sub" if op=="cmp" else "add"}_flags(c,{a},{b},{width});')
    elif op in ('ccmp','ccmn'):
        condition=i.op_str.rsplit(', ',1)[-1]
        if condition not in CONDITIONS:raise Unsupported('CCMP condition')
        a,width=register(args[0]);b=operand(args[1],width);nzcv=args[2].imm
        if args[2].type!=ARM64_OP_IMM or not 0<=nzcv<=15:raise Unsupported('CCMP fallback flags')
        out.append(f'if({CONDITIONS[condition]})(void)boot_{"sub" if op=="ccmp" else "add"}_flags(c,{a},{b},{width});')
        out.append(f'else{{c->n={(nzcv>>3)&1};c->z={(nzcv>>2)&1};c->carry={(nzcv>>1)&1};c->v={nzcv&1};}}')
    elif op in ('tst','ands','bics'):
        index=0 if op=='tst' else 1
        a,width=register(args[index]);b=operand(args[index+1],width)
        if op=='bics':b=f'~({b})'
        out.append(f'address=(uint{width}_t)(({a})&({b}));c->n=(address>>{width-1})&1;c->z=address==0;c->carry=c->v=0;')
        if op!='tst':
            discard=i.reg_name(args[0].reg) in ('xzr','wzr')
            dst,dwidth=register(args[0],not discard)
            if dwidth!=width:raise Unsupported('ANDS widths')
            if not discard:assignment(dst,width,'address')
    elif op in ('cset','csetm','csel','csinc','csinv','csneg','cinc','cneg','cinv'):
        condition=i.op_str.rsplit(', ',1)[-1]
        conditions=CONDITIONS
        if condition not in conditions:raise Unsupported('Unsupported select condition')
        dst,width=register(args[0],True)
        a='UINT64_MAX' if op=='csetm' else '1' if op=='cset' else operand(args[1],width)
        b='0' if op in ('cset','csetm') else a if op in ('cinc','cneg','cinv') else operand(args[2],width)
        if op=='cinc':a=f'({a})+UINT64_C(1)'
        if op=='cneg':a=f'UINT64_C(0)-({a})'
        if op=='cinv':a=f'~({a})'
        if op=='csinc':b=f'({b})+UINT64_C(1)'
        if op=='csinv':b=f'~({b})'
        if op=='csneg':b=f'UINT64_C(0)-({b})'
        assignment(dst,width,f'({conditions[condition]})?({a}):({b})')
    elif op=='nop':pass
    elif op in ('autia1716','autib1716'):
        expected=0xd503219f if op=='autia1716' else 0xd50321df
        if args or int.from_bytes(i.bytes,'little')!=expected:raise Unsupported('PAuth hint encoding')
        # Pinned ARMv8.0 guest profile has no FEAT_PAuth: these hint encodings
        # are architecturally NOP, like XPACLRI below.
        pass
    elif op=='xpaclri':
        if args or int.from_bytes(i.bytes,'little')!=0xd50320ff:raise Unsupported('XPACLRI hint encoding')
        # Pinned ARMv8.0 guest profile has no FEAT_PAuth. The system-hint
        # encoding is architecturally NOP; do not strip bits from LR.
        pass
    elif op in ('wfe','yield','sev','sevl'):
        if args:raise Unsupported('Event hint operands')
        # Always-awake hint profile: no guest thread is parked on an event.
        # WFE/YIELD offer a scheduling point; SEV/SEVL need no wake bookkeeping.
        if op in ('wfe','yield'):
            out.extend(['__builtin_ia32_pause();',
                'if(c->thread_enabled && c->thread_count>1 && c->thread_count<=BOOT_THREAD_CAPACITY && c->current_thread<c->thread_count){',
                'unsigned chosen=c->current_thread;',
                'for(unsigned k=1;k<c->thread_count;++k){unsigned j=(c->current_thread+k)%c->thread_count;',
                'if(c->threads[j].state==2 && (chosen==c->current_thread || c->threads[j].priority<c->threads[chosen].priority))chosen=j;}',
                f'if(chosen!=c->current_thread){{boot_save_thread(c,{pc}+4);boot_load_thread(c,chosen,&target);goto indirect;}}',
                '}'])
            indirect=True
    elif op in ('prfm','prfum'):
        hint=i.op_str.split(',',1)[0]
        if not re.fullmatch(r'(?:pld|pli|pst)l[123](?:keep|strm)',hint) or len(args)!=1 or args[0].type not in (ARM64_OP_MEM,ARM64_OP_IMM):
            raise Unsupported('Prefetch hint scope')
        # Optional cache hint: this host profile performs no prefetch. It must
        # not turn an unmapped hint address into an architectural memory fault.
        pass
    elif op=='dc':
        if len(args)!=2 or i.op_str.split(', ',1)[0]!='civac':raise Unsupported('Data-cache operation scope')
        value,width=register(args[1])
        if width!=64:raise Unsupported('Data-cache address width')
        out.append(f'{{uint64_t line=({value})&~UINT64_C(63);uint8_t*p=boot_access(c,line,64,READ,{pc});if(!p)return c->status;')
        # The validated x86 host has 64-byte CLFLUSH lines. Guest and host
        # allocations need not have matching line alignment, so cover both ends.
        out.append('__builtin_ia32_clflush(p);if((uintptr_t)p&63)__builtin_ia32_clflush(p+63);atomic_thread_fence(memory_order_seq_cst);}')
    elif op=='isb':
        if int.from_bytes(i.bytes,'little')!=0xd5033fdf:raise Unsupported('ISB full-system scope only')
        # Guest executable text is pinned AOT code. Serialize the actual host
        # instruction stream; dynamic guest code generation remains unsupported.
        out.append('{uint32_t a=0,b,d,e;__asm__ volatile("cpuid":"+a"(a),"=b"(b),"=c"(d),"=d"(e)::"memory");}')
    elif op in ('dsb','dmb'):
        if (op,i.op_str) not in (('dsb','sy'),('dsb','ish'),('dmb','sy'),('dmb','st'),('dmb','ish'),('dmb','ishst'),('dmb','ishld')):raise Unsupported('Unsupported barrier domain')
        # Normal host RAM; no device memory. x86-TSO already orders load->load/store and
        # store->store, so DMB ISHLD/ISHST only need to stop compiler reordering; DMB ISH and
        # DSB SY order store->load too and stay full fences.
        if (op,i.op_str) in (('dmb','ishld'),('dmb','ishst'),('dmb','st')):out.append('atomic_signal_fence(memory_order_seq_cst);')
        else:out.append('atomic_thread_fence(memory_order_seq_cst);')
    elif op=='mrs':
        system_register=i.op_str.rsplit(', ',1)[-1]
        if system_register not in ('tpidrro_el0','tpidr_el0','cntpct_el0','fpcr','fpsr','ctr_el0'):raise Unsupported('Unsupported system register')
        dst,width=register(args[0],True)
        if system_register=='tpidrro_el0':
            out.append(f'if(!c->tpidrro){{boot_fault(c,BOOT_SYSREG,{pc},0,0);return c->status;}}')
            assignment(dst,width,'c->tpidrro')
        elif system_register=='tpidr_el0':assignment(dst,width,'c->tpidr')
        elif system_register in ('fpcr','fpsr'):assignment(dst,width,f'c->{system_register}')
        elif system_register=='ctr_el0':assignment(dst,width,'UINT64_C(0x8444c004)')
        else:out.append(f'if(!boot_system_tick(c,{pc},&{dst}))return c->status;')
    elif op=='msr':
        system_register=i.op_str.split(', ',1)[0]
        if system_register not in ('tpidr_el0','fpcr','fpsr','nzcv'):raise Unsupported('Unsupported system register write')
        source,_=register(args[1])
        if system_register=='nzcv':
            out.append(f'{{uint64_t flags=({source});c->n=(flags>>31)&1;c->z=(flags>>30)&1;c->carry=(flags>>29)&1;c->v=(flags>>28)&1;}}')
        elif system_register=='tpidr_el0':out.append(f'c->tpidr={source};')
        else:
            mask='0x03000000' if system_register=='fpcr' else '0x0800009f'
            out.append(f'if((uint32_t)({source})&~UINT32_C({mask})){{boot_fault(c,BOOT_SYSREG,{pc},0,4);return c->status;}}')
            out.append(f'c->{system_register}=(uint32_t)({source});')
    elif op in ('adr','adrp'):
        dst,width=register(args[0],True)
        if width!=64 or args[1].type!=ARM64_OP_IMM:raise Unsupported('ADR form')
        assignment(dst,64,f'c->rtld_base+UINT64_C({args[1].imm})')
    elif op in ('b','bl'):
        if len(args)!=1 or args[0].type!=ARM64_OP_IMM:raise Unsupported('Direct branch form')
        if op=='bl':out.append(f'c->x[30]=c->rtld_base+UINT64_C({next_address});')
        out.append(f'goto L_{args[0].imm:x};');edges=[args[0].imm]
    elif op.startswith('b.'):
        conditions=CONDITIONS
        if op[2:] not in conditions or len(args)!=1 or args[0].type!=ARM64_OP_IMM:raise Unsupported('Conditional branch')
        target=args[0].imm;out.append(f'if({conditions[op[2:]]})goto L_{target:x};');edges.append(target)
    elif op in ('cbz','cbnz'):
        a,_=register(args[0]);target=args[1].imm
        out.append(f'if(({a}){"==" if op=="cbz" else "!="}0)goto L_{target:x};');edges.append(target)
    elif op in ('tbz','tbnz'):
        a,width=register(args[0]);bit=args[1].imm;target=args[2].imm
        if not 0<=bit<width:raise Unsupported('Test bit range')
        out.append(f'if((({a})&(UINT64_C(1)<<{bit})){"==" if op=="tbz" else "!="}0)goto L_{target:x};');edges.append(target)
    elif op in ('br','ret','blr'):
        target='c->x[30]' if op=='ret' else register(args[0])[0]
        out.append(f'target={target};')
        if op=='blr':out.append(f'c->x[30]=c->rtld_base+UINT64_C({next_address});')
        out.append('goto indirect;');edges=[];indirect=True
    elif op=='svc':
        if len(args)!=1 or args[0].type!=ARM64_OP_IMM:raise Unsupported('SVC form')
        out.append('c->exclusive_valid=0;')
        # One out-of-line kernel entry per SVC site (boot_svc in boot_runtime.h holds the bodies that
        # used to be emitted here; 0 = not handled -> unsupported-SVC stop below).
        if args[0].imm in (1, 3, 4, 5, 6, 8, 9, 10, 11, 12, 13, 14, 15, 16, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 31, 33, 37, 41, 44, 45, 52, 53):
            out.append(f'{{int svc_action=boot_svc(c,{pc},boot_address(c,UINT64_C({next_address})),{args[0].imm},&target);'
                       f'if(svc_action<0)return c->status;if(svc_action==1)goto L_{next_address:x};if(svc_action==2)goto indirect;}}')
        out.extend([f'c->svc_id={args[0].imm};',f'boot_fault(c,BOOT_SVC,{pc},0,0);','return c->status;'])
        edges=[next_address] if args[0].imm in (1,4,5,6,8,9,10,14,15,0x17,0x18,0x19,0x1a,0x1b,0x29,0xc,0x25,0x1f,0x16,0x21,0x2c,0x2d) else []
        if args[0].imm in (6,0x29,0xc,0xd,0x10,0x13,0x14,0x15,0x25):edges=[next_address]
        if args[0].imm in (0x1c,0x1d,0x34,0x35):edges=[next_address];indirect=True
        if args[0].imm==0xb:edges=[next_address];indirect=True
    else:raise Unsupported(f'Unsupported {op} {i.op_str}')
    if op not in ('b','bl','br','blr','ret','svc'):out.append(f'goto L_{next_address:x};')
    return out,edges,indirect

def main():
    write_service_profile()
    raw=(ROOT.parent/'uncompressed'/'rtld').read_bytes()
    if hashlib.sha256(raw).hexdigest()!=EXPECTED:raise Unsupported('rtld input changed')
    off,va,size=struct.unpack_from('<III',raw,16)
    md=Cs(CS_ARCH_ARM64,CS_MODE_ARM);md.detail=True
    selected={p for start,end in RANGES for p in range(start,end,4)}-LITERALS
    sdk=(ROOT.parent/'uncompressed'/'sdk').read_bytes()
    if hashlib.sha256(sdk).hexdigest()!=SDK_EXPECTED:raise Unsupported('sdk input changed')
    sdk_off,sdk_va,sdk_size=struct.unpack_from('<III',sdk,16)
    sdk_selected={0x30000000+p for start,end in SDK_RANGES for p in range(start,end,4)}
    selected.update(sdk_selected)
    subsdk=(ROOT.parent/'uncompressed'/'subsdk0').read_bytes()
    if hashlib.sha256(subsdk).hexdigest()!=SUBSDK_EXPECTED:raise Unsupported('subsdk0 input changed')
    subsdk_off,subsdk_va,subsdk_size=struct.unpack_from('<III',subsdk,16)
    subsdk_selected={0x20000000+p for start,end in SUBSDK_RANGES for p in range(start,end,4)}
    selected.update(subsdk_selected)
    main_raw=(ROOT.parent/'uncompressed'/'main').read_bytes()
    if hashlib.sha256(main_raw).hexdigest()!=MAIN_EXPECTED:raise Unsupported('main input changed')
    main_off,main_va,main_size=struct.unpack_from('<III',main_raw,16)
    main_selected={0x10000000+p for start,end in MAIN_RANGES for p in range(start,end,4)}
    selected.update(main_selected)
    source=['#include "boot_runtime.h"',
            '__declspec(dllexport) int boot_open_romfs(BootContext*c,const wchar_t*path){if(!c || c->storage_native)return 0;BootStorage*s=boot_storage_open(path);if(!s)return 0;c->storage_native=(uint64_t)(uintptr_t)s;return 1;}',
            '__declspec(dllexport) int boot_open_host_window(BootContext*c,int visible){return c?boot_attach_window(c,visible):0;}',
            '__declspec(dllexport) uint64_t boot_host_window_handle(BootContext*c){return c && c->window_native?(uint64_t)(uintptr_t)boot_window_handle((BootWindow*)(uintptr_t)c->window_native):0;}',
            '__declspec(dllexport) int boot_test_window_focus(BootContext*c,uint32_t focus){if(!c || !c->window_native || (focus!=1 && focus!=3))return 0;boot_window_focus((BootWindow*)(uintptr_t)c->window_native,focus);return 1;}',
            '__declspec(dllexport) void boot_free_owned_memory(BootContext*c){if(c){boot_release_events(c);boot_release_memory(c);}}',
            '__declspec(dllexport) void boot_configure_layout(BootContext*c){if(c)boot_default_layout(c);}',
            '__declspec(dllexport) uint64_t boot_context_size(void){return sizeof(BootContext);}',
            '__declspec(dllexport) uint32_t boot_test_subflags(uint64_t a,uint64_t b,uint32_t w){',
            '    if(w!=32 && w!=64)return UINT32_MAX;BootContext c={0};boot_sub_flags(&c,a,b,w);',
            '    return (c.n<<3)|(c.z<<2)|(c.carry<<1)|c.v;}',
            '__declspec(dllexport) uint64_t boot_test_integer(uint32_t op,uint64_t a,uint64_t b){',
            '    switch(op){case 0:return boot_umulh(a,b);case 1:return boot_rbit(a,64);',
            '    case 2:return boot_clz(a,64);case 3:return boot_rbit((uint32_t)a,32);',
            '    case 4:return boot_clz((uint32_t)a,32);default:return UINT64_MAX;}}',
            '__declspec(dllexport) uint32_t boot_run(BootContext*c,uint64_t entry_offset){',
            '    if(!c)return CH_ARGUMENT;',
            '    c->status=c->trace_count=c->svc_id=c->query_count=c->info_count=c->layout_query_count=c->thread_query_count=c->exclusive_valid=0;c->steps=c->fault_pc=c->fault_address=c->fault_width=0;',
            '    if(!boot_valid(c)){c->status=CH_ARGUMENT;return c->status;}',
            '    uint64_t address=0,writeback=0,discard0=0,discard1=0,target=c->rtld_base+entry_offset;',
            '    (void)discard0;(void)discard1;',
            '    goto indirect;']
    manifest={'input_sha256':EXPECTED,'entry':0,'ranges':RANGES,'inline_data_offsets':sorted(LITERALS),
              'sdk_input_sha256':SDK_EXPECTED,'sdk_ranges':SDK_RANGES,
              'subsdk0_input_sha256':SUBSDK_EXPECTED,'subsdk0_ranges':SUBSDK_RANGES,
              'main_input_sha256':MAIN_EXPECTED,'main_ranges':MAIN_RANGES,
              'instructions':[],'external_edges':[],'indirect_registry':'Only generated labels; no opcode dispatch'}
    provenance_pending=ROOT/'provenance.json.tmp'
    provenance_prefix,_=json.dumps(manifest,separators=(',',':')).split('"instructions":[]',1)
    provenance_stream=provenance_pending.open('w',encoding='utf-8',buffering=1024*1024)
    provenance_stream.write(provenance_prefix+'"instructions":[')
    provenance_batch=[]
    provenance_count=0
    def flush_provenance():
        nonlocal provenance_count
        if not provenance_batch:return
        if provenance_count:provenance_stream.write(',')
        provenance_stream.write(json.dumps(provenance_batch,separators=(',',':'))[1:-1])
        provenance_count+=len(provenance_batch)
        provenance_batch.clear()
    external=set()
    from instruction_cache import InstructionCache
    import capstone,time
    cache_started=time.perf_counter()
    namespace=hashlib.sha256(Path(__file__).read_bytes()+(ROOT/'float_emit.py').read_bytes()+(ROOT/'float_emit_ext.py').read_bytes()+(ROOT/'simd_mem_ext.py').read_bytes()+(ROOT/'simd_arith_ext.py').read_bytes()+(ROOT/'instruction_cache.py').read_bytes()+(ROOT/'address_arbiter_emit.py').read_bytes()+(ROOT/'sleep_emit.py').read_bytes()+
                             (capstone.__version__+EXPECTED+SDK_EXPECTED+SUBSDK_EXPECTED+MAIN_EXPECTED).encode()).hexdigest()
    # Exact source-namespace aliases require review. Changed sleep, address
    # arbiter, and FPCR-write forms are decoded and refreshed below. Binary64
    # FDIV had no cached records because it was previously unsupported.
    aliases_path=ROOT/'instruction-cache-aliases.json'
    aliases=json.loads(aliases_path.read_text()) if aliases_path.exists() else {}
    effective_namespace=aliases.get(namespace,namespace)
    cache=InstructionCache(ROOT/'emitted-instructions.sqlite',effective_namespace)
    pages={}
    for p in sorted(selected):pages.setdefault(p>>12,[]).append(p)
    for offsets in pages.values():
        records=cache.get(offsets)
        if records is not None:
            for record in records:
                if record['instruction'] not in ('svc #0xb','svc #0x34','svc #0x35') and not record['instruction'].startswith(('msr fpcr, ','sub v','addv ')):continue
                p=record['offset']
                data,file_at,text_va,text_size=(sdk,sdk_off,sdk_va,sdk_size) if p in sdk_selected else (raw,off,va,size)
                if p in subsdk_selected:data,file_at,text_va,text_size=subsdk,subsdk_off,subsdk_va,subsdk_size
                if p in main_selected:data,file_at,text_va,text_size=main_raw,main_off,main_va,main_size
                relative=p&0xfffffff
                if not text_va<=relative<text_va+text_size:raise Unsupported('Cached sleep outside text')
                decoded=list(md.disasm(data[file_at+relative-text_va:file_at+relative-text_va+4],p))
                if len(decoded)!=1 or (record['instruction'].startswith('svc #') and (decoded[0].mnemonic!='svc' or f'svc {decoded[0].op_str}'!=record['instruction'])) or (record['instruction'].startswith('msr fpcr, ') and (decoded[0].mnemonic!='msr' or not decoded[0].op_str.startswith('fpcr, '))) or (record['instruction'].startswith(('sub v','addv ')) and f'{decoded[0].mnemonic} {decoded[0].op_str}'!=record['instruction']):
                    raise Unsupported('Cached system opcode mismatch')
                record['statements'],record['edges'],record['indirect']=emit(decoded[0])
        if records is None:
            records=[]
            for p in offsets:
                data,file_at,text_va,text_size=(sdk,sdk_off,sdk_va,sdk_size) if p in sdk_selected else (raw,off,va,size)
                if p in subsdk_selected:data,file_at,text_va,text_size=subsdk,subsdk_off,subsdk_va,subsdk_size
                if p in main_selected:data,file_at,text_va,text_size=main_raw,main_off,main_va,main_size
                relative=p&0xfffffff
                if not text_va<=relative<text_va+text_size:raise Unsupported('Outside text')
                decoded=list(md.disasm(data[file_at+relative-text_va:file_at+relative-text_va+4],p))
                if len(decoded)!=1:raise Unsupported(f'Invalid opcode at {p:#x}')
                i=decoded[0];statements,edges,indirect=emit(i)
                records.append({'offset':p,'instruction':f'{i.mnemonic} {i.op_str}','statements':statements,'edges':edges,'indirect':indirect})
            cache.put(offsets,records)
        for record in records:
            p=record['offset'];statements=record['statements'];edges=record['edges'];indirect=record['indirect'];instruction=record['instruction']
            source.extend([f'L_{p:x}: /* {instruction} */',f'    if(!boot_tick(c,UINT64_C({p})))return c->status;'])
            source.extend('    '+statement for statement in statements)
            external.update(set(edges)-selected)
            provenance_batch.append({'offset':p,'instruction':instruction.rstrip(),'edges':edges,'indirect':indirect})
            if len(provenance_batch)>=4096:flush_provenance()
    cache_report={'page_hits':cache.hits,'page_misses':cache.misses,'namespace':effective_namespace,
                  'source_namespace':namespace,'seconds':time.perf_counter()-cache_started}
    cache.close()
    (ROOT/'instruction-cache-report.json').write_text(json.dumps(cache_report,indent=2)+'\n')
    for p in sorted(external):
        source.extend([f'L_{p:x}:',f'    boot_fault(c,BOOT_TARGET,c->rtld_base+UINT64_C({p}),c->rtld_base+UINT64_C({p}),0);',
                       '    return c->status;'])
    source.extend(['indirect:', '    if(target==c->return_sentinel)return c->status;',
                   '    switch(boot_offset(c,target)){'])
    for p in sorted(selected):source.append(f'        case {p}:goto L_{p:x};')
    source.extend(['        default:break;','    }',
                   '    boot_fault(c,BOOT_TARGET,target,target,0);return c->status;','}'])
    manifest['external_edges']=sorted(external)
    selected_count=len(selected)
    manifest['native_layout']={'source_page_bytes':4096,'functions':len(pages),
                               'cross_page_transfer':'nonrecursive native return loop'}
    ROOT.mkdir(exist_ok=True)
    # Keep only a small provenance batch in memory. Publish after translated C succeeds.
    flush_provenance()
    if provenance_count!=selected_count:raise ValueError('Incomplete provenance stream')
    _,provenance_suffix=json.dumps(manifest,separators=(',',':')).split('"instructions":[]',1)
    provenance_stream.write(']'+provenance_suffix+'\n')
    provenance_stream.close()
    del manifest,selected,pages,raw,sdk,subsdk,main_raw
    generated='\n'.join(source)+'\n'
    del source
    generated=re.sub(r'c->rtld_base\+UINT64_C\((\d+)\)',r'boot_address(c,UINT64_C(\1))',generated)
    generated=generated.replace('c->rtld_base+entry_offset','boot_address(c,entry_offset)')
    (ROOT/'monolithic_startup.c').write_text(generated,encoding='utf-8',newline='\n')
    partition_pending=ROOT/'translated_startup.c.tmp'
    with partition_pending.open('w',encoding='utf-8',newline='\n',buffering=1024*1024) as partition_stream:
        partition(generated,output=partition_stream)
    partition_pending.replace(ROOT/'translated_startup.c')
    provenance_pending.replace(ROOT/'provenance.json')
    print(f'Generated {selected_count} static instruction translations; external edges: {[hex(x) for x in sorted(external)]}')

if __name__=='__main__':main()
