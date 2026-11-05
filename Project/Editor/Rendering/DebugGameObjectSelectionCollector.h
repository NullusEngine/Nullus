#pragma once
#include <array>
#include <span>
#include <unordered_map>
#include <unordered_set>

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
#include <Rendering/Resources/Shader.h>
#include <Rendering/Geometry/BoundingSphereUtils.h>

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
        std::vector<int32_t> hiddenObjectIds;
    };

    inline bool IsSelectedDebugActorVisibleToSceneView(
        const Engine::GameObject& actor,
        const DebugGameObjectSelectionFilter& filter)
    {
        if (std::binary_search(filter.hiddenObjectIds.begin(), filter.hiddenObjectIds.end(), actor.GetInstanceID())) return false;
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

        const auto* shader=item.sourceMaterial?item.sourceMaterial->GetShader():nullptr;
        const auto contract=shader?shader->GetMaterialInterface():nullptr;
        if(contract && contract->usesWorldPositionOffset)
        {auto bounds=NLS::Render::Geometry::TransformBounds(item.mesh->GetBounds(),item.worldMatrix);
            const auto padding=contract->maximumWorldDisplacement*2.f;bounds.size+=Maths::Vector3(padding,padding,padding);
            return filter.frustum->BoundsInFrustum(bounds,Maths::Matrix4::Identity);}
        return filter.frustum->BoundsInFrustum(item.mesh->GetBounds(),item.worldMatrix);
    }

    inline void CollectSelectedDebugGameObjectsDebugDrawItems(
        std::span<Engine::GameObject* const> selectedGameObjects,
        DebugGameObjectDebugDrawItems& outItems,
        const bool collectCameras = true,
        const bool collectLights = true,
        const DebugGameObjectSelectionFilter& filter = {})
    {
        outItems.Clear();
        std::unordered_set<Engine::GameObject*> visited;
        std::unordered_map<Engine::GameObject*, float> explicitGroups;
        for (auto* object : selectedGameObjects)
            if (object && !explicitGroups.contains(object)) explicitGroups.emplace(object, static_cast<float>(explicitGroups.size() + 1));

        auto collect = [&outItems, collectCameras, collectLights, &filter, &visited, &explicitGroups](
            Engine::GameObject& actor,
            const uint32_t actorDepth,
            float group,
            auto&& collectSelf) -> void
        {
            if (!actor.IsActive() || !visited.insert(&actor).second)
                return;
            const auto explicitGroup = explicitGroups.find(&actor);
            const bool explicitlySelected = explicitGroup != explicitGroups.end();
            if (explicitlySelected) group = explicitGroup->second;

            ++outItems.visitedGameObjects;
            const bool actorVisibleToSceneView = IsSelectedDebugActorVisibleToSceneView(actor, filter);

            // Resolve every component this node can contribute in one pass.
            // GetComponent<T>() re-enters the reflection registry for each
            // requested type, and doing that up to four times per GameObject
            // dominated large selection collections.
            Engine::Components::MeshRenderer* meshRenderer = nullptr;
            Engine::Components::MeshFilter* meshFilter = nullptr;
            Engine::Components::CameraComponent* cameraComponent = nullptr;
            Engine::Components::LightComponent* lightComponent = nullptr;
            if (actorVisibleToSceneView)
            {
                for (const auto& component : actor.GetComponents())
                {
                    if (component == nullptr)
                        continue;

                    auto* rawComponent = component.get();
                    if (meshRenderer == nullptr)
                        meshRenderer = dynamic_cast<Engine::Components::MeshRenderer*>(rawComponent);
                    if (meshFilter == nullptr)
                        meshFilter = dynamic_cast<Engine::Components::MeshFilter*>(rawComponent);
                    if (collectCameras && cameraComponent == nullptr)
                        cameraComponent = dynamic_cast<Engine::Components::CameraComponent*>(rawComponent);
                    if (collectLights && lightComponent == nullptr)
                        lightComponent = dynamic_cast<Engine::Components::LightComponent*>(rawComponent);

                    if (meshRenderer != nullptr && meshFilter != nullptr &&
                        (!collectCameras || cameraComponent != nullptr) &&
                        (!collectLights || lightComponent != nullptr))
                    {
                        break;
                    }
                }
            }

            if (meshRenderer != nullptr && meshRenderer->IsSelfEnabled())
            {
                auto* mesh = meshFilter != nullptr ? meshFilter->ResolveMesh() : nullptr;
                if (mesh != nullptr)
                {
                    NLS::Render::Resources::Material* sourceMaterial = nullptr;
                    const auto materialIndex = mesh->GetMaterialIndex();
                    if (materialIndex < meshRenderer->GetMaterials().size())
                        sourceMaterial = meshRenderer->ResolveMaterialAtIndex(materialIndex);

                    auto* transform = actor.GetTransform();
                    DebugGameObjectDebugDrawItems::SelectionMeshItem item {
                        meshRenderer,
                        sourceMaterial,
                        mesh,
                        transform->GetWorldMatrix(),
                        transform->GetWorldScale(),
                        transform->GetWorldRotation(),
                        transform->GetWorldPosition(),
                        group,
                        explicitlySelected ? kSelectionOutlineParentClassification : kSelectionOutlineChildClassification
                    };
                    if (IsSelectedDebugMeshVisibleToSceneView(item, filter))
                        outItems.selectionMeshItems.push_back(item);
                }
            }

            if (cameraComponent != nullptr)
            {
                auto* transform = actor.GetTransform();
                outItems.cameras.push_back({
                    cameraComponent,
                    transform->GetWorldPosition(),
                    transform->GetWorldRotation(),
                    group,
                    explicitlySelected ? kSelectionOutlineParentClassification : kSelectionOutlineChildClassification
                });
            }

            if (lightComponent != nullptr)
                outItems.lights.push_back(lightComponent);

            for (auto* child : actor.GetChildren())
            {
                if (child != nullptr)
                    collectSelf(*child, actorDepth + 1u, group, collectSelf);
            }
        };

        for (auto* object : selectedGameObjects)
            if (object) collect(*object, 0u, explicitGroups[object], collect);
    }
    inline void CollectSelectedDebugGameObjectDebugDrawItems(
        Engine::GameObject& selectedGameObject, DebugGameObjectDebugDrawItems& outItems,
        const bool collectCameras = true, const bool collectLights = true,
        const DebugGameObjectSelectionFilter& filter = {})
    {
        std::array objects{&selectedGameObject};
        CollectSelectedDebugGameObjectsDebugDrawItems(objects, outItems, collectCameras, collectLights, filter);
    }
}
