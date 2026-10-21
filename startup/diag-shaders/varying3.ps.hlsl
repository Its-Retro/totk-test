struct PsIn { float4 pos : SV_Position; float4 g0 : GENERIC0; float4 g1 : GENERIC1; float4 g2 : GENERIC2; };
float4 main(PsIn i) : SV_Target0 { return float4(i.g0.x, i.g1.x, i.g2.x, 1.0f); }
