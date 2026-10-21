"""Reconstruct uploaded macro RAM and inventory all bound source programs."""
import hashlib,json,struct,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parent
def extract(packets):
    ram={};bindings={};code_pos=bind_pos=None;calls=[]
    for packet in packets:
        for write in packet['writes']:
            if write['class']!=0xb197:continue
            method,value=write['method'],write['value']
            if method==0x114:
                if value>=2048:raise ValueError('Macro code pointer')
                code_pos=value
            elif method==0x118:
                if code_pos is None or code_pos>=2048:raise ValueError('Macro RAM write')
                ram[code_pos]=value;code_pos+=1
            elif method==0x11c:
                if value>=128:raise ValueError('Macro binding pointer')
                bind_pos=value
            elif method==0x120:
                if bind_pos is None or bind_pos>=128 or value>=2048:raise ValueError('Macro binding')
                bindings[bind_pos]=value;bind_pos+=1
            elif 0x3800<=method<0x3c00 and method%8==0:calls.append((method-0x3800)//8)
    return ram,bindings,calls
def main():
    folder=ROOT/'gpu-capture';ram,bindings,calls=extract(json.loads((folder/'first-submit-packets.json').read_text()))
    starts=sorted(set(bindings.values()));manifest=[]
    for index,start in sorted(bindings.items()):
        end=next((v for v in starts if v>start),max(ram)+1)
        if any(p not in ram for p in range(start,end)):raise ValueError('Macro range has uninitialized code')
        data=struct.pack('<'+'I'*(end-start),*(ram[p] for p in range(start,end)))
        name=f'macro-{index:02d}.bin';path=folder/name;path.write_bytes(data)
        dis=subprocess.run([str(ROOT.parent/'tools/envydis.exe'),'-m','macro','-i','-b',hex(start),str(path)],capture_output=True,check=True)
        (folder/f'macro-{index:02d}.txt').write_bytes(dis.stdout)
        manifest.append({'index':index,'start':start,'words':end-start,'calls':calls.count(index),'sha256':hashlib.sha256(data).hexdigest(),'file':name})
    (folder/'macros.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print(json.dumps({'uploaded_words':len(ram),'bindings':len(bindings),'called_indices':sorted(set(calls)),'calls':len(calls)},indent=2))
if __name__=='__main__':main()
