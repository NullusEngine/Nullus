#pragma once

#include "VfxProgram.h"

namespace NLS::VFX
{
// Matrices use row-major storage and column-vector multiplication; clip depth is [0,w].
// Simulation-side camera snapshot. The render pass needs view/projection
// matrices plus the basis vectors, while particle scripts only need the camera
// position, orientation, FOV and viewport size. Publishing this before the tick
// lets camera-relative modules run on the CPU simulation path.
struct CameraContext
{
    Value position{};
    Value right{1,0,0,0}, up{0,1,0,0}, forward{0,0,1,0};
    float fovDegrees = 60;
    float viewportWidth = 1920, viewportHeight = 1080;
    bool valid = false;
};

struct SpriteView
{
    std::array<float,16> viewProjection{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    std::array<float,16> localToWorld{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    Value cameraRight{1,0,0,0}, cameraUp{0,1,0,0}, cameraForward{0,0,1,0};
    bool frustumCulling = true;
};

// Centers are in simulation space; Sprite dimensions are world units, as in SpriteVS.
// Nonfinite centers or dimensions stay in simulation but do not produce render geometry.
struct ParticleBounds
{
    Value minimum{}, maximum{};
    float spriteRadius = 0;
    uint32_t particleCount = 0, invalidCount = 0, padding = 0;
    [[nodiscard]] bool Empty() const { return particleCount == invalidCount; }
};
static_assert(sizeof(ParticleBounds) == 48);

struct ParticleViewStatistics
{
    uint32_t particleCount = 0, visibleCount = 0, culledCount = 0, invalidCount = 0;
    uint32_t vertexCount = 6, instanceCount = 0, firstVertex = 0, firstInstance = 0;
};
static_assert(sizeof(ParticleViewStatistics) == 32);

NLS_VFX_API ParticleBounds CalculateParticleBounds(const ParticleDataSet& data);
NLS_VFX_API ParticleBounds TransformParticleBounds(const ParticleBounds& bounds, const std::array<float,16>& localToWorld,
    const Value& cameraRight, const Value& cameraUp);
NLS_VFX_API ParticleViewStatistics CalculateParticleVisibility(const ParticleDataSet& data, const SpriteView& view);
}
