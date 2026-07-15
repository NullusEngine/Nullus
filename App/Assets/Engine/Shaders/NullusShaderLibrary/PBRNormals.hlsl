#ifndef NULLUS_SHADER_LIBRARY_PBR_NORMALS_INCLUDED
#define NULLUS_SHADER_LIBRARY_PBR_NORMALS_INCLUDED

#include "Common.hlsl"

float3 NLSOrientGeometryNormal(float3 normalWS, bool isFrontFace)
{
    const float3 geometryNormalWS = NLSSafeNormalize(normalWS, float3(0.0f, 0.0f, 1.0f));
    return isFrontFace ? geometryNormalWS : -geometryNormalWS;
}

float3 NLSConstrainShadingNormalToGeometryHemisphere(float3 shadingNormalWS, float3 geometryNormalWS)
{
    const float3 orientedGeometryNormalWS =
        NLSSafeNormalize(geometryNormalWS, float3(0.0f, 0.0f, 1.0f));
    shadingNormalWS = NLSSafeNormalize(shadingNormalWS, orientedGeometryNormalWS);
    shadingNormalWS -=
        min(0.0f, dot(shadingNormalWS, orientedGeometryNormalWS)) * orientedGeometryNormalWS;
    return NLSSafeNormalize(shadingNormalWS, orientedGeometryNormalWS);
}

float2 NLSOctEncodeNormal(float3 normalWS)
{
    const float3 normal = NLSSafeNormalize(normalWS, float3(0.0f, 0.0f, 1.0f));
    float2 encoded = normal.xy / (abs(normal.x) + abs(normal.y) + abs(normal.z));
    if (normal.z < 0.0f)
    {
        const float2 signs = float2(
            encoded.x >= 0.0f ? 1.0f : -1.0f,
            encoded.y >= 0.0f ? 1.0f : -1.0f);
        encoded = (1.0f - abs(encoded.yx)) * signs;
    }
    return encoded;
}

float2 NLSPackOctNormalToUnorm(float2 encoded)
{
    return saturate(encoded * 0.5f + 0.5f);
}

float2 NLSUnpackOctNormalFromUnorm(float2 packed)
{
    return saturate(packed) * 2.0f - 1.0f;
}

float3 NLSOctDecodeNormal(float2 encoded)
{
    float3 normal = float3(encoded, 1.0f - abs(encoded.x) - abs(encoded.y));
    const float fold = saturate(-normal.z);
    normal.xy += float2(
        normal.x >= 0.0f ? -fold : fold,
        normal.y >= 0.0f ? -fold : fold);
    return NLSSafeNormalize(normal, float3(0.0f, 0.0f, 1.0f));
}

float NLSGeometryHorizonVisibility(float geometryNdotDirection, float shadingNdotDirection)
{
    if (geometryNdotDirection <= 0.0f)
        return 0.0f;
    if (shadingNdotDirection <= 0.0f)
        return 1.0f;

    const float horizonRatio = saturate(
        geometryNdotDirection / max(shadingNdotDirection, 1.0e-4f));
    return smoothstep(0.0f, 0.35f, horizonRatio);
}

#endif
