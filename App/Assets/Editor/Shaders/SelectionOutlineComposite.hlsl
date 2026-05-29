#include "SelectionOutlineMaskChannels.hlsli"
#include "../../Engine/Shaders/CommonTypes.hlsli"

Texture2D u_SelectionOutlineMask : register(t0, space2);
Texture2D u_MainTexture : register(t1, space2);
SamplerState u_LinearClampSampler : register(s0, space2);

cbuffer FrameConstants : register(b0, space0)
{
    float4x4 u_ViewProjection;
    float3 u_CameraWorldPos;
    float u_Time;
    float4x4 u_ViewProjectionNoTranslation;
};

cbuffer MaterialConstants : register(b0, space2)
{
    float4 u_OutlineColor;
    float4 u_ChildOutlineColor;
    float4 u_TexelSize;
};

VSOutput BuildFullscreenVertex(VSInput input)
{
    VSOutput output;
    output.PositionCS = float4(input.Position.xy, 0.0f, 1.0f);
    output.PositionWS = float3(input.Position.xy, 0.0f);
    output.NormalWS = float3(0.0f, 0.0f, 1.0f);
    output.TangentWS = float3(1.0f, 0.0f, 0.0f);
    output.BitangentWS = float3(0.0f, 1.0f, 0.0f);
    output.TexCoord = input.TexCoord;
    return output;
}

VSOutput VSMain(VSInput input)
{
    return BuildFullscreenVertex(input);
}

#include "SelectionOutlineCompositeCore.hlsli"

float4 PSMain(VSOutput input) : SV_Target0
{
    return Composite(input);
}
