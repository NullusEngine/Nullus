#ifndef NULLUS_SHADER_LIBRARY_STANDARD_PBR_SURFACE_INCLUDED
#define NULLUS_SHADER_LIBRARY_STANDARD_PBR_SURFACE_INCLUDED

#include "../CommonTypes.hlsli"
#include "PBRNormals.hlsl"

float3 NLSTransformStandardPbrNormal(float3x3 model, float3 normalOS)
{
    return NLSTransformNormalDirection(model, normalOS);
}

NLSTangentFrame NLSBuildStandardPbrTangentFrame(
    float3x3 model,
    float3 normalOS,
    float3 tangentOS,
    float3 bitangentOS)
{
    return NLSBuildSafeTangentFrame(
        NLSTransformStandardPbrNormal(model, normalOS),
        mul(model, tangentOS),
        mul(model, bitangentOS));
}

float3 NLSDecodeStandardPbrNormalSample(float4 normalSample, float normalScale)
{
    const float2 xy = normalSample.xy * 2.0f - 1.0f;
    const float rgbZ = normalSample.z * 2.0f - 1.0f;
    const float reconstructedZ = sqrt(saturate(1.0f - dot(xy, xy)));
    const float useRgbZ = step(0.0039f, normalSample.z);
    const float3 decoded = float3(
        xy * normalScale,
        lerp(reconstructedZ, rgbZ, useRgbZ));
    return NLSSafeNormalize(decoded, float3(0.0f, 0.0f, 1.0f));
}

float3 NLSApplyStandardPbrNormalMap(
    float3 normalWS,
    float3 tangentWS,
    float3 bitangentWS,
    bool isFrontFace,
    float4 normalSample,
    float normalScale)
{
    NLSTangentFrame tangentFrame = NLSBuildSafeTangentFrame(
        normalWS,
        tangentWS,
        bitangentWS);
    tangentFrame = NLSOrientTangentFrameForFace(tangentFrame, isFrontFace);
    return NLSApplyTangentNormal(
        NLSDecodeStandardPbrNormalSample(normalSample, normalScale),
        tangentFrame);
}

void NLSPackStandardPbrGBuffer(
    float3 albedo,
    float3 geometryNormalWS,
    float3 shadingNormalWS,
    float metallic,
    float roughness,
    float ao,
    float receiveShadows,
    out float4 albedoTarget,
    out float4 normalTarget,
    out float4 materialTarget)
{
    const float2 packedGeometryNormal = NLSPackOctNormalToUnorm(
        NLSOctEncodeNormal(geometryNormalWS));
    albedoTarget = float4(albedo, packedGeometryNormal.x);
    normalTarget = float4(shadingNormalWS * 0.5f + 0.5f, packedGeometryNormal.y);
    materialTarget = float4(metallic, roughness, ao, receiveShadows);
}

#endif
