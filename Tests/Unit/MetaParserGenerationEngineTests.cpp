#include "ReflectionTestUtils.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace
{
using NLS::Tests::Reflection::ExpectContains;
using NLS::Tests::Reflection::ExpectNotContains;
using NLS::Tests::Reflection::ReadAllText;
}

TEST(MetaParserGenerationEngineTests, GeneratesExpectedEngineReflectionBindings)
{
    const std::filesystem::path componentSource = std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Gen/Components/Component.generated.cpp";
    const std::filesystem::path transformSource = std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Gen/Components/TransformComponent.generated.cpp";
    const std::filesystem::path cameraSource = std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Gen/Components/CameraComponent.generated.cpp";
    const std::filesystem::path lightSource = std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Gen/Components/LightComponent.generated.cpp";
    const std::filesystem::path materialSource = std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Gen/Components/MaterialRenderer.generated.cpp";
    const std::filesystem::path meshSource = std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Gen/Components/MeshRenderer.generated.cpp";
    const std::filesystem::path gameObjectSource = std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Gen/GameObject.generated.cpp";
    const std::filesystem::path sceneSource = std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Gen/SceneSystem/Scene.generated.cpp";
    const std::filesystem::path engineMetaSource = std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Gen/MetaGenerated.cpp";
    const std::string componentText = ReadAllText(componentSource);
    const std::string transformText = ReadAllText(transformSource);
    const std::string cameraText = ReadAllText(cameraSource);
    const std::string lightText = ReadAllText(lightSource);
    const std::string materialText = ReadAllText(materialSource);
    const std::string meshText = ReadAllText(meshSource);
    const std::string gameObjectText = ReadAllText(gameObjectSource);
    const std::string sceneText = ReadAllText(sceneSource);
    const std::string metaText = ReadAllText(engineMetaSource);

    ExpectContains(componentText, "AllocateType(typeKey, \"NLS::Engine::Components::Component\", moduleKey)");
    ExpectContains(componentText, "AddMethod(\"CreateBy\", static_cast<void (NLS::Engine::Components::Component::*)(NLS::Engine::GameObject*)>(&NLS::Engine::Components::Component::CreateBy), {})");
    ExpectContains(transformText, "AddField<NLS::Engine::Components::TransformComponent, NLS::Maths::Vector3>(\"localPosition\"");
    ExpectContains(cameraText, "AddField<NLS::Engine::Components::CameraComponent, float>(\"fov\"");
    ExpectContains(cameraText, "AddField<NLS::Engine::Components::CameraComponent, bool>(\"frustumGeometryCulling\", static_cast<bool (NLS::Engine::Components::CameraComponent::*)() const>(&NLS::Engine::Components::CameraComponent::HasFrustumGeometryCulling), static_cast<void (NLS::Engine::Components::CameraComponent::*)(bool)>(&NLS::Engine::Components::CameraComponent::SetFrustumGeometryCulling), {})");
    ExpectContains(cameraText, "AddField<NLS::Engine::Components::CameraComponent, NLS::Render::Settings::EProjectionMode>(\"projectionMode\"");
    ExpectContains(lightText, "AddField<NLS::Engine::Components::LightComponent, float>(\"intensity\"");
    ExpectContains(lightText, "AddField<NLS::Engine::Components::LightComponent, NLS::Render::Settings::ELightType>(\"lightType\"");
    ExpectContains(materialText, "AddField<NLS::Engine::Components::MaterialRenderer, NLS::Array<std::string>>(\"materialPaths\"");
    ExpectContains(materialText, "AddField<NLS::Engine::Components::MaterialRenderer, NLS::Array<float>>(\"userMatrixValues\"");
    ExpectContains(meshText, "AddField<NLS::Engine::Components::MeshRenderer, std::string>(\"model\", static_cast<std::string (NLS::Engine::Components::MeshRenderer::*)() const>(&NLS::Engine::Components::MeshRenderer::GetModelPath), static_cast<void (NLS::Engine::Components::MeshRenderer::*)(const std::string&)>(&NLS::Engine::Components::MeshRenderer::SetModelPath), {})");
    ExpectContains(meshText, "AddField<NLS::Engine::Components::MeshRenderer, NLS::Engine::Components::MeshRenderer::EFrustumBehaviour>(\"frustumBehaviour\"");
    ExpectContains(meshText, "AddField<NLS::Engine::Components::MeshRenderer, NLS::Render::Geometry::BoundingSphere>(\"customBoundingSphere\"");
    ExpectContains(gameObjectText, "AllocateType(typeKey, \"NLS::Engine::GameObject\", moduleKey)");
    ExpectContains(gameObjectText, "type.AddField<NLS::Engine::GameObject, bool>(\"active\"");
    ExpectContains(gameObjectText, "type.AddField<NLS::Engine::GameObject, std::string>(\"name\"");
    ExpectContains(sceneText, "AllocateType(typeKey, \"NLS::Engine::SceneSystem::Scene\", moduleKey)");
    ExpectContains(sceneText, "AddMethod(\"Play\", static_cast<void (NLS::Engine::SceneSystem::Scene::*)()>(&NLS::Engine::SceneSystem::Scene::Play), {})");
    ExpectContains(sceneText, "AddMethod(\"GetActors\", static_cast<const std::vector<NLS::Engine::GameObject*>& (NLS::Engine::SceneSystem::Scene::*)() const>(&NLS::Engine::SceneSystem::Scene::GetActors), {})");
    ExpectContains(metaText, "LinkReflectionTypes_NLS_Engine");
    ExpectNotContains(metaText, "ExternalReflection.generated.cpp");
    ExpectContains(metaText, "#include \"Components/Component.generated.cpp\"");
}
