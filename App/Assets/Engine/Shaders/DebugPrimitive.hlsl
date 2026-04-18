#include "CommonTypes.hlsli"

cbuffer FrameConstants : register(b0, space0)
{
    float4x4 u_ViewProjection;
    float3 u_CameraWorldPos;
    float u_Time;
    float4x4 u_ViewProjectionNoTranslation;
};

cbuffer MaterialConstants : register(b0, space2)
{
    float3 u_Point0;
    float u_Padding0;
    float3 u_Point1;
    float u_Padding1;
    float3 u_Point2;
    float u_Padding2;
    float3 u_Color;
    float u_Padding3;
};

struct DebugPrimitiveVSOutput
{
    float4 PositionCS : SV_Position;
    float3 Color : TEXCOORD0;
};

DebugPrimitiveVSOutput VSMain(VSInput input, uint vertexId : SV_VertexID)
{
    DebugPrimitiveVSOutput output;
    float3 positionWS = u_Point0;

    if (vertexId == 1)
    {
        positionWS = u_Point1;
    }
    else if (vertexId == 2)
    {
        positionWS = u_Point2;
    }

    output.PositionCS = mul(u_ViewProjection, float4(positionWS, 1.0f));
    output.Color = u_Color;
    return output;
}

float4 PSMain(DebugPrimitiveVSOutput input) : SV_Target0
{
    return float4(input.Color, 1.0f);
}
