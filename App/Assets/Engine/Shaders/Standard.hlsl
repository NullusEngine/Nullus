#include "CommonTypes.hlsli"
#include "LightGridCommon.hlsli"

cbuffer FrameConstants : register(b0, space0)
{
    float4x4 u_ViewProjection;
    float3 u_CameraWorldPos;
    float u_Time;
    float4x4 u_ViewProjectionNoTranslation;
};

StructuredBuffer<float4x4> ObjectData : register(t0, space3);

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
StructuredBuffer<uint> u_ForwardLocalLightBuffer : register(t0, space1);
StructuredBuffer<uint> u_NumCulledLightsGrid : register(t1, space1);
StructuredBuffer<uint> u_CulledLightDataGrid : register(t2, space1);

VSOutput VSMain(VSInput input, uint instanceId : SV_InstanceID)
{
    VSOutput output;
    const float4x4 model = ObjectData[u_ObjectIndex + instanceId];

    const float4 worldPosition = mul(model, float4(input.Position, 1.0f));
    output.PositionCS = mul(u_ViewProjection, worldPosition);
    output.PositionWS = worldPosition.xyz;
    const float3x3 model3x3 = (float3x3)model;
    const NLSTangentFrame tangentFrame = NLSBuildSafeTangentFrame(
        NLSTransformNormalDirection(model3x3, input.Normal),
        mul(model3x3, input.Tangent),
        mul(model3x3, input.Bitangent));
    output.NormalWS = tangentFrame.normalWS;
    output.TangentWS = tangentFrame.tangentWS;
    output.BitangentWS = tangentFrame.bitangentWS;
    output.TexCoord = input.TexCoord;
    return output;
}

float2 ComputeTexCoord(VSOutput input)
{
    float2 texCoord = u_TextureOffset + frac(input.TexCoord * u_TextureTiling);

    if (u_HeightScale > 0.0f)
    {
        const NLSTangentFrame tangentFrame = NLSBuildSafeTangentFrame(input.NormalWS, input.TangentWS, input.BitangentWS);
        const float3 viewDirWS = NLSSafeNormalize(u_CameraWorldPos - input.PositionWS, tangentFrame.normalWS);
        const float3 viewDirTS = mul(transpose(NLSBuildTangentToWorldMatrix(tangentFrame)), viewDirWS);
        texCoord -= viewDirTS.xy * (u_HeightMap.Sample(u_LinearWrapSampler, texCoord).r * u_HeightScale);
    }

    return texCoord;
}

float3 DecodeNormalMapSample(float4 normalSample)
{
    const float2 xy = normalSample.xy * 2.0f - 1.0f;
    const float rgbZ = normalSample.z * 2.0f - 1.0f;
    const float reconstructedZ = sqrt(saturate(1.0f - dot(xy, xy)));
    const float useRgbZ = step(0.0039f, normalSample.z);
    return NLSSafeNormalize(float3(xy, lerp(reconstructedZ, rgbZ, useRgbZ)), float3(0.0f, 0.0f, 1.0f));
}

float3 ComputeNormal(VSOutput input, float2 texCoord)
{
    float3 normalWS = NLSSafeNormalize(input.NormalWS, float3(0.0f, 0.0f, 1.0f));

    if (u_EnableNormalMapping > 0.5f)
    {
        const NLSTangentFrame tangentFrame = NLSBuildSafeTangentFrame(normalWS, input.TangentWS, input.BitangentWS);
        const float3 tangentNormal = DecodeNormalMapSample(u_NormalMap.Sample(u_LinearWrapSampler, texCoord));
        normalWS = NLSApplyTangentNormal(tangentNormal, tangentFrame);
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

    const float3 lighting = NLSAccumulateClusteredLightingPhong(
        u_ForwardLocalLightBuffer,
        u_NumCulledLightsGrid,
        u_CulledLightDataGrid,
        input.PositionWS,
        normalWS,
        diffuseSample.rgb,
        specularSample,
        u_Shininess);
    return float4(lighting, diffuseSample.a);
}
