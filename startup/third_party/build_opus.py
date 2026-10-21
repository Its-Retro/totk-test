"""Build the vendored libopus 1.5.2 (BSD-3, startup/third_party/opus-1.5.2) as a native static archive.

Portable float build (no RTCD/intrinsics, no DNN/DRED/OSCE) with the runtime's zig toolchain and target.
Output: startup/third_party/opus-build/libopus.a (rebuilt only if missing or the source list/flags change).
compile_cached_fast.py appends this archive to the runtime DLL link; tests link it directly.
"""
import hashlib,json,re,subprocess,sys,concurrent.futures
from pathlib import Path

HERE=Path(__file__).resolve().parent
SRC=HERE/'opus-1.5.2'
OUT=HERE/'opus-build'
ZIG=HERE.parent.parent/'tools/zig-x86_64-windows-0.16.0/zig.exe'
FLAGS=['-target','x86_64-windows-gnu','-std=gnu99','-O2','-w','-DOPUS_BUILD','-DUSE_ALLOCA','-DHAVE_LRINT','-DHAVE_LRINTF',
       '-DPACKAGE_VERSION="1.5.2"','-fno-sanitize=undefined']
INCLUDES=['include','celt','silk','silk/float','src','dnn']

def sources(mk,var):
    text=(SRC/mk).read_text().replace('\\\n',' ')
    m=re.search(rf'^{var}\s*=(.*)$',text,re.M)
    return m.group(1).split()

def main():
    files=(sources('celt_sources.mk','CELT_SOURCES')+sources('silk_sources.mk','SILK_SOURCES')+
           sources('silk_sources.mk','SILK_SOURCES_FLOAT')+sources('opus_sources.mk','OPUS_SOURCES')+
           sources('opus_sources.mk','OPUS_SOURCES_FLOAT'))
    flags=FLAGS+[f'-I{(SRC/i).as_posix()}' for i in INCLUDES]
    key=hashlib.sha256(json.dumps([files,flags]).encode()).hexdigest()
    archive=OUT/'libopus.a';stamp=OUT/'libopus.key'
    if archive.exists() and stamp.exists() and stamp.read_text()==key:
        print(archive,file=sys.stderr);return 0
    OUT.mkdir(exist_ok=True);objs=[]
    def compile_one(rel):
        obj=OUT/(rel.replace('/','_')[:-2]+'.o')
        subprocess.run([str(ZIG),'cc',*flags,'-c',str(SRC/rel),'-o',str(obj)],check=True)
        return obj
    with concurrent.futures.ThreadPoolExecutor(8) as pool:objs=list(pool.map(compile_one,files))
    if archive.exists():archive.unlink()
    subprocess.run([str(ZIG),'ar','rcs',str(archive),*map(str,objs)],check=True)
    stamp.write_text(key);print(archive,file=sys.stderr);return 0

def link_args():
    """Linker arguments for the runtime DLL: the whole archive, because the runtime references
    libopus weakly (weak references alone do not extract archive members). Builds it if needed."""
    if main()!=0:raise RuntimeError('libopus build failed')
    return ['-Wl,--whole-archive',str(OUT/'libopus.a'),'-Wl,--no-whole-archive']

if __name__=='__main__':sys.exit(main())
