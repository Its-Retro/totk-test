"""Ahead-of-time translation of shader programs the game supplies at runtime (not in the
game's shader archives, e.g. NVN/SDK built-ins). The draw executor dumps a program it
cannot find in the manifest to ../missing-shaders/<vs|ps-<mask>>-<canonical id>.bin
(SPH + code through the terminator); this translates each one with the same Maxwell->HLSL
emitter and DXC settings as translate_game_shaders.py and appends it to shader-manifest.tsv.
The live renderer re-reads the manifest on a miss, so the next resume finds it.

Also used for bulk pre-translation of programs extracted offline in the same dump format
(debug/effect_shader_scan.py: effect and UI layout shaders that inventory_game_shaders.py does not
cover):  translate_runtime_shaders.py --dir <dump dir> [--dir ...] [--batch N]
With --batch, manifest lines are appended N at a time (or every 3 minutes) rather than one by one,
so a running game re-reads the manifest a few times instead of once per shader.
"""
import re,sys,time
from pathlib import Path
ROOT=Path(__file__).resolve().parent
sys.path.insert(0,str(ROOT))
import maxwell_hlsl
from maxwell_isa import Unsupported
from translate_game_shaders import assemble,compile_hlsl,OUT
from write_shader_manifest import manifest_path

def translate_one(f,known):
    """Manifest line for dump file f, or None (already known / not translatable; reason printed)."""
    c=re.fullmatch(r'cs-((\d+)x(\d+)x(\d+)s(\d+)l(\d+))-([0-9a-f]{64})\.bin',f.name)
    if c:
        # Compute: no SPH; the variant (block size, shared and local memory from the QMD) is compiled in.
        variant,cid=c.group(1),c.group(7)
        if (cid,'cs',variant) in known:return None
        bx,by,bz,shared,local=(int(c.group(k)) for k in range(2,7))
        launch={'block':(bx,by,bz),'threads':bx*by*bz,'shared':shared,'local':local}
        try:
            em,prog,body=maxwell_hlsl.translate(f.read_bytes(),launch,'compute')
            if prog.canonical_id!=cid:print('canonical id mismatch',f.name,prog.canonical_id);return None
            src=assemble(em,prog,body)
        except Unsupported as e:print('unsupported',f.name,e);return None
        stem='rt-cs-'+cid[:16]+'-'+variant
        path=OUT/(stem+'.hlsl');path.write_text(src)
        hr,obj,msg=compile_hlsl(src,path,'compute')
        if hr!=0:print('compile error',f.name,msg.strip().splitlines()[:3]);return None
        dxil=path.with_suffix('.cs.dxil')
        tex=','.join(str(t) for t in sorted(em.tex_indices)) or '-'
        known.add((cid,'cs',variant));print('added cs',cid[:16],variant,'notes',sorted(em.notes),'uses',sorted(em.uses))
        return f"{cid}\tcs\t{variant}\t{tex}\t{manifest_path(dxil)}\n"
    m=re.fullmatch(r'(vs|ps)(?:-([0-9a-f]+))?-([0-9a-f]{64})\.bin',f.name)
    if not m:return None
    kind,mask,cid=m.group(1),m.group(2),m.group(3)
    if kind=='ps' and mask is None:print('skip (pixel program without vertex output mask)',f.name);return None
    key=(cid,kind,'-' if kind=='vs' else f'{int(mask,16):x}')
    if key in known:return None
    data=f.read_bytes();sph,code=data[:0x50],data[0x50:]
    stage='vertex' if kind=='vs' else 'fragment'
    try:
        em,prog,body=maxwell_hlsl.translate(code,sph,stage)
        if prog.canonical_id!=cid:print('canonical id mismatch',f.name,prog.canonical_id);return None
        src=assemble(em,prog,body,int(mask,16) if kind=='ps' else None)
    except Unsupported as e:print('unsupported',f.name,e);return None
    stem='rt-'+cid[:16]+(f'-{mask}' if kind=='ps' else '')
    path=OUT/(stem+'.hlsl');path.write_text(src)
    hr,obj,msg=compile_hlsl(src,path,stage)
    if hr!=0:print('compile error',f.name,msg.strip().splitlines()[:3]);return None
    dxil=path.with_suffix('.'+kind+'.dxil')
    tex=','.join(str(t) for t in sorted(em.tex_indices)) or '-'
    known.add(key);print('added',kind,cid[:16],'notes',sorted(em.notes))
    return f"{cid}\t{kind}\t{key[2]}\t{tex}\t{manifest_path(dxil)}\n"

def main(argv):
    dirs=[Path(argv[i+1]) for i,a in enumerate(argv) if a=='--dir'] or [ROOT.parent/'missing-shaders']
    batch=int(argv[argv.index('--batch')+1]) if '--batch' in argv else 1
    manifest=ROOT/'shader-manifest.tsv'
    def read_known():return {tuple(l.split('\t')[:3]) for l in manifest.read_text(encoding='utf-8').splitlines() if l}
    known=read_known()
    OUT.mkdir(exist_ok=True);added=0;pending=[];last=time.time()
    def flush():
        nonlocal pending,last
        if pending:
            have=read_known()   # another translator (the live helper) may have added some meanwhile
            lines=[l for l in pending if tuple(l.split('\t')[:3]) not in have]
            if lines:
                with manifest.open('a',encoding='utf-8') as out:out.write(''.join(lines))
            known.update(have)
        pending=[];last=time.time()
    for d in dirs:
        for f in sorted(d.glob('*.bin')):
            line=translate_one(f,known)
            if line:
                pending.append(line);added+=1
                if len(pending)>=batch or time.time()-last>180:flush()
    flush()
    print('runtime shaders added',added)

if __name__=='__main__':main(sys.argv[1:])
