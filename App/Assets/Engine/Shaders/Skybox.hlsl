#include "CommonTypes.hlsli"

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

cbuffer MaterialConstants : register(b0, space2)
{
    int u_UseProceduralSky;
    float u_PaddingUseProceduralSky0;
    float u_PaddingUseProceduralSky1;
    float u_PaddingUseProceduralSky2;
    float3 u_SkyTint;
    float u_Exposure;
    float3 u_GroundColor;
    float3 u_SunDirection;
    float u_AtmosphereThickness;
    float u_SunSize;
    float u_SunSizeConvergence;
    float u_Padding0;
    float u_Padding1;
    float u_Padding2;
};

TextureCube cubeTex : register(t0, space2);
SamplerState u_LinearWrapSampler : register(s0, space2);

struct SkyboxVSOutput
{
    float4 PositionCS : SV_Position;
    float3 TexCoords : TEXCOORD0;
};

SkyboxVSOutput VSMain(VSInput input)
{
    SkyboxVSOutput output;
    output.TexCoords = input.Position;

    const float3 worldPosition = input.Position + u_CameraWorldPos;
    const float4 positionCS = mul(u_ViewProjection, float4(worldPosition, 1.0f));
    output.PositionCS = positionCS.xyww;
    return output;
}

float3 EvalProceduralSky(float3 direction)
{
    const float atmosphere = clamp(u_AtmosphereThickness, 0.25f, 2.0f);
    const float horizon = clamp(direction.y * 0.5f + 0.5f, 0.0f, 1.0f);

    const float3 zenithColor = lerp(float3(0.42f, 0.56f, 0.79f), u_SkyTint, 0.60f);
    const float3 skyMidColor = lerp(float3(0.58f, 0.74f, 0.92f), u_SkyTint, 0.30f);
    const float3 horizonColor = float3(0.86f, 0.97f, 0.99f);

    const float upperBlend = pow(smoothstep(0.52f, 1.0f, horizon), lerp(1.10f, 0.82f, atmosphere * 0.45f));
    const float midBlend = smoothstep(0.50f, 0.74f, horizon);
    const float horizonGlow = 1.0f - smoothstep(0.47f, 0.56f, horizon);
    const float skyBlend = smoothstep(0.49f, 0.53f, horizon);

    float3 sky = lerp(horizonColor, skyMidColor, midBlend);
    sky = lerp(sky, zenithColor, upperBlend);
    sky += horizonColor * horizonGlow * 0.06f;

    return lerp(u_GroundColor, sky, skyBlend) * u_Exposure;
}

float4 PSMain(SkyboxVSOutput input) : SV_Target0
{
    const float3 direction = normalize(input.TexCoords);
    if (u_UseProceduralSky == 0)
        return cubeTex.Sample(u_LinearWrapSampler, direction);

    return float4(EvalProceduralSky(direction), 1.0f);
}
