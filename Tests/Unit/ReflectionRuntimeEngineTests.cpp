#include "ReflectionRuntimeTestFixture.h"

#include <string>
#include <vector>

namespace
{
using NLS::meta::Type;
using NLS::Tests::Reflection::ExpectEnumKeys;
using NLS::Tests::Reflection::ExpectFieldTypeName;
using NLS::Tests::Reflection::ExpectReflectedType;
using NLS::Tests::Reflection::TypeExpectation;
}

TEST_F(ReflectionRuntimeTestFixture, RegistersEngineReflectionTypes)
{
    const std::vector<TypeExpectation> expectations = {
        {"NLS::Engine::Components::Component", {"CreateBy"}, {}, {}, ""},
        {"NLS::Engine::Components::TransformComponent", {"SetLocalPosition", "GetWorldMatrix"}, {}, {"localPosition", "localRotation", "localScale"}, "NLS::Engine::Components::Component"},
        {"NLS::Engine::Components::CameraComponent", {"SetFov", "GetCamera"}, {}, {"fov", "size", "near", "far", "clearColor", "frustumGeometryCulling", "frustumLightCulling", "projectionMode"}, "NLS::Engine::Components::Component"},
        {"NLS::Engine::Components::LightComponent", {"SetIntensity", "GetData"}, {}, {"lightType", "color", "intensity", "constant", "linear", "quadratic", "cutoff", "outerCutoff", "radius", "size"}, "NLS::Engine::Components::Component"},
        {"NLS::Engine::Components::MeshRenderer", {"SetModel", "GetModel"}, {}, {"model", "frustumBehaviour", "customBoundingSphere"}, "NLS::Engine::Components::Component"},
        {"NLS::Engine::Components::MaterialRenderer", {"FillWithMaterial", "GetUserMatrix"}, {}, {"materialPaths", "userMatrixValues"}, "NLS::Engine::Components::Component"},
        {"NLS::Engine::Components::SkyBoxComponent", {"SetCubeMap", "GetModel"}, {}, {}, "NLS::Engine::Components::Component"},
        {"NLS::Engine::GameObject", {"GetName", "SetTag"}, {}, {"name", "tag", "active", "worldID"}, ""},
        {"NLS::Engine::SceneSystem::Scene", {"Play", "GetActors"}, {}, {}, ""},
    };

    for (const TypeExpectation& expectation : expectations)
        ExpectReflectedType(expectation);
}

TEST_F(ReflectionRuntimeTestFixture, RegistersSpecialCasePropertyBindingsWithExpectedTypes)
{
    const Type cameraType = Type::GetFromName("NLS::Engine::Components::CameraComponent");
    const Type lightType = Type::GetFromName("NLS::Engine::Components::LightComponent");
    const Type meshRendererType = Type::GetFromName("NLS::Engine::Components::MeshRenderer");
    const Type materialRendererType = Type::GetFromName("NLS::Engine::Components::MaterialRenderer");
    const Type gameObjectType = Type::GetFromName("NLS::Engine::GameObject");
    const Type projectionModeType = Type::GetFromName("NLS::Render::Settings::EProjectionMode");
    const Type meshFrustumEnumType = Type::GetFromName("NLS::Engine::Components::MeshRenderer::EFrustumBehaviour");

    ASSERT_TRUE(cameraType.IsValid());
    ASSERT_TRUE(lightType.IsValid());
    ASSERT_TRUE(meshRendererType.IsValid());
    ASSERT_TRUE(materialRendererType.IsValid());
    ASSERT_TRUE(gameObjectType.IsValid());
    ASSERT_TRUE(projectionModeType.IsValid());
    ASSERT_TRUE(meshFrustumEnumType.IsValid());

    ExpectFieldTypeName(cameraType, "projectionMode", "NLS::Render::Settings::EProjectionMode");
    ExpectFieldTypeName(cameraType, "frustumGeometryCulling", "bool");
    ExpectFieldTypeName(cameraType, "frustumLightCulling", "bool");
    ExpectFieldTypeName(lightType, "lightType", "NLS::Render::Settings::ELightType");
    ExpectFieldTypeName(meshRendererType, "model", "std::string");
    ExpectFieldTypeName(meshRendererType, "frustumBehaviour", "NLS::Engine::Components::MeshRenderer::EFrustumBehaviour");
    ExpectFieldTypeName(meshRendererType, "customBoundingSphere", "NLS::Render::Geometry::BoundingSphere");
    ExpectFieldTypeName(materialRendererType, "materialPaths", "NLS::Array<std::string>");
    ExpectFieldTypeName(materialRendererType, "userMatrixValues", "NLS::Array<float>");
    ExpectFieldTypeName(gameObjectType, "active", "bool");

    ExpectEnumKeys(projectionModeType, {"ORTHOGRAPHIC", "PERSPECTIVE"});
    ExpectEnumKeys(meshFrustumEnumType, {"CULL_CUSTOM"});
}
