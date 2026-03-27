#include "../../Engine/Shaders/CommonTypes.hlsli"

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
    float3 u_Color;
    float u_Padding0;
};

struct GridVSOutput
{
    float4 PositionCS : SV_Position;
    float3 PositionWS : TEXCOORD0;
    float2 TexCoords : TEXCOORD1;
};

GridVSOutput VSMain(VSInput input)
{
    GridVSOutput output;
    const float4 worldPosition = mul(u_Model, float4(input.Position, 1.0f));
    output.PositionWS = worldPosition.xyz;
    output.TexCoords = output.PositionWS.xz;
    output.PositionCS = mul(u_ViewProjection, worldPosition);
    return output;
}

float MAG(float2 texCoords, float linePitch)
{
    const float lineWidth = 1.0f;
    const float2 coord = texCoords / linePitch;
    const float2 grid = abs(frac(coord - 0.5f) - 0.5f) / max(fwidth(coord), float2(1e-4f, 1e-4f));
    const float lineDistance = min(grid.x, grid.y);
    return lineWidth - min(lineDistance, lineWidth);
}

float GridLod(float2 texCoords, float height, float a, float b, float c)
{
    const float cl = MAG(texCoords, a);
    const float ml = MAG(texCoords, b);
    const float fl = MAG(texCoords, c);

    const float df = clamp((height - 16.0f) / (52.0f - 16.0f), 0.0f, 1.0f);
    const float dff = clamp((height - 96.0f) / (196.0f - 96.0f), 0.0f, 1.0f);

    return lerp(lerp(cl, ml, df), fl, dff);
}

float AxisMask(float2 texCoords, float spacing)
{
    const float2 coord = texCoords / spacing;
    const float2 axis = abs(coord) / max(fwidth(coord), float2(1e-4f, 1e-4f));
    return 1.0f - min(min(axis.x, axis.y), 1.0f);
}

float SingleAxisMask(float coordinate, float spacing)
{
    const float coord = coordinate / spacing;
    const float axis = abs(coord) / max(fwidth(coord), 1e-4f);
    return 1.0f - min(axis, 1.0f);
}

float4 PSMain(GridVSOutput input) : SV_Target0
{
    const float height = distance(u_CameraWorldPos.y, input.PositionWS.y);
    const float viewDistance = length(u_CameraWorldPos.xz - input.PositionWS.xz);
    const float3 viewDir = normalize(u_CameraWorldPos - input.PositionWS);
    const float angleFade = smoothstep(0.05f, 0.16f, abs(viewDir.y));

    const float minorGrid = GridLod(input.TexCoords, height, 1.0f, 2.0f, 4.0f);
    const float midGrid = GridLod(input.TexCoords, height, 5.0f, 10.0f, 20.0f);
    const float majorGrid = GridLod(input.TexCoords, height, 20.0f, 40.0f, 80.0f);

    const float minorAlpha = minorGrid * 0.040f * (1.0f - smoothstep(14.0f, 56.0f, height)) * (1.0f - smoothstep(70.0f, 180.0f, viewDistance));
    const float midAlpha = midGrid * 0.145f * (1.0f - smoothstep(120.0f, 420.0f, height)) * (1.0f - smoothstep(180.0f, 560.0f, viewDistance));
    const float majorAlpha = majorGrid * 0.54f * (1.0f - smoothstep(700.0f, 1800.0f, height)) * (1.0f - smoothstep(360.0f, 1400.0f, viewDistance));
    const float axisAlpha = AxisMask(input.TexCoords, 20.0f) * 0.17f * (1.0f - smoothstep(360.0f, 1400.0f, viewDistance));
    const float xAxisAlpha = SingleAxisMask(input.TexCoords.x, 20.0f) * 0.030f * (1.0f - smoothstep(360.0f, 1400.0f, viewDistance));
    const float zAxisAlpha = SingleAxisMask(input.TexCoords.y, 20.0f) * 0.030f * (1.0f - smoothstep(360.0f, 1400.0f, viewDistance));

    const float3 minorColor = u_Color * 0.74f;
    const float3 midColor = u_Color * 0.92f;
    const float3 majorColor = u_Color * 1.18f;
    const float3 axisColor = u_Color * 1.16f;
    const float3 xAxisColor = lerp(axisColor, float3(0.50f, 0.58f, 0.72f), 0.12f);
    const float3 zAxisColor = lerp(axisColor, float3(0.60f, 0.67f, 0.52f), 0.10f);

    const float3 color =
        minorColor * saturate(minorAlpha) +
        midColor * saturate(midAlpha) +
        majorColor * saturate(majorAlpha) +
        axisColor * saturate(axisAlpha) +
        xAxisColor * saturate(xAxisAlpha) +
        zAxisColor * saturate(zAxisAlpha);

    const float alpha = saturate(minorAlpha + midAlpha + majorAlpha + axisAlpha + xAxisAlpha + zAxisAlpha) * angleFade;
    return float4(color, alpha);
}
