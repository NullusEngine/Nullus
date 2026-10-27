#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "Math/Vector3.h"
#include "RenderDef.h"

namespace NLS::Render::DistanceField
{
    inline constexpr uint32_t BrickSize = 8;
    inline constexpr uint32_t BrickIntervals = 7;
    inline constexpr uint32_t MipCount = 3;
    inline constexpr uint32_t OutsideBrick = UINT32_MAX;
    inline constexpr uint32_t InsideBrick = UINT32_MAX - 1u;
    inline constexpr uint32_t BuilderVersion = 1;

    enum class GenerationMode : uint8_t { Inherit, Enabled, Disabled };

    struct DistanceFieldBuildSettings
    {
        GenerationMode mode = GenerationMode::Inherit;
        float resolutionScale = 1.0f;
        bool twoSided = false;
        bool projectEnabled = false;
        uint32_t maxResolution = 128;
        [[nodiscard]] bool Enabled() const { return mode == GenerationMode::Enabled ||
            (mode == GenerationMode::Inherit && projectEnabled); }
    };

    struct DistanceFieldMip
    {
        std::array<uint32_t, 3> grid{};
        float maxDistance = 0;
        std::vector<uint32_t> indirection;
        std::vector<uint8_t> bricks;
        [[nodiscard]] uint32_t BrickCount() const { return static_cast<uint32_t>(bricks.size() / 512u); }
    };

    struct MeshDistanceFieldArtifact
    {
        Maths::Vector3 boundsMin;
        Maths::Vector3 boundsMax;
        std::array<DistanceFieldMip, MipCount> mips;
        std::array<std::string, MipCount> mipPaths;
        std::string buildIdentity;
        bool twoSided = false;
    };

    struct DistanceFieldSample
    {
        float distance = 0;
        Maths::Vector3 gradient;
        float voxelSize = 0;
        bool valid = false;
    };

    NLS_RENDER_API bool ValidateDistanceFieldMip(const DistanceFieldMip& mip, std::string* error = nullptr);
    NLS_RENDER_API DistanceFieldSample SampleMeshDistanceField(const MeshDistanceFieldArtifact& asset,
        const Maths::Vector3& localPosition, uint32_t mip = 0);
}
