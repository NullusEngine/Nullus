#include "LightGridCommon.hlsli"

RWStructuredBuffer<uint> u_LightGridStartOffsetGrid : register(u1, space1);
RWStructuredBuffer<uint> u_LightGridCulledLightLinks : register(u2, space1);
RWStructuredBuffer<uint> u_LightGridLinkCounter : register(u3, space1);
RWStructuredBuffer<uint> u_LightGridCompactCounter : register(u4, space1);
RWStructuredBuffer<uint> u_NumCulledLightsGrid : register(u5, space1);
RWStructuredBuffer<uint> u_CulledLightDataGrid : register(u6, space1);

[numthreads(4, 4, 4)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint3 gridCoordinate = dispatchThreadId;
    if (any(gridCoordinate >= uint3(NLSGetGridSizeX(), NLSGetGridSizeY(), NLSGetGridSizeZ())))
        return;

    const uint clusterIndex = NLSGetClusterIndex(gridCoordinate.x, gridCoordinate.y, gridCoordinate.z);
    const uint maxLightsPerCluster = NLSGetMaxLightsPerCluster();

    u_LightGridStartOffsetGrid[clusterIndex] = 0xFFFFFFFFu;
    u_NumCulledLightsGrid[clusterIndex * NLS_NUM_CULLED_LIGHTS_GRID_STRIDE + 0u] = 0u;
    u_NumCulledLightsGrid[clusterIndex * NLS_NUM_CULLED_LIGHTS_GRID_STRIDE + 1u] = 0u;

    [loop]
    for (uint lightSlot = 0u; lightSlot < maxLightsPerCluster; ++lightSlot)
    {
        const uint compactIndex = clusterIndex * maxLightsPerCluster + lightSlot;
        const uint linkBase = compactIndex * NLS_LIGHT_LINK_STRIDE;
        u_CulledLightDataGrid[compactIndex] = 0u;
        u_LightGridCulledLightLinks[linkBase + 0u] = 0u;
        u_LightGridCulledLightLinks[linkBase + 1u] = 0xFFFFFFFFu;
    }

    if (clusterIndex == 0u)
    {
        u_LightGridLinkCounter[0] = 0u;
        u_LightGridCompactCounter[0] = 0u;
    }
}
