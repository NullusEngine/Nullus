#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include <Math/Vector4.h>
#include <Math/Matrix4.h>
#include <Rendering/Entities/Light.h>
#include <Rendering/Entities/Camera.h>
#include "EngineDef.h"

namespace NLS::Engine::Rendering
{
    struct NLS_ENGINE_API ClusteredShadingSettings
    {
        uint32_t lightGridPixelSize = 64;
        uint32_t gridSizeX = 16;
        uint32_t gridSizeY = 9;
        uint32_t gridSizeZ = 32;
        uint32_t maxLightsPerCluster = 32;
        bool linkedListCulling = true;
    };

    struct NLS_ENGINE_API LightGridDimensions
    {
        uint32_t x = 1;
        uint32_t y = 1;
        uint32_t z = 1;
    };

    NLS_ENGINE_API uint32_t GetLightGridInjectionGroupSize();
    NLS_ENGINE_API uint32_t GetNumCulledLightsGridStride();
    NLS_ENGINE_API uint32_t GetLightLinkStride();

    NLS_ENGINE_API LightGridDimensions CalculateLightGridDimensions(
        const ClusteredShadingSettings& settings,
        uint32_t renderWidth,
        uint32_t renderHeight);

    NLS_ENGINE_API LightGridDimensions CalculateLightGridDispatchGroups(
        const LightGridDimensions& gridDimensions);

    NLS_ENGINE_API Maths::Vector3 CalculateLightGridZParams(
        float nearPlane,
        float farPlane,
        uint32_t gridSizeZ);

    struct NLS_ENGINE_API ClusterBounds
    {
        Maths::Vector4 minPoint;
        Maths::Vector4 maxPoint;
    };

    struct NLS_ENGINE_API ClusterRecord
    {
        uint32_t offset = 0;
        uint32_t count = 0;
    };

    struct NLS_ENGINE_API ClusteredLightGrid
    {
        ClusteredShadingSettings settings;
        std::vector<ClusterBounds> bounds;
        std::vector<ClusterRecord> records;
        std::vector<uint32_t> lightIndices;

        [[nodiscard]] uint32_t GetClusterCount() const
        {
            return settings.gridSizeX * settings.gridSizeY * settings.gridSizeZ;
        }
    };

    NLS_ENGINE_API ClusteredLightGrid BuildClusteredLightGrid(
        const ClusteredShadingSettings& settings,
        const std::vector<std::reference_wrapper<const NLS::Render::Entities::Light>>& lights,
        const NLS::Render::Entities::Camera& camera,
        const Maths::Matrix4& view,
        const Maths::Matrix4& projection,
        uint32_t renderWidth,
        uint32_t renderHeight,
        float nearPlane,
        float farPlane);
}
