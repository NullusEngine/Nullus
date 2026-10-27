#pragma once

#include "ParticleBounds.h"
#include <Rendering/RHI/Core/RHIMesh.h>

namespace NLS::VFX
{
// Frozen mesh slot: an immutable LOD chain plus the union local bounds. Only RHI
// handles cross the game/render boundary, so MeshManager may unload or reload its
// resource while an earlier view is still in flight.
struct MeshRenderGeometry
{
    struct Level
    {
        std::shared_ptr<Render::RHI::RHIMesh> mesh;
        // Minimum projected viewport-height fraction for this level.
        float screenSize = 1.0f;
    };
    std::vector<Level> levels;
    Value bounds{}; // union local sphere center xyz, radius w
    uint32_t firstLod = 0;

    [[nodiscard]] uint32_t IndexCount(uint32_t lod) const;
    // requested < 0 selects a LOD from the projected on-screen size; otherwise the
    // level index is clamped to the available range.
    [[nodiscard]] NLS_VFX_API uint32_t ChooseLod(const std::array<float,16>& viewProjection, const std::array<float,16>& localToWorld,
        const Value& cameraUp, int32_t requested) const;
};

// Per-view draw information derived from the captured mesh slots.
struct MeshDrawInfo
{
    std::vector<Value> bounds;
    std::vector<uint32_t> indexCounts;
    [[nodiscard]] bool Empty() const { return bounds.empty(); }
};

NLS_VFX_API ParticleBounds CalculateMeshParticleBounds(const ParticleDataSet& data, const MeshSettings& settings, const std::vector<Value>& slotBounds);
NLS_VFX_API ParticleViewStatistics CalculateMeshParticleVisibility(const ParticleDataSet& data, const MeshSettings& settings,
    const std::vector<Value>& slotBounds, const SpriteView& view, const std::vector<uint32_t>& indexCounts);
}
