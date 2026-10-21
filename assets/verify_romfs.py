"""Verify metadata independently, exercise reads/faults, and hash the image."""
import hashlib,json,random,struct,tempfile
from pathlib import Path
from romfs_reader import RomFS,ROOT,NONE

def synthetic_image():
    data=bytearray(517)
    struct.pack_into('<10Q',data,0,80,80,4,84,24,108,4,112,36,512)
    struct.pack_into('<6I',data,84,0,NONE,NONE,0,NONE,0)
    struct.pack_into('<IIQQII',data,112,0,NONE,0,5,NONE,3)
    data[144:147]=b'foo';data[512:]=b'hello'
    return data

with tempfile.TemporaryDirectory(prefix='totk-romfs-test-') as directory:
    path=Path(directory)/'test.bin';raw=synthetic_image();path.write_bytes(raw);fs=RomFS(path)
    assert fs.read('foo',0,99)==b'hello'
    assert fs.read('foo',2,2)==b'll'
    assert fs.read('foo',5,99)==b''
    for offset,length in [(-1,1),(6,1),(0,-1),(0,16*1024*1024+1)]:
        try:fs.read('foo',offset,length)
        except ValueError:pass
        else:raise AssertionError('Out-of-profile read accepted')
    try:fs.read('../foo',0,1)
    except KeyError:pass
    else:raise AssertionError('Unlisted path accepted')
    for offset,fmt,value in [(8,'Q',9999),(116,'I',0),(112,'I',4),(140,'I',9999),(128,'Q',9999)]:
        bad=bytearray(raw);struct.pack_into('<'+fmt,bad,offset,value);path.write_bytes(bad)
        try:RomFS(path)
        except ValueError:pass
        else:raise AssertionError('Malformed image accepted')

fs=RomFS(ROOT/'romfs.bin')
independent={line[5:].replace('\\','/') for line in (ROOT/'romfs-list.txt').read_text(encoding='utf-8-sig').splitlines() if line.startswith('rom:\\')}
assert independent==set(fs.files),(len(independent),len(fs.files))
log=(ROOT/'extraction.log').read_text(encoding='utf-8-sig')
assert '(FAIL)' not in log
assert all(f'Level {level} (GOOD)' in log for level in range(6))
rng=random.Random(0x524f4d46);paths=rng.sample(sorted(fs.files),128)
with fs.path.open('rb') as source:
    for path in paths:
        item=fs.files[path]
        for start in [0,max(0,item['size']-64),item['size']]:
            source.seek(item['offset']+start)
            expected=source.read(min(64,item['size']-start))
            assert fs.read(path,start,64)==expected
print('Synthetic validation, all independent paths, and 384 bounded reads passed; hashing image.',flush=True)
with fs.path.open('rb') as source:
    digest=hashlib.file_digest(source,'sha256').hexdigest()
report={'status':'PASS','image_sha256':digest,'image_bytes':fs.size,'directories':len(fs.directory_paths),
        'files':len(fs.files),'all_paths_match_hactool':True,'ivfc_levels_verified':6,
        'sample_reads':384,'synthetic_valid_and_invalid_cases':13,'metadata_sha256':fs.metadata_sha256,
        'mounted_by_guest':False}
(ROOT/'romfs-verification.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
