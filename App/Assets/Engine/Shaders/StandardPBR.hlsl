#include "CommonTypes.hlsli"
#include "NullusShaderLibrary/StandardPBRSurface.hlsl"
#include "LightGridCommon.hlsli"

cbuffer FrameConstants : register(b0, space0)
{
    float4x4 u_ViewProjection;
    float3 u_CameraWorldPos;
    float u_Padding0;
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
    float4 u_MetallicMapChannel;
    float4 u_RoughnessMapChannel;
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
    const float3x3 model3x3 = (float3x3)model;
    const NLSTangentFrame tangentFrame = NLSBuildStandardPbrTangentFrame(
        model3x3,
        input.Normal,
        input.Tangent,
        input.Bitangent);
    output.NormalWS = tangentFrame.normalWS;
    output.TangentWS = tangentFrame.tangentWS;
    output.BitangentWS = tangentFrame.bitangentWS;
    output.TexCoord = input.TexCoord;
    return output;
}

float4 PSMain(VSOutput input, bool isFrontFace : SV_IsFrontFace) : SV_Target0
{
    const float2 texCoord = input.TexCoord;
    const float4 albedoSample = u_AlbedoMap.Sample(u_LinearWrapSampler, texCoord);
    const float3 albedo = albedoSample.rgb * u_Albedo.rgb;
    const float metallic = u_Metallic *
        dot(u_MetallicMap.Sample(u_LinearWrapSampler, texCoord), u_MetallicMapChannel);
    const float roughness = u_Roughness *
        dot(u_RoughnessMap.Sample(u_LinearWrapSampler, texCoord), u_RoughnessMapChannel);
    const float ao = u_AmbientOcclusionMap.Sample(u_LinearWrapSampler, texCoord).r * u_AmbientOcclusion;
    const float opacity = u_OpacityMap.Sample(u_LinearWrapSampler, texCoord).r;
    const float3 emissive = u_EmissiveMap.Sample(u_LinearWrapSampler, texCoord).rgb * u_Emissive.rgb;

    const float3 interpolatedGeometryNormalWS = NLSSafeNormalize(input.NormalWS, float3(0.0f, 0.0f, 1.0f));
    const float3 geometryNormalWS = NLSOrientGeometryNormal(interpolatedGeometryNormalWS, isFrontFace);
    float3 shadingNormalWS = geometryNormalWS;
    if (u_EnableNormalMapping > 0.5f)
    {
        shadingNormalWS = NLSConstrainShadingNormalToGeometryHemisphere(
            NLSApplyStandardPbrNormalMap(
                input.NormalWS,
                input.TangentWS,
                input.BitangentWS,
                isFrontFace,
                u_NormalMap.Sample(u_LinearWrapSampler, texCoord),
                1.0f),
            geometryNormalWS);
    }
    const float3 lighting = NLSAccumulateClusteredLightingPBR(
        u_ForwardLocalLightBuffer,
        u_NumCulledLightsGrid,
        u_CulledLightDataGrid,
        input.PositionWS,
        geometryNormalWS,
        shadingNormalWS,
        albedo,
        metallic,
        roughness,
        ao);
    // The shared LDR transform preserves highlight hue and softens isolated specular peaks.
    return float4(
        NLSToneMapACES(lighting + emissive),
        u_Albedo.a * albedoSample.a * opacity);
}
