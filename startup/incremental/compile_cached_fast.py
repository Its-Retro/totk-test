"""Faster drop-in for compile_cached.py (same CLI, same object cache keys, same link, same receipt).

Differences from compile_cached.py (outputs are byte-identical objects and the same link command):
  * translated_startup.c is memory-mapped instead of being decoded into a 1 GB str; page boundaries
    are found with bytes.find instead of a multiline regex, and per-unit cache keys are hashed
    from memoryview slices on a thread pool (no 3-4 GB of str copies).
  * Like compile_cached.py, the ~1 GB reference source (native-object-cache/references/<sha>.c, used by
    compare_cached.py) is only produced with TOTK_KEEP_REFERENCE=1; it is then hashed/written by streaming
    in a background thread overlapping compilation, and not re-read if it already exists with the right size.
  * Cached objects are validated once (sha256 == receipt, like the original) and then trusted by
    (size, mtime_ns) via native-object-cache/fast-object-manifest.json instead of re-hashing ~1.5 GB
    of objects every round (--verify-objects restores full re-hashing).
  * Changed units are compiled longest-first so the large dispatch unit does not become the tail.
  * Optional --no-pdb links with -s (no 700+ MB PDB); off by default so the default DLL is the same.
  * Optional --no-load-check skips the ctypes.CDLL() load test that the original performs.
  * Page units and dispatch include the generated slim page header (split_page_header.py) instead of
    boot_runtime.h, and their cache keys depend on it alone; runtime_support includes the full runtime
    header set with the page-visible out-of-line functions made external (defined once for the DLL).
    A runtime edit that leaves the page header unchanged recompiles runtime_support only.
If the source is not pure ASCII/LF (where bytes and the original's str semantics could differ), it
falls back to compile_cached.build() unchanged.
"""
import argparse,concurrent.futures,ctypes,hashlib,json,mmap,os,re,subprocess,sys,threading,time
from pathlib import Path

ROOT=Path(__file__).resolve().parent.parent
PAGE=b'static __attribute__((noinline)) uint32_t boot_page_'
HEX=frozenset(b'0123456789abcdef')
INCLUDE_LINE=b'#include "boot_runtime.h"'
def digest(data):return hashlib.sha256(data).hexdigest()

class Profile:
    def __init__(self):self.started=time.perf_counter();self.last=self.started;self.phases={}
    def mark(self,name):
        now=time.perf_counter();self.phases[name]=round(self.phases.get(name,0)+now-self.last,3);self.last=now

def file_digest_memo(path,memo):
    st=path.stat();key=str(path)
    entry=memo.get(key)
    if entry and entry[0]==st.st_size and entry[1]==st.st_mtime_ns:return entry[2]
    h=hashlib.sha256()
    with open(path,'rb') as f:
        while chunk:=f.read(1<<24):h.update(chunk)
    value=h.hexdigest();memo[key]=[st.st_size,st.st_mtime_ns,value];return value

def find_all(m,needle,start=0,end=None):
    end=len(m) if end is None else end;out=[];pos=m.find(needle,start,end)
    while pos>=0:out.append(pos);pos=m.find(needle,pos+len(needle),end)
    return out

def ascii_lf_only(m):
    if m.find(b'\r')>=0:return False
    for i in range(0,len(m),1<<26):
        if not m[i:i+(1<<26)].isascii():return False
    return True

def build(output,optimization='O2',precompiled_header=False,root=ROOT,verify_objects=False,write_reference=None,
          no_pdb=False,load_check=True,workers=None,profile_path=None,plan_only=False,source_path=None):
    prof=Profile();started=prof.started
    if write_reference is None:write_reference=os.environ.get('TOTK_KEEP_REFERENCE')=='1'
    sys.path.insert(0,str(root))
    from split_host_window import split_host_window
    from split_page_header import split_page_header,runtime_overrides
    source_path=Path(source_path) if source_path else root/'translated_startup.c'
    source_file=open(source_path,'rb')
    m=mmap.mmap(source_file.fileno(),0,access=mmap.ACCESS_READ);mv=memoryview(m)
    if not ascii_lf_only(m):
        mv.release();m.close();source_file.close()
        print('compile_cached_fast: non-ASCII or CR in source; falling back to compile_cached.build',file=sys.stderr,flush=True)
        import compile_cached
        return compile_cached.build(output,optimization,precompiled_header)
    prof.mark('ascii_check')
    zig=root.parent/'tools/zig-x86_64-windows-0.16.0/zig.exe'
    cache=root/'native-object-cache'
    manifest_path=cache/'fast-object-manifest.json'
    try:manifest=json.loads(manifest_path.read_text())
    except (OSError,ValueError):manifest={}
    file_memo=manifest.setdefault('files',{});object_memo=manifest.get('objects',{}) if not verify_objects else {}
    compiler=file_digest_memo(zig,file_memo)
    runtime_path=(root/'boot_runtime.h').resolve()
    window_path=(root/'host_window.h').resolve()
    window_api,window_implementation=split_host_window(window_path.read_text(encoding='utf-8'))
    def read_source(path):return window_api if path==window_path else path.read_text(encoding='utf-8')
    # Page header: generated, self-contained (no local includes); exists only in its snapshot.
    page_header,page_split=split_page_header(root,read_source)
    page_path=(root/'boot_page.h').resolve()
    runtime_files=runtime_overrides(root,page_split['static_extern'],read_source)
    runtime_files.setdefault(window_path,window_api)
    def snapshot_headers(entry,overrides):
        headers={}
        def collect(path):
            path=path.resolve()
            if path in headers:return
            data=overrides[path] if path in overrides else path.read_bytes()
            headers[path]=data
            for name in re.findall(rb'^#include "([^\"]+)"',data,re.M):collect(path.parent/name.decode())
        collect(entry)
        checksum=digest(b''.join(str(p.relative_to(root.parent)).encode()+b'\0'+headers[p] for p in sorted(headers)))
        snapshot=cache/'headers'/checksum
        for path,data in headers.items():
            target=snapshot/path.relative_to(root.parent);target.parent.mkdir(parents=True,exist_ok=True)
            if not target.exists():
                if plan_only:raise RuntimeError(f'plan-only: header snapshot missing {target}')
                target.write_bytes(data)
            if target.read_bytes()!=data:raise ValueError('Immutable header snapshot mismatch')
        return checksum,snapshot
    dependencies,snapshot=snapshot_headers(page_path,{page_path:page_header.encode()})
    runtime_dependencies,runtime_snapshot=snapshot_headers(runtime_path,{p:t.encode() for p,t in runtime_files.items()})
    backend_dependencies,backend_snapshot=snapshot_headers(window_path,{window_path:window_implementation.encode()})
    include=f'#include "{(snapshot/"startup/boot_page.h").as_posix()}"\n'
    runtime_include=f'#include "{(runtime_snapshot/"startup/boot_runtime.h").as_posix()}"\n'
    backend_include=f'#include "{(backend_snapshot/"startup/host_window.h").as_posix()}"\n'
    include_b=include.encode();include_line_b=include.strip().encode();runtime_include_b=runtime_include.encode()
    prof.mark('headers')
    # Partition exactly like the original regex: ^static __attribute__((noinline)) uint32_t boot_page_([0-9a-f]+)\(
    starts=[];names=[]
    for pos in find_all(m,PAGE):
        if pos and m[pos-1]!=10:continue
        i=pos+len(PAGE);j=i
        while j<len(m) and m[j] in HEX:j+=1
        if j==i or j>=len(m) or m[j]!=40:continue
        starts.append(pos);names.append(m[i:j].decode())
    if not starts:raise ValueError('Expected partitioned native source')
    wrapper_start=m.find(b'static uint32_t boot_execute(')
    if wrapper_start<0:
        wrapper_start=m.find(b'__declspec(dllexport) uint32_t boot_run(')
        if wrapper_start<0:raise ValueError('substring not found')
    include_hits=find_all(m,INCLUDE_LINE) if write_reference else []
    prof.mark('partition')
    # Reference source = backend_include + source.replace(include line, full runtime header), streamed:
    # one translation unit with the runtime header set of runtime_support.
    runtime_include_line_b=runtime_include.strip().encode()
    def reference_segments():
        yield backend_include.encode();prev=0
        for hit in include_hits:
            yield mv[prev:hit];yield runtime_include_line_b;prev=hit+len(INCLUDE_LINE)
        yield mv[prev:];yield b'\n'
    def hash_reference():
        h=hashlib.sha256();size=0
        for segment in reference_segments():h.update(segment);size+=len(segment)
        return h.hexdigest(),size
    def hash_source():
        h=hashlib.sha256()
        for i in range(0,len(m),1<<26):h.update(mv[i:i+(1<<26)])
        return h.hexdigest()
    # Patch 58 (play-work/fp-work): page and dispatch units target x86-64-v3 (AVX2, FMA3, BMI1/BMI2, LZCNT, MOVBE,
    # F16C: Intel Haswell / AMD Excavator and newer) for the inline native FP paths (boot_fp_native.h: one host FMA,
    # packed vector FP) and the BMI2/LZCNT integer forms. -ffp-contract=off is required: zig cc defaults to
    # -ffp-contract=on, which would fuse a*b+c expressions that Arm rounds twice. runtime_support stays on the
    # baseline target and gets -DBOOT_REQUIRE_X86_64_V3, a CPU check at DLL load that runs before any page or
    # dispatch code. TOTK_PAGE_ARCH=baseline restores the old flags (FMA then takes patch 57's helper).
    page_arch=[] if os.environ.get('TOTK_PAGE_ARCH')=='baseline' else ['-march=x86_64_v3','-ffp-contract=off']
    flags=['-target','x86_64-windows-gnu','-std=c11','-'+optimization,'-Wall','-Wextra','-Werror']+page_arch
    # Same env gate as compile_cached.py: TOTK_DRAW_PATH=1 adds the define only to runtime_support and adds
    # -lbcrypt -ldxguid to the link; page units are unaffected. runtime_support never uses the PCH (it is
    # built from the page header, which runtime_support does not include).
    draw_path=os.environ.get('TOTK_DRAW_PATH')=='1'
    pch_seconds=0
    if precompiled_header:
        pch_dir=cache/'pch'
        if not plan_only:pch_dir.mkdir(exist_ok=True)
        pch=pch_dir/(digest((compiler+dependencies+json.dumps(flags)).encode())+'.pch')   # page header snapshot
        pch_receipt=pch.with_suffix('.sha256')
        pch_ok=pch.exists() and pch_receipt.exists()
        if pch_ok:
            st=pch.stat();memo=file_memo.get(str(pch));receipt_text=pch_receipt.read_text()
            if memo and memo[0]==st.st_size and memo[1]==st.st_mtime_ns and memo[2]==receipt_text:pass
            else:pch_ok=file_digest_memo(pch,file_memo)==receipt_text
        if not pch_ok and plan_only:raise RuntimeError('plan-only: PCH missing')
        if not pch_ok:
            pch_started=time.perf_counter();temporary=pch.with_suffix(f'.{os.getpid()}.tmp.pch')
            subprocess.run([str(zig),'cc',*flags,'-x','c-header',str(snapshot/'startup/boot_page.h'),'-o',str(temporary)],check=True)
            checksum=digest(temporary.read_bytes());os.replace(temporary,pch);pch_receipt.write_text(checksum)
            file_memo.pop(str(pch),None);pch_seconds=time.perf_counter()-pch_started
        flags+=['-include-pch',str(pch)]
    prof.mark('pch')
    def unit_flags_for(name):
        if name=='runtime_support':
            return ([f for i,f in enumerate(flags) if f!='-include-pch' and not (i and flags[i-1]=='-include-pch') and f not in page_arch]+
                    (['-DTOTK_DRAW_PATH'] if draw_path else [])+(['-DBOOT_REQUIRE_X86_64_V3'] if page_arch else []))
        # Translated pages: line tables + function symbols only (no variable/type debug info). zig's
        # COFF linker accepts no /pdbpagesize, and full debug info of ~13k page functions had grown
        # the PDB to the 2 GiB limit of 4 KiB PDB pages, so profilers could not resolve symbols.
        if name.startswith('page_'):return flags+['-gline-tables-only']
        return flags
    # units: (name, list of byte segments)
    units=[]
    for index,start in enumerate(starts):
        end=starts[index+1] if index+1<len(starts) else wrapper_start
        units.append(('page_'+names[index],(include_b,mv[start+7:end])))  # removeprefix('static ')
    prefix=bytes(mv[:starts[0]]).replace(INCLUDE_LINE,include_line_b)
    prototypes='\n'.join(f'uint32_t boot_page_{n}(BootContext*,uint64_t*,uint32_t*);' for n in names).encode()
    units.append(('dispatch',(prefix,prototypes,b'\n',mv[wrapper_start:])))
    units.append(('runtime_support',(backend_include.encode(),runtime_include_b)))
    objects=cache/'objects'
    if not plan_only:objects.mkdir(parents=True,exist_ok=True)
    hw=workers or int(os.environ.get('TOTK_COMPILE_WORKERS','0') or 0) or min(8,max(1,(os.cpu_count() or 2)//2))
    with concurrent.futures.ThreadPoolExecutor(max_workers=hw+2) as pool:
        source_future=pool.submit(hash_source);reference_future=pool.submit(hash_reference) if write_reference else None
        def unit_key(unit):
            # Pages and dispatch depend on the page header only; runtime_support on the runtime header set
            # (its include lines name both snapshots, so the backend header is covered too).
            unit_dependencies=runtime_dependencies+backend_dependencies if unit[0]=='runtime_support' else dependencies
            h=hashlib.sha256((compiler+unit_dependencies+json.dumps(unit_flags_for(unit[0]))).encode())
            for segment in unit[1]:h.update(segment)
            return h.hexdigest()
        keys=list(pool.map(unit_key,units,chunksize=64))
        prof.mark('unit_keys')
        new_memo={}
        def check(index):
            name,_=units[index];base=objects/(name+'-'+keys[index]);obj=base.with_suffix('.o')
            fname=obj.name
            try:st=os.stat(obj)
            except FileNotFoundError:return index,obj,False
            entry=object_memo.get(fname)
            if entry and entry[0]==st.st_size and entry[1]==st.st_mtime_ns:
                new_memo[fname]=entry;return index,obj,True
            receipt=base.with_suffix('.sha256')
            if not receipt.exists():return index,obj,False
            with open(obj,'rb') as f:value=hashlib.file_digest(f,'sha256').hexdigest()
            if value!=receipt.read_text():return index,obj,False
            new_memo[fname]=[st.st_size,st.st_mtime_ns,value];return index,obj,True
        checked=list(pool.map(check,range(len(units)),chunksize=64))
        prof.mark('object_check')
        source_sha=source_future.result();reference_sha,reference_size=reference_future.result() if reference_future else (None,0)
        prof.mark('source_reference_hash')
    if plan_only:
        plan=[[units[i][0],keys[i],str(obj),ok] for i,obj,ok in checked];del units,checked
        if plan_only=='save-manifest':
            manifest['objects']=new_memo;manifest_path.write_text(json.dumps(manifest))
        return {'plan':plan,'phases':prof.phases,'source_sha256':source_sha,'reference_sha256':reference_sha}
    reference=None;reference_thread=None;reference_error=[]
    if write_reference:
        references=cache/'references';references.mkdir(exist_ok=True)
        reference=references/(reference_sha+'.c')
    if write_reference and not (reference.exists() and reference.stat().st_size==reference_size):
        def write_ref():
            try:
                temporary=reference.with_suffix(f'.{os.getpid()}.tmp')
                with open(temporary,'wb') as f:
                    for segment in reference_segments():f.write(segment)
                os.replace(temporary,reference)
            except BaseException as error:reference_error.append(error)
        reference_thread=threading.Thread(target=write_ref);reference_thread.start()
    pending=[index for index,_,ok in checked if not ok]
    pending.sort(key=lambda index:-sum(len(s) for s in units[index][1]))
    results=[obj for _,obj,_ in checked]
    def compile_unit(index):
        name,segments=units[index];text=b''.join(segments).decode('ascii')
        base=objects/(name+'-'+keys[index]);src=base.with_suffix('.c');obj=base.with_suffix('.o');receipt=base.with_suffix('.sha256')
        unit_started=time.perf_counter()
        if not src.exists():src.write_text(text)
        if src.read_text()!=text:raise ValueError('Immutable source cache mismatch')
        temporary=base.with_suffix(f'.{os.getpid()}.tmp.o')
        subprocess.run([str(zig),'cc',*unit_flags_for(name),'-c',str(src),'-o',str(temporary)],check=True,capture_output=True)
        data=temporary.read_bytes();checksum=digest(data);os.replace(temporary,obj);receipt.write_text(checksum)
        st=os.stat(obj);new_memo[obj.name]=[st.st_size,st.st_mtime_ns,checksum]
        return name,time.perf_counter()-unit_started
    unit_times=[]
    # Live progress for the dashboard (read-only consumers; never affects the build).
    progress_path=root/'build-progress.json';progress_started=time.time()
    # Units compile largest first, so progress is reported in source bytes too (for an honest ETA).
    unit_bytes=[sum(len(s) for s in units[index][1]) for index in pending];total_bytes=sum(unit_bytes) or 1
    done_bytes=[0]
    def progress(phase,compiled):
        try:
            temporary=progress_path.with_suffix(f'.{os.getpid()}.tmp')
            temporary.write_text(json.dumps({'pid':os.getpid(),'output':str(output),'phase':phase,'compiled':compiled,
                                             'total':len(pending),'reused':len(units)-len(pending),
                                             'bytes_done':done_bytes[0],'bytes_total':total_bytes,
                                             'started':progress_started,'updated':time.time()}))
            os.replace(temporary,progress_path)
        except OSError:pass
    progress('compiling',0)
    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=hw) as pool:
            last_progress=time.perf_counter();last_file=last_progress
            for position,done in enumerate(pool.map(compile_unit,pending)):
                unit_times.append(done);now=time.perf_counter();done_bytes[0]+=unit_bytes[position]
                if now-last_file>=3:progress('compiling',len(unit_times));last_file=now
                if now-last_progress>=15:
                    print(f'Native objects compiled: {len(unit_times)}/{len(pending)}; elapsed: {now-started:.1f}s',file=sys.stderr,flush=True)
                    last_progress=now
    except subprocess.CalledProcessError as error:
        message=error.stderr.decode(errors='replace')
        if 'undeclared' in message:
            message+=('\ncompile_cached_fast: a translated unit uses a runtime name the generated page header does not '
                      'provide: add it to PAGE_ROOTS in split_page_header.py (split_page_header.py --scan <native-object-cache/link-*.rsp> lists them).')
        raise RuntimeError(message) from error
    prof.mark('compile')
    response_text='\n'.join('"'+obj.as_posix()+'"' for obj in results)+'\n'
    response=cache/f'link-{digest(response_text.encode())}.rsp'
    if not response.exists():response.write_text(response_text)
    if response.read_text()!=response_text:raise ValueError('Immutable object response mismatch')
    from third_party.build_opus import link_args as opus_link_args  # hwopus: native libopus (vendored, BSD)
    # host gamepads: SDL 3 (vendored, zlib) + host_input_sdl.c; runtime_support references them only with TOTK_DRAW_PATH
    from third_party.build_sdl import link_args as sdl_link_args
    # cutscene movies: libvpx (VP9 decode) + libwebm (mkvparser) + host_movie_*.c/cpp shims; runtime_support references
    # them weakly (host_movie.h), only with TOTK_DRAW_PATH; without them every movie takes the stub path
    from third_party.build_vpx import link_args as vpx_link_args
    from third_party.build_webm import link_args as webm_link_args
    link=[str(zig),'cc','-target','x86_64-windows-gnu','-shared','@'+str(response)]+opus_link_args()+(sdl_link_args()+vpx_link_args()+webm_link_args() if draw_path else [])+['-luser32','-ld3d12','-ldxgi']+(['-lbcrypt','-ldxguid'] if draw_path else [])+['-o',str(output)]
    if no_pdb:link.insert(-2,'-s')
    progress('linking',len(unit_times))
    subprocess.run(link,check=True)
    progress('done',len(unit_times))
    prof.mark('link')
    if reference_thread:
        reference_thread.join()
        if reference_error:raise reference_error[0]
    prof.mark('reference_write_wait')
    manifest['objects']=new_memo
    try:
        temporary=manifest_path.with_suffix(f'.{os.getpid()}.tmp');temporary.write_text(json.dumps(manifest));os.replace(temporary,manifest_path)
    except OSError as error:print(f'compile_cached_fast: manifest not saved: {error}',file=sys.stderr)
    compiled=len(pending);total_units=len(units)
    del units,checked,pending
    try:mv.release();m.close()
    except BufferError:pass
    source_file.close()
    if load_check and sys.platform=='win32':
        ctypes.CDLL(str(output.resolve()))
    prof.mark('load_check')
    report={'status':'PASS','optimization':optimization,'page_arch_flags':page_arch,'source_sha256':source_sha,
            'header_sha256':dependencies,'runtime_header_sha256':runtime_dependencies,'window_backend_sha256':backend_dependencies,
            'page_header_inline':len(page_split['inline']),'page_header_extern':len(page_split['extern']),'compiler_sha256':compiler,'units':total_units,
            'compiled_units':compiled,'reused_units':total_units-compiled,'compiler_workers':hw,
            'precompiled_header':precompiled_header,'pch_creation_seconds':pch_seconds,
            'seconds':time.perf_counter()-started,'output':str(output),'reference_source':str(reference) if reference else None,'object_response_file':str(response)}
    output.with_suffix('.build.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
    if profile_path:
        Path(profile_path).write_text(json.dumps({'phases':prof.phases,'seconds':report['seconds'],
            'unit_seconds':sorted(([n,round(t,2)] for n,t in unit_times),key=lambda x:-x[1])},indent=1)+'\n')
    return report

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--output',type=Path,default=ROOT/'cached-startup.dll')
    parser.add_argument('--optimization',choices=['O0','O2'],default='O2')
    parser.add_argument('--precompiled-header',action='store_true')
    parser.add_argument('--root',type=Path,default=ROOT,help=argparse.SUPPRESS)
    parser.add_argument('--verify-objects',action='store_true',help='re-hash every cached object (original behaviour)')
    parser.add_argument('--no-pdb',action='store_true',help='link with -s (no PDB / debug directory)')
    parser.add_argument('--no-load-check',action='store_true',help='skip ctypes.CDLL load test of the output')
    parser.add_argument('--workers',type=int,default=None)
    parser.add_argument('--profile',type=Path,default=None,help='write phase timings JSON here')
    args=parser.parse_args()
    build(args.output,args.optimization,args.precompiled_header,root=args.root.resolve(),verify_objects=args.verify_objects,
no_pdb=args.no_pdb,load_check=not args.no_load_check,
          workers=args.workers,profile_path=args.profile)
