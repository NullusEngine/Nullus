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

float NLSResolveDeferredDirectVisibility(bool receiveShadows)
{
    // The receiver bit is retained for the future shadow-data contract.
    return 1.0f;
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

#if defined(NLS_SPIRV)
#ifndef NLS_DEFERRED_LIGHT_COUNT
#define NLS_DEFERRED_LIGHT_COUNT 0
#endif

#if NLS_DEFERRED_LIGHT_COUNT < 0 || NLS_DEFERRED_LIGHT_COUNT > 32
#error NLS_DEFERRED_LIGHT_COUNT must be in the range 0..32.
#endif

// AMD's Windows Vulkan driver miscompiles the dynamically indexed BRDF loop.
// Keep ambient accumulation separate from the literal direct-light slots.
// Combining both paths in this macro reproduces the AMD Windows driver bug.
#define NLS_ACCUMULATE_DEFERRED_DIRECT_LIGHT_SLOT(slot, base) \
    if (NLSGetSceneLightCount() > slot) { \
        const uint lightType = u_ForwardLocalLightBuffer[base + 7u]; \
        if (lightType != NLS_LIGHT_TYPE_AMBIENT_BOX && lightType != NLS_LIGHT_TYPE_AMBIENT_SPHERE) { \
            const float3 lightColor = float3( \
                asfloat(u_ForwardLocalLightBuffer[base + 8u]), \
                asfloat(u_ForwardLocalLightBuffer[base + 9u]), \
                asfloat(u_ForwardLocalLightBuffer[base + 10u])); \
            const float lightIntensity = asfloat(u_ForwardLocalLightBuffer[base + 11u]); \
            float3 lightDir = 0.0f.xxx; \
            float attenuation = 1.0f; \
            if (lightType == NLS_LIGHT_TYPE_DIRECTIONAL) { \
                const float3 lightDirectionWS = NLSSafeLightingNormalize(float3( \
                    asfloat(u_ForwardLocalLightBuffer[base + 4u]), \
                    asfloat(u_ForwardLocalLightBuffer[base + 5u]), \
                    asfloat(u_ForwardLocalLightBuffer[base + 6u])), float3(0.0f, -1.0f, 0.0f)); \
                lightDir = NLSSafeLightingNormalize(-lightDirectionWS, float3(0.0f, 1.0f, 0.0f)); \
            } else { \
                const float3 lightPositionWS = float3( \
                    asfloat(u_ForwardLocalLightBuffer[base + 0u]), \
                    asfloat(u_ForwardLocalLightBuffer[base + 1u]), \
                    asfloat(u_ForwardLocalLightBuffer[base + 2u])); \
                const float lightRange = asfloat(u_ForwardLocalLightBuffer[base + 3u]); \
                const float3 toLight = lightPositionWS - worldPosition; \
                const float distanceToLight = length(toLight); \
                if (distanceToLight <= max(lightRange, 0.0001f)) { \
                    lightDir = NLSSafeLightingNormalize(toLight, geometryNormalWS); \
                    const float rawDistanceSquared = distanceToLight * distanceToLight; \
                    const float distanceSquared = max(rawDistanceSquared, 1.0e-4f); \
                    const float rangeRatio = rawDistanceSquared / max(lightRange * lightRange, 1.0e-4f); \
                    float smoothFactor = saturate(1.0f - rangeRatio * rangeRatio); \
                    smoothFactor *= smoothFactor; \
                    attenuation = smoothFactor / distanceSquared; \
                    if (lightType == NLS_LIGHT_TYPE_SPOT) { \
                        const float3 lightDirectionWS = NLSSafeLightingNormalize(float3( \
                            asfloat(u_ForwardLocalLightBuffer[base + 4u]), \
                            asfloat(u_ForwardLocalLightBuffer[base + 5u]), \
                            asfloat(u_ForwardLocalLightBuffer[base + 6u])), float3(0.0f, -1.0f, 0.0f)); \
                        const float spotCos = dot(NLSSafeLightingNormalize(-lightDirectionWS, lightDir), lightDir); \
                        const float outerCutoffCos = cos(radians(asfloat(u_ForwardLocalLightBuffer[base + 12u]))); \
                        attenuation *= saturate((spotCos - outerCutoffCos) / max(1.0f - outerCutoffCos, 1e-3f)); \
                    } \
                } else { \
                    attenuation = 0.0f; \
                } \
            } \
            litColor += NLSEvaluateCookTorranceDirect( \
                geometryNormalWS, shadingNormalWS, viewDir, lightDir, \
                safeAlbedo, safeMetallic, filteredRoughness, \
                lightColor, lightIntensity, attenuation) * safeDirectVisibility; \
        } \
    }
#endif

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

    const float4 albedoSample = u_GBufferAlbedo.Sample(u_LinearWrapSampler, input.TexCoord);
    const float4 normalSample = u_GBufferNormal.Sample(u_LinearWrapSampler, input.TexCoord);
    const float4 materialSample = u_GBufferMaterial.Sample(u_LinearWrapSampler, input.TexCoord);
    const float3 geometryNormalWS = NLSOctDecodeNormal(NLSUnpackOctNormalFromUnorm(
        float2(albedoSample.a, normalSample.a)));
    const float3 shadingNormalWS = NLSConstrainShadingNormalToGeometryHemisphere(
        normalSample.rgb * 2.0f - 1.0f,
        geometryNormalWS);
    const bool receiveShadows = materialSample.a >= 0.5f;
    const float directVisibility = NLSResolveDeferredDirectVisibility(receiveShadows);
    const float3 worldPosition = ReconstructWorldPosition(input.TexCoord, depth01);

    const float metallic = materialSample.x;
    const float roughness = materialSample.y;
    const float ao = materialSample.z;
#if defined(NLS_SPIRV)
    const float3 viewDir = NLSSafeLightingNormalize(
        NLSGetCameraWorldPosition() - worldPosition,
        geometryNormalWS);
    const float3 safeAlbedo = NLSSafePbrAlbedo(albedoSample.rgb);
    const float safeMetallic = NLSSafePbrMetallic(metallic);
    const float filteredRoughness = NLSFilterPerceptualRoughness(shadingNormalWS, roughness);
    const float safeDirectVisibility = isfinite(directVisibility) ? saturate(directVisibility) : 1.0f;
    float3 litColor = NLSAccumulateSceneAmbientLightingPBR(
        u_ForwardLocalLightBuffer,
        albedoSample.rgb,
        ao);
#if NLS_DEFERRED_LIGHT_COUNT > 0
    NLS_ACCUMULATE_DEFERRED_DIRECT_LIGHT_SLOT(0u, 0u);
#endif
#if NLS_DEFERRED_LIGHT_COUNT > 1
    NLS_ACCUMULATE_DEFERRED_DIRECT_LIGHT_SLOT(1u, 16u);
#endif
#if NLS_DEFERRED_LIGHT_COUNT > 2
    NLS_ACCUMULATE_DEFERRED_DIRECT_LIGHT_SLOT(2u, 32u);
#endif
#if NLS_DEFERRED_LIGHT_COUNT > 3
    NLS_ACCUMULATE_DEFERRED_DIRECT_LIGHT_SLOT(3u, 48u);
#endif
#if NLS_DEFERRED_LIGHT_COUNT > 4
    NLS_ACCUMULATE_DEFERRED_DIRECT_LIGHT_SLOT(4u, 64u);
#endif
#if NLS_DEFERRED_LIGHT_COUNT > 5
    NLS_ACCUMULATE_DEFERRED_DIRECT_LIGHT_SLOT(5u, 80u);
#endif
#if NLS_DEFERRED_LIGHT_COUNT > 6
    NLS_ACCUMULATE_DEFERRED_DIRECT_LIGHT_SLOT(6u, 96u);
#endif
#if NLS_DEFERRED_LIGHT_COUNT > 7
    NLS_ACCUMULATE_DEFERRED_DIRECT_LIGHT_SLOT(7u, 112u);
#endif
#if NLS_DEFERRED_LIGHT_COUNT > 8
    NLS_ACCUMULATE_DEFERRED_DIRECT_LIGHT_SLOT(8u, 128u);
#endif
#if NLS_DEFERRED_LIGHT_COUNT > 9
    NLS_ACCUMULATE_DEFERRED_DIRECT_LIGHT_SLOT(9u, 144u);
#endif
#if NLS_DEFERRED_LIGHT_COUNT > 10
    NLS_ACCUMULATE_DEFERRED_DIRECT_LIGHT_SLOT(10u, 160u);
#endif
#if NLS_DEFERRED_LIGHT_COUNT > 11
    NLS_ACCUMULATE_DEFERRED_DIRECT_LIGHT_SLOT(11u, 176u);
#endif
#if NLS_DEFERRED_LIGHT_COUNT > 12
    NLS_ACCUMULATE_DEFERRED_DIRECT_LIGHT_SLOT(12u, 192u);
#endif
#if NLS_DEFERRED_LIGHT_COUNT > 13
    NLS_ACCUMULATE_DEFERRED_DIRECT_LIGHT_SLOT(13u, 208u);
#endif
#if NLS_DEFERRED_LIGHT_COUNT > 14
    NLS_ACCUMULATE_DEFERRED_DIRECT_LIGHT_SLOT(14u, 224u);
#endif
#if NLS_DEFERRED_LIGHT_COUNT > 15
    NLS_ACCUMULATE_DEFERRED_DIRECT_LIGHT_SLOT(15u, 240u);
#endif
#if NLS_DEFERRED_LIGHT_COUNT > 16
    NLS_ACCUMULATE_DEFERRED_DIRECT_LIGHT_SLOT(16u, 256u);
#endif
#if NLS_DEFERRED_LIGHT_COUNT > 17
    NLS_ACCUMULATE_DEFERRED_DIRECT_LIGHT_SLOT(17u, 272u);
#endif
#if NLS_DEFERRED_LIGHT_COUNT > 18
    NLS_ACCUMULATE_DEFERRED_DIRECT_LIGHT_SLOT(18u, 288u);
#endif
#if NLS_DEFERRED_LIGHT_COUNT > 19
    NLS_ACCUMULATE_DEFERRED_DIRECT_LIGHT_SLOT(19u, 304u);
#endif
#if NLS_DEFERRED_LIGHT_COUNT > 20
    NLS_ACCUMULATE_DEFERRED_DIRECT_LIGHT_SLOT(20u, 320u);
#endif
#if NLS_DEFERRED_LIGHT_COUNT > 21
    NLS_ACCUMULATE_DEFERRED_DIRECT_LIGHT_SLOT(21u, 336u);
#endif
#if NLS_DEFERRED_LIGHT_COUNT > 22
    NLS_ACCUMULATE_DEFERRED_DIRECT_LIGHT_SLOT(22u, 352u);
#endif
#if NLS_DEFERRED_LIGHT_COUNT > 23
    NLS_ACCUMULATE_DEFERRED_DIRECT_LIGHT_SLOT(23u, 368u);
#endif
#if NLS_DEFERRED_LIGHT_COUNT > 24
    NLS_ACCUMULATE_DEFERRED_DIRECT_LIGHT_SLOT(24u, 384u);
#endif
#if NLS_DEFERRED_LIGHT_COUNT > 25
    NLS_ACCUMULATE_DEFERRED_DIRECT_LIGHT_SLOT(25u, 400u);
#endif
#if NLS_DEFERRED_LIGHT_COUNT > 26
    NLS_ACCUMULATE_DEFERRED_DIRECT_LIGHT_SLOT(26u, 416u);
#endif
#if NLS_DEFERRED_LIGHT_COUNT > 27
    NLS_ACCUMULATE_DEFERRED_DIRECT_LIGHT_SLOT(27u, 432u);
#endif
#if NLS_DEFERRED_LIGHT_COUNT > 28
    NLS_ACCUMULATE_DEFERRED_DIRECT_LIGHT_SLOT(28u, 448u);
#endif
#if NLS_DEFERRED_LIGHT_COUNT > 29
    NLS_ACCUMULATE_DEFERRED_DIRECT_LIGHT_SLOT(29u, 464u);
#endif
#if NLS_DEFERRED_LIGHT_COUNT > 30
    NLS_ACCUMULATE_DEFERRED_DIRECT_LIGHT_SLOT(30u, 480u);
#endif
#if NLS_DEFERRED_LIGHT_COUNT > 31
    NLS_ACCUMULATE_DEFERRED_DIRECT_LIGHT_SLOT(31u, 496u);
#endif
#undef NLS_ACCUMULATE_DEFERRED_DIRECT_LIGHT_SLOT
#else
    const float3 litColor = NLSAccumulateSceneLightingPBR(
        u_ForwardLocalLightBuffer,
        worldPosition,
        geometryNormalWS,
        shadingNormalWS,
        albedoSample.rgb,
        metallic,
        roughness,
        ao,
        directVisibility);
#endif

    // The shared LDR transform preserves highlight hue and softens isolated specular peaks.
    return float4(NLSToneMapACES(litColor), 1.0f);
}
