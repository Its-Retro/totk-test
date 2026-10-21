"""Write the runtime headers that hold data read from the user's own dumped game files.

These headers are never committed (they contain text and table bytes from Nintendo's binaries); the builder
writes them from the user's decompressed modules after their hashes have been checked:

  host_nvn_names.h            every NVN entry point the game resolves by name: the nvn* identifier strings in
                              main's read-only segment, plus the NVX interop names subsdk0 asks for. Sorted;
                              the index is the trace record id. `used` is not computed here (the runtime never
                              reads it) and is 0 for every name.
  generated_nvn_format_tic.h  boot_nvn_format_tic[161]: word 8 (+0x20) of each 48-byte entry of the NVN driver's
                              per-format table (entries from sdk+0xd015fc, so format f's word 8 is at
                              sdk+0xd01614 + 48 * f, the address the runtime comments quote).
  generated_nvn_format_rt.h   boot_nvn_rt_formats[161] (byte +0x18 of the same entries: Maxwell render-target
                              format; 0 for the depth formats 49..54, whose entry holds a zeta format that
                              boot_nvn_zt_format handles) and boot_nvn_format_blocks[161] (bytes +4/+5:
                              block width << 4 | block height).
  generated_nvn_format_vertex.h  boot_nvn_vertex_words[161]: the 32-bit word at +0x10 of the same entries (the
                              vertex attribute format: size code in bits 21-26, numeric type in bits 27-29),
                              with the driver's placeholder 0x26800000 (no vertex encoding) written as 0.
                              Included by host_nvn_exec.h (batch-3 K3, TOTK_VERTEX_FORMATS).
  generated_gpu_macros.h     a stub with no GPU macro programs (written when missing, or with --macro-stub).
                              The real table comes from a run-time GPU capture; the native NVN play path
                              (TOTK_NVN=native) does not decode command buffers, so it does not need it.

usage: gen_game_headers.py [--root DIR] [--out DIR] [--macro-stub]
  --root  folder holding uncompressed/ (default: the parent of this script's folder)
  --out   folder to write the headers to (default: this script's folder)
"""
import argparse, hashlib, re, struct, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
FORMAT_TABLE = 0xd015fc      # sdk module offset of entry 0 of the driver's per-format table
FORMAT_ENTRY = 48
FORMAT_COUNT = 161
DEPTH_FORMATS = range(49, 55)   # their +0x18 byte is a zeta format, not a render-target format
VERTEX_NONE = 0x26800000        # the +0x10 word of formats with no vertex encoding (depth, compressed, ...)
# Names the runtime must know although main does not ask for them by name (subsdk0 NVX interop and the
# entry points the bootstrap path resolves itself).
EXTRA_NAMES = ('nvnDeviceGetProcAddress', 'nvnDeviceInitialize', 'nvnTextureGetNvRmSurfaceNVX',
               'nvnSyncGetNvRmSyncNVX', 'nvnSyncInitializeFromNvRmSyncNVX', 'nvnSyncFinalize')
# nvn* strings in main's read-only segment that the proc loader never asks for: nvnBootstrapLoader is imported
# from sdk (not looked up by name) and nvnPrivate* are not entry points. Excluding them gives exactly the table
# the development tree built from the loader's GOT slots (same names, same indices).
NOT_LOOKED_UP = ('nvnBootstrapLoader',)
NOT_LOOKED_UP_PREFIXES = ('nvnPrivate',)
NAME_RE = re.compile(rb'(?<![\x21-\x7e])nvn[A-Z][A-Za-z0-9]{2,100}(?=\x00)')


def pinned_hashes():
    """The decompressed-module SHA-256 pins, read from translate_startup.py (one source of truth)."""
    text = (HERE / 'translate_startup.py').read_text(encoding='utf-8')
    pins = dict(re.findall(r"^(MAIN_EXPECTED|EXPECTED|SDK_EXPECTED|SUBSDK_EXPECTED)='([0-9a-f]{64})'", text, re.M))
    return {'main': pins['MAIN_EXPECTED'], 'rtld': pins['EXPECTED'], 'sdk': pins['SDK_EXPECTED'], 'subsdk0': pins['SUBSDK_EXPECTED']}


def load(root, name):
    data = (root / 'uncompressed' / name).read_bytes()
    want = pinned_hashes()[name]
    if hashlib.sha256(data).hexdigest() != want:
        raise SystemExit(f'gen_game_headers: uncompressed/{name} is not the pinned base-game module (expected sha256 {want})')
    if data[:4] != b'NSO0' or struct.unpack_from('<I', data, 12)[0] & 7:
        raise SystemExit(f'gen_game_headers: uncompressed/{name} is not a decompressed NSO')
    return data


def segments(data):
    """[(file offset, module offset, size)] for text, rodata, data."""
    return [struct.unpack_from('<III', data, 0x10 + 0x10 * k) for k in range(3)]


def module_bytes(data, offset, size):
    for file_off, mem_off, seg_size in segments(data):
        if mem_off <= offset and offset + size <= mem_off + seg_size:
            return data[file_off + offset - mem_off:file_off + offset - mem_off + size]
    raise SystemExit(f'gen_game_headers: module range {offset:#x}+{size:#x} is outside every segment')


def nvn_names(main):
    file_off, _, size = segments(main)[1]          # read-only segment
    found = {m.group(0).decode('ascii') for m in NAME_RE.finditer(main, file_off, file_off + size)}
    found = {n for n in found if n not in NOT_LOOKED_UP and not n.startswith(NOT_LOOKED_UP_PREFIXES)}
    return sorted(found | set(EXTRA_NAMES))


def names_header(names):
    out = ['#ifndef TOTK_HOST_NVN_NAMES_H', '#define TOTK_HOST_NVN_NAMES_H',
           '/* GENERATED by startup/gen_game_headers.py from the dumped main (nvn* entry-point names in its read-only',
           ' * segment) plus the NVX interop names: every NVN entry point the game resolves by name, sorted.',
           ' * Index = position (trace record ids). `used` is not computed (the runtime does not read it). Do not commit. */',
           f'#define BOOT_NVN_NAME_COUNT {len(names)}',
           'static const struct { const char *name; unsigned char used; } boot_nvn_names[BOOT_NVN_NAME_COUNT] = {']
    out += [f'    {{"{n}", 0}},' for n in names]
    out += ['};', '#endif', '']
    return '\n'.join(out)


def format_tables(sdk):
    table = module_bytes(sdk, FORMAT_TABLE, FORMAT_ENTRY * FORMAT_COUNT)
    tic, rt, blocks, vertex = [], [], [], []
    for f in range(FORMAT_COUNT):
        e = table[FORMAT_ENTRY * f:FORMAT_ENTRY * (f + 1)]
        tic.append(struct.unpack_from('<I', e, 0x20)[0])
        rt.append(0 if f in DEPTH_FORMATS else e[0x18])
        blocks.append((e[4] << 4) | e[5])
        w = struct.unpack_from('<I', e, 0x10)[0]
        vertex.append(0 if w == VERTEX_NONE else w)
    return tic, rt, blocks, vertex


def rows(values, fmt, per_line):
    return ['    ' + ', '.join(fmt % v for v in values[i:i + per_line]) + ',' for i in range(0, len(values), per_line)]


def tic_header(tic):
    out = ['#ifndef TOTK_GENERATED_NVN_FORMAT_TIC_H', '#define TOTK_GENERATED_NVN_FORMAT_TIC_H',
           '/* GENERATED by startup/gen_game_headers.py from the dumped sdk (NVN driver per-format table: word 8 of',
           ' * each 48-byte entry, at sdk+0xd01614 + 48 * format). Included by host_nvn.h. Do not commit. */',
           f'static const uint32_t boot_nvn_format_tic[{FORMAT_COUNT}] = {{']
    out += rows(tic, '0x%08xu', 8)
    out += ['};', '#endif', '']
    return '\n'.join(out)


def rt_header(rt, blocks):
    out = ['#ifndef TOTK_GENERATED_NVN_FORMAT_RT_H', '#define TOTK_GENERATED_NVN_FORMAT_RT_H',
           '/* GENERATED by startup/gen_game_headers.py from the dumped sdk (NVN driver per-format table, entries',
           ' * from sdk+0xd015fc: byte +0x18 render-target format, 0 for depth formats 49..54; bytes +4/+5 block',
           ' * width/height). Included by host_nvn_exec.h after BOOT_NVN_FORMATS. Do not commit. */',
           'static const uint8_t boot_nvn_rt_formats[BOOT_NVN_FORMATS] = {']
    out += rows(rt, '0x%02x', 16)
    out += ['};', 'static const uint8_t boot_nvn_format_blocks[BOOT_NVN_FORMATS] = {   /* block width << 4 | block height */']
    out += rows(blocks, '0x%02x', 16)
    out += ['};', '#endif', '']
    return '\n'.join(out)


def vertex_header(vertex):
    out = ['#ifndef TOTK_GENERATED_NVN_FORMAT_VERTEX_H', '#define TOTK_GENERATED_NVN_FORMAT_VERTEX_H',
           '/* GENERATED by startup/gen_game_headers.py from the dumped sdk (NVN driver per-format table, entries',
           f' * from sdk+0xd015fc: the word at +0x10, vertex attribute format; 0x{VERTEX_NONE:08x} written as 0).',
           ' * Included by host_nvn_exec.h. Do not commit. */',
           f'static const uint32_t boot_nvn_vertex_words[{FORMAT_COUNT}] = {{']
    out += rows(vertex, '0x%08xu', 8)
    out += ['};', '#endif', '']
    return '\n'.join(out)


MACRO_STUB = '''#ifndef TOTK_GENERATED_GPU_MACROS_H
#define TOTK_GENERATED_GPU_MACROS_H
/* GENERATED by startup/gen_game_headers.py: a table with no GPU macro programs. The real table is made by
 * translate_gpu_macros.py from a run-time capture of the game's GPU command stream, which a release build
 * does not have. The native NVN play path (TOTK_NVN=native) never decodes command buffers, so it never looks
 * a macro up; the legacy nvdrv path would stop with stage error 7 (no translated program) at the first macro.
 * The single entry has count 0, which boot_gpu_stage_macro always skips. */
#include "host_gpu_macro.h"
static int boot_gpu_macro_none(BootGpuMacroIo*io){io->error=7;return 0;}
static const uint32_t boot_gpu_macro_words_none[1]={0};
typedef struct BootGpuMacroProgram{const uint32_t*words;uint32_t count;int(*run)(BootGpuMacroIo*);}BootGpuMacroProgram;
static const BootGpuMacroProgram boot_gpu_macro_programs[]={{boot_gpu_macro_words_none,0,boot_gpu_macro_none}};
#endif
'''


def write(path, text):
    data = text.encode('ascii')
    if path.exists() and path.read_bytes() == data:
        return False
    path.write_bytes(data)
    return True


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--root', type=Path, default=HERE.parent)
    p.add_argument('--out', type=Path, default=HERE)
    p.add_argument('--macro-stub', action='store_true', help='overwrite generated_gpu_macros.h with the stub')
    a = p.parse_args()
    main_image, sdk = load(a.root, 'main'), load(a.root, 'sdk')
    names = nvn_names(main_image)
    tic, rt, blocks, vertex = format_tables(sdk)
    if tic[0] != 0 or not any(rt) or blocks.count(0x11) < 100 or vertex[0] != 0 or sum(1 for w in vertex if w) < 50:
        raise SystemExit('gen_game_headers: the sdk format table does not look as expected; refusing to write headers')
    a.out.mkdir(parents=True, exist_ok=True)
    written = []
    for name, text in (('host_nvn_names.h', names_header(names)), ('generated_nvn_format_tic.h', tic_header(tic)),
                       ('generated_nvn_format_rt.h', rt_header(rt, blocks)),
                       ('generated_nvn_format_vertex.h', vertex_header(vertex))):
        if write(a.out / name, text): written.append(name)
    macros = a.out / 'generated_gpu_macros.h'
    if a.macro_stub or not macros.exists():
        if write(macros, MACRO_STUB): written.append(macros.name)
    print(f'gen_game_headers: {len(names)} NVN names, {FORMAT_COUNT} formats; wrote {", ".join(written) or "nothing (up to date)"}')


if __name__ == '__main__':
    sys.exit(main())
