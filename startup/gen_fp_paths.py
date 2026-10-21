"""Write boot_fp_paths.h (patch 58) from float_emit.fp_paths_header(), next to this script.

  gen_fp_paths.py           (re)write the header
  gen_fp_paths.py --check   exit 1 if the header is missing or differs from what float_emit.py generates

The cold exact helpers in that header are the exact Arm code the translator used to inline at every FP site,
produced by the same float_emit.py functions; regenerate after editing any of them (the translator emits only
calls, so a stale header would silently mean old semantics)."""
import sys
from pathlib import Path
sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import float_emit
path = HERE / 'boot_fp_paths.h'
text = float_emit.fp_paths_header()
if '--check' in sys.argv:
    ok = path.exists() and path.read_bytes() == text.encode('ascii')
    print('boot_fp_paths.h', 'up to date' if ok else 'STALE: run gen_fp_paths.py')
    sys.exit(0 if ok else 1)
path.write_bytes(text.encode('ascii'))
print(f'wrote {path} ({len(text)} bytes, {len(float_emit.fp_path_forms())} cold helpers)')
