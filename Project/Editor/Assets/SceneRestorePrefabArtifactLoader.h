#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "Assets/AssetId.h"
#include "Assets/AssetImporterFacade.h"
#include "Assets/EditorAssetDragDropBridge.h"
#include "Assets/EditorAssetPathUtils.h"

namespace NLS::Engine::Assets
{
    class PrefabArtifact;
}

namespace NLS::Editor::Assets
{
    class EditorAssetDragDropBridge;

    inline std::shared_ptr<const NLS::Engine::Assets::PrefabArtifact> LoadSceneRestorePrefabArtifactReady(
        EditorAssetDragDropBridge& prefabArtifactLoader,
        const std::filesystem::path& projectRoot,
        const std::string& assetPath,
        const std::string& subAssetKey,
        NLS::Core::Assets::AssetId assetId,
        const std::filesystem::path& absoluteSourcePath,
        const std::string& sceneOwnerScope,
        const bool allowRepairReimport = true,
        const bool requireRendererArtifacts = true,
        std::string* diagnosticCode = nullptr,
        std::string* diagnosticMessage = nullptr)
    {
        auto loadRequest = MakeSceneRestoreUnifiedPrefabLoadRequest(
            NormalizePrefabSourceIdentity(
                projectRoot,
                assetPath,
                subAssetKey,
                assetId),
            sceneOwnerScope);
        if (!requireRendererArtifacts)
        {
            loadRequest.requiredReadiness = UnifiedPrefabReadiness::PrefabGraphOnly;
            loadRequest.allowPending = false;
        }
        auto unifiedLoad = prefabArtifactLoader.LoadUnifiedPrefabShared(loadRequest);
        auto artifact = std::move(unifiedLoad.prefab);
        if (diagnosticCode != nullptr)
            *diagnosticCode = unifiedLoad.diagnosticCode;
        if (diagnosticMessage != nullptr)
            *diagnosticMessage = unifiedLoad.diagnosticMessage;

        const bool requiresRendererReadyRestore =
            NLS::Core::Assets::InferAssetType(absoluteSourcePath) == NLS::Core::Assets::AssetType::ModelScene;
        const bool shouldRepairGeneratedModelRestore =
            requiresRendererReadyRestore &&
            !artifact &&
            allowRepairReimport;
        if (!shouldRepairGeneratedModelRestore)
            return artifact;

        AssetImporterFacade importer(MakeProjectEditorAssetRoots(projectRoot));
        if (!importer.SaveAndReimport(assetPath))
            return artifact;

        unifiedLoad = prefabArtifactLoader.LoadUnifiedPrefabShared(loadRequest);
        if (diagnosticCode != nullptr)
            *diagnosticCode = unifiedLoad.diagnosticCode;
        if (diagnosticMessage != nullptr)
            *diagnosticMessage = unifiedLoad.diagnosticMessage;
        return std::move(unifiedLoad.prefab);
    }
}
