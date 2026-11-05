#pragma once

#include "SpriteArtifact.h"
#include <vector>

namespace NLS::Core::Assets
{
struct SpritePatch
{
    std::array<float,4> localRect{};
    std::array<float,4> uvRect{};
    bool repeatX=false,repeatY=false;
};
struct SpritePatchGeometry
{
    std::array<SpritePatch,9> patches{};
    std::array<float,4> bounds{};
    uint32_t count=0;
};
// Keeps corner widths in world units (border pixels / PPU). When the requested
// size is smaller than both borders, shrink them proportionally without overlap.
NLS_CORE_API std::optional<SpritePatchGeometry> BuildSlicedSpriteGeometry(
    const SpriteArtifact& sprite,float width,float height,std::string& error);
struct SpriteTiledGeometry
{
    std::vector<SpritePatch> patches;
    std::array<float,4> bounds{};
};
inline constexpr uint32_t MaxTiledSpritePatches=65536;
// Repeats centers/edge strips at native PPU size and clips the final tile's UVs.
NLS_CORE_API std::optional<SpriteTiledGeometry> BuildTiledSpriteGeometry(
    const SpriteArtifact& sprite,float width,float height,std::string& error);
}
