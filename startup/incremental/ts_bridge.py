"""Read-only bridge to the original translator modules.

Imports translate_startup/partition_native WITHOUT running main() and without
writing bytecode into startup/. Extracts the literal base range lists and the
boot_run prologue lines from translate_startup.py via the AST, so selection
inputs can be pointed at snapshot copies instead of startup/.
"""
import sys
sys.dont_write_bytecode=True
import ast,hashlib,json,sqlite3
from pathlib import Path

HERE=Path(__file__).resolve().parent
STARTUP=HERE.parent
if str(STARTUP) not in sys.path:sys.path.insert(0,str(STARTUP))

import translate_startup as ts          # module-level code only reads files
import partition_native                 # noqa: E402
import instruction_cache                # noqa: E402

TS_PATH=Path(ts.__file__).resolve()
MODULE_FILES=('discovered-rtld-ranges.json','discovered-sdk-ranges.json',
              'discovered-subsdk0-ranges.json','discovered-main-ranges.json')

def sha256_file(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def _literals():
    tree=ast.parse(TS_PATH.read_text(encoding='utf-8'))
    base={};prologue=None
    for node in tree.body:
        if isinstance(node,ast.Assign) and len(node.targets)==1 and isinstance(node.targets[0],ast.Name):
            name=node.targets[0].id
            if name in ('RANGES','SDK_RANGES','SUBSDK_RANGES','MAIN_RANGES') and isinstance(node.value,ast.List):
                base[name]=ast.literal_eval(node.value)
        if isinstance(node,ast.FunctionDef) and node.name=='main':
            for sub in ast.walk(node):
                if (isinstance(sub,ast.Assign) and len(sub.targets)==1 and isinstance(sub.targets[0],ast.Name)
                        and sub.targets[0].id=='source' and isinstance(sub.value,ast.List)):
                    prologue=ast.literal_eval(sub.value)
    if prologue is None or 'RANGES' not in base or 'SDK_RANGES' not in base:
        raise RuntimeError('translate_startup.py layout changed; cannot extract literals')
    return base,prologue

BASE_RANGES,PROLOGUE_LINES=_literals()

def load_ranges(ranges_dir):
    """Same composition as translate_startup's module level, from ranges_dir."""
    ranges_dir=Path(ranges_dir)
    def read(name):
        path=ranges_dir/name
        return json.loads(path.read_text()) if path.exists() else []
    return {'RANGES':list(BASE_RANGES['RANGES'])+read('discovered-rtld-ranges.json'),
            'SDK_RANGES':list(BASE_RANGES['SDK_RANGES'])+read('discovered-sdk-ranges.json'),
            'SUBSDK_RANGES':list(BASE_RANGES.get('SUBSDK_RANGES',[]))+read('discovered-subsdk0-ranges.json'),
            'MAIN_RANGES':list(BASE_RANGES.get('MAIN_RANGES',[]))+read('discovered-main-ranges.json')}

def source_namespace(root=STARTUP):
    """Exactly translate_startup.main()'s instruction-cache namespace."""
    import capstone
    root=Path(root)
    return hashlib.sha256(TS_PATH.read_bytes()+(root/'float_emit.py').read_bytes()+(root/'float_emit_ext.py').read_bytes()+(root/'simd_mem_ext.py').read_bytes()+(root/'simd_arith_ext.py').read_bytes()+(root/'instruction_cache.py').read_bytes()+
                          (root/'address_arbiter_emit.py').read_bytes()+(root/'sleep_emit.py').read_bytes()+
                          (capstone.__version__+ts.EXPECTED+ts.SDK_EXPECTED+ts.SUBSDK_EXPECTED+ts.MAIN_EXPECTED).encode()).hexdigest()

def effective_namespace(namespace,aliases_path):
    aliases_path=Path(aliases_path)
    aliases=json.loads(aliases_path.read_text()) if aliases_path.exists() else {}
    return aliases.get(namespace,namespace)

class ReadOnlyInstructionCache(instruction_cache.InstructionCache):
    """Same lookup/validation as InstructionCache, but the sqlite file is opened
    with mode=ro; misses are kept in an in-memory overlay, never written."""
    def __init__(self,path,namespace):
        uri='file:'+Path(path).resolve().as_posix()+'?mode=ro'
        self.connection=sqlite3.connect(uri,uri=True,timeout=600)
        self.namespace=namespace;self.hits=0;self.misses=0;self.overlay={}
    def get(self,offsets):
        key=self.key(offsets)
        if key in self.overlay:
            self.hits+=1;return json.loads(self.overlay[key])
        try:return super().get(offsets)
        except sqlite3.OperationalError:   # invalid row: original would DELETE it
            self.misses+=1;return None
    def put(self,offsets,records):
        self.overlay[self.key(offsets)]=json.dumps(records,separators=(',',':'))
    def close(self):self.connection.close()

def read_only_cache_factory(real_path):
    """Class usable in place of InstructionCache whose path argument is ignored."""
    class _Bound(ReadOnlyInstructionCache):
        def __init__(self,_path,namespace):super().__init__(real_path,namespace)
    return _Bound
