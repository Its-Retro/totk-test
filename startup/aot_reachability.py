"""Bounded static reachability; never executes or interprets guest instructions."""
from collections import deque

def expand_supported(image,text_size,logical_base,seeds,covered,selected,decoder,emitter,budget):
    """Return at most budget new supported offsets, following direct edges.

    Decoding stays in this module's text. Indirect targets are not guessed.
    Unsupported operations terminate only their own path. Call return sites
    are included independently of the direct call target.
    """
    pending=deque(sorted(seeds));visited=set();additions=set()
    while pending and len(additions)<budget:
        offset=pending.popleft()
        if offset in visited or offset in covered or offset%4 or not 0<=offset<=text_size-4:continue
        visited.add(offset)
        instruction=next(decoder.disasm(bytes(image[offset:offset+4]),logical_base+offset),None)
        if instruction is None or instruction.size!=4:continue
        try:_,edges,_=emitter(instruction)
        except ValueError:continue
        if offset not in selected:additions.add(offset)
        successors=[edge-logical_base for edge in edges]
        if instruction.mnemonic in ('bl','blr'):successors.append(offset+4)
        pending.extend(successors)
    return additions
