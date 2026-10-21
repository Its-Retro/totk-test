"""List the (vertex, fragment) program pairs defined by every BNSH variation.
Writes shader-pairs.json: [[vs_id16, ps_id16, vs_output_mask], ...] (unique)."""
import hashlib,json,struct
from inventory_game_shaders import *

def main():
    rom=RomFS(ROOT.parent/'assets/romfs.bin')
    def read(name):return rom.read(name,0,rom.files[name]['size'])
    pack=decompress(read('Pack/ZsDic.pack.zs'),{});dictionaries={}
    for name,(offset,size) in sarc(pack).items():
        d=zstd.ZstdCompressionDict(pack[offset:offset+size]);dictionaries[d.dict_id()]=d
    sources=[]
    for name in sorted(rom.files):
        if name.startswith('Shader/') and name.endswith('.bfsha.zs'):
            sources.append(decompress(read(name),dictionaries,limit=256*1024*1024))
        elif name.endswith('.bnsh'):sources.append(read(name))
    package=decompress(read('Shader/ApplicationPackage.Nin_NX_NVN.release.sarc.zs'),dictionaries)
    for name,(offset,size) in sarc(package).items():sources.append(package[offset:offset+size])
    pairs=set()
    for data in sources:
        for offset,blob in embedded_bnsh(data):
            for program in parse(blob)['programs']:
                ids={}
                for st in program['stages']:
                    raw=blob[st['bytecode_offset']:st['bytecode_offset']+st['bytecode_size']]
                    code,_=stage_code(raw,st['stage'])
                    ids[st['stage']]=(hashlib.sha256(code).hexdigest()[:16],bytes(raw[0x30:0x80]))
                if 'vertex' in ids and 'fragment' in ids:
                    sph=ids['vertex'][1];mask=0
                    for g in range(32):
                        if (sph[0x36+g//2]>>((g&1)*4))&15:mask|=1<<g
                    pairs.add((ids['vertex'][0],ids['fragment'][0],mask))
    out=sorted(pairs)
    (ROOT/'shader-pairs.json').write_text(json.dumps(out)+'\n')
    ps_variants={(p,m) for _,p,m in out}
    print('pairs',len(out),'distinct (ps, vs_mask) variants',len(ps_variants),'distinct ps',len({p for _,p,_ in out}))
if __name__=='__main__':main()
