#ifndef NULLUS_SHADER_LIBRARY_SPACE_TRANSFORMS_INCLUDED
#define NULLUS_SHADER_LIBRARY_SPACE_TRANSFORMS_INCLUDED

#include "Common.hlsl"

cbuffer FrameConstants : register(b0, space0)
{
    float4x4 u_ViewProjection;
    float3 u_CameraWorldPos;
    float u_Time;
    float4x4 u_ViewProjectionNoTranslation;
};

cbuffer ObjectConstants : register(b0, space3)
{
    float4x4 u_Model;
};

float3 TransformObjectToWorld(float3 positionOS)
{
    return mul(u_Model, float4(positionOS, 1.0f)).xyz;
}

float4 TransformWorldToHClip(float3 positionWS)
{
    return mul(u_ViewProjection, float4(positionWS, 1.0f));
}

float4 TransformObjectToHClip(float3 positionOS)
{
    return TransformWorldToHClip(TransformObjectToWorld(positionOS));
}

float3 TransformObjectToWorldNormal(float3 normalOS)
{
    const float3x3 model = (float3x3)u_Model;
    return NLSSafeNormalize(mul(model, normalOS), float3(0.0f, 0.0f, 1.0f));
}

#endif
