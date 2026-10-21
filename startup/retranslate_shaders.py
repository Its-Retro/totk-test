"""Re-translate every shader listed in shader-manifest.tsv with the current translator
(maxwell_hlsl.py + translate_game_shaders.assemble) and recompile, in place, each one whose
HLSL changed. Manifest paths and ids stay as they are: a program keeps its DXIL file.

Sources: game programs from shader-programs/<stem>.bin/.sph (translate_game_shaders.py), runtime
programs from ../missing-shaders (translate_runtime_shaders.py naming). A program whose new
translation fails, whose canonical id differs, or whose texture handle list differs from the
manifest row is reported and left unchanged.

Usage: retranslate_shaders.py [--dry-run] [--only vs|ps|cs] [--workers N] [--rows <file>] [--with-precise]
  --rows <file>   only the manifest rows whose DXIL path is listed in <file> (one per line)
  --with-precise  also recompile rows whose only change is the 'precise' position qualifier (shader-fixes (1)); by
                  default they are reported 'precise-only' and left as they are: under -Gis every operation is
                  already precise. Checked on 25 vertex rows (shader-fixes/sample-out): the DXIL differs only in
                  phi order, with the same operations and no fast-math flags, so recompiling them changes nothing
"""
import difflib,os,re,sys,multiprocessing as mp
from pathlib import Path
ROOT=Path(__file__).resolve().parent
sys.path.insert(0,str(ROOT))

# render-36c: runtime-captured programs were dumped into several directories over time (missing-shaders is only the
# latest batch): 3357 of 8906 manifest PS rows, 1487 of them with flat inputs, were 'skip-source' with missing-shaders
# alone. The first directory holding the file wins.
RT_DIRS=['missing-shaders','effect-shaders','other-shaders','other-shaders2','exefs-shaders','aot-fixup-shaders','shader-scan']
def rt_file(fn):
    for d in RT_DIRS:
        f=ROOT.parent/d/fn
        if f.exists(): return f
    return ROOT.parent/RT_DIRS[0]/fn
def source(row):
    cid,kind,mask,tex,dxil=row
    name=Path(dxil).name
    m=re.fullmatch(r'rt-cs-[0-9a-f]{16}-(\S+)\.cs\.dxil',name)
    if m: return ('cs',rt_file(f'cs-{m.group(1)}-{cid}.bin'),None,m.group(1))
    m=re.fullmatch(r'rt-[0-9a-f]{16}(?:-([0-9a-f]+))?\.(vs|ps)\.dxil',name)
    if m:
        f=rt_file(f'vs-{cid}.bin' if kind=='vs' else f'ps-{m.group(1)}-{cid}.bin')
        return (kind,f,None,None)
    m=re.fullmatch(r'([0-9a-f]{16})\.(vs|ps)\.dxil',name)
    if m: return (kind,ROOT/'shader-programs'/(m.group(1)+'.bin'),ROOT/'shader-programs'/(m.group(1)+'.sph'),None)
    return None

PRECISE_POS='    precise float4 pos : SV_Position;';PLAIN_POS='    float4 pos : SV_Position;'
WITH_PRECISE='--with-precise' in sys.argv

def work(args):
    row,dry=args
    import maxwell_hlsl
    from maxwell_isa import Unsupported
    from translate_game_shaders import assemble,compile_hlsl
    cid,kind,mask,tex,dxil=row
    src=source(row)
    if not src: return ('skip-name',dxil,'')
    k,binf,sphf,variant=src
    if not binf.exists() or (sphf and not sphf.exists()): return ('skip-source',dxil,str(binf))
    try:
        if k=='cs':
            c=re.fullmatch(r'(\d+)x(\d+)x(\d+)s(\d+)l(\d+)',variant)
            bx,by,bz,shared,local=(int(c.group(i)) for i in range(1,6))
            em,prog,body=maxwell_hlsl.translate(binf.read_bytes(),{'block':(bx,by,bz),'threads':bx*by*bz,'shared':shared,'local':local},'compute')
            stage='compute'; text=assemble(em,prog,body)
        else:
            data=binf.read_bytes()
            if sphf: sph,code=sphf.read_bytes(),data
            else: sph,code=data[:0x50],data[0x50:]
            stage='vertex' if k=='vs' else 'fragment'
            em,prog,body=maxwell_hlsl.translate(code,sph,stage)
            text=assemble(em,prog,body,int(mask,16) if k=='ps' and mask!='-' else None)
    except (Unsupported,Exception) as e:
        return ('fail-translate',dxil,f'{type(e).__name__}: {e}')
    if prog.canonical_id!=cid: return ('fail-id',dxil,prog.canonical_id)
    newtex=','.join(str(t) for t in sorted(em.tex_indices)) or '-'
    if newtex!=tex: return ('fail-tex',dxil,f'{tex} -> {newtex}')
    hlsl=Path(dxil[:-len('.xs.dxil')]+'.hlsl') if dxil.endswith('.dxil') else None
    old=hlsl.read_text() if hlsl and hlsl.exists() else ''
    if not old and hlsl:   # render-36b: the -Gis set (shader-hlsl-gis) was compiled from shader-hlsl/<name>.hlsl; compare against that source
        alt=ROOT/'shader-hlsl'/hlsl.name; old=alt.read_text() if alt.exists() else ''
    if old==text: return ('same',dxil,'')
    if not WITH_PRECISE and old==text.replace(PRECISE_POS,PLAIN_POS): return ('precise-only',dxil,'')
    diff=[l for l in difflib.unified_diff(old.splitlines(),text.splitlines(),lineterm='',n=0) if l[:1] in '+-' and l[:3] not in ('+++','---')]
    if dry: return ('changed',dxil,'\n'.join(diff[:6]))
    tmp=hlsl.with_suffix('.hlsl.new'); tmp.write_text(text)
    hr,obj,msg=compile_hlsl(text,tmp,stage)
    # compile_hlsl writes <tmp stem>.<kind>.dxil next to the source: move both into place
    out=tmp.with_suffix('.'+{'vertex':'vs','fragment':'ps','compute':'cs'}[stage]+'.dxil')
    if hr!=0 or not out.exists():
        tmp.unlink(missing_ok=True); out.unlink(missing_ok=True)
        return ('fail-compile',dxil,msg.strip().splitlines()[:2].__str__())
    os.replace(out,dxil); os.replace(tmp,hlsl)
    return ('recompiled',dxil,'')

def main():
    dry='--dry-run' in sys.argv
    only=sys.argv[sys.argv.index('--only')+1] if '--only' in sys.argv else None
    workers=int(sys.argv[sys.argv.index('--workers')+1]) if '--workers' in sys.argv else max(1,(os.cpu_count() or 2)//2)
    pick=None
    if '--rows' in sys.argv:
        pick={l.strip().lower() for l in Path(sys.argv[sys.argv.index('--rows')+1]).read_text(encoding='utf-8').splitlines() if l.strip()}
    rows={}
    for l in (ROOT/'shader-manifest.tsv').read_text(encoding='utf-8').splitlines():
        p=l.split('\t')
        if len(p)==5 and (not only or p[1]==only) and (pick is None or p[4].lower() in pick): rows.setdefault(p[4],tuple(p))
    counts={}
    with mp.Pool(workers) as pool:
        for n,(st,dxil,info) in enumerate(pool.imap_unordered(work,[(r,dry) for r in rows.values()],chunksize=8)):
            counts[st]=counts.get(st,0)+1
            if st.startswith('fail') or (dry and counts[st]<=3): print(st,dxil,info,flush=True)
            if n%500==0: print(f'  {n}/{len(rows)} {counts}',flush=True)
    print('done',counts)

if __name__=='__main__':main()
