#!/bin/bash
# Compiles the masked-clear shaders with the repository's DXC (same shader model as the translated shaders).
cd "$(dirname "$0")"
DXC=../../tools/dxc/bin/x64/dxc.exe
$DXC -nologo -T vs_6_6 -E vs_main -O3 -Fo clear.vs.dxil clear.hlsl &&
$DXC -nologo -T ps_6_6 -E ps_float -O3 -Fo clear-float.ps.dxil clear.hlsl &&
$DXC -nologo -T ps_6_6 -E ps_uint -O3 -Fo clear-uint.ps.dxil clear.hlsl &&
$DXC -nologo -T ps_6_6 -E ps_sint -O3 -Fo clear-sint.ps.dxil clear.hlsl
