#ifndef NLS_ENGINE_LIGHT_GRID_COMMON_HLSLI
#define NLS_ENGINE_LIGHT_GRID_COMMON_HLSLI

// Built-in HLSL already provides the shared normalization helpers through CommonTypes.hlsli.
#if defined(NLS_ENGINE_COMMON_TYPES_HLSLI) && !defined(NULLUS_SHADER_LIBRARY_COMMON_INCLUDED)
#define NLS_LIGHT_GRID_PBR_NORMALS_USE_ENGINE_COMMON
#define NULLUS_SHADER_LIBRARY_COMMON_INCLUDED
#endif
#include "NullusShaderLibrary/PBRNormals.hlsl"
#if defined(NLS_LIGHT_GRID_PBR_NORMALS_USE_ENGINE_COMMON)
#undef NULLUS_SHADER_LIBRARY_COMMON_INCLUDED
#undef NLS_LIGHT_GRID_PBR_NORMALS_USE_ENGINE_COMMON
#endif

static const uint NLS_LIGHT_TYPE_POINT = 0u;
static const uint NLS_LIGHT_TYPE_DIRECTIONAL = 1u;
static const uint NLS_LIGHT_TYPE_SPOT = 2u;
static const uint NLS_LIGHT_TYPE_AMBIENT_BOX = 3u;
static const uint NLS_LIGHT_TYPE_AMBIENT_SPHERE = 4u;
static const uint NLS_LIGHT_WORD_STRIDE = 16u;
static const uint NLS_NUM_CULLED_LIGHTS_GRID_STRIDE = 2u;
static const uint NLS_LIGHT_LINK_STRIDE = 2u;
static const float NLS_LIGHTING_SAFE_NORMAL_EPSILON = 1.0e-8f;
static const float NLS_LIGHTING_SAFE_NORMAL_MAX_LENGTH_SQ = 1.0e+20f;
static const float NLS_LIGHTING_SAFE_NORMAL_MAX_COMPONENT = 1.0e+30f;
static const float NLS_PBR_PI = 3.14159265358979323846f;
static const float NLS_PBR_INV_PI = 0.31830988618379067154f;
static const float NLS_PBR_ARTIST_LIGHT_INTENSITY_TO_RADIANCE = NLS_PBR_PI;
static const float NLS_PBR_DIELECTRIC_F0 = 0.04f;
static const float NLS_PBR_MIN_PERCEPTUAL_ROUGHNESS = 0.045f;
static const float NLS_PBR_MAX_NORMAL_VARIANCE = 0.18f;
static const float NLS_PBR_MIN_DISTRIBUTION_DENOMINATOR = 1.0e-12f;
static const float NLS_PBR_MIN_DENOMINATOR = 1.0e-6f;

cbuffer ForwardLightData : register(b0, space1)
{
    float4x4 u_LightGridView;
    float4x4 u_LightGridProjection;
    float4x4 u_LightGridInverseViewProjection;
    float4x4 u_LightGridClipToView;
    float4 u_LightGridCameraWorldPositionNearPlane;
    float4 u_LightGridRenderSizeFarPlane;
    float4 u_LightGridGridParams;
    float4 u_LightGridLightingParams;
    float4 u_LightGridZParams;
    float4 u_LightGridPixelParams;
};

struct NLSLightGridLight
{
    float3 positionWS;
    float range;
    float3 directionWS;
    uint type;
    float3 color;
    float intensity;
    float constantAttenuation;
    float linearAttenuation;
    float quadraticAttenuation;
    float outerCutoffDegrees;
};

bool NLSIsFiniteLighting3(float3 value)
{
    return all(value == value) && all(abs(value) < NLS_LIGHTING_SAFE_NORMAL_MAX_COMPONENT);
}

float3 NLSLightingNormalizeFallback(float3 fallback)
{
    const float lengthSq = dot(fallback, fallback);
    if (NLSIsFiniteLighting3(fallback) &&
        lengthSq > NLS_LIGHTING_SAFE_NORMAL_EPSILON &&
        lengthSq < NLS_LIGHTING_SAFE_NORMAL_MAX_LENGTH_SQ)
    {
        return fallback * rsqrt(lengthSq);
    }

    return float3(0.0f, 1.0f, 0.0f);
}

float3 NLSSafeLightingNormalize(float3 value, float3 fallback)
{
    const float lengthSq = dot(value, value);
    if (NLSIsFiniteLighting3(value) &&
        lengthSq > NLS_LIGHTING_SAFE_NORMAL_EPSILON &&
        lengthSq < NLS_LIGHTING_SAFE_NORMAL_MAX_LENGTH_SQ)
    {
        return value * rsqrt(lengthSq);
    }

    return NLSLightingNormalizeFallback(fallback);
}

float3 NLSSafeLightingPerpendicular(float3 normal)
{
    const float3 safeNormal = NLSSafeLightingNormalize(normal, float3(0.0f, 1.0f, 0.0f));
    const float3 reference = abs(safeNormal.y) < 0.999f
        ? float3(0.0f, 1.0f, 0.0f)
        : float3(1.0f, 0.0f, 0.0f);
    return NLSSafeLightingNormalize(cross(reference, safeNormal), float3(0.0f, 0.0f, 1.0f));
}

uint NLSGetGridSizeX() { return (uint)u_LightGridGridParams.x; }
uint NLSGetGridSizeY() { return (uint)u_LightGridGridParams.y; }
uint NLSGetGridSizeZ() { return (uint)u_LightGridGridParams.z; }
uint NLSGetMaxLightsPerCluster() { return (uint)u_LightGridGridParams.w; }
uint NLSGetNumLocalLights() { return (uint)u_LightGridLightingParams.x; }
uint NLSGetSceneLightCount() { return (uint)u_LightGridLightingParams.x; }
float NLSGetDepthFogFactor() { return u_LightGridLightingParams.y; }
float NLSGetAmbientFloor() { return u_LightGridLightingParams.z; }
float NLSGetVisibleAmbientFloor() { return max(NLSGetAmbientFloor(), 0.18f); }
float NLSGetNearPlane() { return u_LightGridCameraWorldPositionNearPlane.w; }
float NLSGetFarPlane() { return u_LightGridRenderSizeFarPlane.w; }
float3 NLSGetCameraWorldPosition() { return u_LightGridCameraWorldPositionNearPlane.xyz; }
float3 NLSGetLightGridZParams() { return u_LightGridZParams.xyz; }
bool NLSUsesLinkedListCulling() { return u_LightGridZParams.w > 0.5f; }
uint3 NLSGetCulledGridSize() { return uint3(NLSGetGridSizeX(), NLSGetGridSizeY(), NLSGetGridSizeZ()); }

float NLSComputeCellNearViewDepthFromZSlice(uint zSlice)
{
    const float3 zParams = NLSGetLightGridZParams();
    float sliceDepth = (exp2((float)zSlice / zParams.z) - zParams.y) / zParams.x;

    if (zSlice == NLSGetGridSizeZ())
        sliceDepth = 2000000.0f;
    if (zSlice == 0u)
        sliceDepth = 0.0f;
    return sliceDepth;
}

float NLSConvertViewDepthToDeviceZ(float viewDepth)
{
    const float nearPlane = max(NLSGetNearPlane(), 1e-4f);
    const float farPlane = max(NLSGetFarPlane(), nearPlane + 1.0f);
    return saturate((viewDepth - nearPlane) / (farPlane - nearPlane));
}

void NLSComputeCellViewAABB(uint3 gridCoordinate, out float3 viewTileMin, out float3 viewTileMax)
{
    const float2 invRenderSize = float2(u_LightGridRenderSizeFarPlane.z, u_LightGridPixelParams.y);
    const float pixelSize = max(1.0f, u_LightGridPixelParams.x);
    const float2 tileSize = float2(2.0f, -2.0f) * pixelSize * invRenderSize;
    const float2 unitPlaneMin = float2(-1.0f, 1.0f);

    const float2 tileMin = (float2)gridCoordinate.xy * tileSize + unitPlaneMin;
    const float2 tileMax = ((float2)gridCoordinate.xy + 1.0f) * tileSize + unitPlaneMin;
    const float minTileZ = NLSComputeCellNearViewDepthFromZSlice(gridCoordinate.z);
    const float maxTileZ = NLSComputeCellNearViewDepthFromZSlice(gridCoordinate.z + 1u);
    const float minDeviceZ = NLSConvertViewDepthToDeviceZ(minTileZ);
    const float maxDeviceZ = NLSConvertViewDepthToDeviceZ(maxTileZ);

    const float4 minCorner0 = mul(float4(tileMin.x, tileMin.y, minDeviceZ, 1.0f), u_LightGridClipToView);
    const float4 minCorner1 = mul(float4(tileMax.x, tileMin.y, minDeviceZ, 1.0f), u_LightGridClipToView);
    const float4 minCorner2 = mul(float4(tileMin.x, tileMax.y, minDeviceZ, 1.0f), u_LightGridClipToView);
    const float4 minCorner3 = mul(float4(tileMax.x, tileMax.y, minDeviceZ, 1.0f), u_LightGridClipToView);
    const float4 maxCorner0 = mul(float4(tileMin.x, tileMin.y, maxDeviceZ, 1.0f), u_LightGridClipToView);
    const float4 maxCorner1 = mul(float4(tileMax.x, tileMin.y, maxDeviceZ, 1.0f), u_LightGridClipToView);
    const float4 maxCorner2 = mul(float4(tileMin.x, tileMax.y, maxDeviceZ, 1.0f), u_LightGridClipToView);
    const float4 maxCorner3 = mul(float4(tileMax.x, tileMax.y, maxDeviceZ, 1.0f), u_LightGridClipToView);

    const float2 viewMin0 = minCorner0.xy / max(abs(minCorner0.w), 1e-5f);
    const float2 viewMin1 = minCorner1.xy / max(abs(minCorner1.w), 1e-5f);
    const float2 viewMin2 = minCorner2.xy / max(abs(minCorner2.w), 1e-5f);
    const float2 viewMin3 = minCorner3.xy / max(abs(minCorner3.w), 1e-5f);
    const float2 viewMax0 = maxCorner0.xy / max(abs(maxCorner0.w), 1e-5f);
    const float2 viewMax1 = maxCorner1.xy / max(abs(maxCorner1.w), 1e-5f);
    const float2 viewMax2 = maxCorner2.xy / max(abs(maxCorner2.w), 1e-5f);
    const float2 viewMax3 = maxCorner3.xy / max(abs(maxCorner3.w), 1e-5f);

    viewTileMin.xy = min(min(min(viewMin0, viewMin1), min(viewMin2, viewMin3)), min(min(viewMax0, viewMax1), min(viewMax2, viewMax3)));
    viewTileMax.xy = max(max(max(viewMin0, viewMin1), max(viewMin2, viewMin3)), max(max(viewMax0, viewMax1), max(viewMax2, viewMax3)));
    viewTileMin.z = minTileZ;
    viewTileMax.z = maxTileZ;
}

float NLSComputeSquaredDistanceFromBoxToPoint(float3 boxCenter, float3 boxExtent, float3 queryPoint)
{
    const float3 offset = abs(queryPoint - boxCenter) - boxExtent;
    const float3 outside = max(offset, 0.0f.xxx);
    return dot(outside, outside);
}

bool NLSIsAabbOutsideInfiniteAcuteConeApprox(
    float3 coneVertex,
    float3 coneAxis,
    float tanConeAngle,
    float3 aabbCenter,
    float3 aabbExtent)
{
    const float3 d = aabbCenter - coneVertex;
    const float3 safeConeAxis = NLSSafeLightingNormalize(coneAxis, float3(0.0f, 0.0f, -1.0f));
    const float3 m = -NLSSafeLightingNormalize(cross(cross(d, safeConeAxis), safeConeAxis), NLSSafeLightingPerpendicular(safeConeAxis));
    const float3 n = -tanConeAngle * safeConeAxis + m;
    const float dist = dot(d, n);
    const float radius = dot(aabbExtent, abs(n));
    return dist > radius;
}

float NLSClamp01(float value)
{
    return clamp(value, 0.0f, 1.0f);
}

float3 NLSToneMapACES(float3 hdrColor)
{
    hdrColor = NLSIsFiniteLighting3(hdrColor) ? max(hdrColor, 0.0f.xxx) : 0.0f.xxx;
    const float peakChannel = max(hdrColor.r, max(hdrColor.g, hdrColor.b));
    if (peakChannel > 1.0f)
    {
        const float shoulderExcess = peakChannel - 1.0f;
        const float compressedPeak = 1.0f + shoulderExcess / (1.0f + shoulderExcess);
        hdrColor *= compressedPeak / peakChannel;
    }
    const float a = 2.51f;
    const float b = 0.03f;
    const float c = 2.43f;
    const float d = 0.59f;
    const float e = 0.14f;
    return saturate(
        (hdrColor * (a * hdrColor + b)) /
        max(hdrColor * (c * hdrColor + d) + e, NLS_PBR_MIN_DENOMINATOR.xxx));
}

uint NLSGetClusterIndex(uint x, uint y, uint z)
{
    return x + y * NLSGetGridSizeX() + z * NLSGetGridSizeX() * NLSGetGridSizeY();
}

float NLSViewDepthToSlice(float depth)
{
    const float nearPlane = NLSGetNearPlane();
    const float farPlane = NLSGetFarPlane();
    if (farPlane <= nearPlane || NLSGetGridSizeZ() == 0u)
        return 0.0f;

    const float3 zParams = NLSGetLightGridZParams();
    return max(0.0f, log2(depth * zParams.x + zParams.y) * zParams.z);
}

NLSLightGridLight NLSLoadLight(StructuredBuffer<uint> forwardLocalLightBuffer, uint lightIndex)
{
    const uint baseIndex = lightIndex * NLS_LIGHT_WORD_STRIDE;
    NLSLightGridLight light;
    light.positionWS = float3(asfloat(forwardLocalLightBuffer[baseIndex + 0u]), asfloat(forwardLocalLightBuffer[baseIndex + 1u]), asfloat(forwardLocalLightBuffer[baseIndex + 2u]));
    light.range = asfloat(forwardLocalLightBuffer[baseIndex + 3u]);
    light.directionWS = NLSSafeLightingNormalize(
        float3(asfloat(forwardLocalLightBuffer[baseIndex + 4u]), asfloat(forwardLocalLightBuffer[baseIndex + 5u]), asfloat(forwardLocalLightBuffer[baseIndex + 6u])),
        float3(0.0f, -1.0f, 0.0f));
    light.type = forwardLocalLightBuffer[baseIndex + 7u];
    light.color = float3(asfloat(forwardLocalLightBuffer[baseIndex + 8u]), asfloat(forwardLocalLightBuffer[baseIndex + 9u]), asfloat(forwardLocalLightBuffer[baseIndex + 10u]));
    light.intensity = asfloat(forwardLocalLightBuffer[baseIndex + 11u]);
    light.constantAttenuation = asfloat(forwardLocalLightBuffer[baseIndex + 12u]);
    light.linearAttenuation = asfloat(forwardLocalLightBuffer[baseIndex + 13u]);
    light.quadraticAttenuation = asfloat(forwardLocalLightBuffer[baseIndex + 14u]);
    light.outerCutoffDegrees = asfloat(forwardLocalLightBuffer[baseIndex + 15u]);
    return light;
}

bool NLSIsGlobalLight(NLSLightGridLight light)
{
    return light.type == NLS_LIGHT_TYPE_DIRECTIONAL || light.type == NLS_LIGHT_TYPE_AMBIENT_BOX;
}

bool NLSIsAmbientLight(NLSLightGridLight light)
{
    return light.type == NLS_LIGHT_TYPE_AMBIENT_BOX || light.type == NLS_LIGHT_TYPE_AMBIENT_SPHERE;
}

bool NLSIsGlobalDeferredLight(NLSLightGridLight light)
{
    return light.type == NLS_LIGHT_TYPE_DIRECTIONAL || NLSIsAmbientLight(light);
}

bool NLSComputeLightClusterRange(
    NLSLightGridLight light,
    out uint3 minRange,
    out uint3 maxRange)
{
    minRange = uint3(0u, 0u, 0u);
    maxRange = uint3(0u, 0u, 0u);

    if (NLSIsGlobalLight(light))
    {
        maxRange = uint3(NLSGetGridSizeX() - 1u, NLSGetGridSizeY() - 1u, NLSGetGridSizeZ() - 1u);
        return true;
    }

    const float radius = light.range;
    if (!isfinite(radius) || radius <= 0.0f)
    {
        maxRange = uint3(NLSGetGridSizeX() - 1u, NLSGetGridSizeY() - 1u, NLSGetGridSizeZ() - 1u);
        return true;
    }

    const float4 viewPosition4 = mul(u_LightGridView, float4(light.positionWS, 1.0f));
    const float3 viewPosition = viewPosition4.xyz;
    const float depth = -viewPosition.z;
    if (depth + radius < NLSGetNearPlane() || depth - radius > NLSGetFarPlane())
        return false;

    const float4 clip = mul(u_LightGridProjection, float4(viewPosition, 1.0f));
    if (abs(clip.w) <= 1e-5f)
    {
        maxRange = uint3(NLSGetGridSizeX() - 1u, NLSGetGridSizeY() - 1u, NLSGetGridSizeZ() - 1u);
        return true;
    }

    const float centerNdcX = clip.x / clip.w;
    const float centerNdcY = clip.y / clip.w;
    const float projX = u_LightGridProjection[0][0];
    const float projY = u_LightGridProjection[1][1];
    const float safeDepth = max(depth, NLSGetNearPlane());
    const float radiusNdcX = abs(projX) * radius / safeDepth;
    const float radiusNdcY = abs(projY) * radius / safeDepth;

    const float minNdcX = clamp(centerNdcX - radiusNdcX, -1.0f, 1.0f);
    const float maxNdcX = clamp(centerNdcX + radiusNdcX, -1.0f, 1.0f);
    const float minNdcY = clamp(centerNdcY - radiusNdcY, -1.0f, 1.0f);
    const float maxNdcY = clamp(centerNdcY + radiusNdcY, -1.0f, 1.0f);

    minRange.x = min(NLSGetGridSizeX() - 1u, (uint)(NLSClamp01(minNdcX * 0.5f + 0.5f) * (float)NLSGetGridSizeX()));
    maxRange.x = min(NLSGetGridSizeX() - 1u, (uint)(NLSClamp01(maxNdcX * 0.5f + 0.5f) * (float)NLSGetGridSizeX()));
    minRange.y = min(NLSGetGridSizeY() - 1u, (uint)(NLSClamp01(minNdcY * 0.5f + 0.5f) * (float)NLSGetGridSizeY()));
    maxRange.y = min(NLSGetGridSizeY() - 1u, (uint)(NLSClamp01(maxNdcY * 0.5f + 0.5f) * (float)NLSGetGridSizeY()));

    const float minDepth = max(NLSGetNearPlane(), depth - radius);
    const float maxDepth = min(NLSGetFarPlane(), depth + radius);
    minRange.z = min(NLSGetGridSizeZ() - 1u, (uint)NLSViewDepthToSlice(minDepth));
    maxRange.z = min(NLSGetGridSizeZ() - 1u, (uint)ceil(NLSViewDepthToSlice(maxDepth)));
    return true;
}

uint NLSComputeClusterIndexFromWorldPosition(float3 worldPosition)
{
    const float4 viewPosition4 = mul(u_LightGridView, float4(worldPosition, 1.0f));
    const float depth = -viewPosition4.z;
    const float4 clip = mul(u_LightGridProjection, float4(viewPosition4.xyz, 1.0f));
    const float2 ndc = clip.xy / max(abs(clip.w), 1e-5f);
    const uint x = min(NLSGetGridSizeX() - 1u, (uint)(NLSClamp01(ndc.x * 0.5f + 0.5f) * (float)NLSGetGridSizeX()));
    const uint y = min(NLSGetGridSizeY() - 1u, (uint)(NLSClamp01(ndc.y * 0.5f + 0.5f) * (float)NLSGetGridSizeY()));
    const uint z = min(NLSGetGridSizeZ() - 1u, (uint)NLSViewDepthToSlice(depth));
    return NLSGetClusterIndex(x, y, z);
}

float NLSComputePointAttenuation(NLSLightGridLight light, float distanceToLight)
{
    const float attenuation = light.constantAttenuation +
        light.linearAttenuation * distanceToLight +
        light.quadraticAttenuation * distanceToLight * distanceToLight;
    return attenuation > 0.0f ? (1.0f / attenuation) : 1.0f;
}

float3 NLSSafePbrAlbedo(float3 albedo)
{
    return NLSIsFiniteLighting3(albedo) ? saturate(albedo) : 0.0f.xxx;
}

float NLSSafePbrMetallic(float metallic)
{
    return isfinite(metallic) ? saturate(metallic) : 0.0f;
}

float NLSSafePbrRoughness(float roughness)
{
    return isfinite(roughness)
        ? clamp(roughness, NLS_PBR_MIN_PERCEPTUAL_ROUGHNESS, 1.0f)
        : 1.0f;
}

float NLSFilterPerceptualRoughness(float3 safeNormalWS, float roughness)
{
    const float3 normalDx = ddx(safeNormalWS);
    const float3 normalDy = ddy(safeNormalWS);
    float normalVariance = max(dot(normalDx, normalDx), dot(normalDy, normalDy));
    normalVariance = isfinite(normalVariance)
        ? clamp(normalVariance, 0.0f, NLS_PBR_MAX_NORMAL_VARIANCE)
        : 0.0f;

    const float safeRoughness = NLSSafePbrRoughness(roughness);
    const float roughnessSquared = safeRoughness * safeRoughness;
    return NLSSafePbrRoughness(sqrt(saturate(roughnessSquared + normalVariance)));
}

float NLSSafePbrAo(float ao)
{
    return isfinite(ao) ? saturate(ao) : 1.0f;
}

float NLSDistributionGGX(float ndoth, float perceptualRoughness)
{
    const float alpha = perceptualRoughness * perceptualRoughness;
    const float alphaSquared = alpha * alpha;
    const float denominatorTerm = ndoth * ndoth * (alphaSquared - 1.0f) + 1.0f;
    return alphaSquared /
        max(NLS_PBR_PI * denominatorTerm * denominatorTerm, NLS_PBR_MIN_DISTRIBUTION_DENOMINATOR);
}

float NLSGeometrySchlickGGX(float ndotDirection, float perceptualRoughness)
{
    const float remappedRoughness = perceptualRoughness + 1.0f;
    const float k = remappedRoughness * remappedRoughness * 0.125f;
    return ndotDirection /
        max(ndotDirection * (1.0f - k) + k, NLS_PBR_MIN_DENOMINATOR);
}

float NLSGeometrySmith(float ndotv, float ndotl, float perceptualRoughness)
{
    return NLSGeometrySchlickGGX(ndotv, perceptualRoughness) *
        NLSGeometrySchlickGGX(ndotl, perceptualRoughness);
}

float3 NLSFresnelSchlick(float viewDotHalf, float3 f0)
{
    const float oneMinusViewDotHalf = 1.0f - saturate(viewDotHalf);
    const float fresnelFactor = oneMinusViewDotHalf * oneMinusViewDotHalf *
        oneMinusViewDotHalf * oneMinusViewDotHalf * oneMinusViewDotHalf;
    return f0 + (1.0f.xxx - f0) * fresnelFactor;
}

float3 NLSEvaluateCookTorranceShadingDirect(
    float3 shadingNormalWS,
    float3 viewDir,
    float3 lightDir,
    float3 safeAlbedo,
    float safeMetallic,
    float safeRoughness,
    float3 lightColor,
    float lightIntensity,
    float attenuation)
{
    const float ndotv = saturate(dot(shadingNormalWS, viewDir));
    const float ndotl = saturate(dot(shadingNormalWS, lightDir));

    const float3 dielectricF0 = NLS_PBR_DIELECTRIC_F0.xxx;
    const float3 f0 = lerp(dielectricF0, safeAlbedo, safeMetallic);
    float3 fresnel = f0;
    float3 specular = 0.0f.xxx;
    if (ndotv > 0.0f)
    {
        const float3 halfVector = NLSSafeLightingNormalize(lightDir + viewDir, shadingNormalWS);
        const float ndoth = saturate(dot(shadingNormalWS, halfVector));
        const float viewDotHalf = saturate(dot(viewDir, halfVector));
        fresnel = NLSFresnelSchlick(viewDotHalf, f0);
        const float distribution = NLSDistributionGGX(ndoth, safeRoughness);
        const float geometry = NLSGeometrySmith(ndotv, ndotl, safeRoughness);
        const float3 numerator = distribution * geometry * fresnel;
        const float denominator = max(
            4.0f * ndotv * ndotl,
            NLS_PBR_MIN_DENOMINATOR);
        specular = numerator / denominator;
    }
    const float3 kd = (1.0f.xxx - fresnel) * (1.0f - safeMetallic);
    const float3 diffuse = kd * safeAlbedo * NLS_PBR_INV_PI;
    const float3 brdf = diffuse + specular;

    const float3 safeLightColor = NLSIsFiniteLighting3(lightColor) ? max(lightColor, 0.0f.xxx) : 0.0f.xxx;
    const float safeIntensity = isfinite(lightIntensity) ? max(lightIntensity, 0.0f) : 0.0f;
    const float visibility = isfinite(attenuation) ? max(attenuation, 0.0f) : 0.0f;
    const float3 radiance = safeLightColor *
        (safeIntensity * NLS_PBR_ARTIST_LIGHT_INTENSITY_TO_RADIANCE);
    return brdf * radiance * ndotl * visibility;
}

float3 NLSEvaluateCookTorranceDirect(
    float3 geometryNormalWS,
    float3 shadingNormalWS,
    float3 viewDir,
    float3 lightDir,
    float3 safeAlbedo,
    float safeMetallic,
    float safeRoughness,
    float3 lightColor,
    float lightIntensity,
    float attenuation)
{
    const float geometryNdotL = dot(geometryNormalWS, lightDir);
    const float geometryNdotV = dot(geometryNormalWS, viewDir);
    if (geometryNdotL <= 0.0f || geometryNdotV <= 0.0f)
        return 0.0f.xxx;
    const float geometryFade = NLSGeometryHorizonFade(geometryNdotL) *
        NLSGeometryHorizonFade(geometryNdotV);

    const float3 shadingDirect = NLSEvaluateCookTorranceShadingDirect(
        shadingNormalWS,
        viewDir,
        lightDir,
        safeAlbedo,
        safeMetallic,
        safeRoughness,
        lightColor,
        lightIntensity,
        attenuation);
    return shadingDirect * geometryFade;
}

// Compatibility overload until Deferred stores geometry and shading normals separately.
float3 NLSEvaluateCookTorranceDirect(
    float3 shadingNormalWS,
    float3 viewDir,
    float3 lightDir,
    float3 safeAlbedo,
    float safeMetallic,
    float safeRoughness,
    float3 lightColor,
    float lightIntensity,
    float attenuation)
{
    return NLSEvaluateCookTorranceShadingDirect(
        shadingNormalWS,
        viewDir,
        lightDir,
        safeAlbedo,
        safeMetallic,
        safeRoughness,
        lightColor,
        lightIntensity,
        attenuation);
}

float3 NLSAccumulateClusteredLightingPhong(
    StructuredBuffer<uint> forwardLocalLightBuffer,
    StructuredBuffer<uint> numCulledLightsGrid,
    StructuredBuffer<uint> culledLightDataGrid,
    float3 worldPosition,
    float3 normalWS,
    float3 baseColor,
    float3 specularColor,
    float shininess)
{
    const uint clusterIndex = NLSComputeClusterIndexFromWorldPosition(worldPosition);
    const uint recordBase = clusterIndex * NLS_NUM_CULLED_LIGHTS_GRID_STRIDE;
    const uint offset = numCulledLightsGrid[recordBase + 0u];
    const uint count = numCulledLightsGrid[recordBase + 1u];
    const float3 safeNormalWS = NLSSafeLightingNormalize(normalWS, float3(0.0f, 0.0f, 1.0f));
    const float3 viewDir = NLSSafeLightingNormalize(NLSGetCameraWorldPosition() - worldPosition, safeNormalWS);
    float3 lighting = baseColor * NLSGetAmbientFloor();

    [loop]
    for (uint i = 0u; i < count; ++i)
    {
        const NLSLightGridLight light = NLSLoadLight(forwardLocalLightBuffer, culledLightDataGrid[offset + i]);
        if (light.type == NLS_LIGHT_TYPE_AMBIENT_BOX || light.type == NLS_LIGHT_TYPE_AMBIENT_SPHERE)
        {
            lighting += baseColor * light.color * max(light.intensity, 0.0f);
            continue;
        }

        float3 lightDir = 0.0f.xxx;
        float attenuation = 1.0f;
        if (light.type == NLS_LIGHT_TYPE_DIRECTIONAL)
        {
            lightDir = NLSSafeLightingNormalize(-light.directionWS, float3(0.0f, 1.0f, 0.0f));
        }
        else
        {
            const float3 toLight = light.positionWS - worldPosition;
            const float distanceToLight = length(toLight);
            if (distanceToLight > max(light.range, 0.0001f))
                continue;

            lightDir = NLSSafeLightingNormalize(toLight, safeNormalWS);
            attenuation = NLSComputePointAttenuation(light, distanceToLight);
            if (light.type == NLS_LIGHT_TYPE_SPOT)
            {
                const float spotCos = dot(NLSSafeLightingNormalize(-light.directionWS, lightDir), lightDir);
                const float outerCutoffCos = cos(radians(light.outerCutoffDegrees));
                attenuation *= saturate((spotCos - outerCutoffCos) / max(1.0f - outerCutoffCos, 1e-3f));
            }
        }

        const float ndotl = saturate(dot(safeNormalWS, lightDir));
        const float3 halfVector = NLSSafeLightingNormalize(lightDir + viewDir, safeNormalWS);
        const float specularTerm = pow(saturate(dot(safeNormalWS, halfVector)), max(shininess, 1.0f));
        lighting += baseColor * light.color * (ndotl * light.intensity * attenuation);
        lighting += specularColor * light.color * (specularTerm * light.intensity * attenuation);
    }

    return lighting;
}

float3 NLSAccumulateClusteredLightingPBR(
    StructuredBuffer<uint> forwardLocalLightBuffer,
    StructuredBuffer<uint> numCulledLightsGrid,
    StructuredBuffer<uint> culledLightDataGrid,
    float3 worldPosition,
    float3 geometryNormalWS,
    float3 shadingNormalWS,
    float3 albedo,
    float metallic,
    float roughness,
    float ao)
{
    const uint clusterIndex = NLSComputeClusterIndexFromWorldPosition(worldPosition);
    const uint recordBase = clusterIndex * NLS_NUM_CULLED_LIGHTS_GRID_STRIDE;
    const uint offset = numCulledLightsGrid[recordBase + 0u];
    const uint count = numCulledLightsGrid[recordBase + 1u];
    const float3 safeGeometryNormalWS = NLSSafeLightingNormalize(
        geometryNormalWS,
        float3(0.0f, 0.0f, 1.0f));
    const float3 safeShadingNormalWS = NLSSafeLightingNormalize(
        shadingNormalWS,
        safeGeometryNormalWS);
    const float3 viewDir = NLSSafeLightingNormalize(
        NLSGetCameraWorldPosition() - worldPosition,
        safeGeometryNormalWS);
    const float3 safeAlbedo = NLSSafePbrAlbedo(albedo);
    const float safeMetallic = NLSSafePbrMetallic(metallic);
    const float filteredRoughness = NLSFilterPerceptualRoughness(safeShadingNormalWS, roughness);
    const float safeAo = NLSSafePbrAo(ao);
    float3 lighting = safeAlbedo * (NLSGetVisibleAmbientFloor() * safeAo);

    [loop]
    for (uint i = 0u; i < count; ++i)
    {
        const NLSLightGridLight light = NLSLoadLight(forwardLocalLightBuffer, culledLightDataGrid[offset + i]);
        if (light.type == NLS_LIGHT_TYPE_AMBIENT_BOX || light.type == NLS_LIGHT_TYPE_AMBIENT_SPHERE)
        {
            lighting += safeAlbedo * light.color * max(light.intensity, 0.0f) * safeAo;
            continue;
        }

        float3 lightDir = 0.0f.xxx;
        float attenuation = 1.0f;
        if (light.type == NLS_LIGHT_TYPE_DIRECTIONAL)
        {
            lightDir = NLSSafeLightingNormalize(-light.directionWS, float3(0.0f, 1.0f, 0.0f));
        }
        else
        {
            const float3 toLight = light.positionWS - worldPosition;
            const float distanceToLight = length(toLight);
            if (distanceToLight > max(light.range, 0.0001f))
                continue;

            lightDir = NLSSafeLightingNormalize(toLight, safeGeometryNormalWS);
            attenuation = NLSComputePointAttenuation(light, distanceToLight);
            if (light.type == NLS_LIGHT_TYPE_SPOT)
            {
                const float spotCos = dot(NLSSafeLightingNormalize(-light.directionWS, lightDir), lightDir);
                const float outerCutoffCos = cos(radians(light.outerCutoffDegrees));
                attenuation *= saturate((spotCos - outerCutoffCos) / max(1.0f - outerCutoffCos, 1e-3f));
            }
        }

        lighting += NLSEvaluateCookTorranceDirect(
            safeGeometryNormalWS,
            safeShadingNormalWS,
            viewDir,
            lightDir,
            safeAlbedo,
            safeMetallic,
            filteredRoughness,
            light.color,
            light.intensity,
            attenuation);
    }

    return lighting;
}

float3 NLSAccumulateSceneLightingPBR(
    StructuredBuffer<uint> forwardLocalLightBuffer,
    float3 worldPosition,
    float3 normalWS,
    float3 albedo,
    float metallic,
    float roughness,
    float ao)
{
    const float3 safeNormalWS = NLSSafeLightingNormalize(normalWS, float3(0.0f, 0.0f, 1.0f));
    const float3 viewDir = NLSSafeLightingNormalize(NLSGetCameraWorldPosition() - worldPosition, safeNormalWS);
    const float3 safeAlbedo = NLSSafePbrAlbedo(albedo);
    const float safeMetallic = NLSSafePbrMetallic(metallic);
    const float filteredRoughness = NLSFilterPerceptualRoughness(safeNormalWS, roughness);
    const float safeAo = NLSSafePbrAo(ao);
    float3 lighting = safeAlbedo * (NLSGetVisibleAmbientFloor() * safeAo);

    [loop]
    for (uint lightIndex = 0u; lightIndex < NLSGetSceneLightCount(); ++lightIndex)
    {
        const NLSLightGridLight light = NLSLoadLight(forwardLocalLightBuffer, lightIndex);
        if (NLSIsAmbientLight(light))
        {
            lighting += safeAlbedo * light.color * max(light.intensity, 0.0f) * safeAo;
            continue;
        }

        float3 lightDir = 0.0f.xxx;
        float attenuation = 1.0f;
        if (light.type == NLS_LIGHT_TYPE_DIRECTIONAL)
        {
            lightDir = NLSSafeLightingNormalize(-light.directionWS, float3(0.0f, 1.0f, 0.0f));
        }
        else
        {
            const float3 toLight = light.positionWS - worldPosition;
            const float distanceToLight = length(toLight);
            if (distanceToLight > max(light.range, 0.0001f))
                continue;

            lightDir = NLSSafeLightingNormalize(toLight, safeNormalWS);
            attenuation = NLSComputePointAttenuation(light, distanceToLight);
            if (light.type == NLS_LIGHT_TYPE_SPOT)
            {
                const float spotCos = dot(NLSSafeLightingNormalize(-light.directionWS, lightDir), lightDir);
                const float outerCutoffCos = cos(radians(light.outerCutoffDegrees));
                attenuation *= saturate((spotCos - outerCutoffCos) / max(1.0f - outerCutoffCos, 1e-3f));
            }
        }

        lighting += NLSEvaluateCookTorranceDirect(
            safeNormalWS,
            viewDir,
            lightDir,
            safeAlbedo,
            safeMetallic,
            filteredRoughness,
            light.color,
            light.intensity,
            attenuation);
    }

    return lighting;
}

#endif
