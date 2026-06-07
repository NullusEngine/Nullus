#include "Panels/ImportedPrefabDragPreviewSession.h"

#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "Serialize/ObjectGraphInstantiator.h"

#include <algorithm>
#include <unordered_set>

namespace NLS::Editor::Panels
{
    namespace
    {
        void AddCachedPath(
            std::vector<std::string>& paths,
            std::unordered_set<std::string>& seen,
            const std::string& path)
        {
            if (path.empty() || !seen.insert(path).second)
                return;
            paths.push_back(path);
        }

        void CollectRendererPaths(
            NLS::Engine::GameObject& object,
            std::vector<std::string>& meshPaths,
            std::unordered_set<std::string>& seenMeshes,
            std::vector<std::string>& materialPaths,
            std::unordered_set<std::string>& seenMaterials,
            std::vector<ImportedPrefabDragPreviewRendererEntry>& rendererEntries)
        {
            auto* meshFilter = object.GetComponent<NLS::Engine::Components::MeshFilter>();
            auto* meshRenderer = object.GetComponent<NLS::Engine::Components::MeshRenderer>();

            if (meshFilter != nullptr)
                AddCachedPath(meshPaths, seenMeshes, meshFilter->GetModelPath());

            if (meshRenderer != nullptr)
            {
                for (const auto& materialPath : meshRenderer->GetMaterialPaths())
                    AddCachedPath(materialPaths, seenMaterials, materialPath);
            }

            if (meshFilter != nullptr && meshRenderer != nullptr)
                rendererEntries.push_back({ meshFilter, meshRenderer });

            for (auto* child : object.GetChildren())
            {
                if (child != nullptr)
                    CollectRendererPaths(*child, meshPaths, seenMeshes, materialPaths, seenMaterials, rendererEntries);
            }
        }
    }

    ImportedPrefabDragPreviewUpdate ImportedPrefabDragPreviewSession::BeginOrUpdate(
        const NLS::Engine::Assets::PrefabArtifact& prefab,
        NLS::Engine::SceneSystem::Scene& scene,
        std::string assetGuid,
        std::string subAssetKey,
        const Maths::Vector3& placement)
    {
        if (m_root != nullptr &&
            m_scene == &scene &&
            m_assetGuid == assetGuid &&
            m_subAssetKey == subAssetKey)
        {
            m_lastPlacement = placement;
            m_root->GetTransform()->SetWorldPosition(placement);
            return { false, m_root, m_scene, {} };
        }

        Clear();
        m_scene = &scene;
        m_assetGuid = std::move(assetGuid);
        m_subAssetKey = std::move(subAssetKey);

        NLS::Engine::Serialize::LoadPolicy previewLoadPolicy;
        previewLoadPolicy.deferAssetReferenceResolution = true;
        previewLoadPolicy.suppressGameObjectCreatedEvents = true;
        auto instantiated = NLS::Engine::Assets::InstantiatePrefabArtifact(
            prefab,
            scene,
            previewLoadPolicy);
        if (instantiated.diagnostics.HasErrors() || instantiated.root == nullptr)
        {
            ImportedPrefabDragPreviewUpdate failed;
            failed.diagnostics = instantiated.diagnostics;
            Clear();
            return failed;
        }

        m_root = instantiated.root;
        m_root->SetEditorTransient(true);
        m_lastPlacement = placement;
        m_root->GetTransform()->SetWorldPosition(placement);
        RebuildRendererPathCache();
        return { true, m_root, m_scene, instantiated.diagnostics };
    }

    void ImportedPrefabDragPreviewSession::Clear()
    {
        if (m_scene != nullptr && m_root != nullptr && m_root->IsAlive())
        {
            m_scene->DestroyGameObject(*m_root);
            m_scene->CollectGarbages();
        }
        m_scene = nullptr;
        m_root = nullptr;
        m_assetGuid.clear();
        m_subAssetKey.clear();
        m_lastPlacement.reset();
        m_cachedMeshPaths.clear();
        m_cachedMaterialPaths.clear();
        m_cachedRendererEntries.clear();
    }

    ImportedPrefabDragPreviewCommitHandoff ImportedPrefabDragPreviewSession::EndForCommit()
    {
        ImportedPrefabDragPreviewCommitHandoff handoff;
        handoff.placement = m_lastPlacement;
        handoff.root = m_root;
        if (handoff.root != nullptr)
            handoff.root->SetEditorTransient(false);
        m_scene = nullptr;
        m_root = nullptr;
        m_assetGuid.clear();
        m_subAssetKey.clear();
        m_lastPlacement.reset();
        m_cachedMeshPaths.clear();
        m_cachedMaterialPaths.clear();
        m_cachedRendererEntries.clear();
        return handoff;
    }

    void ImportedPrefabDragPreviewSession::UpdatePlacement(const Maths::Vector3& placement)
    {
        m_lastPlacement = placement;
        if (m_root != nullptr)
            m_root->GetTransform()->SetWorldPosition(placement);
    }

    NLS::Engine::SceneSystem::Scene* ImportedPrefabDragPreviewSession::GetPreviewScene() const
    {
        return m_scene;
    }

    NLS::Engine::GameObject* ImportedPrefabDragPreviewSession::GetRoot() const
    {
        return m_root;
    }

    const std::string& ImportedPrefabDragPreviewSession::GetAssetGuid() const
    {
        return m_assetGuid;
    }

    const std::string& ImportedPrefabDragPreviewSession::GetSubAssetKey() const
    {
        return m_subAssetKey;
    }

    const std::optional<Maths::Vector3>& ImportedPrefabDragPreviewSession::GetLastPlacement() const
    {
        return m_lastPlacement;
    }

    bool ImportedPrefabDragPreviewSession::ContainsObject(const NLS::Engine::GameObject& object) const
    {
        if (m_root == nullptr)
            return false;

        return m_root == &object || object.IsDescendantOf(m_root);
    }

    const std::vector<std::string>& ImportedPrefabDragPreviewSession::GetCachedMeshPaths() const
    {
        return m_cachedMeshPaths;
    }

    const std::vector<std::string>& ImportedPrefabDragPreviewSession::GetCachedMaterialPaths() const
    {
        return m_cachedMaterialPaths;
    }

    const std::vector<ImportedPrefabDragPreviewRendererEntry>& ImportedPrefabDragPreviewSession::GetCachedRendererEntries() const
    {
        return m_cachedRendererEntries;
    }

    void ImportedPrefabDragPreviewSession::RebuildRendererPathCache()
    {
        m_cachedMeshPaths.clear();
        m_cachedMaterialPaths.clear();
        m_cachedRendererEntries.clear();
        if (m_root == nullptr)
            return;

        std::unordered_set<std::string> seenMeshes;
        std::unordered_set<std::string> seenMaterials;
        CollectRendererPaths(
            *m_root,
            m_cachedMeshPaths,
            seenMeshes,
            m_cachedMaterialPaths,
            seenMaterials,
            m_cachedRendererEntries);
    }
}
