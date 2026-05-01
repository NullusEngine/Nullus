#include "CommonTypes.hlsli"
#include "LightGridCommon.hlsli"

cbuffer FrameConstants : register(b0, space0)
{
    float4x4 u_ViewProjection;
    float3 u_CameraWorldPos;
    float u_Padding0;
    float4x4 u_ViewProjectionNoTranslation;
};

cbuffer ObjectConstants : register(b0, space3)
{
    float4x4 u_Model;
};

cbuffer MaterialConstants : register(b0, space2)
{
    float4 u_Albedo;
    float u_Metallic;
    float u_Roughness;
    float u_AmbientOcclusion;
    float u_EnableNormalMapping;
};

Texture2D u_AlbedoMap : register(t0, space2);
Texture2D u_MetallicMap : register(t1, space2);
Texture2D u_RoughnessMap : register(t2, space2);
Texture2D u_AmbientOcclusionMap : register(t3, space2);
Texture2D u_NormalMap : register(t4, space2);
SamplerState u_LinearWrapSampler : register(s0, space2);
StructuredBuffer<uint> u_LightGridLights : register(t0, space1);
StructuredBuffer<uint> u_LightGridClusterRecords : register(t1, space1);
StructuredBuffer<uint> u_LightGridCompactIndices : register(t2, space1);

VSOutput VSMain(VSInput input)
{
    VSOutput output;

    const float4 worldPosition = mul(u_Model, float4(input.Position, 1.0f));
    output.PositionCS = mul(u_ViewProjection, worldPosition);
    output.PositionWS = worldPosition.xyz;
    output.NormalWS = normalize(mul((float3x3)u_Model, input.Normal));
    output.TangentWS = normalize(mul((float3x3)u_Model, input.Tangent));
    output.BitangentWS = normalize(mul((float3x3)u_Model, input.Bitangent));
    output.TexCoord = input.TexCoord;
    return output;
}

float4 PSMain(VSOutput input) : SV_Target0
{
    const float4 albedoSample = u_AlbedoMap.Sample(u_LinearWrapSampler, input.TexCoord);
    const float3 albedo = albedoSample.rgb * u_Albedo.rgb;
    const float metallic = u_MetallicMap.Sample(u_LinearWrapSampler, input.TexCoord).r * u_Metallic;
    const float roughness = u_RoughnessMap.Sample(u_LinearWrapSampler, input.TexCoord).r * u_Roughness;
    const float ao = u_AmbientOcclusionMap.Sample(u_LinearWrapSampler, input.TexCoord).r * u_AmbientOcclusion;

    const float3 normalWS = normalize(input.NormalWS);
    const float3 lighting = NLSAccumulateClusteredLightingPBR(
        u_LightGridLights,
        u_LightGridClusterRecords,
        u_LightGridCompactIndices,
        input.PositionWS,
        normalWS,
        albedo,
        metallic,
        roughness,
        ao);
    return float4(lighting, u_Albedo.a);
}
