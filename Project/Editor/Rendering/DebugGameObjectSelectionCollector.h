#pragma once

#include <cstdint>
#include <vector>

#include <Components/CameraComponent.h>
#include <Components/LightComponent.h>
#include <Components/MeshFilter.h>
#include <Components/MeshRenderer.h>
#include <Engine/LayerMask.h>
#include <GameObject.h>
#include <Math/Matrix4.h>
#include <Math/Quaternion.h>
#include <Math/Vector3.h>
#include <Rendering/Data/Frustum.h>
#include <Rendering/Resources/Mesh.h>

namespace NLS::Editor::Rendering
{
    constexpr float kSelectionOutlineParentClassification = 0.0f;
    constexpr float kSelectionOutlineChildClassification = 1.0f;

    struct DebugGameObjectDebugDrawItems
    {
        struct SelectionMeshItem
        {
            Engine::Components::MeshRenderer* meshRenderer = nullptr;
            NLS::Render::Resources::Material* sourceMaterial = nullptr;
            NLS::Render::Resources::Mesh* mesh = nullptr;
            Maths::Matrix4 worldMatrix = Maths::Matrix4::Identity;
            Maths::Vector3 worldScale = Maths::Vector3::One;
            Maths::Quaternion worldRotation = Maths::Quaternion::Identity;
            Maths::Vector3 worldPosition = Maths::Vector3::Zero;
            float selectionGroupId = 1.0f;
            float selectionClassification = kSelectionOutlineParentClassification;
        };

        struct SelectionCameraItem
        {
            Engine::Components::CameraComponent* cameraComponent = nullptr;
            Maths::Vector3 worldPosition = Maths::Vector3::Zero;
            Maths::Quaternion worldRotation = Maths::Quaternion::Identity;
            float selectionGroupId = 1.0f;
            float selectionClassification = kSelectionOutlineParentClassification;
        };

        std::vector<SelectionMeshItem> selectionMeshItems;
        std::vector<SelectionCameraItem> cameras;
        std::vector<Engine::Components::LightComponent*> lights;
        uint64_t visitedGameObjects = 0u;

        void Clear()
        {
            selectionMeshItems.clear();
            cameras.clear();
            lights.clear();
            visitedGameObjects = 0u;
        }
    };

    struct DebugGameObjectSelectionFilter
    {
        Engine::LayerMask visibleLayers { 0xFFFFFFFFu };
        const NLS::Render::Data::Frustum* frustum = nullptr;
        bool excludeEditorOnlyObjects = true;
    };

    inline bool IsSelectedDebugActorVisibleToSceneView(
        const Engine::GameObject& actor,
        const DebugGameObjectSelectionFilter& filter)
    {
        if (!filter.visibleLayers.ContainsLayer(actor.GetLayer()))
            return false;

        return !filter.excludeEditorOnlyObjects || actor.GetTag() != "EditorOnly";
    }

    inline bool IsSelectedDebugMeshVisibleToSceneView(
        const DebugGameObjectDebugDrawItems::SelectionMeshItem& item,
        const DebugGameObjectSelectionFilter& filter)
    {
        if (filter.frustum == nullptr)
            return true;
        if (item.meshRenderer == nullptr || item.mesh == nullptr)
            return false;
        if (item.meshRenderer->GetFrustumBehaviour() == Engine::Components::MeshRenderer::EFrustumBehaviour::DISABLED)
            return true;

        return filter.frustum->BoundsInFrustum(item.mesh->GetBounds(), item.worldMatrix);
    }

    inline void CollectSelectedDebugGameObjectDebugDrawItems(
        Engine::GameObject& selectedGameObject,
        DebugGameObjectDebugDrawItems& outItems,
        const bool collectCameras = true,
        const bool collectLights = true,
        const DebugGameObjectSelectionFilter& filter = {})
    {
        outItems.Clear();

        auto collect = [&outItems, collectCameras, collectLights, &filter](
            Engine::GameObject& actor,
            const uint32_t actorDepth,
            auto&& collectSelf) -> void
        {
            if (!actor.IsActive())
                return;

            ++outItems.visitedGameObjects;
            const bool actorVisibleToSceneView = IsSelectedDebugActorVisibleToSceneView(actor, filter);

            if (auto* meshRenderer = actorVisibleToSceneView
                    ? actor.GetComponent<Engine::Components::MeshRenderer>()
                    : nullptr;
                meshRenderer != nullptr && meshRenderer->IsSelfEnabled())
            {
                auto* meshFilter = actor.GetComponent<Engine::Components::MeshFilter>();
                auto* mesh = meshFilter != nullptr ? meshFilter->ResolveMesh() : nullptr;
                if (mesh != nullptr)
                {
                    NLS::Render::Resources::Material* sourceMaterial = nullptr;
                    const auto& materials = meshRenderer->ResolveMaterials();
                    if (mesh->GetMaterialIndex() < materials.size())
                        sourceMaterial = materials[mesh->GetMaterialIndex()];

                    auto* transform = actor.GetTransform();
                    DebugGameObjectDebugDrawItems::SelectionMeshItem item {
                        meshRenderer,
                        sourceMaterial,
                        mesh,
                        transform->GetWorldMatrix(),
                        transform->GetWorldScale(),
                        transform->GetWorldRotation(),
                        transform->GetWorldPosition(),
                        1.0f,
                        actorDepth == 0u ? kSelectionOutlineParentClassification : kSelectionOutlineChildClassification
                    };
                    if (IsSelectedDebugMeshVisibleToSceneView(item, filter))
                        outItems.selectionMeshItems.push_back(item);
                }
            }

            if (collectCameras && actorVisibleToSceneView)
            {
                if (auto* camera = actor.GetComponent<Engine::Components::CameraComponent>(); camera != nullptr)
                {
                    auto* transform = actor.GetTransform();
                    outItems.cameras.push_back({
                        camera,
                        transform->GetWorldPosition(),
                        transform->GetWorldRotation(),
                        1.0f,
                        actorDepth == 0u ? kSelectionOutlineParentClassification : kSelectionOutlineChildClassification
                    });
                }
            }

            if (collectLights && actorVisibleToSceneView)
            {
                if (auto* light = actor.GetComponent<Engine::Components::LightComponent>(); light != nullptr)
                    outItems.lights.push_back(light);
            }

            for (auto* child : actor.GetChildren())
            {
                if (child != nullptr)
                    collectSelf(*child, actorDepth + 1u, collectSelf);
            }
        };

        collect(selectedGameObject, 0u, collect);
    }
}
