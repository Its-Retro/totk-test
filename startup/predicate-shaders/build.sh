#!/bin/bash
# render-38c: compiles the conditional-render predicate shader with the repository's DXC and embeds the DXIL in
# ../host_nvn_predicate_dxil.h (as tile-shaders/build.sh does for host_gpu_tile_dxil.h). OUT overrides the target.
cd "$(dirname "$0")"
DXC=${DXC:-../../tools/dxc/bin/x64/dxc.exe}
PY=${PY:-python}
OUT=${OUT:-../host_nvn_predicate_dxil.h}
$DXC -nologo -T cs_6_0 -E cs_predicate -O3 -Fo predicate.cs.dxil predicate.hlsl &&
$PY embed.py predicate.cs.dxil > "$OUT"
