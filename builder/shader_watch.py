"""Translate shaders the game meets for the first time, while it runs.

When a draw (or compute dispatch) needs a shader the manifest does not have, the renderer skips it and dumps
the program to <root>/missing-shaders/. This watcher translates new dumps with startup/translate_runtime_shaders.py
(same translator and DXC settings as the build) and appends them to the manifest; the running renderer re-reads
the manifest when it changes, so the effect appears a moment later and stays fixed from then on.
Each player captures their own: the dumps are game code and must not be shared.

usage: shader_watch.py <root> [--pid <game pid>] [--interval seconds]
Stops when the game process exits (with --pid), or with Ctrl+C.
"""
import ctypes, os, subprocess, sys, time
from pathlib import Path

BELOW_NORMAL = 0x4000


def alive(pid):
    if pid is None: return True
    k = ctypes.windll.kernel32
    h = k.OpenProcess(0x1000, False, pid)          # PROCESS_QUERY_LIMITED_INFORMATION
    if not h: return False
    code = ctypes.c_ulong()
    ok = k.GetExitCodeProcess(h, ctypes.byref(code)); k.CloseHandle(h)
    return bool(ok) and code.value == 259          # STILL_ACTIVE


def main(argv):
    root = Path(argv[0]).resolve()
    pid = int(argv[argv.index('--pid') + 1]) if '--pid' in argv else None
    interval = float(argv[argv.index('--interval') + 1]) if '--interval' in argv else 5.0
    dumps, startup = root / 'missing-shaders', root / 'startup'
    log = root / 'logs' / 'shader-watch.log'; log.parent.mkdir(exist_ok=True)
    seen = set()
    while alive(pid):
        files = {f.name for f in dumps.glob('*.bin')} if dumps.is_dir() else set()
        if files - seen:
            with log.open('a', encoding='utf-8') as out:
                out.write(f'=== {time.strftime("%H:%M:%S")} {len(files - seen)} new dump(s)\n'); out.flush()
                subprocess.run([sys.executable, '-B', str(startup / 'translate_runtime_shaders.py'), '--batch', '50'],
                               cwd=startup, stdout=out, stderr=subprocess.STDOUT, creationflags=BELOW_NORMAL)
            seen = files
        time.sleep(interval)


if __name__ == '__main__':
    main(sys.argv[1:])
