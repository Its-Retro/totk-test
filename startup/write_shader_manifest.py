"""shader-translation-report.json -> shader-manifest.tsv (runtime shader lookup table).

Columns: canonical id (sha256 of SPH + code through terminator), stage (vs/ps),
paired vertex-output mask (pixel only, hex, '-' otherwise), texture handle
indices read from constant buffer 2 (comma list or '-'), compiled DXIL path.
"""
import json
from pathlib import Path
ROOT = Path(__file__).resolve().parent

def manifest_path(path):
    """DXIL path as written to the manifest: relative to the root folder (the game's working directory, the
    parent of startup/) so a built tree can be moved; absolute only for files outside it."""
    try: return str(Path(path).resolve().relative_to(ROOT.parent.resolve())).replace('/', '\\')
    except ValueError: return str(path)

def main():
    report = json.loads((ROOT / 'shader-translation-report.json').read_text(encoding='utf-8'))
    rows = 0
    with open(ROOT / 'shader-manifest.tsv', 'w', encoding='utf-8') as m:
        for r in report['results']:
            if r['status'] != 'compiled': continue
            ext = 'vs' if r['stage'] == 'vertex' else 'ps'
            path = ROOT / 'shader-hlsl' / f"{r['id']}.{ext}.dxil"
            if not path.exists(): continue
            mask = '-' if ext == 'vs' or r.get('vs_mask') is None else f"{r['vs_mask']:x}"
            tex = ','.join(str(t) for t in r.get('tex_handles', [])) or '-'
            m.write(f"{r['canonical_id']}\t{ext}\t{mask}\t{tex}\t{manifest_path(path)}\n"); rows += 1
    print('manifest rows', rows)

if __name__ == '__main__':
    main()
