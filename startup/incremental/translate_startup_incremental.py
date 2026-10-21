"""Incremental drop-in for translate_startup.py + partition_native.partition.

Produces translated_startup.c byte-identical to the original pipeline and the
same external_edges list, caching each final boot_page_<n> function text in a
sidecar sqlite. A page is regenerated only when its selected-offset bitmap
changed, when an out-of-page goto destination changed selection membership,
or when the emitter/partitioner namespace changed. Page function text is
produced by calling the original partition_native.partition() on a small
synthetic source (the page's real blocks plus placeholder labels for selected
out-of-page destinations), so goto/transfer rewriting is the original code.
Prefix and dispatch tail are regenerated every run, also via partition().

No ARM is interpreted at runtime; this only generates C ahead of time.

  python translate_startup_incremental.py --ranges-dir DIR --output translated_startup.c
         [--edges-output external-edges.json] [--sidecar page-cache.sqlite]
         [--instruction-cache emitted-instructions.sqlite] [--full-provenance WORKDIR]
"""
import sys
sys.dont_write_bytecode=True
import argparse,hashlib,json,os,re,sqlite3,time,zlib
from pathlib import Path
HERE=Path(__file__).resolve().parent
sys.path.insert(0,str(HERE))
import ts_bridge
from ts_bridge import ts,partition_native,STARTUP

SCRATCH=STARTUP/'incremental-gen'   # default work folder (the builder passes every path explicitly)
GOTO=re.compile(r'goto L_([0-9a-f]+);')
LABEL=re.compile(r'^L_([0-9a-f]+):([^\n]*)\n',re.M)
PAGE_HEADER='static __attribute__((noinline)) uint32_t boot_page_{:x}(BootContext'
EXECUTE_HEADER='static uint32_t boot_execute(BootContext*c,uint64_t target){'
TRAILER='\n'.join(partition_native.PAGE_TRAILER)
# Non-instruction label (like an external stub, with a body line so the label
# regex sees it) followed by the end of the label section.
STUB='L_ffffffffffff:\n    return c->status;\nindirect:\n'
SYSTEM_REFRESH=('svc #0xb','svc #0x34','svc #0x35')

def rtld_rewrite(text):
    text=re.sub(r'c->rtld_base\+UINT64_C\((\d+)\)',r'boot_address(c,UINT64_C(\1))',text)
    return text.replace('c->rtld_base+entry_offset','boot_address(c,entry_offset)')
HEADER=rtld_rewrite('\n'.join(ts_bridge.PROLOGUE_LINES)+'\n')

class Selection:
    """Selected offsets as one 1024-bit bitmap per 4 KB page (no per-offset objects)."""
    def __init__(self,ranges):
        self.bitmaps={}
        for prefix,key in ((0,'RANGES'),(3,'SDK_RANGES'),(2,'SUBSDK_RANGES'),(1,'MAIN_RANGES')):
            base=prefix<<28
            for start,end in ranges[key]:
                if end<=start:continue
                if start%4 or start<0 or end>1<<28:raise ValueError(f'Range {start:#x}-{end:#x} not representable')
                a=base+start;b=base+start+((end-start+3)//4)*4
                while a<b:
                    page=a>>12;stop=min(b,(page+1)<<12)
                    lo=(a&0xfff)>>2;hi=((stop-4)&0xfff)>>2
                    self.bitmaps[page]=self.bitmaps.get(page,0)|(((1<<(hi-lo+1))-1)<<lo)
                    a=stop
        for literal in ts.LITERALS:
            if literal%4==0 and literal>>12 in self.bitmaps:self.bitmaps[literal>>12]&=~(1<<((literal&0xfff)>>2))
        self.bitmaps={page:bits for page,bits in self.bitmaps.items() if bits}
        self.pages=sorted(self.bitmaps)
    def member(self,d):
        return d%4==0 and (self.bitmaps.get(d>>12,0)>>((d&0xfff)>>2))&1==1
    def offsets(self,page):
        bits=self.bitmaps[page];base=page<<12;out=[]
        while bits:
            low=bits&-bits;out.append(base|((low.bit_length()-1)<<2));bits^=low
        return out
    def count(self):return sum(bin(bits).count('1') for bits in self.bitmaps.values())

class Emitter:
    """Per-page records exactly as translate_startup.main() derives them."""
    def __init__(self,cache):
        self.cache=cache;self.modules=None
    def _load(self):
        from capstone import Cs,CS_ARCH_ARM64,CS_MODE_ARM
        import struct
        self.md=Cs(CS_ARCH_ARM64,CS_MODE_ARM);self.md.detail=True
        self.modules={}
        for prefix,name,expected in ((0,'rtld',ts.EXPECTED),(3,'sdk',ts.SDK_EXPECTED),(2,'subsdk0',ts.SUBSDK_EXPECTED),(1,'main',ts.MAIN_EXPECTED)):
            data=(STARTUP.parent/'uncompressed'/name).read_bytes()
            if hashlib.sha256(data).hexdigest()!=expected:raise ts.Unsupported(f'{name} input changed')
            self.modules[prefix]=(data,*struct.unpack_from('<III',data,16))
    def decode(self,p,message):
        if self.modules is None:self._load()
        data,file_at,text_va,text_size=self.modules[p>>28]
        relative=p&0xfffffff
        if not text_va<=relative<text_va+text_size:raise ts.Unsupported(message)
        return list(self.md.disasm(data[file_at+relative-text_va:file_at+relative-text_va+4],p))
    def records(self,offsets):
        records=self.cache.get(offsets)
        if records is not None:
            for record in records:
                if record['instruction'] not in SYSTEM_REFRESH and not record['instruction'].startswith(('msr fpcr, ','sub v','addv ')):continue
                decoded=self.decode(record['offset'],'Cached sleep outside text')
                if len(decoded)!=1 or (record['instruction'].startswith('svc #') and (decoded[0].mnemonic!='svc' or f'svc {decoded[0].op_str}'!=record['instruction'])) or (record['instruction'].startswith('msr fpcr, ') and (decoded[0].mnemonic!='msr' or not decoded[0].op_str.startswith('fpcr, '))) or (record['instruction'].startswith(('sub v','addv ')) and f'{decoded[0].mnemonic} {decoded[0].op_str}'!=record['instruction']):
                    raise ts.Unsupported('Cached system opcode mismatch')
                record['statements'],record['edges'],record['indirect']=ts.emit(decoded[0])
            return records
        records=[]
        for p in offsets:
            decoded=self.decode(p,'Outside text')
            if len(decoded)!=1:raise ts.Unsupported(f'Invalid opcode at {p:#x}')
            i=decoded[0];statements,edges,indirect=ts.emit(i)
            records.append({'offset':p,'instruction':f'{i.mnemonic} {i.op_str}','statements':statements,'edges':edges,'indirect':indirect})
        self.cache.put(offsets,records)
        return records

def page_body(page,offsets,records,selection):
    """(instruction blocks text, out-of-page dests [[d,selected]], out-of-page edges) of one page."""
    blocks=[];edges=set()
    for record in records:
        p=record['offset']
        lines=[f'L_{p:x}: /* {record["instruction"]} */',f'    if(!boot_tick(c,UINT64_C({p})))return c->status;']
        lines.extend('    '+statement for statement in record['statements'])
        blocks.append('\n'.join(lines)+'\n')
        edges.update(record['edges'])
    body=rtld_rewrite(''.join(blocks))
    if sum(1 for match in LABEL.finditer(body) if '/*' in match.group(2))!=len(offsets) or sum(1 for _ in LABEL.finditer(body))!=len(offsets):
        raise RuntimeError(f'page {page:x}: emitted text contains extra label lines; use the original generator')
    own=set(offsets)
    dests={int(d,16) for d in GOTO.findall(body)}
    remote=sorted(d for d in dests if d>>12!=page)
    return body,[[d,int(selection.member(d))] for d in remote],sorted(e for e in edges if e not in own)

def build_page(page,offsets,records,selection,entries=None,body=None):
    """Return (function text in 'trailing-newline' form, out-of-page dests, out-of-page edges).
    entries: this page's forced entry offsets (forced_entries); None = every selected offset
    (only for tools that do not track entries). body: page_body's result if already computed."""
    if body is None:body,dests,edges=page_body(page,offsets,records,selection)
    else:body,dests,edges=body
    if entries is None:entries=set(offsets)
    remote=[d for d,_ in dests]
    known=[d for d in remote if selection.member(d)]
    before=''.join(f'L_{d:x}: /* placeholder */\n' for d in known if d<offsets[0])
    after_list=[d for d in known if d>offsets[-1]]
    after=''.join(f'L_{d:x}: /* placeholder */\n' for d in after_list)
    out=partition_native.partition(HEADER+before+body+after+STUB,entries=set(entries))
    start=out.index(PAGE_HEADER.format(page))
    end=out.index('\n'+(PAGE_HEADER.format(after_list[0]>>12) if after_list else EXECUTE_HEADER),start)
    text=out[start:end]
    if not text.endswith('\n\n'+TRAILER):raise RuntimeError(f'page {page:x}: unexpected partition layout')
    return text,dests,edges

def forced_entries(selection,dests_by_page,static_by_page):
    """Per page: selected offsets that must be entries besides the page-local rules: targets of
    gotos from other pages (dests of every selected page) plus static/discovered entries."""
    forced={}
    for page,dests in dests_by_page.items():
        for d,_ in dests:
            if selection.member(d):forced.setdefault(d>>12,set()).add(d)
    for page,offsets in static_by_page.items():
        if page in selection.bitmaps:
            for d in offsets:
                if selection.member(d):forced.setdefault(page,set()).add(d)
    return forced
def entries_blob(offsets):
    bits=0
    for d in offsets:bits|=1<<((d&0xfff)>>2)
    return bits_blob(bits)

def dispatch_parts(pages):
    out=partition_native.partition(HEADER+''.join(f'L_{page<<12:x}: /* placeholder */\n' for page in pages)+STUB,entries=set())
    return out[:out.index('static __attribute__((noinline)) uint32_t boot_page_')],out[out.index(EXECUTE_HEADER):]

def generator_namespace(source_ns,effective_ns):
    parts=[source_ns,effective_ns,ts_bridge.sha256_file(STARTUP/'partition_native.py'),ts_bridge.sha256_file(STARTUP/'entry_points.py'),
           ts_bridge.sha256_file(__file__),ts_bridge.sha256_file(ts_bridge.__file__)]
    return hashlib.sha256(json.dumps(parts).encode()).hexdigest()

def open_sidecar(path,namespace):
    path.parent.mkdir(parents=True,exist_ok=True)
    db=sqlite3.connect(path)
    db.execute('CREATE TABLE IF NOT EXISTS meta (k TEXT PRIMARY KEY, v TEXT NOT NULL)')
    db.execute('CREATE TABLE IF NOT EXISTS entry_pages (page INTEGER PRIMARY KEY, bitmap BLOB NOT NULL, dests TEXT NOT NULL, edges TEXT NOT NULL, text BLOB NOT NULL, sha TEXT NOT NULL, entries BLOB NOT NULL)')
    row=db.execute("SELECT v FROM meta WHERE k='namespace'").fetchone()
    if not row or row[0]!=namespace:
        db.execute('DELETE FROM entry_pages');db.execute("INSERT OR REPLACE INTO meta VALUES ('namespace',?)",(namespace,));db.commit()
    return db

def bits_blob(bits):return bits.to_bytes(128,'little')

def generate(ranges_dir,output,edges_output,sidecar,cache_path,aliases_path=None,write_cache=False,in_place=True):
    timings={};t0=time.perf_counter()
    ranges=ts_bridge.load_ranges(ranges_dir)
    selection=Selection(ranges)
    if not selection.pages:raise ValueError('No generated instruction labels')
    aliases_path=Path(aliases_path) if aliases_path else STARTUP/'instruction-cache-aliases.json'
    source_ns=ts_bridge.source_namespace();effective_ns=ts_bridge.effective_namespace(source_ns,aliases_path)
    db=open_sidecar(Path(sidecar),generator_namespace(source_ns,effective_ns))
    timings['selection']=time.perf_counter()-t0;t=time.perf_counter()
    stored={page:(bitmap,json.loads(dests),json.loads(edges),entries) for page,bitmap,dests,edges,entries in db.execute('SELECT page,bitmap,dests,edges,entries FROM entry_pages')}
    changed=[]
    for page in selection.pages:
        entry=stored.get(page)
        if entry is None or entry[0]!=bits_blob(selection.bitmaps[page]) or any(selection.member(d)!=bool(k) for d,k in entry[1]):
            changed.append(page)
    removed=[page for page in stored if page not in selection.bitmaps]
    timings['dirty_check']=time.perf_counter()-t;t=time.perf_counter()
    cache_cls=ts_bridge.instruction_cache.InstructionCache if write_cache else ts_bridge.ReadOnlyInstructionCache
    import entry_points
    static_by_page=entry_points.by_page(set(entry_points.static_entries())|set(entry_points.extra_entries(Path(ranges_dir)/'discovered-entries.json')))
    timings['static_entries']=time.perf_counter()-t;t=time.perf_counter()
    cache=cache_cls(cache_path,effective_ns) if changed else None
    emitter=Emitter(cache)
    # Pass 1: out-of-page dests of the changed pages (they decide other pages' entries). Bodies are
    # kept (compressed) for a normal round; a full regeneration fetches them again in pass 2.
    keep=len(changed)<=4000
    bodies={};dests_by_page={page:entry[1] for page,entry in stored.items() if page in selection.bitmaps}
    for page in changed:
        offsets=selection.offsets(page)
        body,dests,edges=page_body(page,offsets,emitter.records(offsets),selection)
        dests_by_page[page]=dests
        if keep:bodies[page]=(zlib.compress(body.encode('utf-8'),1),dests,edges)
    forced=forced_entries(selection,dests_by_page,static_by_page)
    changed_set=set(changed)
    dirty=[page for page in selection.pages if page in changed_set or stored[page][3]!=entries_blob(forced.get(page,()))]
    timings['entry_check']=time.perf_counter()-t;t=time.perf_counter()
    if dirty and cache is None:cache=cache_cls(cache_path,effective_ns);emitter=Emitter(cache)
    edges_by_page={page:entry[2] for page,entry in stored.items()}
    for page in removed:db.execute('DELETE FROM entry_pages WHERE page=?',(page,));edges_by_page.pop(page,None)
    for page in dirty:
        offsets=selection.offsets(page)
        page_entries=forced.get(page,set())
        if page in bodies:
            packed,dests,edges=bodies.pop(page)
            text,dests,edges=build_page(page,offsets,None,selection,page_entries,(zlib.decompress(packed).decode('utf-8'),dests,edges))
        else:text,dests,edges=build_page(page,offsets,emitter.records(offsets),selection,page_entries)
        encoded=text.encode('utf-8')
        db.execute('INSERT OR REPLACE INTO entry_pages VALUES (?,?,?,?,?,?,?)',(page,bits_blob(selection.bitmaps[page]),json.dumps(dests,separators=(',',':')),
                   json.dumps(edges,separators=(',',':')),zlib.compress(encoded,1),hashlib.sha256(encoded).hexdigest(),entries_blob(page_entries)))
        edges_by_page[page]=edges
    if cache is not None:
        cache_stats={'page_hits':cache.hits,'page_misses':cache.misses};cache.close()
    else:cache_stats={'page_hits':0,'page_misses':0}
    timings['regenerate']=time.perf_counter()-t;t=time.perf_counter()
    external=sorted({e for page in selection.pages for e in edges_by_page[page] if not selection.member(e)})
    prefix,tail=dispatch_parts(selection.pages)
    output=Path(output).resolve();pages=selection.pages
    last=pages[-1]
    trailer=TRAILER.encode()
    # Suffix rewrite: if the existing output is exactly what we wrote last time
    # (path/size/mtime recorded), keep the unchanged leading pages' bytes and
    # rewrite from the first page whose text or position changed.
    row=db.execute("SELECT v FROM meta WHERE k='output_state'").fetchone()
    state=json.loads(row[0]) if row else None
    first=0;start=None
    if in_place and state and state['path']==str(output) and output.exists():
        info=output.stat()
        if info.st_size==state['size'] and info.st_mtime_ns==state['mtime_ns']:
            old=state['pages'];dirty_set=set(dirty);limit=min(len(old),len(pages))
            while first<limit and old[first]==pages[first] and pages[first] not in dirty_set:first+=1
            if state['external_empty'] or not external:first=min(first,limit-1)
            first=max(first,0)
            start=state['starts'][first] if first<len(old) else state['tail_start']
    db.execute("DELETE FROM meta WHERE k='output_state'");db.commit()   # invalid until the write completes
    starts=state['starts'][:first] if start is not None else []
    def emit_pages(stream,position):
        for page,blob,sha in db.execute('SELECT page,text,sha FROM entry_pages WHERE page>=? ORDER BY page',(pages[first] if first<len(pages) else 1<<62,)):
            data=zlib.decompress(blob)
            if hashlib.sha256(data).hexdigest()!=sha:raise RuntimeError(f'sidecar checksum mismatch for page {page:x}')
            if page==last and not external:data=data[:-len(trailer)-1]+trailer   # last block has no trailing newline
            starts.append(position);stream.write(data);stream.write(b'\n');position+=len(data)+1
        return position
    tail_bytes=tail.encode('utf-8')
    if start is not None:
        with output.open('r+b',buffering=4*1024*1024) as stream:
            stream.seek(start);stream.truncate()
            tail_start=emit_pages(stream,start);stream.write(tail_bytes)
    else:
        pending=output.with_name(output.name+'.tmp')
        with pending.open('wb',buffering=4*1024*1024) as stream:   # stored text is UTF-8 with '\n' only
            prefix_bytes=prefix.encode('utf-8');stream.write(prefix_bytes)
            tail_start=emit_pages(stream,len(prefix_bytes));stream.write(tail_bytes)
        os.replace(pending,output)
    if len(starts)!=len(pages):raise RuntimeError('sidecar/page set mismatch')
    info=output.stat()
    db.execute("INSERT OR REPLACE INTO meta VALUES ('output_state',?)",(json.dumps({'path':str(output),'size':info.st_size,'mtime_ns':info.st_mtime_ns,
               'pages':pages,'starts':starts,'tail_start':tail_start,'external_empty':not external}),))
    db.commit();db.close()
    rewritten=info.st_size-(start if start is not None else 0)
    edges_output=Path(edges_output)
    pending=edges_output.with_name(edges_output.name+'.tmp')
    # Readable by provenance_edges.read_external_edges as well.
    pending.write_text(json.dumps({'external_edges':external},separators=(',',':'))+'\n');os.replace(pending,edges_output)
    timings['write']=time.perf_counter()-t;timings['total']=time.perf_counter()-t0
    return {'pages':len(selection.pages),'regenerated_pages':len(dirty),'entry_only_pages':len(dirty)-len(changed),'removed_pages':len(removed),
            'instructions':selection.count(),'external_edges':len(external),'instruction_cache':cache_stats,
            'output_bytes_rewritten':rewritten,'output_bytes':info.st_size,
            'seconds':{k:round(v,3) for k,v in timings.items()},'output':str(output),'edges_output':str(edges_output)}

def main():
    parser=argparse.ArgumentParser(description=__doc__,formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--ranges-dir',type=Path,default=STARTUP,help='directory holding discovered-*-ranges.json')
    parser.add_argument('--output',type=Path,default=SCRATCH/'out'/'translated_startup.c')
    parser.add_argument('--edges-output',type=Path,help='default: <output dir>/external-edges.json')
    parser.add_argument('--sidecar',type=Path,default=Path(os.environ.get('TOTK_INCREMENTAL_SIDECAR',SCRATCH/'page-cache.sqlite')))
    parser.add_argument('--instruction-cache',type=Path,default=STARTUP/'emitted-instructions.sqlite')
    parser.add_argument('--aliases',type=Path,default=STARTUP/'instruction-cache-aliases.json')
    parser.add_argument('--write-instruction-cache',action='store_true',help='persist misses into the instruction cache (off: read-only)')
    parser.add_argument('--full-rewrite',action='store_true',help='always write a fresh file (tmp+replace) instead of rewriting only the changed suffix')
    parser.add_argument('--full-provenance',type=Path,metavar='WORKDIR',help='also run the original generator into WORKDIR for full provenance.json (slow)')
    args=parser.parse_args()
    edges_output=args.edges_output or args.output.with_name('external-edges.json')
    report=generate(args.ranges_dir,args.output,edges_output,args.sidecar,args.instruction_cache,args.aliases,args.write_instruction_cache,not args.full_rewrite)
    if args.full_provenance:
        import reference_translate
        _,seconds=reference_translate.run(args.ranges_dir,args.full_provenance,args.instruction_cache,args.aliases)
        report['full_provenance']=str(Path(args.full_provenance)/'startup'/'provenance.json');report['full_provenance_seconds']=seconds
    print(json.dumps(report,indent=2))

if __name__=='__main__':main()
