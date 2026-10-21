#!/bin/bash
# Compiles the GPU tiling shaders with the repository's DXC and embeds the DXIL in ../host_gpu_tile_dxil.h.
cd "$(dirname "$0")"
DXC=${DXC:-../../tools/dxc/bin/x64/dxc.exe}
PY=${PY:-python}
$DXC -nologo -T cs_6_0 -E cs_tile -O3 -Fo tile.cs.dxil tile.hlsl &&
$DXC -nologo -T cs_6_0 -E cs_detile -O3 -Fo detile.cs.dxil tile.hlsl &&
$PY embed.py tile.cs.dxil detile.cs.dxil > ../host_gpu_tile_dxil.h
