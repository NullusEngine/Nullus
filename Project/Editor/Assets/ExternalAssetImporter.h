#pragma once

#include "Assets/ArtifactManifest.h"
#include "Assets/ArtifactWriter.h"
#include "Assets/AssetDiagnostics.h"
#include "Assets/AssetMeta.h"
#include "Assets/ImportProgressTracker.h"
#include "Assets/ModelTextureResolutionReport.h"
#include "Assets/PreviewRenderableSnapshot.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

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
    std::filesystem::path editorPathRoot;
    bool preserveModelLocalTextureArtifacts = false;
    std::function<std::optional<NLS::Core::Assets::ArtifactManifest>(
        NLS::Core::Assets::AssetId,
        const std::string&)> loadArtifactManifest;
    // Background preparation must never create texture metadata or artifacts.
    bool allowAutoImportMissingTextureFiles = true;
    bool prepareOnly = false;
};

struct ExternalModelAutoImportedDependency
{
    std::filesystem::path sourcePath;
    std::filesystem::path metaPath;
    bool createdMeta = false;
    std::vector<std::filesystem::path> committedArtifactPaths;
    NLS::Core::Assets::ArtifactManifest manifest;
};

struct ExternalModelImportResult
{
    bool imported = false;
    bool prepared = false;
    bool requiresSerialImport = false;
    NLS::Core::Assets::ArtifactManifest manifest;
    std::vector<ExternalModelAutoImportedDependency> autoImportedDependencies;
    NLS::Core::Assets::AssetDiagnostics diagnostics;
    std::optional<NLS::Core::Assets::ArtifactWriteRequest> preparedWriteRequest;
    std::optional<ModelTextureResolutionReport> preparedTextureResolutionReport;
    // Complete Prefab topology retained from import preparation so a
    // post-commit thumbnail does not deserialize the Prefab artifact again.
    std::shared_ptr<const PreviewRenderableSnapshot> preparedPrefabPreviewSnapshot;
    // Serialized mesh output retained only until the import thumbnail has
    // handed each dependency to MeshManager. Nothing here is persisted.
    std::vector<PreparedPrefabPreviewMeshPayload> preparedPrefabPreviewMeshPayloads;
};

ExternalModelImportResult ImportExternalModelAsset(const ExternalModelImportRequest& request);
ExternalModelImportResult CommitPreparedExternalModelAsset(
    const ExternalModelImportRequest& request,
    ExternalModelImportResult preparedResult);
}
