"""Static page-entry candidates for partition_native (ahead-of-time analysis only).

A translated page function only gets an entry-switch case for an instruction that can be entered
from outside its straight-line flow. Most such addresses follow from the page itself (branch targets,
return sites, resumable stop sites: partition_native). This module adds the addresses that can only
be reached by an indirect branch or from code that is not translated yet, found by scanning the
pinned module images (never at runtime):

  * function starts: defined FUNC symbols, BL targets, RELATIVE relocation targets in text,
    ADR and ADRP+ADD address materialisations into text, module offset 0;
  * instructions that follow an unconditional transfer (B, BR, RET): only reachable by a branch,
    i.e. function starts, jump-table cases or code after a tail call;
  * jump-table cases (CMP/B.HI bound, ADRP+ADD table, LDR{B,H,SW} entry, ADR base, ADD, BR).

Plus the entries the coverage loop recorded (discovered-entries.json): untranslated-entry stops
(BOOT_TARGET, fault_width 1) and every address a resume was requested at.
Offsets are logical (module prefix << 28 | module offset), as in translate_startup_incremental.
"""
import hashlib,json,os,struct
from pathlib import Path
STARTUP=Path(__file__).resolve().parent
REPO=Path(os.environ.get('TOTK_REPO_ROOT',str(STARTUP.parent)))   # holds uncompressed/ and call-chain/
MODULES=((0,'rtld'),(1,'main'),(2,'subsdk0'),(3,'sdk'))
CACHE=STARTUP/'entry-points-static.json'
EXTRA=STARTUP/'discovered-entries.json'
VERSION='entry-points-1'

def _module(name):
    import sys
    sys.path.insert(0,str(REPO/'call-chain'))
    from module_loader import Module,INPUT,BASES
    return Module(name,BASES[name],(INPUT/name).read_bytes())

def _sx(value,bits):
    return value-(1<<bits) if value>>(bits-1)&1 else value

def module_entries(m):
    """Set of module offsets that are entry candidates (see module docstring)."""
    import numpy as np
    text_at=m.regions[0]['offset'];text_size=m.regions[0]['size']
    image=bytes(m.image)
    words=np.frombuffer(image,dtype='<u4',count=text_size//4,offset=text_at).astype(np.int64)
    pcs=np.arange(text_at,text_at+4*len(words),4,dtype=np.int64)
    inside=lambda t:(t>=text_at)&(t<text_at+text_size)&((t&3)==0)
    out=set([text_at])
    for s in m.symbols:
        if s['type']==2 and s['section'] and text_at<=s['value']<text_at+text_size and s['value']%4==0:out.add(s['value'])
    for target,kind,symbol,addend in m.relocations:
        if kind==1027 and text_at<=addend<text_at+text_size and addend%4==0:out.add(addend)
    # BL targets
    bl=(words&0xFC000000)==0x94000000
    imm=words&0x3FFFFFF;imm=np.where(imm>=1<<25,imm-(1<<26),imm)
    t=pcs+imm*4;out.update(int(x) for x in t[bl&inside(t)])
    # ADR
    adr=(words&0x9F000000)==0x10000000
    imm=((words>>5)&0x7FFFF)<<2|((words>>29)&3);imm=np.where(imm>=1<<20,imm-(1<<21),imm)
    t=pcs+imm;out.update(int(x) for x in t[adr&inside(t)])
    # ADRP followed (within 8 instructions) by ADD Xd,Xn,#imm with Xn = the ADRP register
    adrp=np.nonzero((words&0x9F000000)==0x90000000)[0]
    for i in adrp.tolist():
        w=int(words[i]);rd=w&31
        immv=_sx(((w>>5)&0x7FFFF)<<2|((w>>29)&3),21)
        page=(int(pcs[i])&~0xfff)+(immv<<12)
        for j in range(i+1,min(i+9,len(words))):
            v=int(words[j])
            if (v&0xFF800000)==0x91000000 and (v>>5)&31==rd:
                target=page+(((v>>10)&0xFFF)<<(12 if (v>>22)&1 else 0))
                if text_at<=target<text_at+text_size and target%4==0:out.add(target)
                break
    # instruction after B / BR / RET
    after=((words&0xFC000000)==0x14000000)|((words&0xFFFFFC1F)==0xD61F0000)|((words&0xFFFFFC1F)==0xD65F0000)
    nxt=pcs[after]+4;out.update(int(x) for x in nxt[inside(nxt)])
    out.update(jump_table_targets(m,image,text_at,text_size,words))
    return out

def jump_table_targets(m,image,text_at,text_size,words,stats=None):
    """Cases of compiler jump tables (bounded by a preceding CMP + B.HI/B.HS). Unrecognised forms
    are left to the coverage loop's untranslated-entry stops."""
    import numpy as np
    from capstone import Cs,CS_ARCH_ARM64,CS_MODE_ARM
    from capstone.arm64 import ARM64_OP_REG,ARM64_OP_IMM,ARM64_OP_MEM
    md=Cs(CS_ARCH_ARM64,CS_MODE_ARM);md.detail=True
    out=set()
    brs=np.nonzero((words&0xFFFFFC1F)==0xD61F0000)[0]
    limit=len(image)
    for index in brs.tolist():
        start=max(0,index-64)
        at=text_at+4*start
        window=list(md.disasm(image[at:text_at+4*index+4],at))
        if len(window)!=index-start+1 or window[-1].mnemonic!='br':continue
        regs={}            # register name -> ('const',value) | ('load',table_reg_value,width,signed,index_reg,shift)
        bound={}           # index register name -> number of entries
        consts={}
        def name(op):return md.reg_name(op.reg)
        def canon(r):return r.replace('w','x',1) if r.startswith('w') else r
        last_cmp=None
        for k,i in enumerate(window[:-1]):
            ops=i.operands;mn=i.mnemonic
            try:
                if mn=='adrp' and ops[1].type==ARM64_OP_IMM:consts[canon(name(ops[0]))]=ops[1].imm;continue
                if mn=='adr' and ops[1].type==ARM64_OP_IMM:consts[canon(name(ops[0]))]=ops[1].imm;continue
                if mn=='add' and len(ops)==3 and ops[2].type==ARM64_OP_IMM and canon(name(ops[1])) in consts:
                    sh=ops[2].shift.value if ops[2].shift.type else 0
                    consts[canon(name(ops[0]))]=consts[canon(name(ops[1]))]+(ops[2].imm<<sh);continue
                if mn=='cmp' and len(ops)==2 and ops[1].type==ARM64_OP_IMM:last_cmp=(canon(name(ops[0])),ops[1].imm);continue
                if mn in ('cmp','cmn','subs','adds','ands','tst','ccmp','fcmp'):last_cmp=None;continue
                if mn in ('b.hi','b.hs') and last_cmp:
                    bound[last_cmp[0]]=last_cmp[1]+1 if mn=='b.hi' else last_cmp[1];continue
                if mn in ('ldrb','ldrh','ldrsw','ldr','ldrsb','ldrsh') and len(ops)==2 and ops[1].type==ARM64_OP_MEM and ops[1].mem.index:
                    base=canon(md.reg_name(ops[1].mem.base));idx=canon(md.reg_name(ops[1].mem.index))
                    width={'ldrb':1,'ldrsb':1,'ldrh':2,'ldrsh':2,'ldrsw':4}.get(mn,4 if name(ops[0]).startswith('w') else 8)
                    if base in consts:regs[canon(name(ops[0]))]=('load',consts[base],width,mn in ('ldrsw','ldrsb','ldrsh'),idx)
                    continue
                if mn=='add' and len(ops)==3 and ops[2].type==ARM64_OP_REG:
                    src=canon(name(ops[1]));off=canon(name(ops[2]))
                    if src in consts and off in regs:
                        sh=ops[2].shift.value if ops[2].shift.type else 0
                        ext=ops[2].ext
                        regs[canon(name(ops[0]))]=('target',consts[src],regs[off],sh,ext)
                    continue
                if mn=='mov' and len(ops)==2 and ops[1].type==ARM64_OP_REG:
                    s=canon(name(ops[1]));d=canon(name(ops[0]))
                    if s in consts:consts[d]=consts[s]
                    if s in bound:bound[d]=bound[s]
                    continue
            except Exception:
                continue
        target=regs.get(canon(md.reg_name(window[-1].operands[0].reg))) if window[-1].operands else None
        if stats is not None and any(i.mnemonic=='adr' for i in window[-8:]):
            stats.setdefault('adr_br',0);stats['adr_br']+=1
            if not target or target[0]!='target' or not bound.get(target[2][4]):stats.setdefault('miss',[]).append(window)
        if not target or target[0]!='target':continue
        _,base,load,shift,ext=target
        _,table,width,signed,idx=load
        count=bound.get(idx)
        if count is not None and count>4096:continue
        exact=count is not None
        if not exact:count=256 if width==1 else 1024   # unknown bound: over-approximate (extra entries are harmless)
        if not 0<=table<=limit-width*count:continue
        for e in range(count):
            raw=int.from_bytes(image[table+width*e:table+width*e+width],'little',signed=signed)
            if ext in (1,2,3):raw&=(1<<{1:8,2:16,3:32}[ext])-1              # uxtb/uxth/uxtw
            elif ext in (5,6,7):raw=_sx(raw&((1<<{5:8,6:16,7:32}[ext])-1),{5:8,6:16,7:32}[ext])   # sxtb/sxth/sxtw
            value=base+(raw<<shift)
            if not exact and not -0x40000<=value-base<0x40000:break
            if text_at<=value<text_at+text_size and value%4==0:out.add(value)
        if stats is not None:stats['tables']=stats.get('tables',0)+1
    return out

def static_entries():
    """Sorted logical offsets of all static entry candidates, cached by module and analysis hashes."""
    key=hashlib.sha256((VERSION+Path(__file__).read_text(encoding='utf-8')).encode())
    for prefix,name in MODULES:key.update(hashlib.sha256((REPO/'uncompressed'/name).read_bytes()).digest())
    key=key.hexdigest()
    try:
        cached=json.loads(CACHE.read_text())
        if cached.get('key')==key:return sorted(set(cached['entries'])|script_entries())
    except (OSError,ValueError):pass
    entries=[]
    for prefix,name in MODULES:
        entries.extend(prefix<<28|offset for offset in module_entries(_module(name)))
    entries=sorted(set(entries))
    pending=CACHE.with_name(CACHE.name+'.tmp')
    pending.write_text(json.dumps({'key':key,'entries':entries},separators=(',',':')));os.replace(pending,CACHE)
    return sorted(set(entries)|script_entries())

SCRIPTS=('test_*.py','inspect_*.py','run_*.py')
FROZEN_SCRIPT_ENTRIES=STARTUP/'script-entries.json'
def _frozen_script_entries():
    """script_entries() of the development tree, frozen (offsets only): its test, inspection and probe
    scripts are not part of the release repo, and without their literals the page entry switches would differ."""
    try:return json.loads(FROZEN_SCRIPT_ENTRIES.read_text())
    except OSError:return []
def script_entries():
    """Guest addresses the test, inspection and probe scripts start, stop (checkpoint_offset) or
    resume at: every 4-aligned hex literal of at least 0x400 in those scripts, and the next
    instruction (tests use ENTRY+4 checkpoints). Over-approximation only adds switch cases."""
    import re
    out=set(_frozen_script_entries())
    for pattern in SCRIPTS:
        for path in Path(os.environ.get('TOTK_ENTRY_SCRIPTS',str(STARTUP))).glob(pattern):   # env: testing only
            try:text=path.read_text(encoding='utf-8',errors='replace')
            except OSError:continue
            for literal in re.findall(r'(?<![0-9A-Za-z_])0x([0-9a-fA-F]{3,8})(?![0-9A-Za-z_])',text):
                value=int(literal,16)
                if value>=0x400 and value%4==0 and value<1<<30:out.update((value,value+4))
    return out

def extra_entries(path=None):
    """Logical offsets recorded by the coverage loop (list of ints or hex strings)."""
    path=Path(path) if path else EXTRA
    if not path.exists():return []
    return sorted({int(v,16) if isinstance(v,str) else int(v) for v in json.loads(path.read_text())})

def record_entry(offset,path=None):
    """Add one logical offset to discovered-entries.json (atomic replace); returns True if new."""
    path=Path(path) if path else EXTRA
    current=set(extra_entries(path))
    if offset in current:return False
    current.add(offset)
    pending=path.with_name(path.name+'.tmp')
    pending.write_text(json.dumps([hex(v) for v in sorted(current)],indent=0)+'\n');os.replace(pending,path)
    return True

def by_page(offsets):
    pages={}
    for offset in offsets:pages.setdefault(offset>>12,set()).add(offset)
    return pages

if __name__=='__main__':
    import sys,time
    t=time.time();e=static_entries()
    print(len(e),'static entries',round(time.time()-t,1),'s')
