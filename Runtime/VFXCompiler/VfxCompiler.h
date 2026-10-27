#pragma once

#include <string>
#include <vector>

#include <VFX/VfxProgram.h>
#include <VFX/CompiledSystem.h>
#include "CommonDef.h"

#if defined(NLS_SHARED_LIB)
#if defined(NLS_VFXCOMPILER_EXPORT)
#define NLS_VFX_COMPILER_API DLLEXPORT
#else
#define NLS_VFX_COMPILER_API DLLIMPORT
#endif
#else
#define NLS_VFX_COMPILER_API
#endif

namespace NLS::VFX
{
struct CompiledGraph
{
    Program program;
    std::string hlsl;
    std::vector<std::string> parameters;
    std::vector<Value> parameterDefaults;
};

struct CompileResult
{
    CompiledGraph compiled;
    std::vector<Diagnostic> diagnostics;
    [[nodiscard]] bool Succeeded() const { return diagnostics.empty(); }
};

NLS_VFX_COMPILER_API CompileResult CompileGraph(const Graph& graph, const std::vector<Attribute>& attributes);
NLS_VFX_COMPILER_API bool CompileSystem(const SystemAsset& asset, CompiledSystem& output, std::vector<Diagnostic>& diagnostics);
NLS_VFX_COMPILER_API bool BuildGpuCompactionShader(const std::vector<Attribute>& attributes, std::string& source, std::string& error);
NLS_VFX_COMPILER_API bool BuildSpriteShader(const std::vector<Attribute>& attributes, std::string& source, std::string& error, const SpriteSettings& settings = {}, const MeshSettings* mesh = nullptr);
NLS_VFX_COMPILER_API bool BuildMeshShader(const std::vector<Attribute>& attributes, std::string& source, std::string& error, const MeshSettings& mesh = {}, const SpriteSettings& material = {});
// Ribbon strip shader: two vertices per particle connected into a camera-facing
// strip whose width comes from a scalar attribute.
NLS_VFX_COMPILER_API bool BuildRibbonShader(const std::vector<Attribute>& attributes, std::string& source, std::string& error, const RibbonSettings& settings = {}, const SpriteSettings& material = {});
NLS_VFX_COMPILER_API bool BuildGpuSortShader(const std::vector<Attribute>& attributes, std::string& source, std::string& error);
NLS_VFX_COMPILER_API bool BuildGpuBoundsShader(const std::vector<Attribute>& attributes, std::string& source, std::string& error, const MeshSettings* mesh = nullptr);
NLS_VFX_COMPILER_API bool BuildGpuVisibilityShader(const std::vector<Attribute>& attributes, std::string& source, std::string& error, const MeshSettings* mesh = nullptr);
}
