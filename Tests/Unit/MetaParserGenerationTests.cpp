#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace
{
std::string ReadAllText(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::in | std::ios::binary);
    EXPECT_TRUE(input.is_open()) << "Failed to open " << path.string();
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

void ExpectContains(const std::string& content, const std::string& needle)
{
    EXPECT_NE(content.find(needle), std::string::npos) << "Missing generated fragment: " << needle;
}
} // namespace

TEST(MetaParserGenerationTests, GeneratesModuleSpecificRegistrationEntrypoints)
{
    const std::filesystem::path baseHeader = std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Base/Gen/MetaGenerated.h";
    const std::filesystem::path engineHeader = std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Gen/MetaGenerated.h";

    const std::string baseText = ReadAllText(baseHeader);
    const std::string engineText = ReadAllText(engineHeader);

    ExpectContains(baseText, "LinkReflectionTypes_NLS_Base");
    ExpectContains(baseText, "NLS_META_GENERATED_LINK_FUNCTION");
    ExpectContains(engineText, "LinkReflectionTypes_NLS_Engine");
    ExpectContains(engineText, "NLS_META_GENERATED_LINK_FUNCTION");
}

TEST(MetaParserGenerationTests, GeneratesExpectedBaseReflectionBindings)
{
    const std::filesystem::path sampleSource = std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Base/Gen/Reflection/MetaParserFieldMethodSample.generated.cpp";
    const std::filesystem::path metaSource = std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Base/Gen/MetaGenerated.cpp";
    const std::string sampleText = ReadAllText(sampleSource);
    const std::string metaText = ReadAllText(metaSource);

    ExpectContains(sampleText, "AllocateType(\"NLS::meta::MetaParserFieldMethodSample\")");
    ExpectContains(sampleText, "AddField<NLS::meta::MetaParserFieldMethodSample, int>(\"value\"");
    ExpectContains(sampleText, "&NLS::meta::MetaParserFieldMethodSample::GetValue");
    ExpectContains(sampleText, "&NLS::meta::MetaParserFieldMethodSample::SetValue");
    ExpectContains(sampleText, "AddMethod(\"GetValue\", &NLS::meta::MetaParserFieldMethodSample::GetValue, {})");
    EXPECT_EQ(sampleText.find("AddMethod(\"OnSerialize\""), std::string::npos);
    ExpectContains(metaText, "LinkReflectionTypes_NLS_Base");
    ExpectContains(metaText, "#include \"Reflection/MetaParserFieldMethodSample.generated.cpp\"");
}

TEST(MetaParserGenerationTests, GeneratesExpectedEngineReflectionBindings)
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

    ExpectContains(componentText, "AllocateType(\"NLS::Engine::Components::Component\")");
    ExpectContains(componentText, "AddMethod(\"CreateBy\", &NLS::Engine::Components::Component::CreateBy, {})");
    ExpectContains(transformText, "AddField<NLS::Engine::Components::TransformComponent, NLS::Maths::Vector3>(\"localPosition\"");
    ExpectContains(cameraText, "AddField<NLS::Engine::Components::CameraComponent, float>(\"fov\"");
    ExpectContains(cameraText, "AddField<NLS::Engine::Components::CameraComponent, bool>(\"frustumGeometryCulling\", &NLS::Engine::Components::CameraComponent::HasFrustumGeometryCulling, &NLS::Engine::Components::CameraComponent::SetFrustumGeometryCulling, {})");
    ExpectContains(cameraText, "AddField<NLS::Engine::Components::CameraComponent, NLS::Render::Settings::EProjectionMode>(\"projectionMode\"");
    ExpectContains(lightText, "AddField<NLS::Engine::Components::LightComponent, float>(\"intensity\"");
    ExpectContains(lightText, "AddField<NLS::Engine::Components::LightComponent, NLS::Render::Settings::ELightType>(\"lightType\"");
    EXPECT_EQ(materialText.find("AddField<NLS::Engine::Components::MaterialRenderer, NLS::Array<std::string>>(\"materials\""), std::string::npos);
    EXPECT_EQ(materialText.find("AddField<NLS::Engine::Components::MaterialRenderer, NLS::Array<float>>(\"userMatrix\""), std::string::npos);
    ExpectContains(meshText, "AddField<NLS::Engine::Components::MeshRenderer, std::string>(\"model\", &NLS::Engine::Components::MeshRenderer::GetModelPath, &NLS::Engine::Components::MeshRenderer::SetModelPath, {})");
    ExpectContains(meshText, "AddField<NLS::Engine::Components::MeshRenderer, NLS::Engine::Components::MeshRenderer::EFrustumBehaviour>(\"frustumBehaviour\"");
    ExpectContains(meshText, "AddField<NLS::Engine::Components::MeshRenderer, NLS::Render::Geometry::BoundingSphere>(\"customBoundingSphere\"");
    ExpectContains(gameObjectText, "AllocateType(\"NLS::Engine::GameObject\")");
    ExpectContains(gameObjectText, "type.AddField<NLS::Engine::GameObject, std::string>(\"name\"");
    ExpectContains(gameObjectText, "type.AddField<NLS::Engine::GameObject, bool>(\"active\"");
    ExpectContains(gameObjectText, "&NLS::Engine::GameObject::IsSelfActive");
    ExpectContains(sceneText, "AllocateType(\"NLS::Engine::SceneSystem::Scene\")");
    ExpectContains(sceneText, "AddMethod(\"Play\", &NLS::Engine::SceneSystem::Scene::Play, {})");
    ExpectContains(sceneText, "AddMethod(\"GetActors\", static_cast<const std::vector<NLS::Engine::GameObject*>& (NLS::Engine::SceneSystem::Scene::*)() const>(&NLS::Engine::SceneSystem::Scene::GetActors), {})");
    ExpectContains(metaText, "LinkReflectionTypes_NLS_Engine");
    EXPECT_EQ(metaText.find("ExternalReflection.generated.cpp"), std::string::npos);
    ExpectContains(metaText, "#include \"Components/Component.generated.cpp\"");
}

TEST(MetaParserGenerationTests, GeneratesExpectedRenderEnumAndStructBindings)
{
    const std::filesystem::path projectionModeSource = std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Rendering/Gen/Settings/EProjectionMode.generated.cpp";
    const std::filesystem::path lightTypeSource = std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Rendering/Gen/Settings/ELightType.generated.cpp";
    const std::filesystem::path boundingSphereSource = std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Rendering/Gen/Geometry/BoundingSphere.generated.cpp";
    const std::string projectionModeText = ReadAllText(projectionModeSource);
    const std::string lightTypeText = ReadAllText(lightTypeSource);
    const std::string boundingSphereText = ReadAllText(boundingSphereSource);

    ExpectContains(projectionModeText, "type.SetEnum<NLS::Render::Settings::EProjectionMode>");
    ExpectContains(projectionModeText, "\"PERSPECTIVE\"");
    ExpectContains(lightTypeText, "type.SetEnum<NLS::Render::Settings::ELightType>");
    ExpectContains(lightTypeText, "\"DIRECTIONAL\"");
    ExpectContains(boundingSphereText, "AddField<NLS::Render::Geometry::BoundingSphere, NLS::Maths::Vector3>(\"position\"");
    ExpectContains(boundingSphereText, "AddField<NLS::Render::Geometry::BoundingSphere, float>(\"radius\"");
}
