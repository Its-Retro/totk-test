"""Move selected cold runtime handlers out of translated page headers."""
import re

HANDLERS = ('boot_valid', 'boot_attach_window', 'boot_thread_info', 'boot_set_thread_priority', 'boot_thread_affinity',
            'boot_new_session', 'boot_session_service', 'boot_cmif_control',
            'boot_cmif_domain', 'boot_audio_completion_worker', 'boot_schedule_audio_completion',
            'boot_send_sync', 'boot_set_memory_attribute', 'boot_query_memory',
            'boot_physical_memory', 'boot_set_heap_size', 'boot_alias_memory', 'boot_create_transfer',
            'boot_create_thread', 'boot_schedule_thread', 'boot_mutex', 'boot_condition_variable',
            'boot_wait_synchronization', 'boot_cancel_synchronization', 'boot_release_memory',
            'boot_svc_sleep', 'boot_svc_arbiter', 'boot_svc_kernel', 'boot_svc', 'boot_host_fn_dispatch')
TOKENS = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]', re.S)

def split_runtime(source):
    edits=[];bodies=[]
    for name in HANDLERS:
        pattern=rf'^static inline ([^\n{{;]+\b{re.escape(name)}\([^;{{]*\))\s*{{'
        matches=list(re.finditer(pattern,source,re.M))
        if len(matches)!=1:raise ValueError(f'Expected one runtime definition: {name}')
        match=matches[0];opening=match.end()-1;depth=0;end=None
        for token in TOKENS.finditer(source,opening):
            if token.group()=='{':depth+=1
            elif token.group()=='}':
                depth-=1
                if depth==0:end=token.end();break
        if end is None:raise ValueError(f'Unterminated runtime definition: {name}')
        signature=match.group(1)
        edits.append((match.start(),end,signature+';'))
        bodies.append(signature+source[opening:end])
    header=source
    for start,end,replacement in sorted(edits,reverse=True):header=header[:start]+replacement+header[end:]
    return header,'\n\n'.join(bodies)+'\n'
