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
    float4 u_Diffuse;
};

static const float3 kLightDirection = normalize(float3(-0.55f, -0.75f, 0.35f));
static const float3 kLightColor = float3(1.0f, 1.0f, 1.0f);
static const float3 kAmbientColor = float3(0.30f, 0.30f, 0.30f);

VSOutput VSMain(VSInput input)
{
    VSOutput output;

    const float4 worldPosition = mul(u_Model, float4(input.Position, 1.0f));
    output.PositionCS = mul(u_ViewProjection, worldPosition);
    output.PositionWS = worldPosition.xyz;
    output.NormalWS = normalize(mul((float3x3)u_Model, input.Normal));
    output.TangentWS = mul((float3x3)u_Model, input.Tangent);
    output.BitangentWS = mul((float3x3)u_Model, input.Bitangent);
    output.TexCoord = input.TexCoord;
    return output;
}

float4 PSMain(VSOutput input) : SV_Target0
{
    const float3 normalWS = normalize(input.NormalWS);
    const float diffuseTerm = saturate(dot(normalWS, -kLightDirection));
    const float3 lighting = saturate(kAmbientColor + kLightColor * diffuseTerm);
    return float4(lighting * u_Diffuse.rgb, u_Diffuse.a);
}
