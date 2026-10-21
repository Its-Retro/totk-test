"""ExeFS NSO -> the decompressed NSO layout the translator and launcher read (uncompressed/<name>).

The output is byte-identical to `hactool -t nso --uncompressed=...` (checked on all four base-game modules):
  * each segment (0 text, 1 rodata, 2 data) whose flag bit (1 << k) is set is LZ4-block-decompressed to its
    size, and checked against its SHA-256 when flag bit (8 << k) is set;
  * the header keeps every field except: flags &= ~7, the word at 0x2C is cleared, each segment's file offset
    becomes 0x100 + its memory offset and each stored (compressed) size becomes the decompressed size;
  * the file is that header plus a zero-filled image with each segment at 0x100 + its memory offset.
No keys are involved: ExeFS NSOs are only compressed, not encrypted.
"""
import hashlib, struct


class NsoError(ValueError):
    pass


def lz4_block(src, out_size):
    """Decode one raw LZ4 block (no frame) of exactly out_size bytes."""
    dst = bytearray(); i = 0; n = len(src)
    while i < n:
        tok = src[i]; i += 1
        lit = tok >> 4
        if lit == 15:
            while True:
                if i >= n: raise NsoError('truncated LZ4 literal length')
                b = src[i]; i += 1; lit += b
                if b != 255: break
        if i + lit > n: raise NsoError('truncated LZ4 literals')
        dst += src[i:i + lit]; i += lit
        if i >= n: break                      # last sequence: literals only
        if i + 2 > n: raise NsoError('truncated LZ4 match offset')
        off = src[i] | (src[i + 1] << 8); i += 2
        if off == 0 or off > len(dst): raise NsoError('bad LZ4 match offset')
        ml = tok & 15
        if ml == 15:
            while True:
                if i >= n: raise NsoError('truncated LZ4 match length')
                b = src[i]; i += 1; ml += b
                if b != 255: break
        ml += 4
        start = len(dst) - off
        if off >= ml:
            dst += dst[start:start + ml]
        else:                                 # overlapping copy
            for k in range(ml): dst.append(dst[start + k])
        if len(dst) > out_size: raise NsoError('LZ4 output larger than the segment')
    if len(dst) != out_size: raise NsoError(f'LZ4 output {len(dst)} bytes, segment is {out_size}')
    return bytes(dst)


def build_id(data):
    """The 32-byte module id at 0x40 (the first 20 bytes are the GNU build id), as hex."""
    if data[:4] != b'NSO0' or len(data) < 0x100: raise NsoError('not an NSO file')
    return data[0x40:0x60].hex()


def decompress(data):
    if data[:4] != b'NSO0' or len(data) < 0x100: raise NsoError('not an NSO file (no NSO0 header)')
    flags = struct.unpack_from('<I', data, 0xC)[0]
    segs = []
    for k in range(3):
        foff, va, size = struct.unpack_from('<III', data, 0x10 + 0x10 * k)
        csize = struct.unpack_from('<I', data, 0x60 + 4 * k)[0]
        stored = csize if flags & (1 << k) else size
        if foff + stored > len(data): raise NsoError(f'segment {k} lies outside the file')
        raw = data[foff:foff + stored]
        body = lz4_block(raw, size) if flags & (1 << k) else raw
        if flags & (8 << k):
            if hashlib.sha256(body).digest() != data[0xA0 + 0x20 * k:0xC0 + 0x20 * k]:
                raise NsoError(f'segment {k} SHA-256 mismatch (damaged file)')
        segs.append((va, size, body))
    hdr = bytearray(data[:0x100])
    struct.pack_into('<I', hdr, 0xC, flags & ~7)
    struct.pack_into('<I', hdr, 0x2C, 0)
    end = 0
    for k, (va, size, body) in enumerate(segs):
        struct.pack_into('<I', hdr, 0x10 + 0x10 * k, 0x100 + va)
        struct.pack_into('<I', hdr, 0x60 + 4 * k, size)
        end = max(end, 0x100 + va + size)
    out = bytearray(end); out[:0x100] = hdr
    for va, size, body in segs: out[0x100 + va:0x100 + va + size] = body
    return bytes(out)


def segment_image(uncompressed):
    """text + rodata + data of a decompressed NSO, concatenated (the input of the main-module shader scan)."""
    out = bytearray()
    for k in range(3):
        foff, _, size = struct.unpack_from('<III', uncompressed, 0x10 + 0x10 * k)
        out += uncompressed[foff:foff + size]
    return bytes(out)
