"""totk-test builder: your own extracted ExeFS + RomFS -> a native Windows build of the game, in one command.

Normally started by build.cmd / build.ps1, which find or fetch Python 3.12 and the pinned Python packages first:
    build.cmd -ExeFS <folder> -RomFS <folder or romfs.bin> [-OutputDir <folder>] [-Workers <n>]
Directly (Python 3.12 with builder/requirements.txt installed):
    python builder/build.py --exefs DIR --romfs DIR|FILE [--out DIR] [--workers N]
                            [--from STEP | --only STEP] [--check-inputs] [--dry-run]

Everything generated goes into the output folder (default: <repo>/output, ignored by git). It mirrors the
development layout the scripts expect:
    output/startup/      copy of the repo's startup/ plus everything generated there (C, DLL, shaders, saves)
    output/call-chain/, output/assets/*.py   copies of the repo's helper sources
    output/exefs/, output/uncompressed/      your ExeFS files and their decompressed modules
    output/assets/romfs.bin                  your RomFS as one image
    output/tools/, output/downloads/         zig, DXC (and Python if it was fetched)
    output/logs/                             one log per step
A step that already finished with the same inputs is skipped, so running the command again resumes.
"""
import argparse, ctypes, hashlib, json, os, platform, shutil, struct, subprocess, sys, tarfile, time, urllib.request, zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
sys.path.insert(0, str(HERE))
import pins, nso, romfs_pack          # noqa: E402

BELOW_NORMAL = 0x4000


class BuildError(Exception):
    """A problem the user can act on; the message is shown as is."""


def say(msg=''):
    print(msg, flush=True)


def human(n):
    for unit in ('B', 'KB', 'MB', 'GB', 'TB'):
        if n < 1000 or unit == 'TB': return f'{n:.1f} {unit}' if unit != 'B' else f'{n} B'
        n /= 1000


def minutes(s):
    s = int(s)
    return f'{s // 3600} h {s % 3600 // 60:02d} min' if s >= 3600 else f'{s // 60} min {s % 60:02d} s'


def sha256_file(path, progress=None):
    h = hashlib.sha256(); size = path.stat().st_size; done = 0; last = 0.0
    with open(path, 'rb', buffering=0) as f:
        buf = bytearray(4 << 20); view = memoryview(buf)
        while True:
            n = f.readinto(buf)
            if not n: break
            h.update(view[:n]); done += n
            if progress and time.monotonic() - last > 5: progress(done, size); last = time.monotonic()
    return h.hexdigest()


def fingerprint(paths, extra=''):
    h = hashlib.sha256(extra.encode())
    for p in sorted(paths, key=str):
        h.update(str(p).encode()); h.update(Path(p).read_bytes() if Path(p).is_file() else b'<missing>')
    return h.hexdigest()


# ---------------------------------------------------------------------------------------------------------------
class Ctx:
    def __init__(self, a):
        self.a = a
        self.out = Path(a.out).resolve()
        self.startup = self.out / 'startup'
        self.tools = self.out / 'tools'
        self.logs = self.out / 'logs'
        self.downloads = self.out / 'downloads'
        self.state_path = self.out / 'build-state.json'
        try: self.state = json.loads(self.state_path.read_text())
        except (OSError, ValueError): self.state = {}
        self.zig = self.tools / pins.ZIG['dir'] / 'zig.exe'
        self.dxc = self.tools / pins.DXC['dir'] / 'bin' / 'x64' / 'dxc.exe'
        self.perl = self.git = None
        self.cpu_v3 = True
        self.ram = 0
        self.workers = a.workers
        self.romfs_source = None

    def save(self):
        self.out.mkdir(parents=True, exist_ok=True)
        tmp = self.state_path.with_suffix('.tmp'); tmp.write_text(json.dumps(self.state, indent=1)); os.replace(tmp, self.state_path)

    def env(self, **extra):
        e = dict(os.environ)
        e.update(PYTHONNOUSERSITE='1', TOTK_DRAW_PATH='1', TOTK_MEMORY_PROFILE='expanded-lab')
        if self.perl: e['PERL'] = str(self.perl)
        e.update({k: str(v) for k, v in extra.items()})
        return e

    def run(self, cmd, log, cwd=None, env=None, heartbeat=None, what=None):
        """Run a tool at below-normal priority with its output in logs/<log>.log; show a heartbeat line every
        minute; on failure raise BuildError with the end of the log."""
        self.logs.mkdir(parents=True, exist_ok=True)
        path = self.logs / f'{log}.log'
        cmd = [str(c) for c in cmd]
        with open(path, 'a', encoding='utf-8', errors='replace') as out:
            out.write(f'\n===== {time.strftime("%Y-%m-%d %H:%M:%S")} {" ".join(cmd)}\n'); out.flush()
            started = time.monotonic()
            p = subprocess.Popen(cmd, cwd=cwd or self.startup, env=env or self.env(), stdout=out, stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, creationflags=BELOW_NORMAL)
            try:
                while True:
                    try: rc = p.wait(timeout=60); break
                    except subprocess.TimeoutExpired:
                        extra = ''
                        if heartbeat:
                            try: extra = heartbeat() or ''
                            except Exception: extra = ''
                        say(f'      ... still working ({minutes(time.monotonic() - started)}){": " + extra if extra else ""}')
            except KeyboardInterrupt:
                subprocess.run(['taskkill', '/T', '/F', '/PID', str(p.pid)], capture_output=True)
                raise
        if rc != 0:
            tail = path.read_text(encoding='utf-8', errors='replace').splitlines()[-25:]
            raise BuildError(f'{what or Path(cmd[1] if len(cmd) > 1 else cmd[0]).name} failed (exit code {rc}).\n'
                             f'Last lines of {path}:\n    ' + '\n    '.join(tail))
        return path

    def py(self, script, *args, log, **kw):
        return self.run([sys.executable, '-B', script, *args], log, **kw)


# ---------------------------------------------------------------------------------------------------------------
# Step 1: the PC
def total_ram():
    class MS(ctypes.Structure):
        _fields_ = [('len', ctypes.c_ulong), ('load', ctypes.c_ulong), ('total', ctypes.c_ulonglong), ('avail', ctypes.c_ulonglong),
                    ('tp', ctypes.c_ulonglong), ('ap', ctypes.c_ulonglong), ('tv', ctypes.c_ulonglong), ('av', ctypes.c_ulonglong),
                    ('ae', ctypes.c_ulonglong)]
    m = MS(); m.len = ctypes.sizeof(MS)
    return m.total if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m)) else 0


def step_check_pc(c):
    if sys.platform != 'win32': raise BuildError('This builder runs on Windows 10 or 11 (64-bit) only.')
    if platform.machine().upper() not in ('AMD64', 'X86_64'):
        raise BuildError(f'This PC is {platform.machine()}; the game needs a 64-bit x86 (Intel/AMD) PC. Windows on ARM is not supported.')
    w = sys.getwindowsversion()
    say(f'    Windows {w.major}.{w.minor} build {w.build}' + ('' if w.build >= 19041 else '  (older than Windows 10 2004: untested)'))
    if sys.version_info[:2] != (3, 12):
        raise BuildError(f'Python {platform.python_version()} is running this script; the build needs Python 3.12 (build.cmd sets it up).')
    try:
        import capstone, zstandard
    except ImportError as e:
        raise BuildError(f'A Python package is missing ({e.name}). Run build.cmd, which installs builder/requirements.txt.')
    for mod, want in (('capstone', pins.PY_PACKAGES['capstone']), ('zstandard', pins.PY_PACKAGES['zstandard'])):
        have = sys.modules[mod].__version__
        if have != want: raise BuildError(f'{mod} {have} is installed; the build needs exactly {want} (builder/requirements.txt).')
    k = ctypes.windll.kernel32
    c.cpu_v3 = bool(k.IsProcessorFeaturePresent(40))      # PF_AVX2_INSTRUCTIONS_AVAILABLE
    say(f'    CPU: {platform.processor() or "unknown"}; AVX2 ' + ('yes' if c.cpu_v3 else 'NOT CONFIRMED'))
    if not c.cpu_v3:
        say('    WARNING: the game needs an x86-64-v3 CPU (AVX2/FMA/BMI2: Intel Haswell 2013 or newer, AMD Ryzen).\n'
            '    Windows could not confirm AVX2 here. The build continues, but the game may refuse to start.')
    c.ram = total_ram()
    say(f'    RAM: {human(c.ram)}')
    cpus = os.cpu_count() or 2
    if not c.workers:
        c.workers = max(1, min(8, cpus // 2))
        if c.ram and c.ram < 15 * 10**9: c.workers = min(c.workers, 4)
    say(f'    Parallel jobs: {c.workers} (change with -Workers)')
    if c.ram and c.ram < 15 * 10**9:
        say('    WARNING: less than 16 GB of RAM. The build may run out of memory; use -Workers 2 and a large page file.')
    if len(str(c.out)) > 90 and c.a.check_inputs:
        say(f'    WARNING: the output folder path is long ({len(str(c.out))} characters); use a short one such as C:\\totk-output to build.')
    elif len(str(c.out)) > 90:
        raise BuildError(f'The output folder path is {len(str(c.out))} characters long; some generated paths would pass the\n'
                         f'Windows 260-character limit. Use a short path such as C:\\totk-output (-OutputDir).')
    c.out.mkdir(parents=True, exist_ok=True)
    free = shutil.disk_usage(c.out).free
    existing = sum(p.stat().st_size for p in (c.out / 'assets/romfs.bin', c.startup / 'translated_startup.c',
                                                c.startup / 'emitted-instructions.sqlite') if p.is_file())
    need = (pins.DISK_NEEDED_NO_PACK if Path(c.a.romfs).is_file() else pins.DISK_NEEDED) - existing
    say(f'    Free space on the output drive: {human(free)} (this build needs about {human(max(need, 0))} more)')
    if free < need and c.a.check_inputs:
        say('    WARNING: not enough free space for the build itself.')
    elif free < need:
        raise BuildError(f'Not enough free disk space on {c.out.anchor}: {human(free)} free, about {human(need)} needed.\n'
                         f'Free some space or pass -OutputDir on a bigger drive.')
    try:
        gpus = subprocess.run(['powershell', '-NoProfile', '-Command', '(Get-CimInstance Win32_VideoController).Name'],
                              capture_output=True, text=True, timeout=30).stdout.split('\n')
        say('    GPU: ' + '; '.join(g.strip() for g in gpus if g.strip()) + '  (needs Direct3D 12 with Shader Model 6.6)')
    except Exception:
        pass


# Step 2: sources
SOURCE_DIRS = ('startup', 'call-chain')


def step_sources(c):
    copied = 0
    for top in SOURCE_DIRS:
        for src in (REPO / top).rglob('*'):
            if not src.is_file() or '__pycache__' in src.parts: continue
            dst = c.out / src.relative_to(REPO)
            if dst.is_file() and dst.stat().st_size == src.stat().st_size and dst.read_bytes() == src.read_bytes(): continue
            dst.parent.mkdir(parents=True, exist_ok=True); shutil.copy2(src, dst); copied += 1
    for src in (REPO / 'assets').glob('*.py'):
        dst = c.out / 'assets' / src.name
        if not (dst.is_file() and dst.read_bytes() == src.read_bytes()):
            dst.parent.mkdir(parents=True, exist_ok=True); shutil.copy2(src, dst); copied += 1
    say(f'    {copied} file(s) updated in {c.out}')


# Step 3: toolchain
def download(c, url, sha256, name, size=None):
    c.downloads.mkdir(parents=True, exist_ok=True)
    dest = c.downloads / name
    if dest.is_file() and sha256_file(dest) == sha256: return dest
    tmp = dest.with_name(dest.name + '.partial')
    say(f'    downloading {url}' + (f' ({human(size)})' if size else ''))
    req = urllib.request.Request(url, headers={'User-Agent': 'totk-test-builder'})
    try:
        with urllib.request.urlopen(req, timeout=120) as r, open(tmp, 'wb') as f:
            total = int(r.headers.get('Content-Length') or size or 0); done = 0; last = 0.0
            while True:
                b = r.read(1 << 20)
                if not b: break
                f.write(b); done += len(b)
                if total and time.monotonic() - last > 5:
                    say(f'      {done * 100 // total}% of {human(total)}'); last = time.monotonic()
    except OSError as e:          # URLError, timeouts and connection resets are OSErrors
        raise BuildError(f'Could not download {url}\n  ({e})\nCheck the internet connection and run the command again.')
    got = sha256_file(tmp)
    if got != sha256:
        tmp.unlink()
        raise BuildError(f'The download of {name} does not have the expected SHA-256.\n  expected {sha256}\n  got      {got}\n'
                         f'It may be a network problem; run the command again. If it keeps failing, report it.')
    os.replace(tmp, dest)
    return dest


def safe_extract_zip(archive, target):
    target = Path(target).resolve()
    with zipfile.ZipFile(archive) as z:
        for info in z.infolist():
            name = info.filename.replace('\\', '/')
            dest = (target / name).resolve()
            if target != dest and target not in dest.parents: raise BuildError(f'{archive.name}: unsafe path {name!r}')
            if name.endswith('/'): dest.mkdir(parents=True, exist_ok=True); continue
            dest.parent.mkdir(parents=True, exist_ok=True)
            with z.open(info) as s, open(dest, 'wb') as d: shutil.copyfileobj(s, d, 1 << 20)


def find_git():
    for cand in (shutil.which('git'), r'C:\Program Files\Git\cmd\git.exe', r'C:\Program Files (x86)\Git\cmd\git.exe'):
        if cand and Path(cand).is_file(): return Path(cand)
    return None


def find_perl(git):
    cands = [os.environ.get('PERL'), shutil.which('perl')]
    if git: cands.append(str(Path(git).resolve().parent.parent / 'usr' / 'bin' / 'perl.exe'))
    cands += [r'C:\Program Files\Git\usr\bin\perl.exe', r'C:\Strawberry\perl\bin\perl.exe']
    for cand in cands:
        if cand and Path(cand).is_file():
            try:
                if subprocess.run([cand, '-e', 'print 1'], capture_output=True, text=True, timeout=30).stdout == '1': return Path(cand)
            except (OSError, subprocess.SubprocessError):
                pass
    return None


def step_toolchain(c):
    c.git = find_git()
    c.perl = find_perl(c.git)
    if not c.git:
        raise BuildError('Git was not found. Install Git for Windows from https://git-scm.com/download/win\n'
                         '(it also brings Perl, which the libvpx build needs) and run the command again.')
    if not c.perl:
        raise BuildError('Perl was not found (libvpx needs it). It comes with Git for Windows in "Git\\usr\\bin\\perl.exe";\n'
                         'reinstall Git for Windows or set the PERL environment variable to a perl.exe.')
    say(f'    git: {c.git}\n    perl: {c.perl}')
    if not c.zig.is_file():
        z = download(c, pins.ZIG['url'], pins.ZIG['sha256'], Path(pins.ZIG['url']).name, pins.ZIG['bytes'])
        say('    unpacking zig'); safe_extract_zip(z, c.tools)
    if not c.zig.is_file(): raise BuildError(f'zig.exe is missing after unpacking ({c.zig}).')
    dxc_dir = c.tools / pins.DXC['dir']
    if not all((dxc_dir / 'bin/x64' / f).is_file() for f in ('dxc.exe', 'dxcompiler.dll', 'dxil.dll')):
        z = download(c, pins.DXC['url'], pins.DXC['sha256'], Path(pins.DXC['url']).name, pins.DXC['bytes'])
        say('    unpacking DXC'); safe_extract_zip(z, dxc_dir)
    if not c.dxc.is_file(): raise BuildError(f'dxc.exe is missing after unpacking ({c.dxc}).')
    say(f'    zig {pins.ZIG["version"]}, DXC {pins.DXC["version"]}: ready')


# Step 4: third-party sources
def git_checkout(c, spec, dest):
    if (dest / '.git').is_dir():
        head = subprocess.run([str(c.git), '-C', str(dest), 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()
        if head == spec['commit']: return
        shutil.rmtree(dest)
    elif dest.exists():
        shutil.rmtree(dest)
    for url in (spec['git'], spec['mirror']):
        say(f'    cloning {url} at {spec["tag"]}')
        r = subprocess.run([str(c.git), '-c', 'core.autocrlf=false', '-c', 'advice.detachedHead=false', 'clone', '--quiet',
                            '--depth', '1', '--branch', spec['tag'], url, str(dest)], capture_output=True, text=True)
        if r.returncode == 0: break
        say(f'      failed: {r.stderr.strip().splitlines()[-1] if r.stderr.strip() else r.returncode}')
        if dest.exists(): shutil.rmtree(dest)
    else:
        raise BuildError(f'Could not download {spec["dir"]} with git (check the internet connection).')
    head = subprocess.run([str(c.git), '-C', str(dest), 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()
    if head != spec['commit']:
        raise BuildError(f'{spec["dir"]}: tag {spec["tag"]} is commit {head}, expected {spec["commit"]}. Not building with it.')


def step_libraries(c):
    tp = c.startup / 'third_party'; tp.mkdir(parents=True, exist_ok=True)
    if not (tp / pins.OPUS['dir'] / 'celt_sources.mk').is_file():
        a = download(c, pins.OPUS['url'], pins.OPUS['sha256'], Path(pins.OPUS['url']).name)
        say('    unpacking libopus')
        with tarfile.open(a) as t: t.extractall(tp, filter='data')
    if not (tp / pins.SDL['dir'] / 'include' / 'SDL3' / 'SDL.h').is_file():
        a = download(c, pins.SDL['url'], pins.SDL['sha256'], Path(pins.SDL['url']).name)
        say('    unpacking SDL3'); safe_extract_zip(a, tp)
    git_checkout(c, pins.LIBVPX, tp / pins.LIBVPX['dir'])
    git_checkout(c, pins.LIBWEBM, tp / pins.LIBWEBM['dir'])
    say(f'    libopus {pins.OPUS["version"]}, SDL {pins.SDL["version"]}, libvpx {pins.LIBVPX["version"]}, libwebm {pins.LIBWEBM["version"]}: ready')


# Step 5: ExeFS
EXEFS_NAMES = ('main', 'main.npdm', 'rtld', 'sdk', 'subsdk0')


def exefs_folder(path):
    p = Path(path)
    if not p.is_dir(): raise BuildError(f'The ExeFS folder {p} does not exist.')
    if not (p / 'main').is_file() and not (p / 'main.nso').is_file():
        for sub in ('exefs', 'ExeFS', 'EXEFS'):
            if (p / sub / 'main').is_file() or (p / sub / 'main.nso').is_file(): return p / sub
    return p


def check_exefs(c):
    folder = exefs_folder(c.a.exefs)
    found, problems = {}, []
    for name in EXEFS_NAMES:
        cand = [folder / name] + ([folder / f'{name}.nso'] if name != 'main.npdm' else [])
        path = next((p for p in cand if p.is_file()), None)
        if not path: problems.append(f'  - "{name}" is missing'); continue
        data = path.read_bytes(); digest = hashlib.sha256(data).hexdigest()
        if name == 'main.npdm':
            if digest != pins.EXEFS[name][1]: problems.append(f'  - main.npdm is not the base game\'s (sha256 {digest[:16]}...)')
            else: found[name] = (path, 'npdm')
            continue
        if digest == pins.EXEFS[name][1]: found[name] = (path, 'compressed'); continue
        if digest == pins.UNCOMPRESSED[name][1]: found[name] = (path, 'decompressed'); continue
        try: bid = nso.build_id(data)[:40]
        except nso.NsoError: problems.append(f'  - "{path.name}" is not an NSO file'); continue
        if bid == pins.BUILD_IDS[name]:
            problems.append(f'  - "{path.name}" has the right build id but different bytes: the file is probably damaged; dump it again')
        else:
            problems.append(f'  - "{path.name}" is from a different version of the game (build id {bid}, expected {pins.BUILD_IDS[name]})')
    if problems:
        hint = ''
        if any('different version' in p for p in problems):
            hint = (f'\nThis test needs the base game, version {pins.GAME["version"]} (main build id {pins.GAME["main_build_id"]}).\n'
                    'An installed update replaces the ExeFS: dump the ExeFS of the base game (cartridge or base NSP), not the update.')
        raise BuildError(f'Your ExeFS folder ({folder}) does not match {pins.GAME["name"]} {pins.GAME["version"]}:\n'
                         + '\n'.join(problems) + hint + f'\nIt needs these five files: {", ".join(EXEFS_NAMES)}.')
    return found


def step_exefs(c):
    found = check_exefs(c)
    dest = c.out / 'exefs'; dest.mkdir(parents=True, exist_ok=True)
    for name, (path, kind) in found.items():
        target = dest / name
        if not (target.is_file() and target.stat().st_size == path.stat().st_size and target.read_bytes() == path.read_bytes()):
            shutil.copyfile(path, target)
    say(f'    all five files match {pins.GAME["title_id"]} v{pins.GAME["version"]} (main build id {pins.GAME["main_build_id"][:16]}...)')


# Step 6: decompress
def step_decompress(c):
    dest = c.out / 'uncompressed'; dest.mkdir(parents=True, exist_ok=True)
    for name in ('rtld', 'main', 'subsdk0', 'sdk'):
        target = dest / name; size, want = pins.UNCOMPRESSED[name]
        if target.is_file() and target.stat().st_size == size and sha256_file(target) == want:
            say(f'    {name}: already decompressed'); continue
        data = (c.out / 'exefs' / name).read_bytes()
        t = time.monotonic()
        out = data if data[:4] == b'NSO0' and not struct.unpack_from('<I', data, 12)[0] & 7 else nso.decompress(data)
        if hashlib.sha256(out).hexdigest() != want:
            raise BuildError(f'{name} decompressed to unexpected bytes; the ExeFS file may be damaged. Dump it again.')
        tmp = target.with_suffix('.partial'); tmp.write_bytes(out); os.replace(tmp, target)
        say(f'    {name}: {human(len(data))} -> {human(len(out))} ({time.monotonic() - t:.1f} s), SHA-256 matches')


# Step 7: RomFS
def romfs_source(c):
    p = Path(c.a.romfs)
    if p.is_file(): return p
    if not p.is_dir(): raise BuildError(f'The RomFS path {p} does not exist.')
    if not (p / 'Pack').is_dir():
        for sub in ('romfs', 'RomFS', 'ROMFS'):
            if (p / sub / 'Pack').is_dir(): return p / sub
    return p


def explain_romfs(files, ndirs):
    names = {f[0].decode() for f in files}
    lines = [f'  found {len(files):,} files and {ndirs - 1:,} folders; the base game has {pins.ROMFS["files"]:,} files and '
             f'{pins.ROMFS["dirs"] - 1:,} folders']
    for k in pins.ROMFS['key_files']:
        if k not in names: lines.append(f'  - {k} is missing')
    if len(files) > pins.ROMFS['files']:
        lines.append('  More files than the base game: update or mod files may be mixed in. Use the base game\'s RomFS only.')
    elif len(files) < pins.ROMFS['files']:
        lines.append('  Fewer files than the base game: the extraction may be incomplete. Extract the RomFS again.')
    else:
        lines.append('  The file count is right, so some names or sizes differ: update or mod files may have replaced originals.')
    return '\n'.join(lines)


def check_romfs(c, write=False):
    src = romfs_source(c)
    image = c.out / 'assets' / 'romfs.bin'
    stamp = image.with_name('romfs.bin.json')
    try: st = json.loads(stamp.read_text())
    except (OSError, ValueError): st = {}
    if image.is_file() and st.get('image_sha256') == pins.ROMFS['image_sha256'] and st.get('bytes') == image.stat().st_size \
            and st.get('mtime') == image.stat().st_mtime_ns:
        say('    assets/romfs.bin is already in place and verified'); return
    if src.is_file():
        sys.path.insert(0, str(REPO / 'assets'))
        from romfs_reader import RomFS
        try: fs = RomFS(src)
        except ValueError as e: raise BuildError(f'{src} is not a RomFS image ({e}).')
        if fs.metadata_sha256 != pins.ROMFS['metadata_sha256']:
            raise BuildError(f'{src} is a RomFS image, but not the base game\'s ({len(fs.files):,} files; expected '
                             f'{pins.ROMFS["files"]:,}). Use the RomFS of v{pins.GAME["version"]} with no update.')
        say(f'    {src.name}: the file list matches the base game')
        if not write: return
        image.parent.mkdir(parents=True, exist_ok=True)
        if image.exists() and os.path.samefile(src, image):
            say('    it is already the output folder\'s assets/romfs.bin')
        else:
            if image.exists(): image.unlink()
            try:
                os.link(src, image); say('    linked into the output folder (no copy needed)')
            except OSError:
                say(f'    copying {human(src.stat().st_size)} into the output folder'); shutil.copyfile(src, image)
        say('    checking the whole image (one read, a few minutes)')
        got = sha256_file(image, lambda d, t: say(f'      {d * 100 // t}%'))
    else:
        say(f'    reading the file list of {src}')
        files, ndirs = romfs_pack.scan(src)
        meta = romfs_pack.build(files)
        if romfs_pack.metadata_sha256(meta) != pins.ROMFS['metadata_sha256']:
            raise BuildError(f'Your RomFS folder ({src}) is not the base game\'s RomFS:\n' + explain_romfs(files, ndirs))
        say(f'    {meta["files"]:,} files in {meta["dirs"] - 1:,} folders: names and sizes match the base game')
        if not write: return
        image.parent.mkdir(parents=True, exist_ok=True)
        say(f'    packing {human(meta["image_bytes"])} into assets/romfs.bin (10-30 minutes, disk speed decides)')
        t0 = time.monotonic()
        def progress(done, total):
            rate = done / max(time.monotonic() - t0, 1e-3)
            say(f'      {done * 100 // total}%  {human(rate)}/s  about {minutes((total - done) / max(rate, 1))} left')
        state = {'last': 0}
        def throttled(done, total):
            if done == total or time.monotonic() - state['last'] > 30: progress(done, total); state['last'] = time.monotonic()
        got = romfs_pack.write_image(meta, image, throttled)
    if got != pins.ROMFS['image_sha256'] and not c.a.allow_romfs_mismatch:
        raise BuildError('The RomFS has the right file names and sizes, but the contents of at least one file differ from the\n'
                         'base game (a damaged or modified file). Extract the RomFS again. (To build anyway, add\n'
                         '-AllowRomfsMismatch; the game may then break in unexpected ways.)')
    stamp.write_text(json.dumps({'image_sha256': got, 'bytes': image.stat().st_size, 'mtime': image.stat().st_mtime_ns,
                                 'source': str(src)}, indent=1))
    say('    RomFS image verified: identical to the base game\'s' if got == pins.ROMFS['image_sha256'] else
        '    WARNING: RomFS contents differ from the base game (continuing because of -AllowRomfsMismatch)')


def step_romfs(c):
    check_romfs(c, write=True)


# Step 8: headers generated from the game files
def step_headers(c):
    c.py('service_profile.py', log='headers', what='Writing boot_service_access.h from main.npdm')
    c.py('gen_fp_paths.py', log='headers', what='Writing boot_fp_paths.h')
    c.py('gen_game_headers.py', log='headers', what='Writing the NVN name and format headers')
    say('    boot_service_access.h, boot_fp_paths.h, host_nvn_names.h, generated_nvn_format_*.h, generated_gpu_macros.h (stub)')


# Step 9: translate
def step_translate(c):
    db = c.startup / 'emitted-instructions.sqlite'
    def beat():
        return f'instruction cache {human(db.stat().st_size)}' if db.is_file() else ''
    c.py('incremental/translate_startup_incremental.py', '--ranges-dir', '.', '--output', 'translated_startup.c',
         '--edges-output', 'external-edges.json', '--sidecar', 'incremental-page-cache.sqlite', '--write-instruction-cache',
         log='translate', heartbeat=beat, what='The ARM64 -> C translation')
    out = c.startup / 'translated_startup.c'
    say(f'    translated_startup.c: {human(out.stat().st_size)}')


# Step 10: shaders
def step_shaders(c):
    env = c.env(SHADER_WORKERS=c.workers)
    say('    helper shaders (clear, diagnostics)')
    for stage, entry, out_name, src in (('vs_6_6', 'vs_main', 'clear.vs.dxil', 'clear.hlsl'), ('ps_6_6', 'ps_float', 'clear-float.ps.dxil', 'clear.hlsl'),
                                        ('ps_6_6', 'ps_uint', 'clear-uint.ps.dxil', 'clear.hlsl'), ('ps_6_6', 'ps_sint', 'clear-sint.ps.dxil', 'clear.hlsl')):
        c.run([c.dxc, '-nologo', '-T', stage, '-E', entry, '-O3', '-Fo', out_name, src], 'shaders', cwd=c.startup / 'clear-shaders', what='DXC (clear shaders)')
    for name in ('const.ps', 'varying3.ps'):   # optional: only used with debug settings
        try:
            c.run([c.dxc, '-nologo', '-T', 'ps_6_6', '-E', 'main', '-O3', '-Fo', f'{name}.dxil', f'{name}.hlsl'], 'shaders',
                  cwd=c.startup / 'diag-shaders', what='DXC (diagnostic shaders)')
        except BuildError:
            say(f'      (diagnostic shader {name} did not compile; it is optional, continuing)')
    progs = c.startup / 'shader-programs'
    beat = lambda: f'{sum(1 for _ in progs.glob("*.bin")):,} programs extracted' if progs.is_dir() else ''
    say('    S1 extracting shader programs from the RomFS shader archives')
    c.py('inventory_game_shaders.py', '--no-disasm', log='shaders', env=env, heartbeat=beat, what='Shader inventory')
    say('    S2/S3 shader headers and vertex/pixel pairs')
    c.py('extract_shader_headers.py', log='shaders', env=env, what='Shader header extraction')
    c.py('extract_shader_pairs.py', log='shaders', env=env, what='Shader pair extraction')
    say('    early-Z flags')
    c.py('shader-tools/extract_early_z.py', 'early-z-scan.tsv', log='shaders', env=env, what='Early-Z scan')
    c.py('shader-tools/extract_early_z.py', 'early-z-sharcb.tsv', '--sharcb', log='shaders', env=env, what='Early-Z scan (AGL)')
    (c.startup / 'shader-early-z.tsv').write_bytes((c.startup / 'early-z-scan.tsv').read_bytes() + (c.startup / 'early-z-sharcb.tsv').read_bytes())
    hl = c.startup / 'shader-hlsl'
    beat = lambda: f'{sum(1 for _ in hl.glob("*.dxil")):,} DXIL files' if hl.is_dir() else ''
    say(f'    S4 translating the game\'s shader programs to HLSL and compiling them to DXIL ({c.workers} jobs)')
    c.py('translate_game_shaders.py', log='shaders', env=env, heartbeat=beat, what='Shader translation')
    c.py('write_shader_manifest.py', log='shaders', env=env, what='Writing shader-manifest.tsv')
    say('    S6 scanning effect, UI and main-module data for more shader programs')
    seg = c.out / 'exefs-scan' / 'main.text-rodata-data.bin'; seg.parent.mkdir(exist_ok=True)
    seg.write_bytes(nso.segment_image((c.out / 'uncompressed' / 'main').read_bytes()))
    scans = (('effect-shaders', []), ('other-shaders', ['UI/', 'Lib/']), ('shader-scan', ['Shader/', 'Lib/']), ('exefs-shaders', ['--file', str(seg)]))
    expected = {'effect-shaders': 12687, 'other-shaders': 174, 'shader-scan': 9191, 'exefs-shaders': 40}
    for d, extra in scans:
        c.py('debug/effect_shader_scan.py', str(c.out / d), *extra, log='shaders', env=env, what=f'Shader scan ({d})')
        n = sum(1 for _ in (c.out / d).glob('*.bin'))
        say(f'      {d}: {n:,} programs' + ('' if n == expected[d] else f'  (the development build found {expected[d]:,})'))
    say(f'    S7 translating the scanned programs ({c.workers} jobs)')
    c.run([sys.executable, '-B', HERE / 'runtime_shaders_parallel.py', c.startup, c.workers] + [c.out / d for d, _ in
          (('other-shaders', 0), ('exefs-shaders', 0), ('effect-shaders', 0), ('shader-scan', 0))], 'shaders', env=env,
          heartbeat=beat, what='Scanned shader translation')
    rows = sum(1 for l in (c.startup / 'shader-manifest.tsv').read_text(encoding='utf-8').splitlines() if l)
    say(f'    shader-manifest.tsv: {rows:,} shaders (the development build has about 22,560 before run-time captures)')


# Step 11: compile
def step_compile(c):
    prog = c.startup / 'build-progress.json'
    def beat():
        p = json.loads(prog.read_text())
        return f'{p["phase"]}: {p["compiled"]:,} of {p["total"]:,} units' if p.get('total') else p.get('phase', '')
    args = ['--optimization', 'O2', '--precompiled-header', '--no-pdb', '--output', 'totk-play.dll', '--workers', str(c.workers)]
    if not c.cpu_v3: args.append('--no-load-check')
    c.py('incremental/compile_cached_fast.py', *args, log='compile', env=c.env(TOTK_COMPILE_WORKERS=c.workers), heartbeat=beat,
         what='The game DLL compile')
    dll = c.startup / 'totk-play.dll'
    say(f'    totk-play.dll: {human(dll.stat().st_size)}')
    if dll.stat().st_size >= 2 * 1024**3: raise BuildError('totk-play.dll is 2 GB or larger; Windows cannot load it. Report this.')


# Step 12: launcher
def step_launcher(c):
    c.run([c.zig, 'cc', '-target', 'x86_64-windows-gnu', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-municode', '-I', 'startup',
           'startup/totk_launcher.c', '-lbcrypt', '-luser32', '-o', 'startup/totk.exe'], 'launcher', cwd=c.out, what='The launcher build')
    say('    startup/totk.exe built')


# Step 13: start script
def step_play(c):
    shutil.copyfile(REPO / 'play.ps1', c.out / 'play.ps1')
    shutil.copyfile(HERE / 'shader_watch.py', c.out / 'tools' / 'shader_watch.py')
    py = Path(sys.executable)
    try: py_ref = str(py.relative_to(c.out))
    except ValueError: py_ref = str(py)
    (c.out / 'play-settings.json').write_text(json.dumps({'python': py_ref}, indent=1))
    (c.out / 'play.cmd').write_bytes(b'@echo off\r\nrem Starts the game (see play.ps1 for options).\r\n'
                                     b'powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0play.ps1" %*\r\n')
    say(f'    {c.out / "play.cmd"}')


STEPS = [
    ('check-pc', 'Checking your PC', 'seconds', step_check_pc, None),
    ('sources', 'Copying the source code into the output folder', 'seconds', step_sources, None),
    ('toolchain', 'Getting the compilers (zig, DXC) and checking for Git and Perl', 'a few minutes the first time', step_toolchain, None),
    ('libraries', 'Getting the open-source libraries (libopus, SDL3, libvpx, libwebm)', 'a few minutes the first time', step_libraries, None),
    ('exefs', 'Checking your ExeFS files', 'seconds', step_exefs, None),
    ('decompress', 'Decompressing the game code (NSO -> uncompressed modules)', 'about 10 seconds', step_decompress, None),
    ('romfs', 'Preparing your RomFS (one image file the game reads)', '10-30 minutes', step_romfs, None),
    ('headers', 'Writing headers generated from your game files', 'seconds', step_headers, None),
    ('translate', 'Translating the game\'s ARM64 code to C', 'about 45 minutes', step_translate,
     lambda c: fingerprint([c.startup / f for f in TRANSLATOR_INPUTS], extra='capstone ' + pins.PY_PACKAGES['capstone'])),
    ('shaders', 'Translating the game\'s shaders to HLSL and compiling them to DXIL', '30-90 minutes', step_shaders,
     lambda c: fingerprint([c.startup / f for f in SHADER_INPUTS] + [HERE / 'runtime_shaders_parallel.py'],
                           extra=pins.ROMFS['image_sha256'])),
    ('compile', 'Compiling and linking the game DLL (the longest step)', 'about 1.5-3 hours', step_compile,
     lambda c: fingerprint(sorted(c.startup.glob('*.h')) + [c.startup / f for f in RUNTIME_C + COMPILE_INPUTS],
                           extra=stat_key(c.startup / 'translated_startup.c'))),   # the 3.7 GB C file: size+time only
    ('launcher', 'Building the launcher (totk.exe)', 'under a minute', step_launcher,
     lambda c: fingerprint(sorted(c.startup.glob('*.h')) + [c.startup / 'totk_launcher.c'])),
    ('play', 'Writing the start script', 'seconds', step_play, None),
]
TRANSLATOR_INPUTS = ['translate_startup.py', 'float_emit.py', 'float_emit_ext.py', 'simd_mem_ext.py', 'simd_arith_ext.py',
                     'instruction_cache.py', 'address_arbiter_emit.py', 'sleep_emit.py', 'partition_native.py', 'entry_points.py',
                     'service_profile.py', 'incremental/translate_startup_incremental.py', 'incremental/ts_bridge.py',
                     'discovered-main-ranges.json', 'discovered-sdk-ranges.json', 'discovered-subsdk0-ranges.json',
                     'discovered-rtld-ranges.json', 'discovered-entries.json', 'entry-points-static.json', 'script-entries.json']
SHADER_INPUTS = ['maxwell_hlsl.py', 'maxwell_isa.py', 'maxwell_runtime.hlsli', 'translate_game_shaders.py', 'translate_runtime_shaders.py',
                 'inventory_game_shaders.py', 'shader_package_reader.py', 'bnsh_reader.py', 'extract_shader_pairs.py',
                 'extract_shader_headers.py', 'write_shader_manifest.py', 'debug/effect_shader_scan.py', 'shader-tools/extract_early_z.py',
                 'clear-shaders/clear.hlsl', 'diag-shaders/const.ps.hlsl', 'diag-shaders/varying3.ps.hlsl']
RUNTIME_C = ['host_input_sdl.c', 'host_movie_vpx.c', 'host_movie_webm.cpp']   # never glob *.c: translated_startup.c is huge
COMPILE_INPUTS = ['incremental/compile_cached_fast.py', 'compile_cached.py', 'split_page_header.py', 'split_host_window.py',
                  'split_runtime.py', 'third_party/build_opus.py', 'third_party/build_sdl.py', 'third_party/build_vpx.py',
                  'third_party/build_webm.py']
# Cheap steps that always run (they skip their own work when it is done; later steps need what they find)
ALWAYS = ('check-pc', 'sources', 'toolchain')
# Steps whose outputs must exist for a skip to be trusted
OUTPUTS = {'translate': ['translated_startup.c'], 'shaders': ['shader-manifest.tsv'], 'compile': ['totk-play.dll'], 'launcher': ['totk.exe']}


def stat_key(p):
    try: s = p.stat(); return f'{s.st_size}:{s.st_mtime_ns}'
    except OSError: return 'missing'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--exefs', required=True, help='folder with main, main.npdm, rtld, sdk, subsdk0 from your dump')
    ap.add_argument('--romfs', required=True, help='your extracted RomFS folder, or a RomFS image file')
    ap.add_argument('--out', default=str(REPO / 'output'), help='output folder (default: <repo>/output)')
    ap.add_argument('--workers', type=int, default=0, help='parallel jobs (default: half the CPU threads, at most 8)')
    g = ap.add_mutually_exclusive_group()
    g.add_argument('--from', dest='from_step', choices=[s[0] for s in STEPS], help='redo this step and every step after it')
    g.add_argument('--only', choices=[s[0] for s in STEPS], help='run just this step (and the PC check)')
    ap.add_argument('--check-inputs', action='store_true', help='only check the PC and your ExeFS/RomFS (nothing is written but logs)')
    ap.add_argument('--allow-romfs-mismatch', action='store_true', help=argparse.SUPPRESS)
    ap.add_argument('--dry-run', action='store_true', help='show the plan and stop')
    a = ap.parse_args()
    c = Ctx(a)
    say(f'totk-test builder - {pins.GAME["name"]} v{pins.GAME["version"]}\n  ExeFS:  {a.exefs}\n  RomFS:  {a.romfs}\n  Output: {c.out}\n')
    if a.check_inputs:
        for title, fn in (('Checking your PC', step_check_pc), ('Checking your ExeFS files', lambda c: check_exefs(c) and say('    ExeFS: all five files match')),
                          ('Checking your RomFS', lambda c: check_romfs(c, write=False))):
            say(f'== {title}'); fn(c)
        say('\nYour game files are the right version. Run the same command without -CheckInputs to build.'); return 0
    names = [s[0] for s in STEPS]
    first = names.index(a.from_step) if a.from_step else 0
    if a.dry_run:
        say('Plan:')
        for i, (name, title, est, fn, fp) in enumerate(STEPS, 1):
            mark = 'skip' if (a.only and name not in (a.only,) + ALWAYS) or (i - 1 < first and name not in ALWAYS) else 'run '
            say(f'  {i:2d}. [{mark}] {title} ({est})  [{name}]')
        say('\nNothing was run (--dry-run).'); return 0
    t_all = time.monotonic()
    for i, (name, title, est, fn, fp) in enumerate(STEPS, 1):
        if a.only and name not in (a.only,) + ALWAYS: continue
        forced = a.from_step and i - 1 >= first or a.only == name
        if not forced and i - 1 < first and name not in ALWAYS: continue
        say(f'\n== Step {i} of {len(STEPS)}: {title}  ({est})')
        key = fp(c) if fp and name not in ('check-pc',) and (c.startup / 'translate_startup.py').is_file() else None
        done = c.state.get('steps', {}).get(name)
        if key and not forced and done == key and all((c.startup / o).is_file() for o in OUTPUTS.get(name, [])):
            say('    already done with the same inputs: skipped'); continue
        t = time.monotonic()
        fn(c)
        if fp and (c.startup / 'translate_startup.py').is_file():
            c.state.setdefault('steps', {})[name] = fp(c); c.save()
        say(f'    done in {minutes(time.monotonic() - t)}')
    say(f'\nBuild finished in {minutes(time.monotonic() - t_all)}.\nTo play: double-click {c.out / "play.cmd"}  (see README.md)')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except BuildError as e:
        say(f'\nSTOPPED: {e}\n\nNothing is lost: fix the problem and run the same command again; finished steps are skipped.')
        sys.exit(1)
    except KeyboardInterrupt:
        say('\nStopped by Ctrl+C. Run the same command again to continue.')
        sys.exit(130)
