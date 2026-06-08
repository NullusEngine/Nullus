#pragma once

#include "Assets/ArtifactManifest.h"
#include "Engine/Assets/PrefabAsset.h"
#include "EngineDef.h"
#include "Rendering/Assets/ImportedScene.h"

#include <vector>

namespace NLS::Engine::Assets
{
struct NLS_ENGINE_API GeneratedModelPrefabHLODSchema
{
    static constexpr const char* PropertyName = "largeSceneHLOD";
    static constexpr const char* SourceField = "source";
    static constexpr const char* ImportedHierarchySource = "imported-hierarchy";
    static constexpr const char* ClusterKeyField = "clusterKey";
    static constexpr const char* ChildrenField = "children";
    static constexpr const char* ProxySubAssetKeyField = "proxySubAssetKey";
    static constexpr const char* ProxySubAssetKeyPrefix = "hlod-proxy:";
};

NLS_ENGINE_API PrefabImportResult BuildGeneratedModelPrefab(
    const NLS::Render::Assets::ImportedScene& scene,
    const std::vector<NLS::Render::Assets::GeneratedSceneSubAsset>& subAssets,
    const NLS::Core::Assets::ArtifactManifest& manifest);
}
