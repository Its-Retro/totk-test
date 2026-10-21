"""Read-only RomFS inventory and bounded file reads. No host-path extraction.

Layout reference: libnx runtime/devices/romfs_dev.h. File data stays in the
verified image; only the metadata tables are loaded into memory.
"""
import hashlib,json,struct
from pathlib import Path

ROOT=Path(__file__).resolve().parent
NONE=0xffffffff

class RomFS:
    def __init__(self,path):
        self.path=Path(path);self.size=self.path.stat().st_size
        with self.path.open('rb') as f:
            header=f.read(80)
            if len(header)!=80:raise ValueError('Truncated RomFS header')
            fields=struct.unpack('<10Q',header)
            if fields[0]!=80:raise ValueError('Unsupported RomFS header')
            self.data_offset=fields[9]
            if not 80<=self.data_offset<=self.size:raise ValueError('Invalid file-data offset')
            for off,size in zip(fields[1:9:2],fields[2:9:2]):
                if off<80 or off>self.size or size>self.size-off:raise ValueError('Metadata outside image')
                if size>64*1024*1024:raise ValueError('Metadata exceeds bounded reader profile')
            f.seek(fields[3]);self.directories=f.read(fields[4])
            f.seek(fields[7]);self.file_table=f.read(fields[8])
        self.files={};self.directory_paths=[];self._index()
        self.metadata_sha256=hashlib.sha256(header+self.directories+self.file_table).hexdigest()

    def entry(self,table,offset,directory):
        width=24 if directory else 32
        if offset%4 or offset>len(table)-width:raise ValueError('Bad metadata entry offset')
        values=struct.unpack_from('<6I' if directory else '<IIQQII',table,offset)
        length=values[-1]
        if length>len(table)-offset-width:raise ValueError('Truncated metadata name')
        name=table[offset+width:offset+width+length].decode('utf-8')
        if '\0' in name or '/' in name or '\\' in name or name in ('.','..'):raise ValueError('Invalid entry name')
        return values,name,(offset+width+length+3)&~3

    def _index(self):
        valid=[]
        for table,directory in [(self.directories,True),(self.file_table,False)]:
            offset=0;entries=set()
            while offset<len(table):
                entries.add(offset);_,_,offset=self.entry(table,offset,directory)
            if offset!=len(table):raise ValueError('Metadata alignment mismatch')
            valid.append(entries)
        seen_dirs=set();seen_files=set();pending=[(0,0,'')]
        while pending:
            offset,parent,prefix=pending.pop()
            if offset in seen_dirs or offset not in valid[0]:raise ValueError('Directory cycle or duplicate')
            seen_dirs.add(offset);entry,name,_=self.entry(self.directories,offset,True)
            if entry[0]!=parent or (offset==0 and name):raise ValueError('Directory parent mismatch')
            path=f'{prefix}/{name}' if prefix else name
            self.directory_paths.append(path)
            child=entry[2];siblings=set()
            while child!=NONE:
                if child in siblings:raise ValueError('Directory sibling cycle')
                siblings.add(child);sub,_,_=self.entry(self.directories,child,True)
                pending.append((child,offset,path));child=sub[1]
            child=entry[3]
            while child!=NONE:
                if child in seen_files or child not in valid[1]:raise ValueError('File cycle or duplicate')
                seen_files.add(child);item,name,_=self.entry(self.file_table,child,False)
                if item[0]!=offset or not name:raise ValueError('File parent/name mismatch')
                absolute=self.data_offset+item[2];size=item[3]
                if absolute>self.size or size>self.size-absolute:raise ValueError('File data outside image')
                file_path=f'{path}/{name}' if path else name
                if file_path in self.files:raise ValueError('Duplicate file path')
                self.files[file_path]={'offset':absolute,'size':size};child=item[1]
        if seen_dirs!=valid[0] or seen_files!=valid[1]:raise ValueError('Unreachable metadata entries')

    def read(self,path,offset,length):
        entry=self.files[path]
        if offset<0 or length<0 or offset>entry['size'] or length>16*1024*1024:raise ValueError('Read outside bounded file profile')
        length=min(length,entry['size']-offset)
        with self.path.open('rb') as f:
            f.seek(entry['offset']+offset);data=f.read(length)
        if len(data)!=length:raise ValueError('Truncated file data')
        return data

if __name__=='__main__':
    fs=RomFS(ROOT/'romfs.bin')
    inventory={'image_bytes':fs.size,'file_data_offset':fs.data_offset,'metadata_sha256':fs.metadata_sha256,
               'directory_count':len(fs.directory_paths),'file_count':len(fs.files),'file_bytes':sum(x['size'] for x in fs.files.values()),
               'files':dict(sorted(fs.files.items()))}
    (ROOT/'romfs-index.json').write_text(json.dumps(inventory,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({k:v for k,v in inventory.items() if k!='files'},indent=2))
