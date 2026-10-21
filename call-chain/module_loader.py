"""Bounded NSO image loader and explicit ELF64-style RELA resolver.

This loads bytes and fixes pointers. It never executes module entry points,
constructors, imported ARM code, or Nintendo's runtime loader.
"""
import hashlib
import json
import struct
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent
INPUT = ROOT.parent/'uncompressed'
MASK64 = (1 << 64)-1
NAMES = ['rtld', 'main', 'subsdk0', 'sdk']
BASES = {name: 0x7100000000+i*0x10000000 for i,name in enumerate(NAMES)}


class LoadError(ValueError):
    pass


def span(data, offset, size):
    if offset < 0 or size < 0 or offset > len(data)-size:
        raise LoadError(f'Out-of-range metadata at {offset:#x}, size {size:#x}')


def align(value):
    return (value+4095)&~4095


class Module:
    def __init__(self, name, base, raw):
        if len(raw)<256 or raw[:4]!=b'NSO0':
            raise LoadError('Invalid NSO header')
        if struct.unpack_from('<I',raw,12)[0]&7:
            raise LoadError('Input must be decompressed')
        if base&4095 or not 0<base<1<<64:
            raise LoadError('Module base must be nonzero and page aligned')
        self.name,self.base,self.sha256=name,base,hashlib.sha256(raw).hexdigest()
        self.regions=[]
        descriptions=[]
        end=0
        for index in range(3):
            off,dest,size,extra=struct.unpack_from('<IIII',raw,16+16*index)
            span(raw,off,size)
            if (struct.unpack_from('<I',raw,12)[0]>>(index+3))&1:
                expected=raw[0xa0+32*index:0xc0+32*index]
                if hashlib.sha256(raw[off:off+size]).digest()!=expected:
                    raise LoadError('NSO segment hash mismatch')
            allocated=align(size+(extra if index==2 else 0))
            if dest&4095 or dest<end or dest+allocated>0x10000000:
                raise LoadError('Invalid, overlapping or excessive NSO segment')
            end=dest+allocated
            descriptions.append((off,dest,size,allocated))
            self.regions.append({'offset':dest,'size':allocated,'permissions':[5,1,3][index]})
        if end>MASK64-base:
            raise LoadError('Module address wraps')
        self.image=bytearray(end)
        for off,dest,size,_ in descriptions:
            self.image[dest:dest+size]=raw[off:off+size]
        self.bss_offset=descriptions[2][1]+descriptions[2][2]
        self.bss_size=struct.unpack_from('<I',raw,0x3c)[0]
        self.initial_bss_zero=not any(self.image[self.bss_offset:self.bss_offset+self.bss_size])
        mod=struct.unpack_from('<I',self.image,4)[0]
        span(self.image,mod,28)
        if self.image[mod:mod+4]!=b'MOD0':
            raise LoadError('Missing MOD0')
        dynamic=mod+struct.unpack_from('<i',self.image,mod+4)[0]
        self.tags=defaultdict(list)
        for i in range(4096):
            pos=dynamic+16*i
            span(self.image,pos,16)
            tag,value=struct.unpack_from('<qQ',self.image,pos)
            if tag==0:break
            self.tags[tag].append(value)
        else:raise LoadError('Unterminated dynamic table')
        # Known inputs have RELA and SysV hash tables; other representations fail closed.
        if any(t in self.tags for t in [17,18,19,35,36,37,0x6ffffff0,0x6ffffffc,0x6ffffffe]):
            raise LoadError('REL, RELR or symbol-version tables require additional implementation')
        if self.tag(11)!=24 or self.tag(9,24)!=24:
            raise LoadError('Unsupported symbol/RELA entry size')
        hash_at=self.tag(4)
        span(self.image,hash_at,8)
        buckets,count=struct.unpack_from('<II',self.image,hash_at)
        if count>1000000 or buckets>1000000:raise LoadError('Excessive hash table')
        span(self.image,hash_at,8+4*(buckets+count))
        strings_at,strings_size=self.tag(5),self.tag(10)
        span(self.image,strings_at,strings_size)
        strings=bytes(self.image[strings_at:strings_at+strings_size])
        def string(offset):
            if offset>=len(strings):raise LoadError('String offset out of bounds')
            end=strings.find(b'\0',offset)
            if end<0:raise LoadError('Unterminated symbol string')
            return strings[offset:end].decode('utf-8',errors='strict')
        self.needed=[string(n) for n in self.tags.get(1,[])]
        self.soname=string(self.tag(14)) if 14 in self.tags else name
        self.symbols=[]
        symbols_at=self.tag(6)
        span(self.image,symbols_at,24*count)
        for i in range(count):
            n,info,other,shndx,value,size=struct.unpack_from('<IBBHQQ',self.image,symbols_at+24*i)
            self.symbols.append({'name':string(n),'bind':info>>4,'type':info&15,
                                 'visibility':other&3,'section':shndx,'value':value,'size':size})
        self.relocations=[]
        tables=[]
        if 7 in self.tags:tables.append((self.tag(7),self.tag(8)))
        if 23 in self.tags:
            if self.tag(20)!=7:raise LoadError('PLT must use RELA')
            tables.append((self.tag(23),self.tag(2)))
        occupied=[]
        for off,size in tables:
            if size%24:raise LoadError('Partial RELA entry')
            span(self.image,off,size)
            if any(off<hi and lo<off+size for lo,hi in occupied):raise LoadError('Overlapping relocation tables')
            occupied.append((off,off+size))
            for pos in range(off,off+size,24):
                target,info,addend=struct.unpack_from('<QQq',self.image,pos)
                self.relocations.append((target,info&0xffffffff,info>>32,addend))

    def tag(self,tag,default=None):
        if tag not in self.tags:
            if default is not None:return default
            raise LoadError(f'Missing dynamic tag {tag:#x}')
        if len(self.tags[tag])!=1:raise LoadError('Duplicate singleton dynamic tag')
        return self.tags[tag][0]

    def mapped(self,offset,width):
        return any(r['offset']<=offset and offset+width<=r['offset']+r['size'] for r in self.regions)

    def symbol_address(self,symbol):
        if symbol['type'] in (6,10):raise LoadError('TLS/IFUNC symbol cannot be resolved as an ordinary address')
        if symbol['section']==0:raise LoadError('Undefined symbol has no address')
        if symbol['section']==0xfff1:return symbol['value']
        if symbol['section']>=0xff00:raise LoadError('Unsupported special symbol section')
        if not 0<=symbol['value']<=len(self.image):raise LoadError('Defined symbol outside image')
        return self.base+symbol['value']


class Loader:
    def __init__(self,modules):
        self.modules=modules
        self.exports=defaultdict(list)
        intervals=[]
        for m in modules:
            if any(m.base<hi and lo<m.base+len(m.image) for lo,hi in intervals):
                raise LoadError('Overlapping modules')
            intervals.append((m.base,m.base+len(m.image)))
            for s in m.symbols:
                if s['name'] and s['section'] and s['bind'] in (1,2) and s['visibility'] in (0,3):
                    self.exports[s['name']].append((m,s))

    def resolve(self,module,index):
        if index<0 or index>=len(module.symbols):raise LoadError('Invalid symbol index')
        if index==0:return 0,None,'null-symbol'
        s=module.symbols[index]
        if s['section'] and (s['bind']==0 or s['visibility'] in (1,2,3)):
            return module.symbol_address(s),module.name,'local'
        options=self.exports.get(s['name'],[])
        if options:
            owner,definition=options[0]  # Explicit scope order; no claim of Nintendo loader equivalence.
            return owner.symbol_address(definition),owner.name,'scope'
        if s['bind']==2:
            return 0,None,'unresolved-weak-zero'
        raise LoadError(f'Unresolved strong symbol: {s["name"]}')

    def relocate(self):
        report={'scope_order':[m.name for m in self.modules], 'modules':[],
                'applied':0,'cross_module_relocations':0,'unresolved':[], 'unsupported':[],
                'duplicate_export_names':sum(len(v)>1 for v in self.exports.values()),
                'duplicate_binding_policy':'First eligible definition in explicit module scope order'}
        sonames={m.soname for m in self.modules}
        report['missing_needed']=[{'module':m.name,'dependency':n} for m in self.modules for n in m.needed if n not in sonames]
        for m in self.modules:
            counts=Counter()
            applied=cross=weak=0
            examples=[]
            seen=set()
            for target,kind,index,addend in m.relocations:
                counts[kind]+=1
                if kind==0:continue
                if kind not in (257,1025,1026,1027):
                    report['unsupported'].append({'module':m.name,'target':hex(target),'type':kind})
                    continue
                if target in seen:raise LoadError('Multiple relocations write the same target')
                seen.add(target)
                if not m.mapped(target,8):raise LoadError('Relocation write outside mapped segment')
                owner=None
                if kind==1027:
                    if index!=0:raise LoadError('RELATIVE relocation has nonzero symbol index')
                    value=(m.base+addend)&MASK64
                else:
                    try:
                        address,owner,mode=self.resolve(m,index)
                    except LoadError as e:
                        report['unresolved'].append({'module':m.name,'target':hex(target),
                            'symbol_index':index,'reason':str(e)})
                        continue
                    value=(address+addend)&MASK64
                    if mode=='unresolved-weak-zero':weak+=1
                    if owner and owner!=m.name:
                        cross+=1
                        if len(examples)<5:examples.append({'symbol':m.symbols[index]['name'],
                            'target_offset':hex(target),'owner':owner,'resolved_address':hex(value)})
                struct.pack_into('<Q',m.image,target,value)
                applied+=1
            report['applied']+=applied
            report['cross_module_relocations']+=cross
            report['modules'].append({'name':m.name,'soname':m.soname,'base':hex(m.base),
                'bytes':len(m.image),'sha256':m.sha256,'needed':m.needed,'regions':m.regions,
                'bss_zero_before_relocation':m.initial_bss_zero,'symbols':len(m.symbols),
                'relocation_types':dict(counts),'applied':applied,'cross_module':cross,
                'unresolved_weak_zero':weak,'cross_module_examples':examples})
        report['complete_for_supported_tables']=not report['unresolved'] and not report['unsupported'] and not report['missing_needed']
        return report


def load_all(bases=None):
    bases=bases or BASES
    modules=[Module(name,bases[name],(INPUT/name).read_bytes()) for name in NAMES]
    loader=Loader(modules)
    report=loader.relocate()
    return loader,report


def export_bundle(loader,path):
    regions=[(m,r) for m in loader.modules for r in m.regions]
    header_size=24+32*len(regions)
    offset=header_size
    header=bytearray(struct.pack('<8sIIQ',b'TOTKMAP1',len(regions),0,
                                 next(m.base for m in loader.modules if m.name=='main')))
    for m,r in regions:
        header.extend(struct.pack('<QQIIQ',m.base+r['offset'],r['size'],r['permissions'],0,offset))
        offset+=r['size']
    digest=hashlib.sha256()
    with path.open('wb') as stream:
        stream.write(header);digest.update(header)
        for m,r in regions:
            block=memoryview(m.image)[r['offset']:r['offset']+r['size']]
            stream.write(block);digest.update(block)
    return {'path':str(path),'bytes':offset,'regions':len(regions),'sha256':digest.hexdigest()}


if __name__=='__main__':
    import sys
    loader,report=load_all()
    if '--bundle' in sys.argv:
        if not report['complete_for_supported_tables']:raise LoadError('Cannot export incomplete linking result')
        bundle=export_bundle(loader,ROOT/'loaded-modules.bin')
        (ROOT/'bundle.json').write_text(json.dumps(bundle,indent=2)+'\n')
        print('Exported',bundle['bytes'],'bytes in',bundle['regions'],'regions')
    (ROOT/'loader-report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({k:v for k,v in report.items() if k!='modules'},indent=2))
    for m in report['modules']:
        print(m['name'],m['bytes'],'bytes,',m['applied'],'relocations,',m['cross_module'],'cross-module')
