#include "LightGridCommon.hlsli"

StructuredBuffer<uint> u_LightGridLights : register(t0, space1);
RWStructuredBuffer<uint> u_LightGridClusterLightCounts : register(u1, space1);
RWStructuredBuffer<uint> u_LightGridClusterScratchIndices : register(u2, space1);

[numthreads(64, 1, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint lightIndex = dispatchThreadId.x;
    if (lightIndex >= NLSGetSceneLightCount())
        return;

    const NLSLightGridLight light = NLSLoadLight(u_LightGridLights, lightIndex);
    uint3 minRange;
    uint3 maxRange;
    if (!NLSComputeLightClusterRange(light, minRange, maxRange))
        return;

    [loop]
    for (uint z = minRange.z; z <= maxRange.z; ++z)
    {
        [loop]
        for (uint y = minRange.y; y <= maxRange.y; ++y)
        {
            [loop]
            for (uint x = minRange.x; x <= maxRange.x; ++x)
            {
                const uint clusterIndex = NLSGetClusterIndex(x, y, z);
                uint slot = 0u;
                InterlockedAdd(u_LightGridClusterLightCounts[clusterIndex], 1u, slot);
                if (slot < NLSGetMaxLightsPerCluster())
                    u_LightGridClusterScratchIndices[clusterIndex * NLSGetMaxLightsPerCluster() + slot] = lightIndex;
            }
        }
    }
}
