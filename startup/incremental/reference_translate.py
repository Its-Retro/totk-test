"""Run the ORIGINAL translate_startup.main() with every path redirected.

ROOT is pointed at <work>/startup (emitter files copied there so the namespace
hash is identical), binaries are copied to <work>/uncompressed, range globals
are loaded from --ranges-dir, write_service_profile is disabled and the
instruction cache is opened read-only. Nothing is written into startup/.

  python reference_translate.py --ranges-dir SNAP --work DIR [--keep-monolithic]
"""
import sys
sys.dont_write_bytecode=True
import argparse,hashlib,json,shutil,time
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
import ts_bridge
from ts_bridge import ts,STARTUP

COPIED=('float_emit.py','instruction_cache.py','address_arbiter_emit.py','sleep_emit.py')

def prepare(work,aliases_path):
    root=Path(work)/'startup';binaries=Path(work)/'uncompressed'
    root.mkdir(parents=True,exist_ok=True);binaries.mkdir(parents=True,exist_ok=True)
    for name in COPIED:shutil.copyfile(STARTUP/name,root/name)
    aliases_path=Path(aliases_path)
    if aliases_path.exists():shutil.copyfile(aliases_path,root/'instruction-cache-aliases.json')
    for name in ('rtld','sdk','subsdk0','main'):
        target=binaries/name
        if not target.exists():shutil.copyfile(STARTUP.parent/'uncompressed'/name,target)
    if (root/'emitted-instructions.sqlite').exists():raise RuntimeError('unexpected sqlite in work root')
    return root

def run(ranges_dir,work,cache_path=STARTUP/'emitted-instructions.sqlite',aliases_path=None,keep_monolithic=False):
    aliases_path=Path(aliases_path) if aliases_path else Path(ranges_dir)/'instruction-cache-aliases.json'
    root=prepare(work,aliases_path)
    if root.resolve()==STARTUP.resolve():raise RuntimeError('refusing to write into startup/')
    for name,value in ts_bridge.load_ranges(ranges_dir).items():setattr(ts,name,value)
    ts.ROOT=root
    ts.write_service_profile=lambda:None
    ts_bridge.instruction_cache.InstructionCache=ts_bridge.read_only_cache_factory(cache_path)
    started=time.perf_counter()
    ts.main()
    seconds=time.perf_counter()-started
    if not keep_monolithic:(root/'monolithic_startup.c').unlink(missing_ok=True)
    return root,seconds

if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--ranges-dir',type=Path,required=True)
    parser.add_argument('--work',type=Path,required=True)
    parser.add_argument('--instruction-cache',type=Path,default=STARTUP/'emitted-instructions.sqlite')
    parser.add_argument('--aliases',type=Path)
    parser.add_argument('--keep-monolithic',action='store_true')
    args=parser.parse_args()
    root,seconds=run(args.ranges_dir,args.work,args.instruction_cache,args.aliases,args.keep_monolithic)
    print(json.dumps({'seconds':seconds,'translated':str(root/'translated_startup.c'),'provenance':str(root/'provenance.json')}))
