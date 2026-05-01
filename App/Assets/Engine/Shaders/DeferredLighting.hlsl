#include "LightGridCommon.hlsli"

Texture2D u_GBufferAlbedo : register(t0, space2);
Texture2D u_GBufferNormal : register(t1, space2);
Texture2D u_GBufferMaterial : register(t2, space2);
Texture2D u_GBufferDepth : register(t3, space2);
TextureCube u_SkyboxCube : register(t4, space2);
SamplerState u_LinearWrapSampler : register(s0, space2);
StructuredBuffer<uint> u_LightGridLights : register(t0, space1);
StructuredBuffer<uint> u_LightGridClusterRecords : register(t1, space1);
StructuredBuffer<uint> u_LightGridCompactIndices : register(t2, space1);

struct VSInput
{
    float3 Position : POSITION;
    float2 TexCoord : TEXCOORD0;
};

struct VSOutput
{
    float4 PositionCS : SV_Position;
    float2 TexCoord : TEXCOORD0;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    output.PositionCS = float4(input.Position.xy, 0.0f, 1.0f);
    output.TexCoord = input.TexCoord;
    return output;
}

float3 ReconstructFarWorldDirection(float2 texCoord)
{
    const float2 clipXY = texCoord * 2.0f - 1.0f;
    const float4 worldPosition = mul(u_LightGridInverseViewProjection, float4(clipXY, 1.0f, 1.0f));
    const float3 world = worldPosition.xyz / max(abs(worldPosition.w), 1e-5f);
    return normalize(world - NLSGetCameraWorldPosition());
}

float3 ReconstructWorldPosition(float2 texCoord, float depth01)
{
    const float2 clipXY = texCoord * 2.0f - 1.0f;
    const float clipZ = depth01 * 2.0f - 1.0f;
    const float4 worldPosition = mul(u_LightGridInverseViewProjection, float4(clipXY, clipZ, 1.0f));
    return worldPosition.xyz / max(abs(worldPosition.w), 1e-5f);
}

float4 PSMain(VSOutput input) : SV_Target0
{
    const float depth01 = u_GBufferDepth.Sample(u_LinearWrapSampler, input.TexCoord).r;
    if (depth01 >= 0.9995f)
    {
        const float3 skyDirection = ReconstructFarWorldDirection(input.TexCoord);
        if (u_LightGridLightingParams.w > 0.5f)
            return u_SkyboxCube.Sample(u_LinearWrapSampler, skyDirection);

        return float4(0.55f, 0.70f, 0.92f, 1.0f);
    }

    const float4 albedo = u_GBufferAlbedo.Sample(u_LinearWrapSampler, input.TexCoord);
    const float3 encodedNormal = u_GBufferNormal.Sample(u_LinearWrapSampler, input.TexCoord).xyz;
    const float3 normalWS = normalize(encodedNormal * 2.0f - 1.0f);
    const float3 materialParams = u_GBufferMaterial.Sample(u_LinearWrapSampler, input.TexCoord).xyz;
    const float3 worldPosition = ReconstructWorldPosition(input.TexCoord, depth01);

    const float metallic = materialParams.x;
    const float roughness = materialParams.y;
    const float ao = materialParams.z;
    const float3 litColor = NLSAccumulateClusteredLightingPBR(
        u_LightGridLights,
        u_LightGridClusterRecords,
        u_LightGridCompactIndices,
        worldPosition,
        normalWS,
        albedo.rgb,
        metallic,
        roughness,
        ao);

    return float4(litColor, albedo.a);
}
