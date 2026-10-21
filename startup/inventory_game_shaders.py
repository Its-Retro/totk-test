"""Offline inventory of every Maxwell program in the game's shader packages.

Extracts each BNSH embedded in the RomFS shader packages, deduplicates stage
bytecode by SHA-256, disassembles every unique program with envydis and counts
instruction mnemonics. This scopes the shader translator; it translates nothing.
Outputs: shader-programs/ (unique code blobs), game-shader-inventory.json.
"""
import collections,hashlib,json,re,struct,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parent
sys.path.insert(0,str(ROOT.parent/'tools/shader-python'))
sys.path.insert(0,str(ROOT.parent/'assets'))
import zstandard as zstd
from romfs_reader import RomFS
from shader_package_reader import sarc,decompress
from bnsh_reader import parse,BnshError

OUT=ROOT/'shader-programs'
ENVYDIS=ROOT.parent/'tools/envydis.exe'
WRAPPER_MAGIC=b'\x78\x56\x34\x12'

def embedded_bnsh(data):
    position=0
    while True:
        position=data.find(b'BNSH\0\0\0\0',position)
        if position<0:return
        if position+32<=len(data):
            size=struct.unpack_from('<I',data,position+28)[0]
            if 32<=size<=len(data)-position:
                yield position,bytes(data[position:position+size])
        position+=8

def stage_code(blob,stage):
    """Split the NVN wrapper: 0x30-byte header, 0x50-byte SPH for graphics stages."""
    if blob[:4]!=WRAPPER_MAGIC or len(blob)<0x30:raise BnshError('Unexpected shader wrapper')
    start=0x30 if stage=='compute' else 0x80
    code=blob[start:]
    if len(code)%8:raise BnshError('Misaligned shader code')
    header=None
    if stage!='compute':
        word=struct.unpack_from('<I',blob,0x30)[0]
        header={'sph_type':word&31,'version':(word>>5)&31,'shader_type':(word>>10)&15}
    return code,header

def main():
    rom=RomFS(ROOT.parent/'assets/romfs.bin')
    def read(name):return rom.read(name,0,rom.files[name]['size'])
    pack=decompress(read('Pack/ZsDic.pack.zs'),{})
    dictionaries={}
    for name,(offset,size) in sarc(pack).items():
        d=zstd.ZstdCompressionDict(pack[offset:offset+size]);dictionaries[d.dict_id()]=d
    sources=[]
    for name in sorted(rom.files):
        if name.startswith('Shader/') and name.endswith('.bfsha.zs'):
            sources.append((name,decompress(read(name),dictionaries,limit=256*1024*1024)))
        elif name.endswith('.bnsh'):
            sources.append((name,read(name)))
    package=decompress(read('Shader/ApplicationPackage.Nin_NX_NVN.release.sarc.zs'),dictionaries)
    for name,(offset,size) in sarc(package).items():
        sources.append(('Shader/ApplicationPackage/'+name,package[offset:offset+size]))

    OUT.mkdir(exist_ok=True)
    unique={};containers=[];errors=[]
    for name,data in sources:
        found=0
        for offset,blob in embedded_bnsh(data):
            try:report=parse(blob)
            except BnshError as e:errors.append({'source':name,'offset':offset,'error':str(e)});continue
            found+=1;stage_refs=0
            for program in report['programs']:
                for st in program['stages']:
                    raw=blob[st['bytecode_offset']:st['bytecode_offset']+st['bytecode_size']]
                    try:code,header=stage_code(raw,st['stage'])
                    except BnshError as e:errors.append({'source':name,'offset':offset,'error':str(e)});continue
                    key=hashlib.sha256(code).hexdigest();stage_refs+=1
                    entry=unique.setdefault(key,{'sha256':key,'stage':st['stage'],'bytes':len(code),'header':header,'refs':[]})
                    if len(entry['refs'])<4:entry['refs'].append(f"{name}@{offset:#x}#{program['index']}")
                    entry['ref_count']=entry.get('ref_count',0)+1
            containers.append({'source':name,'bnsh_offset':offset,'variations':report['variation_count'],'stage_refs':stage_refs})
        if not found and name.endswith(('.bfsha','.bfsha.zs','.bnsh')):
            errors.append({'source':name,'error':'no parseable BNSH'})
    print(f'{len(containers)} BNSH containers, {len(unique)} unique stage programs, {len(errors)} errors',flush=True)

    # Disassemble in batches: envydis takes one hex word per line.
    mnemonics=collections.Counter();per_stage=collections.defaultdict(collections.Counter)
    unknown=0;programs_with_unknown=0
    word_re=re.compile(r'^[0-9a-f]{8}:\s+[0-9a-f]{8} [0-9a-f]{8}\s+(?:[A-Z]\s+)?(.*)$')
    for entry in unique.values():entry['file']=entry['sha256'][:16]+'.bin'
    # Second pass writes code (kept separate so the dedupe pass stays memory-light).
    for name,data in sources:
        for offset,blob in embedded_bnsh(data):
            try:report=parse(blob)
            except BnshError:continue
            for program in report['programs']:
                for st in program['stages']:
                    raw=blob[st['bytecode_offset']:st['bytecode_offset']+st['bytecode_size']]
                    try:code,_=stage_code(raw,st['stage'])
                    except BnshError:continue
                    path=OUT/(hashlib.sha256(code).hexdigest()[:16]+'.bin')
                    if not path.exists():path.write_bytes(code)
    # envydis only feeds the statistics below; translation uses maxwell_isa.py. Without it (or with --no-disasm)
    # the counts are left at zero.
    disassemble=ENVYDIS.exists() and '--no-disasm' not in sys.argv
    if not disassemble:print('envydis not used: instruction statistics skipped',flush=True)
    for n,entry in enumerate(unique.values()):
        if not disassemble:
            entry['instructions']=0;entry['unknown']=0;continue
        code=(OUT/entry['file']).read_bytes()
        txt=OUT/(entry['sha256'][:16]+'.hex')
        txt.write_text('\n'.join(f'{v:016x}' for (v,) in struct.iter_unpack('<Q',code))+'\n')
        result=subprocess.run([str(ENVYDIS),'-m','gm107','-V','sm52','-W','-n',str(txt)],capture_output=True,text=True)
        txt.unlink()
        dis=OUT/(entry['sha256'][:16]+'.sass');dis.write_text(result.stdout)
        # Count up to the trailing self-branch padding that ends every program.
        local=collections.Counter();bad=0
        for line in result.stdout.splitlines():
            m=word_re.match(line)
            if not m:continue
            text=m.group(1).strip()
            if text.startswith('sched'):continue
            op=text.split()[0] if text else '???'
            if op=='bra' and line.split(':')[0].lstrip('0') and text.split()[-1]==hex(int(line.split(':')[0],16)):break
            if op.startswith('???'):bad+=1;op='???'
            local[op]+=1
        entry['instructions']=sum(local.values());entry['unknown']=bad
        unknown+=bad;programs_with_unknown+=bool(bad)
        mnemonics.update(local);per_stage[entry['stage']].update(local)
        if n%500==0:print(f'  disassembled {n}/{len(unique)}',flush=True)

    report={'scope':'Offline inventory; no shader translated or executed',
        'containers':len(containers),'unique_programs':len(unique),
        'programs_by_stage':dict(collections.Counter(e['stage'] for e in unique.values())),
        'total_unique_instructions':sum(e['instructions'] for e in unique.values()),
        'unknown_instruction_words':unknown,'programs_with_unknown':programs_with_unknown,
        'mnemonics':dict(mnemonics.most_common()),
        'mnemonics_by_stage':{k:dict(v.most_common()) for k,v in per_stage.items()},
        'errors':errors[:200],'error_count':len(errors),
        'container_list':containers,'programs':sorted(unique.values(),key=lambda e:-e['ref_count'])}
    (ROOT/'game-shader-inventory.json').write_text(json.dumps(report,indent=1)+'\n',encoding='utf-8')
    print(json.dumps({k:v for k,v in report.items() if k not in ('container_list','programs','errors','mnemonics_by_stage')},indent=1))

if __name__=='__main__':main()
