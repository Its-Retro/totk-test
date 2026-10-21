"""Compile the dumped NPDM's exact service-access profile, never a wildcard."""
import hashlib,json,struct
from pathlib import Path
ROOT=Path(__file__).resolve().parent
NPDM_SHA256='f5a5eecc49bbc31d6508c72f2e702fa9dcf2911efcc458d45963f1bdc2722485'

def parse_sac(data):
    result=[];offset=0
    while offset<len(data):
        control=data[offset];offset+=1;length=(control&15)+1
        if control&0xf0 or length>8 or offset+length>len(data):raise ValueError('Unsupported/truncated service-access entry')
        raw=data[offset:offset+length];offset+=length
        if any(c<33 or c>126 for c in raw) or b'*' in raw:raise ValueError('Service name outside exact-name profile')
        name=raw.decode('ascii')
        if name in result:raise ValueError('Duplicate service-access entry')
        result.append(name)
    return result

def read_profile():
    data=(ROOT.parent/'exefs'/'main.npdm').read_bytes()
    if hashlib.sha256(data).hexdigest()!=NPDM_SHA256:raise ValueError('NPDM hash mismatch')
    aci,size,acid,acid_size=struct.unpack_from('<IIII',data,0x70)
    lists=[]
    for base,length,magic_offset,magic,sac_offset in [(aci,size,0,b'ACI0',0x28),(acid,acid_size,0x200,b'ACID',0x228)]:
        if base+length>len(data) or data[base+magic_offset:base+magic_offset+4]!=magic:raise ValueError('Invalid NPDM section')
        start,extent=struct.unpack_from('<II',data,base+sac_offset)
        if start+extent>length:raise ValueError('Service-access range outside NPDM section')
        lists.append(parse_sac(data[base+start:base+start+extent]))
    if lists[0]!=lists[1]:raise ValueError('ACI0/ACID service-access mismatch')
    reference_path=ROOT.parent/'permissions.json'   # optional: hactool's JSON of the same NPDM, an independent cross-check
    if reference_path.exists():
        reference=json.loads(reference_path.read_text())
        if lists[0]!=reference['service_access'] or reference['service_host']:raise ValueError('Independent hactool service list mismatch')
    return lists[0]

def write_service_profile():
    names=read_profile()
    rows=[','.join(str(x) for x in n.encode().ljust(8,b'\0')) for n in names]
    source=['/* Generated from pinned main.npdm; see service_profile.py. */',
            '#ifndef TOTK_BOOT_SERVICE_ACCESS_H','#define TOTK_BOOT_SERVICE_ACCESS_H',
            '#include <stdint.h>','#include <string.h>',
            'static inline int boot_service_allowed(const uint8_t*name){',
            '    static const uint8_t allowed[][8]={'+','.join('{'+r+'}' for r in rows)+'};',
            '    for(unsigned i=0;i<sizeof(allowed)/sizeof(allowed[0]);++i)if(!memcmp(name,allowed[i],8))return 1;',
            '    return 0;','}','#endif','']
    (ROOT/'boot_service_access.h').write_text('\n'.join(source))
    (ROOT/'service-access-profile.json').write_text(json.dumps({'npdm_sha256':NPDM_SHA256,'service_access':names,'hactool_match':(ROOT.parent/'permissions.json').exists()},indent=2)+'\n')
    return names

if __name__=='__main__':print(f'Generated {len(write_service_profile())} exact NPDM service permissions')
