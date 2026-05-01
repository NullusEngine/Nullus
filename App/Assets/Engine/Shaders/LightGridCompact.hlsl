#include "LightGridCommon.hlsli"

StructuredBuffer<uint> u_LightGridClusterLightCounts : register(t1, space1);
StructuredBuffer<uint> u_LightGridClusterScratchIndices : register(t2, space1);
RWStructuredBuffer<uint> u_LightGridCompactCounter : register(u3, space1);
RWStructuredBuffer<uint> u_LightGridClusterRecords : register(u4, space1);
RWStructuredBuffer<uint> u_LightGridCompactIndices : register(u5, space1);

[numthreads(64, 1, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint clusterCount = NLSGetGridSizeX() * NLSGetGridSizeY() * NLSGetGridSizeZ();
    const uint clusterIndex = dispatchThreadId.x;
    if (clusterIndex >= clusterCount)
        return;

    const uint count = min(u_LightGridClusterLightCounts[clusterIndex], NLSGetMaxLightsPerCluster());
    uint offset = 0u;
    InterlockedAdd(u_LightGridCompactCounter[0], count, offset);
    u_LightGridClusterRecords[clusterIndex * 2u + 0u] = offset;
    u_LightGridClusterRecords[clusterIndex * 2u + 1u] = count;

    [loop]
    for (uint i = 0u; i < count; ++i)
    {
        u_LightGridCompactIndices[offset + i] =
            u_LightGridClusterScratchIndices[clusterIndex * NLSGetMaxLightsPerCluster() + i];
    }
}
