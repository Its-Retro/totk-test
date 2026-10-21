#!/bin/bash
# batch-2: compiles the occlusion-report sum shader with the repository's DXC and embeds the DXIL in ../host_nvn_occl_dxil.h
# (as predicate-shaders/build.sh does for host_nvn_predicate_dxil.h). OUT overrides the target.
cd "$(dirname "$0")"
DXC=${DXC:-../../tools/dxc/bin/x64/dxc.exe}
PY=${PY:-python}
OUT=${OUT:-../host_nvn_occl_dxil.h}
$DXC -nologo -T cs_6_0 -E cs_occl_sum -O3 -Fo occl_sum.cs.dxil occl_sum.hlsl &&
$PY -I embed.py occl_sum.cs.dxil > "$OUT"
