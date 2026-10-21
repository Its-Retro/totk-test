"""Maxwell (SM5x) instruction decoder for the opcodes used by the game's shaders.

The specification below is transcribed from envytools envydis/gm107.c (MIT,
tools/envytools). Each instruction is a sequence of atoms; the same spec
yields (a) named fields for the HLSL emitter and (b) text identical to envydis,
which test_maxwell_isa.py checks against envydis on every game instruction.
Opcodes not listed here raise Unsupported: translation stops, never guesses.
"""

class Unsupported(ValueError):
    pass

def bits(w, pos, n):
    return (w >> pos) & ((1 << n) - 1)

def sext(v, n):
    return v - (1 << n) if v & (1 << (n - 1)) else v

# ---------------------------------------------------------------- atom types
class N:            # literal token
    def __init__(s, text): s.text = text
class ON:           # flag token printed when bit set
    def __init__(s, bit, name, key=None): s.bit, s.name, s.key = bit, name, key or name
class ZN:           # flag token printed when bit clear
    def __init__(s, bit, name, key=None): s.bit, s.name, s.key = bit, name, key or name
class T:            # enumerated sub-table; entries are (value, mask, [atoms|str])
    def __init__(s, table, key): s.table, s.key = table, key
class R:            # 8-bit register, 255 = RZ
    def __init__(s, pos, key): s.pos, s.key = pos, key
class P:            # 3-bit predicate, 7 = PT
    def __init__(s, pos, key, xorend=0): s.pos, s.key, s.xorend = pos, key, xorend
class PN:           # optional 'not' + predicate
    def __init__(s, notbit, pos, key): s.notbit, s.pos, s.key = notbit, pos, key
class I:            # immediate: fields [(pos,len),...] low-to-high, signed?, left shift
    def __init__(s, fields, key, signed=False, shl=0, pcrel=False):
        s.fields, s.key, s.signed, s.shl, s.pcrel = fields, key, signed, shl, pcrel
class SR:           # special register (s2r)
    def __init__(s, pos, key): s.pos, s.key = pos, key
class M:            # memory operand: space, bank field, base reg pos, offset I
    def __init__(s, space, key, bank=None, reg=None, off=None):
        s.space, s.key, s.bank, s.reg, s.off = space, key, bank, reg, off

def imm_value(a, w):
    v = 0; n = 0
    for pos, length in a.fields:
        v |= bits(w, pos, length) << n; n += length
    if a.signed: v = sext(v, n)
    return v << a.shl if a.shl and v >= 0 else (v * (1 << a.shl) if a.shl else v)

def fmt_hex(v):
    return f'-0x{-v:x}' if v < 0 else f'0x{v:x}'

def T_(*entries):
    return [(v, m, [N(x) if isinstance(x, str) else x for x in atoms]) for v, m, *atoms in entries]

# ---------------------------------------------------------------- sub-tables
CMP16 = ['f','lt','eq','le','gt','ne','ge','num','nan','ltu','equ','leu','gtu','neu','geu','t']
CC32 = CMP16[:15] + [None,'off','lo','sff','ls','hi','sft','hs','oft','csm_ta','csm_tr','csm_mx',
                     'fcsm_ta','fcsm_tr','fcsm_mx','rle','rgt']
def enum(pos, names, width=None):
    width = width or max(1, (len(names) - 1).bit_length())
    return [(i << pos, ((1 << width) - 1) << pos, *([n] if n else [])) for i, n in enumerate(names)]

f0f8_0 = T_(*enum(0, CC32, 5))
f0f0_0 = T_((0, 1 << 29), (1 << 29, 1 << 29, 'le'))
eff0_0 = T_(*enum(47, ['b32', 'b64', 'b96', 'b128']))
ef90_0 = T_(*enum(44, [None, 'il', 'is', 'isl']))
ef90sz = T_(*[(i << 48, 7 << 48, n) for i, n in enumerate(['u8','s8','u16','s16','b32','b64'])])
ef58sz = T_(*[(i << 48, 7 << 48, n) for i, n in enumerate(['u8','s8','u16','s16','b32','b64','b128'])])
ef40_0 = T_(*enum(44, [None, 'lu', 'ci', 'cv']))
ef50_0 = T_(*enum(44, [None, 'cg', 'cs', 'wt']))
ef10_0 = T_(*enum(30, ['idx', 'up', 'down', 'bfly']))
ef10_1 = T_((0, 3 << 28, R(20, 'b'), R(39, 'c')),
            (1 << 28, 3 << 28, I([(20, 5)], 'b'), R(39, 'c')),
            (2 << 28, 3 << 28, R(20, 'b'), I([(34, 13)], 'c')),
            (3 << 28, 3 << 28, I([(20, 5)], 'b'), I([(34, 13)], 'c')))
eed8_0 = T_(*enum(46, [None, 'cg', 'cs', 'wt']))
eed0_0 = T_(*enum(46, [None, 'cg', 'ci', 'cv']))
eed0sz = T_(*[(i << 48, 7 << 48, n) for i, n in enumerate(['u8','s8','u16','s16','b32','b64','b128'])],
            (7 << 48, 7 << 48, 'u', 'b128'))
ed00_0 = T_(*[(i << 52, 0xf << 52, n) for i, n in enumerate(['add', 'min', 'max', 'inc', 'dec', 'and', 'or', 'xor', 'exch'])],
            (0xa << 52, 0xf << 52, 'safeadd'))
ed00sz = T_((0 << 49, 7 << 49, 'u32'), (1 << 49, 7 << 49, 's32'), (2 << 49, 7 << 49, 'u64'),
            (3 << 49, 7 << 49, 'ftz', 'rn', 'f32'), (4 << 49, 7 << 49, 'u128'), (5 << 49, 7 << 49, 's64'))
ebf8_0 = T_(*enum(23, ['add', 'min', 'max', 'inc', 'dec', 'and', 'or', 'xor']))
ebf8sz = T_((0 << 20, 7 << 20, 'u32'), (1 << 20, 7 << 20, 's32'), (2 << 20, 7 << 20, 'u64'),
            (3 << 20, 7 << 20, 'ftz', 'rn', 'f32'), (4 << 20, 7 << 20, 'u128'), (5 << 20, 7 << 20, 's64'))
e000_0 = T_(*enum(54, ['pass', None, 'constant', 'sc']))
e000_1 = T_((0, 3 << 52), (1 << 52, 3 << 52, 'centroid'), (2 << 52, 3 << 52, 'offset'))
df60_0 = T_(*enum(29, ['t1d', 't2d', 't3d', 'tcube']))
df50_0 = T_((0x01 << 22, 0x3f << 22, 'dimension'), (0x02 << 22, 0x3f << 22, 'texture_type'),
            (0x05 << 22, 0x3f << 22, 'sample_pos'), (0x10 << 22, 0x3f << 22, 'filter'),
            (0x12 << 22, 0x3f << 22, 'lod'), (0x14 << 22, 0x3f << 22, 'wrap'),
            (0x16 << 22, 0x3f << 22, 'border_color'))
df00_0 = T_(*enum(52, ['r', 'g', 'b', 'a']))
dc38_0 = T_((0, 1 << 55, 'lz'), (1 << 55, 1 << 55, 'll'))
_d = 0xf << 53
d200_0 = T_((0x0 << 53, _d, 'lz'), (0x1 << 53, _d, 'll'), (0x2 << 53, _d, 'lz'), (0x4 << 53, _d, 'lz', 'aoffi'),
            (0x5 << 53, _d, 'll'), (0x6 << 53, _d, 'lz', 'mz'), (0x7 << 53, _d, 'lz'), (0x8 << 53, _d, 'lz'),
            (0xc << 53, _d, 'll', 'aoffi'))
d200_1 = T_((0x0 << 53, _d, 't1d'), (0x1 << 53, _d, 't1d'), (0x2 << 53, _d, 't2d'), (0x4 << 53, _d, 't2d'),
            (0x5 << 53, _d, 't2d'), (0x6 << 53, _d, 't2d'), (0x7 << 53, _d, 't3d'), (0x8 << 53, _d, 'a2d'),
            (0xc << 53, _d, 't2d'))
_s = 7 << 50; _z = 0xff << 28
d200_2 = T_(*[(i << 50 | _z, _s | _z, n) for i, n in enumerate(['r', 'g', 'b', 'a', 'rg', 'ra', 'ga', 'ba'])],
            *[(i << 50, _s, n) for i, n in enumerate(['rgb', 'rga', 'rba', 'gba', 'rgba'])])
d000_0 = T_((0x0 << 53, _d, 'lz'), (0x1 << 53, _d), (0x2 << 53, _d, 'lz'), (0x3 << 53, _d, 'll'),
            (0x4 << 53, _d, 'dc'), (0x5 << 53, _d, 'll', 'dc'), (0x6 << 53, _d, 'lz', 'dc'), (0x7 << 53, _d),
            (0x8 << 53, _d, 'lz'), (0x9 << 53, _d, 'lz', 'dc'), (0xa << 53, _d), (0xb << 53, _d, 'lz'),
            (0xc << 53, _d), (0xd << 53, _d, 'll'))
d000_1 = T_(*[(i << 53, _d, n) for i, n in enumerate(
    ['t1d', 't2d', 't2d', 't2d', 't2d', 't2d', 't2d', 'a2d', 'a2d', 'a2d', 't3d', 't3d', 'tcube', 'tcube'])])
c838_0 = T_(*enum(56, ['r', 'g', 'b', 'a']))
c838_1 = T_((0, 3 << 54), (1 << 54, 3 << 54, 'aoffi'), (2 << 54, 3 << 54, 'ptp'))
c038_0 = T_((0, 7 << 55), (1 << 55, 7 << 55, 'lz'), (2 << 55, 7 << 55, 'lb'), (3 << 55, 7 << 55, 'll'),
            (6 << 55, 7 << 55, 'lba'), (7 << 55, 7 << 55, 'lla'))
s5cf8_1 = T_((0, 3 << 37), (2 << 37, 3 << 37, 'u64'), (3 << 37, 3 << 37, 's64'))
s5cf8_0 = T_(*enum(48, [None, 'hi', 'x', 'xhi']))
s5cf0_0 = T_((0, 1 << 40, 'pr'), (1 << 40, 1 << 40, 'cc'))
s5cf0_1 = T_(*enum(41, [None, 'b1', 'b2', 'b3']))
_i = 0x13 << 8
s5ce0_0 = T_((0x000, _i, 'u8'), (0x100, _i, 'u16'), (0x200, _i, 'u32'),
             (0x1000, _i, 's8'), (0x1100, _i, 's16'), (0x1200, _i, 's32'))
_j = 0x2c << 8
s5ce0_1 = T_((0x000, _j, 'u8'), (0x400, _j, 'u16'), (0x800, _j, 'u32'),
             (0x2000, _j, 's8'), (0x2400, _j, 's16'), (0x2800, _j, 's32'))
s5cc0_0 = T_(*enum(37, [None, 'rs', 'ls']))
s5cc0_1 = T_(*enum(35, [None, 'h0', 'h1']))
s5cc0_2 = T_(*enum(33, [None, 'h0', 'h1']))
s5cc0_3 = T_(*enum(31, [None, 'h0', 'h1']))
s5cb8_0 = T_((1 << 8, 3 << 8, 'f16'), (2 << 8, 3 << 8, 'f32'), (3 << 8, 3 << 8, 'f64'))
s5cb8_1 = T_((0x000, _j, 'u8'), (0x400, _j, 'u16'), (0x800, _j, 'u32'), (0xc00, _j, 'u64'),
             (0x2000, _j, 's8'), (0x2400, _j, 's16'), (0x2800, _j, 's32'), (0x2c00, _j, 's64'))
s5cb8_2 = T_(*enum(39, [None, 'rm', 'rp', 'rz']))
s5cb0_2 = T_((0x000, _i, 'u8'), (0x100, _i, 'u16'), (0x200, _i, 'u32'), (0x300, _i, 'u64'),
             (0x1000, _i, 's8'), (0x1100, _i, 's16'), (0x1200, _i, 's32'), (0x1300, _i, 'u64'))
s5cb0_0 = T_((1 << 10, 3 << 10, 'f16'), (2 << 10, 3 << 10, 'f32'), (3 << 10, 3 << 10, 'f64'))
s5cb0_1 = T_(*enum(39, [None, 'floor', 'ceil', 'trunc']))
_k = 0xb << 39
s5ca8_0 = T_((0, _k), (3 << 39, _k, 'pass'), (8 << 39, _k, 'round'), (9 << 39, _k, 'floor'),
             (0xa << 39, _k, 'ceil'), (0xb << 39, _k, 'trunc'))
s5c90_0 = T_((0, 1 << 39, 'sincos'), (1 << 39, 1 << 39, 'ex2'))
s5c68_0 = T_(*enum(44, [None, 'ftz', 'fmz']))
s5c68_1 = T_(*enum(41, [None, 'd2', 'd4', 'd8', 'm8', 'm4', 'm2'], 3))
s5c40_0 = T_(*enum(41, ['and', 'or', 'xor', 'pass_b']))
s5c40_1 = T_(*enum(44, [None, 't', 'z', 'nz']))
s5c30_0 = T_((0, 1 << 48, 'u32'), (1 << 48, 1 << 48))
s5c20_0 = T_(*enum(43, [None, 'xlo', 'xmed', 'xhi']))
s5be0_0 = T_(*enum(36, [None, 't', 'z', 'nz']))
s5bb0_0 = T_(*enum(48, CMP16))
s5bb0_1 = T_(*enum(45, ['and', 'or', 'xor']))
s5b70_0 = T_(*enum(50, [None, 'rm', 'rp', 'rz']))
s5b60_0 = T_(*enum(49, ['f', 'lt', 'eq', 'le', 'gt', 'ne', 'ge', 't']))
s5b00_0 = T_((0, 3 << 48), (1 << 48, 3 << 48, 's16', 'u16'), (2 << 48, 3 << 48, 'u16', 's16'),
             (3 << 48, 3 << 48, 's16', 's16'))
s5b00_1 = T_(*enum(50, [None, 'clo', 'chi', 'csfu', 'cbcc'], 3))
s5100_0 = T_(*enum(50, [None, 'clo', 'chi', 'csfu']))
s5980_0 = T_(*enum(53, [None, 'ftz', 'fmz']))
s5980_1 = T_(*enum(51, [None, 'rm', 'rp', 'rz']))
s5700_0 = T_((0, 1 << 54, 'ud'), (1 << 54, 1 << 54))
_v = (1 << 48) | (1 << 38)
s5700_1 = T_((0, _v, 'u8'), (1 << 38, _v, 'u16'), (1 << 48, _v, 's8'), (_v, _v, 's16'))
s5700_3 = T_(*enum(51, ['mrg_16h', 'mrg_16l', 'mrg_8b0', 'mrg_8b2', 'acc', 'min', 'max', None]))
s5f00_3 = T_(*enum(36, [None, 'b1', 'b2', 'b3']))
_c = (1 << 47) | (3 << 43)
s50f0_0 = T_((0, _c, 'f'), (1 << 43, _c, 'lt'), (2 << 43, _c, 'eq'), (3 << 43, _c, 'le'),
             (1 << 47, _c, 'gt'), ((1 << 47) | (1 << 43), _c, 'ne'), ((1 << 47) | (2 << 43), _c, 'ge'),
             (_c, _c, 't'))
s50f0_1_const = T_((0, 1 << 49, I([(20, 16)], 'b')), (1 << 49, 1 << 49, I([(20, 16)], 'b')))
s50f0_1_reg_8 = T_(*enum(28, ['b0', 'b1', 'b2', 'b3']))
_w = (1 << 49) | (1 << 30); _x = (1 << 49) | (7 << 28)
s50f0_1_size = T_((0, _w, 'u8', T(s50f0_1_reg_8, 'bsel')), (1 << 49, _w, 's8', T(s50f0_1_reg_8, 'bsel')),
                  (4 << 28, _x, 'u16', 'h0'), ((1 << 49) | (4 << 28), _x, 's16', 'h0'),
                  (5 << 28, _x, 'u16', 'h1'), ((1 << 49) | (5 << 28), _x, 's16', 'h1'),
                  (6 << 28, _x, 'u32'), ((1 << 49) | (6 << 28), _x, 's32'))
s50f0_1 = T_((0, 1 << 50, T(s50f0_1_const, 'bconst')), (1 << 50, 1 << 50, T(s50f0_1_size, 'bsize'), R(20, 'b')))
s50d8_0 = T_(*enum(48, ['all', 'any', 'eq']))
s50b0_0 = T_(*enum(8, CC32, 5))
s5090_0 = T_(*enum(24, ['and', 'or', 'xor']))
s5080_0 = T_(*enum(20, ['cos', 'sin', 'ex2', 'lg2', 'rcp', 'rsq', 'rcp64h', 'rsq64h', 'sqrt'], 4))
s0400_0 = T_(*enum(53, ['and', 'or', 'xor', 'pass_b']))
# compute-only forms (envydis gm107.c tabee00_0/ee00sz/ec00_0/ec00sz/eef0sz/f0a8_0/f0a8_1/ef98_0/ef98_1)
ee00_0 = T_((0 << 53, 3 << 53, 'cast'), (1 << 53, 3 << 53, 'cast', 'spin'), (2 << 53, 3 << 53, 'cas'))
ee00sz = T_((0, 1 << 52, 'b32'), (1 << 52, 1 << 52, 'b64'))
eef0sz = T_((0, 1 << 49, 'b32'), (1 << 49, 1 << 49, 'b64'))
ec00_0 = T_(*[(i << 52, 0xf << 52, n) for i, n in enumerate(['add', 'min', 'max', 'inc', 'dec', 'and', 'or', 'xor', 'exch'])])
ec00sz = T_(*[(i << 28, 3 << 28, n) for i, n in enumerate(['u32', 's32', 'u64', 's64'])])
_b = 0x9b << 32
f0a8_0 = T_((0x02 << 32, _b, 'red', 'popc'), (0x03 << 32, _b, 'scan'), (0x0a << 32, _b, 'red', 'and'),
            (0x12 << 32, _b, 'red', 'or'), (0x80 << 32, _b, 'sync'), (0x81 << 32, _b, 'arrive'))
f0a8_1 = T_((0 << 43, 3 << 43, R(8, 'a'), R(20, 'b')), (1 << 43, 3 << 43, R(8, 'a'), I([(20, 12)], 'b')),
            (2 << 43, 3 << 43, I([(8, 8)], 'a'), R(20, 'b')), (3 << 43, 3 << 43, I([(8, 8)], 'a'), I([(20, 12)], 'b')))
ef98_0 = T_(*enum(8, ['cta', 'gl', 'sys', 'vc']))
ef98_1 = T_((0, 3), (1, 3, 'ivalld'), (2, 3, 'ivallt'), (3, 3, 'ivalltd'))

# ---------------------------------------------------------------- operands
R00, R08, R20, R28, R39 = R(0, 'd'), R(8, 'a'), R(20, 'b'), R(28, 'd2'), R(39, 'c')
C34 = M('c', 'b', bank=(34, 5), off=I([(20, 14)], 'off', signed=True, shl=2))
C34C = M('c', 'c', bank=(34, 5), off=I([(20, 14)], 'off', signed=True, shl=2))     # cbuf as 3rd source
C36R = M('c', 'mem', bank=(36, 5), reg=8, off=I([(20, 16)], 'off', signed=True))
S20 = I([(20, 19), (56, 1)], 'b', signed=True)
F20 = I([(20, 19), (56, 1)], 'b', shl=12)
D20 = I([(20, 19), (56, 1)], 'b', shl=44)
U32 = I([(20, 32)], 'b')
S32 = I([(20, 32)], 'b', signed=True)
BTARG = I([(20, 24)], 'target', signed=True, pcrel=True)
PRED39 = PN(42, 39, 'p')

# Each entry: (value, mask, name, [atoms after name]); guard predicate is implicit.
FADD = lambda b: [ON(44, 'ftz'), ON(50, 'sat'), T(s5cb8_2, 'rnd'), ON(47, 'cc'), R00, ON(48, 'neg', 'neg_a'),
                  ON(46, 'abs', 'abs_a'), R08, ON(45, 'neg', 'neg_b'), ON(49, 'abs', 'abs_b'), b]
FMUL = lambda b: [T(s5c68_0, 'fmz'), T(s5c68_1, 'scale'), T(s5cb8_2, 'rnd'), ON(50, 'sat'), ON(47, 'cc'), R00,
                  R08, ON(48, 'neg', 'neg_b'), b]
FMNMX = lambda b: [ON(44, 'ftz'), ON(47, 'cc'), R00, ON(48, 'neg', 'neg_a'), ON(46, 'abs', 'abs_a'), R08,
                   ON(45, 'neg', 'neg_b'), ON(49, 'abs', 'abs_b'), b, PRED39]
FSETP = lambda b: [T(s5bb0_0, 'cmp'), ON(47, 'ftz'), T(s5bb0_1, 'bop'), P(3, 'pd'), P(0, 'pd2'),
                   ON(43, 'neg', 'neg_a'), ON(7, 'abs', 'abs_a'), R08, ON(6, 'neg', 'neg_b'),
                   ON(44, 'abs', 'abs_b'), b, PRED39]
FSET = lambda b: [ON(52, 'bf'), T(s5bb0_0, 'cmp'), T(s5bb0_1, 'bop'), ON(55, 'ftz'), ON(47, 'cc'), R00,
                  ON(43, 'neg', 'neg_a'), ON(54, 'abs', 'abs_a'), R08, ON(53, 'neg', 'neg_b'),
                  ON(44, 'abs', 'abs_b'), b, PRED39]
FFMA = lambda b, c: [T(s5980_0, 'fmz'), T(s5980_1, 'rnd'), ON(50, 'sat'), ON(47, 'cc'), R00, R08,
                     ON(48, 'neg', 'neg_b'), b, ON(49, 'neg', 'neg_c'), c]
FCMP = lambda b, c: [T(s5bb0_0, 'cmp'), ON(47, 'ftz'), R00, R08, b, c]
MOV = lambda b: [R00, b, I([(39, 4)], 'mask')]
SEL = lambda b: [R00, R08, b, PRED39]
I2F = lambda b: [T(s5cb8_0, 'dfmt'), T(s5cb8_1, 'sfmt'), T(s5cb8_2, 'rnd'), ON(47, 'cc'), R00, ON(45, 'neg'),
                 ON(49, 'abs'), T(s5cf0_1, 'bsel'), b]
F2I = lambda b: [ON(44, 'ftz'), T(s5cb0_2, 'dfmt'), T(s5cb0_0, 'sfmt'), T(s5cb0_1, 'rnd'), ON(47, 'cc'), R00,
                 ON(45, 'neg'), ON(49, 'abs'), b]
F2F = lambda b: [ON(44, 'ftz'), T(s5cb0_0, 'sfmt'), T(s5cb8_0, 'dfmt'), T(s5ca8_0, 'rnd'), ON(50, 'sat'),
                 ON(47, 'cc'), R00, ON(45, 'neg'), ON(49, 'abs'), b]
I2I = lambda b: [T(s5ce0_0, 'dfmt'), T(s5ce0_1, 'sfmt'), ON(50, 'sat'), ON(47, 'cc'), R00, T(s5cf0_1, 'bsel'),
                 ON(45, 'neg'), ON(49, 'abs'), b]
IADD = lambda b: [ON(50, 'sat'), ON(43, 'x'), ON(47, 'cc'), R00, ON(49, 'neg', 'neg_a'), R08,
                  ON(48, 'neg', 'neg_b'), b]
ISCADD = lambda b: [ON(47, 'cc'), R00, ON(49, 'neg', 'neg_a'), R08, ON(48, 'neg', 'neg_b'), b,
                    I([(39, 5)], 'shift')]
ISETP = lambda b: [T(s5b60_0, 'cmp'), T(s5c30_0, 'u32'), ON(43, 'x'), T(s5bb0_1, 'bop'), P(3, 'pd'),
                   P(0, 'pd2'), R08, b, PRED39]
ISET = lambda b: [ON(44, 'bf'), T(s5b60_0, 'cmp'), T(s5c30_0, 'u32'), ON(43, 'x'), T(s5bb0_1, 'bop'),
                  ON(47, 'cc'), R00, R08, b, PRED39]
ICMP = lambda b, c: [T(s5b60_0, 'cmp'), T(s5c30_0, 'u32'), R00, R08, b, c]
IMNMX = lambda b: [T(s5c30_0, 'u32'), T(s5c20_0, 'xmode'), ON(47, 'cc'), R00, R08, b, PRED39]
LOP = lambda b: [T(s5c40_0, 'lop'), ON(43, 'x'), T(s5c40_1, 'pop'), P(48, 'pd'), ON(47, 'cc'), R00,
                 ON(39, 'inv', 'inv_a'), R08, ON(40, 'inv', 'inv_b'), b]
SHL = lambda b: [ON(39, 'w'), ON(43, 'x'), ON(47, 'cc'), R00, R08, b]
SHR = lambda b: [T(s5c30_0, 'u32'), ON(39, 'w'), ON(44, 'x'), ON(40, 'brev'), ON(47, 'cc'), R00, R08, b]
FLO = lambda b: [T(s5c30_0, 'u32'), ON(41, 'sh'), ON(47, 'cc'), R00, ON(40, 'inv'), b]
BFE = lambda b: [T(s5c30_0, 'u32'), ON(40, 'brev'), ON(47, 'cc'), R00, R08, b]
RRO = lambda b: [T(s5c90_0, 'op'), R00, ON(45, 'neg'), ON(49, 'abs'), b]
R2P = lambda b: [T(s5cf0_0, 'dst'), T(s5cf0_1, 'bsel'), R08, b]
DADD = lambda b: [T(s5cb8_2, 'rnd'), ON(47, 'cc'), R00, ON(48, 'neg', 'neg_a'), ON(46, 'abs', 'abs_a'), R08,
                  ON(45, 'neg', 'neg_b'), ON(49, 'abs', 'abs_b'), b]
DMUL = lambda b: [T(s5cb8_2, 'rnd'), ON(47, 'cc'), R00, R08, ON(48, 'neg', 'neg_b'), b]
DFMA = lambda b, c: [T(s5b70_0, 'rnd'), ON(47, 'cc'), R00, R08, ON(48, 'neg', 'neg_b'), b, ON(49, 'neg', 'neg_c'), c]
XMAD_R = [T(s5b00_0, 'signs'), ON(36, 'psl'), ON(37, 'mrg'), T(s5b00_1, 'mode'), ON(38, 'x'), ON(47, 'cc'), R00,
          ON(53, 'h1', 'h1_a'), R08, ON(35, 'h1', 'h1_b'), R20, R39]
XMAD_I = [T(s5b00_0, 'signs'), ON(36, 'psl'), ON(37, 'mrg'), T(s5b00_1, 'mode'), ON(38, 'x'), ON(47, 'cc'), R00,
          ON(53, 'h1', 'h1_a'), R08, I([(20, 16)], 'b'), R39]
XMAD_C = [T(s5b00_0, 'signs'), ON(55, 'psl'), ON(56, 'mrg'), T(s5100_0, 'mode'), ON(54, 'x'), ON(47, 'cc'), R00,
          ON(53, 'h1', 'h1_a'), R08, ON(52, 'h1', 'h1_b'), C34, R39]
XMAD_RC = [T(s5b00_0, 'signs'), T(s5100_0, 'mode'), ON(54, 'x'), ON(47, 'cc'), R00, ON(53, 'h1', 'h1_a'), R08,
           ON(52, 'h1', 'h1_b'), R(39, 'b'), C34C]
TEXDIM = [ON(28, 'array'), T(df60_0, 'dim'), I([(31, 4)], 'mask')]
VSET = [T(s50f0_0, 'cmp'), T(s5700_1, 'asize'), T(s5700_3, 'vmode'), ON(47, 'cc'), R00, T(s5f00_3, 'asel'), R08,
        T(s50f0_1, 'bopnd'), R39]
VMNMX = [T(s5700_0, 'dsigned'), T(s5700_1, 'asize'), T(s5700_3, 'vmode'), ON(47, 'cc'), R00, T(s5f00_3, 'asel'),
         R08, T(s50f0_1, 'bopnd'), R39]

# Packed half-precision arithmetic (SM53, Tegra X1: HADD2 / HMUL2 / HFMA2). envydis (gm107) does not
# know these. Opcodes and field positions match nxport's decoder table form by form (MIT,
# crates/sass2spv/src/decoders/inst_decoders.rs + inst_table_data.rs, generated from Ryujinx's
# InstDecoders.cs, MIT); checked on random words of every form by play-work/p96/provenance/verify_half_ops.py. One
# difference: HFMA2_32I takes swizzle 53 / precision 55 like nxport's HADD2_32I and HMUL2_32I, and
# neg_c 52 (no .SAT), where nxport reuses the HFMA2 immediate-form positions 47 / 57 / 51, which
# overlap the 32-bit immediate and the opcode. The token text is this decoder's own (there is no envydis text to match). Operand swizzles and the result
# merge mode: see maxwell_hlsl.Emitter.half_op.
HSWZ = ['h1_h0', 'f32', 'h0_h0', 'h1_h1']
HMRG = ['h1_h0', 'f32', 'mrg_h0', 'mrg_h1']
HPREC = [None, 'ftz', 'fmz', 'prec3']
hswz = lambda pos: T_(*enum(pos, HSWZ))
hmrg = lambda pos: T_(*enum(pos, HMRG))
hprec = lambda pos: T_(*enum(pos, HPREC))
# 9+1+9-bit packed half immediate of the HADD2/HMUL2/HFMA2 imm forms: low half bits 6..14 at
# insn 20..28, low sign at 29, high half bits 6..14 at 30..38; high sign at bit 56 (key neg_hi).
HIMM = I([(20, 9), (29, 1), (30, 9)], 'b')
HALF_OPS = [
    (0x5d10 << 48, 0xfff8 << 48, 'hadd2', [T(hmrg(49), 'merge'), ON(39, 'ftz'), ON(32, 'sat'), R00, ON(43, 'neg', 'neg_a'), ON(44, 'abs', 'abs_a'), R08,
                                           T(hswz(47), 'swz_a'), ON(31, 'neg', 'neg_b'), ON(30, 'abs', 'abs_b'), R20, T(hswz(28), 'swz_b')]),
    (0x7a80 << 48, 0xfe80 << 48, 'hadd2', [T(hmrg(49), 'merge'), ON(39, 'ftz'), ON(52, 'sat'), R00, ON(43, 'neg', 'neg_a'), ON(44, 'abs', 'abs_a'), R08,
                                           T(hswz(47), 'swz_a'), ON(56, 'neg', 'neg_b'), ON(54, 'abs', 'abs_b'), C34]),
    (0x7a00 << 48, 0xfe80 << 48, 'hadd2', [T(hmrg(49), 'merge'), ON(39, 'ftz'), ON(52, 'sat'), R00, ON(43, 'neg', 'neg_a'), ON(44, 'abs', 'abs_a'), R08,
                                           T(hswz(47), 'swz_a'), ON(56, 'neg_hi'), HIMM]),
    (0x2c00 << 48, 0xfe00 << 48, 'hadd2_32i', [ON(55, 'ftz'), ON(52, 'sat'), R00, ON(56, 'neg', 'neg_a'), R08, T(hswz(53), 'swz_a'), U32]),
    (0x5d08 << 48, 0xfff8 << 48, 'hmul2', [T(hmrg(49), 'merge'), T(hprec(39), 'prec'), ON(32, 'sat'), R00, ON(44, 'abs', 'abs_a'), R08,
                                           T(hswz(47), 'swz_a'), ON(31, 'neg', 'neg_b'), ON(30, 'abs', 'abs_b'), R20, T(hswz(28), 'swz_b')]),
    (0x7880 << 48, 0xfe80 << 48, 'hmul2', [T(hmrg(49), 'merge'), T(hprec(39), 'prec'), ON(52, 'sat'), R00, ON(43, 'neg', 'neg_a'), ON(44, 'abs', 'abs_a'), R08,
                                           T(hswz(47), 'swz_a'), ON(54, 'abs', 'abs_b'), C34]),
    (0x7800 << 48, 0xfe80 << 48, 'hmul2', [T(hmrg(49), 'merge'), T(hprec(39), 'prec'), ON(52, 'sat'), R00, ON(43, 'neg', 'neg_a'), ON(44, 'abs', 'abs_a'), R08,
                                           T(hswz(47), 'swz_a'), ON(56, 'neg_hi'), HIMM]),
    (0x2a00 << 48, 0xfe00 << 48, 'hmul2_32i', [T(hprec(55), 'prec'), ON(52, 'sat'), R00, R08, T(hswz(53), 'swz_a'), U32]),
    (0x5d00 << 48, 0xfff8 << 48, 'hfma2', [T(hmrg(49), 'merge'), T(hprec(37), 'prec'), ON(32, 'sat'), R00, R08, T(hswz(47), 'swz_a'),
                                           ON(31, 'neg', 'neg_b'), R20, T(hswz(28), 'swz_b'), ON(30, 'neg', 'neg_c'), R39, T(hswz(35), 'swz_c')]),
    (0x6080 << 48, 0xf880 << 48, 'hfma2', [T(hmrg(49), 'merge'), T(hprec(57), 'prec'), ON(52, 'sat'), R00, R08, T(hswz(47), 'swz_a'),
                                           ON(56, 'neg', 'neg_b'), R(39, 'b'), T(hswz(53), 'swz_b'), ON(51, 'neg', 'neg_c'), C34C]),
    (0x7080 << 48, 0xf880 << 48, 'hfma2', [T(hmrg(49), 'merge'), T(hprec(57), 'prec'), ON(52, 'sat'), R00, R08, T(hswz(47), 'swz_a'),
                                           ON(56, 'neg', 'neg_b'), C34, ON(51, 'neg', 'neg_c'), R39, T(hswz(53), 'swz_c')]),
    (0x7000 << 48, 0xf880 << 48, 'hfma2', [T(hmrg(49), 'merge'), T(hprec(57), 'prec'), ON(52, 'sat'), R00, R08, T(hswz(47), 'swz_a'),
                                           ON(56, 'neg_hi'), HIMM, ON(51, 'neg', 'neg_c'), R39, T(hswz(53), 'swz_c')]),
    (0x2800 << 48, 0xfe00 << 48, 'hfma2_32i', [T(hprec(55), 'prec'), R00, R08, T(hswz(53), 'swz_a'), U32, ON(52, 'neg', 'neg_c'), R(0, 'c')]),
]

ROOT = HALF_OPS + [
    (0xf0f8 << 48, 0xfff8 << 48, 'sync', [T(f0f8_0, 'cc')]),
    (0xf0f0 << 48, 0xfff8 << 48, 'depbar', [T(f0f0_0, 'le'), I([(26, 3)], 'sb'), I([(20, 6)], 'cnt'), I([(0, 6)], 'mask')]),
    (0xf0c8 << 48, 0xfff8 << 48, 'mov', [R00, SR(20, 'sr')]),
    (0xf0a8 << 48, 0xfff8 << 48, 'bar', [T(f0a8_0, 'mode'), T(f0a8_1, 'ops'), PRED39]),
    (0xef98 << 48, 0xfff8 << 48, 'membar', [T(ef98_0, 'level'), T(ef98_1, 'ival')]),
    (0xef58 << 48, 0xfff8 << 48, 'st', [T(ef58sz, 'size'), M('s', 'mem', reg=8, off=I([(20, 24)], 'off', signed=True)), R(0, 'v')]),
    (0xef48 << 48, 0xfff8 << 48, 'ld', [ON(44, 'u'), T(ef58sz, 'size'), R00, M('s', 'mem', reg=8, off=I([(20, 24)], 'off', signed=True))]),
    (0xeef0 << 48, 0xfff0 << 48, 'atom', [N('cas'), ON(48, 'e'), T(eef0sz, 'size'), R00, M('g', 'mem', reg=8, off=I([(28, 20)], 'off', signed=True)), R(20, 'v')]),
    (0xee00 << 48, 0xff80 << 48, 'atoms', [T(ee00_0, 'op'), T(ee00sz, 'size'), R00, M('s', 'mem', reg=8, off=I([(30, 22)], 'off', signed=True, shl=2)), R(20, 'v')]),
    (0xec00 << 48, 0xff00 << 48, 'atoms', [T(ec00_0, 'op'), T(ec00sz, 'size'), R00, M('s', 'mem', reg=8, off=I([(30, 22)], 'off', signed=True, shl=2)), R(20, 'v')]),
    (0xeff0 << 48, 0xfff8 << 48, 'st', [ON(31, 'p'), T(eff0_0, 'size'), M('a', 'mem', reg=8, off=I([(20, 10)], 'off')), R(0, 'v'), R39]),
    (0xefd8 << 48, 0xfff8 << 48, 'ld', [ON(32, 'o'), ON(31, 'p'), T(eff0_0, 'size'), R00, M('a', 'mem', reg=8, off=I([(20, 10)], 'off')), R39]),
    (0xef90 << 48, 0xfff8 << 48, 'ld', [T(ef90_0, 'cache'), T(ef90sz, 'size'), R00, C36R]),
    (0xef50 << 48, 0xfff8 << 48, 'st', [T(ef50_0, 'cache'), T(ef58sz, 'size'), M('l', 'mem', reg=8, off=I([(20, 24)], 'off', signed=True)), R(0, 'v')]),
    (0xef40 << 48, 0xfff8 << 48, 'ld', [T(ef40_0, 'cache'), T(ef58sz, 'size'), R00, M('l', 'mem', reg=8, off=I([(20, 24)], 'off', signed=True))]),
    (0xef10 << 48, 0xfff8 << 48, 'shfl', [T(ef10_0, 'mode'), P(48, 'pd'), R00, R08, T(ef10_1, 'bc')]),
    (0xeed8 << 48, 0xfff8 << 48, 'stg', [ON(45, 'e'), T(eed8_0, 'cache'), T(ef58sz, 'size'), M('ncg', 'mem', reg=8, off=I([(20, 24)], 'off', signed=True)), R(0, 'v')]),
    (0xeed0 << 48, 0xfff8 << 48, 'ldg', [ON(45, 'e'), T(eed0_0, 'cache'), T(eed0sz, 'size'), R00, M('ncg', 'mem', reg=8, off=I([(20, 24)], 'off', signed=True))]),
    (0xed00 << 48, 0xff00 << 48, 'atom', [T(ed00_0, 'op'), ON(48, 'e'), T(ed00sz, 'size'), R00, M('g', 'mem', reg=8, off=I([(28, 20)], 'off', signed=True)), R(20, 'v')]),
    (0xebf8 << 48, 0xfff8 << 48, 'red', [T(ebf8_0, 'op'), ON(48, 'e'), T(ebf8sz, 'size'), M('g', 'mem', reg=8, off=I([(28, 20)], 'off', signed=True)), R(0, 'v')]),
    (0xe340 << 48, 0xfff0 << 48, 'brk', [T(f0f8_0, 'cc')]),
    (0xe330 << 48, 0xfff0 << 48, 'kil', [T(f0f8_0, 'cc')]),
    (0xe300 << 48, 0xfff0 << 48, 'exit', [ON(5, 'keeprefcount'), T(f0f8_0, 'cc')]),
    (0xe2a0 << 48, (0xfff0 << 48) | 0x20, 'pbk', [BTARG]),
    (0xe290 << 48, (0xfff0 << 48) | 0x20, 'ssy', [BTARG]),
    (0xe250 << 48, (0xfff0 << 48) | 0x20, 'brx', [ON(6, 'lmt'), T(f0f8_0, 'cc'), R08, I([(20, 24)], 'off', signed=True)]),
    (0xe240 << 48, (0xfff0 << 48) | 0x20, 'bra', [ON(7, 'u'), ON(6, 'lmt'), T(f0f8_0, 'cc'), BTARG]),
    (0xe0 << 56 | 0xff00, (0xff << 56) | (1 << 38) | 0xff00, 'ipa',
        [T(e000_0, 'mode'), T(e000_1, 'sample'), ON(51, 'sat'), R00, M('a', 'mem', off=I([(28, 10)], 'off')), R20, R39, PN(50, 47, 'p')]),
    (0xdf50 << 48, 0xfff8 << 48, 'txq', [N('b'), ON(49, 'nodep'), R00, R08, T(df50_0, 'query'), I([(36, 13)], 'tex'), I([(31, 4)], 'mask')]),
    (0xdf58 << 48, 0xfff8 << 48, 'tmml', [ON(49, 'nodep'), ON(35, 'ndv'), R00, R08, R20, I([(36, 13)], 'tex')] + TEXDIM),
    (0xdf48 << 48, 0xfff8 << 48, 'txq', [ON(49, 'nodep'), R00, R08, T(df50_0, 'query'), I([(36, 13)], 'tex'), I([(31, 4)], 'mask')]),
    (0xdf00 << 48, 0xff40 << 48, 'tld4s', [T(df00_0, 'comp'), ON(51, 'aoffi'), ON(50, 'dc'), ON(49, 'nodep'), R28, R00, R08, R20, I([(36, 13)], 'tex')]),
    (0xde38 << 48, 0xfffc << 48, 'txd', [ON(35, 'aoffi'), ON(49, 'nodep'), R00, R08, R20, I([(36, 13)], 'tex')] + TEXDIM),
    (0xdc38 << 48, 0xff38 << 48, 'tld', [T(dc38_0, 'lod'), ON(35, 'aoffi'), ON(50, 'ms'), ON(54, 'cl'), ON(49, 'nodep'), R00, R08, R20, I([(36, 13)], 'tex')] + TEXDIM),
    (0xd200 << 48, 0xf600 << 48, 'tlds', [T(d200_0, 'lod'), ON(49, 'nodep'), R28, R00, R08, R20, I([(36, 13)], 'tex'), T(d200_1, 'target'), T(d200_2, 'comp')]),
    (0xd000 << 48, 0xf600 << 48, 'texs', [T(d000_0, 'lod'), ON(49, 'nodep'), R28, R00, R08, R20, I([(36, 13)], 'tex'), T(d000_1, 'target'), T(d200_2, 'comp')]),
    (0xc838 << 48, 0xfc38 << 48, 'tld4', [T(c838_0, 'comp'), T(c838_1, 'offs'), ON(50, 'dc'), ON(35, 'ndv'), ON(49, 'nodep'), R00, R08, R20, I([(36, 13)], 'tex')] + TEXDIM),
    (0xc038 << 48, 0xfc38 << 48, 'tex', [T(c038_0, 'lod'), ON(54, 'aoffi'), ON(50, 'dc'), ON(49, 'nodep'), R00, R08, R20, I([(36, 13)], 'tex')] + TEXDIM),
    (0x5cf8 << 48, 0xfff8 << 48, 'shf', [N('r'), ON(50, 'w'), T(s5cf8_1, 'type'), T(s5cf8_0, 'hix'), ON(47, 'cc'), R00, R08, R20, R39]),
    (0x5cf0 << 48, 0xfff8 << 48, 'r2p', R2P(R20)),
    (0x5ce0 << 48, 0xfff8 << 48, 'i2i', I2I(R20)),
    (0x5cc0 << 48, 0xfff0 << 48, 'iadd3', [T(s5cc0_0, 'shift'), ON(48, 'x'), ON(47, 'cc'), R00, ON(51, 'neg', 'neg_a'), T(s5cc0_1, 'ha'), R08,
                                           ON(50, 'neg', 'neg_b'), T(s5cc0_2, 'hb'), R20, ON(49, 'neg', 'neg_c'), T(s5cc0_3, 'hc'), R39]),
    (0x5cb8 << 48, 0xfff8 << 48, 'i2f', I2F(R20)),
    (0x5cb0 << 48, 0xfff8 << 48, 'f2i', F2I(R20)),
    (0x5ca8 << 48, 0xfff8 << 48, 'f2f', F2F(R20)),
    (0x5ca0 << 48, 0xfff8 << 48, 'sel', SEL(R20)),
    (0x5c98 << 48, 0xfff8 << 48, 'mov', MOV(R20)),
    (0x5c90 << 48, 0xfff8 << 48, 'rro', RRO(R20)),
    (0x5c80 << 48, 0xfff8 << 48, 'dmul', DMUL(R20)),
    (0x5c70 << 48, 0xfff8 << 48, 'dadd', DADD(R20)),
    (0x5c68 << 48, 0xfff8 << 48, 'fmul', FMUL(R20)),
    (0x5c60 << 48, 0xfff8 << 48, 'fmnmx', FMNMX(R20)),
    (0x5c58 << 48, 0xfff8 << 48, 'fadd', FADD(R20)),
    (0x5c48 << 48, 0xfff8 << 48, 'shl', SHL(R20)),
    (0x5c40 << 48, 0xfff8 << 48, 'lop', LOP(R20)),
    (0x5c30 << 48, 0xfff8 << 48, 'flo', FLO(R20)),
    (0x5c28 << 48, 0xfff8 << 48, 'shr', SHR(R20)),
    (0x5c20 << 48, 0xfff8 << 48, 'imnmx', IMNMX(R20)),
    (0x5c18 << 48, 0xfff8 << 48, 'iscadd', ISCADD(R20)),
    (0x5c10 << 48, 0xfff8 << 48, 'iadd', IADD(R20)),
    (0x5c08 << 48, 0xfff8 << 48, 'popc', [R00, ON(40, 'inv'), R20]),
    (0x5c00 << 48, 0xfff8 << 48, 'bfe', BFE(R20)),
    (0x5bf8 << 48, 0xfff8 << 48, 'shf', [N('l'), ON(50, 'w'), T(s5cf8_1, 'type'), T(s5cf8_0, 'hix'), ON(47, 'cc'), R00, R08, R20, R39]),
    (0x5bf0 << 48, 0xfff8 << 48, 'bfi', [ON(47, 'cc'), R00, R08, R20, R39]),
    (0x5be0 << 48, 0xfff8 << 48, 'lop3', [N('lut'), ON(38, 'x'), T(s5be0_0, 'pop'), P(48, 'pd'), ON(47, 'cc'), R00, R08, R20, R39, I([(28, 8)], 'lut')]),
    (0x5bd0 << 48, 0xfff8 << 48, 'lea', [ON(46, 'x'), P(48, 'pd'), ON(47, 'cc'), R00, ON(45, 'neg', 'neg_a'), R08, R20, I([(39, 5)], 'shift')]),
    (0x5bb0 << 48, 0xfff0 << 48, 'fsetp', FSETP(R20)),
    (0x5ba0 << 48, 0xfff0 << 48, 'fcmp', FCMP(R20, R39)),
    (0x5b70 << 48, 0xfff0 << 48, 'dfma', DFMA(R20, R39)),
    (0x5b60 << 48, 0xfff0 << 48, 'isetp', ISETP(R20)),
    (0x5b50 << 48, 0xfff0 << 48, 'iset', ISET(R20)),
    (0x5b40 << 48, 0xfff0 << 48, 'icmp', ICMP(R20, R39)),
    (0x5b00 << 48, 0xffc0 << 48, 'xmad', XMAD_R),
    (0x5980 << 48, 0xff80 << 48, 'ffma', FFMA(R20, R39)),
    (0x5800 << 48, 0xff00 << 48, 'fset', FSET(R20)),
    (0x53f0 << 48, 0xfff8 << 48, 'bfi', [ON(47, 'cc'), R00, R08, R(39, 'b'), C34C]),
    (0x53a0 << 48, 0xfff0 << 48, 'fcmp', FCMP(R(39, 'b'), C34C)),
    (0x5340 << 48, 0xfff0 << 48, 'icmp', ICMP(R(39, 'b'), C34C)),
    (0x5180 << 48, 0xff80 << 48, 'ffma', FFMA(R(39, 'b'), C34C)),
    (0x5100 << 48, 0xff80 << 48, 'xmad', XMAD_RC),
    (0x50f8 << 48, 0xfff8 << 48, 'fswzadd', [ON(44, 'ftz'), T(s5cb8_2, 'rnd'), ON(38, 'ndv'), ON(47, 'cc'), R00, R08, R20, I([(28, 8)], 'swz')]),
    (0x50d8 << 48, 0xfff8 << 48, 'vote', [T(s50d8_0, 'mode'), R00, P(45, 'pd'), PRED39]),
    (0x50b0 << 48, 0xfff8 << 48, 'nop', [ON(13, 'trig'), T(s50b0_0, 'cc'), I([(20, 16)], 'imm')]),
    (0x50a0 << 48, 0xfff8 << 48, 'csetp', [T(s50b0_0, 'cc'), T(s5bb0_1, 'bop'), P(3, 'pd'), P(0, 'pd2'), N('cc'), PRED39]),
    (0x5090 << 48, 0xfff8 << 48, 'psetp', [T(s5090_0, 'bop2'), T(s5bb0_1, 'bop'), P(3, 'pd'), P(0, 'pd2'), PN(15, 12, 'pa'), PN(32, 29, 'pb'), PRED39]),
    (0x5088 << 48, 0xfff8 << 48, 'pset', [ON(44, 'bf'), T(s5090_0, 'bop2'), T(s5bb0_1, 'bop'), ON(47, 'cc'), R00, PN(15, 12, 'pa'), PN(32, 29, 'pb'), PRED39]),
    (0x5080 << 48, 0xfff8 << 48, 'mufu', [T(s5080_0, 'op'), ON(50, 'sat'), R00, ON(48, 'neg'), ON(46, 'abs'), R08]),
    (0x4e00 << 48, 0xfe00 << 48, 'xmad', XMAD_C),
    (0x4ce0 << 48, 0xfff8 << 48, 'i2i', I2I(C34)),
    (0x4cc0 << 48, 0xfff0 << 48, 'iadd3', [ON(48, 'x'), ON(47, 'cc'), R00, ON(51, 'neg', 'neg_a'), R08, ON(50, 'neg', 'neg_b'), C34, ON(49, 'neg', 'neg_c'), R39]),
    (0x4cb8 << 48, 0xfff8 << 48, 'i2f', I2F(C34)),
    (0x4cb0 << 48, 0xfff8 << 48, 'f2i', F2I(C34)),
    (0x4ca8 << 48, 0xfff8 << 48, 'f2f', F2F(C34)),
    (0x4ca0 << 48, 0xfff8 << 48, 'sel', SEL(C34)),
    (0x4c98 << 48, 0xfff8 << 48, 'mov', MOV(C34)),
    (0x4c90 << 48, 0xfff8 << 48, 'rro', RRO(C34)),
    (0x4c80 << 48, 0xfff8 << 48, 'dmul', DMUL(C34)),
    (0x4c70 << 48, 0xfff8 << 48, 'dadd', DADD(C34)),
    (0x4c68 << 48, 0xfff8 << 48, 'fmul', FMUL(C34)),
    (0x4c60 << 48, 0xfff8 << 48, 'fmnmx', FMNMX(C34)),
    (0x4c58 << 48, 0xfff8 << 48, 'fadd', FADD(C34)),
    (0x4c48 << 48, 0xfff8 << 48, 'shl', SHL(C34)),
    (0x4c40 << 48, 0xfff8 << 48, 'lop', LOP(C34)),
    (0x4c30 << 48, 0xfff8 << 48, 'flo', FLO(C34)),
    (0x4c28 << 48, 0xfff8 << 48, 'shr', SHR(C34)),
    (0x4c20 << 48, 0xfff8 << 48, 'imnmx', IMNMX(C34)),
    (0x4c18 << 48, 0xfff8 << 48, 'iscadd', ISCADD(C34)),
    (0x4c10 << 48, 0xfff8 << 48, 'iadd', IADD(C34)),
    (0x4c08 << 48, 0xfff8 << 48, 'popc', [R00, ON(40, 'inv'), C34]),
    (0x4c00 << 48, 0xfff8 << 48, 'bfe', BFE(C34)),
    (0x4bf0 << 48, 0xfff8 << 48, 'bfi', [ON(47, 'cc'), R00, R08, C34, R39]),
    (0x4bd0 << 48, 0xfff0 << 48, 'lea', [ON(46, 'x'), P(48, 'pd'), ON(47, 'cc'), R00, ON(45, 'neg', 'neg_a'), R08, C34, I([(39, 5)], 'shift')]),
    (0x4bb0 << 48, 0xfff0 << 48, 'fsetp', FSETP(C34)),
    (0x4ba0 << 48, 0xfff0 << 48, 'fcmp', FCMP(C34, R39)),
    (0x4b70 << 48, 0xfff0 << 48, 'dfma', DFMA(C34, R39)),
    (0x4b60 << 48, 0xfff0 << 48, 'isetp', ISETP(C34)),
    (0x4b50 << 48, 0xfff0 << 48, 'iset', ISET(C34)),
    (0x4b40 << 48, 0xfff0 << 48, 'icmp', ICMP(C34, R39)),
    (0x4980 << 48, 0xff80 << 48, 'ffma', FFMA(C34, R39)),
    (0x4800 << 48, 0xfe00 << 48, 'fset', FSET(C34)),
    (0x4000 << 48, 0xfe00 << 48, 'vset', VSET),
    (0x3c00 << 48, 0xfc00 << 48, 'lop3', [N('lut'), ON(47, 'cc'), R00, R08, S20, I([(48, 8)], 'lut')]),
    (0x3a00 << 48, 0xfe00 << 48, 'vmnmx', VMNMX),
    (0x38f8 << 48, 0xfef8 << 48, 'shf', [N('r'), ON(50, 'w'), T(s5cf8_1, 'type'), T(s5cf8_0, 'hix'), ON(47, 'cc'), R00, R08, I([(20, 6)], 'b'), R39]),
    (0x38f0 << 48, 0xfef8 << 48, 'r2p', R2P(S20)),
    (0x38e0 << 48, 0xfef8 << 48, 'i2i', I2I(S20)),
    (0x38c0 << 48, 0xfef0 << 48, 'iadd3', [ON(48, 'x'), ON(47, 'cc'), R00, ON(51, 'neg', 'neg_a'), R08, ON(50, 'neg', 'neg_b'), S20, ON(49, 'neg', 'neg_c'), R39]),
    (0x38b8 << 48, 0xfef8 << 48, 'i2f', I2F(S20)),
    (0x38b0 << 48, 0xfef8 << 48, 'f2i', F2I(F20)),
    (0x38a8 << 48, 0xfef8 << 48, 'f2f', F2F(F20)),
    (0x38a0 << 48, 0xfef8 << 48, 'sel', SEL(S20)),
    (0x3898 << 48, 0xfef8 << 48, 'mov', MOV(S20)),
    (0x3890 << 48, 0xfef8 << 48, 'rro', RRO(F20)),
    (0x3880 << 48, 0xfef8 << 48, 'dmul', DMUL(D20)),
    (0x3870 << 48, 0xfef8 << 48, 'dadd', DADD(D20)),
    (0x3868 << 48, 0xfef8 << 48, 'fmul', FMUL(F20)),
    (0x3860 << 48, 0xfef8 << 48, 'fmnmx', FMNMX(F20)),
    (0x3858 << 48, 0xfef8 << 48, 'fadd', FADD(F20)),
    (0x3848 << 48, 0xfef8 << 48, 'shl', SHL(S20)),
    (0x3840 << 48, 0xfef8 << 48, 'lop', LOP(S20)),
    (0x3830 << 48, 0xfef8 << 48, 'flo', FLO(S20)),
    (0x3828 << 48, 0xfef8 << 48, 'shr', SHR(S20)),
    (0x3820 << 48, 0xfef8 << 48, 'imnmx', IMNMX(S20)),
    (0x3818 << 48, 0xfef8 << 48, 'iscadd', ISCADD(S20)),
    (0x3810 << 48, 0xfef8 << 48, 'iadd', IADD(S20)),
    (0x3808 << 48, 0xfef8 << 48, 'popc', [R00, ON(40, 'inv'), S20]),
    (0x3800 << 48, 0xfef8 << 48, 'bfe', BFE(S20)),
    (0x36f8 << 48, 0xfef8 << 48, 'shf', [N('l'), ON(50, 'w'), T(s5cf8_1, 'type'), T(s5cf8_0, 'hix'), ON(47, 'cc'), R00, R08, S20, R39]),
    (0x36f0 << 48, 0xfef8 << 48, 'bfi', [ON(47, 'cc'), R00, R08, S20, R39]),
    (0x36d0 << 48, 0xfef8 << 48, 'lea', [ON(46, 'x'), P(48, 'pd'), ON(47, 'cc'), R00, ON(45, 'neg', 'neg_a'), R08, S20, I([(39, 5)], 'shift')]),
    (0x36b0 << 48, 0xfef0 << 48, 'fsetp', FSETP(F20)),
    (0x36a0 << 48, 0xfef0 << 48, 'fcmp', FCMP(F20, R39)),
    (0x3670 << 48, 0xfef0 << 48, 'dfma', DFMA(D20, R39)),
    (0x3660 << 48, 0xfef0 << 48, 'isetp', ISETP(S20)),
    (0x3650 << 48, 0xfef0 << 48, 'iset', ISET(S20)),
    (0x3640 << 48, 0xfef0 << 48, 'icmp', ICMP(S20, R39)),
    (0x3600 << 48, 0xfec0 << 48, 'xmad', XMAD_I),
    (0x3280 << 48, 0xfe80 << 48, 'ffma', FFMA(F20, R39)),
    (0x3000 << 48, 0xfe00 << 48, 'fset', FSET(F20)),
    (0x1e00 << 48, 0xff00 << 48, 'fmul32i', [T(s5980_0, 'fmz'), ON(55, 'sat'), ON(52, 'cc'), R00, R08, U32]),
    (0x1c00 << 48, 0xfe80 << 48, 'iadd32i', [ON(54, 'sat'), ON(53, 'x'), ON(52, 'cc'), R00, ON(56, 'neg', 'neg_a'), R08, S32]),
    (0x0800 << 48, 0xfc00 << 48, 'fadd32i', [ON(55, 'ftz'), ON(52, 'cc'), R00, ON(56, 'neg', 'neg_a'), ON(54, 'abs', 'abs_a'), R08,
                                             ON(53, 'neg', 'neg_b'), ON(57, 'abs', 'abs_b'), U32]),
    (0x0400 << 48, 0xfc00 << 48, 'lop32i', [T(s0400_0, 'lop'), ON(57, 'x'), ON(52, 'cc'), R00, ON(55, 'inv', 'inv_a'), R08, ON(56, 'inv', 'inv_b'), U32]),
    (0x0200 << 48, 0xfe00 << 48, 'lop3', [N('lut'), ON(56, 'x'), ON(47, 'cc'), R00, R08, C34, R39, I([(48, 8)], 'lut')]),
    (0x0100 << 48, 0xfff0 << 48, 'mov32i', [R00, U32, I([(12, 4)], 'mask')]),
]

SYSREGS = {0x00: 'laneid', 0x12: 'y_direction', 0x14: 'shader_type', 0x20: 'tid', 0x21: 'tidx', 0x22: 'tidy',
           0x23: 'tidz', 0x24: 'cta_param', 0x25: 'ctaidx', 0x26: 'ctaidy', 0x27: 'ctaidz', 0x38: 'eqmask', 0x39: 'ltmask',
           0x3a: 'lemask', 0x3b: 'gtmask', 0x3c: 'gemask', 0x50: 'clocklo', 0x51: 'clockhi',
           0x10: 'prim_type', 0x11: 'invocation_id', 0x1d: 'invocation_info'}

NO_GUARD = {'ssy', 'pbk'}

class Insn:
    __slots__ = ('addr', 'word', 'name', 'opcode', 'f', 'tokens', 'guard')
    def __repr__(s): return f'{s.addr:#06x}: {" ".join(s.tokens)}'

def _walk(atoms, w, addr, f, out):
    for a in atoms:
        if isinstance(a, N): out.append(a.text)
        elif isinstance(a, ON):
            v = bool(w >> a.bit & 1); f[a.key] = v
            if v: out.append(a.name)
        elif isinstance(a, ZN):
            v = not (w >> a.bit & 1); f[a.key] = v
            if v: out.append(a.name)
        elif isinstance(a, T):
            for value, mask, sub in a.table:
                if w & mask == value:
                    names = [x.text for x in sub if isinstance(x, N)]
                    f[a.key] = ' '.join(names)
                    _walk(sub, w, addr, f, out); break
            else: raise Unsupported(f'no {a.key} encoding')
        elif isinstance(a, R):
            r = bits(w, a.pos, 8); f[a.key] = r
            out.append('0x0' if r == 255 else f'$r{r}')
        elif isinstance(a, P):
            p = bits(w, a.pos, 3) ^ a.xorend; f[a.key] = p
            out.append('0x1' if p == 7 else f'$p{p}')
        elif isinstance(a, PN):
            neg = bool(w >> a.notbit & 1); p = bits(w, a.pos, 3)
            f[a.key] = (p, neg)
            if neg: out.append('not')
            out.append('0x1' if p == 7 else f'$p{p}')
        elif isinstance(a, I):
            v = imm_value(a, w)
            if a.pcrel:
                v = addr + 8 + v; out.append(fmt_hex(v))
                f[a.key] = v + 8 if v % 32 == 0 else v
            else:
                f[a.key] = v; out.append(fmt_hex(v))
        elif isinstance(a, SR):
            s = bits(w, a.pos, 8); f[a.key] = s
            out.append('$' + SYSREGS[s] if s in SYSREGS else f'$s{s}')
        elif isinstance(a, M):
            bank = bits(w, *a.bank) if a.bank else None
            reg = bits(w, a.reg, 8) if a.reg is not None else 255
            off = imm_value(a.off, w) if a.off else 0
            f[a.key] = (a.space, bank, reg, off)
            name = a.space + (str(bank) if bank is not None else '')
            if reg != 255:
                inner = f'$r{reg}' + ('' if not off else (f'+0x{off:x}' if off > 0 else f'-0x{-off:x}'))
            else:
                inner = fmt_hex(off)
            out.append(f'{name}[{inner}]')
        else: raise TypeError(a)

def decode(word, addr):
    for value, mask, name, atoms in ROOT:
        if word & mask == value:
            ins = Insn(); ins.addr = addr; ins.word = word; ins.name = name; ins.opcode = value
            f = {}; out = []
            pred = bits(word, 16, 3); neg = bool(word >> 19 & 1)
            if name in NO_GUARD: pred, neg = 7, False
            ins.guard = (pred, neg)
            if pred == 7 and neg: out.append('never')
            elif pred != 7:
                if neg: out.append('not')
                out.append(f'$p{pred}')
            out.append(name)
            _walk(atoms, word, addr, f, out)
            ins.f = f; ins.tokens = out
            return ins
    raise Unsupported(f'unsupported Maxwell opcode {word >> 48:#06x} at {addr:#x}')

def is_sched(addr):
    return addr % 32 == 0

def program_words(code):
    import struct
    return [(i * 8, w) for i, (w,) in enumerate(struct.iter_unpack('<Q', code))]
