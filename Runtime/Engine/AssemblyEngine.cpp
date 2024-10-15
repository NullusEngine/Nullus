#include "AssemblyEngine.h"
#include "Components/Component.h"
#include "Components/TransformComponent.h"
#include "Components/CameraComponent.h"
#include "Components/LightComponent.h"
#include "Components/MeshRenderer.h"
#include "Components/MaterialRenderer.h"
#include "Components/SkyBoxComponent.h"
#include "GameObject.h"
#include "Serialize/Serializer.h"
#include "SceneSystem/Scene.h"
#include "Serialize/GameobjectSerialize.h"
namespace NLS
{
namespace Engine
{
    using namespace Components;
void AssemblyEngine::Initialize()
{
    Serializer::Instance()->AddHandler<GameObjectSerializeHandler>();

    Component::Bind();
    GameObject::Bind();
    TransformComponent::Bind();
    CameraComponent::Bind();
    LightComponent::Bind();
    MaterialRenderer::Bind();
    MeshRenderer::Bind();
    SkyBoxComponent::Bind();
    SceneSystem::Scene::Bind();
}
} // namespace Engine
} // namespace NLS