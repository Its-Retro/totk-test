"""Find nearby same-module function-pointer relocations for bounded AOT work."""

def nearby_function_pointers(relocations,target,text_size,image_size,radius=128,limit=64):
    if target%4 or not 0<=target<text_size or radius<0 or limit<=0:return []
    pointers={location:addend for location,kind,index,addend in relocations
              if kind==1027 and index==0 and location%8==0 and text_size<=location<=image_size-8
              and addend%4==0 and 0<=addend<text_size}
    anchors=sorted(location for location,addend in pointers.items() if addend==target)
    candidates=[];seen={target}
    for anchor in anchors:
        for location in sorted(range(max(0,anchor-radius)//8*8,anchor+radius+1,8),key=lambda value:(abs(value-anchor),value)):
            candidate=pointers.get(location)
            if candidate is None or candidate in seen:continue
            seen.add(candidate);candidates.append({'target':candidate,'reference':location,'anchor':anchor})
            if len(candidates)>=limit:return candidates
    return candidates
