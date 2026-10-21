"""Build the vendored libwebm 1.0.0.32 (BSD-3, startup/third_party/libwebm) mkvparser as a native static archive, plus the
movie demux shim (startup/host_movie_webm.cpp) that host_movie.h references weakly.

Only mkvparser/mkvparser.cc (the WebM/Matroska reader); no muxer, no webm_parser, no tools. C++ is compiled with zig
c++ for x86_64-windows-gnu with -fno-exceptions -fno-rtti -fno-threadsafe-statics and NDEBUG, and the shim supplies
operator new/delete and __cxa_pure_virtual on the C heap, so the game DLL (linked with zig cc) needs no C++ runtime.
Output: startup/third_party/webm-build/libwebm.a and libtotk_movie_webm.a (rebuilt only when sources/flags change).
Run directly it lowers its own priority; compiles always run below normal priority. link_args(): the shim whole-archive (weak references), then libwebm.
"""
import ctypes,hashlib,json,os,subprocess,sys
from pathlib import Path

HERE=Path(__file__).resolve().parent
TP=HERE      # startup/third_party
STARTUP=TP.parent
SRC=TP/'libwebm'
OUT=TP/'webm-build'
ZIG=STARTUP.parent/'tools/zig-x86_64-windows-0.16.0/zig.exe'
SHIM=STARTUP/'host_movie_webm.cpp'
BELOW_NORMAL=0x4000
FLAGS=['-target','x86_64-windows-gnu','-std=c++17','-O2','-DNDEBUG','-fno-exceptions','-fno-rtti','-fno-threadsafe-statics',
       '-fno-sanitize=undefined',f'-I{SRC.as_posix()}']

def run(cmd):
    return subprocess.run(cmd,check=True,creationflags=BELOW_NORMAL if sys.platform=='win32' else 0)

def main():
    key=hashlib.sha256(json.dumps([FLAGS,(SRC/'mkvparser/mkvparser.cc').stat().st_size,SHIM.read_text(),(SHIM.parent/'host_movie.h').read_text()]).encode()).hexdigest()
    lib=OUT/'libwebm.a';shim=OUT/'libtotk_movie_webm.a';stamp=OUT/'libwebm.key'
    if lib.exists() and shim.exists() and stamp.exists() and stamp.read_text()==key:
        print(lib,file=sys.stderr);return 0
    OUT.mkdir(exist_ok=True)
    obj=OUT/'mkvparser.o';sobj=OUT/'host_movie_webm.o'
    run([str(ZIG),'c++',*FLAGS,'-w','-c',str(SRC/'mkvparser/mkvparser.cc'),'-o',str(obj)])
    run([str(ZIG),'c++',*FLAGS,'-Wall','-Wextra','-Werror','-c',str(SHIM),'-o',str(sobj)])
    for a,o in ((lib,obj),(shim,sobj)):
        if a.exists():a.unlink()
        run([str(ZIG),'ar','rcs',str(a),str(o)])
    stamp.write_text(key);print(lib,file=sys.stderr);return 0

def link_args():
    """Runtime DLL link: the shim whole (referenced weakly), then libwebm. Builds them if needed."""
    if main()!=0:raise RuntimeError('libwebm build failed')
    return ['-Wl,--whole-archive',str(OUT/'libtotk_movie_webm.a'),'-Wl,--no-whole-archive',str(OUT/'libwebm.a')]

if __name__=='__main__':
    if sys.platform=='win32':ctypes.windll.kernel32.SetPriorityClass(ctypes.windll.kernel32.GetCurrentProcess(),BELOW_NORMAL)
    sys.exit(main())
