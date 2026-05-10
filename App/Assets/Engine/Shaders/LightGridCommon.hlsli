#ifndef NLS_ENGINE_LIGHT_GRID_COMMON_HLSLI
#define NLS_ENGINE_LIGHT_GRID_COMMON_HLSLI

static const uint NLS_LIGHT_TYPE_POINT = 0u;
static const uint NLS_LIGHT_TYPE_DIRECTIONAL = 1u;
static const uint NLS_LIGHT_TYPE_SPOT = 2u;
static const uint NLS_LIGHT_TYPE_AMBIENT_BOX = 3u;
static const uint NLS_LIGHT_TYPE_AMBIENT_SPHERE = 4u;
static const uint NLS_LIGHT_WORD_STRIDE = 16u;

cbuffer LightGridPassConstants : register(b0, space1)
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

uint NLSGetGridSizeX() { return (uint)u_LightGridGridParams.x; }
uint NLSGetGridSizeY() { return (uint)u_LightGridGridParams.y; }
uint NLSGetGridSizeZ() { return (uint)u_LightGridGridParams.z; }
uint NLSGetMaxLightsPerCluster() { return (uint)u_LightGridGridParams.w; }
uint NLSGetSceneLightCount() { return (uint)u_LightGridLightingParams.x; }
float NLSGetDepthFogFactor() { return u_LightGridLightingParams.y; }
float NLSGetAmbientFloor() { return u_LightGridLightingParams.z; }
float NLSGetNearPlane() { return u_LightGridCameraWorldPositionNearPlane.w; }
float NLSGetFarPlane() { return u_LightGridRenderSizeFarPlane.w; }
float3 NLSGetCameraWorldPosition() { return u_LightGridCameraWorldPositionNearPlane.xyz; }
float3 NLSGetLightGridZParams() { return u_LightGridZParams.xyz; }
bool NLSUsesLinkedListCulling() { return u_LightGridZParams.w > 0.5f; }

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

float NLSComputeSquaredDistanceFromBoxToPoint(float3 boxCenter, float3 boxExtent, float3 point)
{
    const float3 offset = abs(point - boxCenter) - boxExtent;
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
    const float3 m = -normalize(cross(cross(d, coneAxis), coneAxis));
    const float3 n = -tanConeAngle * coneAxis + m;
    const float dist = dot(d, n);
    const float radius = dot(aabbExtent, abs(n));
    return dist > radius;
}

float NLSClamp01(float value)
{
    return clamp(value, 0.0f, 1.0f);
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

NLSLightGridLight NLSLoadLight(StructuredBuffer<uint> packedLights, uint lightIndex)
{
    const uint baseIndex = lightIndex * NLS_LIGHT_WORD_STRIDE;
    NLSLightGridLight light;
    light.positionWS = float3(asfloat(packedLights[baseIndex + 0u]), asfloat(packedLights[baseIndex + 1u]), asfloat(packedLights[baseIndex + 2u]));
    light.range = asfloat(packedLights[baseIndex + 3u]);
    light.directionWS = normalize(float3(asfloat(packedLights[baseIndex + 4u]), asfloat(packedLights[baseIndex + 5u]), asfloat(packedLights[baseIndex + 6u])));
    light.type = packedLights[baseIndex + 7u];
    light.color = float3(asfloat(packedLights[baseIndex + 8u]), asfloat(packedLights[baseIndex + 9u]), asfloat(packedLights[baseIndex + 10u]));
    light.intensity = asfloat(packedLights[baseIndex + 11u]);
    light.constantAttenuation = asfloat(packedLights[baseIndex + 12u]);
    light.linearAttenuation = asfloat(packedLights[baseIndex + 13u]);
    light.quadraticAttenuation = asfloat(packedLights[baseIndex + 14u]);
    light.outerCutoffDegrees = asfloat(packedLights[baseIndex + 15u]);
    return light;
}

bool NLSIsGlobalLight(NLSLightGridLight light)
{
    return light.type == NLS_LIGHT_TYPE_DIRECTIONAL || light.type == NLS_LIGHT_TYPE_AMBIENT_BOX;
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

float3 NLSAccumulateClusteredLightingPhong(
    StructuredBuffer<uint> packedLights,
    StructuredBuffer<uint> clusterRecords,
    StructuredBuffer<uint> compactIndices,
    float3 worldPosition,
    float3 normalWS,
    float3 baseColor,
    float3 specularColor,
    float shininess)
{
    const uint clusterIndex = NLSComputeClusterIndexFromWorldPosition(worldPosition);
    const uint recordBase = clusterIndex * 2u;
    const uint offset = clusterRecords[recordBase + 0u];
    const uint count = clusterRecords[recordBase + 1u];
    const float3 viewDir = normalize(NLSGetCameraWorldPosition() - worldPosition);
    float3 lighting = baseColor * NLSGetAmbientFloor();

    [loop]
    for (uint i = 0u; i < count; ++i)
    {
        const NLSLightGridLight light = NLSLoadLight(packedLights, compactIndices[offset + i]);
        if (light.type == NLS_LIGHT_TYPE_AMBIENT_BOX || light.type == NLS_LIGHT_TYPE_AMBIENT_SPHERE)
        {
            lighting += baseColor * light.color * max(light.intensity, 0.0f);
            continue;
        }

        float3 lightDir = 0.0f.xxx;
        float attenuation = 1.0f;
        if (light.type == NLS_LIGHT_TYPE_DIRECTIONAL)
        {
            lightDir = normalize(-light.directionWS);
        }
        else
        {
            const float3 toLight = light.positionWS - worldPosition;
            const float distanceToLight = length(toLight);
            if (distanceToLight > max(light.range, 0.0001f))
                continue;

            lightDir = toLight / max(distanceToLight, 1e-4f);
            attenuation = NLSComputePointAttenuation(light, distanceToLight);
            if (light.type == NLS_LIGHT_TYPE_SPOT)
            {
                const float spotCos = dot(normalize(-light.directionWS), lightDir);
                const float outerCutoffCos = cos(radians(light.outerCutoffDegrees));
                attenuation *= saturate((spotCos - outerCutoffCos) / max(1.0f - outerCutoffCos, 1e-3f));
            }
        }

        const float ndotl = saturate(dot(normalWS, lightDir));
        const float3 halfVector = normalize(lightDir + viewDir);
        const float specularTerm = pow(saturate(dot(normalWS, halfVector)), max(shininess, 1.0f));
        lighting += baseColor * light.color * (ndotl * light.intensity * attenuation);
        lighting += specularColor * light.color * (specularTerm * light.intensity * attenuation);
    }

    return lighting;
}

float3 NLSAccumulateClusteredLightingPBR(
    StructuredBuffer<uint> packedLights,
    StructuredBuffer<uint> clusterRecords,
    StructuredBuffer<uint> compactIndices,
    float3 worldPosition,
    float3 normalWS,
    float3 albedo,
    float metallic,
    float roughness,
    float ao)
{
    const uint clusterIndex = NLSComputeClusterIndexFromWorldPosition(worldPosition);
    const uint recordBase = clusterIndex * 2u;
    const uint offset = clusterRecords[recordBase + 0u];
    const uint count = clusterRecords[recordBase + 1u];
    const float3 viewDir = normalize(NLSGetCameraWorldPosition() - worldPosition);
    float3 lighting = albedo * (NLSGetAmbientFloor() * ao);

    [loop]
    for (uint i = 0u; i < count; ++i)
    {
        const NLSLightGridLight light = NLSLoadLight(packedLights, compactIndices[offset + i]);
        if (light.type == NLS_LIGHT_TYPE_AMBIENT_BOX || light.type == NLS_LIGHT_TYPE_AMBIENT_SPHERE)
        {
            lighting += albedo * light.color * max(light.intensity, 0.0f) * ao;
            continue;
        }

        float3 lightDir = 0.0f.xxx;
        float attenuation = 1.0f;
        if (light.type == NLS_LIGHT_TYPE_DIRECTIONAL)
        {
            lightDir = normalize(-light.directionWS);
        }
        else
        {
            const float3 toLight = light.positionWS - worldPosition;
            const float distanceToLight = length(toLight);
            if (distanceToLight > max(light.range, 0.0001f))
                continue;

            lightDir = toLight / max(distanceToLight, 1e-4f);
            attenuation = NLSComputePointAttenuation(light, distanceToLight);
            if (light.type == NLS_LIGHT_TYPE_SPOT)
            {
                const float spotCos = dot(normalize(-light.directionWS), lightDir);
                const float outerCutoffCos = cos(radians(light.outerCutoffDegrees));
                attenuation *= saturate((spotCos - outerCutoffCos) / max(1.0f - outerCutoffCos, 1e-3f));
            }
        }

        const float ndotl = saturate(dot(normalWS, lightDir));
        const float3 halfVector = normalize(lightDir + viewDir);
        const float specularHint = pow(saturate(dot(normalWS, halfVector)), max(4.0f, (1.0f - roughness) * 64.0f));
        lighting += albedo * light.color * (ndotl * light.intensity * attenuation);
        lighting += light.color * (specularHint * metallic * (1.0f - roughness) * light.intensity * attenuation);
    }

    return lighting;
}

#endif
