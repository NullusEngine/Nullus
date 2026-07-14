#include "CommonTypes.hlsli"
#include "NullusShaderLibrary/StandardPBRSurface.hlsl"

cbuffer FrameConstants : register(b0, space0)
{
    float4x4 u_ViewProjection;
    float3 u_CameraWorldPos;
    float u_Padding0;
    float4x4 u_ViewProjectionNoTranslation;
};

StructuredBuffer<float4x4> ObjectData : register(t0, space3);

cbuffer MaterialConstants : register(b0, space2)
{
    float4 u_Albedo;
};

Texture2D u_AlbedoMap : register(t0, space2);
Texture2D u_OpacityMap : register(t5, space2);
SamplerState u_LinearWrapSampler : register(s0, space2);

VSOutput VSMain(VSInput input, uint instanceId : SV_InstanceID)
{
    VSOutput output;
    const float4x4 model = ObjectData[u_ObjectIndex + instanceId];
    const float4 worldPosition = mul(model, float4(input.Position, 1.0f));
    output.PositionCS = mul(u_ViewProjection, worldPosition);
    output.PositionWS = worldPosition.xyz;
    output.NormalWS = 0.0f;
    output.TangentWS = 0.0f;
    output.BitangentWS = 0.0f;
    output.TexCoord = input.TexCoord;
    return output;
}

float4 PSMain(VSOutput input) : SV_Target0
{
    const float4 baseSample = u_AlbedoMap.Sample(u_LinearWrapSampler, input.TexCoord);
    const float opacity = u_OpacityMap.Sample(u_LinearWrapSampler, input.TexCoord).r;
    return NLSEvaluateStandardPbrBaseColorAndOpacity(baseSample, u_Albedo, opacity);
}
