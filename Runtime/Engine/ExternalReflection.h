#pragma once

#include "Reflection/Macros.h"
#include "Reflection/Array.h"
#include "Components/CameraComponent.h"
#include "Components/LightComponent.h"
#include "Components/MaterialRenderer.h"
#include "Components/MeshRenderer.h"
#include "Components/TransformComponent.h"
#include "Core/ResourceManagement/MaterialManager.h"
#include "Core/ResourceManagement/ModelManager.h"
#include "Core/ServiceLocator.h"
#include "Debug/Logger.h"
#include "GameObject.h"
#include "Rendering/Geometry/BoundingSphere.h"
#include "SceneSystem/Scene.h"

namespace NLS::Engine::Reflection
{
using namespace NLS::Engine::Components;

inline int GetProjectionModeValue(const CameraComponent &component)
{
    return static_cast<int>(component.GetProjectionMode());
}

inline void SetProjectionModeValue(CameraComponent &component, int value)
{
    component.SetProjectionMode(static_cast<NLS::Render::Settings::EProjectionMode>(value));
}

inline int GetLightTypeValue(const LightComponent &component)
{
    return static_cast<int>(component.GetLightType());
}

inline void SetLightTypeValue(LightComponent &component, int value)
{
    component.SetLightType(static_cast<NLS::Render::Settings::ELightType>(value));
}

inline int GetFrustumBehaviourValue(const MeshRenderer &component)
{
    return static_cast<int>(component.GetFrustumBehaviour());
}

inline void SetFrustumBehaviourValue(MeshRenderer &component, int value)
{
    component.SetFrustumBehaviour(static_cast<MeshRenderer::EFrustumBehaviour>(value));
}

inline std::string GetModelPath(const MeshRenderer &component)
{
    if (auto *model = component.GetModel())
        return model->path;
    return {};
}

inline void SetModelPath(MeshRenderer &component, const std::string &path)
{
    if (path.empty())
    {
        component.SetModel(nullptr);
        return;
    }

    auto *model = NLS_SERVICE(Core::ResourceManagement::ModelManager)[path];
    if (!model)
        NLS_LOG_WARNING("Failed to resolve model path during reflection load: " + path);
    component.SetModel(model);
}

inline NLS::Array<std::string> GetMaterialPaths(const MaterialRenderer &component)
{
    NLS::Array<std::string> result;
    const auto &materials = component.GetMaterials();
    size_t lastUsedIndex = 0;
    bool hasMaterial = false;
    for (size_t index = 0; index < materials.size(); ++index)
    {
        if (materials[index] && !materials[index]->path.empty())
        {
            lastUsedIndex = index;
            hasMaterial = true;
        }
    }

    if (!hasMaterial)
        return result;

    for (size_t index = 0; index <= lastUsedIndex; ++index)
    {
        if (materials[index] && !materials[index]->path.empty())
            result.push_back(materials[index]->path);
        else
            result.push_back({});
    }

    return result;
}

inline void SetMaterialPaths(MaterialRenderer &component, const NLS::Array<std::string> &paths)
{
    component.RemoveAllMaterials();
    for (size_t index = 0; index < paths.size() && index < kMaxMaterialCount; ++index)
    {
        if (paths[index].empty())
            continue;

        if (auto *material = NLS_SERVICE(Core::ResourceManagement::MaterialManager)[paths[index]])
            component.SetMaterialAtIndex(static_cast<uint8_t>(index), *material);
    }
}

inline NLS::Array<float> GetUserMatrixValues(const MaterialRenderer &component)
{
    NLS::Array<float> result;
    result.reserve(16);
    const auto &matrix = component.GetUserMatrix();
    for (float value : matrix.data)
        result.push_back(value);
    return result;
}

inline void SetUserMatrixValues(MaterialRenderer &component, const NLS::Array<float> &values)
{
    for (uint32_t row = 0; row < 4; ++row)
    {
        for (uint32_t column = 0; column < 4; ++column)
        {
            const size_t index = static_cast<size_t>(row) * 4 + column;
            if (index < values.size())
                component.SetUserMatrixElement(row, column, values[index]);
        }
    }
}
} // namespace NLS::Engine::Reflection

namespace NLS::Render::Geometry
{
MetaExternal(NLS::Render::Geometry::BoundingSphere)

REFLECT_EXTERNAL(
    NLS::Render::Geometry::BoundingSphere,
    Fields(
        REFLECT_FIELD(NLS::Maths::Vector3, position),
        REFLECT_FIELD(float, radius)
    )
)
} // namespace NLS::Render::Geometry

namespace NLS::Engine
{
MetaExternal(NLS::Engine::GameObject)

REFLECT_EXTERNAL(
    NLS::Engine::GameObject,
    Fields(
        REFLECT_PROPERTY(std::string, name, &NLS::Engine::GameObject::GetName, &NLS::Engine::GameObject::SetName),
        REFLECT_PROPERTY(std::string, tag, &NLS::Engine::GameObject::GetTag, &NLS::Engine::GameObject::SetTag),
        REFLECT_PROPERTY(bool, active, &NLS::Engine::GameObject::IsSelfActive, &NLS::Engine::GameObject::SetActive),
        REFLECT_PROPERTY(int, worldID, &NLS::Engine::GameObject::GetWorldID, &NLS::Engine::GameObject::SetWorldID)
    )
)
} // namespace NLS::Engine

namespace NLS::Engine::SceneSystem
{
MetaExternal(NLS::Engine::SceneSystem::Scene)

REFLECT_EXTERNAL(
    NLS::Engine::SceneSystem::Scene,
    Methods(
        REFLECT_METHOD_EX(GetActors, static_cast<const std::vector<NLS::Engine::GameObject*>& (NLS::Engine::SceneSystem::Scene::*)() const>(&NLS::Engine::SceneSystem::Scene::GetActors))
    )
)
} // namespace NLS::Engine::SceneSystem

namespace NLS::Engine::Components
{
MetaExternal(NLS::Engine::Components::TransformComponent)

REFLECT_EXTERNAL(
    NLS::Engine::Components::TransformComponent,
    Fields(
        REFLECT_PROPERTY(NLS::Maths::Vector3, localPosition, &NLS::Engine::Components::TransformComponent::GetLocalPosition, &NLS::Engine::Components::TransformComponent::SetLocalPosition),
        REFLECT_PROPERTY(NLS::Maths::Quaternion, localRotation, &NLS::Engine::Components::TransformComponent::GetLocalRotation, &NLS::Engine::Components::TransformComponent::SetLocalRotation),
        REFLECT_PROPERTY(NLS::Maths::Vector3, localScale, &NLS::Engine::Components::TransformComponent::GetLocalScale, &NLS::Engine::Components::TransformComponent::SetLocalScale)
    )
)

MetaExternal(NLS::Engine::Components::CameraComponent)

REFLECT_EXTERNAL(
    NLS::Engine::Components::CameraComponent,
    Fields(
        REFLECT_PROPERTY(float, fov, &NLS::Engine::Components::CameraComponent::GetFov, &NLS::Engine::Components::CameraComponent::SetFov),
        REFLECT_PROPERTY(float, size, &NLS::Engine::Components::CameraComponent::GetSize, &NLS::Engine::Components::CameraComponent::SetSize),
        REFLECT_PROPERTY(float, near, &NLS::Engine::Components::CameraComponent::GetNear, &NLS::Engine::Components::CameraComponent::SetNear),
        REFLECT_PROPERTY(float, far, &NLS::Engine::Components::CameraComponent::GetFar, &NLS::Engine::Components::CameraComponent::SetFar),
        REFLECT_PROPERTY(NLS::Maths::Vector3, clearColor, &NLS::Engine::Components::CameraComponent::GetClearColor, &NLS::Engine::Components::CameraComponent::SetClearColor),
        REFLECT_PROPERTY(bool, frustumGeometryCulling, &NLS::Engine::Components::CameraComponent::HasFrustumGeometryCulling, &NLS::Engine::Components::CameraComponent::SetFrustumGeometryCulling),
        REFLECT_PROPERTY(bool, frustumLightCulling, &NLS::Engine::Components::CameraComponent::HasFrustumLightCulling, &NLS::Engine::Components::CameraComponent::SetFrustumLightCulling),
        REFLECT_PROPERTY(int, projectionMode, &NLS::Engine::Reflection::GetProjectionModeValue, &NLS::Engine::Reflection::SetProjectionModeValue)
    )
)

MetaExternal(NLS::Engine::Components::LightComponent)

REFLECT_EXTERNAL(
    NLS::Engine::Components::LightComponent,
    Fields(
        REFLECT_PROPERTY(int, lightType, &NLS::Engine::Reflection::GetLightTypeValue, &NLS::Engine::Reflection::SetLightTypeValue),
        REFLECT_PROPERTY(NLS::Maths::Vector3, color, &NLS::Engine::Components::LightComponent::GetColor, &NLS::Engine::Components::LightComponent::SetColor),
        REFLECT_PROPERTY(float, intensity, &NLS::Engine::Components::LightComponent::GetIntensity, &NLS::Engine::Components::LightComponent::SetIntensity),
        REFLECT_PROPERTY(float, constant, &NLS::Engine::Components::LightComponent::GetConstant, &NLS::Engine::Components::LightComponent::SetConstant),
        REFLECT_PROPERTY(float, linear, &NLS::Engine::Components::LightComponent::GetLinear, &NLS::Engine::Components::LightComponent::SetLinear),
        REFLECT_PROPERTY(float, quadratic, &NLS::Engine::Components::LightComponent::GetQuadratic, &NLS::Engine::Components::LightComponent::SetQuadratic),
        REFLECT_PROPERTY(float, cutoff, &NLS::Engine::Components::LightComponent::GetCutoff, &NLS::Engine::Components::LightComponent::SetCutoff),
        REFLECT_PROPERTY(float, outerCutoff, &NLS::Engine::Components::LightComponent::GetOuterCutoff, &NLS::Engine::Components::LightComponent::SetOuterCutoff),
        REFLECT_PROPERTY(float, radius, &NLS::Engine::Components::LightComponent::GetRadius, &NLS::Engine::Components::LightComponent::SetRadius),
        REFLECT_PROPERTY(NLS::Maths::Vector3, size, &NLS::Engine::Components::LightComponent::GetSize, &NLS::Engine::Components::LightComponent::SetSize)
    )
)

MetaExternal(NLS::Engine::Components::MeshRenderer)

REFLECT_EXTERNAL(
    NLS::Engine::Components::MeshRenderer,
    Fields(
        REFLECT_PROPERTY(std::string, model, &NLS::Engine::Reflection::GetModelPath, &NLS::Engine::Reflection::SetModelPath),
        REFLECT_PROPERTY(int, frustumBehaviour, &NLS::Engine::Reflection::GetFrustumBehaviourValue, &NLS::Engine::Reflection::SetFrustumBehaviourValue),
        REFLECT_PROPERTY(NLS::Render::Geometry::BoundingSphere, customBoundingSphere, &NLS::Engine::Components::MeshRenderer::GetCustomBoundingSphere, &NLS::Engine::Components::MeshRenderer::SetCustomBoundingSphere)
    )
)

MetaExternal(NLS::Engine::Components::MaterialRenderer)

REFLECT_EXTERNAL(
    NLS::Engine::Components::MaterialRenderer,
    Fields(
        REFLECT_PROPERTY(NLS::Array<std::string>, materials, &NLS::Engine::Reflection::GetMaterialPaths, &NLS::Engine::Reflection::SetMaterialPaths),
        REFLECT_PROPERTY(NLS::Array<float>, userMatrix, &NLS::Engine::Reflection::GetUserMatrixValues, &NLS::Engine::Reflection::SetUserMatrixValues)
    )
)
} // namespace NLS::Engine::Components
