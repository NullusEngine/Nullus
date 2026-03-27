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
    float3 u_Start;
    float u_Padding0;
    float3 u_End;
    float u_Padding1;
    float3 u_Color;
    float u_Padding2;
};

struct DebugLineVSOutput
{
    float4 PositionCS : SV_Position;
    float3 Color : TEXCOORD0;
};

DebugLineVSOutput VSMain(VSInput input, uint vertexId : SV_VertexID)
{
    DebugLineVSOutput output;
    const float3 positionWS = vertexId == 0 ? u_Start : u_End;
    output.PositionCS = mul(u_ViewProjection, float4(positionWS, 1.0f));
    output.Color = u_Color;
    return output;
}

float4 PSMain(DebugLineVSOutput input) : SV_Target0
{
    return float4(input.Color, 1.0f);
}
