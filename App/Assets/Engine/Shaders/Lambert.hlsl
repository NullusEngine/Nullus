#include "CommonTypes.hlsli"
#include "LightGridCommon.hlsli"

cbuffer FrameConstants : register(b0, space0)
{
    float4x4 u_ViewProjection;
    float3 u_CameraWorldPos;
    float u_Time;
    float4x4 u_ViewProjectionNoTranslation;
};

StructuredBuffer<float4x4> ObjectData : register(t0, space3);

cbuffer MaterialConstants : register(b0, space2)
{
    float4 u_Diffuse;
    float2 u_TextureTiling;
    float2 u_TextureOffset;
};

Texture2D u_DiffuseMap : register(t0, space2);
SamplerState u_LinearWrapSampler : register(s0, space2);
StructuredBuffer<uint> u_ForwardLocalLightBuffer : register(t0, space1);
StructuredBuffer<uint> u_NumCulledLightsGrid : register(t1, space1);
StructuredBuffer<uint> u_CulledLightDataGrid : register(t2, space1);

VSOutput VSMain(VSInput input, uint instanceId : SV_InstanceID)
{
    VSOutput output;
    const float4x4 model = ObjectData[u_ObjectIndex + instanceId];

    const float4 worldPosition = mul(model, float4(input.Position, 1.0f));
    output.PositionCS = mul(u_ViewProjection, worldPosition);
    output.PositionWS = worldPosition.xyz;
    output.NormalWS = normalize(mul((float3x3)model, input.Normal));
    output.TangentWS = mul((float3x3)model, input.Tangent);
    output.BitangentWS = mul((float3x3)model, input.Bitangent);
    output.TexCoord = input.TexCoord;
    return output;
}

float4 PSMain(VSOutput input) : SV_Target0
{
    const float2 tiledTexCoord = u_TextureOffset + frac(input.TexCoord * u_TextureTiling);
    const float4 diffuse = u_DiffuseMap.Sample(u_LinearWrapSampler, tiledTexCoord) * u_Diffuse;
    const float3 normalWS = normalize(input.NormalWS);
    const float3 lighting = saturate(NLSAccumulateClusteredLightingPhong(
        u_ForwardLocalLightBuffer,
        u_NumCulledLightsGrid,
        u_CulledLightDataGrid,
        input.PositionWS,
        normalWS,
        diffuse.rgb,
        0.0f.xxx,
        1.0f));
    return float4(lighting, diffuse.a);
}
