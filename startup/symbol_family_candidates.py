"""Bounded AOT candidates from defined, named functions near an observed PC."""
import re
def family(name):
    # Recognize only simple length-prefixed nested names. Unrecognized C++
    # forms are skipped; this is a selection hint, never an execution target.
    match=re.match(r'^_ZN[KVR]*',name)
    if not match:return None
    position=match.end();parts=[]
    while position<len(name):
        length=re.match(r'[0-9]+',name[position:])
        if not length:break
        position+=len(length[0]);size=int(length[0])
        if not size or position+size>len(name):return None
        parts.append(name[position:position+size]);position+=size
    return tuple(parts[:2]) if len(parts)>=3 else None
def related_functions(symbols,pc,text_size,limit=64):
    if pc%4 or not 0<=pc<text_size or limit<=0:return []
    valid=[s for s in symbols if s.get('section') and s.get('type')==2 and
           s['value']%4==0 and s['size']%4==0 and 0<s['size']<=16384 and
           0<=s['value']<=text_size-s['size']]
    anchors=sorted((s for s in valid if s['value']<=pc<s['value']+s['size']),key=lambda s:s['size'])
    if not anchors:return []
    group=family(anchors[0]['name'])
    if group is None:return []
    result=[];seen=set()
    for s in sorted(valid,key=lambda s:(abs(s['value']-pc),s['value'])):
        if s['value'] not in seen and family(s['name'])==group:
            seen.add(s['value']);result.append(s)
            if len(result)==limit:break
    return result
