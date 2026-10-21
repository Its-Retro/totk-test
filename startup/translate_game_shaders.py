"""Translate every game shader program to HLSL and compile it, ahead of time.

For each unique program in game-shader-inventory.json: maxwell_hlsl.translate()
-> complete HLSL (shader-hlsl/<id>.hlsl) -> FXC (D3DCompile, SM 5.1) object
(shader-hlsl/<id>.<vs|ps>.cso). Writes shader-translation-report.json with the
exact reason for every program that is not yet translated. Usage:
  translate_game_shaders.py [limit] [--no-compile] [--only <id-prefix>]
"""
import collections,ctypes as C,json,multiprocessing as mp,os,re,sys,time,traceback
from pathlib import Path
ROOT=Path(__file__).resolve().parent
OUT=ROOT/'shader-hlsl'

_PAIRS = None
def ps_vs_mask(ps_id16):
    global _PAIRS
    if _PAIRS is None:
        path = ROOT / 'shader-pairs.json'
        _PAIRS = {p: m for _, p, m in json.loads(path.read_text())} if path.exists() else {}
    return _PAIRS.get(ps_id16)

SMEM_HELPERS = """groupshared uint smem_[SMEM_WORDS];
uint smem_load(uint a) { return (a >> 2) < SMEM_WORDS ? smem_[a >> 2] : 0u; }
void smem_store(uint a, uint v) { if ((a >> 2) < SMEM_WORDS) smem_[a >> 2] = v; }
void smem_store_bits(uint a, uint v, uint mask) {
    if ((a >> 2) >= SMEM_WORDS) return;
    uint sh = (a & 3u) * 8u, o;
    InterlockedAnd(smem_[a >> 2], ~(mask << sh), o); InterlockedOr(smem_[a >> 2], (v & mask) << sh, o);
}
#define SMEM_ATOMIC(name, fn) uint smem_atomic_##name(uint a, uint v) { uint o = 0u; if ((a >> 2) < SMEM_WORDS) fn(smem_[a >> 2], v, o); return o; }
SMEM_ATOMIC(add, InterlockedAdd) SMEM_ATOMIC(min, InterlockedMin) SMEM_ATOMIC(max, InterlockedMax)
SMEM_ATOMIC(and, InterlockedAnd) SMEM_ATOMIC(or, InterlockedOr) SMEM_ATOMIC(xor, InterlockedXor) SMEM_ATOMIC(exch, InterlockedExchange)
uint smem_cas(uint a, uint cmp, uint v) { uint o = 0u; if ((a >> 2) < SMEM_WORDS) InterlockedCompareExchange(smem_[a >> 2], cmp, v, o); return o; }
// read-modify-write forms without a native groupshared atomic: compare-exchange loops
#define SMEM_RMW(name, expr) uint smem_atomic_##name(uint a, uint v) { if ((a >> 2) >= SMEM_WORDS) return 0u; uint i = a >> 2, old_ = smem_[i], seen_; \\
    [loop] for (uint k_ = 0u; k_ < 65536u; ++k_) { uint nv = (expr); InterlockedCompareExchange(smem_[i], old_, nv, seen_); if (seen_ == old_) break; old_ = seen_; } return old_; }
SMEM_RMW(min_s, (uint)min((int)old_, (int)v)) SMEM_RMW(max_s, (uint)max((int)old_, (int)v))
SMEM_RMW(inc, old_ >= v ? 0u : old_ + 1u) SMEM_RMW(dec, (old_ == 0u || old_ > v) ? v : old_ - 1u)
"""

# Early depth/stencil (nxport, MIT: crates/build/src/shader_cache_build.rs early_tests_flag, metal/msl_specialize.rs
# early_fragment_tests). A fragment stage whose GLSLC control section has bit 0 of byte +0x718 set asks for early
# fragment tests; the NVN driver then sets Maxwell's EarlyZForce: the depth/stencil test AND write run before the
# shader, so an occluded fragment never runs and never stores to memory, and a fragment the shader kills has already
# written depth/stencil (GL early_fragment_tests). D3D12 [earlydepthstencil] (FORCE_EARLY_DEPTH_STENCIL) is the same
# rule; without it D3D12 must test after any pixel shader that may write a UAV or depth. The bit is in neither the SPH
# nor the code: shader-early-z.tsv (canonical id, bit; 'm' = differs between occurrences, treated as not set) is
# extracted from the BNSH and AGL .sharcb control sections by shader-tools/extract_early_z.py.
# TOTK_SHADER_EARLY_Z:
#   off   never (the translator's output before this change).
#   uav   (default) requested, stores to guest memory (GMEM_WRITABLE), no kill: the only shaders whose results change
#         (occluded fragments stop storing, as on the Switch); a shader without UAV writes is already culled early by
#         the driver. The default is the recommended mode so that a later retranslate_shaders.py run without the
#         variable does not silently strip [earlydepthstencil] again; set off for the A/B baseline.
#   exact every requested stage, kill included (the source GPU's behaviour; changes depth/stencil and occlusion counts
#         for killed fragments, so A/B it before use).
# Never on a stage that writes depth (D3D12 ignores the write once the test has run; none of the requested stages does)
# or the sample mask.
EARLY_Z_MODE = os.environ.get('TOTK_SHADER_EARLY_Z', 'uav')
if EARLY_Z_MODE not in ('off', 'uav', 'exact'): raise ValueError(f'TOTK_SHADER_EARLY_Z={EARLY_Z_MODE!r}: off, uav or exact')
_EARLY_Z = None
def early_z_requested(cid):
    global _EARLY_Z
    if _EARLY_Z is None:
        path = Path(os.environ.get('TOTK_SHADER_EARLY_Z_TABLE', str(ROOT / 'shader-early-z.tsv')))
        # A missing table must not quietly turn early tests off (a rerun would then strip them): fail the row instead,
        # retranslate_shaders.py reports it 'fail-translate' and leaves it as it is.
        if not path.exists(): raise FileNotFoundError(f'early-Z table {path} (TOTK_SHADER_EARLY_Z={EARLY_Z_MODE})')
        _EARLY_Z = {}
        for line in path.read_text().splitlines():
            f = line.split('\t')
            if len(f) >= 2: _EARLY_Z[f[0]] = f[1] == '1' and _EARLY_Z.get(f[0], True)
    return _EARLY_Z.get(cid, False)

def assemble(em, prog, body, ps_mask=None):
    st = prog.stage; L = []
    if st == 'compute': return assemble_compute(em, prog, body)
    L.append('#define STAGE_FRAGMENT' if st == 'fragment' else '#define STAGE_VERTEX')
    gmem_writable = 'gmem' in em.uses and any('gmem_store' in l or 'gmem_atomic' in l for _, b in body for l in b)
    if gmem_writable:
        L.append('#define GMEM_WRITABLE')
    L.append('#define KILLED ' + ('killed_' if 'kill' in em.uses else 'false'))
    L.append('#include "maxwell_runtime.hlsli"')
    if 'lmem' in em.uses:
        n = max(prog.local_bytes // 4, 1)
        L += [f'static uint lmem_[{n}];',
              f'uint lmem_load(uint o) {{ return (o >> 2) < {n}u ? lmem_[o >> 2] : 0u; }}',
              f'void lmem_store(uint o, uint v) {{ if ((o >> 2) < {n}u) lmem_[o >> 2] = v; }}']
    if st == 'vertex':
        # Position invariance (nxport, MIT: metal/msl_specialize.rs, [[invariant]] gl_Position): TOTK draws alpha-tested
        # geometry in a depth-only pre-pass and then a colour pass with an EQUAL depth test, through different vertex
        # programs; the same position math rounded differently left 16x16 tiles without colour. 'precise' makes every
        # operation that feeds the position (back through registers, bit casts and the dispatch loop's phis) IEEE-exact:
        # no contraction into FMA, no reassociation, by DXC and by the driver. -Gis already does that for the whole
        # program; this keeps the position exact if a stage is ever compiled without it.
        L.append('struct VsOut {'); L.append('    precise float4 pos : SV_Position;')
        # Declared outputs follow the SPH output map (what the runtime pairs on), plus anything written.
        omap = {g for g in range(32) if (prog.sph[0x36 + g // 2] >> ((g & 1) * 4)) & 15}
        em.vs_declared = sorted(omap | set(em.vs_outputs))
        for g in em.vs_declared: L.append(f'    float4 g{g} : GENERIC{g};')
        for k in range(2):
            if f'clip{k}' in em.uses: L.append(f'    float4 clip{k} : SV_ClipDistance{k};')
        if 'layer' in em.uses: L.append('    uint layer : SV_RenderTargetArrayIndex;')
        if 'viewport' in em.uses: L.append('    uint viewport : SV_ViewportArrayIndex;')
        if 'psize' in em.uses: L.append('    float psize : PSIZE;')
        L.append('};')
        L.append('VsOut finish_vs(VsOut o) { if (host_depth_minus1 != 0u) o.pos.z = (o.pos.z + o.pos.w) * 0.5f; if (host_flip_y != 0u) o.pos.y = -o.pos.y; return o; }')
        L.append('#define FINISH return finish_vs(o)')
        L.append('VsOut main(uint vid_ : SV_VertexID, uint iid_ : SV_InstanceID) {')
        # Outputs the program never stores read (0, 0, 0, 1) on the source GPU (suyu EmitPrologue stores
        # that default to the position and every declared generic before the program runs).
        L.append('    VsOut o = (VsOut)0; o.pos.w = 1.0f;' + ''.join(f' o.g{g}.w = 1.0f;' for g in em.vs_declared))
        L.append('    uint vid = vid_ + host_vertex_base, iid = iid_ + host_instance_base;')
        for g in sorted(em.vs_inputs): L.append(f'    uint4 va{g} = vin_attr4({g}u, vid, iid);')
    else:
        targets, depth, sample_mask = prog.ps_outputs()
        L.append('struct PsIn {'); L.append('    float4 pos : SV_Position;')
        # The input signature must equal the paired vertex shader's outputs register for register.
        declared = sorted({g for g in range(32) if ps_mask >> g & 1}) if ps_mask is not None else sorted(em.ps_inputs)
        missing = set(em.ps_inputs) - set(declared)
        # Some pixel shaders read generics their paired vertex shader never writes (verified: the
        # VS header and its stores agree). The source reads an undefined value there; D3D12 cannot
        # link an undeclared input, so those reads become zero.
        if missing:
            em.notes.add('unwritten_input_zero')
            for _, lines in body:
                for k, line in enumerate(lines):
                    for g in missing: line = line.replace(f'i.g{g}.', 'float4(0.0f, 0.0f, 0.0f, 0.0f).')
                    lines[k] = line
        flat = []   # render-36: flat (constant) generics, read from the provoking vertex below
        for g in declared:
            modes = set(em.ps_inputs.get(g, {}).values()) - {0}
            if len(modes) > 1: raise_unsupported(f'mixed interpolation in generic {g}')
            mode = modes.pop() if modes else 2
            q = {1: 'nointerpolation ', 2: '', 3: 'noperspective '}[mode]
            L.append(f'    {q}float4 g{g} : GENERIC{g};')
            if mode == 1 and g in em.ps_inputs: flat.append(g)
        # render-36: Maxwell (NVN default, GL convention) takes a flat input from the LAST vertex of the primitive; D3D12
        # takes it from the first. TOTK packs per-vertex material indices into flat generics (the cave rock decal pass,
        # PS 73f9b49a: bitfields of g5.x index the material table), so each triangle read the wrong vertex's material
        # -> white/blue 'camouflage' patches. GetAttributeAtVertex (SM 6.1 barycentrics) reads the vertex explicitly:
        # index 2 = the last vertex in submission order (lists and strips), 0 = the first (host_provoking_first, A/B).
        if flat:
            for _, lines in body:
                for k, line in enumerate(lines):
                    for g in flat: line = line.replace(f'i.g{g}.', f'fl_g{g}.')
                    lines[k] = line
        if 'pointcoord' in em.uses: L.append('    float2 pointcoord : POINTCOORD;')
        if 'front_face' in em.uses: L.append('    bool ff : SV_IsFrontFace;')
        L.append('};')
        L.append('struct PsOut {')
        for rt in range(8):
            if any(targets[rt]): L.append(f'    float4 c{rt} : SV_Target{rt};')
        if depth: L.append('    float depth : SV_Depth;')
        L.append('};')
        fin = ['PsOut po_ = (PsOut)0;']; reg = 0
        for rt in range(8):
            for c in range(4):
                if targets[rt][c]:
                    fin.append(f'po_.c{rt}.{"xyzw"[c]} = asfloat(r{reg});'); em.regs.add(reg); reg += 1
        if sample_mask: reg += 1
        if depth: fin.append(f'po_.depth = asfloat(r{reg + 1 if not sample_mask else reg});'); em.regs.add(reg + 1 if not sample_mask else reg)
        if 'kill' in em.uses: fin.insert(0, 'if (killed_) discard;')
        fin.append('return po_')
        L.append('#define FINISH { ' + ' '.join(fin) + '; }')
        if (EARLY_Z_MODE in ('uav', 'exact') and early_z_requested(prog.canonical_id) and not depth and not sample_mask
                and (EARLY_Z_MODE == 'exact' or (gmem_writable and 'kill' not in em.uses))):
            em.notes.add('early_z'); L.append('[earlydepthstencil]')
        L.append('PsOut main(PsIn i) {')
        L.append('    float rcp_w = 1.0f / i.pos.w;')
        for g in flat:   # render-36
            L.append(f'    float4 fl_g{g} = host_provoking_first != 0u ? GetAttributeAtVertex(i.g{g}, 0) : GetAttributeAtVertex(i.g{g}, 2);')
        if 'front_face' in em.uses: L.append('    uint front_face_bits = i.ff ? 0xffffffffu : 0u;')
    return assemble_tail(em, body, L)

def assemble_compute(em, prog, body):
    """Compute program: [numthreads] from the QMD block size; SR_TID/SR_CTAID from SV ids (plus the
    host's group offset for grids split over D3D12's 65535 limit); shared memory as groupshared."""
    launch = prog.launch; L = []
    bx, by, bz = launch['block']
    L.append('#define STAGE_COMPUTE')
    L.append('#define GMEM_WRITABLE')     # compute reads guest memory through the UAV as well (maxwell_runtime.hlsli)
    L.append('#define KILLED false')
    L.append('#include "maxwell_runtime.hlsli"')
    if 'lmem' in em.uses:
        n = max(prog.local_bytes // 4, 1)
        L += [f'static uint lmem_[{n}];',
              f'uint lmem_load(uint o) {{ return (o >> 2) < {n}u ? lmem_[o >> 2] : 0u; }}',
              f'void lmem_store(uint o, uint v) {{ if ((o >> 2) < {n}u) lmem_[o >> 2] = v; }}']
    if 'smem' in em.uses:
        words = max(launch.get('shared', 0) // 4, 1)
        L.append(f'#define SMEM_WORDS {words}u')
        L.append(SMEM_HELPERS)
    L.append('#define FINISH return')
    if 'wave' in em.uses: L.append('[WaveSize(32)]')   # Maxwell warps are 32 lanes: votes/shuffles/lane masks assume it
    L.append(f'[numthreads({bx}, {by}, {bz})]')
    L.append('void main(uint3 tid_ : SV_GroupThreadID, uint3 gid_ : SV_GroupID) {')
    L.append('    uint3 ctaid_ = gid_ + uint3(host_vertex_base, host_instance_base, host_flip_y);   // HostState words 4-6: group offset')
    return assemble_tail(em, body, L)

def assemble_tail(em, body, L):
    regs = sorted(em.regs)
    for k in range(0, len(regs), 16):
        L.append('    uint ' + ', '.join(f'r{r} = 0u' for r in regs[k:k + 16]) + ';')
    if em.preds: L.append('    bool ' + ', '.join(f'p{p} = false' for p in sorted(em.preds)) + ';')
    if 'cc' in em.uses: L.append('    bool cc_z = false, cc_s = false, cc_c = false, cc_o = false;')
    if 'kill' in em.uses: L.append('    bool killed_ = false;')
    if 'rstack' in em.uses: L.append('    uint ssy_s_[16], pbk_s_[16]; uint ssy_n_ = 0u, pbk_n_ = 0u;   // runtime reconvergence stacks')
    if 'loopguard' in em.uses:   # back edges taken (maxwell_hlsl.loop_guard): cap, then FINISH with the outputs so far
        L.append('#define LOOP_CAP 65536u')
        L.append('    uint loops_ = 0u;')
    if len(body) == 1:
        L += body[0][1]
    else:
        L.append(f'    uint pc_ = 0x{body[0][0]:x}u;')
        if BACKEND == 'fxc':
            # FXC cannot assign a switch selector inside its own switch in a loop (internal
            # error "l-value expected"), so blocks are dispatched by an if-chain.
            L.append('#define NEXT continue')
            L.append('    [loop] for (;;) {')
            for start, lines in body:
                L.append(f'        if (pc_ == 0x{start:x}u) {{')
                L += lines
                L.append('        }')
            L.append('        FINISH;')
            L.append('    }')
        else:
            L.append('#define NEXT break')
            L.append('    [loop] for (;;) {')
            L.append('        switch (pc_) {')
            for start, lines in body:
                L.append(f'        case 0x{start:x}u: {{')
                L += lines
                L.append('        }')
            L.append('        default: FINISH;')
            L.append('        }')
            L.append('    }')
        L.append('    FINISH;   // unreachable; FXC cannot prove the dispatch loop never exits')
    L.append('}')
    return '\n'.join(L) + '\n'

BACKEND = os.environ.get('SHADER_COMPILER', 'dxc')
DXC = ROOT.parent / 'tools' / 'dxc' / 'bin' / 'x64' / 'dxc.exe'

class _U(Exception): pass
def raise_unsupported(msg): raise _U(msg)

def compile_dxc(src, path, stage, timeout=600):
    import subprocess
    kind = {'vertex': 'vs', 'fragment': 'ps', 'compute': 'cs'}[stage]
    out = path.with_suffix('.' + kind + '.dxil')
    cmd = [str(DXC), '-nologo', '-T', kind + '_6_6', '-E', 'main', '-HV', '2021',
           '-O3', '-Gis', '-I', str(ROOT), '-Fo', str(out), str(path)]   # -Gis: IEEE-strict, no fast-math reassociation (it folded away the game's '+1e-10' divide guards -> NaN)
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return 1, b'', f'dxc timeout after {timeout}s'
    obj = out.read_bytes() if r.returncode == 0 and out.exists() else b''
    return r.returncode, obj, r.stderr + r.stdout

_d3d = None
def compile_hlsl(src, path, stage):
    if BACKEND != 'fxc': return compile_dxc(src, path, stage)
    global _d3d
    if _d3d is None:
        _d3d = C.WinDLL('d3dcompiler_47')
        _d3d.D3DCompile.argtypes = [C.c_char_p, C.c_size_t, C.c_char_p, C.c_void_p, C.c_void_p, C.c_char_p, C.c_char_p,
                                    C.c_uint, C.c_uint, C.POINTER(C.c_void_p), C.POINTER(C.c_void_p)]
        _d3d.D3DCompile.restype = C.c_long
    code = C.c_void_p(); err = C.c_void_p()
    data = src.encode()
    flags = (1 << 20) | (1 << 2) | (1 << 21)   # unbounded descriptor tables, skip optimization (coverage pass), resources may alias
    hr = _d3d.D3DCompile(data, len(data), str(path).encode(), None, C.c_void_p(1), b'main',
                         b'vs_5_1' if stage == 'vertex' else b'ps_5_1', flags, 0, C.byref(code), C.byref(err))
    def blob_bytes(b):
        if not b.value: return b''
        vt = C.cast(b, C.POINTER(C.POINTER(C.c_void_p))).contents
        get_ptr = C.WINFUNCTYPE(C.c_void_p, C.c_void_p)(vt[3]); get_size = C.WINFUNCTYPE(C.c_size_t, C.c_void_p)(vt[4])
        release = C.WINFUNCTYPE(C.c_ulong, C.c_void_p)(vt[2])
        out = C.string_at(get_ptr(b), get_size(b)); release(b); return out
    obj, msg = blob_bytes(code), blob_bytes(err)
    return hr, obj, msg.decode(errors='replace')

def work(args):
    entry, do_compile = args
    from maxwell_isa import Unsupported
    import maxwell_hlsl
    stem = entry['sha256'][:16]
    result = {'id': stem, 'stage': entry['stage'], 'refs': entry.get('ref_count', 1)}
    try:
        code = (ROOT / 'shader-programs' / (stem + '.bin')).read_bytes()
        sph = (ROOT / 'shader-programs' / (stem + '.sph')).read_bytes()
        em, prog, body = maxwell_hlsl.translate(code, sph, entry['stage'])
        mask = ps_vs_mask(stem) if entry['stage'] == 'fragment' else None
        src = assemble(em, prog, body, mask)
        result['vs_mask'] = mask
        path = OUT / (stem + '.hlsl'); path.write_text(src)
        result.update(status='translated', canonical_id=prog.canonical_id, notes=sorted(em.notes), uses=sorted(em.uses), blocks=len(body),
                      cbufs=sorted(em.used_cbufs), tex_handles=sorted(em.tex_indices), vs_inputs=sorted(em.vs_inputs),
                      vs_outputs={str(g): sorted(c) for g, c in em.vs_outputs.items()},
                      ps_inputs={str(g): {str(c): m for c, m in v.items()} for g, v in em.ps_inputs.items()},
                      local_bytes=prog.local_bytes)
        if do_compile:
            t = time.perf_counter()
            hr, obj, msg = compile_hlsl(src, path, entry['stage'])
            result['compile_seconds'] = round(time.perf_counter() - t, 3)
            if hr != 0:
                lines = [l for l in msg.strip().splitlines() if 'error' in l] or msg.strip().splitlines()
                result.update(status='compile_error', error=lines[0] if lines else f'hr {hr & 0xffffffff:#x}')
            else:
                if BACKEND == 'fxc': (OUT / f"{stem}.{'vs' if entry['stage'] == 'vertex' else 'ps'}.cso").write_bytes(obj)
                result['status'] = 'compiled'
    except (Unsupported, _U) as e:
        result.update(status='unsupported', reason=str(e))
    except Exception as e:
        result.update(status='translator_error', reason=f'{type(e).__name__}: {e}', trace=traceback.format_exc()[-600:])
    return result

def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    limit = int(args[0]) if args else None
    do_compile = '--no-compile' not in sys.argv
    inv = json.loads((ROOT / 'game-shader-inventory.json').read_text(encoding='utf-8'))
    programs = inv['programs']
    if '--only' in sys.argv:
        prefix = sys.argv[sys.argv.index('--only') + 1]; programs = [p for p in programs if p['sha256'].startswith(prefix)]
    programs = programs[:limit]
    OUT.mkdir(exist_ok=True)
    with mp.Pool(int(os.environ.get("SHADER_WORKERS", max(1, os.cpu_count() // 2)))) as pool:
        results = []
        for n, r in enumerate(pool.imap_unordered(work, [(p, do_compile) for p in programs], chunksize=4)):
            results.append(r)
            if n % 500 == 0: print(f'  {n}/{len(programs)}', flush=True)
    status = collections.Counter(r['status'] for r in results)
    reasons = collections.Counter(r.get('reason') or r.get('error') for r in results if r['status'] != 'compiled' and r['status'] != 'translated')
    weighted = collections.Counter()
    for r in results: weighted[r['status']] += r['refs']
    report = {'scope': 'Ahead-of-time Maxwell->HLSL translation of game shader programs; FXC SM5.1 compile check only, no rendering validation',
              'programs': len(results), 'status': dict(status), 'status_weighted_by_variation_refs': dict(weighted),
              'top_reasons': dict(reasons.most_common(60)), 'results': sorted(results, key=lambda r: r['id'])}
    (ROOT / 'shader-translation-report.json').write_text(json.dumps(report, indent=1) + '\n', encoding='utf-8')
    print(json.dumps({k: report[k] for k in ('programs', 'status', 'status_weighted_by_variation_refs')}, indent=1))
    for k, v in reasons.most_common(40): print(f'{v:6d}  {k}')

if __name__ == '__main__':
    main()
