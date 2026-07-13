#include "LightGridCommon.hlsli"

Texture2D u_GBufferAlbedo : register(t0, space2);
Texture2D u_GBufferNormal : register(t1, space2);
Texture2D u_GBufferMaterial : register(t2, space2);
Texture2D u_GBufferDepth : register(t3, space2);
TextureCube u_SkyboxCube : register(t4, space2);
SamplerState u_LinearWrapSampler : register(s0, space2);
StructuredBuffer<uint> u_ForwardLocalLightBuffer : register(t0, space1);
StructuredBuffer<uint> u_NumCulledLightsGrid : register(t1, space1);
StructuredBuffer<uint> u_CulledLightDataGrid : register(t2, space1);

cbuffer MaterialConstants : register(b0, space2)
{
    int u_UseProceduralSky;
    float u_PaddingUseProceduralSky0;
    float u_PaddingUseProceduralSky1;
    float u_PaddingUseProceduralSky2;
    float3 u_SkyTint;
    float u_Exposure;
    float3 u_GroundColor;
    float3 u_SunDirection;
    float u_AtmosphereThickness;
    float u_SunSize;
    float u_SunSizeConvergence;
    float u_Padding0;
    float u_Padding1;
    float u_Padding2;
};

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

static const float NLS_DEFERRED_SAFE_NORMAL_EPSILON = 1.0e-8f;
static const float NLS_DEFERRED_SAFE_NORMAL_MAX_LENGTH_SQ = 1.0e+20f;
static const float NLS_DEFERRED_SAFE_NORMAL_MAX_COMPONENT = 1.0e+30f;

bool NLSDeferredIsFinite3(float3 value)
{
    return all(value == value) && all(abs(value) < NLS_DEFERRED_SAFE_NORMAL_MAX_COMPONENT);
}

float3 NLSDeferredSafeNormalize(float3 value, float3 fallback)
{
    const float lengthSq = dot(value, value);
    if (NLSDeferredIsFinite3(value) &&
        lengthSq > NLS_DEFERRED_SAFE_NORMAL_EPSILON &&
        lengthSq < NLS_DEFERRED_SAFE_NORMAL_MAX_LENGTH_SQ)
    {
        return value * rsqrt(lengthSq);
    }

    const float fallbackLengthSq = dot(fallback, fallback);
    if (NLSDeferredIsFinite3(fallback) &&
        fallbackLengthSq > NLS_DEFERRED_SAFE_NORMAL_EPSILON &&
        fallbackLengthSq < NLS_DEFERRED_SAFE_NORMAL_MAX_LENGTH_SQ)
    {
        return fallback * rsqrt(fallbackLengthSq);
    }

    return float3(0.0f, 0.0f, 1.0f);
}

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    output.PositionCS = float4(input.Position.xy, 0.0f, 1.0f);
    output.TexCoord = input.TexCoord;
    return output;
}

float2 ToDeferredClipXY(float2 texCoord)
{
    return float2(texCoord.x * 2.0f - 1.0f, 1.0f - texCoord.y * 2.0f);
}

float3 ReconstructFarWorldDirection(float2 texCoord)
{
    const float2 clipXY = ToDeferredClipXY(texCoord);
    const float4 worldPosition = mul(u_LightGridInverseViewProjection, float4(clipXY, 1.0f, 1.0f));
    const float3 world = worldPosition.xyz / max(abs(worldPosition.w), 1e-5f);
    return normalize(world - NLSGetCameraWorldPosition());
}

float3 ReconstructWorldPosition(float2 texCoord, float depth01)
{
    const float2 clipXY = ToDeferredClipXY(texCoord);
    const float clipZ = depth01;
    const float4 worldPosition = mul(u_LightGridInverseViewProjection, float4(clipXY, clipZ, 1.0f));
    return worldPosition.xyz / max(abs(worldPosition.w), 1e-5f);
}

float3 EvalProceduralSky(float3 direction)
{
    const float atmosphere = clamp(u_AtmosphereThickness, 0.25f, 2.0f);
    const float horizon = clamp(direction.y * 0.5f + 0.5f, 0.0f, 1.0f);

    const float3 zenithColor = lerp(float3(0.42f, 0.56f, 0.79f), u_SkyTint, 0.60f);
    const float3 skyMidColor = lerp(float3(0.58f, 0.74f, 0.92f), u_SkyTint, 0.30f);
    const float3 horizonColor = float3(0.86f, 0.97f, 0.99f);

    const float upperBlend = pow(smoothstep(0.52f, 1.0f, horizon), lerp(1.10f, 0.82f, atmosphere * 0.45f));
    const float midBlend = smoothstep(0.50f, 0.74f, horizon);
    const float horizonGlow = 1.0f - smoothstep(0.47f, 0.56f, horizon);
    const float skyBlend = smoothstep(0.49f, 0.53f, horizon);

    float3 sky = lerp(horizonColor, skyMidColor, midBlend);
    sky = lerp(sky, zenithColor, upperBlend);
    sky += horizonColor * horizonGlow * 0.06f;

    return lerp(u_GroundColor, sky, skyBlend) * u_Exposure;
}

float4 PSMain(VSOutput input) : SV_Target0
{
    const float depth01 = u_GBufferDepth.Sample(u_LinearWrapSampler, input.TexCoord).r;
    if (depth01 >= 0.9995f)
    {
        const float3 skyDirection = ReconstructFarWorldDirection(input.TexCoord);
        if (u_LightGridLightingParams.w > 0.5f)
        {
            const float4 skyboxColor = u_SkyboxCube.Sample(u_LinearWrapSampler, skyDirection);
            return float4(NLSToneMapACES(skyboxColor.rgb), skyboxColor.a);
        }

        if (u_UseProceduralSky != 0)
            return float4(NLSToneMapACES(EvalProceduralSky(skyDirection)), 1.0f);

        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    const float4 albedo = u_GBufferAlbedo.Sample(u_LinearWrapSampler, input.TexCoord);
    const float3 encodedNormal = u_GBufferNormal.Sample(u_LinearWrapSampler, input.TexCoord).xyz;
    const float3 normalWS = NLSDeferredSafeNormalize(encodedNormal * 2.0f - 1.0f, float3(0.0f, 0.0f, 1.0f));
    const float3 materialParams = u_GBufferMaterial.Sample(u_LinearWrapSampler, input.TexCoord).xyz;
    const float3 worldPosition = ReconstructWorldPosition(input.TexCoord, depth01);

    const float metallic = materialParams.x;
    const float roughness = materialParams.y;
    const float ao = materialParams.z;
    const float3 litColor = NLSAccumulateSceneLightingPBR(
        u_ForwardLocalLightBuffer,
        worldPosition,
        normalWS,
        albedo.rgb,
        metallic,
        roughness,
        ao);

    // The shared LDR transform preserves highlight hue and softens isolated specular peaks.
    return float4(NLSToneMapACES(litColor), 1.0f);
}
