#pragma once

#include "Assets/AssetMeta.h"
#include "Assets/ArtifactWriter.h"
#include <array>
#include <optional>
#include <span>

namespace NLS::Core::Assets
{
inline constexpr uint32_t SpriteArtifactVersion = 1;
// The native envelope stays at v1. Bordered payloads use v2; unbordered
// payloads retain their original v1 bytes and remain readable by older players.
inline constexpr uint32_t SpriteArtifactPayloadVersion = 2;

// Authoring coordinates are pixels from the source image's top-left corner.
// The logical source dimensions survive target texture resizing and repacking.
struct SpriteArtifact
{
    std::string id;
    std::string name;
    AssetId textureAssetId;
    std::string textureSubAssetKey;
    uint32_t sourceWidth = 0, sourceHeight = 0;
    uint32_t x = 0, y = 0, width = 0, height = 0;
    std::array<float, 2> pivot {0.5f, 0.5f};
    float pixelsPerUnit = 100.0f;
    // Pixel widths in Left, Bottom, Right, Top order.
    std::array<uint32_t,4> border{};

    NLS_CORE_API bool Validate(std::string& error) const;
    NLS_CORE_API std::array<float, 4> LocalRect() const;
    NLS_CORE_API std::array<float, 4> UvRect() const;
    NLS_CORE_API std::string SubAssetKey() const;
};

NLS_CORE_API bool IsSpriteTexture(const AssetMeta& meta);
NLS_CORE_API std::optional<SpriteArtifact> BuildSingleSpriteArtifact(
    const AssetMeta& meta, uint32_t sourceWidth, uint32_t sourceHeight,
    std::string textureSubAssetKey, std::string name, std::string& error);
NLS_CORE_API std::vector<uint8_t> SerializeSpriteArtifact(const SpriteArtifact& sprite);
NLS_CORE_API std::optional<SpriteArtifact> DeserializeSpriteArtifact(
    std::span<const uint8_t> bytes, std::string& error);
// Adds a sibling payload, leaving the primary Texture and its identity intact.
NLS_CORE_API bool AppendSingleSpriteArtifact(ArtifactWriteRequest& request,
    const AssetMeta& meta, uint32_t sourceWidth, uint32_t sourceHeight,
    const std::string& textureSubAssetKey, const std::string& name, std::string& error);
}
