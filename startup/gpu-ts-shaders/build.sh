#!/bin/bash
# G1: compiles the GPU-timestamp conversion shader with the repository's DXC and embeds the DXIL in ../host_nvn_gpu_ts_dxil.h
# (as occl-shaders/build.sh does for host_nvn_occl_dxil.h). OUT overrides the target.
cd "$(dirname "$0")"
DXC=${DXC:-../../tools/dxc/bin/x64/dxc.exe}
PY=${PY:-python}
OUT=${OUT:-../host_nvn_gpu_ts_dxil.h}
$DXC -nologo -T cs_6_0 -E cs_gpu_ts -O3 -Fo gpu_ts.cs.dxil gpu_ts.hlsl &&
$PY -I embed.py gpu_ts.cs.dxil > "$OUT"
