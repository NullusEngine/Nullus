#pragma once
#include <VFX/Authoring/VfxAuthoring.h>
#include <VFX/Authoring/VfxLibrary.h>
#include "VfxCompiler.h"

namespace NLS::VFX
{
class CompileCache;
}
namespace NLS::VFX::Authoring
{
// Lowers ordered module stacks to the common CPU/GPU expression IR. Attribute
// reads see prior module writes; only final values become terminal graph writes.
NLS_VFX_COMPILER_API bool Compile(const System& source, CompiledSystem& output, std::vector<Issue>& issues);
NLS_VFX_COMPILER_API bool Compile(const System& source, const SourceLibrary& library, CompiledSystem& output,
    std::vector<SourceDependency>& dependencies, std::vector<Issue>& issues, System* resolved = nullptr,
    NLS::VFX::CompileCache* cache = nullptr);
// Captures a code-defined module's lowering as a reusable module graph. The
// graph reads Module.* parameters and writes particle attributes; emitter
// policy modules (spawning, events, collision settings) have no graph form and
// return false.
NLS_VFX_COMPILER_API bool BuildModuleGraph(const Module& module, Graph& graph, std::string& error,
    Stage stage = Stage::ParticleUpdate);
}
