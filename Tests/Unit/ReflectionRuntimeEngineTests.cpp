#include "ReflectionRuntimeTestFixture.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "Components/LightComponent.h"
#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "GameObject.h"
#include "Reflection/Object.h"

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
        {"NLS::Engine::Components::MeshFilter", {}, {}, {"mesh"}, "NLS::Engine::Components::Component"},
        {"NLS::Engine::Components::MeshRenderer", {"FillWithMaterial", "GetUserMatrix"}, {}, {"frustumBehaviour", "customBoundingSphere", "materials", "userMatrixValues"}, "NLS::Engine::Components::Component"},
        {"NLS::Engine::Components::SkyBoxComponent", {"SetCubeMap", "GetMesh"}, {}, {}, "NLS::Engine::Components::Component"},
        {"NLS::Engine::GameObject", {"GetName", "SetTag"}, {}, {"active", "name", "tag"}, "NLS::Object"},
        {"NLS::Engine::SceneSystem::Scene", {"Play", "GetGameObjects"}, {}, {}, "NLS::Object"},
    };

    for (const TypeExpectation& expectation : expectations)
        ExpectReflectedType(expectation);
}

TEST_F(ReflectionRuntimeTestFixture, RegistersSpecialCasePropertyBindingsWithExpectedTypes)
{
    const Type cameraType = Type::GetFromName("NLS::Engine::Components::CameraComponent");
    const Type lightType = Type::GetFromName("NLS::Engine::Components::LightComponent");
    const Type meshFilterType = Type::GetFromName("NLS::Engine::Components::MeshFilter");
    const Type meshRendererType = Type::GetFromName("NLS::Engine::Components::MeshRenderer");
    const Type projectionModeType = Type::GetFromName("NLS::Render::Settings::EProjectionMode");
    const Type meshFrustumEnumType = Type::GetFromName("NLS::Engine::Components::MeshRenderer::EFrustumBehaviour");

    ASSERT_TRUE(cameraType.IsValid());
    ASSERT_TRUE(lightType.IsValid());
    ASSERT_TRUE(meshFilterType.IsValid());
    ASSERT_TRUE(meshRendererType.IsValid());
    ASSERT_TRUE(projectionModeType.IsValid());
    ASSERT_TRUE(meshFrustumEnumType.IsValid());
    EXPECT_FALSE(Type::GetFromName("NLS::Engine::Components::MaterialRenderer").IsValid());

    ExpectFieldTypeName(cameraType, "projectionMode", "NLS::Render::Settings::EProjectionMode");
    ExpectFieldTypeName(cameraType, "frustumGeometryCulling", "bool");
    ExpectFieldTypeName(cameraType, "frustumLightCulling", "bool");
    ExpectFieldTypeName(lightType, "lightType", "NLS::Render::Settings::ELightType");
    ExpectFieldTypeName(meshFilterType, "mesh", "NLS::Engine::Serialize::PPtr<NLS::Render::Resources::Mesh>");
    EXPECT_FALSE(meshRendererType.GetField("mesh").IsValid());
    ExpectFieldTypeName(meshRendererType, "frustumBehaviour", "NLS::Engine::Components::MeshRenderer::EFrustumBehaviour");
    ExpectFieldTypeName(meshRendererType, "customBoundingSphere", "NLS::Render::Geometry::BoundingSphere");
    ExpectFieldTypeName(meshRendererType, "materials", "NLS::Array<NLS::Engine::Serialize::PPtr<NLS::Render::Resources::Material>>");
    ExpectFieldTypeName(meshRendererType, "userMatrixValues", "NLS::Array<float>");
    ExpectEnumKeys(projectionModeType, {"ORTHOGRAPHIC", "PERSPECTIVE"});
    ExpectEnumKeys(meshFrustumEnumType, {"CULL_CUSTOM"});
}

TEST_F(ReflectionRuntimeTestFixture, PointerTypesDecayToTheirReflectedObjectTypes)
{
    const Type lightPointerType = NLS_TYPEOF(NLS::Engine::Components::LightComponent*);
    const Type constLightPointerType = NLS_TYPEOF(const NLS::Engine::Components::LightComponent*);
    const Type lightType = Type::GetFromName("NLS::Engine::Components::LightComponent");
    const Type componentPointerType = NLS_TYPEOF(NLS::Engine::Components::Component*);
    const Type componentType = Type::GetFromName("NLS::Engine::Components::Component");
    const Type lightTypeOf = NLS_TYPEOF(NLS::Engine::Components::LightComponent);

    ASSERT_TRUE(lightPointerType.IsValid());
    ASSERT_TRUE(constLightPointerType.IsValid());
    ASSERT_TRUE(lightType.IsValid());
    EXPECT_EQ(lightTypeOf.GetName(), lightType.GetName());
    EXPECT_EQ(lightTypeOf.GetID(), lightType.GetID());
    ASSERT_TRUE(componentPointerType.IsValid());
    ASSERT_TRUE(componentType.IsValid());
    EXPECT_TRUE(lightPointerType.IsPointer());
    EXPECT_EQ(lightPointerType.GetDecayedType(), lightType);
    EXPECT_EQ(constLightPointerType.GetDecayedType(), lightType);
    EXPECT_EQ(componentPointerType.GetDecayedType(), componentType);
    EXPECT_EQ(lightType.GetDecayedType(), lightType);
}

TEST_F(ReflectionRuntimeTestFixture, ReflectedEngineObjectInstancesReportGeneratedRuntimeTypeNames)
{
    NLS::Engine::GameObject owner("Owner");
    auto* light = owner.AddComponent<NLS::Engine::Components::LightComponent>();
    ASSERT_NE(light, nullptr);

    EXPECT_STREQ(owner.GetObjectTypeName(), "NLS::Engine::GameObject");
    ASSERT_NE(owner.GetTransform(), nullptr);
    EXPECT_STREQ(owner.GetTransform()->GetObjectTypeName(), "NLS::Engine::Components::TransformComponent");
    EXPECT_STREQ(light->GetObjectTypeName(), "NLS::Engine::Components::LightComponent");
    EXPECT_EQ(light->GetType(), Type::GetFromName("NLS::Engine::Components::LightComponent"));
}

TEST_F(ReflectionRuntimeTestFixture, MeshRendererCopyKeepsSerializableRendererStateWithoutMeshFilterAssetState)
{
    using MeshRenderer = NLS::Engine::Components::MeshRenderer;

    MeshRenderer source;
    NLS::Render::Resources::Material resolvedMaterial;
    source.SetMaterialAtIndex(0u, resolvedMaterial);
    source.SetFrustumBehaviour(MeshRenderer::EFrustumBehaviour::CULL_CUSTOM);
    NLS::Render::Geometry::BoundingSphere customBounds;
    customBounds.position = { 1.0f, 2.0f, 3.0f };
    customBounds.radius = 4.0f;
    source.SetCustomBoundingSphere(customBounds);

    MeshRenderer clone(source);

    EXPECT_EQ(clone.GetFrustumBehaviour(), MeshRenderer::EFrustumBehaviour::CULL_CUSTOM);
    EXPECT_EQ(clone.GetCustomBoundingSphere().position.x, 1.0f);
    EXPECT_EQ(clone.GetCustomBoundingSphere().position.y, 2.0f);
    EXPECT_EQ(clone.GetCustomBoundingSphere().position.z, 3.0f);
    EXPECT_EQ(clone.GetCustomBoundingSphere().radius, 4.0f);
    EXPECT_EQ(clone.GetMaterialAtIndex(0u), nullptr);

    const auto sourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Components/MeshRenderer.cpp";
    std::ifstream stream(sourcePath, std::ios::binary);
    const std::string sourceText{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};
    ASSERT_FALSE(sourceText.empty());
    const auto copyBegin = sourceText.find("MeshRenderer::MeshRenderer(const MeshRenderer& other)");
    ASSERT_NE(copyBegin, std::string::npos);
    const auto copyEnd = sourceText.find("MeshRenderer& MeshRenderer::operator=", copyBegin);
    ASSERT_NE(copyEnd, std::string::npos);
    const auto copyCode = sourceText.substr(copyBegin, copyEnd - copyBegin);
    EXPECT_EQ(copyCode.find("m_failedModelPath(other.m_failedModelPath)"), std::string::npos);
    EXPECT_EQ(copyCode.find("m_modelPath(other.m_modelPath)"), std::string::npos);
}

TEST_F(ReflectionRuntimeTestFixture, MeshRendererAssignmentPreservesExistingComponentOwnerAndLifecycleState)
{
    using MeshRenderer = NLS::Engine::Components::MeshRenderer;

    NLS::Engine::GameObject owner("Owner");
    auto* destination = owner.AddComponent<MeshRenderer>();
    ASSERT_NE(destination, nullptr);
    ASSERT_EQ(destination->gameobject(), &owner);

    MeshRenderer source;
    NLS::Render::Resources::Material resolvedMaterial;
    source.SetMaterialAtIndex(0u, resolvedMaterial);
    source.SetFrustumBehaviour(MeshRenderer::EFrustumBehaviour::CULL_CUSTOM);
    source.DestroyFromOwner();

    *destination = source;

    EXPECT_EQ(destination->gameobject(), &owner);
    EXPECT_EQ(destination->GetFrustumBehaviour(), MeshRenderer::EFrustumBehaviour::CULL_CUSTOM);
    EXPECT_EQ(destination->GetMaterialAtIndex(0u), nullptr);
    destination->DestroyFromOwner();
    EXPECT_EQ(destination->gameobject(), nullptr);
}

TEST_F(ReflectionRuntimeTestFixture, MeshRendererMaterialSlotAccessIgnoresOutOfRangeIndices)
{
    using MeshRenderer = NLS::Engine::Components::MeshRenderer;

    MeshRenderer renderer;
    NLS::Render::Resources::Material material;

    EXPECT_NO_THROW(renderer.SetMaterialAtIndex(MeshRenderer::kMaxMaterialCount, material));
    EXPECT_NO_THROW(renderer.RemoveMaterialAtIndex(MeshRenderer::kMaxMaterialCount));
    EXPECT_EQ(renderer.GetMaterialAtIndex(MeshRenderer::kMaxMaterialCount), nullptr);
}

TEST_F(ReflectionRuntimeTestFixture, RemoveComponentNotifiesRemovedComponentBeforeStorageIsReleased)
{
    NLS::Engine::GameObject owner("Owner");
    auto* light = owner.AddComponent<NLS::Engine::Components::LightComponent>();
    ASSERT_NE(light, nullptr);

    NLS::Engine::Components::Component* removedComponent = nullptr;
    bool removedTypeWasReadable = false;
    const auto listener = owner.ComponentRemovedEvent += [&](NLS::Engine::Components::Component* component)
    {
        removedComponent = component;
        removedTypeWasReadable = component && component->GetType().IsValid();
    };

    EXPECT_TRUE(owner.RemoveComponent(light));
    owner.ComponentRemovedEvent -= listener;

    EXPECT_EQ(removedComponent, light);
    EXPECT_TRUE(removedTypeWasReadable);
    EXPECT_EQ(owner.GetComponent<NLS::Engine::Components::LightComponent>(), nullptr);
}

TEST_F(ReflectionRuntimeTestFixture, GameObjectFindsConcreteComponentsByReflectedType)
{
    NLS::Engine::GameObject owner("Owner");
    auto* light = owner.AddComponent<NLS::Engine::Components::LightComponent>();
    ASSERT_NE(light, nullptr);

    const auto lightType = Type::GetFromName("NLS::Engine::Components::LightComponent");
    ASSERT_TRUE(lightType.IsValid());
    EXPECT_EQ(owner.GetComponent(lightType, true), light);
    EXPECT_NE(owner.GetComponent(NLS_TYPEOF(NLS::Engine::Components::Component), true), nullptr);
    EXPECT_EQ(owner.GetComponent(NLS_TYPEOF(NLS::Engine::Components::Component), false), nullptr);
    EXPECT_EQ(owner.GetComponent<NLS::Engine::Components::Component>(false), nullptr);
}

TEST_F(ReflectionRuntimeTestFixture, GameObjectReflectedComponentLookupDoesNotUseAHandWrittenComponentCastTable)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto sourcePaths = {
        root / "Runtime/Engine/GameObject.cpp",
        root / "Runtime/Engine/GameObject.inl"
    };

    for (const auto& sourcePath : sourcePaths)
    {
        std::ifstream stream(sourcePath, std::ios::binary);
        const std::string sourceText{
            std::istreambuf_iterator<char>(stream),
            std::istreambuf_iterator<char>()};
        ASSERT_FALSE(sourceText.empty()) << sourcePath.string();

        EXPECT_EQ(sourceText.find("CastKnownComponent"), std::string::npos) << sourcePath.string();
        EXPECT_EQ(sourceText.find("CastComponentByReflectedType"), std::string::npos) << sourcePath.string();
        EXPECT_EQ(sourceText.find("typeid(*component)"), std::string::npos) << sourcePath.string();
        EXPECT_EQ(sourceText.find("dynamic_cast<"), std::string::npos) << sourcePath.string();
    }
}
