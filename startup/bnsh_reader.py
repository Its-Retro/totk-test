"""Bounded BNSH 2.1.2 / 2.2.1 binary-stage reader; never executes shader data.

Layout cross-checked against ShaderLibrary/Switch/BnshLoader.cs and the
bnsh-decoder BNSH.ksy. Only the observed little-endian, unrelocated profile
is accepted. This is container parsing, not Maxwell instruction translation.
"""
import hashlib,struct

class BnshError(ValueError):
    pass

def parse(data):
    def span(offset,size):
        if offset<0 or size<0 or offset>len(data)-size:
            raise BnshError(f'Out-of-range BNSH field at {offset:#x}, size {size:#x}')
        return memoryview(data)[offset:offset+size]
    def unpack(fmt,offset):
        return struct.unpack(fmt,span(offset,struct.calcsize(fmt)))
    if bytes(span(0,8))!=b'BNSH\0\0\0\0':raise BnshError('Invalid BNSH signature')
    # 2.1.2, 2.1.12 (flower package) and 2.2.1; all field reads below are bounds-checked.
    if bytes(span(8,4)) not in (b'\x02\x01\x02\0',b'\x0c\x01\x02\0',b'\x01\x02\x02\0'):raise BnshError('Unsupported BNSH version')
    bom,alignment,target=unpack('<HBB',12)
    name,flags,block,rlt,size=unpack('<IHHII',16)
    if bom!=0xfeff or flags!=0 or size!=len(data) or alignment>16:
        raise BnshError('Unsupported endian, relocated flags, size or alignment')
    if bytes(span(block,4))!=b'grsc':raise BnshError('Missing shader block')
    if bytes(span(rlt,4))!=b'_RLT':raise BnshError('Missing relocation table')
    api,api_version,code_type,reserved,compiler,count,variations,pool=unpack('<HHHHIIQQ',block+16)
    if api!=4 or count>65536:raise BnshError('Unsupported graphics API or variation count')
    span(variations,count*64)
    programs=[]
    for index in range(count):
        source,intermediate,binary,parent=unpack('<QQQQ',variations+index*64)
        if not binary:raise BnshError('No binary shader program')
        span(binary,160)
        stages=[]
        for stage,descriptor in zip(('vertex','tess_control','tess_eval','geometry','fragment','compute'),unpack('<6Q',binary+8)):
            if not descriptor:continue
            span(descriptor,64)
            control,code,code_size,control_size=unpack('<QQII',descriptor+8)
            if not code_size or code_size>16*1024*1024 or control_size>16*1024*1024:
                raise BnshError('Shader stage exceeds bounded profile')
            bytecode=span(code,code_size);control_code=span(control,control_size)
            stages.append({'stage':stage,'descriptor_offset':descriptor,'bytecode_offset':code,
                'bytecode_size':code_size,'bytecode_sha256':hashlib.sha256(bytecode).hexdigest(),
                'control_offset':control,'control_size':control_size,
                'control_sha256':hashlib.sha256(control_code).hexdigest()})
        if not stages:raise BnshError('Empty binary program')
        programs.append({'index':index,'program_offset':binary,'stages':stages})
    return {'sha256':hashlib.sha256(data).hexdigest(),'bytes':len(data),'api':api,
            'compiler_version':compiler,'variation_count':count,'programs':programs}

if __name__=='__main__':
    import json,sys
    from pathlib import Path
    root=Path(__file__).resolve().parent
    sys.path.insert(0,str(root.parent/'assets'))
    from romfs_reader import RomFS
    rom=RomFS(root.parent/'assets/romfs.bin');name='Lib/NintendoSDK/TextureShader.bnsh'
    report=parse(rom.read(name,0,rom.files[name]['size']));report['romfs_path']=name
    (root/'texture-shader-inventory.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))
