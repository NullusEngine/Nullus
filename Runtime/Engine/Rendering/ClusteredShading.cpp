#include "Rendering/ClusteredShading.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace NLS::Engine::Rendering
{
    namespace
    {
        constexpr uint32_t kLightGridInjectionGroupSize = 4u;
        constexpr uint32_t kNumCulledLightsGridStride = 2u;
        constexpr uint32_t kLightLinkStride = 2u;

        struct ClusterRange
        {
            uint32_t minX;
            uint32_t maxX;
            uint32_t minY;
            uint32_t maxY;
            uint32_t minZ;
            uint32_t maxZ;
        };

        inline uint32_t GetClusterIndex(const ClusteredShadingSettings& settings, uint32_t x, uint32_t y, uint32_t z)
        {
            return x + y * settings.gridSizeX + z * settings.gridSizeX * settings.gridSizeY;
        }

        inline float Clamp01(float value)
        {
            return std::max(0.0f, std::min(1.0f, value));
        }

        inline float ViewDepthToSlice(float depth, float nearPlane, float farPlane, uint32_t gridSizeZ)
        {
            if (farPlane <= nearPlane || gridSizeZ == 0)
                return 0.0f;

            return Clamp01((depth - nearPlane) / (farPlane - nearPlane)) * static_cast<float>(gridSizeZ - 1);
        }

        bool TryMultiply(uint64_t lhs, uint64_t rhs, uint64_t& out)
        {
            if (lhs != 0u && rhs > (std::numeric_limits<uint64_t>::max)() / lhs)
                return false;
            out = lhs * rhs;
            return true;
        }

        bool IsWithinElementBudget(uint64_t value, uint64_t maxElementCount)
        {
            return maxElementCount == 0u || value <= maxElementCount;
        }

        inline bool IsGlobalLight(const NLS::Render::Entities::Light& light)
        {
            using LightType = NLS::Render::Settings::ELightType;
            return light.type == LightType::DIRECTIONAL || light.type == LightType::AMBIENT_BOX;
        }

        inline Maths::Vector4 TransformPoint(const Maths::Matrix4& matrix, const Maths::Vector3& point)
        {
            return matrix * Maths::Vector4(point.x, point.y, point.z, 1.0f);
        }

        ClusterRange CalculateLightRange(
            const ClusteredShadingSettings& settings,
            const NLS::Render::Entities::Light& light,
            const Maths::Matrix4& view,
            const Maths::Matrix4& projection,
            float nearPlane,
            float farPlane)
        {
            if (IsGlobalLight(light))
            {
                return { 0, settings.gridSizeX - 1, 0, settings.gridSizeY - 1, 0, settings.gridSizeZ - 1 };
            }

            const auto radius = light.GetEffectRange();
            if (!std::isfinite(radius) || radius <= 0.0f)
            {
                return { 0, settings.gridSizeX - 1, 0, settings.gridSizeY - 1, 0, settings.gridSizeZ - 1 };
            }

            const auto viewPosition4 = TransformPoint(view, light.transform->GetWorldPosition());
            const Maths::Vector3 viewPosition(viewPosition4.x, viewPosition4.y, viewPosition4.z);
            const float depth = -viewPosition.z;

            if (depth + radius < nearPlane || depth - radius > farPlane)
            {
                return { 1, 0, 1, 0, 1, 0 };
            }

            const auto clip = projection * Maths::Vector4(viewPosition.x, viewPosition.y, viewPosition.z, 1.0f);
            if (std::abs(clip.w) <= 1e-5f)
            {
                return { 0, settings.gridSizeX - 1, 0, settings.gridSizeY - 1, 0, settings.gridSizeZ - 1 };
            }

            const float centerNdcX = clip.x / clip.w;
            const float centerNdcY = clip.y / clip.w;
            const float projX = projection.data[0];
            const float projY = projection.data[5];
            const float safeDepth = std::max(depth, nearPlane);
            const float radiusNdcX = std::abs(projX) * radius / safeDepth;
            const float radiusNdcY = std::abs(projY) * radius / safeDepth;

            const float minNdcX = std::max(-1.0f, centerNdcX - radiusNdcX);
            const float maxNdcX = std::min(1.0f, centerNdcX + radiusNdcX);
            const float minNdcY = std::max(-1.0f, centerNdcY - radiusNdcY);
            const float maxNdcY = std::min(1.0f, centerNdcY + radiusNdcY);

            const auto toClusterX = [&](float ndc) -> uint32_t
            {
                const float normalized = Clamp01(ndc * 0.5f + 0.5f);
                return std::min(settings.gridSizeX - 1, static_cast<uint32_t>(normalized * static_cast<float>(settings.gridSizeX)));
            };
            const auto toClusterY = [&](float ndc) -> uint32_t
            {
                const float normalized = Clamp01(ndc * 0.5f + 0.5f);
                return std::min(settings.gridSizeY - 1, static_cast<uint32_t>(normalized * static_cast<float>(settings.gridSizeY)));
            };

            const float minDepth = std::max(nearPlane, depth - radius);
            const float maxDepth = std::min(farPlane, depth + radius);
            const uint32_t minZ = std::min(settings.gridSizeZ - 1, static_cast<uint32_t>(ViewDepthToSlice(minDepth, nearPlane, farPlane, settings.gridSizeZ)));
            const uint32_t maxZ = std::min(settings.gridSizeZ - 1, static_cast<uint32_t>(std::ceil(ViewDepthToSlice(maxDepth, nearPlane, farPlane, settings.gridSizeZ))));

            return {
                toClusterX(minNdcX),
                toClusterX(maxNdcX),
                toClusterY(minNdcY),
                toClusterY(maxNdcY),
                minZ,
                maxZ
            };
        }
    } // namespace

    uint32_t GetLightGridInjectionGroupSize()
    {
        return kLightGridInjectionGroupSize;
    }

    uint32_t GetNumCulledLightsGridStride()
    {
        return kNumCulledLightsGridStride;
    }

    uint32_t GetLightLinkStride()
    {
        return kLightLinkStride;
    }

    LightGridDimensions CalculateLightGridDimensions(
        const ClusteredShadingSettings& settings,
        const uint32_t renderWidth,
        const uint32_t renderHeight)
    {
        const uint32_t pixelSize = std::max(1u, settings.lightGridPixelSize);
        return {
            std::max(1u, (renderWidth + pixelSize - 1u) / pixelSize),
            std::max(1u, (renderHeight + pixelSize - 1u) / pixelSize),
            std::max(1u, settings.gridSizeZ)
        };
    }

    bool TryCalculateLightGridBufferElementCounts(
        const ClusteredShadingSettings& settings,
        const LightGridDimensions& dimensions,
        const uint32_t lightLinkStride,
        const uint32_t numCulledLightsGridStride,
        const uint64_t maxElementCount,
        uint64_t& outClusterCount,
        uint64_t& outCulledLightLinksCount,
        uint64_t& outNumCulledLightsGridCount,
        uint64_t& outCulledLightDataGridCount)
    {
        outClusterCount = 0u;
        outCulledLightLinksCount = 0u;
        outNumCulledLightsGridCount = 0u;
        outCulledLightDataGridCount = 0u;

        uint64_t xy = 0u;
        uint64_t clusterCount = 0u;
        if (!TryMultiply(dimensions.x, dimensions.y, xy) ||
            !TryMultiply(xy, dimensions.z, clusterCount) ||
            !IsWithinElementBudget(clusterCount, maxElementCount))
        {
            return false;
        }

        uint64_t culledLightLinksCount = 0u;
        uint64_t culledLightDataGridCount = 0u;
        uint64_t numCulledLightsGridCount = 0u;
        if (!TryMultiply(clusterCount, settings.maxLightsPerCluster, culledLightDataGridCount) ||
            !TryMultiply(culledLightDataGridCount, lightLinkStride, culledLightLinksCount) ||
            !TryMultiply(clusterCount, numCulledLightsGridStride, numCulledLightsGridCount) ||
            !IsWithinElementBudget(culledLightLinksCount, maxElementCount) ||
            !IsWithinElementBudget(numCulledLightsGridCount, maxElementCount) ||
            !IsWithinElementBudget(culledLightDataGridCount, maxElementCount))
        {
            return false;
        }

        outClusterCount = clusterCount;
        outCulledLightLinksCount = culledLightLinksCount;
        outNumCulledLightsGridCount = numCulledLightsGridCount;
        outCulledLightDataGridCount = culledLightDataGridCount;
        return true;
    }

    LightGridDimensions CalculateLightGridDispatchGroups(
        const LightGridDimensions& gridDimensions)
    {
        const auto groupSize = GetLightGridInjectionGroupSize();
        return {
            (gridDimensions.x + groupSize - 1u) / groupSize,
            (gridDimensions.y + groupSize - 1u) / groupSize,
            (gridDimensions.z + groupSize - 1u) / groupSize
        };
    }

    Maths::Vector3 CalculateLightGridZParams(
        const float nearPlane,
        const float farPlane,
        const uint32_t gridSizeZ)
    {
        const double nearOffset = 0.095 * 100.0;
        const double scale = 4.05;
        const double nearValue = static_cast<double>(nearPlane) + nearOffset;
        const double farValue = std::max(
            static_cast<double>(farPlane),
            nearValue + 1.0);
        const double sliceCount = std::max(1u, gridSizeZ);
        const double offset =
            (farValue - nearValue * std::exp2((sliceCount - 1.0) / scale)) /
            (farValue - nearValue);
        const double bias = (1.0 - offset) / nearValue;
        return {
            static_cast<float>(bias),
            static_cast<float>(offset),
            static_cast<float>(scale)
        };
    }

    ClusteredLightGrid BuildClusteredLightGrid(
        const ClusteredShadingSettings& settings,
        const std::vector<std::reference_wrapper<const NLS::Render::Entities::Light>>& lights,
        const NLS::Render::Entities::Camera&,
        const Maths::Matrix4& view,
        const Maths::Matrix4& projection,
        uint32_t renderWidth,
        uint32_t renderHeight,
        float nearPlane,
        float farPlane)
    {
        ClusteredLightGrid grid;
        grid.settings = settings;
        const auto dimensions = CalculateLightGridDimensions(settings, renderWidth, renderHeight);
        grid.settings.gridSizeX = dimensions.x;
        grid.settings.gridSizeY = dimensions.y;
        grid.settings.gridSizeZ = dimensions.z;
        const auto& effectiveSettings = grid.settings;
        grid.bounds.resize(grid.GetClusterCount());
        grid.records.resize(grid.GetClusterCount());

        std::vector<std::vector<uint32_t>> perClusterLights(grid.GetClusterCount());

        for (uint32_t lightIndex = 0; lightIndex < lights.size(); ++lightIndex)
        {
            const auto range = CalculateLightRange(effectiveSettings, lights[lightIndex].get(), view, projection, nearPlane, farPlane);
            if (range.minX > range.maxX || range.minY > range.maxY || range.minZ > range.maxZ)
                continue;

            for (uint32_t z = range.minZ; z <= range.maxZ; ++z)
            {
                for (uint32_t y = range.minY; y <= range.maxY; ++y)
                {
                    for (uint32_t x = range.minX; x <= range.maxX; ++x)
                    {
                        auto& clusterLights = perClusterLights[GetClusterIndex(effectiveSettings, x, y, z)];
                        if (clusterLights.size() < effectiveSettings.maxLightsPerCluster)
                            clusterLights.push_back(lightIndex);
                    }
                }
            }
        }

        uint32_t runningOffset = 0;
        for (uint32_t z = 0; z < effectiveSettings.gridSizeZ; ++z)
        {
            const float z0 = nearPlane + (farPlane - nearPlane) * (static_cast<float>(z) / static_cast<float>(effectiveSettings.gridSizeZ));
            const float z1 = nearPlane + (farPlane - nearPlane) * (static_cast<float>(z + 1) / static_cast<float>(effectiveSettings.gridSizeZ));

            for (uint32_t y = 0; y < effectiveSettings.gridSizeY; ++y)
            {
                for (uint32_t x = 0; x < effectiveSettings.gridSizeX; ++x)
                {
                    const uint32_t clusterIndex = GetClusterIndex(effectiveSettings, x, y, z);
                    auto& bounds = grid.bounds[clusterIndex];
                    bounds.minPoint = { static_cast<float>(x), static_cast<float>(y), z0, 1.0f };
                    bounds.maxPoint = { static_cast<float>(x + 1), static_cast<float>(y + 1), z1, 1.0f };

                    auto& record = grid.records[clusterIndex];
                    record.offset = runningOffset;
                    record.count = static_cast<uint32_t>(perClusterLights[clusterIndex].size());
                    for (const auto lightIndex : perClusterLights[clusterIndex])
                        grid.lightIndices.push_back(lightIndex);
                    runningOffset += record.count;
                }
            }
        }

        return grid;
    }
}
