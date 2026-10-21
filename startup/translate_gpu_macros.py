"""Source macro programs -> native C control flow, ahead of time.

Covers the full Maxwell macro ISA: ALU (add/addc/sub/subb/xor/or/and/andnot/
nand with carry), add-immediate, extract-insert, both extract-shift-left forms,
read and branch, with every result assignment. Field layout and semantics
follow the documented macro format (yuzu macro_interpreter / Ryujinx
MacroInterpreter). Register-amount shifts >= 32 stop instead of guessing.
"""
import hashlib,json,struct
from pathlib import Path
ROOT=Path(__file__).resolve().parent
ALU={0:'add',1:'addc',2:'sub',3:'subb',8:'xor',9:'or',10:'and',11:'andnot',12:'nand'}
def emit_program(index,words):
    n=len(words);out=[f'static int boot_gpu_macro_{index}(BootGpuMacroIo*io){{',
        'uint32_t r[8]={0},defined=3,method=0,increment=0,has_method=0,carry=0;io->consumed=io->steps=io->error=0;(void)carry;(void)method;(void)increment;(void)has_method;',
        'if(!boot_gpu_macro_param(io,&r[1]))return 0;goto L_0;']
    def reg(which):
        return f'if(!(defined&(1u<<{which}))){{io->error=4;return 0;}}'
    def body(pc):
        if not 0<=pc<n:raise ValueError('Macro fallthrough out of bounds')
        word=words[pc];op=word&7;assign=(word>>4)&7;dst=(word>>8)&7;a=(word>>11)&7;b=(word>>14)&7
        imm=word>>14;imm=imm-(1<<18) if imm&(1<<17) else imm
        src=(word>>17)&31;size=(word>>22)&31;dest=(word>>27)&31;mask=(1<<size)-1
        s=['if(++io->steps>100000){io->error=5;return 0;}',reg(a),'{uint32_t value;']
        if op==0:
            alu=(word>>17)&31
            if alu not in ALU:raise ValueError(f'Undefined macro ALU {alu} at {pc}')
            s.append(reg(b));x,y=f'r[{a}]',f'r[{b}]'
            if alu in (0,1):
                s.append(f'{{uint64_t t=(uint64_t){x}+{y}'+('+carry' if alu==1 else '')+';carry=t>>32;value=(uint32_t)t;}')
            elif alu in (2,3):
                s.append(f'{{uint64_t t=(uint64_t){x}-{y}'+('-(1u-carry)' if alu==3 else '')+';carry=t<UINT64_C(0x100000000);value=(uint32_t)t;}')
            else:
                s.append('value='+{8:f'{x}^{y}',9:f'{x}|{y}',10:f'{x}&{y}',11:f'{x}&~{y}',12:f'~({x}&{y})'}[alu]+';')
        elif op==1:s.append(f'value=r[{a}]+UINT32_C({imm&0xffffffff});')
        elif op==2:
            s.extend([reg(b),f'value=(r[{a}]&~UINT32_C({(mask<<dest)&0xffffffff}))|(((r[{b}]>>{src})&UINT32_C({mask}))<<{dest});'])
        elif op==3:
            # ((rA >> rB) & mask) << dst_bit
            s.extend([reg(b),f'if(r[{b}]>31){{io->error=9;return 0;}}',
                f'value=(uint32_t)(((r[{a}]>>r[{b}])&UINT32_C({mask}))<<{dest});'])
        elif op==4:
            # ((rA >> src_bit) & mask) << rB
            s.extend([reg(b),f'if(r[{b}]>31){{io->error=9;return 0;}}',
                f'value=(uint32_t)(((r[{a}]>>{src})&UINT32_C({mask}))<<r[{b}]);'])
        elif op==5:
            s.append(f'if(!boot_gpu_macro_read(io,r[{a}]+UINT32_C({imm&0xffffffff}),&value))return 0;')
        else:raise ValueError(f'Unsupported macro opcode {op} at {pc}')
        setreg=f'if({dst}){{r[{dst}]=value;defined|=1u<<{dst};}}'
        setmethod='method=value&4095;increment=(value>>12)&63;has_method=1;'
        send='if(!has_method){io->error=6;return 0;}if(!boot_gpu_macro_send(io,&method,increment,value))return 0;'
        load=f'if(!boot_gpu_macro_param(io,&value))return 0;{setreg}'
        if assign==0:s.append(load)
        elif assign==1:s.append(setreg)
        elif assign==2:s.append(setreg+setmethod)
        elif assign==3:s.append('{uint32_t fetched;if(!boot_gpu_macro_param(io,&fetched))return 0;'+send+f'value=fetched;{setreg}}}')
        elif assign==4:s.append(setreg+send)
        elif assign==5:s.append(setmethod+load)
        elif assign==6:s.append(setreg+setmethod+'if(!boot_gpu_macro_param(io,&value))return 0;'+send)
        elif assign==7:s.append(setreg+setmethod+'value=(value>>12)&63;'+send)
        s.append('}');return s
    def target(pc,exiting=False):
        if not 0<=pc<n:raise ValueError('Macro branch outside uploaded program')
        return f'goto {"E" if exiting else "L"}_{pc};'
    exits=set()
    for pc,word in enumerate(words):
        out.append(f'L_{pc}: __attribute__((unused));')
        if word&7==7:
            a=(word>>11)&7;offset=word>>14;offset=offset-(1<<18) if offset&(1<<17) else offset;dest=pc+offset
            condition=f'r[{a}]'+('!=0' if word&16 else '==0')
            out.extend(['if(++io->steps>100000){io->error=5;return 0;}',reg(a),f'if({condition}){{'])
            if word&32:out.append(target(dest))
            else:
                out.extend(body(pc+1));exit_delay=bool(words[pc+1]&128)
                if exit_delay:exits.add(dest)
                out.append(target(dest,exit_delay))
            out.append('}')
            if word&128:exits.add(pc+1)
            out.append(target(pc+1,bool(word&128)))
        else:
            out.extend(body(pc))
            if word&128:exits.add(pc+1)
            if pc+1<n:out.append(target(pc+1,bool(word&128)))
            else:out.append('io->error=7;return 0;')
    for pc in sorted(exits):
        out.append(f'E_{pc}: __attribute__((unused));');out.extend(body(pc))
        out.append('if(io->consumed!=io->count){io->error=8;return 0;}return 1;')
    out.append('}');return '\n'.join(out)
def main():
    folder=ROOT/'gpu-capture';manifest=json.loads((folder/'macros.json').read_text());parts=['#ifndef TOTK_GENERATED_GPU_MACROS_H','#define TOTK_GENERATED_GPU_MACROS_H','#include "host_gpu_macro.h"'];compiled=[]
    seen=set()
    for item in manifest:
        data=(folder/item['file']).read_bytes()
        if hashlib.sha256(data).hexdigest()!=item['sha256']:raise ValueError('Macro source hash changed')
        if item['sha256'] in seen:continue
        seen.add(item['sha256'])
        words=struct.unpack('<'+'I'*(len(data)//4),data)
        parts.append(f'static const uint32_t boot_gpu_macro_words_{item["index"]}[]={{'+','.join(f'UINT32_C({w})' for w in words)+'};')
        parts.append(emit_program(item['index'],words));compiled.append(item)
    # Programs are matched by uploaded content, not by slot: the game binds macro
    # slots at runtime, and unknown uploads must stop rather than run.
    parts.append('typedef struct BootGpuMacroProgram{const uint32_t*words;uint32_t count;int(*run)(BootGpuMacroIo*);}BootGpuMacroProgram;')
    parts.append('static const BootGpuMacroProgram boot_gpu_macro_programs[]={'+
        ','.join(f'{{boot_gpu_macro_words_{x["index"]},{x["words"]},boot_gpu_macro_{x["index"]}}}' for x in compiled)+'};')
    parts.extend(['#endif','']);(ROOT/'generated_gpu_macros.h').write_text('\n'.join(parts))
    (folder/'macro-aot-provenance.json').write_text(json.dumps(compiled,indent=2)+'\n')
    print('Generated native control flow for macros:',[x['index'] for x in compiled])
if __name__=='__main__':main()
