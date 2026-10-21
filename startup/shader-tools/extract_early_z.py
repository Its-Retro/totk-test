"""Early-fragment-tests flag of every fragment stage in the game's shader binaries, keyed by canonical id.

The flag is not in the SPH or the code: it is bit 0 of byte +0x718 of the stage's GLSLC control section (nxport,
MIT: crates/build/src/shader_cache_build.rs early_tests_flag; the NVN driver then sets Maxwell EarlyZForce). The
BNSH stage descriptor holds the control section next to the code (bnsh_reader.parse: control_offset/control_size);
inventory_game_shaders.py and debug/effect_shader_scan.py read it but keep only the code and SPH.

Scans the same sources as those two scripts (Shader/*.bfsha.zs, *.bnsh, the ApplicationPackage SARC, and the
RomFS prefixes given, default Effect/, with nested SARC/zstd members) and writes a TSV:
    canonical id (sha256(SPH + code through the terminator), the manifest key) \t bit (0, 1, or m = differs between
    occurrences) \t occurrences \t distinct +0x718 byte values (hex) \t control section sizes
AGL's .sharcb archives in the ApplicationPackage (the deferred-lighting tile pass among them) hold no BNSH: raw NVN
program blobs on 256-byte boundaries, and the stage control sections in a table before them, in the same order.
The u32 0x60 bytes before each control section is its blob's offset in the code area (BAHS header +0x1c);
--sharcb scans only those.
Reads game files only. Usage: extract_early_z.py <out.tsv> [prefix ...] | extract_early_z.py <out.tsv> --sharcb
"""
import collections,hashlib,re,struct,sys
from pathlib import Path
START=Path(__file__).resolve().parents[1]          # startup/ (this file: startup/shader-tools/)
sys.path.insert(0,str(START));sys.path.insert(0,str(START/'debug'))
sys.path.insert(0,str(START.parent/'tools/shader-python'));sys.path.insert(0,str(START.parent/'assets'))
import zstandard as zstd
from romfs_reader import RomFS
from shader_package_reader import sarc,decompress
from bnsh_reader import parse,BnshError
from inventory_game_shaders import embedded_bnsh,WRAPPER_MAGIC
from effect_shader_scan import terminator_end

CTRL_MAGIC=0x98761234
CTRL_BYTES=struct.pack('<I',CTRL_MAGIC)

def sharcb_stages(blob,stats):
    """(canonical id, control section) of every fragment blob of one .sharcb archive (BAHS). Header word 7 (+0x1c) is
    the code area; each program entry holds its control section, and the u32 0x60 bytes before the control section is
    the stage blob's offset in the code area (checked: a wrapper there, and control +0x704 = the space up to the next
    blob or less)."""
    base=struct.unpack_from('<I',blob,0x1c)[0]
    starts=[m.start() for m in re.finditer(rb'[xy]V4',blob) if m.start()%256==0 and m.start()>=base]
    nxt={s:(starts[i+1] if i+1<len(starts) else len(blob)) for i,s in enumerate(starts)}
    ctls=[m.start() for m in re.finditer(re.escape(CTRL_BYTES),blob) if m.start()%4==0 and m.start()<base
          and struct.unpack_from('<I',blob,m.start()+0x20)[0]>=0x71c]
    for c in ctls:
        s=base+struct.unpack_from('<I',blob,c-0x60)[0];want=struct.unpack_from('<I',blob,c+0x704)[0]
        if s not in nxt or want>nxt[s]-s:stats['sharcb unpaired control']+=1;continue
        stats['sharcb paired']+=1
        sph_type=struct.unpack_from('<I',blob,s+0x30)[0]
        if blob[s]!=0x78 or (sph_type&31,(sph_type>>10)&15)!=(2,5):stats['sharcb non-fragment']+=1;continue
        sph,code=blob[s+0x30:s+0x80],blob[s+0x80:s+want];end=terminator_end(code)
        if end is None:stats['sharcb no terminator']+=1;continue
        stats['sharcb fragment']+=1
        yield hashlib.sha256(sph+code[:end]).hexdigest(),blob[c:c+struct.unpack_from('<I',blob,c+0x20)[0]]

def main():
    out=Path(sys.argv[1]);prefixes=tuple(a for a in sys.argv[2:] if a!='--sharcb') or ('Effect/',)
    rom=RomFS(START.parent/'assets/romfs.bin')
    def read(name):return rom.read(name,0,rom.files[name]['size'])
    pack=decompress(read('Pack/ZsDic.pack.zs'),{})
    dicts={}
    for name,(o,s) in sarc(pack).items():
        d=zstd.ZstdCompressionDict(pack[o:o+s]);dicts[d.dict_id()]=d
    def nested(data,depth=0):
        yield data
        if depth<3 and data[:4]==b'SARC':
            try:members=sarc(data)
            except ValueError:return
            for _,(o,s) in members.items():
                m=data[o:o+s]
                if m[:4]==b'\x28\xb5\x2f\xfd':
                    try:m=decompress(m,dicts,limit=512*1024*1024)
                    except Exception:continue
                elif m[:4]!=b'SARC':continue
                yield from nested(m,depth+1)
    names=[n for n in sorted(rom.files) if (n.startswith('Shader/') and n.endswith('.bfsha.zs')) or n.endswith('.bnsh')
           or n=='Shader/ApplicationPackage.Nin_NX_NVN.release.sarc.zs' or n.startswith(prefixes)]
    seen=collections.defaultdict(lambda:[0,set(),set()]);stats=collections.Counter()
    if '--sharcb' in sys.argv:
        names=[]
        package=decompress(read('Shader/ApplicationPackage.Nin_NX_NVN.release.sarc.zs'),dicts)
        for name,(o,s) in sarc(package).items():
            if not name.endswith('.sharcb'):continue
            for cid,ctl in sharcb_stages(package[o:o+s],stats):
                e=seen[cid];e[0]+=1;e[1].add(ctl[0x718]);e[2].add(len(ctl))
    for k,name in enumerate(names):
        try:
            data=read(name)
            if name.endswith('.zs'):data=decompress(data,dicts,limit=512*1024*1024)
        except Exception:stats['read error']+=1;continue
        for d in nested(data):
            for offset,blob in embedded_bnsh(d):
                try:report=parse(blob)
                except BnshError:stats['bnsh error']+=1;continue
                for program in report['programs']:
                    for s in program['stages']:
                        if s['stage']!='fragment':continue
                        raw=blob[s['bytecode_offset']:s['bytecode_offset']+s['bytecode_size']]
                        if raw[:4]!=WRAPPER_MAGIC:stats['bad wrapper']+=1;continue
                        sph,code=raw[0x30:0x80],raw[0x80:];end=terminator_end(code)
                        if end is None:stats['no terminator']+=1;continue
                        ctl=blob[s['control_offset']:s['control_offset']+s['control_size']]
                        if len(ctl)<0x71c or struct.unpack_from('<I',ctl,0)[0]!=CTRL_MAGIC:stats['no control']+=1;continue
                        e=seen[hashlib.sha256(sph+code[:end]).hexdigest()]
                        e[0]+=1;e[1].add(ctl[0x718]);e[2].add(len(ctl));stats['fragment stages']+=1
        if k%100==0:print(k,len(names),name,dict(stats),len(seen),flush=True)
    bits=collections.Counter()
    with out.open('w',encoding='utf-8') as f:
        for cid,(n,vals,sizes) in sorted(seen.items()):
            b={v&1 for v in vals};bit='m' if len(b)>1 else str(b.pop());bits[bit]+=1
            f.write(f"{cid}\t{bit}\t{n}\t{','.join(f'{v:02x}' for v in sorted(vals))}\t{','.join(str(z) for z in sorted(sizes))}\n")
    print('done',dict(stats),'unique fragment programs',len(seen),'bit',dict(bits))

if __name__=='__main__':main()
