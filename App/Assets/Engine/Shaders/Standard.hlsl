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
    float2 u_TextureTiling;
    float2 u_TextureOffset;
    float4 u_Diffuse;
    float3 u_Specular;
    float u_Shininess;
    float u_HeightScale;
    float u_EnableNormalMapping;
    float2 u_Padding0;
};

Texture2D u_DiffuseMap : register(t0, space2);
Texture2D u_SpecularMap : register(t1, space2);
Texture2D u_NormalMap : register(t2, space2);
Texture2D u_HeightMap : register(t3, space2);
Texture2D u_MaskMap : register(t4, space2);
SamplerState u_LinearWrapSampler : register(s0, space2);

static const float3 kLightDirection = normalize(float3(-0.55f, -0.75f, 0.35f));
static const float3 kLightColor = float3(1.0f, 1.0f, 1.0f);
static const float3 kAmbientColor = float3(0.20f, 0.20f, 0.20f);

VSOutput VSMain(VSInput input)
{
    VSOutput output;

    const float4 worldPosition = mul(u_Model, float4(input.Position, 1.0f));
    output.PositionCS = mul(u_ViewProjection, worldPosition);
    output.PositionWS = worldPosition.xyz;
    output.NormalWS = normalize(mul((float3x3)u_Model, input.Normal));
    output.TangentWS = normalize(mul((float3x3)u_Model, input.Tangent));
    output.BitangentWS = normalize(mul((float3x3)u_Model, input.Bitangent));
    output.TexCoord = input.TexCoord;
    return output;
}

float2 ComputeTexCoord(VSOutput input)
{
    float2 texCoord = u_TextureOffset + frac(input.TexCoord * u_TextureTiling);

    if (u_HeightScale > 0.0f)
    {
        const float3 viewDirWS = normalize(u_CameraWorldPos - input.PositionWS);
        const float3x3 tbn = float3x3(
            normalize(input.TangentWS),
            normalize(input.BitangentWS),
            normalize(input.NormalWS));
        const float3 viewDirTS = mul(transpose(tbn), viewDirWS);
        texCoord -= viewDirTS.xy * (u_HeightMap.Sample(u_LinearWrapSampler, texCoord).r * u_HeightScale);
    }

    return texCoord;
}

float3 ComputeNormal(VSOutput input, float2 texCoord)
{
    float3 normalWS = normalize(input.NormalWS);

    if (u_EnableNormalMapping > 0.5f)
    {
        const float3 tangentNormal = u_NormalMap.Sample(u_LinearWrapSampler, texCoord).xyz * 2.0f - 1.0f;
        const float3x3 tbn = float3x3(
            normalize(input.TangentWS),
            normalize(input.BitangentWS),
            normalWS);
        normalWS = normalize(mul(tangentNormal, tbn));
    }

    return normalWS;
}

float4 PSMain(VSOutput input) : SV_Target0
{
    const float2 texCoord = ComputeTexCoord(input);

    if (u_MaskMap.Sample(u_LinearWrapSampler, texCoord).r <= 0.0f)
        discard;

    const float4 diffuseSample = u_DiffuseMap.Sample(u_LinearWrapSampler, texCoord) * u_Diffuse;
    const float3 specularSample = u_SpecularMap.Sample(u_LinearWrapSampler, texCoord).rgb * u_Specular;
    const float3 normalWS = ComputeNormal(input, texCoord);

    const float3 lightDir = -kLightDirection;
    const float3 viewDir = normalize(u_CameraWorldPos - input.PositionWS);
    const float3 halfVector = normalize(lightDir + viewDir);

    const float diffuseTerm = saturate(dot(normalWS, lightDir));
    const float specularTerm = pow(saturate(dot(normalWS, halfVector)), max(u_Shininess, 1.0f));

    const float3 lighting = kAmbientColor + kLightColor * diffuseTerm + specularSample * specularTerm;
    return float4(diffuseSample.rgb * lighting, diffuseSample.a);
}
