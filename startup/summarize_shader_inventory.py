"""Recount instruction mnemonics from shader-programs/*.sass.

Strips predicate guards ("$p4", "not $p4", "never"), stops each program at its
trailing self-branch terminator (data such as constants follows it), and
records programs where no terminator was found. Updates game-shader-inventory.json.
"""
import collections,json,re
from pathlib import Path
ROOT=Path(__file__).resolve().parent
LINE=re.compile(r'^([0-9a-f]{8}):\s+[0-9a-f]{8} [0-9a-f]{8}\s+(?:[A-Z]\s+)?(.*)$')
GUARD=re.compile(r'^(?:not\s+)?(?:\$p[0-7]|\$pt|never|always)\s+')

def scan(text):
    ops=collections.Counter();unknown=0;terminated=False
    for line in text.splitlines():
        m=LINE.match(line)
        if not m:continue
        addr=int(m.group(1),16);body=m.group(2).strip()
        if body.startswith('sched'):continue
        body=GUARD.sub('',body)
        op=body.split()[0] if body else '???'
        if op=='bra' and body.split()[-1] in (hex(addr),hex(addr-8)):terminated=True;break  # envydis may print the target one word early
        if op.startswith('???') or '[unknown' in body:unknown+=1;op='???'
        ops[op]+=1
    return ops,unknown,terminated

def main():
    path=ROOT/'game-shader-inventory.json';report=json.loads(path.read_text(encoding='utf-8'))
    total=collections.Counter();stage=collections.defaultdict(collections.Counter);programs_using=collections.Counter()
    unknown_programs=0;unterminated=0
    for entry in report['programs']:
        ops,unknown,terminated=scan((ROOT/'shader-programs'/(entry['sha256'][:16]+'.sass')).read_text())
        entry.update(instructions=sum(ops.values()),unknown=unknown,terminated=terminated)
        total.update(ops);stage[entry['stage']].update(ops);programs_using.update(ops.keys())
        unknown_programs+=bool(unknown);unterminated+=not terminated
    report.update(total_unique_instructions=sum(total.values()),unknown_instruction_words=total['???'],
        programs_with_unknown=unknown_programs,programs_without_terminator=unterminated,
        mnemonics=dict(total.most_common()),programs_using_mnemonic=dict(programs_using.most_common()),
        mnemonics_by_stage={k:dict(v.most_common()) for k,v in stage.items()})
    path.write_text(json.dumps(report,indent=1)+'\n',encoding='utf-8')
    print(json.dumps({k:report[k] for k in ('unique_programs','programs_by_stage','total_unique_instructions',
        'unknown_instruction_words','programs_with_unknown','programs_without_terminator')},indent=1))
    print(len(total),'distinct mnemonics:')
    print(' '.join(f'{k}:{v}({programs_using[k]})' for k,v in total.most_common()))
if __name__=='__main__':main()
