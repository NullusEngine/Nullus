#pragma once

#include "Engine/Assets/PrefabAsset.h"
#include "SceneSystem/Scene.h"
#include "Math/Vector3.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace NLS::Engine::Components
{
    class MeshFilter;
    class MeshRenderer;
}

namespace NLS::Editor::Panels
{
    struct ImportedPrefabDragPreviewRendererEntry
    {
        NLS::Engine::Components::MeshFilter* meshFilter = nullptr;
        NLS::Engine::Components::MeshRenderer* meshRenderer = nullptr;
    };

    struct ImportedPrefabDragPreviewUpdate
    {
        bool created = false;
        NLS::Engine::GameObject* root = nullptr;
        NLS::Engine::SceneSystem::Scene* scene = nullptr;
        NLS::Engine::Serialize::SerializationDiagnosticList diagnostics;
    };

    struct ImportedPrefabDragPreviewCommitHandoff
    {
        std::optional<Maths::Vector3> placement;
        NLS::Engine::GameObject* root = nullptr;
    };

    class ImportedPrefabDragPreviewSession
    {
    public:
        ImportedPrefabDragPreviewUpdate BeginOrUpdate(
            const NLS::Engine::Assets::PrefabArtifact& prefab,
            NLS::Engine::SceneSystem::Scene& scene,
            std::string assetGuid,
            std::string subAssetKey,
            const Maths::Vector3& placement);

        void Clear();
        ImportedPrefabDragPreviewCommitHandoff EndForCommit();
        void UpdatePlacement(const Maths::Vector3& placement);

        NLS::Engine::SceneSystem::Scene* GetPreviewScene() const;
        NLS::Engine::GameObject* GetRoot() const;
        const std::string& GetAssetGuid() const;
        const std::string& GetSubAssetKey() const;
        const std::optional<Maths::Vector3>& GetLastPlacement() const;
        bool ContainsObject(const NLS::Engine::GameObject& object) const;
        const std::vector<std::string>& GetCachedMeshPaths() const;
        const std::vector<std::string>& GetCachedMaterialPaths() const;
        const std::vector<ImportedPrefabDragPreviewRendererEntry>& GetCachedRendererEntries() const;

    private:
        void RebuildRendererPathCache();

        NLS::Engine::SceneSystem::Scene* m_scene = nullptr;
        NLS::Engine::GameObject* m_root = nullptr;
        std::string m_assetGuid;
        std::string m_subAssetKey;
        std::optional<Maths::Vector3> m_lastPlacement;
        std::vector<std::string> m_cachedMeshPaths;
        std::vector<std::string> m_cachedMaterialPaths;
        std::vector<ImportedPrefabDragPreviewRendererEntry> m_cachedRendererEntries;
    };
}
