"""Bounded speculative AOT coverage in verified executable text only.

This selects instructions for compilation, never for execution. Unknown
instructions remain absent from the dispatch registry and still stop at runtime.
"""

def supported_neighborhood(image,text_size,logical_base,target,covered,selected,decoder,emit,budget=2048,radius=4096):
    if budget<=0 or radius<0 or target%4 or not 0<=target<text_size or text_size>len(image):return set()
    low=max(0,target-radius);low=(low+3)&~3
    high=min(text_size,target+radius+4)
    result=set()
    for offset in sorted(range(low,high-3,4),key=lambda p:(abs(p-target),p)):
        if offset in covered or offset in selected:continue
        decoded=list(decoder.disasm(bytes(image[offset:offset+4]),logical_base+offset))
        if len(decoded)!=1 or decoded[0].size!=4:continue
        try:emit(decoded[0])
        except ValueError:continue
        result.add(offset)
        if len(result)>=budget:break
    return result
