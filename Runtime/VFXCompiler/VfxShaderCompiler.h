#pragma once

#include <atomic>
#include <filesystem>
#include <span>
#include "VfxCompiler.h"

namespace NLS::VFX
{
struct PlatformCompileCacheStats
{
    uint64_t hits = 0, misses = 0, shaderCompilations = 0;
};
// Produces platform bytecode for simulation and Sprite rendering. Existing programs survive
// failures and cancellation. The runtime consumes this package without graph/shader compilation.
NLS_VFX_COMPILER_API bool CompilePlatformPrograms(CompiledSystem& system,
    std::span<const Render::RHI::NativeBackendType> backends, const std::filesystem::path& cacheRoot,
    std::vector<Diagnostic>& diagnostics, const std::atomic_bool* cancelled = nullptr,PlatformCompileCacheStats* cacheStats = nullptr);
}
