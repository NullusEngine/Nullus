#include "CommonTypes.hlsli"
#include "NullusShaderLibrary/PBRNormals.hlsl"

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
    const float3x3 model3x3 = (float3x3)model;
    const NLSTangentFrame tangentFrame = NLSBuildSafeTangentFrame(
        NLSTransformNormalDirection(model3x3, input.Normal),
        mul(model3x3, input.Tangent),
        mul(model3x3, input.Bitangent));
    output.NormalWS = tangentFrame.normalWS;
    output.TangentWS = tangentFrame.tangentWS;
    output.BitangentWS = tangentFrame.bitangentWS;
    output.TexCoord = input.TexCoord;
    return output;
}

float3 DecodeNormalMapSample(float4 normalSample)
{
    const float2 xy = normalSample.xy * 2.0f - 1.0f;
    const float rgbZ = normalSample.z * 2.0f - 1.0f;
    const float reconstructedZ = sqrt(saturate(1.0f - dot(xy, xy)));
    const float useRgbZ = step(0.0039f, normalSample.z);
    return NLSSafeNormalize(float3(xy, lerp(reconstructedZ, rgbZ, useRgbZ)), float3(0.0f, 0.0f, 1.0f));
}

float3 ComputeShadingNormal(
    VSOutput input,
    float2 texCoord,
    bool isFrontFace)
{
    NLSTangentFrame tangentFrame = NLSBuildSafeTangentFrame(
        input.NormalWS,
        input.TangentWS,
        input.BitangentWS);
    tangentFrame = NLSOrientTangentFrameForFace(tangentFrame, isFrontFace);
    const float3 tangentNormal = DecodeNormalMapSample(u_NormalMap.Sample(u_LinearWrapSampler, texCoord));
    return NLSApplyTangentNormal(tangentNormal, tangentFrame);
}

GBufferOutput PSMain(VSOutput input, bool isFrontFace : SV_IsFrontFace)
{
    GBufferOutput output;

    const float2 texCoord = input.TexCoord;
    const float4 albedoSample = u_AlbedoMap.Sample(u_LinearWrapSampler, texCoord);
    const float3 albedo = albedoSample.rgb * u_Albedo.rgb;
    const float metallic = u_Metallic *
        dot(u_MetallicMap.Sample(u_LinearWrapSampler, texCoord), u_MetallicMapChannel);
    const float roughness = u_Roughness *
        dot(u_RoughnessMap.Sample(u_LinearWrapSampler, texCoord), u_RoughnessMapChannel);
    const float ao = u_AmbientOcclusionMap.Sample(u_LinearWrapSampler, texCoord).r * u_AmbientOcclusion;
    const float3 interpolatedGeometryNormalWS = NLSSafeNormalize(input.NormalWS, float3(0.0f, 0.0f, 1.0f));
    const float3 geometryNormalWS = NLSOrientGeometryNormal(interpolatedGeometryNormalWS, isFrontFace);
    float3 shadingNormalWS = geometryNormalWS;
    if (u_EnableNormalMapping > 0.5f)
    {
        shadingNormalWS = NLSConstrainShadingNormalToGeometryHemisphere(
            ComputeShadingNormal(input, texCoord, isFrontFace),
            geometryNormalWS);
    }
    const float2 geometryNormalOct = NLSOctEncodeNormal(geometryNormalWS);
    const float receiveShadows =
        (u_ObjectFlags & NLS_OBJECT_FLAG_RECEIVE_SHADOWS) != 0u ? 1.0f : 0.0f;

    output.Albedo = float4(albedo, geometryNormalOct.x);
    output.Normal = float4(shadingNormalWS * 0.5f + 0.5f, geometryNormalOct.y);
    output.Material = float4(metallic, roughness, ao, receiveShadows);
    return output;
}
