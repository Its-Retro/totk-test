"""Maxwell shader program -> HLSL source, ahead of time.

Input: one stage's code (after the NVN wrapper) and its 0x50-byte SPH.
Output: HLSL using the runtime ABI in maxwell_runtime.hlsli. Every
instruction form is translated explicitly; anything unknown raises
Unsupported so the program is reported, never approximated silently.
Known approximations are listed in APPROXIMATIONS and recorded per program.
"""
import os, struct
from maxwell_isa import decode, Unsupported, bits

APPROXIMATIONS = {
    'rounding': 'rm/rp/rz rounding on fp32 arithmetic and i2f uses round-to-nearest',
    'mufu': 'mufu transcendental precision follows the host GPU',
    'warp': 'warp vote/shuffle use wave intrinsics (host wave size may differ from 32)',
    'half_fma': 'HFMA2 on fp16 sources is evaluated as fp32 fma then rounded to fp16 (may differ in the last fp16 bit)',
    'unwritten_input_zero': 'pixel input the paired vertex shader never writes reads 0 (undefined on the source GPU)',
}

# batch-4 translator fixes, each with a switch read once here (default on; NAME=0 translates exactly as before). They act
# when shaders are retranslated (retranslate_shaders.py), so set them for that run, not for the game.
def _switch(name): return os.environ.get(name, '1') != '0'
TMML_HW_ORDER = _switch('TOTK_SHADER_TMML_HW')     # shd-S2: TMML result order of the source GPU (op_tmml)
IADD3_RS_CARRY = _switch('TOTK_SHADER_IADD3_CARRY')   # K4: IADD3.RS shifts the 33-bit sum (op_iadd3)
ATOM_KILL_GUARD = _switch('TOTK_SHADER_ATOM_KILL')  # L3: ATOM after a deferred KIL does not reach memory (op_atom)
TEX_STRICT = _switch('TOTK_SHADER_TEX_STRICT')      # L1/L2: unverified TEXS/TLDS/TLD4S forms are unsupported

def f32_literal(bits32):
    return f'asfloat(0x{bits32 & 0xffffffff:08x}u)'

class Program:
    def __init__(self, code, sph, stage):
        self.code, self.sph, self.stage = code, sph, stage
        self.words = [w for (w,) in struct.iter_unpack('<Q', code)]
        self.insns = []
        for i, w in enumerate(self.words):
            addr = i * 8
            if addr % 32 == 0: continue                      # scheduling word
            ins = decode(w, addr)
            if ins.name == 'bra' and ins.f['target'] == addr and not ins.f['cc'] and ins.guard == (7, False):
                self.end = addr + 8                          # trailing self-branch terminator
                break
            self.insns.append(ins)
        else:
            raise Unsupported('program has no terminator')
        import hashlib
        if stage == 'compute':
            # Compute programs have no SPH; launch parameters come from the QMD (block size, shared
            # and local memory), passed in as `sph` = dict by the caller (translate_runtime_shaders.py).
            self.launch = dict(sph or {})
            self.sph = b''; self.sph_words = (0,) * 20
            self.canonical_id = hashlib.sha256(code[:self.end]).hexdigest()
            self.local_bytes = self.launch.get('local', 0)
            return
        self.sph_words = struct.unpack('<20I', sph)
        # Runtime identity: what the host can hash from guest GPU memory at draw time.
        self.canonical_id = hashlib.sha256(sph + code[:self.end]).hexdigest()
        self.local_bytes = self.sph_words[1] & 0xffffff

    # -------------------------------------------------------------- header
    def ps_outputs(self):
        target = self.sph_words[18]; flags = self.sph_words[19]
        return [[bool(target >> (rt * 4 + c) & 1) for c in range(4)] for rt in range(8)], bool(flags & 2), bool(flags & 1)

    def ps_input_modes(self):
        """Per generic attribute, per component: 0 unused, 1 constant, 2 perspective, 3 screen-linear."""
        modes = []
        for i in range(32):
            byte = self.sph[0x18 + i]
            modes.append([(byte >> (c * 2)) & 3 for c in range(4)])
        return modes


class Emitter:
    def __init__(self, prog):
        self.prog = prog
        self.out = []
        self.regs = set(); self.preds = set()
        self.used_cbufs = set(); self.notes = set()
        self.uses = set()          # features: tex, gmem, lmem, cc, fp64, wave, kill
        self.vs_inputs = set(); self.vs_outputs = {}; self.ps_inputs = {}
        self.tex_indices = set()
        self.indent = 1

    # -------------------------------------------------------------- operands
    def emit(self, line): self.out.append('    ' * self.indent + line)
    def r(self, n):
        if n == 255: return '0u'
        self.regs.add(n); return f'r{n}'
    def rf(self, n): return '0.0f' if n == 255 else f'asfloat({self.r(n)})'
    def setr(self, n, expr):
        if n != 255: self.regs.add(n); self.emit(f'r{n} = {expr};')
    def setrf(self, n, expr): self.setr(n, f'asuint({expr})')
    def p(self, n):
        if n == 7: return 'true'
        self.preds.add(n); return f'p{n}'
    def pn(self, pv):
        n, neg = pv; e = self.p(n)
        return f'(!{e})' if neg else e
    def setp(self, n, expr):
        if n != 7: self.preds.add(n); self.emit(f'p{n} = {expr};')

    def cbuf(self, mem, extra_word=0):
        space, bank, reg, off = mem
        off += extra_word * 4
        self.used_cbufs.add(bank)
        if reg == 255:
            if off < 0 or off % 4: raise Unsupported('unaligned constant buffer offset')
            return f'cb{bank}[{off >> 4}].{"xyzw"[(off >> 2) & 3]}'
        self.uses.add('cbdyn')
        return f'cbuf_load{bank}({self.r(reg)} + {off & 0xffffffff}u)'

    def src(self, ins, key):
        """Integer (raw 32-bit) value of an operand B/C as an HLSL uint expression."""
        v = ins.f[key]
        if isinstance(v, tuple): return self.cbuf(v)
        a = self._atom_of(ins, key)
        if a == 'reg': return self.r(v)
        return f'0x{v & 0xffffffff:x}u'

    def _atom_of(self, ins, key):
        from maxwell_isa import ROOT, R, I, M
        for value, mask, name, atoms in ROOT:
            if value == ins.opcode:
                for a in _flatten(atoms, ins.word):
                    if getattr(a, 'key', None) == key:
                        return 'reg' if isinstance(a, R) else 'mem' if isinstance(a, M) else 'imm'
        raise Unsupported(f'no operand {key}')

    def fsrc(self, ins, key, neg=None, absk=None):
        v = ins.f[key]
        kind = 'mem' if isinstance(v, tuple) else self._atom_of(ins, key)
        if kind == 'imm': e = f32_literal(v >> 32 if v > 0xffffffff else v)
        else: e = f'asfloat({self.src(ins, key)})'
        if absk and ins.f.get(absk): e = f'abs({e})'
        if neg and ins.f.get(neg): e = f'(-{e})'
        return e

    def fa(self, ins, neg='neg_a', absk='abs_a'):
        e = self.rf(ins.f['a'])
        if absk and ins.f.get(absk): e = f'abs({e})'
        if neg and ins.f.get(neg): e = f'(-{e})'
        return e

    def fres(self, ins, expr, d='d'):
        if ins.f.get('sat'): expr = f'saturate({expr})'
        if ins.f.get('cc'):
            self.uses.add('cc'); self.emit(f'{{ float t_ = {expr}; cc_z = t_ == 0.0f; cc_s = t_ < 0.0f; cc_c = false; cc_o = false; {self.r(ins.f[d]) if ins.f[d] != 255 else "t_"}{" = asuint(t_)" if ins.f[d] != 255 else ""}; }}')
            return
        self.setrf(ins.f[d], expr)

    def ires(self, ins, expr, d='d'):
        if ins.f.get('cc'):
            self.uses.add('cc')
            self.emit(f'{{ uint t_ = {expr}; cc_z = t_ == 0u; cc_s = (int)t_ < 0; cc_c = false; cc_o = false;'
                      + (f' r{ins.f[d]} = t_;' if ins.f[d] != 255 else '') + ' }')
            if ins.f[d] != 255: self.regs.add(ins.f[d])
            return
        self.setr(ins.f[d], expr)

    # -------------------------------------------------------------- per op
    def op(self, ins):
        f = ins.f; n = ins.name
        h = getattr(self, 'op_' + n, None)
        if h is None: raise Unsupported(f'no HLSL lowering for {n}')
        if ins.guard != (7, False) and n not in ('ssy', 'pbk'):
            pred, neg = ins.guard
            if pred == 7 and neg: return            # never
            self.emit(f'if ({"!" if neg else ""}{self.p(pred)}) {{'); self.indent += 1
            h(ins); self.indent -= 1; self.emit('}')
        else:
            h(ins)

    # float arithmetic
    def op_fadd(self, ins):
        if ins.f['rnd']: self.notes.add('rounding')
        self.fres(ins, f"{self.fa(ins)} + {self.fsrc(ins, 'b', 'neg_b', 'abs_b')}")
    def op_fadd32i(self, ins):
        self.fres(ins, f"{self.fa(ins)} + {self.fsrc(ins, 'b', 'neg_b', 'abs_b')}")
    def op_fmul(self, ins):
        f = ins.f
        if f['rnd']: self.notes.add('rounding')
        a, b = self.rf(f['a']), self.fsrc(ins, 'b', 'neg_b')
        e = f'fmul_fmz({a}, {b})' if f['fmz'] == 'fmz' else f'({a} * {b})'
        scale = {'': '', 'd2': ' * 0.5f', 'd4': ' * 0.25f', 'd8': ' * 0.125f', 'm2': ' * 2.0f', 'm4': ' * 4.0f', 'm8': ' * 8.0f'}[f['scale']]
        self.fres(ins, e + scale)
    def op_fmul32i(self, ins):
        f = ins.f; a, b = self.rf(f['a']), self.fsrc(ins, 'b')
        self.fres(ins, f'fmul_fmz({a}, {b})' if f['fmz'] == 'fmz' else f'({a} * {b})')
    def op_ffma(self, ins):
        f = ins.f
        if f['rnd']: self.notes.add('rounding')
        a, b, c = self.rf(f['a']), self.fsrc(ins, 'b', 'neg_b'), self.fsrc(ins, 'c', 'neg_c')
        self.fres(ins, f'ffma_fmz({a}, {b}, {c})' if f['fmz'] == 'fmz' else f'mad({a}, {b}, {c})')
    def op_fmnmx(self, ins):
        f = ins.f
        self.fres(ins, f"({self.pn(f['p'])} ? min({self.fa(ins)}, {self.fsrc(ins, 'b', 'neg_b', 'abs_b')}) : max({self.fa(ins)}, {self.fsrc(ins, 'b', 'neg_b', 'abs_b')}))")
    def fcmp_expr(self, cmp, a, b):
        return {
            'f': 'false', 't': 'true',
            'lt': f'({a} < {b})', 'eq': f'({a} == {b})', 'le': f'({a} <= {b})', 'gt': f'({a} > {b})',
            'ne': f'fne_ordered({a}, {b})', 'ge': f'({a} >= {b})', 'num': f'fnum({a}, {b})', 'nan': f'(!fnum({a}, {b}))',
            'ltu': f'(!({a} >= {b}))', 'equ': f'(!fne_ordered({a}, {b}))', 'leu': f'(!({a} > {b}))',
            'gtu': f'(!({a} <= {b}))', 'neu': f'({a} != {b})', 'geu': f'(!({a} < {b}))',
        }[cmp]
    def bop(self, op, x, y):
        return {'and': f'({x} && {y})', 'or': f'({x} || {y})', 'xor': f'({x} != {y})'}[op]
    def op_fsetp(self, ins):
        f = ins.f
        c = self.fcmp_expr(f['cmp'], self.fa(ins), self.fsrc(ins, 'b', 'neg_b', 'abs_b'))
        pr = self.pn(f['p'])
        self.emit(f'{{ bool c_ = {c};'); self.indent += 1
        self.setp(f['pd'], self.bop(f['bop'], 'c_', pr)); self.setp(f['pd2'], self.bop(f['bop'], '!c_', pr))
        self.indent -= 1; self.emit('}')
    def op_fset(self, ins):
        f = ins.f
        c = self.bop(f['bop'], self.fcmp_expr(f['cmp'], self.fa(ins), self.fsrc(ins, 'b', 'neg_b', 'abs_b')), self.pn(f['p']))
        self.ires(ins, f'({c} ? 0x3f800000u : 0u)' if f['bf'] else f'({c} ? 0xffffffffu : 0u)')
    def op_fcmp(self, ins):
        f = ins.f
        c = self.fcmp_expr(f['cmp'], self.fsrc(ins, 'c'), '0.0f')
        self.setr(f['d'], f"({c} ? {self.r(f['a'])} : {self.src(ins, 'b')})")
    def op_mufu(self, ins):
        f = ins.f; x = self.rf(f['a'])
        if f['abs']: x = f'abs({x})'
        if f['neg']: x = f'(-{x})'
        e = {'cos': f'cos({x})', 'sin': f'sin({x})', 'ex2': f'exp2({x})', 'lg2': f'log2({x})', 'rcp': f'(1.0f / {x})',
             'rsq': f'rsqrt({x})', 'sqrt': f'sqrt({x})'}.get(f['op'])
        if e is None: raise Unsupported(f"mufu {f['op']}")
        self.notes.add('mufu'); self.fres(ins, e)
    def op_rro(self, ins):
        # Range reduction is folded into the host sin/cos/exp2 (as Ryujinx does; nxport sass2spv float.rs emit_rro).
        self.fres(ins, self.fsrc(ins, 'b', 'neg', 'abs'))
    def op_fswzadd(self, ins):
        # Per-lane (lane & 3) 2-bit selector from the swizzle LUT: a*{1,-1,1,0} + b*{1,1,-1,1}.
        f = ins.f; self.uses.add('wave')
        if f['rnd']: self.notes.add('rounding')
        self.fres(ins, f"fswzadd_({self.rf(f['a'])}, {self.rf(f['b'])}, {f['swz']}u)")

    # packed half precision (HADD2 / HMUL2 / HFMA2)
    def half_src(self, ins, key, swz):
        """(low, high, is_f32) float expressions of a packed-half operand under its swizzle. The
        imm forms carry a 9+1+9-bit half pair (bits 6..15 / 22..31 of the 32-bit pair, the sign of
        the high half in bit 56); the 32I forms a full 32-bit pair."""
        v = ins.f[key]
        if not isinstance(v, tuple) and self._atom_of(ins, key) == 'imm':
            if ins.name in ('hadd2', 'hmul2', 'hfma2'):
                v = ((v & 0x1ff) << 6) | ((v >> 9 & 1) << 15) | ((v >> 10 & 0x1ff) << 22) | (int(ins.f.get('neg_hi', False)) << 31)
            x = f'0x{v & 0xffffffff:x}u'
        else:
            x = self.src(ins, key)
        if swz == 'f32':
            e = f'asfloat({x})'; return e, e, True
        lo, hi = f'f16tof32(({x}) & 0xffffu)', f'f16tof32(({x}) >> 16)'
        return {'h1_h0': (lo, hi), 'h0_h0': (lo, lo), 'h1_h1': (hi, hi)}[swz] + (False,)
    def half_op(self, ins, kind):
        """Packed-half semantics (operand roles, swizzles and merge modes as nxport sass2spv
        instructions/float.rs hadd2_*/hmul2_*/hfma2_*, from Ryujinx): each source is a pair of halves (or one fp32 value
        used for both lanes, swizzle f32); the two lanes are computed independently, saturated, and
        rounded to fp16 (round to nearest even, h_round) before the merge: h1_h0 packs both lanes,
        f32 writes the low lane as fp32, mrg_h0/mrg_h1 replace one half of the destination.
        fp16 inputs are exact in fp32 and fp32 add/mul of two fp16 values rounded once more to fp16
        equals the fp16 result (24 >= 2*11+2 bits); HFMA2 with all-fp16 sources is the one case
        where the fp32 intermediate can differ in the last fp16 bit (note 'half_fma')."""
        f = ins.f; n = ins.name
        merge = f.get('merge') or 'h1_h0'
        prec = f.get('prec') or ('ftz' if f.get('ftz') else '')
        if prec == 'prec3': raise Unsupported(f'{n} precision mode 3')
        a = self.half_src(ins, 'a', f.get('swz_a') or 'h1_h0')
        b = self.half_src(ins, 'b', f.get('swz_b') or ('f32' if isinstance(f['b'], tuple) else 'h1_h0'))
        srcs = [a, b]
        if kind == 'fma':
            srcs.append(self.half_src(ins, 'c', f.get('swz_c') or ('f32' if isinstance(f['c'], tuple) else 'h1_h0')))
            if not any(s[2] for s in srcs): self.notes.add('half_fma')
        def mod(s, lane, neg, absk):
            e = s[lane]
            if prec == 'ftz' and not s[2]: e = f'h_ftz({e})'     # fp16 denormal inputs flush to zero
            if absk and f.get(absk): e = f'abs({e})'
            if f.get(neg): e = f'(-{e})'
            return e
        lanes = []
        for lane in (0, 1):
            x = mod(a, lane, 'neg_a', 'abs_a'); y = mod(b, lane, 'neg_b', 'abs_b')
            if kind == 'add': e = f'({x} + {y})'
            elif kind == 'mul': e = f'fmul_fmz({x}, {y})' if prec == 'fmz' else f'({x} * {y})'
            else:
                z = mod(srcs[2], lane, 'neg_c', None)
                e = f'ffma_fmz({x}, {y}, {z})' if prec == 'fmz' else f'mad({x}, {y}, {z})'
            if f.get('sat'): e = f'saturate({e})'
            lanes.append(e)
        flush = 'h_round_ftz' if prec == 'ftz' else 'h_round'
        lo, hi = f'{flush}({lanes[0]})', f'{flush}({lanes[1]})'
        d = f['d']; old = self.r(d)
        e = {'h1_h0': f'({lo} | ({hi} << 16))', 'f32': f'asuint(f16tof32({lo}))',
             'mrg_h0': f'(({old}) & 0xffff0000u) | {lo}', 'mrg_h1': f'(({old}) & 0xffffu) | ({hi} << 16)'}[merge]
        self.uses.add('half'); self.setr(d, e)
    def op_hadd2(self, ins): self.half_op(ins, 'add')
    def op_hadd2_32i(self, ins): self.half_op(ins, 'add')
    def op_hmul2(self, ins): self.half_op(ins, 'mul')
    def op_hmul2_32i(self, ins): self.half_op(ins, 'mul')
    def op_hfma2(self, ins): self.half_op(ins, 'fma')
    def op_hfma2_32i(self, ins): self.half_op(ins, 'fma')

    # conversions
    def op_f2f(self, ins):
        f = ins.f
        if f['sfmt'] != 'f32' or f['dfmt'] != 'f32':
            return self.f2f_wide(ins)
        x = self.fsrc(ins, 'b', 'neg', 'abs')
        e = {'': x, 'pass': x, 'round': f'round_ne({x})', 'floor': f'floor({x})', 'ceil': f'ceil({x})', 'trunc': f'trunc({x})'}[f['rnd']]
        self.fres(ins, e)
    def f2f_wide(self, ins):
        f = ins.f; self.uses.add('fp64')
        if f['sfmt'] == 'f32' and f['dfmt'] == 'f64':
            x = self.fsrc(ins, 'b', 'neg', 'abs')
            d = f['d']; self.emit(f'{{ uint lo_, hi_; asuint((double){x}, lo_, hi_); r{d} = lo_; r{d + 1} = hi_; }}')
            self.regs.update((d, d + 1)); return
        if f['sfmt'] == 'f64' and f['dfmt'] == 'f32':
            b = ins.f['b']
            if isinstance(b, tuple): x = f'asdouble({self.cbuf(b)}, {self.cbuf(b, 1)})'
            else: x = f'asdouble({self.r(b)}, {self.r(b + 1)})'
            if f['abs']: x = f'abs({x})'
            if f['neg']: x = f'(-{x})'
            self.fres(ins, f'(float){x}'); return
        raise Unsupported(f"f2f {f['sfmt']}->{f['dfmt']}")
    def op_f2i(self, ins):
        f = ins.f
        if f['sfmt'] != 'f32': raise Unsupported('f2i from non-f32')
        x = self.fsrc(ins, 'b', 'neg', 'abs')
        x = {'': f'round_ne({x})', 'floor': f'floor({x})', 'ceil': f'ceil({x})', 'trunc': f'trunc({x})'}[f['rnd']]
        conv = {'s32': 'f2i_s32', 'u32': 'f2i_u32', 's16': 'f2i_s16', 'u16': 'f2i_u16', 's8': 'f2i_s8', 'u8': 'f2i_u8'}.get(f['dfmt'])
        if not conv: raise Unsupported(f"f2i to {f['dfmt']}")
        self.ires(ins, f'{conv}({x})')
    def int_src(self, ins, fmt, key='b', bsel='bsel'):
        x = self.src(ins, key)
        sel = {'': 0, 'b1': 1, 'b2': 2, 'b3': 3}[ins.f.get(bsel, '')]
        size = {'u8': 8, 's8': 8, 'u16': 16, 's16': 16, 'u32': 32, 's32': 32}[fmt]
        if size < 32:
            x = f'(({x}) >> {sel * 8}u)'
            x = f'(uint)(((int)({x} << {32 - size}u)) >> {32 - size})' if fmt[0] == 's' else f'({x} & 0x{(1 << size) - 1:x}u)'
        return x
    def op_i2f(self, ins):
        f = ins.f
        if f['dfmt'] != 'f32' or f['sfmt'] in ('u64', 's64'): raise Unsupported(f"i2f {f['sfmt']}->{f['dfmt']}")
        if f['rnd']: self.notes.add('rounding')
        x = self.int_src(ins, f['sfmt'])
        signed = f['sfmt'][0] == 's'
        if f['abs']: x = f'(uint)abs((int){x})'
        if f['neg']: x = f'(uint)(-(int){x})'
        self.fres(ins, f'(float)(int){x}' if signed or f['abs'] or f['neg'] else f'(float){x}')
    def op_i2i(self, ins):
        f = ins.f
        x = self.int_src(ins, f['sfmt'])
        if f['abs']: x = f'(uint)abs((int){x})'
        if f['neg']: x = f'(uint)(-(int){x})'
        d = f['dfmt']
        if d in ('u32', 's32') and not f['sat']: e = x
        elif d == 's32' and f['sat'] and f['sfmt'] == 'u32': e = f'min({x}, 0x7fffffffu)'
        elif d == 'u32' and f['sat'] and f['sfmt'][0] == 's': e = f'(uint)max((int){x}, 0)'
        elif d in ('u32', 's32'): e = x
        else:
            lo, hi = {'u8': (0, 255), 's8': (-128, 127), 'u16': (0, 65535), 's16': (-32768, 32767)}[d]
            signed_src = f['sfmt'][0] == 's'
            if f['sat']:
                e = f'(uint)clamp((int){x}, {lo}, {hi})' if signed_src else f'min({x}, {hi}u)'
            else:
                size = 8 if '8' in d else 16
                e = f'(uint)(((int)({x} << {32 - size}u)) >> {32 - size})' if d[0] == 's' else f'({x} & 0x{(1 << size) - 1:x}u)'
        self.ires(ins, e)

    # integer arithmetic
    def ineg(self, e, flag): return f'(uint)(-(int){e})' if flag else e
    def op_iadd(self, ins):
        f = ins.f
        a = self.ineg(self.r(f['a']), f['neg_a']); b = self.ineg(self.src(ins, 'b'), f['neg_b'])
        if f['sat']: raise Unsupported('iadd.sat')
        self.iadd_core(ins, a, b)
    def iadd_core(self, ins, a, b):
        f = ins.f; carry_in = ' + (cc_c ? 1u : 0u)' if f.get('x') else ''
        if f.get('x'): self.uses.add('cc')
        if f.get('cc'):
            self.uses.add('cc')
            d = f['d']
            self.emit(f'{{ uint a_ = {a}, b_ = {b}; uint t_ = a_ + b_{carry_in}; '
                      f'cc_c = t_ < a_ || (t_ == a_ && b_ != 0u{" || cc_c" if carry_in else ""}) ; '
                      f'cc_o = ((~(a_ ^ b_)) & (a_ ^ t_)) >> 31 != 0u; cc_z = t_ == 0u; cc_s = (int)t_ < 0;'
                      + (f' r{d} = t_;' if d != 255 else '') + ' }')
            if d != 255: self.regs.add(d)
        else:
            self.setr(f['d'], f'{a} + {b}{carry_in}')
    def op_iadd32i(self, ins):
        f = ins.f
        if f['sat']: raise Unsupported('iadd32i.sat')
        self.iadd_core(ins, self.ineg(self.r(f['a']), f['neg_a']), self.src(ins, 'b'))
    def op_iadd3(self, ins):
        f = ins.f
        if f.get('x') or f.get('cc'): raise Unsupported('iadd3 x/cc')
        def half(e, h): return {'': e, 'h0': f'({e} & 0xffffu)', 'h1': f'({e} >> 16)'}[h]
        a = self.ineg(half(self.r(f['a']), f.get('ha', '')), f['neg_a'])
        b = self.ineg(half(self.src(ins, 'b'), f.get('hb', '')), f['neg_b'])
        c = self.ineg(half(self.r(f['c']), f.get('hc', '')), f['neg_c'])
        shift = f.get('shift', '')
        # K4 (batch-4): .RS shifts the sum with its carry (33 bits): NVCC's mulhi idiom (XMAD lo*lo, lo*hi, hi*lo, XMAD.CHI,
        # IADD3.RS) is exact only then; a 32-bit sum is 65536 low in 7% of random 32x32 products (play-work/explode-work2/
        # shader-translation-b.md C; nxport crates/sass2spv/src/instructions/integer.rs:283-293 has the 32-bit form too).
        # With a negated operand the old form stays.
        if shift == 'rs' and IADD3_RS_CARRY and not f['neg_a'] and not f['neg_b']:
            self.setr(f['d'], f'(uint)(((uint64_t)({a}) + (uint64_t)({b})) >> 16) + {c}')
        elif shift == 'rs': self.setr(f['d'], f'(({a} + {b}) >> 16) + {c}')
        elif shift == 'ls': self.setr(f['d'], f'(({a} + {b}) << 16) + {c}')
        else: self.setr(f['d'], f'{a} + {b} + {c}')
    def op_iscadd(self, ins):
        f = ins.f
        a = self.ineg(self.r(f['a']), f['neg_a']); b = self.ineg(self.src(ins, 'b'), f['neg_b'])
        self.iadd_core(ins, f"({a} << {f['shift']}u)", b)
    def op_lea(self, ins):
        f = ins.f
        if f['x'] or f['pd'] != 7: raise Unsupported('lea x/predicate')
        a = self.ineg(self.r(f['a']), f['neg_a'])
        self.iadd_core(ins, f"({a} << {f['shift']}u)", self.src(ins, 'b'))
    def op_xmad(self, ins):
        f = ins.f
        if f.get('x') or f.get('cc'): raise Unsupported('xmad x/cc')
        signs = f['signs']; sa = signs in ('s16 u16', 's16 s16'); sb = signs in ('u16 s16', 's16 s16')
        def half(e, h1, signed):
            if signed: return f'(uint)(((int)({e} << {0 if h1 else 16}u)) >> 16)'
            return f'({e} >> 16)' if h1 else f'({e} & 0xffffu)'
        a = half(self.r(f['a']), f.get('h1_a'), sa)
        braw = self.src(ins, 'b')
        b = half(braw, f.get('h1_b'), sb)
        c = self.src(ins, 'c') if 'c' in f else None
        mode = f['mode']
        if c is None: raise Unsupported('xmad without c')
        opc = {'': c, 'clo': f'({c} & 0xffffu)', 'chi': f'({c} >> 16)', 'cbcc': f'(({braw} << 16) + {c})'}.get(mode)
        if opc is None: raise Unsupported(f'xmad mode {mode}')
        prod = f'({a} * {b})'
        if f.get('psl'): prod = f'({prod} << 16)'
        e = f'{prod} + {opc}'
        if f.get('mrg'): e = f'((({e}) & 0xffffu) | ({braw} << 16))'
        self.setr(f['d'], e)
    def icmp_expr(self, cmp, a, b, unsigned):
        if not unsigned: a, b = f'(int){a}', f'(int){b}'
        return {'f': 'false', 't': 'true', 'lt': f'({a} < {b})', 'eq': f'({a} == {b})', 'le': f'({a} <= {b})',
                'gt': f'({a} > {b})', 'ne': f'({a} != {b})', 'ge': f'({a} >= {b})'}[cmp]
    def icmp_extended(self, cmp, a, b, unsigned):
        """.X compare: the high words of a 64-bit compare whose low words were subtracted by a
        preceding IADD.CC (CC.C = no borrow, CC.Z = low words equal); nxport sass2spv int_compare.rs int_comparison_extended."""
        self.uses.add('cc')
        A, B = (f'(uint64_t){a}', f'(uint64_t){b}') if unsigned else (f'(int64_t)(int){a}', f'(int64_t)(int){b}')
        lt = f'({A} < {B} + (cc_c ? 0 : 1))'; eq = f'({a} == {b} && cc_z)'
        return {'f': 'false', 't': 'true', 'lt': lt, 'eq': eq, 'le': f'({lt} || {eq})', 'gt': f'(!{lt} && !{eq})',
                'ne': f'(!{eq})', 'ge': f'(!{lt})'}[cmp]
    def op_isetp(self, ins):
        f = ins.f
        if f['x']: c = self.icmp_extended(f['cmp'], self.r(f['a']), self.src(ins, 'b'), f['u32'] == 'u32')
        else: c = self.icmp_expr(f['cmp'], self.r(f['a']), self.src(ins, 'b'), f['u32'] == 'u32')
        pr = self.pn(f['p'])
        self.emit(f'{{ bool c_ = {c};'); self.indent += 1
        self.setp(f['pd'], self.bop(f['bop'], 'c_', pr)); self.setp(f['pd2'], self.bop(f['bop'], '!c_', pr))
        self.indent -= 1; self.emit('}')
    def op_iset(self, ins):
        f = ins.f
        cmp = self.icmp_extended if f['x'] else self.icmp_expr
        c = self.bop(f['bop'], cmp(f['cmp'], self.r(f['a']), self.src(ins, 'b'), f['u32'] == 'u32'), self.pn(f['p']))
        self.ires(ins, f'({c} ? 0x3f800000u : 0u)' if f['bf'] else f'({c} ? 0xffffffffu : 0u)')
    def op_icmp(self, ins):
        f = ins.f
        c = self.icmp_expr(f['cmp'], self.src(ins, 'c'), '0u', f['u32'] == 'u32')
        self.setr(f['d'], f"({c} ? {self.r(f['a'])} : {self.src(ins, 'b')})")
    def op_imnmx(self, ins):
        f = ins.f
        if f['xmode']: raise Unsupported('imnmx extended')
        a, b = self.r(f['a']), self.src(ins, 'b')
        if f['u32'] != 'u32': a, b = f'(int){a}', f'(int){b}'
        self.ires(ins, f"(uint)({self.pn(f['p'])} ? min({a}, {b}) : max({a}, {b}))")
    def logic(self, op, a, b):
        return {'and': f'({a} & {b})', 'or': f'({a} | {b})', 'xor': f'({a} ^ {b})', 'pass_b': b}[op]
    def pred_result(self, ins, res):
        f = ins.f
        if f.get('pd', 7) == 7: return
        pop = f['pop']
        if not pop: raise Unsupported('logic predicate op f')
        self.setp(f['pd'], {'t': 'true', 'z': f'({res} == 0u)', 'nz': f'({res} != 0u)'}[pop])
    def op_lop(self, ins):
        f = ins.f
        if f['x']: raise Unsupported('lop.x')
        a = self.r(f['a']); b = self.src(ins, 'b')
        if f['inv_a']: a = f'(~{a})'
        if f['inv_b']: b = f'(~{b})'
        e = self.logic(f['lop'], a, b)
        if f['pd'] != 7:
            self.emit(f'{{ uint l_ = {e};'); self.indent += 1
            self.pred_result(ins, 'l_'); self.ires(ins, 'l_')
            self.indent -= 1; self.emit('}')
        else: self.ires(ins, e)
    def op_lop32i(self, ins):
        f = ins.f
        if f['x']: raise Unsupported('lop32i.x')
        a = self.r(f['a']); b = self.src(ins, 'b')
        if f['inv_a']: a = f'(~{a})'
        if f['inv_b']: b = f'(~{b})'
        self.ires(ins, self.logic(f['lop'], a, b))
    def op_lop3(self, ins):
        f = ins.f
        if f.get('x'): raise Unsupported('lop3.x')
        a = self.r(f['a']); b = self.src(ins, 'b')
        c = self.r(bits(ins.word, 39, 8)) if 'c' not in f else self.r(f['c'])   # imm form: envydis omits reg39
        lut = f['lut']
        terms = []
        for i in range(8):
            if lut >> i & 1:
                terms.append('(' + ' & '.join([a if i & 4 else f'~{a}', b if i & 2 else f'~{b}', c if i & 1 else f'~{c}']) + ')')
        e = ' | '.join(terms) if terms else '0u'
        if lut == 0xff: e = '0xffffffffu'
        if f.get('pd', 7) != 7:
            self.emit(f'{{ uint l_ = {e};'); self.indent += 1
            self.pred_result(ins, 'l_'); self.ires(ins, 'l_'); self.indent -= 1; self.emit('}')
        else: self.ires(ins, e)
    def op_shl(self, ins):
        f = ins.f
        if f['x']: raise Unsupported('shl.x')
        s = self.src(ins, 'b')
        self.ires(ins, f"({self.r(f['a'])} << ({s} & 31u))" if f['w'] else f"shl_clamp({self.r(f['a'])}, {s})")
    def op_shr(self, ins):
        f = ins.f
        if f['x'] or f['brev']: raise Unsupported('shr.x/brev')
        a, s = self.r(f['a']), self.src(ins, 'b')
        if f['u32'] == 'u32':
            self.ires(ins, f'({a} >> ({s} & 31u))' if f['w'] else f'shr_u_clamp({a}, {s})')
        else:
            self.ires(ins, f'(uint)((int){a} >> ({s} & 31u))' if f['w'] else f'shr_s_clamp({a}, {s})')
    def op_shf(self, ins):
        f = ins.f
        if f['hix'] or f.get('cc'): raise Unsupported('shf hi/x')
        lo, s, hi = self.r(f['a']), self.src(ins, 'b'), self.r(f['c'])
        left = ins.tokens[ins.tokens.index('shf') + 1] == 'l'
        mask = '63u' if f['type'] else '31u'
        fn = 'shf_l' if left else ('shf_r_s' if f['type'] == 's64' else 'shf_r_u')
        self.setr(f['d'], f'{fn}({lo}, {hi}, {s}, {mask}, {"true" if f["w"] else "false"})')
    def op_flo(self, ins):
        # Find leading one: bit index of the most significant 1 (signed: first bit differing from the sign),
        # 0xffffffff when none; .sh returns the shift amount (31 - index).
        f = ins.f; x = self.src(ins, 'b')
        if f['inv']: x = f'(~{x})'
        e = f'firstbithigh({x})' if f['u32'] == 'u32' else f'firstbithigh((int){x})'
        if f['sh']: e = f'(({e}) == 0xffffffffu ? 0xffffffffu : 31u - ({e}))'
        self.ires(ins, f'(uint)({e})')
    def op_popc(self, ins):
        x = self.src(ins, 'b')
        self.setr(ins.f['d'], f"countbits({'~' if ins.f['inv'] else ''}{x})")
    def op_bfe(self, ins):
        f = ins.f
        if f['brev']: raise Unsupported('bfe.brev')
        fn = 'bfe_u' if f['u32'] == 'u32' else 'bfe_s'
        self.ires(ins, f"{fn}({self.r(f['a'])}, {self.src(ins, 'b')})")
    def op_bfi(self, ins):
        f = ins.f
        self.ires(ins, f"bfi_({self.r(f['a'])}, {self.src(ins, 'b')}, {self.src(ins, 'c')})")
    def op_sel(self, ins):
        f = ins.f
        self.setr(f['d'], f"({self.pn(f['p'])} ? {self.r(f['a'])} : {self.src(ins, 'b')})")
    def op_mov(self, ins):
        f = ins.f
        if 'sr' in f: return self.op_s2r(ins)
        if f['mask'] != 0xf: raise Unsupported('mov with partial lane mask')
        self.setr(f['d'], self.src(ins, 'b'))
    def op_mov32i(self, ins):
        f = ins.f
        if f['mask'] != 0xf: raise Unsupported('mov32i with partial lane mask')
        self.setr(f['d'], self.src(ins, 'b'))
    def op_s2r(self, ins):
        sr = ins.f['sr']
        if self.prog.stage == 'compute':
            e = {0x00: 'lane_id()', 0x20: '(tid_.x | (tid_.y << 16) | (tid_.z << 26))', 0x21: 'tid_.x', 0x22: 'tid_.y', 0x23: 'tid_.z',
                 0x25: 'ctaid_.x', 0x26: 'ctaid_.y', 0x27: 'ctaid_.z',
                 0x38: '(1u << lane_id())', 0x39: '((1u << lane_id()) - 1u)', 0x3a: '(lane_id() == 31u ? 0xffffffffu : (2u << lane_id()) - 1u)',
                 0x3b: '(lane_id() == 31u ? 0u : ~((2u << lane_id()) - 1u))', 0x3c: '(~((1u << lane_id()) - 1u))'}.get(sr)
            if sr in (0x00, 0x38, 0x39, 0x3a, 0x3b, 0x3c): self.uses.add('wave'); self.notes.add('warp')
            if e is None: raise Unsupported(f'compute system register {sr:#x}')
            return self.setr(ins.f['d'], e)
        e = {0x12: 'asuint(host_y_direction)', 0x00: 'lane_id()', 0x10: '0u', 0x11: 'invocation_id_()', 0x13: 'thread_kill_()',
             # SR_LANEMASK_EQ/LT/LE/GT/GE, as in compute (same lane numbering as lane_id and wave_ballot)
             0x38: '(1u << lane_id())', 0x39: '((1u << lane_id()) - 1u)', 0x3a: '(lane_id() == 31u ? 0xffffffffu : (2u << lane_id()) - 1u)',
             0x3b: '(lane_id() == 31u ? 0u : ~((2u << lane_id()) - 1u))', 0x3c: '(~((1u << lane_id()) - 1u))'}.get(sr)
        if sr in (0x38, 0x39, 0x3a, 0x3b, 0x3c): self.uses.add('wave'); self.notes.add('warp')
        if e is None: raise Unsupported(f'system register {sr:#x}')
        self.setr(ins.f['d'], e)
    def op_r2p(self, ins):
        f = ins.f
        if f['dst'] != 'pr': raise Unsupported('r2p cc')
        sel = {'': 0, 'b1': 8, 'b2': 16, 'b3': 24}[f['bsel']]
        mask = ins.f['b'] if self._atom_of(ins, 'b') == 'imm' else None
        if mask is None: raise Unsupported('r2p register mask')
        for i in range(7):
            if mask >> i & 1: self.setp(i, f"((({self.r(f['a'])} >> {sel + i}u) & 1u) != 0u)")
    def op_psetp(self, ins):
        f = ins.f
        ab = self.bop(f['bop2'], self.pn(f['pa']), self.pn(f['pb']))
        self.emit(f'{{ bool c_ = {ab};'); self.indent += 1
        self.setp(f['pd'], self.bop(f['bop'], 'c_', self.pn(f['p']))); self.setp(f['pd2'], self.bop(f['bop'], '!c_', self.pn(f['p'])))
        self.indent -= 1; self.emit('}')
    def op_pset(self, ins):
        f = ins.f
        c = self.bop(f['bop'], self.bop(f['bop2'], self.pn(f['pa']), self.pn(f['pb'])), self.pn(f['p']))
        self.ires(ins, f'({c} ? 0x3f800000u : 0u)' if f['bf'] else f'({c} ? 0xffffffffu : 0u)')
    def cc_test(self, cc):
        self.uses.add('cc')
        return {'': 'true', 't': 'true', 'f': 'false', 'eq': 'cc_z', 'ne': '!cc_z', 'lt': '(cc_s != cc_o)',
                'le': '(cc_z || cc_s != cc_o)', 'gt': '(!cc_z && cc_s == cc_o)', 'ge': '(cc_s == cc_o)',
                'neu': '!cc_z', 'equ': 'cc_z', 'ltu': '(cc_s != cc_o)', 'geu': '(cc_s == cc_o)',
                'lo': '!cc_c', 'hs': 'cc_c', 'hi': '(cc_c && !cc_z)', 'ls': '(!cc_c || cc_z)',
                'sff': '!cc_s', 'sft': 'cc_s', 'off': '!cc_o', 'oft': 'cc_o'}.get(cc) or self._bad_cc(cc)
    def _bad_cc(self, cc): raise Unsupported(f'condition code test {cc}')
    def op_csetp(self, ins):
        f = ins.f; c = self.cc_test(f['cc'])
        self.emit(f'{{ bool c_ = {c};'); self.indent += 1
        self.setp(f['pd'], self.bop(f['bop'], 'c_', self.pn(f['p']))); self.setp(f['pd2'], self.bop(f['bop'], '!c_', self.pn(f['p'])))
        self.indent -= 1; self.emit('}')
    def op_vote(self, ins):
        f = ins.f; self.uses.add('wave'); self.notes.add('warp'); pr = self.pn(f['p'])
        mode = f['mode']
        self.setr(f['d'], f'wave_ballot({pr})')
        self.setp(f['pd'], {'all': f'WaveActiveAllTrue({pr})', 'any': f'WaveActiveAnyTrue({pr})', 'eq': f'WaveActiveAllEqual({pr})'}[mode])
    def op_shfl(self, ins):
        # PTX-style SHFL: c = (segment mask << 8) | clamp. Out-of-range lanes keep their own value.
        f = ins.f; self.uses.add('wave'); self.notes.add('warp')
        b = self.src(ins, 'b'); c = self.src(ins, 'c')
        self.emit(f"{{ uint2 sh_ = shfl_{f['mode']}({self.r(f['a'])}, {b}, {c});"); self.indent += 1
        self.setr(f['d'], 'sh_.x'); self.setp(f['pd'], '(sh_.y != 0u)')
        self.indent -= 1; self.emit('}')
    def op_vset(self, ins):
        raise Unsupported('vset')
    def video_operand(self, value, width, sel, signed):
        """Video instruction operand: width 0/1 byte, 2 halfword, 3 word (= VectorSelect >> 1, nxport helper.rs extend)."""
        if width == 3: return f'(int){value}' if signed else value
        bits = 8 if width < 2 else 16; pos = sel * bits
        if signed: return f'(((int){value} << {32 - pos - bits}) >> {32 - bits})'
        return f'(({value} >> {pos}u) & 0x{(1 << bits) - 1:x}u)'
    def op_vmnmx(self, ins):
        # VMNMX (field positions as nxport decoders/inst_decoders.rs InstVmnmx, read from the raw word): d = op(min|max(a', b'), c)
        # where a', b' are the selected byte/halfword/word of Ra and Rb (or a 16-bit immediate), the
        # first min/max takes its signedness from b, the second (MIN/MAX with Rc) from DSIGN.
        w = ins.word; f = ins.f
        if bits(w, 47, 1): raise Unsupported('vmnmx cc')
        if bits(w, 55, 1): raise Unsupported('vmnmx sat')
        op = bits(w, 51, 3)
        if op not in (5, 6): raise Unsupported(f'vmnmx secondary op {op}')
        b_reg = bits(w, 50, 1)
        a_w, a_sel, a_s = bits(w, 37, 2), bits(w, 36, 2), bits(w, 48, 1)
        b_w, b_sel, b_s = (bits(w, 29, 2), bits(w, 28, 2), bits(w, 49, 1)) if b_reg else (2, 0, bits(w, 49, 1))
        a = self.video_operand(self.r(bits(w, 8, 8)), a_w, a_sel, a_s)
        b = self.video_operand(self.r(bits(w, 20, 8)) if b_reg else f'{bits(w, 20, 16)}u', b_w, b_sel, b_s)
        c = self.r(bits(w, 39, 8))
        t1 = 'int' if b_s else 'uint'; t2 = 'int' if bits(w, 54, 1) else 'uint'
        first = f"{'max' if bits(w, 56, 1) else 'min'}(({t1})({a}), ({t1})({b}))"
        self.setr(bits(w, 0, 8), f"asuint({'min' if op == 5 else 'max'}(({t2})({first}), ({t2}){c}))")

    # fp64
    def dsrc(self, ins, key, neg=None, absk=None):
        v = ins.f[key]; kind = 'mem' if isinstance(v, tuple) else self._atom_of(ins, key)
        if kind == 'imm': e = f'asdouble(0x{v & 0xffffffff:x}u, 0x{v >> 32 & 0xffffffff:x}u)'
        elif kind == 'mem': e = f'asdouble({self.cbuf(v)}, {self.cbuf(v, 1)})'
        else: e = f'asdouble({self.r(v)}, {self.r(v + 1) if v != 255 else "0u"})'
        if absk and ins.f.get(absk): e = f'abs({e})'
        if neg and ins.f.get(neg): e = f'(-{e})'
        return e
    def dres(self, ins, e):
        d = ins.f['d']; self.uses.add('fp64')
        if ins.f.get('cc'): raise Unsupported('fp64 cc')
        if d == 255: return
        self.regs.update((d, d + 1)); self.emit(f'asuint({e}, r{d}, r{d + 1});')
    def op_dadd(self, ins):
        a = self.dsrc(ins, 'a', 'neg_a', 'abs_a'); self.dres(ins, f"{a} + {self.dsrc(ins, 'b', 'neg_b', 'abs_b')}")
    def op_dmul(self, ins):
        self.dres(ins, f"{self.dsrc(ins, 'a')} * {self.dsrc(ins, 'b', 'neg_b')}")
    def op_dfma(self, ins):
        self.dres(ins, f"fma({self.dsrc(ins, 'a')}, {self.dsrc(ins, 'b', 'neg_b')}, {self.dsrc(ins, 'c', 'neg_c')})")

    # memory and I/O
    def ld_base_aliased(self, reg, d, words):
        """A multi-word load whose address register is written before its last word is read (d <= reg < d + words - 1).
        TOTK_LDC_ALIAS_FIX=0: the old translation (word i's address formed after words 0..i-1 were written)."""
        if words < 2 or reg == 255 or d == 255 or not (d <= reg < d + words - 1): return False
        return os.environ.get('TOTK_LDC_ALIAS_FIX', '1') != '0'
    def op_ld(self, ins):
        f = ins.f; space, bank, reg, off = f['mem']
        words = {'b32': 1, 'b64': 2, 'b96': 3, 'b128': 4, 'u8': 1, 's8': 1, 'u16': 1, 's16': 1}[f['size']]
        d = f['d']
        if space == 'a': return self.load_attr(ins, reg, off, words, d)
        if space == 'c':
            if f['size'] in ('u8', 's8', 'u16', 's16'):
                x = self.cbuf((space, bank, reg, off & ~3))
                sh = (off & 3) * 8; size = 8 if '8' in f['size'] else 16
                e = f'(({x} >> {sh}u) & 0x{(1 << size) - 1:x}u)' if f['size'][0] == 'u' else f'(uint)(((int)({x} << {32 - size - sh}u)) >> {32 - size})'
                return self.setr(d, e)
            if f.get('cache'): self.notes.add('cbuf_cache_hint')
            if self.ld_base_aliased(reg, d, words):
                # LDC.64/.128 Rd, c[b][Rd+off]: every word comes from the address formed with the base register's
                # value before the load (the source GPU reads its operands first). Writing word 0 into the base
                # register and then forming word 1's address from it read the wrong constant (batch-d: 1668 programs,
                # e.g. 571 lit pixel shaders load the sun-shadow cascade matrix's w row c4[R+728] this way: m33 came
                # from c4[asuint(m32) + 732], 0 when m32 is -0.0, so w = 0 and the shadow compares failed; skinning
                # vertex shaders read bone matrix words the same way).
                self.used_cbufs.add(bank); self.uses.add('cbdyn')
                self.emit(f'{{ uint ldc_base_ = {self.r(reg)};'); self.indent += 1
                for i in range(words): self.setr(d + i, f'cbuf_load{bank}(ldc_base_ + {(off + 4 * i) & 0xffffffff}u)')
                self.indent -= 1; self.emit('}')
                return
            for i in range(words): self.setr(d + i if d != 255 else 255, self.cbuf((space, bank, reg, off), i))
            return
        if space == 's':
            if self.prog.stage != 'compute': raise Unsupported('shared memory outside compute')
            # .U (bit 44) is a uniform-address hint, not a lock; nxport's lds (sass2spv memory.rs) ignores it too
            self.uses.add('smem')
            base = f'({self.r(reg)} + {off & 0xffffffff}u)' if reg != 255 else f'{off & 0xffffffff}u'
            if f['size'] in ('u8', 's8', 'u16', 's16'):
                size = 8 if '8' in f['size'] else 16
                self.emit(f'{{ uint sa_ = {base}; uint w_ = smem_load(sa_); uint sh_ = (sa_ & 3u) * 8u;'); self.indent += 1
                if f['size'][0] == 'u': self.setr(d, f'((w_ >> sh_) & 0x{(1 << size) - 1:x}u)')
                else: self.setr(d, f'(uint)(((int)(w_ << ({32 - size}u - sh_))) >> {32 - size})')
                self.indent -= 1; self.emit('}'); return
            words = {'b32': 1, 'b64': 2, 'b128': 4}[f['size']]
            self.emit(f'{{ uint sa_ = {base};'); self.indent += 1
            for i in range(words): self.setr(d + i if d != 255 else 255, f'smem_load(sa_ + {4 * i}u)')
            self.indent -= 1; self.emit('}')
            return
        if space == 'l':
            self.uses.add('lmem')
            if f['size'] not in ('b32', 'b64', 'b128'): raise Unsupported('ld.local sub-word')
            base = f'({self.r(reg)} + {off & 0xffffffff}u)' if reg != 255 else f'{off & 0xffffffff}u'
            if self.ld_base_aliased(reg, d, words):   # LDL.64/.128 Rd, [Rd+off]: as LDC above, one address for every word
                self.emit(f'{{ uint ldl_base_ = {base};'); self.indent += 1
                for i in range(words): self.setr(d + i, f'lmem_load(ldl_base_ + {4 * i}u)')
                self.indent -= 1; self.emit('}')
                return
            for i in range(words): self.setr(d + i if d != 255 else 255, f'lmem_load({base} + {4 * i}u)')
            return
        raise Unsupported(f'ld {space}')
    def op_st(self, ins):
        f = ins.f; space, bank, reg, off = f['mem']
        if space == 's':
            if self.prog.stage != 'compute': raise Unsupported('shared memory outside compute')
            self.uses.add('smem'); v = f['v']
            base = f'({self.r(reg)} + {off & 0xffffffff}u)' if reg != 255 else f'{off & 0xffffffff}u'
            if f['size'] in ('u8', 's8', 'u16', 's16'):
                mask = 0xff if '8' in f['size'] else 0xffff
                self.emit(f"smem_store_bits({base}, {self.r(v) if v != 255 else '0u'}, 0x{mask:x}u);"); return
            words = {'b32': 1, 'b64': 2, 'b128': 4}[f['size']]
            self.emit(f'{{ uint sa_ = {base};'); self.indent += 1
            for i in range(words): self.emit(f"smem_store(sa_ + {4 * i}u, {self.r(v + i) if v != 255 else '0u'});")
            self.indent -= 1; self.emit('}')
            return
        words = {'b32': 1, 'b64': 2, 'b96': 3, 'b128': 4}.get(f['size'])
        if words is None: raise Unsupported('st sub-word')
        v = f['v']
        if space == 'a':
            if reg != 255 or f.get('c', 255) != 255 or f.get('p'): raise Unsupported('indexed/patch attribute store')
            for i in range(words): self.store_attr(off + 4 * i, self.r(v + i) if v != 255 else '0u')
            return
        if space == 'l':
            self.uses.add('lmem')
            base = f'({self.r(reg)} + {off & 0xffffffff}u)' if reg != 255 else f'{off & 0xffffffff}u'
            for i in range(words): self.emit(f'lmem_store({base} + {4 * i}u, {self.r(v + i) if v != 255 else "0u"});')
            return
        raise Unsupported(f'st {space}')
    def gaddr(self, ins):
        f = ins.f; space, bank, reg, off = f['mem']
        if not f['e']: raise Unsupported('32-bit global address')
        self.uses.add('gmem')
        lo = self.r(reg); hi = self.r(reg + 1) if reg != 255 else '0u'
        return f'gmem_addr({lo}, {hi}, {off})'
    def op_ldg(self, ins):
        f = ins.f; words = {'b32': 1, 'b64': 2, 'b128': 4, 'u b128': 4}.get(f['size'])
        if f['size'] in ('u8', 's8', 'u16', 's16'):
            size = 8 if '8' in f['size'] else 16
            self.emit(f'{{ uint2 ga_ = {self.gaddr(ins)}; uint w_ = gmem_load(ga_, 0u); uint sh_ = (ga_.x & 3u) * 8u;'); self.indent += 1
            if f['size'][0] == 'u': self.setr(f['d'], f'((w_ >> sh_) & 0x{(1 << size) - 1:x}u)')
            else: self.setr(f['d'], f'(uint)(((int)(w_ << ({32 - size}u - sh_))) >> {32 - size})')
            self.indent -= 1; self.emit('}'); return
        if words is None: raise Unsupported(f"ldg {f['size']}")
        self.emit(f'{{ uint2 ga_ = {self.gaddr(ins)};'); self.indent += 1
        for i in range(words): self.setr(f['d'] + i if f['d'] != 255 else 255, f'gmem_load(ga_, {4 * i}u)')
        self.indent -= 1; self.emit('}')
    def op_stg(self, ins):
        f = ins.f; words = {'b32': 1, 'b64': 2, 'b128': 4}.get(f['size'])
        if f['size'] in ('u8', 's8', 'u16', 's16'):
            mask = 0xff if '8' in f['size'] else 0xffff
            self.emit(f"if (!KILLED) gmem_store_bits({self.gaddr(ins)}, 0u, {self.r(f['v']) if f['v'] != 255 else '0u'}, 0x{mask:x}u);")
            return
        if words is None: raise Unsupported(f"stg {f['size']}")
        self.emit(f'{{ uint2 ga_ = {self.gaddr(ins)};'); self.indent += 1
        for i in range(words): self.emit(f"if (!KILLED) gmem_store(ga_, {4 * i}u, {self.r(f['v'] + i) if f['v'] != 255 else '0u'});")
        self.indent -= 1; self.emit('}')
    def op_red(self, ins):
        f = ins.f
        if f['size'] not in ('u32', 's32'): raise Unsupported(f"red {f['size']}")
        if not f['e']:
            ins.f['e'] = True
        op = f['op']
        if op not in ('add', 'min', 'max', 'and', 'or', 'xor'): raise Unsupported(f'red {op}')
        signed = f['size'] == 's32'
        self.emit(f"if (!KILLED) gmem_atomic_{op}{'_s' if signed and op in ('min', 'max') else ''}({self.gaddr(ins)}, {self.r(f['v'])});")

    def op_atom(self, ins):
        f = ins.f
        # L3 (batch-4): KIL is deferred (op_kil): a killed pixel keeps running, so, like RED and STG, its atomic must not
        # reach memory (on the source GPU the thread has ended). Only pixel shaders kill: other stages emit no guard (their
        # text stays as before). A result register of RZ still performs the atomic (setr drops the whole call for RZ).
        def ret(expr):
            if not ATOM_KILL_GUARD: return self.setr(f['d'], expr)
            g = 'if (!KILLED) ' if self.prog.stage == 'fragment' else ''
            if f['d'] == 255: return self.emit(f'{g}{expr};')
            self.regs.add(f['d']); self.emit(f"{g}r{f['d']} = {expr};")
        if 'op' not in f:   # atom.cas: compare b, new value b+1
            if f['size'] != 'b32': raise Unsupported(f"atom.cas {f['size']}")
            v = f['v']
            ret(f"gmem_atomic_cas({self.gaddr(ins)}, {self.r(v)}, {self.r(v + 1) if v != 255 else '0u'})"); return
        if f['size'] not in ('u32', 's32'): raise Unsupported(f"atom {f['size']}")
        op = f['op']
        fn = {'add': 'add', 'min': 'min', 'max': 'max', 'and': 'and', 'or': 'or', 'xor': 'xor', 'exch': 'exch', 'inc': 'inc', 'dec': 'dec'}.get(op)
        if fn is None: raise Unsupported(f'atom {op}')
        signed = f['size'] == 's32' and op in ('min', 'max')
        ret(f"gmem_atomic_ret_{fn}{'_s' if signed else ''}({self.gaddr(ins)}, {self.r(f['v'])})")

    # compute: shared-memory atomics, barriers
    def op_atoms(self, ins):
        f = ins.f; self.uses.add('smem')
        space, bank, reg, off = f['mem']
        base = f'({self.r(reg)} + {off & 0xffffffff}u)' if reg != 255 else f'{off & 0xffffffff}u'
        v = f['v']
        if f['op'] in ('cas', 'cast', 'cast spin'):
            if f['op'] != 'cas' or f['size'] != 'b32': raise Unsupported(f"atoms {f['op']} {f['size']}")
            return self.setr(f['d'], f"smem_cas({base}, {self.r(v)}, {self.r(v + 1) if v != 255 else '0u'})")
        if f['size'] not in ('u32', 's32'): raise Unsupported(f"atoms {f['size']}")
        op = f['op']
        signed = f['size'] == 's32' and op in ('min', 'max')
        if op not in ('add', 'min', 'max', 'inc', 'dec', 'and', 'or', 'xor', 'exch'): raise Unsupported(f'atoms {op}')
        self.setr(f['d'], f"smem_atomic_{op}{'_s' if signed else ''}({base}, {self.r(v)})")
    def op_bar(self, ins):
        f = ins.f
        if self.prog.stage != 'compute': raise Unsupported('bar outside compute')
        if f['mode'] != 'sync': raise Unsupported(f"bar {f['mode']}")
        # BAR.SYNC a, b: barrier a (immediate 0 only), thread count b (RZ/0 = all threads of the block)
        ops = (ins.word >> 43) & 3
        a, b = f['a'], f['b']
        if ops & 2:
            if a != 0: raise Unsupported(f'bar sync barrier {a}')
        elif a != 255: raise Unsupported('bar sync with register barrier id')
        if ops & 1:
            if b not in (0, self.prog.launch.get('threads', -1)): raise Unsupported(f'bar sync with partial thread count {b}')
        elif b != 255: raise Unsupported('bar sync with register thread count')
        if ins.guard != (7, False): raise Unsupported('predicated bar')
        self.uses.add('barrier')
        self.emit('GroupMemoryBarrierWithGroupSync();')
    def op_membar(self, ins):
        lvl = ins.f['level']
        self.emit('GroupMemoryBarrier();' if lvl == 'cta' else 'DeviceMemoryBarrier();')

    # vertex/fragment attributes
    def load_attr(self, ins, reg, off, words, d):
        if reg != 255 or ins.f.get('c', 255) != 255 or ins.f.get('p') or ins.f.get('o'):
            raise Unsupported('indexed/patch/output attribute load')
        for i in range(words):
            a = off + 4 * i
            self.setr(d + i if d != 255 else 255, self.attr_in(a))
    def attr_in(self, a):
        st = self.prog.stage
        if st == 'vertex':
            if 0x80 <= a < 0x280:
                g, c = (a - 0x80) // 16, (a // 4) & 3; self.vs_inputs.add(g)
                return f'va{g}.{"xyzw"[c]}'
            if a == 0x2f8: return 'iid'
            if a == 0x2fc: return 'vid'
            raise Unsupported(f'vertex input attribute {a:#x}')
        raise Unsupported(f'ld a[{a:#x}] in {st}')
    def store_attr(self, a, value):
        if self.prog.stage != 'vertex': raise Unsupported('attribute store outside vertex shader')
        if 0x70 <= a < 0x80: name = f'o.pos.{"xyzw"[(a - 0x70) // 4]}'
        elif 0x80 <= a < 0x280:
            g, c = (a - 0x80) // 16, (a // 4) & 3
            self.vs_outputs.setdefault(g, set()).add(c); name = f'o.g{g}.{"xyzw"[c]}'
        elif a == 0x6c: name = 'o.psize'; self.uses.add('psize')
        elif 0x2c0 <= a < 0x2e0:
            k = (a - 0x2c0) // 4; name = f'o.clip{k // 4}.{"xyzw"[k % 4]}'; self.uses.add(f'clip{k // 4}')
        elif a == 0x64: name = 'o.layer'; self.uses.add('layer')
        elif a == 0x68: name = 'o.viewport'; self.uses.add('viewport')
        else: raise Unsupported(f'vertex output attribute {a:#x}')
        if name.startswith(('o.layer', 'o.viewport')): self.emit(f'{name} = {value};')
        else: self.emit(f'{name} = asfloat({value});')
    def op_ipa(self, ins):
        f = ins.f
        if self.prog.stage != 'fragment': raise Unsupported('ipa outside fragment shader')
        _, _, _, a = f['mem']
        if f['c'] != 255 or f['p'] != (7, False): raise Unsupported('ipa with offset/sample predicate')
        mode, sample = f['mode'], f['sample']
        if sample: raise Unsupported(f'ipa {sample}')
        if a == 0x7c:
            e = 'rcp_w'
        elif 0x70 <= a < 0x7c:
            e = f'i.pos.{"xyz"[(a - 0x70) // 4]}'
        elif 0x80 <= a < 0x280:
            g, c = (a - 0x80) // 16, (a // 4) & 3
            interp = self.prog.ps_input_modes()[g][c]
            self.ps_inputs.setdefault(g, {})[c] = interp
            e = f'i.g{g}.{"xyzw"[c]}'
            if interp == 2 and mode != 'constant': e = f'({e} * rcp_w)'
        elif a == 0x3fc:
            e = 'asfloat(front_face_bits)'; self.uses.add('front_face')
        elif a in (0x2e0, 0x2e4):
            # Point sprite coordinate; D3D12 has no point sprites, the host expands points to quads
            # and supplies the coordinate in this interpolant (G2: a geometry shader from make_point_gs.py,
            # bound by host_gpu_render_draw.h boot_pt_apply).
            e = f'i.pointcoord.{"xy"[(a - 0x2e0) // 4]}'; self.uses.add('pointcoord')
        else: raise Unsupported(f'ipa attribute {a:#x}')
        if mode == '' : e = f"({e} * {self.rf(f['b'])})"
        elif mode not in ('pass', 'constant'): raise Unsupported(f'ipa mode {mode}')
        if f['sat']: e = f'saturate({e})'
        self.setrf(f['d'], e)
    def op_kil(self, ins):
        if ins.f['cc']: raise Unsupported('kil with cc')
        # Deferred: FXC mis-compiles assignments after a conditional discard inside the
        # dispatch loop. Output is discarded at FINISH; side-effect stores check killed_.
        self.uses.add('kill'); self.emit('killed_ = true;')

    # textures
    def tex_handle(self, idx):
        self.uses.add('tex'); self.tex_indices.add(idx); return f'tex_handle({idx}u)'
    def write_tex(self, ins, comps, dregs, value):
        """Write selected components of float4 'value' sequentially to registers."""
        self.emit(f'{{ float4 t_ = {value};'); self.indent += 1
        for reg, comp in zip(dregs, comps):
            if reg != 255: self.setr(reg, f'asuint(t_.{comp})')
        self.indent -= 1; self.emit('}')
    def sample_call(self, kind, dim, handle, coord, lod=None, dref=None, offset=None, array=None):
        self.uses.add('tex'); self.uses.add('tex_' + dim + ('_array' if array is not None else ''))
        args = [handle, coord]
        if array is not None: args.append(array)
        if dref is not None: args.append(dref)
        if lod is not None: args.append(lod)
        if offset is not None: args.append(offset)
        return f"tex_{kind}_{dim}{'a' if array is not None else ''}({', '.join(args)})"
    def op_texs(self, ins):
        f = ins.f; a, b = f['a'], f['b']
        # L1 (batch-4): bit 59 clear is TEXS.F16 (results packed as half2 in d and d2; nxport decoders/inst_table_data.rs:
        # 270-271, instructions/texture.rs:406-439), which texs_write would spread over four registers: unsupported until
        # implemented (no TOTK program uses it). Same for TLDS (bit 59) and TLD4S (bit 55 set).
        if TEX_STRICT and not bits(ins.word, 59, 1): raise Unsupported('texs.f16 (packed half results)')
        enc = bits(ins.word, 53, 4)
        h = self.tex_handle(f['tex'])
        F = self.rf
        if enc == 0: call = self.sample_call('level', '1d', h, F(a), lod='0.0f')
        elif enc == 1: call = self.sample_call('implicit', '2d', h, f'float2({F(a)}, {F(b)})')
        elif enc == 2: call = self.sample_call('level', '2d', h, f'float2({F(a)}, {F(b)})', lod='0.0f')
        elif enc == 3: call = self.sample_call('level', '2d', h, f'float2({F(a)}, {F(a + 1)})', lod=F(b))
        elif enc == 4: call = self.sample_call('cmp', '2d', h, f'float2({F(a)}, {F(a + 1)})', dref=F(b))
        elif enc == 5: call = self.sample_call('cmplevel', '2d', h, f'float2({F(a)}, {F(a + 1)})', dref=F(b), lod=F(b + 1))
        elif enc == 6: call = self.sample_call('cmplevel', '2d', h, f'float2({F(a)}, {F(a + 1)})', dref=F(b), lod='0.0f')
        elif enc == 7: call = self.sample_call('implicit', '2d', h, f'float2({F(a + 1)}, {F(b)})', array=self.r(a))
        elif enc == 8: call = self.sample_call('level', '2d', h, f'float2({F(a + 1)}, {F(b)})', array=self.r(a), lod='0.0f')
        elif enc == 9: call = self.sample_call('cmplevel', '2d', h, f'float2({F(a + 1)}, {F(b)})', array=self.r(a), dref=F(b + 1), lod='0.0f')
        elif enc == 10: call = self.sample_call('implicit', '3d', h, f'float3({F(a)}, {F(a + 1)}, {F(b)})')
        elif enc == 11: call = self.sample_call('level', '3d', h, f'float3({F(a)}, {F(a + 1)}, {F(b)})', lod='0.0f')
        elif enc == 12: call = self.sample_call('implicit', 'cube', h, f'float3({F(a)}, {F(a + 1)}, {F(b)})')
        elif enc == 13: call = self.sample_call('level', 'cube', h, f'float3({F(a)}, {F(a + 1)}, {F(b)})', lod=F(b + 1))   # TEXS CUBE.LL: coords (a, a+1, b), lod b+1
        else: raise Unsupported(f'texs encoding {enc}')
        self.texs_write(ins, call)
    def texs_write(self, ins, call):
        f = ins.f; comps = f['comp']; d, d2 = f['d'], f['d2']
        regs = [d, d + 1 if d != 255 else 255] + ([d2, d2 + 1 if d2 != 255 else 255] if len(comps) > 2 else [])
        self.write_tex(ins, comps, regs, call)
    def op_tlds(self, ins):
        f = ins.f; a, b = f['a'], f['b']; enc = bits(ins.word, 53, 4); h = self.tex_handle(f['tex']); R = self.r
        if TEX_STRICT and not bits(ins.word, 59, 1): raise Unsupported('tlds.f16 (packed half results)')   # L1 (op_texs)
        # L2 (batch-4): 2D array LZ (enc 8): nxport (instructions/texture.rs:368-373) reads x = Rb, y = Rb+1, layer = Ra,
        # this translator x = Ra+1, y = Rb. Unverified either way and unused by TOTK (0 of 31.8k programs): unsupported.
        if TEX_STRICT and enc == 8: raise Unsupported('tlds 2d array lz (coordinate order unverified)')
        if enc == 0: call = self.sample_call('fetch', '1d', h, f'int({R(a)})', lod='0')
        elif enc == 1: call = self.sample_call('fetch', '1d', h, f'int({R(a)})', lod=f'int({R(b)})')
        elif enc == 2: call = self.sample_call('fetch', '2d', h, f'int2({R(a)}, {R(b)})', lod='0')
        elif enc == 4: call = self.sample_call('fetch', '2d', h, f'int2({R(a)}, {R(a + 1)})', lod='0', offset=f'aoffi2({R(b)})')
        elif enc == 5: call = self.sample_call('fetch', '2d', h, f'int2({R(a)}, {R(a + 1)})', lod=f'int({R(b)})')
        elif enc == 7: call = self.sample_call('fetch', '3d', h, f'int3({R(a)}, {R(a + 1)}, {R(b)})', lod='0')
        elif enc == 8: call = self.sample_call('fetch', '2d', h, f'int2({R(a + 1)}, {R(b)})', array=R(a), lod='0')
        elif enc == 12: call = self.sample_call('fetch', '2d', h, f'int2({R(a)}, {R(a + 1)})', lod=f'int({R(b)})', offset=f'aoffi2({R(b + 1)})')
        else: raise Unsupported(f'tlds encoding {enc}')
        self.texs_write(ins, call)
    def tex_coords(self, ins, dim, array, base):
        n = {'t1d': 1, 't2d': 2, 't3d': 3, 'tcube': 3}[dim]
        reg = base; arr = None
        if array: arr = self.r(reg); reg += 1
        coords = [self.rf(reg + i) for i in range(n)]
        return (coords[0] if n == 1 else f'float{n}({", ".join(coords)})'), arr
    def reg_constant(self, reg, ins):
        """Value of `reg` before `ins` when an unpredicated MOV32I in the same basic block sets it
        (and nothing after it in the block writes it); None otherwise."""
        insns = self.prog.insns
        if not hasattr(self, '_leaders'):
            if any(i.name == 'brx' for i in insns): self._leaders = None
            else:
                lead = {insns[0].addr}
                for k, i in enumerate(insns):
                    if i.name in ('bra', 'ssy', 'pbk'): lead.add(i.f['target'])
                    if (i.name in CONTROL or i.name == 'kil') and k + 1 < len(insns): lead.add(insns[k + 1].addr)
                self._leaders = lead
            self._index = {i.addr: k for k, i in enumerate(insns)}
        if self._leaders is None: return None
        k = self._index[ins.addr]
        while insns[k].addr not in self._leaders:
            k -= 1; p = insns[k]
            if p.name in CONTROL or p.name == 'kil': return None
            dests = [p.f[n] for n in ('d', 'd2') if isinstance(p.f.get(n), int) and p.f[n] != 255]
            if p.name in SINGLE_DEST: span = 0
            elif p.name in ('tex', 'tld', 'txd', 'tld4', 'txq', 'tmml'): span = max(bin(p.f['mask']).count('1') - 1, 0)
            elif p.name in ('texs', 'tlds', 'tld4s'): span = 1
            else: span = 3                                 # other writers: up to four registers
            if not any(v <= reg <= v + span for v in dests): continue
            if p.name == 'mov32i' and p.f['d'] == reg and p.f['mask'] == 0xf and p.guard == (7, False) and isinstance(p.f['b'], int):
                return p.f['b'] & 0xffffffff
            return None
        return None
    def tex_offset(self, ins, reg, n):
        """TEX/TLD .AOFFI operand: n signed 4-bit texel offsets packed from bit 0 (nxport sass2spv texture.rs emit_tex)."""
        v = self.reg_constant(reg, ins)
        if v is None: return f'aoffi{n}({self.r(reg)})'
        comps = [((v >> (4 * i)) & 0xf) - (16 if (v >> (4 * i)) & 0x8 else 0) for i in range(n)]
        return str(comps[0]) if n == 1 else f'int{n}({", ".join(map(str, comps))})'
    def op_tex(self, ins):
        # Ra: array index (low 16 bits) then coordinates; Rb: LOD or bias, offsets (.AOFFI), reference (.DC)
        f = ins.f
        dim = f['dim']; coord, arr = self.tex_coords(ins, dim, f['array'], f['a'])
        meta = f['b']; lod = dref = offset = None; kind = 'implicit'
        if f['lod'] in ('ll', 'lb', 'lla', 'lba'):
            lod = self.rf(meta); meta += 1; kind = 'level' if f['lod'] in ('ll', 'lla') else 'bias'
        elif f['lod'] == 'lz': lod = '0.0f'; kind = 'level'
        if f['aoffi']:
            if dim == 'tcube': raise Unsupported('tex aoffi on a cube')
            offset = self.tex_offset(ins, meta, {'t1d': 1, 't2d': 2, 't3d': 3}[dim]); meta += 1
        if f['dc']:
            dref = self.rf(meta); meta += 1
            kind = 'cmp' if kind == 'implicit' else 'cmplevel' if kind == 'level' else None
            if kind is None: raise Unsupported('tex dc with bias')
        call = self.sample_call(kind, dim[1:] if dim != 'tcube' else 'cube', self.tex_handle(f['tex']), coord, lod=lod, dref=dref, offset=offset, array=arr)
        comps = [c for i, c in enumerate('xyzw') if f['mask'] >> i & 1]
        d = f['d']; self.write_tex(ins, comps, [d + i if d != 255 else 255 for i in range(len(comps))], call)
    def op_tld4(self, ins):
        f = ins.f
        if f['offs'] or f['ndv']: raise Unsupported('tld4 offsets')
        dim = f['dim']; coord, arr = self.tex_coords(ins, dim, f['array'], f['a'])
        dref = self.rf(f['b']) if f['dc'] else None
        call = self.sample_call('gather' + ('cmp' if dref else '') + f['comp'], dim[1:] if dim != 'tcube' else 'cube', self.tex_handle(f['tex']), coord, dref=dref, array=arr)
        comps = [c for i, c in enumerate('xyzw') if f['mask'] >> i & 1]
        d = f['d']; self.write_tex(ins, comps, [d + i if d != 255 else 255 for i in range(len(comps))], call)
    def op_tld4s(self, ins):
        f = ins.f
        if f['aoffi']: raise Unsupported('tld4s aoffi')
        if TEX_STRICT and bits(ins.word, 55, 1): raise Unsupported('tld4s.f16 (packed half results)')   # L1 (op_texs)
        a, b = f['a'], f['b']
        coord = f'float2({self.rf(a)}, {self.rf(b if not f["dc"] else a + 1)})'
        dref = self.rf(b) if f['dc'] else None
        call = self.sample_call('gather' + ('cmp' if dref else '') + f['comp'], '2d', self.tex_handle(f['tex']), coord, dref=dref)
        d, d2 = f['d'], f['d2']
        self.write_tex(ins, 'xyzw', [d, d + 1 if d != 255 else 255, d2, d2 + 1 if d2 != 255 else 255], call)
    def op_txq(self, ins):
        f = ins.f
        if f['query'] != 'dimension': raise Unsupported(f"txq {f['query']}")
        comps = [c for i, c in enumerate('xyzw') if f['mask'] >> i & 1]
        d = f['d']
        self.emit(f"{{ uint4 q_ = tex_query_dims({self.tex_handle(f['tex'])}, {self.r(f['a'])});"); self.indent += 1
        for i, c in enumerate(comps): self.setr(d + i if d != 255 else 255, f'q_.{c}')
        self.indent -= 1; self.emit('}')
    def op_tmml(self, ins):
        # TMML: level-of-detail query at the coordinates in Ra (array index first, dropped), results in 8.8 fixed point.
        # shd-S2 (batch-4): the result order is the inverse of GLSL textureQueryLod's: x = the computed LOD (signed,
        # unclamped), y = the level accessed (clamped to the texture's levels and the sampler's LOD range). nxport
        # crates/sass2spv/src/instructions/texture.rs:559-562 swaps the components ("inverse of GLSL's") and converts
        # LOD * 256 to s32; Mesa nouveau's TXLQ lowering (MIT) swaps them the same way, reading x as s16 and y as u16
        # (semantics only, from both). TOTK reads only y (3103 sites), which was the unclamped LOD here: it differs where
        # the LOD leaves the texture's mip range or a sampler LOD clamp applies. TOTK_SHADER_TMML_HW=0: x clamped,
        # y unclamped, both clamped at 0 (before). z/w (BA results) are not implemented.
        f = ins.f
        if f['mask'] & 0b1100: raise Unsupported('tmml ba results')
        if f['dim'] == 't3d' and f['array']: raise Unsupported('tmml 3d array')
        if self.prog.stage != 'fragment': raise Unsupported('tmml outside fragment')
        dim = f['dim']; n = {'t1d': 1, 't2d': 2, 't3d': 3, 'tcube': 3}[dim]
        base = f['a'] + (1 if f['array'] else 0)
        coords = [self.rf(base + i) for i in range(n)]
        coord = coords[0] if n == 1 else f'float{n}({", ".join(coords)})'
        kind = dim[1:] if dim != 'tcube' else 'cube'
        self.uses.add('tex'); self.uses.add('tex_' + kind + ('_array' if f['array'] else ''))
        call = f"tex_lod_{kind}{'a' if f['array'] else ''}({self.tex_handle(f['tex'])}, {coord})"
        d = f['d']
        self.emit(f'{{ float2 l_ = {call};'); self.indent += 1
        k = 0
        for i, c in enumerate('xy'):
            if f['mask'] >> i & 1:
                if not TMML_HW_ORDER: e = f'(uint)(max(l_.{c}, 0.0f) * 256.0f)'
                elif c == 'x': e = 'f2i_s32(l_.y * 256.0f)'   # tex_lod_*: .x clamped (accessed) LOD, .y unclamped
                else: e = 'f2i_u32(l_.x * 256.0f)'
                self.setr(d + k if d != 255 else 255, e); k += 1
        self.indent -= 1; self.emit('}')
    def op_txd(self, ins):
        # TXD (Mesa NAK nak_nir_lower_tex.c, SM50 txd): coordinates from Ra, then the array index (low 16 bits) in
        # the register after them; derivatives from Rb interleaved as (ds/dx, ds/dy, dt/dx, dt/dy).
        f = ins.f; F = self.rf
        if f['aoffi']: raise Unsupported('txd aoffi')
        dim = f['dim']
        if dim not in ('t1d', 't2d'): raise Unsupported(f'txd {dim}')
        n = 1 if dim == 't1d' else 2; a, b = f['a'], f['b']
        coord = F(a) if n == 1 else f'float2({F(a)}, {F(a + 1)})'
        if n == 1: ddx, ddy = F(b), F(b + 1)
        else: ddx, ddy = f'float2({F(b)}, {F(b + 2)})', f'float2({F(b + 1)}, {F(b + 3)})'
        h = self.tex_handle(f['tex']); kind = dim[1:]
        self.uses.add('tex_' + kind + ('_array' if f['array'] else ''))
        if f['array']: call = f'tex_grad_{kind}a({h}, {coord}, {self.r(a + n)}, {ddx}, {ddy})'
        else: call = f'tex_grad_{kind}({h}, {coord}, {ddx}, {ddy})'
        comps = [c for i, c in enumerate('xyzw') if f['mask'] >> i & 1]
        d = f['d']; self.write_tex(ins, comps, [d + i if d != 255 else 255 for i in range(len(comps))], call)
    def op_tld(self, ins):
        # TLD (Mesa NAK nak_nir_lower_tex.c, SM50 txf): integer texel coordinates from Ra (array index first, low 16
        # bits), then from Rb: LOD (.LL), packed 4-bit offsets (.AOFFI), sample index (.MS).
        f = ins.f; R = self.r
        if f['ms']: raise Unsupported('tld ms')
        if f['cl']: raise Unsupported('tld cl')
        dim = f['dim']
        if dim == 'tcube': raise Unsupported('tld cube')
        n = {'t1d': 1, 't2d': 2, 't3d': 3}[dim]
        if f['array'] and n == 3: raise Unsupported('tld 3d array')
        reg = f['a']; arr = None
        if f['array']: arr = R(reg); reg += 1
        coords = [f'int({R(reg + i)})' for i in range(n)]
        coord = coords[0] if n == 1 else f'int{n}({", ".join(coords)})'
        meta = f['b']; lod = '0'
        if f['lod'] == 'll': lod = f'int({R(meta)})'; meta += 1
        offset = None
        if f['aoffi']: offset = self.tex_offset(ins, meta, n); meta += 1
        call = self.sample_call('fetch', dim[1:], self.tex_handle(f['tex']), coord, lod=lod, offset=offset, array=arr)
        comps = [c for i, c in enumerate('xyzw') if f['mask'] >> i & 1]
        d = f['d']; self.write_tex(ins, comps, [d + i if d != 255 else 255 for i in range(len(comps))], call)

    # control (straight-line handling; structured flow handled by translate())
    def op_depbar(self, ins): pass
    def op_nop(self, ins): pass


def _flatten(atoms, word):
    from maxwell_isa import T
    for a in atoms:
        if isinstance(a, T):
            yield a
            for value, mask, sub in a.table:
                if word & mask == value:
                    yield from _flatten(sub, word); break
        else:
            yield a


CONTROL = {'bra', 'ssy', 'sync', 'pbk', 'brk', 'brx', 'exit'}
# Instructions whose destination is exactly one register (Emitter.reg_constant)
SINGLE_DEST = {'mov', 'mov32i', 'mufu', 'fadd', 'fadd32i', 'ffma', 'ffma32i', 'fmul', 'fmul32i', 'fmnmx', 'fset', 'iset',
               'iadd', 'iadd3', 'iadd32i', 'imad', 'imnmx', 'lop', 'lop3', 'lop32i', 'shl', 'shr', 'sel', 'i2f', 'f2i', 'f2f',
               'i2i', 'bfe', 'bfi', 'xmad', 'prmt', 'ipa', 's2r', 'flo', 'popc'}

def blocks_of(prog):
    """Split into basic blocks keyed by start address."""
    insns = prog.insns
    leaders = {insns[0].addr}
    if any(i.name == 'brx' for i in insns):
        # Indirect targets come from runtime data (jump tables in constant
        # buffers): every instruction is a potential target and a switch case.
        leaders.update(i.addr for i in insns)
    for i, ins in enumerate(insns):
        if ins.name in ('bra', 'ssy', 'pbk'): leaders.add(ins.f['target'])
        if ins.name in CONTROL or ins.name == 'kil':
            if i + 1 < len(insns): leaders.add(insns[i + 1].addr)
    order = [ins.addr for ins in insns]
    blocks = {}; cur = None
    for ins in insns:
        if ins.addr in leaders: cur = ins.addr; blocks[cur] = []
        blocks[cur].append(ins)
    for t in leaders:
        if t not in blocks: raise Unsupported(f'branch target {t:#x} outside program')
    return blocks, order


# Loop guard (nxport idea, MIT: crates/engine/src/metal/msl_specialize.rs guard_loops, LOOP_GUARD_LIMIT = 65536).
# The dispatch loop runs one basic block per iteration; a jump to a block at or below the current block's start
# address is a back edge, and every cycle of the control-flow graph contains one (forward jumps strictly increase
# pc_). Counting back edges per invocation therefore bounds the whole loop: at most (LOOP_CAP + 1) * blocks
# dispatches. LOOP_CAP (65536, assemble_tail) is the total number of loop iterations of all loops of one invocation,
# far above what a real shader runs; a translator or jump-table bug that cycles forever then ends at FINISH with
# the outputs computed so far instead of hanging the GPU (TDR, DXGI_ERROR_DEVICE_HUNG 887a0006). Programs whose
# jumps are all forward get no guard and their HLSL is unchanged.
def loop_guard(em, start, target=None):
    """Guard text placed before a jump out of the block at `start`; target None = computed at run time (pc_ set)."""
    if target is not None:
        if target > start: return ''
        em.uses.add('loopguard'); return 'if (++loops_ > LOOP_CAP) FINISH; '
    em.uses.add('loopguard'); return f'if (pc_ <= 0x{start:x}u && ++loops_ > LOOP_CAP) FINISH; '


def translate(code, sph, stage):
    """Static reconvergence first; a program whose indirect branches (BRX) are reached with
    different SSY/PBK states is retried with a per-invocation runtime stack. Each invocation
    runs as one thread here, so SSY/PBK push a target and SYNC/BRK pop and jump to it: exact
    without SIMT divergence. Programs the static pass accepts are emitted unchanged."""
    try:
        return _translate(code, sph, stage, dynamic=False)
    except Unsupported as e:
        if 'reconvergence' not in str(e): raise
        return _translate(code, sph, stage, dynamic=True)


def _translate(code, sph, stage, dynamic):
    prog = Program(code, sph, stage)
    em = Emitter(prog)
    blocks, order = blocks_of(prog)
    starts = sorted(blocks)
    # Static reconvergence stacks: entry state per block, propagated in program order.
    entry = {starts[0]: ((), ())}
    straight = len(starts) == 1
    brx_state = None
    body = []
    em.in_cf = not straight
    em.indent = 1 if straight else 3
    for bi, start in enumerate(starts):
        if start not in entry: entry[start] = None
        state = entry[start]
        if state is None and brx_state is not None: state = entry[start] = brx_state
        if dynamic: state = ((), ())
        if state is None: raise Unsupported(f'block {start:#x} unreachable or has unknown reconvergence state')
        ssy, pbk = state
        em.out = []
        ins_list = blocks[start]
        nxt = starts[bi + 1] if bi + 1 < len(starts) else None
        fell = True
        def propagate(target, st):
            if dynamic: return
            if target in entry and entry[target] is not None and entry[target] != st:
                raise Unsupported(f'conflicting reconvergence state at {target:#x}')
            entry[target] = st
        for ins in ins_list:
            n = ins.name
            guarded = ins.guard != (7, False)
            cond = None
            if guarded:
                pred, neg = ins.guard
                if pred == 7 and neg: continue
                cond = f'{"!" if neg else ""}{em.p(pred)}'
            if n == 'ssy':
                if dynamic: em.uses.add('rstack'); em.emit(f"if (ssy_n_ < 16u) ssy_s_[ssy_n_++] = 0x{ins.f['target']:x}u;")
                else: ssy = ssy + (ins.f['target'],)
                continue
            if n == 'brx':
                if ins.f['cc'] or ins.f['lmt']: raise Unsupported('brx with cc/lmt')
                if not dynamic and brx_state not in (None, (ssy, pbk)): raise Unsupported('brx sites with different reconvergence state')
                brx_state = (ssy, pbk)
                # target = address of the next instruction + register + immediate (table entries are program offsets).
                # A target on a bundle's scheduling word (32-byte aligned) executes from the bundle's first
                # instruction, as static branch targets do (maxwell_isa decode): jump tables commonly hold them.
                jump = (f"{{ uint brx_ = {ins.addr + 8}u + {em.r(ins.f['a'])} + {ins.f['off'] & 0xffffffff}u; "
                        f"pc_ = (brx_ & 31u) == 0u ? brx_ + 8u : brx_; }} {loop_guard(em, start)}NEXT;")
                if cond: em.emit(f'if ({cond}) {{ {jump} }}')
                else: em.emit(jump); fell = False; break
                continue
            if n == 'pbk':
                if dynamic: em.uses.add('rstack'); em.emit(f"if (pbk_n_ < 16u) pbk_s_[pbk_n_++] = 0x{ins.f['target']:x}u;")
                else: pbk = pbk + (ins.f['target'],)
                continue
            if n in ('bra', 'sync', 'brk', 'exit'):
                if ins.f.get('cc'): cond = (cond + ' && ' if cond else '') + em.cc_test(ins.f['cc'])
                if n == 'bra': target, st = ins.f['target'], (ssy, pbk)
                elif dynamic and n in ('sync', 'brk'):
                    em.uses.add('rstack')
                    stack = 'ssy' if n == 'sync' else 'pbk'
                    jump = f'if ({stack}_n_ == 0u) FINISH; pc_ = {stack}_s_[--{stack}_n_]; {loop_guard(em, start)}NEXT;'
                    if cond: em.emit(f'if ({cond}) {{ {jump} }}')
                    else: em.emit(jump); fell = False; break
                    continue
                elif n == 'sync':
                    if not ssy: raise Unsupported('sync without ssy')
                    target, st = ssy[-1], (ssy[:-1], pbk)
                elif n == 'brk':
                    if not pbk: raise Unsupported('brk without pbk')
                    target, st = pbk[-1], (ssy, pbk[:-1])
                else: target, st = None, None
                if target is not None:
                    propagate(target, st)
                    jump = f'{loop_guard(em, start, target)}pc_ = 0x{target:x}u; NEXT;' if not straight else None
                    if straight: raise Unsupported('branch in straight-line program')
                else:
                    jump = 'FINISH;'
                if cond:
                    em.emit(f'if ({cond}) {{ {jump} }}')
                else:
                    em.emit(jump); fell = False; break
                continue
            em.op(ins)
        if fell:
            if nxt is None:
                raise Unsupported('falls off the end of the program')
            propagate(nxt, (ssy, pbk))
            if not straight: em.emit(f'pc_ = 0x{nxt:x}u; NEXT;')
        body.append((start, em.out))
    return em, prog, body
