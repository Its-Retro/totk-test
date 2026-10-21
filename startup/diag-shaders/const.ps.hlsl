struct PsOut { float4 c0 : SV_Target0; float4 c1 : SV_Target1; float4 c2 : SV_Target2; float4 c3 : SV_Target3;
               float4 c4 : SV_Target4; float4 c5 : SV_Target5; float4 c6 : SV_Target6; float4 c7 : SV_Target7; };
PsOut main(float4 pos : SV_Position) {
    PsOut o; float4 c = float4(1.0f, 0.0f, 1.0f, 1.0f);
    o.c0 = c; o.c1 = c; o.c2 = c; o.c3 = c; o.c4 = c; o.c5 = c; o.c6 = c; o.c7 = c; return o;
}
