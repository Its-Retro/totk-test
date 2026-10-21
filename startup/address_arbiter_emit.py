"""WaitForAddress / SignalToAddress (svc 0x34 / 0x35): the body is boot_svc_arbiter in
boot_runtime.h; this emits the call for tests and callers."""
def emit_address_arbiter(pc,next_pc,svc):
    return [f'if(boot_svc_arbiter(c,{pc},{next_pc},{svc},&target)<0)return c->status;']
