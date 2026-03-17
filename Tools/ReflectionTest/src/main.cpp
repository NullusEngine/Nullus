#include "Assembly.h"
#include "Core/AssemblyCore.h"
#include "Engine/AssemblyEngine.h"
#include "Math/AssemblyMath.h"
#include "Reflection/Field.h"
#include "Reflection/Method.h"
#include "Reflection/ReflectionDatabase.h"
#include "Reflection/Type.h"

#include <iostream>
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
    std::vector<std::string> requiredStaticMethods;
    std::vector<std::string> requiredFields;
    std::string requiredBase;
};

bool Require(bool condition, const std::string& message)
{
    if (!condition)
    {
        std::cerr << "[FAIL] " << message << std::endl;
        return false;
    }

    return true;
}

bool RequireValidType(const TypeExpectation& expectation)
{
    const Type type = Type::GetFromName(expectation.name);
    if (!Require(type.IsValid(), "Type was not registered: " + expectation.name))
        return false;

    if (!expectation.requiredBase.empty())
    {
        const Type baseType = Type::GetFromName(expectation.requiredBase);
        if (!Require(baseType.IsValid(), "Required base type was not registered: " + expectation.requiredBase))
            return false;
        if (!Require(type.DerivesFrom(baseType), expectation.name + " does not derive from " + expectation.requiredBase))
            return false;
    }

    for (const std::string& fieldName : expectation.requiredFields)
    {
        const Field& field = type.GetField(fieldName);
        if (!Require(field.IsValid(), expectation.name + " is missing reflected field " + fieldName))
            return false;
    }

    for (const std::string& methodName : expectation.requiredMethods)
    {
        const Method& method = type.GetMethod(methodName);
        if (!Require(method.IsValid(), expectation.name + " is missing reflected method " + methodName))
            return false;
    }

    for (const std::string& methodName : expectation.requiredStaticMethods)
    {
        const auto& method = type.GetStaticMethod(methodName);
        if (!Require(method.IsValid(), expectation.name + " is missing reflected static method " + methodName))
            return false;
    }

    std::cout << "[PASS] " << expectation.name << std::endl;
    return true;
}
} // namespace

#if 0
int main()
{
    std::cout << "=== Reflection System Test ===" << std::endl;

    // 触发数据库单例构造（构造中会调用 MetaParser 生成代码的注册函数）
    auto& db = NLS::meta::ReflectionDatabase::Instance();
    (void)db;

    const auto testObjectType = NLS::meta::Type::GetFromName("NLS::meta::TestObject");
    if (!testObjectType.IsValid())
    {
        std::cerr << "[FAIL] Type NLS::meta::TestObject was not registered." << std::endl;
        return 1;
    }

    const auto sampleType = NLS::meta::Type::GetFromName("NLS::meta::ReflectionObjectSample");
    if (!sampleType.IsValid())
    {
        std::cerr << "[FAIL] Type NLS::meta::ReflectionObjectSample was not registered." << std::endl;
        return 1;
    }

    std::cout << "✓ Reflected type found: " << testObjectType.GetName() << std::endl;
    std::cout << "✓ Reflected type found: " << sampleType.GetName() << std::endl;
    std::cout << "=== All tests passed! ===" << std::endl;

    return 0;
}
#endif

int main()
{
    std::cout << "=== Reflection Registration Test ===" << std::endl;

    auto& assembly = NLS::Assembly::Instance();
    assembly.Load<NLS::AssemblyCore>();
    assembly.Load<NLS::AssemblyMath>();
    assembly.Load<NLS::Engine::AssemblyEngine>();

    auto& db = NLS::meta::ReflectionDatabase::Instance();
    (void)db;

    const std::vector<TypeExpectation> expectations = {
        {"NLS::meta::MetaParserFieldMethodSample", {"GetValue", "SetValue"}, {}, {"Value"}, ""},
        {"NLS::meta::PrivateReflectionExternalSample", {"GetHiddenValue"}, {}, {"m_hiddenValue"}, ""},
        {"NLS::meta::MetaProperty", {}, {}, {}, ""},
        {"NLS::meta::ReflectionObjectSample", {"OnSerialize"}, {}, {}, ""},
        {"NLS::meta::TestObject", {"OnSerialize", "OnDeserialize"}, {}, {}, ""},
        {"NLS::Maths::Vector3", {"Length", "Normalised"}, {"Dot", "Cross"}, {"x", "y", "z"}, ""},
        {"NLS::Engine::Components::Component", {"CreateBy"}, {}, {}, ""},
        {"NLS::Engine::Components::TransformComponent", {"SetLocalPosition", "GetWorldMatrix"}, {}, {"localPosition", "localRotation", "localScale"}, "NLS::Engine::Components::Component"},
        {"NLS::Engine::Components::CameraComponent", {"SetFov", "GetCamera"}, {}, {"fov", "size", "near", "far", "clearColor", "frustumGeometryCulling", "frustumLightCulling", "projectionMode"}, "NLS::Engine::Components::Component"},
        {"NLS::Engine::Components::LightComponent", {"SetIntensity", "GetData"}, {}, {"lightType", "color", "intensity", "constant", "linear", "quadratic", "cutoff", "outerCutoff", "radius", "size"}, "NLS::Engine::Components::Component"},
        {"NLS::Engine::Components::MeshRenderer", {"SetModel", "GetModel"}, {}, {"model", "frustumBehaviour", "customBoundingSphere"}, "NLS::Engine::Components::Component"},
        {"NLS::Engine::Components::MaterialRenderer", {"FillWithMaterial", "GetUserMatrix"}, {}, {"materials", "userMatrix"}, "NLS::Engine::Components::Component"},
        {"NLS::Engine::Components::SkyBoxComponent", {"SetCubeMap", "GetModel"}, {}, {}, "NLS::Engine::Components::Component"},
        {"NLS::Engine::GameObject", {"GetName", "SetTag"}, {}, {"name", "tag", "active", "worldID"}, ""},
        {"NLS::Engine::SceneSystem::Scene", {"Play", "GetActors"}, {}, {}, ""},
    };

    bool allPassed = true;
    for (const TypeExpectation& expectation : expectations)
        allPassed = RequireValidType(expectation) && allPassed;

    if (!allPassed)
    {
        std::cerr << "=== Reflection tests failed ===" << std::endl;
        return 1;
    }

    std::cout << "=== All reflection tests passed ===" << std::endl;
    return 0;
}
