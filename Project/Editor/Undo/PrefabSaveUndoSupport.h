#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <Assets/AssetMeta.h>
#include "Assets/PrefabEditorWorkflow.h"
#include "GameObject.h"
#include "SceneSystem/Scene.h"

namespace NLS::Editor::Undo::PrefabSave
{
// A Prefab connection is identified by the asset, the concrete subasset and
// the owning scene/stage. Matching only the asset GUID can disconnect a newer
// connection that was established outside the Undo timeline.
[[nodiscard]] inline bool MatchesInstanceIdentity(
    const NLS::Editor::Assets::PrefabInstanceRecord& instance,
    const NLS::Core::Assets::AssetId prefabAssetId,
    const std::string_view prefabSubAssetKey,
    const NLS::Core::Assets::AssetId sceneAssetId)
{
    return instance.prefabAssetId == prefabAssetId &&
        instance.prefabSubAssetKey == prefabSubAssetKey &&
        instance.sceneAssetId == sceneAssetId;
}

// Resolves the durable owner that will receive the prefab-instance dependency.
// Unsaved current scenes intentionally have no asset identifier. Other loaded
// scenes cannot be associated safely and are rejected instead of inheriting the
// current scene's identity.
[[nodiscard]] inline std::optional<NLS::Core::Assets::AssetId> ResolveOwningSceneAssetId(
    const NLS::Engine::GameObject& gameObject,
    const NLS::Engine::SceneSystem::Scene* currentScene,
    const std::filesystem::path& currentSceneSourcePath,
    const NLS::Editor::Assets::PrefabStageState* activePrefabStage,
    std::string& error)
{
    error.clear();

    const auto* owningScene = gameObject.GetScene();
    if (owningScene == nullptr)
    {
        error = "the GameObject is not owned by a live Scene";
        return std::nullopt;
    }

    if (activePrefabStage != nullptr &&
        activePrefabStage->stageScene != nullptr &&
        activePrefabStage->stageScene.get() == owningScene)
    {
        if (!activePrefabStage->prefabAssetId.IsValid())
        {
            error = "the active Prefab Stage has no stable asset identifier";
            return std::nullopt;
        }
        return activePrefabStage->prefabAssetId;
    }

    if (owningScene != currentScene)
    {
        error = "the GameObject belongs to a scene that is not the active edit scene or Prefab Stage";
        return std::nullopt;
    }

    if (currentSceneSourcePath.empty())
        return NLS::Core::Assets::AssetId {};

    const auto metadata = NLS::Core::Assets::AssetMeta::Load(
        NLS::Core::Assets::GetAssetMetaPath(currentSceneSourcePath));
    if (!metadata.has_value() || !metadata->id.IsValid())
    {
        error = "the active Scene is missing a valid .meta asset identifier";
        return std::nullopt;
    }
    return metadata->id;
}
} // namespace NLS::Editor::Undo::PrefabSave
