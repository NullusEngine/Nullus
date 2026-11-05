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
cbuffer MaterialConstants : register(b0, space2) { float4 u_WireColor; };
struct WireframeOutput { float4 position : SV_Position; };
WireframeOutput VSMain(VSInput input)
{
#if defined(NLS_SKINNED)
    input.Position = NLSSkinPosition(input.Position, input.skinJoints, input.skinWeights);
#endif
    WireframeOutput output;
    output.position = mul(u_ViewProjection, mul(u_Model, float4(input.Position, 1.0f)));
    return output;
}
float4 PSMain(WireframeOutput input) : SV_Target0
{
    return u_WireColor;
}
