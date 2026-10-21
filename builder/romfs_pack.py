"""Pack an extracted RomFS folder into a RomFS image (assets/romfs.bin), the form the runtime and the shader
tools read.

Layout (level-3 RomFS, as the game's own image is laid out):
  0x000  header: 0x50, then (offset, size) of the directory hash table, directory table, file hash table and
         file table, then the file-data offset (0x200)
  0x200  file data, one file after another in sorted-path order, each start aligned to 16 bytes, zero padding
  then   directory hash table, directory table, file hash table, file table (contiguous, after the data
         rounded up to 16)
Files are taken in byte order of their full path ('/' separated). A directory entry is created the first
time a file inside it is reached (parents first) and appended to its parent's child list; files are
appended to their directory's file list. Hash buckets: the first count >= n not divisible by 2..17 (3 for
n < 3, n | 1 for n < 19); a new entry goes to the head of its bucket chain. Path hash: h = parent ^ 123456789,
then for each name byte h = ror(h, 5) ^ byte (host_svc_fs.h svc_romfs_hash uses the same function).

The image is written to <out>.partial and renamed when complete; its SHA-256 is computed while writing.
Memory: only metadata (a few tens of MB for the base game) plus one 4 MiB copy buffer.
"""
import hashlib, os, struct, time
from pathlib import Path

NONE = 0xFFFFFFFF
DATA_OFFSET = 0x200
CHUNK = 4 * 1024 * 1024


def bucket_count(n):
    if n < 3: return 3
    if n < 19: return n | 1
    c = n
    while any(c % p == 0 for p in (2, 3, 5, 7, 11, 13, 17)): c += 1
    return c


def path_hash(parent, name):
    h = parent ^ 123456789
    for b in name:
        h = ((h >> 5) | (h << 27)) & 0xFFFFFFFF
        h ^= b
    return h


def align(v, a):
    return (v + a - 1) // a * a


def scan(folder):
    """[(path bytes with '/' separators, size, absolute path)] for every file under folder, plus the number
    of directories (root included). Raises on names that are not plain ASCII (the base game has none)."""
    folder = Path(folder)
    files, dirs = [], 1
    for here, subdirs, names in os.walk(folder):
        subdirs.sort()
        dirs += len(subdirs)
        rel = Path(here).relative_to(folder).as_posix()
        for n in names:
            p = n if rel == '.' else f'{rel}/{n}'
            try:
                b = p.encode('ascii')
            except UnicodeEncodeError:
                raise ValueError(f'non-ASCII file name in the RomFS folder: {p!r}')
            full = os.path.join(here, n)
            files.append((b, os.path.getsize(full), full))
    return files, dirs


def build(files):
    """Metadata for [(path bytes, size, source)]: returns dict with header, the four tables, the data
    layout [(data offset, size, source)] and the total image size."""
    files = sorted(files, key=lambda f: f[0])
    # directories: path -> record; records in creation order
    dirs = {b'': {'name': b'', 'parent': None, 'dirs': [], 'files': []}}
    order = [b'']
    def ensure(path):
        if path in dirs: return
        parent = path.rpartition(b'/')[0] if b'/' in path else b''
        ensure(parent)
        dirs[path] = {'name': path.rpartition(b'/')[2], 'parent': parent, 'dirs': [], 'files': []}
        dirs[parent]['dirs'].append(path); order.append(path)
    frecs = []
    for path, size, source in files:
        parent, _, name = path.rpartition(b'/')
        if not name: raise ValueError(f'empty file name: {path!r}')
        ensure(parent)
        rec = {'name': name, 'parent': parent, 'size': size, 'source': source}
        dirs[parent]['files'].append(len(frecs)); frecs.append(rec)
    # directory table offsets (creation order)
    off = 0
    for p in order:
        dirs[p]['off'] = off; off += 0x18 + align(len(dirs[p]['name']), 4)
    dir_meta_size = off
    off = 0; data = 0; layout = []
    for r in frecs:
        r['off'] = off; off += 0x20 + align(len(r['name']), 4)
        r['data'] = data; layout.append((data, r['size'], r['source']))
        data = align(data + r['size'], 16)
    file_meta_size = off; data_size = data
    dir_meta = bytearray(dir_meta_size); file_meta = bytearray(file_meta_size)
    nd, nf = bucket_count(len(order)), bucket_count(len(frecs))
    dir_heads, file_heads = [NONE] * nd, [NONE] * nf
    for p in order:
        d = dirs[p]
        parent_off = 0 if d['parent'] is None else dirs[d['parent']]['off']
        b = path_hash(parent_off, d['name']) % nd
        sib = NONE
        if d['parent'] is not None:
            siblings = dirs[d['parent']]['dirs']; i = siblings.index(p)
            sib = dirs[siblings[i + 1]]['off'] if i + 1 < len(siblings) else NONE
        child_dir = dirs[d['dirs'][0]]['off'] if d['dirs'] else NONE
        child_file = frecs[d['files'][0]]['off'] if d['files'] else NONE
        struct.pack_into('<6I', dir_meta, d['off'], parent_off, sib, child_dir, child_file, dir_heads[b], len(d['name']))
        dir_meta[d['off'] + 0x18:d['off'] + 0x18 + len(d['name'])] = d['name']
        dir_heads[b] = d['off']
    for p in order:   # file sibling links
        fl = dirs[p]['files']
        for i, idx in enumerate(fl):
            frecs[idx]['sib'] = frecs[fl[i + 1]]['off'] if i + 1 < len(fl) else NONE
    for r in frecs:
        parent_off = dirs[r['parent']]['off']
        b = path_hash(parent_off, r['name']) % nf
        struct.pack_into('<IIQQII', file_meta, r['off'], parent_off, r['sib'], r['data'], r['size'], file_heads[b], len(r['name']))
        file_meta[r['off'] + 0x20:r['off'] + 0x20 + len(r['name'])] = r['name']
        file_heads[b] = r['off']
    dir_hash = struct.pack(f'<{nd}I', *dir_heads); file_hash = struct.pack(f'<{nf}I', *file_heads)
    t = DATA_OFFSET + data_size
    header = struct.pack('<10Q', 0x50, t, len(dir_hash), t + len(dir_hash), len(dir_meta),
                         t + len(dir_hash) + len(dir_meta), len(file_hash),
                         t + len(dir_hash) + len(dir_meta) + len(file_hash), len(file_meta), DATA_OFFSET)
    total = t + len(dir_hash) + len(dir_meta) + len(file_hash) + len(file_meta)
    return {'header': header, 'dir_hash': dir_hash, 'dir_meta': bytes(dir_meta), 'file_hash': file_hash,
            'file_meta': bytes(file_meta), 'layout': layout, 'image_bytes': total, 'files': len(frecs),
            'dirs': len(order), 'data_bytes': data_size}


def metadata_sha256(meta):
    """Same value as assets/romfs_reader.RomFS.metadata_sha256: sha256(header + directory table + file table)."""
    return hashlib.sha256(meta['header'] + meta['dir_meta'] + meta['file_meta']).hexdigest()


def write_image(meta, out, progress=None):
    """Write the image to out (via out.partial); returns its SHA-256. progress(done_bytes, total_bytes)."""
    out = Path(out); tmp = out.with_name(out.name + '.partial')
    h = hashlib.sha256(); done = 0; total = meta['image_bytes']; last = 0.0
    buf = bytearray(CHUNK); view = memoryview(buf)
    with open(tmp, 'wb', buffering=0) as f:
        def put(b):
            nonlocal done
            f.write(b); h.update(b); done += len(b)
        put(meta['header'] + bytes(DATA_OFFSET - len(meta['header'])))
        for data_off, size, source in meta['layout']:
            pos = DATA_OFFSET + data_off
            if done < pos: put(bytes(pos - done))
            left = size
            with open(source, 'rb', buffering=0) as s:
                while left:
                    n = s.readinto(view[:min(CHUNK, left)])
                    if not n: raise IOError(f'{source} is shorter than when it was scanned')
                    put(view[:n]); left -= n
            if progress and time.monotonic() - last > 2:
                progress(done, total); last = time.monotonic()
        tables_at = struct.unpack_from('<Q', meta['header'], 8)[0]
        if done < tables_at: put(bytes(tables_at - done))
        for k in ('dir_hash', 'dir_meta', 'file_hash', 'file_meta'): put(meta[k])
    if done != total: raise IOError(f'image size {done} != expected {total}')
    if progress: progress(done, total)
    os.replace(tmp, out)
    return h.hexdigest()


if __name__ == '__main__':
    import argparse
    ap = argparse.ArgumentParser(description='Pack an extracted RomFS folder into a RomFS image.')
    ap.add_argument('folder'); ap.add_argument('image')
    a = ap.parse_args()
    files, ndirs = scan(a.folder)
    m = build(files)
    print(f"{m['files']} files, {m['dirs']} directories, image {m['image_bytes']} bytes, metadata {metadata_sha256(m)}")
    print('sha256', write_image(m, a.image, lambda d, t: print(f'  {d * 100 // max(t, 1)}%', flush=True)))
