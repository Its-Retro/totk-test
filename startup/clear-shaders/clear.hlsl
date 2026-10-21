// Masked colour clears (host_gpu_render_draw.h boot_render_clear_masked): D3D12 clears whole
// texels, so a clear of some channels draws a full-target triangle with the channel write mask.
// Compiled ahead of time (clear-shaders/build.sh); bound with the renderer's graphics root
// signature: the clear value is pixel-stage constant buffer b0.
cbuffer ClearValue : register(b0) { float4 value_f; uint4 value_u; };
float4 vs_main(uint id : SV_VertexID) : SV_Position {
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * 2.0f - 1.0f, 0.0f, 1.0f);
}
float4 ps_float(float4 pos : SV_Position) : SV_Target0 { return value_f; }
uint4 ps_uint(float4 pos : SV_Position) : SV_Target0 { return value_u; }
int4 ps_sint(float4 pos : SV_Position) : SV_Target0 { return (int4)value_u; }
