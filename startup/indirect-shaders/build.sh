#!/bin/bash
# render-38d: compiles the indirect-argument pre-pass with the repository's DXC and embeds the DXIL in the generated
# block of host_gpu_render.h (embed_into.py). TARGET overrides the header to update.
cd "$(dirname "$0")"
DXC=${DXC:-../../tools/dxc/bin/x64/dxc.exe}
PY=${PY:-python}
TARGET=${TARGET:-../host_gpu_render.h}
$DXC -nologo -T cs_6_0 -E cs_indirect -O3 -Fo indirect.cs.dxil indirect.hlsl &&
$PY -B embed_into.py indirect.cs.dxil "$TARGET"
