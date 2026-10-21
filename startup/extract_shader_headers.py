"""Write shader-programs/<id>.sph: the 0x50-byte shader program header of each
unique graphics program (code identity as in inventory_game_shaders.py)."""
import hashlib
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
    written=conflicts=0
    for data in sources:
        for offset,blob in embedded_bnsh(data):
            report=parse(blob)
            for program in report['programs']:
                for st in program['stages']:
                    raw=blob[st['bytecode_offset']:st['bytecode_offset']+st['bytecode_size']]
                    code,_=stage_code(raw,st['stage'])
                    if st['stage']=='compute':continue
                    path=OUT/(hashlib.sha256(code).hexdigest()[:16]+'.sph');sph=bytes(raw[0x30:0x80])
                    if path.exists():
                        conflicts+=path.read_bytes()!=sph;continue
                    path.write_bytes(sph);written+=1
    print('headers written',written,'identical-code/different-header conflicts',conflicts)
if __name__=='__main__':main()
