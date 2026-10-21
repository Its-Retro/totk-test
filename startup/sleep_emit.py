"""SleepThread (svc 0x0b): the body is boot_svc_sleep in boot_runtime.h (bounded real/guest
timer sleep and yield for the native scheduler); this emits the call for tests and callers."""
def emit_sleep(pc,next_pc):
    return [f'if(boot_svc_sleep(c,{pc},{next_pc},&target)<0)return c->status;']
