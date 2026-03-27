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
    float4 u_Diffuse;
    float2 u_TextureTiling;
    float2 u_TextureOffset;
};

Texture2D u_DiffuseMap : register(t0, space2);
SamplerState u_LinearWrapSampler : register(s0, space2);

VSOutput VSMain(VSInput input)
{
    VSOutput output;

    const float4 worldPosition = mul(u_Model, float4(input.Position, 1.0f));
    output.PositionCS = mul(u_ViewProjection, worldPosition);
    output.PositionWS = worldPosition.xyz;
    output.NormalWS = mul((float3x3)u_Model, input.Normal);
    output.TangentWS = mul((float3x3)u_Model, input.Tangent);
    output.BitangentWS = mul((float3x3)u_Model, input.Bitangent);
    output.TexCoord = input.TexCoord;
    return output;
}

float4 PSMain(VSOutput input) : SV_Target0
{
    const float2 tiledTexCoord = u_TextureOffset + frac(input.TexCoord * u_TextureTiling);
    const float4 diffuse = u_DiffuseMap.Sample(u_LinearWrapSampler, tiledTexCoord) * u_Diffuse;
    return diffuse;
}
