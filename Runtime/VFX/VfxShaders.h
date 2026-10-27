#pragma once

#include <Rendering/RHI/Core/RHIPipeline.h>

namespace NLS::VFX
{
inline constexpr uint32_t kVfxShaderLayoutVersion = 12;

struct GpuBoundsShaders
{
    Render::RHI::RHIShaderModuleDesc reduce, finish;
};
struct GpuVisibilityShaders
{
    Render::RHI::RHIShaderModuleDesc cull, scan, scatter;
};
struct GpuEmitterShaders
{
    Render::RHI::RHIShaderModuleDesc beginSpawn, spawn, update, advanceSpawn, compact, finish, reset;
    Render::RHI::RHIShaderModuleDesc resetEvents, receiveEvents;
    GpuBoundsShaders bounds;
    std::vector<Render::RHI::RHIShaderModuleDesc> stages;
};

struct GpuSortShaders
{
    Render::RHI::RHIShaderModuleDesc initialize, step;
};

struct EmitterShaderPrograms
{
    GpuEmitterShaders simulation;
    Render::RHI::RHIShaderModuleDesc spriteVertex, spriteFragment;
    GpuSortShaders sort;
    GpuVisibilityShaders visibility;
};

struct PlatformShaderPrograms
{
    Render::RHI::NativeBackendType backend = Render::RHI::NativeBackendType::None;
    std::vector<EmitterShaderPrograms> emitters;
};
}
