#include "LightGridCommon.hlsli"

RWStructuredBuffer<uint> u_LightGridStartOffsetGrid : register(u1, space1);
RWStructuredBuffer<uint> u_LightGridCulledLightLinks : register(u2, space1);
RWStructuredBuffer<uint> u_LightGridLinkCounter : register(u3, space1);
RWStructuredBuffer<uint> u_LightGridCompactCounter : register(u4, space1);
RWStructuredBuffer<uint> u_LightGridClusterRecords : register(u5, space1);
RWStructuredBuffer<uint> u_LightGridCompactIndices : register(u6, space1);

static const uint NLS_LIGHT_LINK_STRIDE = 2u;

[numthreads(4, 4, 4)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint3 gridCoordinate = dispatchThreadId;
    if (any(gridCoordinate >= uint3(NLSGetGridSizeX(), NLSGetGridSizeY(), NLSGetGridSizeZ())))
        return;

    const uint clusterIndex = NLSGetClusterIndex(gridCoordinate.x, gridCoordinate.y, gridCoordinate.z);
    const uint maxLightsPerCluster = NLSGetMaxLightsPerCluster();

    u_LightGridStartOffsetGrid[clusterIndex] = 0xFFFFFFFFu;
    u_LightGridClusterRecords[clusterIndex * 2u + 0u] = 0u;
    u_LightGridClusterRecords[clusterIndex * 2u + 1u] = 0u;

    [loop]
    for (uint lightSlot = 0u; lightSlot < maxLightsPerCluster; ++lightSlot)
    {
        const uint compactIndex = clusterIndex * maxLightsPerCluster + lightSlot;
        const uint linkBase = compactIndex * NLS_LIGHT_LINK_STRIDE;
        u_LightGridCompactIndices[compactIndex] = 0u;
        u_LightGridCulledLightLinks[linkBase + 0u] = 0u;
        u_LightGridCulledLightLinks[linkBase + 1u] = 0xFFFFFFFFu;
    }

    if (clusterIndex == 0u)
    {
        u_LightGridLinkCounter[0] = 0u;
        u_LightGridCompactCounter[0] = 0u;
    }
}
