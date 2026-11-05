Texture2D<float4> u_SceneDebugTexture : register(t0, space2);
SamplerState u_SceneDebugSampler : register(s0, space2);
cbuffer MaterialConstants : register(b0, space2) { int u_ResolveMode; float u_OverdrawMax; float2 u_Padding; };
struct Input { float3 position : POSITION; float2 uv : TEXCOORD0; };
struct Output { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Output VSMain(Input input) { Output output; output.position = float4(input.position.xy, 0, 1); output.uv = input.uv; return output; }
float4 PSMain(Output input) : SV_Target0
{
    float4 value = u_SceneDebugTexture.Sample(u_SceneDebugSampler, input.uv);
    if (value.r < 0 || value.a < 0) return float4(0.35f, 0.35f, 0.35f, 1);
    if (u_ResolveMode == 6) return float4(value.aaa, 1);
    if (u_ResolveMode == 7)
    {
        if (value.r < 0.5f) return float4(0, 0, 0, 1);
        float heat = saturate(value.r / max(u_OverdrawMax, 1));
        float3 color = heat < 1.0f / 3 ? lerp(float3(0, 0.15f, 1), float3(0, 1, 1), heat * 3) :
            heat < 2.0f / 3 ? lerp(float3(0, 1, 1), float3(1, 1, 0), heat * 3 - 1) :
            lerp(float3(1, 1, 0), float3(1, 0, 0), heat * 3 - 2);
        return float4(color, 1);
    }
    return float4(value.rgb, 1);
}
