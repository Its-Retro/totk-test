"""Slim page header: what translated page units (and dispatch) include instead of boot_runtime.h.

Every translated page unit used to include all of boot_runtime.h and its local includes (scheduler,
fastmem, guest clock, multi-core, ...), and the object-cache key of every unit hashed that whole
header set, so any runtime edit recompiled all ~13k units. split_page_header derives, at build time
and from the unmodified sources, two header sets:

  page header (boot_page.h, one generated file): exactly what page/dispatch code needs: the macros,
      types and globals it uses, the bodies of the inline helpers pages inline (boot_block_check*,
      boot_load/boot_store fast paths, boot_access, flag helpers, boot_address/boot_offset, ...) and extern
      prototypes of every runtime function page code calls out of line (noinline helpers such as
      boot_tick_steps, boot_access_fm, boot_*_fm_slow/_legacy, boot_fault, the syscall handlers, ...).
      Bodies of out-of-line functions never enter the page header, so static locals in them stay single
      (runtime_support) instead of being duplicated per page unit. Comments are
      dropped, so comment edits never rebuild pages.
  runtime header set: the full sources, except that each function the page header declares extern
      loses `static`/`inline`, so runtime_support defines it once for the whole DLL.

Only page-header changes rebuild page units; a change confined to a function body that pages call
out of line (or never call) changes only the runtime header set, which only runtime_support depends on.

Rules (a mistake shows up as a compile error in page units, never as different behaviour):
  * PAGE_ROOTS: identifiers translated page code uses that the header set defines (from a scan of all
    13,263 page units of the latest build). DISPATCH_ROOTS: the same for the dispatch unit (prefix
    exports, boot_execute, boot_run/boot_resume). A new runtime helper emitted by the translator fails
    page compilation ("undeclared") until it is added to PAGE_ROOTS; `python split_page_header.py --scan
    native-object-cache/link-<sha>.rsp` lists names a build's units use that the lists miss.
  * A function reached from page code keeps its body in the page header (it is inlined into pages
    exactly as before) unless it is declared noinline, or listed in EXTERN; then it is extern.
    Functions reached only from dispatch are extern.
  * #if TOTK_FASTMEM / #ifdef TOTK_DRAW_PATH are resolved for the page header with the values page
    units are compiled with (TOTK_FASTMEM: -D flag or the host_fastmem.h default; TOTK_DRAW_PATH is
    a runtime_support-only define). The page header #errors if compiled with other values.
"""
import re
from pathlib import Path

# ---- roots ---------------------------------------------------------------------------------------
# Header-defined identifiers used by translated page code (scan of the 13,263 page units of the build
# linked by native-object-cache/link-4e452e73...rsp, 2026-10-15) plus the helpers the emitters in
# translate_startup.py / partition_native.py / *_emit.py / *_ext.py can generate.
PAGE_ROOTS = frozenset('''
BOOT_FP_SCOPE BOOT_PAGE_DISPATCH BOOT_SVC BOOT_SYSREG BOOT_TARGET BOOT_THREAD_CAPACITY BootContext CH_ARGUMENT
READ boot_access boot_access_read boot_add_finite32 boot_add_flags boot_address boot_block_check_steps boot_clz boot_fault
boot_load boot_load_exclusive boot_load_exclusive_pair boot_load_pair boot_load_thread boot_rbit
boot_save_thread boot_shift_jam boot_sign_extend boot_sp boot_store boot_store_bytes boot_store_exclusive
boot_store_exclusive_pair boot_store_pair boot_store_release boot_sub_flags boot_svc boot_system_tick
boot_tick_steps boot_umulh little_read little_write
boot_fp_recip_table boot_fp_rsqrt_table boot_fp_fma32 boot_fp_fma64
''' # emitted by partition_native.py / sleep_emit.py / address_arbiter_emit.py but absent from the scan
'''
boot_block_check boot_tick boot_tick_light boot_svc_arbiter boot_svc_sleep
BOOT_DIRECT_PAGE boot_call_page boot_call_indirect BOOT_DIRECT_CALLS BOOT_R BOOT_RL BOOT_WB BOOT_RDECL BOOT_FP_NATIVE BOOT_PAGE_LOCALS BOOT_PAGE_ENTER BOOT_STEP_AT BOOT_STEP BOOT_LEADER BOOT_LOOP_HEAD BOOT_STEPS_SYNC BOOT_CHECKPOINT_STOP
'''.split())
# Patch 58: the FP helpers the translator emits (boot_fpn_* inline wrappers, boot_fpx_*/boot_fps_* cold helpers of
# boot_fp_paths.h), listed by the generator that writes that header.
from float_emit import fp_page_roots
PAGE_ROOTS = PAGE_ROOTS | fp_page_roots()
# Header-defined identifiers used by the dispatch unit (translated_startup.c prefix exports and the
# partition_native dispatch tail).
DISPATCH_ROOTS = frozenset('''
BOOT_PAGE_TABLE_SIZE BootPageFunction BootStorage BootWindow CH_BUDGET boot_attach_window boot_context_size
boot_call_watermark boot_default_layout boot_mt_execute boot_offset boot_page_table boot_release_events boot_release_memory
boot_run boot_storage_open boot_test_subflags boot_valid boot_window_focus boot_window_handle
'''.split())
# Page-reached functions that are not noinline but stay out of line: the cold runtime handlers that
# split_runtime.py already kept out of page units (syscalls, IPC, memory management), and
#   boot_fault       never inlined (an out-of-line copy in every page object); it reaches the fastmem
#                    stop/sync code (boot_fm_on_stop), which would otherwise be page-visible.
#   boot_system_tick CNTPCT read (8 pages); reaches the whole guest clock (guest_clock.h, guest_time.h).
#   boot_save_thread / boot_load_thread  thread switch for inline-emitted yields (14 pages); scheduler code.
from split_runtime import HANDLERS
EXTERN = frozenset(HANDLERS) | frozenset(('boot_fault', 'boot_system_tick', 'boot_save_thread', 'boot_load_thread'))

KEYWORDS = frozenset('''auto break case char const continue default do double else enum extern float for goto if inline
int long register restrict return short signed sizeof static struct switch typedef union unsigned void volatile while
_Bool _Complex _Static_assert _Alignas _Alignof _Atomic _Generic _Noreturn _Thread_local __attribute__ __declspec
__typeof__ __builtin_expect __restrict __inline __int64 defined'''.split())
IDENT = re.compile(r'[A-Za-z_][A-Za-z0-9_]*')

def _strip_comments(text):
    """Comments -> one space (newlines kept); string/char literals unchanged."""
    out = []; i = 0; n = len(text)
    while i < n:
        c = text[i]
        if c == '/' and i + 1 < n and text[i+1] == '*':
            j = text.find('*/', i + 2)
            if j < 0: raise ValueError('unterminated comment')
            out.append(' ' + '\n' * text.count('\n', i, j)); i = j + 2
        elif c == '/' and i + 1 < n and text[i+1] == '/':
            j = text.find('\n', i)
            # a // comment ending in backslash continues: never seen in these headers, reject it
            if j > 0 and text[j-1] == '\\': raise ValueError('line comment with continuation')
            out.append(' '); i = n if j < 0 else j
        elif c in '"\'':
            j = i + 1
            while j < n and text[j] != c:
                if text[j] == '\\': j += 1
                elif text[j] == '\n': raise ValueError('unterminated literal')
                j += 1
            out.append(text[i:j+1]); i = j + 1
        else:
            j = i
            while j < n and text[j] not in '/"\'': j += 1
            if j == i: j = i + 1
            out.append(text[i:j]); i = j
    return ''.join(out)

def _skip_literals(text):
    """Text with string/char literal contents blanked (same length): for brace/identifier scans."""
    return re.sub(r'"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', lambda m: m.group()[0] + ' ' * (len(m.group()) - 2) + m.group()[-1], text)

# ---- conditional resolution ----------------------------------------------------------------------
def _eval_condition(kind, expression, defines):
    """1/0 when the condition only involves known macros, else None (kept verbatim)."""
    expression = expression.strip()
    if kind in ('ifdef', 'ifndef'):
        name = expression.split()[0]
        if name not in defines: return None
        value = defines[name] is not None
        return int(value if kind == 'ifdef' else not value)
    unknown = []
    def defined(m):
        name = m.group(1) or m.group(2)
        if name not in defines: unknown.append(name); return '0'
        return '1' if defines[name] is not None else '0'
    expr = re.sub(r'defined\s*\(\s*(\w+)\s*\)|defined\s+(\w+)', defined, expression)
    def ident(m):
        name = m.group()
        if name in defines: return '0' if defines[name] is None else defines[name]
        unknown.append(name); return '0'
    expr = IDENT.sub(ident, expr)
    # a known-false conjunct decides the whole && chain (defined(TOTK_DRAW_PATH) && defined(GUARD))
    if unknown and '||' not in expr and '&&' in expr:
        terms = [t.strip() for t in expr.split('&&')]
        if any(re.fullmatch(r'[(\s]*0[)\s]*', t) for t in terms) and not any('?' in t for t in terms): return 0
    if unknown: return None
    expr = expr.replace('&&', ' and ').replace('||', ' or ')
    expr = re.sub(r'!(?!=)', ' not ', expr)
    if not re.fullmatch(r'[\s0-9()<>=!+\-*/andortn]*', expr): raise ValueError('cannot evaluate #if ' + expression)
    return int(bool(eval(expr, {'__builtins__': {}})))

def _resolve_conditionals(text, defines, include=None, guards=frozenset()):
    """unifdef: resolve #if/#ifdef/#ifndef/#elif/#else whose conditions involve only `defines`.
    include(name) supplies the text spliced in for an emitted #include "name"; an emitted #define
    of a name in `guards` (include guards) marks it defined in `defines` from there on."""
    lines = text.split('\n'); out = []; stack = []   # entries: [mode, active, taken]; mode 'known'|'kept'|'dead'
    def emitting(): return all(e[1] for e in stack if e[0] in ('known', 'dead'))
    i = 0
    while i < len(lines):
        line = lines[i]; full = line
        while full.endswith('\\') and i + 1 < len(lines): i += 1; full += '\n' + lines[i]
        m = re.match(r'\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)', full, re.S)
        i += 1
        if not m:
            if not emitting(): continue
            inc = re.match(r'\s*#\s*include\s+"([^"]+)"', full)
            if inc and include: out.append(include(inc.group(1))); continue
            d = re.match(r'\s*#\s*define\s+(\w+)', full)
            if d and d.group(1) in guards: defines[d.group(1)] = '1'
            out.append(full); continue
        kind, rest = m.group(1), m.group(2)
        if kind in ('if', 'ifdef', 'ifndef'):
            if not emitting(): stack.append(['dead', False, True]); continue
            value = _eval_condition(kind, rest, defines)
            if value is None: stack.append(['kept', True, False]); out.append(full)
            else: stack.append(['known', bool(value), bool(value)])
        elif kind == 'elif':
            top = stack[-1]
            if top[0] == 'dead': continue
            if top[0] == 'kept':
                if _eval_condition('if', rest, defines) is not None: raise ValueError('#elif on a known macro after an unknown #if: ' + full)
                out.append(full); continue
            value = _eval_condition('if', rest, defines)
            if value is None: raise ValueError('#elif on an unknown macro after a known #if: ' + full)
            top[1] = not top[2] and bool(value); top[2] = top[2] or bool(value)
        elif kind == 'else':
            top = stack[-1]
            if top[0] == 'dead': continue
            if top[0] == 'kept': out.append(full); continue
            top[1] = not top[2]; top[2] = True
        else:
            top = stack.pop()
            if top[0] == 'kept' and emitting(): out.append(full)
    if stack: raise ValueError('unbalanced conditionals')
    return '\n'.join(out)

# ---- flattening and partitioning -----------------------------------------------------------------
def _include_guards(directories, read):
    guards = set()
    for directory in directories:
        for path in Path(directory).glob('*.h'):
            m = re.match(r'\s*#ifndef\s+(\w+)\s*\n\s*#define\s+\1\b', _strip_comments(read(path.resolve())))
            if m: guards.add(m.group(1))
    return guards

def _flatten(path, read, defines, seen, guards=frozenset()):
    """Local #include "x" spliced in once (headers are include-guarded); system includes kept.
    Include-guard macros of local headers count as known: defined once their header was spliced."""
    path = path.resolve(); seen.add(path)
    def include(name):
        target = (path.parent / name).resolve()
        return '' if target in seen else _flatten(target, read, defines, seen, guards)
    return _resolve_conditionals(_strip_comments(read(path)), defines, include, guards)

def _remove_groups(text, keyword):
    """Remove `keyword(...)` groups (balanced parentheses), e.g. __attribute__((noinline))."""
    while True:
        m = re.search(r'\b' + keyword + r'\s*\(', text)
        if not m: return text
        depth = 0
        for j in range(m.end() - 1, len(text)):
            if text[j] == '(': depth += 1
            elif text[j] == ')':
                depth -= 1
                if not depth: break
        text = text[:m.start()] + ' ' + text[j+1:]

def _remove_balanced(text, open_, close):
    out = []; depth = 0
    for ch in text:
        if ch == open_: depth += 1; continue
        if ch == close: depth -= 1; continue
        if not depth: out.append(ch)
    return ''.join(out)

def _split_top(text, sep=','):
    parts = []; depth = 0; start = 0
    for j, ch in enumerate(text):
        if ch in '([{': depth += 1
        elif ch in ')]}': depth -= 1
        elif ch == sep and not depth: parts.append(text[start:j]); start = j + 1
    parts.append(text[start:]); return parts

class Item:
    __slots__ = ('kind', 'text', 'head', 'names', 'refs', 'sig_refs', 'noinline')
    def __init__(self, kind, text, head=''):
        self.kind = kind; self.text = text; self.head = head; self.names = []; self.refs = set(); self.sig_refs = set(); self.noinline = False

def _idents(text):
    return {t for t in IDENT.findall(_skip_literals(text)) if t not in KEYWORDS}

def _clean(text):
    return _remove_groups(_remove_groups(text, '__attribute__'), '__declspec')

def _declarator_names(decl):
    """Names declared by a declaration without its trailing ';' (typedef or variable)."""
    body = _remove_balanced(decl, '{', '}')
    names = []
    for part in _split_top(body):
        part = part.split('=')[0] if '=' in _remove_balanced(part, '(', ')') else part
        m = re.search(r'\(\s*(?:\w+\s+)*\*\s*(\w+)\s*\)', part)   # function pointer declarator (WINAPI *name)
        if m: names.append(m.group(1)); continue
        part = _remove_balanced(part, '[', ']')
        ids = [t for t in IDENT.findall(part) if t not in KEYWORDS]
        if ids: names.append(ids[-1])
    return names

def _partition(text):
    items = []; n = len(text); i = 0
    blank = _skip_literals(text)
    while i < n:
        while i < n and text[i] in ' \t\r\n': i += 1
        if i >= n: break
        if text[i] == '#':
            j = i
            while True:
                k = text.find('\n', j)
                if k < 0: k = n; break
                if text[k-1] == '\\': j = k + 1; continue
                break
            items.append(Item('pp', text[i:k].rstrip())); i = k; continue
        depth = paren = 0; j = i; head = None
        while j < n:
            ch = blank[j]
            if ch == '\n' and j + 1 < n and not depth and not paren and text[j+1:j+2] == '#':
                raise ValueError('preprocessor directive inside a declaration: ' + text[i:j][:200])
            if ch in '([': paren += 1
            elif ch in ')]': paren -= 1
            elif ch == '{':
                if not depth and head is None: head = text[i:j]
                depth += 1
            elif ch == '}':
                depth -= 1
                if not depth and not paren and head is not None and _clean(head).rstrip().endswith(')') and '=' not in _remove_balanced(_clean(head), '(', ')'):
                    items.append(Item('func', text[i:j+1], head)); j += 1; break
            elif ch == ';' and not depth and not paren:
                items.append(Item('decl', text[i:j+1])); j += 1; break
            j += 1
        else:
            raise ValueError('unterminated declaration: ' + text[i:i+200])
        i = j
    return items

def _analyse(item):
    t = item.text
    if item.kind == 'pp':
        m = re.match(r'#\s*(\w+)\s*(.*)', t, re.S)
        directive, rest = m.group(1), m.group(2)
        item.kind = 'pp_' + directive
        if directive == 'define':
            dm = re.match(r'(\w+)(\([^)]*\))?', rest); item.names = [dm.group(1)]
            params = set(IDENT.findall(dm.group(2) or ''))
            item.refs = _idents(rest[dm.end():]) - {dm.group(1)} - params
        elif directive in ('if', 'ifdef', 'ifndef', 'elif'):
            item.refs = _idents(rest)
        return
    if item.kind == 'func':
        clean = _clean(item.head)
        m = re.search(r'(\w+)\s*\(', clean)
        if not m or m.group(1) in KEYWORDS: raise ValueError('cannot name function: ' + item.head[:200])
        item.names = [m.group(1)]
        item.noinline = bool(re.search(r'__attribute__\s*\(\([^)]*\bnoinline\b', item.head))
        item.refs = _idents(t) - set(item.names); item.sig_refs = _idents(item.head) - set(item.names)
        return
    clean = _clean(t[:-1]).strip()
    item.refs = _idents(t)
    if re.match(r'_Static_assert\b', clean): item.kind = 'assert'; return
    names = []
    for tag in re.finditer(r'\b(?:struct|union|enum)\s+(\w+)\s*\{', clean): names.append(tag.group(1))
    for em in re.finditer(r'\benum\b[^{;]*\{', clean):
        start = em.end(); depth = 1; j = start
        while depth:
            depth += {'{': 1, '}': -1}.get(clean[j], 0); j += 1
        for part in _split_top(clean[start:j-1]):
            part = part.strip()
            if part: names.append(IDENT.match(part).group())
    flat = _remove_balanced(clean, '{', '}')
    if re.match(r'typedef\b', flat):
        item.kind = 'typedef'; names += _declarator_names(clean[len('typedef'):])
    else:
        fm = re.fullmatch(r'([^=]*?)\b(\w+)\s*\(([^=]*)\)', flat.strip(), re.S)
        if fm and '(*' not in fm.group(1) and fm.group(2) not in KEYWORDS:
            item.kind = 'proto'; names.append(fm.group(2))
            item.noinline = bool(re.search(r'__attribute__\s*\(\([^)]*\bnoinline\b', t))
            item.sig_refs = _idents(t) - {fm.group(2)}
        else:
            stripped = re.sub(r'^\s*(?:(?:static|extern|const|volatile|struct|union|enum)\b\s*)*', '', flat)
            if stripped.strip() and (re.search(r'\w', stripped)) and not re.fullmatch(r'\s*\w+\s*', stripped):
                item.kind = 'var'; names += _declarator_names(re.sub(r'^\s*(?:static|extern)\b', '', clean))
            elif names: item.kind = 'type'
            else: item.kind = 'type'; names += _declarator_names(clean)
    item.names = names
    item.refs -= set(names)

# ---- the split -----------------------------------------------------------------------------------
def fastmem_default(startup):
    m = re.search(r'#ifndef TOTK_FASTMEM\s*\n\s*#define TOTK_FASTMEM\s+(\w+)', (Path(startup) / 'host_fastmem.h').read_text(encoding='utf-8'))
    if not m: raise ValueError('host_fastmem.h: TOTK_FASTMEM default not found')
    return m.group(1)

def play_default(startup):
    '''TOTK_PLAY for the page header: env TOTK_PLAY (0/1) if set, else the play_config.h default.'''
    import os
    value = os.environ.get('TOTK_PLAY')
    if value in ('0', '1'): return value
    m = re.search(r'#ifndef TOTK_PLAY\s*\n\s*#define TOTK_PLAY\s+(\w+)', (Path(startup) / 'play_config.h').read_text(encoding='utf-8'))
    if not m: raise ValueError('play_config.h: TOTK_PLAY default not found')
    return m.group(1)

def fixed_default(startup):
    '''TOTK_FM_FIXED for the page header: env TOTK_FM_FIXED (0/1) if set, else the host_fastmem.h default.'''
    import os
    value = os.environ.get('TOTK_FM_FIXED')
    if value in ('0', '1'): return value
    m = re.search(r'#ifndef TOTK_FM_FIXED\s*\n\s*#define TOTK_FM_FIXED\s+(\w+)', (Path(startup) / 'host_fastmem.h').read_text(encoding='utf-8'))
    return m.group(1) if m else '0'

def flatten(startup, read, fastmem):
    """boot_runtime.h with its local includes spliced in, comments removed and the page-unit
    configuration (TOTK_FASTMEM, no TOTK_DRAW_PATH, include guards) resolved."""
    guards = _include_guards((startup, startup.parent / 'call-chain'), read)
    defines = {'TOTK_FASTMEM': fastmem, 'TOTK_DRAW_PATH': None, 'TOTK_PLAY': play_default(startup), 'TOTK_FM_FIXED': fixed_default(startup)}
    defines.update({g: None for g in guards})
    return _flatten(startup / 'boot_runtime.h', read, defines, set(), guards)

def split_page_header(startup, read=None, fastmem=None, page_roots=PAGE_ROOTS, dispatch_roots=DISPATCH_ROOTS, extern=EXTERN):
    """(page_header_text, info). read(path)->str supplies (possibly overridden) sources. info['static_extern']
    lists the functions runtime_overrides must make external (info['extern'] also has already-external ones)."""
    startup = Path(startup).resolve()
    read = read or (lambda p: p.read_text(encoding='utf-8'))
    fastmem = fastmem or fastmem_default(startup)
    items = _partition(flatten(startup, read, fastmem))
    for item in items: _analyse(item)
    defs = {}
    for index, item in enumerate(items):
        for name in item.names: defs.setdefault(name, []).append(index)
    noinline = {n for it in items if it.noinline for n in it.names}
    functions = {n for it in items if it.kind in ('func', 'proto') for n in it.names}
    bodies = {n for it in items if it.kind == 'func' for n in it.names}   # prototype-only: defined elsewhere
    kept = set(); mode = {}   # mode: function name -> 'inline' | 'extern'
    def visit(roots, page):
        work = [r for r in roots if r in defs]
        while work:
            name = work.pop()
            if name in functions:
                want = 'inline' if page and name in bodies and name not in noinline and name not in extern else 'extern'
                if mode.get(name) == 'inline' or mode.get(name) == want: continue
                mode[name] = want
                for index in defs[name]:
                    item = items[index]; kept.add(index)
                    refs = item.refs if want == 'inline' and item.kind == 'func' else item.sig_refs
                    work.extend(r for r in refs if r in defs)
                continue
            for index in defs[name]:
                if index in kept: continue
                kept.add(index); work.extend(r for r in items[index].refs if r in defs)
    # unnamed items (conditionals, system includes, pragmas, static asserts) are always kept
    always = [ix for ix, it in enumerate(items) if not it.names]
    visit({r for ix in always for r in items[ix].refs}, True)
    visit(page_roots, True)
    visit(dispatch_roots, False)
    missing_inline = [n for n, m in mode.items() if m == 'inline' and not any(items[ix].kind == 'func' for ix in defs[n])]
    if missing_inline: raise ValueError('page-inline functions without a definition: ' + ', '.join(sorted(missing_inline)))
    extern_names = sorted(n for n, m in mode.items() if m == 'extern')
    out = []
    for index, item in enumerate(items):
        if index not in kept and item.names: continue
        if item.kind in ('func', 'proto') and mode.get(item.names[0]) == 'extern':
            head = item.head if item.kind == 'func' else item.text[:-1]
            if item.kind == 'func' and not any(items[ix].kind == 'proto' for ix in defs[item.names[0]] if ix < index) or item.kind == 'proto':
                out.append(_extern_prototype(head, item.names[0]) + ';')
            continue
        out.append(item.text)
    body = '\n'.join(_squeeze(t) for t in out)
    header = ('/* GENERATED by startup/split_page_header.py from boot_runtime.h and its local includes for\n'
              ' * translated page units and dispatch: do not edit, do not include from hand-written code. */\n'
              '#ifndef TOTK_BOOT_PAGE_H\n#define TOTK_BOOT_PAGE_H\n'
              '#ifdef TOTK_DRAW_PATH\n#error "boot_page.h: TOTK_DRAW_PATH is a runtime_support-only define"\n#endif\n'
              f'#if defined(TOTK_FASTMEM) && TOTK_FASTMEM!={fastmem}\n#error "boot_page.h was generated for TOTK_FASTMEM={fastmem}"\n#endif\n'
              f'#ifndef TOTK_FASTMEM\n#define TOTK_FASTMEM {fastmem}\n#endif\n'
              f'#if defined(TOTK_PLAY) && TOTK_PLAY!={play_default(startup)}\n#error "boot_page.h was generated for TOTK_PLAY={play_default(startup)}"\n#endif\n'
              f'#ifndef TOTK_PLAY\n#define TOTK_PLAY {play_default(startup)}\n#endif\n'
              f'#if defined(TOTK_FM_FIXED) && TOTK_FM_FIXED!={fixed_default(startup)}\n#error "boot_page.h was generated for TOTK_FM_FIXED={fixed_default(startup)}"\n#endif\n'
              f'#ifndef TOTK_FM_FIXED\n#define TOTK_FM_FIXED {fixed_default(startup)}\n#endif\n'
              + body + '\n#endif\n')
    static_extern = sorted(n for n in extern_names if any(re.match(r'\s*static\b', items[ix].text) for ix in defs[n]))
    info = {'inline': sorted(n for n, m in mode.items() if m == 'inline'), 'extern': extern_names, 'static_extern': static_extern,
            'kept_items': len(kept), 'items': len(items)}
    return header, info

def _squeeze(text):
    return '\n'.join(line.rstrip() for line in text.split('\n') if line.strip())

def _extern_prototype(head, name):
    """`static inline __attribute__((noinline)) int f(...)` -> `__attribute__((noinline)) int f(...)`."""
    at = re.search(r'\b' + re.escape(name) + r'\s*\(', head).start()
    prefix = re.sub(r'\b(?:static|inline)\b\s*', '', head[:at])
    return (prefix + head[at:]).strip()

def externalize(source, names):
    """Runtime header text with `static`/`inline` dropped from every top-level declaration or
    definition (a line starting with `static`) of the functions in `names`."""
    names = set(names); found = set()
    def fix(m):
        line = m.group(0)
        clean = _clean(line)
        fm = re.search(r'(\w+)\s*\(', clean)
        if not fm or fm.group(1) not in names: return line
        found.add(fm.group(1))
        at = re.search(r'\b' + re.escape(fm.group(1)) + r'\s*\(', line).start()
        return re.sub(r'\b(?:static|inline)\b\s*', '', line[:at]) + line[at:]
    return re.sub(r'^static\b[^\n]*', fix, source, flags=re.M), found

def runtime_overrides(startup, extern_names, read=None):
    """{path: text} for the whole runtime header set: every local header reachable from boot_runtime.h
    (as read(path) supplies it), with the extern functions made external. Raises if an extern function
    is not found."""
    startup = Path(startup).resolve()
    read = read or (lambda p: p.read_text(encoding='utf-8'))
    out = {}; found = set(); seen = set()
    def walk(path):
        path = path.resolve()
        if path in seen: return
        seen.add(path); text = read(path)
        new, hit = externalize(text, extern_names); found.update(hit)
        out[path] = new
        for name in re.findall(r'^\s*#\s*include\s+"([^"]+)"', text, re.M): walk(path.parent / name)
    walk(startup / 'boot_runtime.h')
    missing = set(extern_names) - found
    if missing: raise ValueError('extern functions not found in the runtime sources: ' + ', '.join(sorted(missing)))
    return out

def _read_with_window_facade(startup):
    from split_host_window import split_host_window
    window = (startup / 'host_window.h').resolve()
    facade, _ = split_host_window(window.read_text(encoding='utf-8'))
    return lambda p: facade if p == window else p.read_text(encoding='utf-8')

if __name__ == '__main__':
    # split_page_header.py              summary of the page header for the current sources
    # split_page_header.py --scan RSP   names page/dispatch units of a build (native-object-cache link-*.rsp;
    #                                   the unit sources are the .c files next to the objects) use that the
    #                                   header set defines but PAGE_ROOTS/DISPATCH_ROOTS do not list
    import sys
    startup = Path(__file__).resolve().parent
    read = _read_with_window_facade(startup)
    if len(sys.argv) == 3 and sys.argv[1] == '--scan':
        items = _partition(flatten(startup, read, fastmem_default(startup)))
        for item in items: _analyse(item)
        defined = {n for item in items for n in item.names}
        used = {}
        for line in open(sys.argv[2]):
            path = Path(line.strip().strip('"')).with_suffix('.c'); name = path.name.split('-')[0]
            if not (name.startswith('page_') or name == 'dispatch'): continue
            text = _strip_comments(path.read_text(encoding='utf-8').split('\n', 1)[1])
            for ident in set(IDENT.findall(_skip_literals(text))) & defined: used.setdefault(ident, set()).add(name == 'dispatch')
        missing = sorted(n for n in used if n not in PAGE_ROOTS | DISPATCH_ROOTS)
        print('missing page roots:', ' '.join(n for n in missing if False in used[n]) or '-')
        print('missing dispatch roots:', ' '.join(n for n in missing if used[n] == {True}) or '-')
    else:
        header, info = split_page_header(startup, read)
        print(f"page header: {len(header)} bytes, {header.count(chr(10))} lines; {len(info['inline'])} inline, {len(info['extern'])} extern")
        print('inline:', ' '.join(info['inline'])); print('extern:', ' '.join(info['extern']))
