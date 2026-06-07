#pragma once

#include "Assets/ArtifactManifest.h"
#include "Assets/AssetDiagnostics.h"
#include "Assets/AssetMeta.h"
#include "Assets/ImportProgressTracker.h"

#include <cstdint>
#include <filesystem>

namespace NLS::Editor::Assets
{
inline constexpr uint32_t kExternalTexturePostprocessorVersion = 2u;
inline constexpr const char* kExternalTextureBuildPipelineDependencyName = "external-texture-build-pipeline";

struct ExternalModelImportRequest
{
    std::filesystem::path sourcePath;
    std::filesystem::path stagingRoot;
    std::filesystem::path committedRoot;
    NLS::Core::Assets::AssetMeta meta;
    std::string sceneKey;
    std::string targetPlatform = "editor";
    const NLS::Core::Assets::ArtifactManifest* previousManifest = nullptr;
    ImportProgressTracker* progressTracker = nullptr;
    ImportJobId progressJob;
    std::filesystem::path textureResourcePathPrefix;
    std::filesystem::path projectRoot;
    std::string materialShaderResourcePath;
};

struct ExternalModelImportResult
{
    bool imported = false;
    NLS::Core::Assets::ArtifactManifest manifest;
    NLS::Core::Assets::AssetDiagnostics diagnostics;
};

ExternalModelImportResult ImportExternalModelAsset(const ExternalModelImportRequest& request);
}
