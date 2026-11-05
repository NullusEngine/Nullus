#pragma once

#include "Assets/SpriteArtifact.h"
#include <string_view>

namespace NLS::Core::Assets
{
inline constexpr uint32_t SpriteSliceMetadataVersion = 2;
inline constexpr size_t MaxSpriteSlices = 65536;
inline constexpr const char* SpriteImportModeSetting = "SPRITE_MODE";
inline constexpr const char* SpriteSlicesSetting = "SPRITE_SLICES";
inline constexpr const char* SpriteCompilerDependency = "nullus-sprite-compiler";
inline constexpr const char* SpriteCompilerVersion = "2";
NLS_CORE_API bool HasCurrentSpriteCompilerDependency(const ArtifactManifest& manifest,const AssetMeta& meta);

struct SpriteSliceDefinition
{
    std::string id, name;
    uint32_t x = 0, y = 0, width = 0, height = 0;
    std::array<float,2> pivot{0.5f,0.5f};
    float pixelsPerUnit = 100;
    std::array<uint32_t,4> border{};
};

struct SpriteGridSettings
{
    uint32_t cellWidth = 16, cellHeight = 16;
    uint32_t offsetX = 0, offsetY = 0, paddingX = 0, paddingY = 0;
    bool includePartialCells = false;
    std::string namePrefix = "Sprite";
    std::array<float,2> pivot{0.5f,0.5f};
    float pixelsPerUnit = 100;
};

NLS_CORE_API std::string SerializeSpriteSlices(std::span<const SpriteSliceDefinition> slices, std::string& error);
NLS_CORE_API std::optional<std::vector<SpriteSliceDefinition>> DeserializeSpriteSlices(std::string_view text, std::string& error);
// Matches existing rectangles to preserve their UUID/name. Newly created cells
// receive new UUIDs; array order and names never supply resource identity.
NLS_CORE_API std::optional<std::vector<SpriteSliceDefinition>> GenerateGridSpriteSlices(
    uint32_t sourceWidth, uint32_t sourceHeight, const SpriteGridSettings& settings,
    std::span<const SpriteSliceDefinition> existing, std::string& error);
// RGBA rows use source top-left coordinates. Four-connected opaque regions
// become rectangles; matching rectangles retain their UUID and authoring edits.
NLS_CORE_API std::optional<std::vector<SpriteSliceDefinition>> GenerateAlphaSpriteSlices(
    std::span<const uint8_t> rgba, uint32_t width, uint32_t height,
    uint8_t alphaThreshold, uint32_t minimumOpaquePixels, const SpriteGridSettings& defaults,
    std::span<const SpriteSliceDefinition> existing, std::string& error);
NLS_CORE_API std::optional<std::vector<SpriteArtifact>> BuildSpriteArtifacts(
    const AssetMeta& meta, uint32_t sourceWidth, uint32_t sourceHeight,
    const std::string& textureSubAssetKey, const std::string& name, std::string& error);
// Shared by Editor and Cook. Validates every slice before appending any payload,
// preserving the primary Texture and the caller's request on invalid metadata.
NLS_CORE_API bool AppendSpriteArtifacts(ArtifactWriteRequest& request, const AssetMeta& meta,
    uint32_t sourceWidth, uint32_t sourceHeight, const std::string& textureSubAssetKey,
    const std::string& name, std::string& error);
}
