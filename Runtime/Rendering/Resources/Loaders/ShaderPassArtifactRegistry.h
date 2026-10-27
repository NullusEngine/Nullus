#pragma once

#include "RenderDef.h"

#include <string>
#include <string_view>
#include <vector>

namespace NLS::Render::Resources::Loaders
{
// Packaged players do not ship ArtifactDB. RuntimeAssetDatabase feeds the
// shader pass artifact paths from RuntimeAssetManifest.json into this small
// registry so MaterialLoader can bind ShaderLab passes by source path.
NLS_RENDER_API void RegisterShaderPassArtifacts(
    std::string_view shaderSourcePath,
    std::vector<std::string> artifactPaths);
NLS_RENDER_API void ClearShaderPassArtifactRegistry();
NLS_RENDER_API std::vector<std::string> FindShaderPassArtifacts(
    std::string_view shaderSourcePath);
} // namespace NLS::Render::Resources::Loaders
