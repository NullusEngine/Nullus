#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Engine/Assets/PrefabAsset.h"
#include "Engine/Assets/RuntimeAssetDatabase.h"
#include "EngineDef.h"

namespace NLS::Engine::Rendering
{
struct SceneRendererMaterialArtifactBinding
{
    std::string rendererDebugName;
    uint32_t slotIndex = 0u;
    NLS::Engine::Assets::RuntimeAssetRef reference;
    NLS::Core::Assets::ArtifactType artifactType = NLS::Core::Assets::ArtifactType::Unknown;
    std::string loaderId;
    std::string artifactPath;
    bool resolved = false;
};

NLS_ENGINE_API std::vector<SceneRendererMaterialArtifactBinding> ResolveSceneRendererMaterialBindings(
    const NLS::Engine::Assets::PrefabArtifact& prefab,
    const NLS::Engine::Assets::RuntimeAssetDatabase& runtimeAssets);
}
