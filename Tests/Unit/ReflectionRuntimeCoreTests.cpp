#include "ReflectionRuntimeTestFixture.h"

#include "Reflection/Object.h"
#include "Reflection/ArrayWrapper.h"
#include "Reflection/ReflectionDiagnostics.h"
#include "Reflection/RuntimeMetaProperties.h"
#include "Reflection/TestObject.h"

#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/Mesh.h"
#include "Rendering/Resources/Shader.h"
#include "Rendering/Resources/Texture.h"
#include "Rendering/Resources/Texture2D.h"
#include "Rendering/Resources/TextureCube.h"

#include <array>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{
using NLS::Tests::Reflection::ExpectReflectedType;
using NLS::Tests::Reflection::TypeExpectation;

class UnreflectedRuntimeObject : public NLS::Object
{
};

struct ReflectedArrayElementFixture
{
    int value = 0;
};

struct NonDefaultArrayElementFixture
{
    explicit NonDefaultArrayElementFixture(int p_value)
        : value(p_value)
    {
    }

    int value = 0;
};
}

TEST_F(ReflectionRuntimeTestFixture, RegistersBaseReflectionTypes)
{
    const std::vector<TypeExpectation> expectations = {
        {"NLS::NamedObject", {}, {}, {}, "NLS::Object"},
        {"NLS::meta::MetaParserFieldMethodSample", {"GetValue", "SetValue"}, {}, {"value"}, ""},
        {"NLS::meta::MetaProperty", {}, {}, {}, ""},
        {"NLS::meta::ReflectionObjectSample", {}, {}, {}, "NLS::Object"},
        {"NLS::meta::TestObject", {}, {}, {}, "NLS::Object"},
    };

    for (const TypeExpectation& expectation : expectations)
        ExpectReflectedType(expectation);
}

TEST_F(ReflectionRuntimeTestFixture, ArrayWrapperOwnsWrapperContainerWithMoveOnlySemantics)
{
    static_assert(!std::is_copy_constructible_v<NLS::meta::ArrayWrapper>);
    static_assert(!std::is_copy_assignable_v<NLS::meta::ArrayWrapper>);
    static_assert(std::is_move_constructible_v<NLS::meta::ArrayWrapper>);
    static_assert(std::is_move_assignable_v<NLS::meta::ArrayWrapper>);

    NLS::Array<int> values = {1, 2};
    auto source = NLS::meta::ArrayWrapper(values);
    auto moved = std::move(source);

    ASSERT_FALSE(source.IsValid());
    ASSERT_TRUE(moved.IsValid());
    EXPECT_EQ(moved.Size(), 2u);
    moved.SetValue(1, 7);
    EXPECT_EQ(values[1], 7);
}

TEST_F(ReflectionRuntimeTestFixture, VectorTypesExposeSequentialArrayTraitsAndWrapperMutation)
{
    static_assert(NLS::meta_traits::IsArray<std::vector<int>>::value);
    static_assert(std::is_same_v<NLS::meta_traits::RemoveArray<std::vector<int>>::type, int>);

    std::vector<ReflectedArrayElementFixture> values {{1}, {2}};
    auto wrapper = NLS::meta::ArrayWrapper(values);

    ASSERT_TRUE(wrapper.IsValid());
    EXPECT_FALSE(wrapper.IsConst());
    EXPECT_TRUE(wrapper.CanSetValue());
    EXPECT_TRUE(wrapper.CanInsert());
    EXPECT_TRUE(wrapper.CanInsertDefault());
    EXPECT_TRUE(wrapper.CanRemove());
    EXPECT_TRUE(wrapper.CanResize());
    EXPECT_EQ(wrapper.Size(), 2u);

    EXPECT_EQ(wrapper.GetValue(0).GetValue<ReflectedArrayElementFixture>().value, 1);

    wrapper.SetValue(1, ReflectedArrayElementFixture {7});
    ASSERT_EQ(values.size(), 2u);
    EXPECT_EQ(values[1].value, 7);

    wrapper.InsertDefault(1);
    ASSERT_EQ(values.size(), 3u);
    EXPECT_EQ(values[1].value, 0);

    wrapper.Insert(2, ReflectedArrayElementFixture {9});
    ASSERT_EQ(values.size(), 4u);
    EXPECT_EQ(values[2].value, 9);

    wrapper.Remove(0);
    ASSERT_EQ(values.size(), 3u);
    EXPECT_EQ(values[0].value, 0);

    wrapper.Resize(5);
    ASSERT_EQ(values.size(), 5u);
    EXPECT_EQ(values[4].value, 0);
}

TEST_F(ReflectionRuntimeTestFixture, VectorVariantNoCopyPreservesArrayViewAndOriginalContainer)
{
    std::vector<int> values {1, 2};
    NLS::meta::Variant variant(values, NLS::meta::variant_policy::NoCopy {});

    ASSERT_TRUE(variant.IsArray());
    EXPECT_TRUE(variant.GetType().IsArray());
    EXPECT_EQ(variant.GetType().GetArrayType(), NLS_TYPEOF(int));

    auto wrapper = variant.GetArray();
    ASSERT_TRUE(wrapper.IsValid());
    ASSERT_EQ(wrapper.Size(), 2u);
    wrapper.SetValue(0, 42);

    EXPECT_EQ(values[0], 42);
}

TEST_F(ReflectionRuntimeTestFixture, ConstVectorVariantExposesReadOnlyArrayWrapper)
{
    const std::vector<int> values {1, 2};
    NLS::meta::Variant variant(values);

    ASSERT_TRUE(variant.IsArray());
    auto wrapper = variant.GetArray();

    ASSERT_TRUE(wrapper.IsValid());
    EXPECT_TRUE(wrapper.IsConst());
    EXPECT_FALSE(wrapper.CanSetValue());
    EXPECT_FALSE(wrapper.CanInsert());
    EXPECT_FALSE(wrapper.CanInsertDefault());
    EXPECT_FALSE(wrapper.CanRemove());
    EXPECT_FALSE(wrapper.CanResize());
    EXPECT_EQ(wrapper.Size(), 2u);
    EXPECT_EQ(wrapper.GetValue(0).GetValue<int>(), 1);
}

TEST_F(ReflectionRuntimeTestFixture, ConstNlsArrayVariantExposesReadOnlyArrayWrapper)
{
    const NLS::Array<int> values {1, 2};
    NLS::meta::Variant variant(values);

    ASSERT_TRUE(variant.IsArray());
    auto wrapper = variant.GetArray();

    ASSERT_TRUE(wrapper.IsValid());
    EXPECT_TRUE(wrapper.IsConst());
    EXPECT_FALSE(wrapper.CanSetValue());
    EXPECT_FALSE(wrapper.CanInsert());
    EXPECT_FALSE(wrapper.CanInsertDefault());
    EXPECT_FALSE(wrapper.CanRemove());
    EXPECT_FALSE(wrapper.CanResize());
    EXPECT_EQ(wrapper.Size(), 2u);
    EXPECT_EQ(wrapper.GetValue(0).GetValue<int>(), 1);
}

TEST_F(ReflectionRuntimeTestFixture, ArrayWrapperReportsUnsupportedDefaultConstructionWithoutInstantiatingInvalidOperations)
{
    std::vector<NonDefaultArrayElementFixture> values;
    values.emplace_back(3);

    auto wrapper = NLS::meta::ArrayWrapper(values);

    ASSERT_TRUE(wrapper.IsValid());
    EXPECT_TRUE(wrapper.CanSetValue());
    EXPECT_TRUE(wrapper.CanInsert());
    EXPECT_FALSE(wrapper.CanInsertDefault());
    EXPECT_TRUE(wrapper.CanRemove());
    EXPECT_FALSE(wrapper.CanResize());

    wrapper.InsertDefault(1);
    wrapper.Resize(3);

    ASSERT_EQ(values.size(), 1u);
    EXPECT_EQ(values[0].value, 3);
}

TEST_F(ReflectionRuntimeTestFixture, ObjectGetTypeResolvesReflectedRuntimeType)
{
    NLS::meta::TestObject object;

    const auto type = object.GetType();

    ASSERT_TRUE(type.IsValid());
    EXPECT_EQ(type.GetName(), "NLS::meta::TestObject");
}

TEST_F(ReflectionRuntimeTestFixture, ObjectVariantUsesRuntimeObjectType)
{
    NLS::meta::TestObject object;
    NLS::Object* basePointer = &object;

    const auto variant = ObjectVariant(basePointer);
    const auto type = variant.GetType();

    ASSERT_TRUE(type.IsValid());
    EXPECT_EQ(type.GetName(), "NLS::meta::TestObject");
}

TEST_F(ReflectionRuntimeTestFixture, ObjectVariantRejectsWrongDerivedPointerRequest)
{
    NLS::meta::TestObject object;
    const auto variant = ObjectVariant(&object);

    auto* wrongType = variant.GetValue<UnreflectedRuntimeObject*>();
    auto* baseType = variant.GetValue<NLS::Object*>();
    const auto* constType = variant.GetValue<const NLS::meta::TestObject*>();

    EXPECT_EQ(wrongType, nullptr);
    EXPECT_EQ(baseType, &object);
    EXPECT_EQ(constType, &object);
}

TEST_F(ReflectionRuntimeTestFixture, ObjectPointerVariantCastsDerivedPointerToBasePointer)
{
    NLS::meta::TestObject object;
    NLS::meta::TestObject* typedPointer = &object;
    const auto variant = NLS::meta::Variant(typedPointer, NLS::meta::variant_policy::NoCopy {});

    auto* baseType = variant.GetValue<NLS::Object*>();
    const auto* constBaseType = variant.GetValue<const NLS::Object*>();

    EXPECT_EQ(baseType, &object);
    EXPECT_EQ(constBaseType, &object);
}

TEST_F(ReflectionRuntimeTestFixture, ObjectVariantKeepsNullObjectPointersNull)
{
    NLS::meta::TestObject* object = nullptr;

    const auto variant = ObjectVariant(object);

    ASSERT_TRUE(variant.GetType().IsValid());
    EXPECT_EQ(variant.GetType().GetName(), "NLS::meta::TestObject");
    EXPECT_EQ(variant.GetValue<NLS::meta::TestObject*>(), nullptr);
}

TEST_F(ReflectionRuntimeTestFixture, ObjectVariantClonePreservesRuntimeType)
{
    NLS::meta::TestObject object;
    NLS::Object* basePointer = &object;

    const auto variant = ObjectVariant(basePointer);
    const NLS::meta::Variant clone = variant;

    ASSERT_TRUE(clone.GetType().IsValid());
    EXPECT_EQ(clone.GetType().GetName(), "NLS::meta::TestObject");
    EXPECT_EQ(clone.GetValue<NLS::Object*>(), &object);
}

TEST_F(ReflectionRuntimeTestFixture, ObjectGetTypeFallsBackToObjectForUnreflectedDerivedTypes)
{
    UnreflectedRuntimeObject object;

    const auto type = object.GetType();

    ASSERT_TRUE(type.IsValid());
    EXPECT_EQ(type.GetName(), "NLS::Object");
}

TEST_F(ReflectionRuntimeTestFixture, MetaManagerAssignmentReplacesExistingProperties)
{
    NLS::meta::MetaManager manager({
        {NLS_TYPEOF(NLS::meta::Range), NLS::meta::MetaPropertyInitializer<NLS::meta::Range>(0.0f, 1.0f)}
    });

    ASSERT_NE(manager.GetProperty<NLS::meta::Range>(), nullptr);

    const NLS::meta::MetaManager empty;
    manager = empty;

    EXPECT_EQ(manager.GetProperty<NLS::meta::Range>(), nullptr);
    EXPECT_TRUE(manager.GetProperties().empty());

    const NLS::meta::MetaManager menu({
        {NLS_TYPEOF(NLS::meta::ComponentMenu), NLS::meta::MetaPropertyInitializer<NLS::meta::ComponentMenu>("Rendering/Test")}
    });
    manager = menu;

    EXPECT_EQ(manager.GetProperty<NLS::meta::Range>(), nullptr);
    const auto* componentMenu = manager.GetProperty<NLS::meta::ComponentMenu>();
    ASSERT_NE(componentMenu, nullptr);
    EXPECT_EQ(componentMenu->path, "Rendering/Test");
    EXPECT_EQ(manager.GetProperties().size(), 1u);
}

TEST_F(ReflectionRuntimeTestFixture, RegistersExternalReflectionAndSerializationTypes)
{
    const std::vector<TypeExpectation> expectations = {
        {"NLS::Guid", {}, {}, {}, ""},
        {"NLS::Maths::Vector3", {"Length", "Normalised"}, {"Dot", "Cross"}, {"x", "y", "z"}, ""},
        {"NLS::Maths::Quaternion", {}, {}, {"x", "y", "z", "w"}, ""},
        {"NLS::Render::Geometry::Bounds", {}, {}, {"center", "size"}, ""},
        {"NLS::Engine::Serialize::FileType", {}, {}, {}, ""},
        {"NLS::Engine::Serialize::ObjectIdentifier", {}, {}, {"guid", "localIdentifierInFile", "fileType", "filePath"}, ""},
        {"NLS::meta::PrivateReflectionExternalSample", {"GetHiddenValue"}, {}, {"m_hiddenValue"}, ""},
    };

    for (const TypeExpectation& expectation : expectations)
        ExpectReflectedType(expectation);
}

TEST_F(ReflectionRuntimeTestFixture, RegistersRenderingResourceTypesThroughGeneratedReflection)
{
    struct ResourceTypeExpectation
    {
        const char* typeName;
        const char* directBaseTypeName;
    };

    const std::array<ResourceTypeExpectation, 6> resourceTypeNames = {
        ResourceTypeExpectation{"NLS::Render::Resources::Mesh", "NLS::NamedObject"},
        ResourceTypeExpectation{"NLS::Render::Resources::Material", "NLS::NamedObject"},
        ResourceTypeExpectation{"NLS::Render::Resources::Shader", "NLS::NamedObject"},
        ResourceTypeExpectation{"NLS::Render::Resources::Texture", "NLS::NamedObject"},
        ResourceTypeExpectation{"NLS::Render::Resources::Texture2D", "NLS::Render::Resources::Texture"},
        ResourceTypeExpectation{"NLS::Render::Resources::TextureCube", "NLS::Render::Resources::Texture"},
    };

    for (const ResourceTypeExpectation& expectation : resourceTypeNames)
    {
        const char* resourceTypeName = expectation.typeName;
        const auto resourceType = NLS::meta::Type::GetFromName(resourceTypeName);
        ASSERT_TRUE(resourceType.IsValid()) << resourceTypeName;
        EXPECT_TRUE(resourceType.IsClass()) << resourceTypeName;
        EXPECT_EQ(resourceType.GetName(), resourceTypeName);

        const auto directBaseType = NLS::meta::Type::GetFromName(expectation.directBaseTypeName);
        ASSERT_TRUE(directBaseType.IsValid()) << expectation.directBaseTypeName;
        const auto& directBaseTypes = resourceType.GetBaseClasses();
        EXPECT_EQ(directBaseTypes.size(), 1u) << resourceTypeName;
        EXPECT_NE(directBaseTypes.find(directBaseType), directBaseTypes.end()) << resourceTypeName;

        const auto pointerType = NLS::meta::Type::GetFromName(std::string(resourceTypeName) + "*");
        ASSERT_TRUE(pointerType.IsValid()) << resourceTypeName;
        EXPECT_TRUE(pointerType.IsPointer()) << resourceTypeName;
        EXPECT_EQ(pointerType.GetDecayedType(), resourceType) << resourceTypeName;

        const auto constPointerType = NLS::meta::Type::GetFromName(std::string("const ") + resourceTypeName + "*");
        ASSERT_TRUE(constPointerType.IsValid()) << resourceTypeName;
        EXPECT_TRUE(constPointerType.IsPointer()) << resourceTypeName;
        EXPECT_EQ(constPointerType.GetDecayedType(), resourceType) << resourceTypeName;
    }

    EXPECT_STREQ(NLS::Render::Resources::Mesh::StaticMetaTypeName(), "NLS::Render::Resources::Mesh");
    EXPECT_STREQ(NLS::Render::Resources::Material::StaticMetaTypeName(), "NLS::Render::Resources::Material");
    EXPECT_STREQ(NLS::Render::Resources::Shader::StaticMetaTypeName(), "NLS::Render::Resources::Shader");
    EXPECT_STREQ(NLS::Render::Resources::Texture::StaticMetaTypeName(), "NLS::Render::Resources::Texture");
    EXPECT_STREQ(NLS::Render::Resources::Texture2D::StaticMetaTypeName(), "NLS::Render::Resources::Texture2D");
    EXPECT_STREQ(NLS::Render::Resources::TextureCube::StaticMetaTypeName(), "NLS::Render::Resources::TextureCube");
}

TEST_F(ReflectionRuntimeTestFixture, RenderingResourceReflectionDoesNotReportMissingBaseTypes)
{
    for (const auto& diagnostic : NLS::meta::ReflectionDiagnostics::Snapshot())
    {
        if (diagnostic.typeName.rfind("NLS::Render::Resources::", 0) != 0)
            continue;

        EXPECT_NE(diagnostic.subject, "missing referenced type")
            << NLS::meta::ReflectionDiagnostics::Format(diagnostic);
    }
}

TEST_F(ReflectionRuntimeTestFixture, RegistersComponentMenuMetadataOnEngineComponents)
{
    const auto meshRendererType = NLS::meta::Type::GetFromName("NLS::Engine::Components::MeshRenderer");
    const auto meshRendererMenu = meshRendererType.GetMeta().GetProperty<NLS::meta::ComponentMenu>();

    ASSERT_NE(meshRendererMenu, nullptr);
    EXPECT_EQ(meshRendererMenu->path, "Rendering/Mesh Renderer");

    const auto cameraType = NLS::meta::Type::GetFromName("NLS::Engine::Components::CameraComponent");
    const auto cameraMenu = cameraType.GetMeta().GetProperty<NLS::meta::ComponentMenu>();

    ASSERT_NE(cameraMenu, nullptr);
    EXPECT_EQ(cameraMenu->path, "Rendering/Camera");
}

TEST_F(ReflectionRuntimeTestFixture, RegistersRenderEnumAndStructReflectionTypes)
{
    const std::vector<TypeExpectation> expectations = {
        {"NLS::Render::Settings::EProjectionMode", {}, {}, {}, ""},
        {"NLS::Render::Settings::ELightType", {}, {}, {}, ""},
        {"NLS::Render::Geometry::BoundingSphere", {}, {}, {"position", "radius"}, ""},
        {"NLS::Engine::Components::MeshRenderer::EFrustumBehaviour", {}, {}, {}, ""}
    };

    for (const TypeExpectation& expectation : expectations)
        ExpectReflectedType(expectation);
}
