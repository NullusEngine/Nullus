#include "CommonTypes.hlsli"
#include "NullusShaderLibrary/SceneDebug.hlsl"
cbuffer FrameConstants : register(b0, space0)
{
    float4x4 u_ViewProjection; float3 u_CameraWorldPos; float u_Time;
    float4x4 u_ViewProjectionNoTranslation;
};
StructuredBuffer<float4x4> ObjectData : register(t0, space3);
StructuredBuffer<uint> InstanceObjectIndices : register(t1, space3);
cbuffer MaterialConstants : register(b0, space2)
{
    float4 u_Albedo; float u_Metallic; float u_Roughness; float u_AmbientOcclusion; float u_EnableNormalMapping;
    float4 u_Emissive; float4 u_Specular; float4 u_MetallicMapChannel; float4 u_RoughnessMapChannel;
};
Texture2D u_AlbedoMap : register(t0, space2);
Texture2D u_MetallicMap : register(t1, space2);
Texture2D u_OpacityMap : register(t5, space2);
SamplerState u_LinearWrapSampler : register(s0, space2);
struct Output { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Output VSMain(VSInput input, uint instanceId : SV_InstanceID)
{
#if defined(NLS_SKINNED)
    input.Position = NLSSkinPosition(input.Position, input.skinJoints, input.skinWeights);
#endif
    Output output;
    float4x4 model = ObjectData[InstanceObjectIndices[u_ObjectIndex + instanceId]];
    output.position = mul(u_ViewProjection, mul(model, float4(input.Position, 1)));
    output.uv = input.TexCoord;
    return output;
}
float4 PSMain(Output input) : SV_Target0
{
    float4 base = u_AlbedoMap.Sample(u_LinearWrapSampler, input.uv) * u_Albedo;
    base.a *= u_OpacityMap.Sample(u_LinearWrapSampler, input.uv).r;
    float metallic = u_Metallic * dot(u_MetallicMap.Sample(u_LinearWrapSampler, input.uv), u_MetallicMapChannel);
    uint width,height; u_AlbedoMap.GetDimensions(width,height);
    return NLSSceneDebug(base.rgb, metallic, base.a, NLSSceneTextureLod(input.uv, float2(width,height)));
}
