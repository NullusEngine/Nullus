#include "LightGridCommon.hlsli"

StructuredBuffer<uint> u_ForwardLocalLightBuffer : register(t0, space1);
RWStructuredBuffer<uint> u_LightGridStartOffsetGrid : register(u1, space1);
RWStructuredBuffer<uint> u_LightGridCulledLightLinks : register(u2, space1);
RWStructuredBuffer<uint> u_LightGridLinkCounter : register(u3, space1);

[numthreads(4, 4, 4)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint3 gridCoordinate = dispatchThreadId;
    if (any(gridCoordinate >= uint3(NLSGetGridSizeX(), NLSGetGridSizeY(), NLSGetGridSizeZ())))
        return;

    const uint clusterIndex = NLSGetClusterIndex(gridCoordinate.x, gridCoordinate.y, gridCoordinate.z);
    float3 viewTileMin;
    float3 viewTileMax;
    NLSComputeCellViewAABB(gridCoordinate, viewTileMin, viewTileMax);

    const float3 viewTileCenter = 0.5f * (viewTileMin + viewTileMax);
    const float3 viewTileExtent = viewTileMax - viewTileCenter;

    [loop]
    for (uint lightIndex = 0u; lightIndex < NLSGetSceneLightCount(); ++lightIndex)
    {
        const NLSLightGridLight light = NLSLoadLight(u_ForwardLocalLightBuffer, lightIndex);
        bool intersectsCell = NLSIsGlobalLight(light);
        if (!intersectsCell)
        {
            const float3 viewSpaceLightPosition = mul(u_LightGridView, float4(light.positionWS, 1.0f)).xyz;
            const float lightRadius = max(light.range, 0.0f);
            const float boxDistanceSq =
                NLSComputeSquaredDistanceFromBoxToPoint(viewTileCenter, viewTileExtent, viewSpaceLightPosition);
            intersectsCell = boxDistanceSq < lightRadius * lightRadius;

            if (intersectsCell && light.type == NLS_LIGHT_TYPE_SPOT)
            {
                const float3 viewSpaceLightDirection =
                    normalize(mul((float3x3)u_LightGridView, light.directionWS));
                const float tanConeAngle = tan(radians(light.outerCutoffDegrees));
                if (tanConeAngle > 0.0f)
                {
                    intersectsCell =
                        !NLSIsAabbOutsideInfiniteAcuteConeApprox(
                            viewSpaceLightPosition,
                            -viewSpaceLightDirection,
                            tanConeAngle,
                            viewTileCenter,
                            viewTileExtent);
                }
            }
        }

        if (intersectsCell)
        {
            uint nextLink = 0u;
            const uint maxAvailableLinks =
                NLSGetGridSizeX() * NLSGetGridSizeY() * NLSGetGridSizeZ() * NLSGetMaxLightsPerCluster();
            InterlockedAdd(u_LightGridLinkCounter[0], 1u, nextLink);
            if (nextLink < maxAvailableLinks)
            {
                uint previousLink = 0xFFFFFFFFu;
                InterlockedExchange(u_LightGridStartOffsetGrid[clusterIndex], nextLink, previousLink);
                u_LightGridCulledLightLinks[nextLink * NLS_LIGHT_LINK_STRIDE + 0u] = lightIndex;
                u_LightGridCulledLightLinks[nextLink * NLS_LIGHT_LINK_STRIDE + 1u] = previousLink;
            }
        }
    }
}
