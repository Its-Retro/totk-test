"""Expose a stable opaque window ABI to translated instruction pages."""
import re

def split_host_window(source):
    signatures=re.findall(r'^static inline ([^\n{;]+\bboot_window_\w+\([^;{]*\))\s*{',source,re.M)
    if not signatures or not any('boot_window_close(' in s for s in signatures):
        raise ValueError('Missing host window API')
    facade='#ifndef TOTK_HOST_WINDOW_H\n#define TOTK_HOST_WINDOW_H\n#include <windows.h>\n#include <stdint.h>\ntypedef struct BootWindow BootWindow;\n'
    facade+='\n'.join(signature+';' for signature in signatures)+'\n#endif\n'
    implementation=source.replace('TOTK_HOST_WINDOW_H','TOTK_HOST_WINDOW_BACKEND_H')
    implementation=re.sub(r'^static inline (?=[^\n{;]+\bboot_window_)','',implementation,flags=re.M)
    return facade,implementation
