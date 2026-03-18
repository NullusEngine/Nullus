#include "Assembly.h"
#include "Core/AssemblyCore.h"
#include "Engine/AssemblyEngine.h"
#include "Rendering/AssemblyRender.h"
#include "Reflection/Field.h"
#include "Reflection/Method.h"
#include "Reflection/ReflectionDatabase.h"
#include "Reflection/Type.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace
{
using NLS::meta::Field;
using NLS::meta::Method;
using NLS::meta::Type;

struct TypeExpectation
{
    std::string name;
    std::vector<std::string> requiredMethods;
    std::vector<std::string> requiredFields;
    std::string requiredBase;
};

class ReflectionRuntimeTests : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        auto& assembly = NLS::Assembly::Instance();
        assembly.Load<NLS::AssemblyCore>();
        assembly.Load<NLS::AssemblyRender>();
        assembly.Load<NLS::Engine::AssemblyEngine>();

        auto& db = NLS::meta::ReflectionDatabase::Instance();
        (void)db;
    }
};

void ExpectReflectedType(const TypeExpectation& expectation)
{
    const Type type = Type::GetFromName(expectation.name);
    ASSERT_TRUE(type.IsValid()) << "Type was not registered: " << expectation.name;

    if (!expectation.requiredBase.empty())
    {
        const Type baseType = Type::GetFromName(expectation.requiredBase);
        ASSERT_TRUE(baseType.IsValid()) << "Required base type missing: " << expectation.requiredBase;
        EXPECT_TRUE(type.DerivesFrom(baseType))
            << expectation.name << " does not derive from " << expectation.requiredBase;
    }

    for (const std::string& fieldName : expectation.requiredFields)
    {
        const Field& field = type.GetField(fieldName);
        EXPECT_TRUE(field.IsValid())
            << expectation.name << " is missing reflected field " << fieldName;
    }

    for (const std::string& methodName : expectation.requiredMethods)
    {
        const Method& method = type.GetMethod(methodName);
        EXPECT_TRUE(method.IsValid())
            << expectation.name << " is missing reflected method " << methodName;
    }
}

void ExpectFieldTypeName(const Type& type, const std::string& fieldName, const std::string& expectedTypeName)
{
    const Field& field = type.GetField(fieldName);
    ASSERT_TRUE(field.IsValid()) << type.GetName() << " is missing reflected field " << fieldName;
    EXPECT_EQ(field.GetType().GetName(), expectedTypeName)
        << type.GetName() << "." << fieldName << " has unexpected reflected type";
}
} // namespace

TEST_F(ReflectionRuntimeTests, RegistersBaseReflectionTypes)
{
    const std::vector<TypeExpectation> expectations = {
        {"NLS::meta::MetaParserFieldMethodSample", {"GetValue", "SetValue"}, {"value"}, ""},
        {"NLS::meta::MetaProperty", {}, {}, ""},
        {"NLS::meta::ReflectionObjectSample", {"OnSerialize"}, {}, ""},
        {"NLS::meta::TestObject", {"OnSerialize", "OnDeserialize"}, {}, ""},
    };

    for (const TypeExpectation& expectation : expectations)
        ExpectReflectedType(expectation);
}

TEST_F(ReflectionRuntimeTests, RegistersRenderEnumAndStructReflectionTypes)
{
    const std::vector<TypeExpectation> expectations = {
        {"NLS::Render::Settings::EProjectionMode", {}, {}, ""},
        {"NLS::Render::Settings::ELightType", {}, {}, ""},
        {"NLS::Render::Geometry::BoundingSphere", {}, {"position", "radius"}, ""},
        {"NLS::Engine::Components::MeshRenderer::EFrustumBehaviour", {}, {}, ""}
    };

    for (const TypeExpectation& expectation : expectations)
        ExpectReflectedType(expectation);
}

TEST_F(ReflectionRuntimeTests, RegistersEngineReflectionTypes)
{
    const std::vector<TypeExpectation> expectations = {
        {"NLS::Engine::Components::Component", {"CreateBy"}, {}, ""},
        {"NLS::Engine::Components::TransformComponent", {"SetLocalPosition", "GetWorldMatrix"}, {"localPosition", "localRotation", "localScale"}, "NLS::Engine::Components::Component"},
        {"NLS::Engine::Components::CameraComponent", {"SetFov", "GetCamera"}, {"fov", "size", "near", "far", "clearColor", "frustumGeometryCulling", "frustumLightCulling", "projectionMode"}, "NLS::Engine::Components::Component"},
        {"NLS::Engine::Components::LightComponent", {"SetIntensity", "GetData"}, {"lightType", "color", "intensity", "constant", "linear", "quadratic", "cutoff", "outerCutoff", "radius", "size"}, "NLS::Engine::Components::Component"},
        {"NLS::Engine::Components::MeshRenderer", {"SetModel", "GetModel"}, {"model", "frustumBehaviour", "customBoundingSphere"}, "NLS::Engine::Components::Component"},
        {"NLS::Engine::Components::MaterialRenderer", {"FillWithMaterial", "GetUserMatrix"}, {"materials", "userMatrix"}, "NLS::Engine::Components::Component"},
        {"NLS::Engine::Components::SkyBoxComponent", {"SetCubeMap", "GetModel"}, {}, "NLS::Engine::Components::Component"},
        {"NLS::Engine::GameObject", {"GetName", "SetTag"}, {"name", "tag", "active", "worldID"}, ""},
        {"NLS::Engine::SceneSystem::Scene", {"Play", "GetActors"}, {}, ""},
    };

    for (const TypeExpectation& expectation : expectations)
        ExpectReflectedType(expectation);
}

TEST_F(ReflectionRuntimeTests, RegistersSpecialCasePropertyBindingsWithExpectedTypes)
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
    ExpectFieldTypeName(materialRendererType, "materials", "Array<std::string>");
    ExpectFieldTypeName(materialRendererType, "userMatrix", "Array<float>");
    ExpectFieldTypeName(gameObjectType, "active", "bool");

    const auto projectionModeKeys = projectionModeType.GetEnum().GetKeys();
    EXPECT_NE(std::find(projectionModeKeys.begin(), projectionModeKeys.end(), "ORTHOGRAPHIC"), projectionModeKeys.end());
    EXPECT_NE(std::find(projectionModeKeys.begin(), projectionModeKeys.end(), "PERSPECTIVE"), projectionModeKeys.end());

    const auto meshFrustumKeys = meshFrustumEnumType.GetEnum().GetKeys();
    EXPECT_NE(std::find(meshFrustumKeys.begin(), meshFrustumKeys.end(), "CULL_CUSTOM"), meshFrustumKeys.end());
}
