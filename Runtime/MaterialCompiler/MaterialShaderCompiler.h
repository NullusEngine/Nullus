#pragma once
#include <filesystem>
#include "Rendering/Assets/ShaderArtifact.h"
#include "Rendering/ShaderLab/ShaderLabVariant.h"
#include "MaterialDependencySnapshot.h"

namespace NLS::MaterialCompiler
{
struct MaterialShaderCompileOptions
{
    std::filesystem::path sourcePath;
    std::filesystem::path generatedPath;
    std::string sourceIdentity;
    std::string subAssetKey;
    std::string cacheDatabasePath;
    std::vector<std::string> includeDirectories;
    std::vector<Render::ShaderCompiler::ShaderTargetPlatform> targets;
    std::vector<Render::ShaderLab::ShaderLabKeywordSet> variants;
    size_t variantBudget = 1024;
    bool generateGlsl = true;
    std::shared_ptr<const MaterialDependencySnapshot> dependencies;
    std::map<std::string,MaterialDependencySource> sourceOverrides;
    std::shared_ptr<const MaterialDependencySnapshot>* capturedDependencies = nullptr;
};

// Shared ShaderLab/graph backend. The frontends supply pass source and logical
// schema; this layer compiles only requested variants and reflects each target.
Render::Assets::ShaderArtifact CompileMaterialPass(const Render::ShaderLab::ShaderLabPassDesc& pass,
    const MaterialShaderCompileOptions& options, const Render::Resources::MaterialParameterSchema& schema = {});
Render::Resources::MaterialParameterSchema BuildMaterialParameterSchema(
    const Render::ShaderLab::ShaderLabAssetDesc& shader, std::string_view sourceIdentity);
}
