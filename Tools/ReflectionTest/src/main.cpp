#include "Assembly.h"
#include "Core/AssemblyCore.h"
#include "Engine/AssemblyEngine.h"
#include "Math/AssemblyMath.h"
#include "Components/CameraComponent.h"
#include "Components/LightComponent.h"
#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "Components/TransformComponent.h"
#include "GameObject.h"
#include "Rendering/AssemblyRender.h"
#include "Rendering/Settings/EProjectionMode.h"
#include "Rendering/Settings/ELightType.h"
#include "Reflection/Field.h"
#include "Reflection/Method.h"
#include "Reflection/ReflectionDiagnostics.h"
#include "Reflection/ReflectionDatabase.h"
#include "Reflection/ReflectionModule.h"
#include "Reflection/Type.h"
#include "../../../Project/Editor/Panels/InspectorReflectionUtils.h"

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

bool RequireInvalidType(const std::string& typeName)
{
    if (!Require(!Type::GetFromName(typeName).IsValid(), "Type should not be registered: " + typeName))
        return false;

    std::cout << "[PASS] " << typeName << " is absent" << std::endl;
    return true;
}

bool RequireTypeDrivenComponentLookup()
{
    using namespace NLS::Engine;
    using namespace NLS::Engine::Components;

    GameObject actor("ReflectionActor", "Test");
    auto* camera = actor.AddComponent<CameraComponent>();
    auto* light = actor.AddComponent<LightComponent>();

    if (!Require(camera != nullptr, "Failed to create reflected CameraComponent for lookup test"))
        return false;
    if (!Require(light != nullptr, "Failed to create reflected LightComponent for lookup test"))
        return false;

    auto* transformByType = actor.GetComponent(NLS_TYPEOF(Component), true);
    if (!Require(transformByType == actor.GetTransform(), "includeSubType=true should return the first component deriving from Component"))
        return false;

    auto* exactBaseLookup = actor.GetComponent(NLS_TYPEOF(Component), false);
    if (!Require(exactBaseLookup == nullptr, "includeSubType=false should not treat derived components as an exact Component match"))
        return false;

    auto* exactBaseTemplateLookup = actor.GetComponent<Component>(false);
    if (!Require(exactBaseTemplateLookup == nullptr, "templated includeSubType=false should not treat derived components as an exact Component match"))
        return false;

    auto* cameraByMetaType = actor.GetComponent(NLS_TYPEOF(CameraComponent), false);
    if (!Require(cameraByMetaType == camera, "meta::Type lookup should return the exact CameraComponent instance"))
        return false;

    auto* lightByMetaType = actor.GetComponent(NLS_TYPEOF(LightComponent), true);
    if (!Require(lightByMetaType == light, "meta::Type lookup should return the exact LightComponent instance"))
        return false;

    std::cout << "[PASS] meta::Type component lookup" << std::endl;
    return true;
}

bool RequireEnumChoiceMetadata()
{
    using namespace NLS::Render::Settings;
    using namespace NLS::Engine::Components;
    using NLS::Editor::Panels::BuildEnumChoices;

    const auto projectionChoices = BuildEnumChoices(Type::GetFromName("NLS::Render::Settings::EProjectionMode"));
    const auto lightChoices = BuildEnumChoices(Type::GetFromName("NLS::Render::Settings::ELightType"));
    const auto frustumChoices = BuildEnumChoices(Type::GetFromName("NLS::Engine::Components::MeshRenderer::EFrustumBehaviour"));

    if (!Require(projectionChoices.contains(static_cast<int>(EProjectionMode::ORTHOGRAPHIC)), "Projection mode enum choices should contain ORTHOGRAPHIC"))
        return false;
    if (!Require(projectionChoices.at(static_cast<int>(EProjectionMode::ORTHOGRAPHIC)) == "Orthographic", "Projection mode enum labels should be humanized from metadata"))
        return false;

    if (!Require(lightChoices.contains(static_cast<int>(ELightType::AMBIENT_SPHERE)), "Light type enum choices should contain AMBIENT_SPHERE"))
        return false;
    if (!Require(lightChoices.at(static_cast<int>(ELightType::AMBIENT_SPHERE)) == "Ambient Sphere", "Light type enum labels should be humanized from metadata"))
        return false;

    if (!Require(frustumChoices.contains(static_cast<int>(MeshRenderer::EFrustumBehaviour::CULL_MESHES)), "Frustum behaviour enum choices should contain CULL_MESHES"))
        return false;
    if (!Require(frustumChoices.at(static_cast<int>(MeshRenderer::EFrustumBehaviour::CULL_MESHES)) == "Cull Meshes", "Frustum behaviour labels should be humanized from metadata"))
        return false;

    std::cout << "[PASS] enum metadata choices" << std::endl;
    return true;
}

bool RequireInspectorPropertySupport()
{
    using namespace NLS;
    using namespace NLS::Maths;

    const Type transformType = Type::GetFromName("NLS::Engine::Components::TransformComponent");
    if (!Require(transformType.IsValid(), "TransformComponent type should be registered for inspector support test"))
        return false;

    if (!Require(transformType.GetField("localPosition").GetType() == NLS_TYPEOF(Vector3),
            "Transform.localPosition should compare equal to NLS_TYPEOF(Vector3) across DLLs"))
        return false;
    if (!Require(transformType.GetField("localRotation").GetType() == NLS_TYPEOF(Quaternion),
            "Transform.localRotation should compare equal to NLS_TYPEOF(Quaternion) across DLLs"))
        return false;
    if (!Require(transformType.GetField("localScale").GetType() == NLS_TYPEOF(Vector3),
            "Transform.localScale should compare equal to NLS_TYPEOF(Vector3) across DLLs"))
        return false;

    std::cout << "[PASS] external math type comparisons" << std::endl;
    return true;
}

bool RequireTypeKeysAreStable()
{
    const Type transformType = Type::GetFromName("NLS::Engine::Components::TransformComponent");
    if (!Require(transformType.IsValid(), "TransformComponent type should be registered for TypeKey test"))
        return false;

    const auto key = transformType.GetKey();
    if (!Require(key != NLS::meta::InvalidTypeKey, "TransformComponent should expose a non-zero 128-bit TypeKey"))
        return false;
    if (!Require(key == NLS::meta::HashTypeKey("NLS::Engine::Components::TransformComponent"), "TransformComponent TypeKey should be stable from the qualified name"))
        return false;

    const auto method = transformType.GetMethod("SetLocalPosition");
    if (!Require(method.IsValid(), "Cached method should be valid while its owner type is alive"))
        return false;

    const auto field = transformType.GetField("localPosition");
    if (!Require(field.IsValid(), "Cached field should be valid while its owner type is alive"))
        return false;

    std::cout << "[PASS] stable 128-bit TypeKey and member validity" << std::endl;
    return true;
}

bool RequireReflectionDiagnosticsAreClean()
{
    const auto& diagnostics = NLS::meta::ReflectionDiagnostics::Get();
    bool clean = true;
    for (const auto& diagnostic : diagnostics)
    {
        if (diagnostic.severity != NLS::meta::ReflectionDiagnosticSeverity::Error)
            continue;

        clean = false;
        std::cerr
            << "[FAIL] Reflection diagnostic error in "
            << diagnostic.moduleName
            << " type=" << diagnostic.typeName
            << " subject=" << diagnostic.subject
            << " message=" << diagnostic.message
            << std::endl;
    }

    if (!Require(clean, "Reflection diagnostics should not contain errors"))
        return false;

    NLS::meta::ReflectionModuleRegistry::Unload(NLS::meta::HashTypeKey("missing-test-module"));
    std::cout << "[PASS] reflection diagnostics and unload API" << std::endl;
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
    assembly.Load<NLS::AssemblyRender>();
    assembly.Load<NLS::Engine::AssemblyEngine>();

    auto& db = NLS::meta::ReflectionDatabase::Instance();
    (void)db;

    const std::vector<TypeExpectation> expectations = {
        // Inline reflected class with field + methods
        {"NLS::meta::MetaParserFieldMethodSample", {"GetValue", "SetValue"}, {}, {"value"}, ""},
        // External private binding
        {"NLS::meta::PrivateReflectionExternalSample", {"GetHiddenValue"}, {}, {"m_hiddenValue"}, ""},
        // External reflected value type with static methods
        {"NLS::Maths::Vector3", {"Length", "Normalised"}, {"Dot", "Cross"}, {"x", "y", "z"}, ""},
        // Reflected enum
        {"NLS::Render::Settings::EProjectionMode", {}, {}, {}, ""},
        // Reflected struct/value type
        {"NLS::Render::Geometry::BoundingSphere", {}, {}, {"position", "radius"}, ""},
        // Core transform component used by the editor inspector
        {"NLS::Engine::Components::TransformComponent", {"SetLocalPosition", "GetWorldMatrix"}, {}, {"localPosition", "localRotation", "localScale"}, "NLS::Engine::Components::Component"},
        // Engine type with explicit property + nested enum/struct-backed fields
        {"NLS::Engine::Components::MeshFilter", {}, {}, {"mesh"}, "NLS::Engine::Components::Component"},
        {"NLS::Engine::Components::MeshRenderer", {"FillWithMaterial", "GetUserMatrix"}, {}, {"frustumBehaviour", "customBoundingSphere", "materials", "userMatrixValues"}, "NLS::Engine::Components::Component"},
        // Engine object with auto-inferred properties
        {"NLS::Engine::GameObject", {"GetName", "SetTag"}, {}, {"name", "tag"}, "NLS::Object"},
        {"NLS::Engine::SceneSystem::Scene", {"Play", "GetGameObjects"}, {}, {}, "NLS::Object"},
    };

    bool allPassed = true;
    for (const TypeExpectation& expectation : expectations)
        allPassed = RequireValidType(expectation) && allPassed;

    allPassed = RequireInvalidType("NLS::Engine::Components::MaterialRenderer") && allPassed;
    allPassed = RequireTypeDrivenComponentLookup() && allPassed;
    allPassed = RequireEnumChoiceMetadata() && allPassed;
    allPassed = RequireInspectorPropertySupport() && allPassed;
    allPassed = RequireTypeKeysAreStable() && allPassed;
    allPassed = RequireReflectionDiagnosticsAreClean() && allPassed;

    if (!allPassed)
    {
        std::cerr << "=== Reflection tests failed ===" << std::endl;
        return 1;
    }

    std::cout << "=== All reflection tests passed ===" << std::endl;
    return 0;
}
