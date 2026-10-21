"""Partition our generated static labels; no guest opcode decoding at runtime."""
import re
from pathlib import Path

class PartitionOutput:
    """Write exactly the old joined format without retaining emitted pages."""
    def __init__(self,stream):
        self.stream=stream
        self.first=True
    def append(self,chunk):
        chunk=chunk.replace('uint64_t address=0,writeback=0,discard0=0,discard1=0,target=boot_address(c,entry_offset);',
                            'uint64_t target=boot_address(c,entry_offset);').replace('    (void)discard0;(void)discard1;','')
        if not self.first:self.stream.write('\n')
        self.stream.write(chunk)
        self.first=False
    def extend(self,chunks):
        for chunk in chunks:self.append(chunk)

# Runtime helpers that never read or write c->steps and never touch guest memory (checked in
# boot_runtime.h). A statement that passes c only to these needs no store of the step-count local
# before it. Every other helper receiving c gets c->steps stored before the statement: helpers that
# may read it (scheduler, guest clock in boot_system_tick) and every guest-memory helper, because
# a fastmem access fault (host_fastmem.h VEH) leaves the page function without running its code,
# so c->steps must already be exact whenever a guest access can fault.
STEP_NEUTRAL=frozenset(('boot_address','boot_offset','boot_fault','boot_sp','boot_sub_flags','boot_add_flags'))
# Last lines of every page function (translate_startup_incremental checks for them).
PAGE_TRAILER=('if(0)goto indirect;if(0)goto transfer;if(0)goto boot_checkpoint_;',
              'indirect:','*direct=0;','transfer:',
              '*next=target;BOOT_STEPS_SYNC(c);','BOOT_PAGE_DISPATCH(c,next,direct);',
              'boot_checkpoint_:',
              'BOOT_CHECKPOINT_STOP(c);','}')
TICK=re.compile(r'    if\(!boot_tick(_light)?\(c,UINT64_C\((\d+)\)\)\)return c->status;\n')
# Page entries. A page function's entry switch only lists the instructions that can be entered other
# than by straight-line flow inside the page (entry_points.py has the static rules); every other
# label is reached by its predecessor's fall-through only, so the compiler sees real basic blocks.
# Local rules (entry_set): budget-check sites (block leaders: a CH_BUDGET stop resumes there), the
# instruction after BL/BLR/SVC (return and thread-switch resume sites) and resumable stop sites:
# an instruction whose code can stop with a status boot_resume accepts (BOOT_SVC, BOOT_SYSREG,
# BOOT_TARGET, CH_BUDGET literals, or any helper receiving c that is not listed in NONRESUMABLE).
# BOOT_FP_SCOPE is deliberately not a rule (every FP instruction checks it; such a stop needs
# emitter work and a rebuild, and the resume then takes one untranslated-entry round).
# Entering a page at a selected non-entry stops cleanly: BOOT_TARGET at that address with
# fault_width 1 (0 = untranslated); the coverage loop records it in discovered-entries.json.
NONRESUMABLE=frozenset(('boot_address','boot_offset','boot_fault','boot_sp','boot_sub_flags','boot_add_flags',
    'boot_access','boot_load','boot_store','boot_load_pair','boot_store_pair','boot_store_bytes',
    'boot_load_exclusive','boot_store_exclusive','boot_load_exclusive_pair','boot_store_exclusive_pair'))
RESUMABLE_STATUS=re.compile(r'(?<![A-Za-z0-9_])(?:BOOT_SVC|BOOT_SYSREG|BOOT_TARGET|CH_BUDGET)(?![A-Za-z0-9_])')
RETURN_SITE=re.compile(r'L_[0-9a-f]+: /\* (?:bl|blr|svc) ')
CALLEE=re.compile(r'([A-Za-z_][A-Za-z0-9_]*)\(c(?=\s*[,)])')
def resumable_stop(block):
    """True if the instruction block can stop with a resumable status at its own address."""
    if RESUMABLE_STATUS.search(block):return True
    bare=BARE_C.findall(block);direct=CALLEE.findall(block)
    if len(bare)==len(direct) and all(name in NONRESUMABLE for name in direct):return False
    return any(name not in NONRESUMABLE for name in context_callees(block))
def entry_set(page_blocks,leaders,forced):
    """Offsets of one page that get an entry-switch case (see the comment above NONRESUMABLE)."""
    entries=set(leaders)|{offset for offset in forced if offset in page_blocks}
    for offset,block in page_blocks.items():
        if RETURN_SITE.match(block) and offset+4 in page_blocks:entries.add(offset+4)
        tick=TICK.search(block)   # the instruction's own code follows its boot_tick line
        if tick and resumable_stop(block[tick.end():]):entries.add(offset)
    return entries
def inbound_targets(source,start,end):
    """Offsets reached by a goto from another page (direct transfers between pages), full-source form."""
    inbound=set();page=None
    for match in re.compile(r'^L_([0-9a-f]+):|goto L_([0-9a-f]+);',re.M).finditer(source,start,end):
        if match.group(1):page=int(match.group(1),16)>>12
        else:
            destination=int(match.group(2),16)
            if destination>>12!=page:inbound.add(destination)
    return inbound
# Direct guest calls (patch 56, play-work/DIRECT-CALLS.md). Applied to a page's instruction blocks after its
# entries are known: BL to a translated destination and BLR become C calls of the destination page function
# (boot_call_page / boot_call_indirect); RET returns from the page function to whoever owns the host frame.
# With BOOT_DIRECT_CALLS=0 (probe build) the call helpers are constant 0 and the RET branch is dead: the
# old transfers below them run unchanged.
CALL_LABEL=re.compile(r'L_([0-9a-f]+): /\* (bl|blr|ret)\b')
CALL_GOTO=re.compile(r'goto L_([0-9a-f]+);')
def call_rewrite(block,page,known,entries,local):
    """block: one instruction block (raw emitted form). known: selected offsets (or None: any
    destination with a label); entries: this page's entry offsets; local: this page's offsets."""
    m=CALL_LABEL.match(block)
    if not m:return block
    offset=int(m.group(1),16);kind=m.group(2);ret=offset+4
    # continuation after a call that returned to ret: straight-line code if ret is in this page and is an
    # entry (a resume after any stop needs it anyway), else the generic transfer of the target (== ret)
    back=f'goto L_{ret:x};' if ret in local and ret in entries else '{*direct=0;goto transfer;}'
    tail=f'case 1:{back}case 2:{{*direct=0;goto transfer;}}case 3:return c->status;default:break;}}'
    if kind=='ret':
        if block.count('goto indirect;')!=1 or 'target=c->x[30];' not in block:return block
        return block.replace('goto indirect;','if(BOOT_DIRECT_CALLS){next[0]=target;*direct=0;return c->status;}\n    goto indirect;')
    if kind=='blr':
        if block.count('goto indirect;')!=1:return block
        return block.replace('goto indirect;',f'switch(boot_call_indirect(c,next,target,boot_address(c,UINT64_C({ret})),&target)){{{tail}\n    goto indirect;')
    gotos=CALL_GOTO.findall(block)
    if len(gotos)!=1:return block
    destination=int(gotos[0],16)
    if destination>>12==page:
        if destination not in local or destination not in entries:return block   # not enterable: keep the goto
    elif known is not None and destination not in known:return block              # untranslated: old fault path
    call=(f'switch(boot_call_page(c,next,boot_page_table[{destination>>12:#x}],boot_address(c,UINT64_C({destination})),'
          f'UINT64_C({destination}),boot_address(c,UINT64_C({ret})),&target)){{{tail}\n    ')
    at=block.rindex(f'goto L_{destination:x};')
    return block[:at]+call+block[at:]
BARE_C=re.compile(r'(?<![A-Za-z0-9_])c(?![A-Za-z0-9_])(?!\s*->)')
FIRST_ARG_C=re.compile(r'([A-Za-z_][A-Za-z0-9_]*)\(c(?=\s*[,)])')
def context_callees(text):
    """Names of the functions whose argument lists contain the bare context pointer c ('' if none)."""
    names=[];stack=[];i=0;n=len(text)
    while i<n:
        ch=text[i]
        if ch in '"\'':
            j=i+1
            while j<n and text[j]!=ch:j+=2 if text[j]=='\\' else 1
            i=j+1;continue
        if text.startswith('/*',i):i=text.index('*/',i)+2;continue
        if ch=='(':
            k=i-1
            while k>=0 and text[k]==' ':k-=1
            m=k
            while m>=0 and (text[m].isalnum() or text[m]=='_'):m-=1
            stack.append(text[m+1:k+1]);i+=1;continue
        if ch==')':
            if stack:stack.pop()
            i+=1;continue
        if ch=='c' and (i==0 or not (text[i-1].isalnum() or text[i-1]=='_')) and (i+1==n or not (text[i+1].isalnum() or text[i+1]=='_')):
            j=i+1
            while j<n and text[j]==' ':j+=1
            if not text.startswith('->',j):names.append(stack[-1] if stack else '')
        i+=1
    return names
def local_steps(block,entry=True,loop=False):
    """Rewrite one instruction block for its page function's boot_steps_/boot_ckpt_ locals. Step and
    checkpoint code is emitted as macros (boot_runtime.h): TOTK_PLAY=0 expands them to the former text,
    the play build (play_config.h) to nothing but a poll of the run token c->limit at page entries and
    loop heads (loop: a leader that a goto of the page reaches backwards)."""
    match=TICK.search(block)
    if not match:
        # translate_startup_incremental's synthetic placeholder labels (never emitted pages)
        if re.fullmatch(r'L_[0-9a-f]+: /\* placeholder \*/\n',block):return block
        raise ValueError('instruction block without tick')
    offset=int(match.group(2))
    head,body=block[:match.start()],block[match.end():]
    if any(name in body for name in ('boot_steps_','boot_ckpt_','boot_checkpoint_','boot_entry_','boot_next_')):raise ValueError('reserved name in emitted code')
    if 'c->steps' in body or 'c->checkpoint_offset' in body:raise ValueError('emitted code uses c->steps')
    if match.group(1):   # boot_tick_light: step count; only page entries test the checkpoint (offset 0 never matches)
        tick=f'    BOOT_STEP_AT({offset});\n' if offset and entry else '    BOOT_STEP;\n'
    else:
        tick=f'    BOOT_LOOP_HEAD(c,{offset});\n' if loop else f'    BOOT_LEADER(c,{offset});\n'
    # Fast path: every bare c is the first argument of a directly named step-neutral helper (the
    # common emitted form); otherwise the exact scanner decides.
    bare=BARE_C.findall(body);direct=FIRST_ARG_C.findall(body)
    neutral=len(bare)==len(direct) and all(name in STEP_NEUTRAL for name in direct)
    if not neutral and any(name not in STEP_NEUTRAL for name in context_callees(body)):tick+='    BOOT_STEPS_SYNC(c);\n'
    return head+tick+body.replace('return c->status;','{BOOT_STEPS_SYNC(c);return c->status;}')

# Region-local guest registers (play build). A region is a straight-line run of instruction blocks that
# starts at a label any goto or the entry switch can reach (or at the first block of the page) and ends
# before the next such label: blocks inside a region are only entered from their predecessor. Inside a
# region the general registers live in C locals (BOOT_R(n) = r_n): each is loaded once (BOOT_RL) at the
# start of the first block that touches it, and the registers the region wrote are stored back (BOOT_WB)
# before every exit: a goto out of the region, goto transfer/indirect, and every return. A block that calls
# a helper which may read or write the registers through c (svc, system registers, host functions, ...;
# anything not in CACHE_SAFE), or indexes c->x non-constantly, is a barrier: dirty registers are stored
# before it and it keeps c->x. The probe build (TOTK_PLAY=0) expands BOOT_R(n) to c->x[n] and the other
# macros to nothing, so its code is unchanged. Guest memory faults (fastmem VEH) are terminal in the play
# build, so registers held in locals at a fault need not reach c.
CACHE_SAFE=frozenset(('boot_address','boot_offset','boot_fault','boot_sp','boot_sub_flags','boot_add_flags',
    'boot_access','boot_load','boot_store','boot_load_pair','boot_store_pair','boot_store_bytes',
    'boot_load_exclusive','boot_store_exclusive','boot_load_exclusive_pair','boot_store_exclusive_pair','boot_store_release',
    'BOOT_STEPS_SYNC','BOOT_LEADER','BOOT_LOOP_HEAD','BOOT_DIRECT_PAGE'))   # step/poll macros never touch c->x
XREF=re.compile(r'c->x\[(\d+)\]')
XDEF=re.compile(r'(&\s*c->x\[(\d+)\])|c->x\[(\d+)\]\s*(?:<<|>>|[-+*/%&|^])?=(?!=)')
XANY=re.compile(r'c->x(?!\[\d+\])')
EXIT=re.compile(r'(?:BOOT_DIRECT_PAGE\(c,next,direct,\d+\);)?goto (L_[0-9a-f]+|transfer|indirect);|return c->status;')
def cache_registers(offsets,texts,starts):
    """texts: offset -> block text (after transfer rewriting); starts: region-starting offsets.
    Returns (new texts, registers used)."""
    out={};used=set();dirty=set();loaded=set();previous=None
    def wb(regs):return ''.join(f'BOOT_WB({n});' for n in sorted(regs))
    for offset in offsets:
        text=texts[offset]
        if offset in starts or previous is None or previous!=offset-4:dirty=set();loaded=set()
        previous=offset
        callees=context_callees(text)
        barrier=bool(XANY.search(text)) or any(name not in CACHE_SAFE for name in callees)
        lines=text.split('\n')
        # the tick line (first line starting with BOOT_ after the label) stays first
        at=next((i for i,l in enumerate(lines) if l.lstrip().startswith('BOOT_')),None)
        if at is None:out[offset]=text;continue
        if barrier:
            pre=wb(dirty)
            if pre:lines.insert(at+1,'    '+pre)
            dirty=set();loaded=set();out[offset]=text if not pre else '\n'.join(lines);continue
        refs={int(n) for n in XREF.findall(text)}
        defs={int(m.group(2) or m.group(3)) for m in XDEF.finditer(text)}
        need=(refs|defs)-loaded
        body='\n'.join(lines[at+1:])
        body=re.sub(r'c->x\[(\d+)\]',lambda m:f'BOOT_R({m.group(1)})',body)
        written=dirty|defs
        nxt=f'L_{offset+4:x}'
        def exit_(m):
            if m.group(1)==nxt and offset+4 in texts and offset+4 not in starts:return m.group(0)   # stays in the region
            return '{'+wb(written)+m.group(0)+'}' if written else m.group(0)
        body=EXIT.sub(exit_,body)
        head='\n'.join(lines[:at+1])
        loads=''.join(f'BOOT_RL({n});' for n in sorted(need))
        out[offset]=head+('\n    '+loads if loads else '')+'\n'+body
        used|=refs|defs;loaded|=need;dirty=written
    return out,used

def entry_points_by_page(offsets):
    pages={}
    for offset in offsets:pages.setdefault(offset>>12,set()).add(offset)
    return pages
def partition(source,output=None,entries=None):
    """entries: offsets forced to be page entries (cross-page goto targets, entry_points static and
    discovered entries); None computes them from the whole source (full generator)."""
    marker='__declspec(dllexport) uint32_t boot_run(BootContext*c,uint64_t entry_offset){'
    marker_at=source.index(marker)
    body_at=marker_at+len(marker)
    first=source.index('\nL_',body_at)
    label_end=source.index('\nindirect:',first)
    prefix=source[:marker_at]
    initialization=source[body_at:first]
    # Scan the original string by bounds, avoiding corpus-sized body slices.
    label_pattern=re.compile(r'^L_([0-9a-f]+):([^\n]*)\n',re.M)
    # Keep only the target addresses; C blocks are consumed a page at a time.
    known_offsets={int(match.group(1),16) for match in label_pattern.finditer(source,first,label_end)
                   if '/*' in match.group(2)}
    if not known_offsets:raise ValueError('No generated instruction labels')
    if entries is None:
        import entry_points
        entries=inbound_targets(source,first,label_end)|set(entry_points.static_entries())|set(entry_points.extra_entries())
    forced=entry_points_by_page(entries)
    result=[] if output is None else PartitionOutput(output)
    result.append(prefix)
    page_numbers=[]
    page=None
    page_blocks={}
    def flush_page():
        if page is None:return
        offsets=sorted(page_blocks)
        page_numbers.append(page)
        # Page entry. The caller (boot_execute or another page's dispatch) passes the target address
        # in next[0] and its boot_offset in next[1], so the offset is computed once per transfer.
        # The guest step count and the checkpoint offset live in locals for the whole call: no
        # runtime helper writes c->steps or c->checkpoint_offset, so c->steps is stored back before
        # every return and before any statement passing c to a helper that may read it.
        head=[];head.extend([f'static __attribute__((noinline)) uint32_t boot_page_{page:x}(BootContext*c,uint64_t*next,uint32_t*direct){{',
                       'uint64_t target=next[0],address=0,writeback=0,discard0=0,discard1=0;',
                       '(void)address;(void)writeback;(void)discard0;(void)discard1;',
                       'BOOT_PAGE_LOCALS(c,next);',
                       'BOOT_PAGE_ENTER(c);',
                       'switch((boot_entry_>>2)&1023){'])   # instruction slot: dense cases 0..1023 (a jump table)
        # Leaders: instructions a goto reaches other than their predecessor's trailing fall-through
        # (they keep the full boot_tick: budget/permission/trace checks).
        references={}
        for offset in offsets:
            for target in re.findall(r'goto L_([0-9a-f]+);',page_blocks[offset]):
                references[int(target,16)]=references.get(int(target,16),0)+1
        leaders=set()
        for offset in offsets:
            count=references.get(offset,0)
            previous_block=page_blocks.get(offset-4)
            if previous_block is not None and previous_block.rstrip().endswith(f'goto L_{offset:x};'):count-=1
            if count>0:leaders.add(offset)
        # Loop heads: leaders some goto of this page reaches backwards (or a self-loop). The play build
        # polls the run token only there and at the page entry: every guest loop passes one of them.
        loop_heads={int(t,16) for offset in offsets for t in re.findall(r'goto L_([0-9a-f]+);',page_blocks[offset]) if int(t,16)<=offset}
        # An instruction no goto of the page reaches (its predecessor is not translated or does not
        # fall through) can only be entered from outside: always an entry (and its label is used).
        entry=entry_set(page_blocks,leaders,forced.get(page,()))|{offset for offset in offsets if not references.get(offset)}
        # Cases are instruction slots, not offsets: LLVM only builds a jump table for a switch whose
        # cases are dense in their value range, and ~17% of 1024 offsets spaced 4 apart are not
        # (it fell back to a compare tree on every page entry).
        head.extend(f'case {(offset>>2)&1023}:goto L_{offset:x};' for offset in offsets if offset in entry)
        # Not an entry: fault_width 1 marks a translated (selected) instruction entered from outside
        # its straight-line flow (untranslated entry); 0 an untranslated instruction, as before.
        bits=[0]*32
        for offset in offsets:bits[(offset&0xfff)>>7]|=1<<((offset>>2)&31)
        head.extend(['default:{static const uint32_t boot_selected_[32]={'+','.join(f'{b:#x}' for b in bits)+'};',
                       'uint64_t boot_slot_=(boot_entry_>>2)&1023;',
                       'boot_fault(c,BOOT_TARGET,target,target,(boot_entry_>>12)==UINT64_C(%d) && (boot_selected_[boot_slot_>>5]>>(boot_slot_&31)&1));return c->status;}' % page,
                       '}'])
        def transfer(match):
            destination=int(match.group(1),16)
            if destination in known_offsets and destination>>12==page:return match.group(0)
            if destination not in known_offsets:
                address=f'boot_address(c,UINT64_C({destination}))'
                return f'{{boot_fault(c,BOOT_TARGET,{address},{address},0);BOOT_STEPS_SYNC(c);return c->status;}}'
            # Known destination page: tail-call it directly through the page table (BOOT_DIRECT_PAGE), skipping
            # the dispatch lookup; a hooked page (host function) still takes the generic transfer.
            return f'{{target=boot_address(c,UINT64_C({destination}));*direct=1;BOOT_DIRECT_PAGE(c,next,direct,{destination});goto transfer;}}'
        # Match once per native page, preserving the exact separators used by
        # the per-instruction output. This avoids a Python regex dispatch for
        # every translated instruction on every coverage-expansion round.
        # Block leaders keep the full boot_tick: any instruction a goto reaches other than its
        # predecessor's trailing fall-through. Page entries are checked by boot_block_check above;
        # every other instruction only counts its step (boot_tick_light).
        for offset in offsets:
            if offset not in leaders:
                tick=f'if(!boot_tick(c,UINT64_C({offset})))return c->status;'
                if page_blocks[offset].count(tick)==1:
                    page_blocks[offset]=page_blocks[offset].replace(tick,f'if(!boot_tick_light(c,UINT64_C({offset})))return c->status;')
        local=set(offsets)
        texts={offset:re.sub(r'goto L_([0-9a-f]+);',transfer,local_steps(call_rewrite(page_blocks[offset],page,known_offsets,entry,local),offset in entry,offset in loop_heads)) for offset in offsets}
        texts,used=cache_registers(offsets,texts,entry|leaders)
        # register locals are declared before the entry switch (a jump into their scope skips no initializer)
        at=head.index('BOOT_PAGE_ENTER(c);')+1
        if used:head.insert(at,'BOOT_RDECL('+','.join(f'r_{n}=0' for n in sorted(used))+');')
        result.extend(head)
        result.append('\n'.join(texts[offset] for offset in offsets))
        # Some pages have only direct control flow. Keep both transfer labels
        # referenced without introducing a runtime instruction decoder.
        # A transfer continues in the destination page by a tail call: BOOT_PAGE_DISPATCH does
        # exactly what one more iteration of boot_execute would, without growing the stack.
        result.extend(PAGE_TRAILER)
        page_blocks.clear()
    previous=None
    for match in label_pattern.finditer(source,first,label_end):
        if previous is not None and '/*' in previous.group(2):
            offset=int(previous.group(1),16)
            next_page=offset>>12
            if page is not None and next_page!=page:
                if next_page<page:raise ValueError('Instruction labels are not ordered by page')
                flush_page()
            page=next_page
            page_blocks[offset]=source[previous.start():match.start()]
        previous=match
    if previous is not None and '/*' in previous.group(2):
        offset=int(previous.group(1),16)
        next_page=offset>>12
        if page is not None and next_page!=page:
            if next_page<page:raise ValueError('Instruction labels are not ordered by page')
            flush_page()
        page=next_page
        page_blocks[offset]=source[previous.start():label_end]
    flush_page()
    # Cross-page transfers return to one loop rather than recursing. Temporary
    # emitter variables are instruction-local; all guest CPU state is in c.
    initialization=initialization.replace('    goto indirect;','')
    # Pages by boot_offset>>12; a null entry is an untranslated page (BOOT_TARGET fault).
    # Host functions (host_fn.h): registry startup on entry; targets without a page or in a marked
    # page are offered to boot_host_fn_dispatch, as in BOOT_PAGE_DISPATCH.
    result.extend(['static uint32_t boot_execute(BootContext*c,uint64_t target){',
                   'uint64_t state[3]={target,0,boot_call_watermark()};uint32_t direct=0;boot_host_fn_dispatch(c,NULL,NULL);','for(;;){',
                   'if(!direct && state[0]==c->return_sentinel)return c->status;',
                   'uint64_t offset=boot_offset(c,state[0]);',
                   'BootPageFunction function=offset<(BOOT_PAGE_TABLE_SIZE<<12)?boot_page_table[offset>>12]:NULL;',
                   'if(!function || boot_host_fn_mark[offset>>12]){if(boot_host_fn_dispatch(c,state,&direct)){if(c->status)return c->status;continue;}'
                   'if(!function){boot_fault(c,BOOT_TARGET,state[0],state[0],0);return c->status;}}',
                   'state[1]=offset;function(c,state,&direct);',
                   'if(c->status)return c->status;','}','}',
                   'const BootPageFunction boot_page_table[BOOT_PAGE_TABLE_SIZE]={'])
    result.extend(f'[{page:#x}]=boot_page_{page:x},' for page in page_numbers)
    result.extend(['};',
                   marker,initialization,'return boot_mt_execute(c,target,boot_execute);','}',
                   '__declspec(dllexport) uint32_t boot_resume(BootContext*c,uint64_t address){',
                   'if(!c || (c->status!=BOOT_TARGET && c->status!=BOOT_SYSREG && c->status!=BOOT_FP_SCOPE && c->status!=CH_BUDGET && !(c->status==BOOT_SVC && (c->svc_id==0x03 || c->svc_id==0x21 || c->svc_id==0x29 || c->svc_id==0x1a || c->svc_id==0x34 || c->svc_id==0x16))) || address!=c->fault_pc)return CH_ARGUMENT;',
                   'if(!boot_valid(c)){c->status=CH_ARGUMENT;return c->status;}',
                   'c->status=0;c->fault_pc=c->fault_address=c->fault_width=0;',
                   'return boot_mt_execute(c,address,boot_execute);','}',''])
    # The outer wrapper no longer needs per-instruction temporary values.
    # Clean individual chunks before joining, avoiding two additional
    # corpus-sized allocations while the input and page chunks are live.
    if output is not None:return
    result=[chunk.replace('uint64_t address=0,writeback=0,discard0=0,discard1=0,target=boot_address(c,entry_offset);',
                          'uint64_t target=boot_address(c,entry_offset);').replace(
                          '    (void)discard0;(void)discard1;','') for chunk in result]
    return '\n'.join(result)

if __name__=='__main__':
    root=Path(__file__).resolve().parent
    (root/'partitioned_startup.c').write_text(partition((root/'monolithic_startup.c').read_text()))
