#include "CommonTypes.hlsli"

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
    float4 u_Albedo;
    float u_Metallic;
    float u_Roughness;
    float u_AmbientOcclusion;
    float u_EnableNormalMapping;
    float4 u_Emissive;
    float4 u_Specular;
};

Texture2D u_AlbedoMap : register(t0, space2);
Texture2D u_MetallicMap : register(t1, space2);
Texture2D u_RoughnessMap : register(t2, space2);
Texture2D u_AmbientOcclusionMap : register(t3, space2);
Texture2D u_NormalMap : register(t4, space2);
Texture2D u_OpacityMap : register(t5, space2);
Texture2D u_EmissiveMap : register(t6, space2);
Texture2D u_SpecularMap : register(t7, space2);
SamplerState u_LinearWrapSampler : register(s0, space2);

struct GBufferOutput
{
    float4 Albedo : SV_Target0;
    float4 Normal : SV_Target1;
    float4 Material : SV_Target2;
};

VSOutput VSMain(VSInput input, uint instanceId : SV_InstanceID)
{
    VSOutput output;
    const float4x4 model = ObjectData[u_ObjectIndex + instanceId];

    const float4 worldPosition = mul(model, float4(input.Position, 1.0f));
    output.PositionCS = mul(u_ViewProjection, worldPosition);
    output.PositionWS = worldPosition.xyz;
    output.NormalWS = normalize(mul((float3x3)model, input.Normal));
    output.TangentWS = normalize(mul((float3x3)model, input.Tangent));
    output.BitangentWS = normalize(mul((float3x3)model, input.Bitangent));
    output.TexCoord = input.TexCoord;
    return output;
}

float3 ComputeNormal(VSOutput input, float2 texCoord)
{
    float3 normalWS = normalize(input.NormalWS);

    if (u_EnableNormalMapping > 0.5f)
    {
        const float3 tangentNormal = u_NormalMap.Sample(u_LinearWrapSampler, texCoord).xyz * 2.0f - 1.0f;
        const float3x3 tbn = float3x3(
            normalize(input.TangentWS),
            normalize(input.BitangentWS),
            normalWS);
        normalWS = normalize(mul(tangentNormal, tbn));
    }

    return normalWS;
}

GBufferOutput PSMain(VSOutput input)
{
    GBufferOutput output;

    const float2 texCoord = input.TexCoord;
    const float4 albedoSample = u_AlbedoMap.Sample(u_LinearWrapSampler, texCoord);
    const float3 albedo = albedoSample.rgb * u_Albedo.rgb;
    const float metallic = u_MetallicMap.Sample(u_LinearWrapSampler, texCoord).r * u_Metallic;
    const float roughness = u_RoughnessMap.Sample(u_LinearWrapSampler, texCoord).r * u_Roughness;
    const float ao = u_AmbientOcclusionMap.Sample(u_LinearWrapSampler, texCoord).r * u_AmbientOcclusion;
    const float opacity = u_OpacityMap.Sample(u_LinearWrapSampler, texCoord).r;
    const float3 normalWS = ComputeNormal(input, texCoord);

    output.Albedo = float4(albedo, u_Albedo.a * albedoSample.a * opacity);
    output.Normal = float4(normalWS * 0.5f + 0.5f, 1.0f);
    output.Material = float4(metallic, roughness, ao, 1.0f);
    return output;
}
