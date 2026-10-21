"""Offline scan of RomFS files outside Shader/ for embedded BNSH shader binaries (effect .esetb
resources carry their own vertex/pixel/compute programs; inventory_game_shaders.py only covers
Shader/*.bfsha and *.bnsh, so these programs were only discovered at run time, one missed draw at a
time). Writes each unique stage program in the runtime dump format the live translator consumes
(vs-<id>.bin / ps-<vs mask>-<id>.bin = 0x50-byte SPH + code, id = sha256(SPH + code through the
terminator), the pixel variant keyed by the output map of the vertex program it is paired with in
the same BNSH program) to an output directory. Reads game files only; never writes them.
    python debug/effect_shader_scan.py <out dir> [prefix ...]      (default prefix Effect/)
"""
import collections,hashlib,json,struct,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parent.parent
sys.path.insert(0,str(ROOT));sys.path.insert(0,str(ROOT.parent/'tools/shader-python'));sys.path.insert(0,str(ROOT.parent/'assets'))
import zstandard as zstd
from romfs_reader import RomFS
from shader_package_reader import sarc,decompress
from bnsh_reader import parse,BnshError
from inventory_game_shaders import embedded_bnsh,WRAPPER_MAGIC

COMPUTE_MAGIC=b'\x79\x56\x34\x12'   # NVN wrapper of compute stages in effect resources (0x30-byte header)

def terminator_end(code):
    """Byte offset after the trailing unconditional self-branch (BRA PT always, offset -8)."""
    for i,(w,) in enumerate(struct.iter_unpack('<Q',code)):
        a=i*8
        if a%32==0:continue
        if (w>>48)&0xfff0==0xe240 and (w>>16)&0xf==7 and w&0x1f==0xf:
            off=(w>>20)&0xffffff
            if off&0x800000:off-=1<<24
            if off==-8 or (off==-16 and (a-8)%32==0):return a+8   # branch to self (or to its bundle word)
    return None

def vs_mask(sph):
    m=0
    for g in range(32):
        if (sph[0x36+g//2]>>((g&1)*4))&15:m|=1<<g
    return m

def main():
    out=Path(sys.argv[1]);out.mkdir(parents=True,exist_ok=True)
    prefixes=tuple(sys.argv[2:]) or ('Effect/',)
    rom=RomFS(ROOT.parent/'assets/romfs.bin')
    def read(name):return rom.read(name,0,rom.files[name]['size'])
    pack=decompress(read('Pack/ZsDic.pack.zs'),{})
    dicts={}
    for name,(o,s) in sarc(pack).items():
        d=zstd.ZstdCompressionDict(pack[o:o+s]);dicts[d.dict_id()]=d
    stats=collections.Counter();written=set();sources=collections.defaultdict(list)
    names=[n for n in sorted(rom.files) if n.startswith(prefixes)]
    if prefixes[0]=='--file':   # plain files on disk instead of RomFS paths (e.g. decompressed exefs segments)
        names=list(prefixes[1:]);read=lambda name:Path(name).read_bytes()
    def nested(data,depth=0):
        """data and, for SARC archives, every member (zstd members decompressed), recursively."""
        yield data
        if depth<3 and data[:4]==b'SARC':
            try:members=sarc(data)
            except ValueError:return
            for _,(o,s) in members.items():
                m=data[o:o+s]
                if m[:4]==b'\x28\xb5\x2f\xfd':
                    try:m=decompress(m,dicts,limit=512*1024*1024)
                    except Exception:stats['member read error']+=1;continue
                elif m[:4]!=b'SARC':continue   # plain members are already covered by the outer search
                yield from nested(m,depth+1)
    def all_bnsh(data):
        for d in nested(data):yield from embedded_bnsh(d)
    for k,name in enumerate(names):
        try:
            data=read(name)
            if name.endswith('.zs'):data=decompress(data,dicts,limit=512*1024*1024)
        except Exception as e:stats['read error']+=1;continue
        for offset,blob in all_bnsh(data):
            try:report=parse(blob)
            except BnshError:stats['bnsh error']+=1;continue
            stats['bnsh']+=1
            for program in report['programs']:
                st={};s_ctl={}
                for s in program['stages']:
                    s_ctl[s['stage']]=(s['control_offset'],s['control_size'])
                    raw=blob[s['bytecode_offset']:s['bytecode_offset']+s['bytecode_size']]
                    if raw[:4]!=WRAPPER_MAGIC and not (s['stage']=='compute' and raw[:4]==COMPUTE_MAGIC):stats['bad wrapper']+=1;continue
                    st[s['stage']]=raw
                vmask=None
                if 'vertex' in st:vmask=vs_mask(st['vertex'][0x30:0x80])
                for stage,raw in st.items():
                    if stage=='compute':
                        code=raw[0x100:] if raw[:4]==COMPUTE_MAGIC else raw[0x30:];end=terminator_end(code)
                        if end is None:stats['no terminator']+=1;continue
                        cid=hashlib.sha256(code[:end]).hexdigest()
                        # The launch variant (QMD block size / shared bytes) is chosen by the driver from the
                        # stage's control section: block x,y,z at +0x748, shared bytes at +0x754. The QMD
                        # value observed at run time is that size rounded up (384->512, 896->1024); the
                        # rounding rule is not pinned down for other sizes, so both 256-alignment and the next
                        # power of two are emitted when they differ (each is a complete, exact translation).
                        ctl=blob[s_ctl[stage][0]:s_ctl[stage][0]+s_ctl[stage][1]]
                        if len(ctl)<0x758:stats['cs without control']+=1;continue
                        bx,by,bz,sh=struct.unpack_from('<4I',ctl,0x748)
                        shs={0} if not sh else {(sh+255)&~255,1<<(sh-1).bit_length()}
                        payload=code[:end]+code[end:end+64]
                        for S in sorted(shs):
                            fn=f'cs-{bx}x{by}x{bz}s{S}l0-{cid}.bin'
                            if len(sources[fn])<3:sources[fn].append(f'{name}@{offset:#x}#{program["index"]}')
                            if fn not in written:written.add(fn);(out/fn).write_bytes(payload);stats['cs']+=1
                        continue
                    elif stage in('vertex','fragment'):
                        sph,code=raw[0x30:0x80],raw[0x80:];end=terminator_end(code)
                        if end is None:stats['no terminator']+=1;continue
                        cid=hashlib.sha256(sph+code[:end]).hexdigest()
                        if stage=='vertex':fn=f'vs-{cid}.bin'
                        elif vmask is None:stats['ps without vs']+=1;continue
                        else:fn=f'ps-{vmask:x}-{cid}.bin'
                        payload=sph+code[:end]+code[end:end+64]
                    else:stats['stage '+stage]+=1;continue
                    if len(sources[fn])<3:sources[fn].append(f'{name}@{offset:#x}#{program["index"]}')
                    if fn in written:continue
                    written.add(fn);(out/fn).write_bytes(payload);stats[fn[:2]]+=1
        if k%200==0:print(k,len(names),dict(stats),flush=True)
    (out/'sources.json').write_text(json.dumps(sources,indent=0))
    print('done',dict(stats))

if __name__=='__main__':main()
