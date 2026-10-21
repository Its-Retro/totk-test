"""Parallel version of `translate_runtime_shaders.py --dir A --dir B ...` for the offline shader dumps.

Same result as the one-process script: every dump file is translated by the same translate_one() with the same
emitter and DXC settings, the first folder holding a program wins, programs already in the manifest are skipped,
and the manifest rows are appended. Differences: the duplicate check happens up front, the work is spread over
worker processes, and the row order in the manifest follows completion (the runtime does not depend on it).

usage: runtime_shaders_parallel.py <startup dir> <workers> <dump dir> [<dump dir> ...]
"""
import multiprocessing as mp
import re, sys, time
from pathlib import Path

KEY_CS = re.compile(r'cs-((\d+)x(\d+)x(\d+)s(\d+)l(\d+))-([0-9a-f]{64})\.bin')
KEY_GFX = re.compile(r'(vs|ps)(?:-([0-9a-f]+))?-([0-9a-f]{64})\.bin')


def key_of(name):
    """The manifest key translate_one() uses for a dump file name, or None (not a dump / not translatable)."""
    c = KEY_CS.fullmatch(name)
    if c: return (c.group(7), 'cs', c.group(1))
    m = KEY_GFX.fullmatch(name)
    if not m: return None
    kind, mask, cid = m.groups()
    if kind == 'ps' and mask is None: return None
    return (cid, kind, '-' if kind == 'vs' else f'{int(mask, 16):x}')


def _init(startup):
    sys.path.insert(0, startup)
    global trs
    import translate_runtime_shaders as trs


def _work(path):
    return trs.translate_one(Path(path), set())


def main(argv):
    startup, workers, dirs = Path(argv[0]).resolve(), int(argv[1]), [Path(d) for d in argv[2:]]
    manifest = startup / 'shader-manifest.tsv'
    known = {tuple(l.split('\t')[:3]) for l in manifest.read_text(encoding='utf-8').splitlines() if l}
    todo, seen = [], set(known)
    for d in dirs:
        for f in sorted(d.glob('*.bin')):
            k = key_of(f.name)
            if k and k not in seen:
                seen.add(k); todo.append(str(f))
    print(f'{len(todo)} programs to translate with {workers} workers ({len(known)} manifest rows already)', flush=True)
    (startup / 'shader-hlsl').mkdir(exist_ok=True)
    added = 0; pending = []; last = time.time()
    def flush():
        nonlocal pending, last
        if pending:
            with manifest.open('a', encoding='utf-8') as out: out.write(''.join(pending))
        pending = []; last = time.time()
    with mp.Pool(workers, initializer=_init, initargs=(str(startup),)) as pool:
        for n, line in enumerate(pool.imap_unordered(_work, todo, chunksize=8)):
            if line:
                pending.append(line); added += 1
                if len(pending) >= 200 or time.time() - last > 180: flush()
            if n % 500 == 0: print(f'  {n}/{len(todo)} done, {added} added', flush=True)
    flush()
    print('runtime shaders added', added, flush=True)


if __name__ == '__main__':
    main(sys.argv[1:])
