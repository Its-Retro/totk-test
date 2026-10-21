#!/bin/bash
# bc7-work: compiles the BC7 encoder (bc7_encode.hlsl, ported from DirectXTex BC7Encode.hlsl: MIT, see
# ../../THIRD-PARTY-NOTICES.md) with the repository's DXC and embeds the DXIL in ../host_bc7_dxil.h.
# -HV 2018: upstream relies on the legacy scope of for-loop variables (written for FXC).
# OUT overrides the target.
cd "$(dirname "$0")"
DXC=${DXC:-../../tools/dxc/bin/x64/dxc.exe}
PY=${PY:-python}
OUT=${OUT:-../host_bc7_dxil.h}
for e in TryMode456CS TryMode137CS TryMode02CS EncodeBlockCS; do
    $DXC -nologo -T cs_6_0 -HV 2018 -E $e -O3 -Fo bc7_$e.cs.dxil bc7_encode.hlsl || exit 1
done
$PY -I embed.py bc7_TryMode456CS.cs.dxil bc7_TryMode137CS.cs.dxil bc7_TryMode02CS.cs.dxil bc7_EncodeBlockCS.cs.dxil > "$OUT"
