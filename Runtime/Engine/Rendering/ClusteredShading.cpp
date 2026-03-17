#include "Rendering/ClusteredShading.h"

#include <algorithm>
#include <cmath>

namespace NLS::Engine::Rendering
{
    namespace
    {
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

    ClusteredLightGrid BuildClusteredLightGrid(
        const ClusteredShadingSettings& settings,
        const std::vector<std::reference_wrapper<const NLS::Render::Entities::Light>>& lights,
        const NLS::Render::Entities::Camera&,
        const Maths::Matrix4& view,
        const Maths::Matrix4& projection,
        uint32_t,
        uint32_t,
        float nearPlane,
        float farPlane)
    {
        ClusteredLightGrid grid;
        grid.settings = settings;
        grid.bounds.resize(grid.GetClusterCount());
        grid.records.resize(grid.GetClusterCount());

        std::vector<std::vector<uint32_t>> perClusterLights(grid.GetClusterCount());

        for (uint32_t lightIndex = 0; lightIndex < lights.size(); ++lightIndex)
        {
            const auto range = CalculateLightRange(settings, lights[lightIndex].get(), view, projection, nearPlane, farPlane);
            if (range.minX > range.maxX || range.minY > range.maxY || range.minZ > range.maxZ)
                continue;

            for (uint32_t z = range.minZ; z <= range.maxZ; ++z)
            {
                for (uint32_t y = range.minY; y <= range.maxY; ++y)
                {
                    for (uint32_t x = range.minX; x <= range.maxX; ++x)
                    {
                        auto& clusterLights = perClusterLights[GetClusterIndex(settings, x, y, z)];
                        if (clusterLights.size() < settings.maxLightsPerCluster)
                            clusterLights.push_back(lightIndex);
                    }
                }
            }
        }

        uint32_t runningOffset = 0;
        for (uint32_t z = 0; z < settings.gridSizeZ; ++z)
        {
            const float z0 = nearPlane + (farPlane - nearPlane) * (static_cast<float>(z) / static_cast<float>(settings.gridSizeZ));
            const float z1 = nearPlane + (farPlane - nearPlane) * (static_cast<float>(z + 1) / static_cast<float>(settings.gridSizeZ));

            for (uint32_t y = 0; y < settings.gridSizeY; ++y)
            {
                for (uint32_t x = 0; x < settings.gridSizeX; ++x)
                {
                    const uint32_t clusterIndex = GetClusterIndex(settings, x, y, z);
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
