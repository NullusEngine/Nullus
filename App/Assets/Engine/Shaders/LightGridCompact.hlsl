#include "LightGridCommon.hlsli"

StructuredBuffer<uint> u_LightGridStartOffsetGrid : register(t1, space1);
StructuredBuffer<uint> u_LightGridCulledLightLinks : register(t2, space1);
RWStructuredBuffer<uint> u_LightGridCompactCounter : register(u3, space1);
RWStructuredBuffer<uint> u_NumCulledLightsGrid : register(u4, space1);
RWStructuredBuffer<uint> u_CulledLightDataGrid : register(u5, space1);

[numthreads(4, 4, 4)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint3 gridCoordinate = dispatchThreadId;
    if (any(gridCoordinate >= uint3(NLSGetGridSizeX(), NLSGetGridSizeY(), NLSGetGridSizeZ())))
        return;

    const uint clusterIndex = NLSGetClusterIndex(gridCoordinate.x, gridCoordinate.y, gridCoordinate.z);
    uint linkOffset = u_LightGridStartOffsetGrid[clusterIndex];
    uint count = 0u;
    while (linkOffset != 0xFFFFFFFFu && count < NLSGetSceneLightCount())
    {
        ++count;
        linkOffset = u_LightGridCulledLightLinks[linkOffset * NLS_LIGHT_LINK_STRIDE + 1u];
    }

    uint offset = 0u;
    InterlockedAdd(u_LightGridCompactCounter[0], count, offset);
    u_NumCulledLightsGrid[clusterIndex * NLS_NUM_CULLED_LIGHTS_GRID_STRIDE + 0u] = offset;
    u_NumCulledLightsGrid[clusterIndex * NLS_NUM_CULLED_LIGHTS_GRID_STRIDE + 1u] = count;

    linkOffset = u_LightGridStartOffsetGrid[clusterIndex];
    uint i = 0u;
    while (linkOffset != 0xFFFFFFFFu && i < count)
    {
        u_CulledLightDataGrid[offset + count - i - 1u] =
            u_LightGridCulledLightLinks[linkOffset * NLS_LIGHT_LINK_STRIDE + 0u];
        ++i;
        linkOffset = u_LightGridCulledLightLinks[linkOffset * NLS_LIGHT_LINK_STRIDE + 1u];
    }
}
