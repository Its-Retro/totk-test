"""Compile immutable static page units, reusing content-checked object files."""
import os,argparse,concurrent.futures,ctypes,hashlib,json,os,re,subprocess,sys,time
from pathlib import Path
from split_runtime import split_runtime
from split_host_window import split_host_window

ROOT=Path(__file__).resolve().parent
def digest(data):return hashlib.sha256(data).hexdigest()
def build(output,optimization='O2',precompiled_header=False):
    started=time.perf_counter();source=(ROOT/'translated_startup.c').read_text()
    zig=ROOT.parent/'tools/zig-x86_64-windows-0.16.0/zig.exe'
    compiler=digest(zig.read_bytes())
    runtime_path=(ROOT/'boot_runtime.h').resolve()
    runtime_header,runtime_body=split_runtime(runtime_path.read_text(encoding='utf-8'))
    window_path=(ROOT/'host_window.h').resolve()
    window_api,window_implementation=split_host_window(window_path.read_text(encoding='utf-8'))
    cache=ROOT/'native-object-cache'
    def snapshot_headers(entry,overrides):
        headers={}
        def collect(path):
            path=path.resolve()
            if path in headers:return
            data=overrides[path] if path in overrides else path.read_bytes()
            headers[path]=data
            for name in re.findall(rb'^#include "([^\"]+)"',data,re.M):collect(path.parent/name.decode())
        collect(entry)
        checksum=digest(b''.join(str(p.relative_to(ROOT.parent)).encode()+b'\0'+headers[p] for p in sorted(headers)))
        snapshot=cache/'headers'/checksum
        for path,data in headers.items():
            target=snapshot/path.relative_to(ROOT.parent);target.parent.mkdir(parents=True,exist_ok=True)
            if not target.exists():target.write_bytes(data)
            if target.read_bytes()!=data:raise ValueError('Immutable header snapshot mismatch')
        return checksum,snapshot
    dependencies,snapshot=snapshot_headers(runtime_path,{runtime_path:runtime_header.encode(),window_path:window_api.encode()})
    backend_dependencies,backend_snapshot=snapshot_headers(window_path,{window_path:window_implementation.encode()})
    include=f'#include "{(snapshot/"startup/boot_runtime.h").as_posix()}"\n'
    backend_include=f'#include "{(backend_snapshot/"startup/host_window.h").as_posix()}"\n'
    # A full (~1 GB) monolithic reference copy per round is only needed by compare_cached.py
    # audits; it grew the cache by ~1 TB. Opt in with TOTK_KEEP_REFERENCE=1.
    reference=None
    if os.environ.get('TOTK_KEEP_REFERENCE')=='1':
        references=cache/'references';references.mkdir(exist_ok=True)
        reference_text=backend_include+source.replace('#include "boot_runtime.h"',include.strip())+'\n'+runtime_body
        reference=references/(digest(reference_text.encode())+'.c')
        if not reference.exists():reference.write_text(reference_text,encoding='utf-8',newline='\n')
        if reference.read_text(encoding='utf-8')!=reference_text:raise ValueError('Immutable reference source mismatch')
        del reference_text
    matches=list(re.finditer(r'^static __attribute__\(\(noinline\)\) uint32_t boot_page_([0-9a-f]+)\(',source,re.M))
    if not matches:raise ValueError('Expected partitioned native source')
    wrapper_start=source.index('static uint32_t boot_execute(') if 'static uint32_t boot_execute(' in source else source.index('__declspec(dllexport) uint32_t boot_run(')
    units=[];prototypes=[]
    for index,match in enumerate(matches):
        end=matches[index+1].start() if index+1<len(matches) else wrapper_start
        body=source[match.start():end].removeprefix('static ')
        name='page_'+match.group(1);units.append((name,include+body))
        prototypes.append(f'uint32_t boot_page_{match.group(1)}(BootContext*,uint64_t*,uint32_t*);')
    prefix=source[:matches[0].start()].replace('#include "boot_runtime.h"',include.strip())
    units.append(('dispatch',prefix+'\n'.join(prototypes)+'\n'+source[wrapper_start:]))
    units.append(('runtime_support',backend_include+include+runtime_body))
    objects=cache/'objects';objects.mkdir(parents=True,exist_ok=True)
    flags=['-target','x86_64-windows-gnu','-std=c11','-'+optimization,'-Wall','-Wextra','-Werror']
    pch_seconds=0
    if precompiled_header:
        pch_dir=cache/'pch';pch_dir.mkdir(exist_ok=True)
        pch=pch_dir/(digest((compiler+dependencies+json.dumps(flags)).encode())+'.pch')
        pch_receipt=pch.with_suffix('.sha256')
        if not (pch.exists() and pch_receipt.exists() and digest(pch.read_bytes())==pch_receipt.read_text()):
            pch_started=time.perf_counter();temporary=pch.with_suffix(f'.{os.getpid()}.tmp.pch')
            subprocess.run([str(zig),'cc',*flags,'-x','c-header',str(snapshot/'startup/boot_runtime.h'),'-o',str(temporary)],check=True)
            checksum=digest(temporary.read_bytes());os.replace(temporary,pch);pch_receipt.write_text(checksum)
            pch_seconds=time.perf_counter()-pch_started
        flags+=['-include-pch',str(pch)]
    # The D3D12 draw/present path lives only in the window backend, compiled into
    # runtime_support; translated pages never see it, so only that unit takes the define.
    draw_path=os.environ.get('TOTK_DRAW_PATH')=='1'
    def compile_unit(unit):
        name,text=unit
        unit_flags=flags
        if draw_path and name=='runtime_support':   # without the PCH: it was built without the define
            unit_flags=[f for i,f in enumerate(flags) if f!='-include-pch' and not (i and flags[i-1]=='-include-pch')]+['-DTOTK_DRAW_PATH']
        key=digest((compiler+dependencies+json.dumps(unit_flags)+text).encode())
        base=objects/(name+'-'+key);src=base.with_suffix('.c');obj=base.with_suffix('.o');receipt=base.with_suffix('.sha256')
        if obj.exists() and receipt.exists() and digest(obj.read_bytes())==receipt.read_text():return obj,False
        if not src.exists():src.write_text(text)
        if src.read_text()!=text:raise ValueError('Immutable source cache mismatch')
        temporary=base.with_suffix(f'.{os.getpid()}.tmp.o')
        subprocess.run([str(zig),'cc',*unit_flags,'-c',str(src),'-o',str(temporary)],check=True,capture_output=True)
        checksum=digest(temporary.read_bytes());os.replace(temporary,obj);receipt.write_text(checksum)
        return obj,True
    workers=min(8,max(1,(os.cpu_count() or 2)//2))
    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
            results=[];last_progress=started
            for result in pool.map(compile_unit,units):
                results.append(result)
                now=time.perf_counter()
                if now-last_progress>=15:
                    print(f'Native objects checked: {len(results)}/{len(units)}; rebuilt: {sum(changed for _,changed in results)}; elapsed: {now-started:.1f}s',file=sys.stderr,flush=True)
                    last_progress=now
    except subprocess.CalledProcessError as error:
        raise RuntimeError(error.stderr.decode(errors='replace')) from error
    response_text='\n'.join('"'+obj.as_posix()+'"' for obj,_ in results)+'\n'
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
    draw=os.environ.get('TOTK_DRAW_PATH')=='1'
    subprocess.run([str(zig),'cc','-target','x86_64-windows-gnu','-shared','@'+str(response)]+opus_link_args()+(sdl_link_args()+vpx_link_args()+webm_link_args() if draw else [])+['-luser32','-ld3d12','-ldxgi']+(['-lbcrypt','-ldxguid'] if draw else [])+['-o',str(output)],check=True)
    if sys.platform=='win32':
        ctypes.CDLL(str(output.resolve()))
    report={'status':'PASS','optimization':optimization,'source_sha256':digest(source.encode()),
            'header_sha256':dependencies,'window_backend_sha256':backend_dependencies,'compiler_sha256':compiler,'units':len(units),
            'compiled_units':sum(changed for _,changed in results),'reused_units':sum(not changed for _,changed in results),'compiler_workers':workers,
            'precompiled_header':precompiled_header,'pch_creation_seconds':pch_seconds,
            'runtime_support_sha256':digest(runtime_body.encode()),
            'seconds':time.perf_counter()-started,'output':str(output),'reference_source':str(reference) if reference else None,'object_response_file':str(response)}
    output.with_suffix('.build.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
    return report

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--output',type=Path,default=ROOT/'cached-startup.dll')
    parser.add_argument('--optimization',choices=['O0','O2'],default='O2')
    parser.add_argument('--precompiled-header',action='store_true');args=parser.parse_args()
    build(args.output,args.optimization,args.precompiled_header)
