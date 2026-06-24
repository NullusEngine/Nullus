#pragma once

#include "Rendering/RenderDef.h"
#include "Rendering/Resources/ShaderReflection.h"
#include "Rendering/ShaderCompiler/ShaderCompilationTypes.h"
#include "Rendering/ShaderLab/ShaderLabTypes.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace NLS::Render::Assets
{
struct ShaderArtifactStage
{
    ShaderCompiler::ShaderStage stage = ShaderCompiler::ShaderStage::Vertex;
    ShaderCompiler::ShaderTargetPlatform targetPlatform = ShaderCompiler::ShaderTargetPlatform::Unknown;
    std::string entryPoint;
    std::string targetProfile;
    ShaderCompiler::ShaderCompilationOutput output;
    uint64_t keywordHash = 0u;
};

struct ShaderArtifact
{
    std::string sourcePath;
    std::string subAssetKey;
    std::string targetPlatform = "editor";
    Resources::ShaderReflection reflection;
    std::string shaderLabLightMode;
    std::optional<ShaderLab::ShaderLabPassState> shaderLabPassState;
    std::vector<ShaderArtifactStage> stages;
};

NLS_RENDER_API std::vector<uint8_t> SerializeShaderArtifact(const ShaderArtifact& artifact);
NLS_RENDER_API std::optional<ShaderArtifact> DeserializeShaderArtifact(std::string_view text);
NLS_RENDER_API std::optional<ShaderArtifact> DeserializeShaderArtifact(const std::vector<uint8_t>& bytes);
NLS_RENDER_API std::optional<ShaderArtifact> LoadShaderArtifact(const std::filesystem::path& path);
NLS_RENDER_API void AppendGlslShaderArtifactStages(ShaderArtifact& artifact);
NLS_RENDER_API bool HasUsableShaderArtifactStage(const ShaderArtifact& artifact);
NLS_RENDER_API bool HasUsableShaderArtifactStage(
    const ShaderArtifact& artifact,
    ShaderCompiler::ShaderTargetPlatform targetPlatform);
}
