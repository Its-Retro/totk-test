"""Read-only, bounded SARC/Zstandard shader package metadata inspection."""
import hashlib,json,struct,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parent
sys.path.insert(0,str(ROOT.parent/'tools/shader-python'))
sys.path.insert(0,str(ROOT.parent/'assets'))
import zstandard as zstd
from romfs_reader import RomFS

def sarc(data):
    if len(data)<32 or data[:4]!=b'SARC' or data[6:8]!=b'\xff\xfe':raise ValueError('Unsupported SARC header')
    header,=struct.unpack_from('<H',data,4)
    size,start=struct.unpack_from('<II',data,8)
    if size!=len(data) or header!=20 or start>size or start<32:raise ValueError('Invalid SARC extent')
    if data[header:header+4]!=b'SFAT':raise ValueError('Missing SFAT')
    fat_size,count,multiplier=struct.unpack_from('<HHI',data,header+4)
    names=header+fat_size+count*16
    if fat_size!=12 or names+8>start or data[names:names+4]!=b'SFNT':raise ValueError('Invalid SARC tables')
    if struct.unpack_from('<H',data,names+4)[0]!=8:raise ValueError('Unsupported name table')
    names+=8;entries={}
    for index in range(count):
        hashed,attribute,lo,hi=struct.unpack_from('<IIII',data,header+fat_size+index*16)
        offset=names+(attribute&0xffffff)*4
        if attribute>>24!=1 or not names<=offset<start or lo>hi or hi>size-start:raise ValueError('Invalid SARC entry')
        end=data.find(b'\0',offset,start)
        if end<0:raise ValueError('Unterminated SARC name')
        raw=data[offset:end];name=raw.decode('utf-8')
        calculated=0
        for byte in raw:calculated=(calculated*multiplier+byte)&0xffffffff
        if calculated!=hashed or not name or name in entries:raise ValueError('SARC name hash/identity mismatch')
        entries[name]=(start+lo,hi-lo)
    return entries

def decompress(data,dictionaries,limit=16*1024*1024):
    parameters=zstd.get_frame_parameters(data)
    if parameters.content_size in (zstd.CONTENTSIZE_UNKNOWN,zstd.CONTENTSIZE_ERROR) or parameters.content_size>limit or parameters.window_size>limit:
        raise ValueError('Zstandard output outside bounded profile')
    dictionary=dictionaries.get(parameters.dict_id)
    if parameters.dict_id and dictionary is None:raise ValueError('Missing Zstandard dictionary')
    output=zstd.ZstdDecompressor(dict_data=dictionary).decompress(data,max_output_size=limit,allow_extra_data=False)
    if len(output)!=parameters.content_size:raise ValueError('Zstandard output size mismatch')
    return output

def main():
    rom=RomFS(ROOT.parent/'assets/romfs.bin')
    def read(name):return rom.read(name,0,rom.files[name]['size'])
    pack=decompress(read('Pack/ZsDic.pack.zs'),{})
    dictionaries={};dictionary_info=[]
    for name,(offset,size) in sarc(pack).items():
        raw=pack[offset:offset+size];dictionary=zstd.ZstdCompressionDict(raw)
        identity=dictionary.dict_id()
        if not identity or identity in dictionaries:raise ValueError('Invalid dictionary identity')
        dictionaries[identity]=dictionary
        dictionary_info.append({'name':name,'id':identity,'bytes':size,'sha256':hashlib.sha256(raw).hexdigest()})
    shader_name='Shader/ApplicationPackage.Nin_NX_NVN.release.sarc.zs'
    package=decompress(read(shader_name),dictionaries)
    entries=sarc(package)
    def shader_headers(data):
        found=[];position=0
        while True:
            position=data.find(b'BNSH\0\0\0\0',position)
            if position<0:break
            if position+32<=len(data):
                size=struct.unpack_from('<I',data,position+28)[0]
                block=struct.unpack_from('<H',data,position+22)[0]
                if 32<=size<=len(data)-position and block+96<=size and data[position+block:position+block+4]==b'grsc':
                    count=struct.unpack_from('<I',data,position+block+28)[0]
                    found.append({'offset':position,'bytes':size,'version':data[position+8:position+12].hex(),'variation_count':count})
            position+=8
        return found
    items=[{'name':name,'offset':offset,'size':size,'signature':package[offset:offset+min(size,8)].hex(),
            'bnsh_headers':shader_headers(package[offset:offset+size])} for name,(offset,size) in entries.items()]
    compressed=[]
    for name,item in rom.files.items():
        if name.startswith('Shader/') and name.endswith('.zs'):
            params=zstd.get_frame_parameters(rom.read(name,0,min(64,item['size'])))
            info={'name':name,'stored_bytes':item['size'],'decompressed_bytes':params.content_size,'dictionary_id':params.dict_id}
            if name!=shader_name:
                content=decompress(read(name),dictionaries,limit=128*1024*1024)
                info['sha256']=hashlib.sha256(content).hexdigest();info['bnsh_headers']=shader_headers(content)
                del content
            compressed.append(info)
    report={'zstandard_version':zstd.__version__,'dictionaries':dictionary_info,'package_sha256':hashlib.sha256(package).hexdigest(),
        'package_bytes':len(package),'entries':items,'compressed_shader_packages':compressed,
        'scope':'Read-only container inventory; shader programs not translated by this tool'}
    (ROOT/'shader-package-inventory.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))
if __name__=='__main__':main()
