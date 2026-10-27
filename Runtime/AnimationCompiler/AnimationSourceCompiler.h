#pragma once
#include "AnimationCompiler.h"
#include <Assets/ArtifactDatabase.h>
#include <Assets/ArtifactWriter.h>

namespace NLS::AnimationCompiler
{
struct SourceCompileResult
{
    Core::Assets::ArtifactPayload payload;
    std::shared_ptr<const Animation::AnimationProgram> program;
    std::shared_ptr<const Animation::Model> model;
    std::vector<Core::Assets::AssetDependencyRecord> dependencies;
};
// References use <source GUID>#<sub-asset key>. Path references are accepted
// while authoring and resolved through .meta before producing runtime bytes.
NLS_ANIMATION_COMPILER_API bool CompileSource(const std::filesystem::path& projectRoot,
    const Core::Assets::ArtifactDatabase& database,std::string_view target,
    const std::filesystem::path& path,std::string_view source,SourceCompileResult& result,std::string& error);
}
