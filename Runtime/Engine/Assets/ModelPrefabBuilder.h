#pragma once

#include "Assets/ArtifactManifest.h"
#include "Engine/Assets/PrefabAsset.h"
#include "EngineDef.h"
#include "Rendering/Assets/ImportedScene.h"

#include <vector>

namespace NLS::Engine::Assets
{
NLS_ENGINE_API PrefabImportResult BuildGeneratedModelPrefab(
    const NLS::Render::Assets::ImportedScene& scene,
    const std::vector<NLS::Render::Assets::GeneratedSceneSubAsset>& subAssets,
    const NLS::Core::Assets::ArtifactManifest& manifest);
}
