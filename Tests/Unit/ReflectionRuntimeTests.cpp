#include "Assembly.h"
#include "Core/AssemblyCore.h"
#include "Engine/AssemblyEngine.h"
#include "Reflection/Field.h"
#include "Reflection/Method.h"
#include "Reflection/ReflectionDatabase.h"
#include "Reflection/Type.h"

#include <gtest/gtest.h>

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
} // namespace

TEST_F(ReflectionRuntimeTests, RegistersBaseReflectionTypes)
{
    const std::vector<TypeExpectation> expectations = {
        {"NLS::meta::MetaParserFieldMethodSample", {"GetValue", "SetValue"}, {"Value"}, ""},
        {"NLS::meta::MetaProperty", {}, {}, ""},
        {"NLS::meta::ReflectionObjectSample", {"OnSerialize"}, {}, ""},
        {"NLS::meta::TestObject", {"OnSerialize", "OnDeserialize"}, {}, ""},
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
