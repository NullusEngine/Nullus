#include <gtest/gtest.h>

#include <atomic>
#include <limits>
#include <thread>
#include <unordered_set>
#include <vector>

#include "Core/ServiceLocator.h"
#include "Object/Object.h"
#include "Reflection/Type.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/Mesh.h"
#include "Rendering/Resources/Shader.h"
#include "Rendering/Resources/Texture.h"
#include "Rendering/Resources/Texture2D.h"
#include "Rendering/Resources/TextureCube.h"
#include "Rendering/Settings/DriverSettings.h"
#include "Rendering/Settings/EGraphicsBackend.h"
#include "Serialize/ObjectReferenceResolver.h"
#include "Serialize/PPtr.h"

namespace
{
class ForwardDeclaredObject;

class TestObject : public NLS::Object
{
public:
    using Object::Object;
};

class TestNamedObject : public NLS::NamedObject
{
public:
    explicit TestNamedObject(std::string name)
        : NamedObject(std::move(name))
    {
    }
};

class OtherTestObject : public NLS::Object
{
public:
    using Object::Object;
};

template <typename T, typename = void>
struct IsPPtrDefaultConstructible : std::false_type
{
};

template <typename T>
struct IsPPtrDefaultConstructible<T, std::void_t<decltype(NLS::Engine::Serialize::PPtr<T> {})>> : std::true_type
{
};

template <typename T, typename = void>
struct IsPPtrInstanceIDConstructible : std::false_type
{
};

template <typename T>
struct IsPPtrInstanceIDConstructible<
    T,
    std::void_t<decltype(NLS::Engine::Serialize::PPtr<T>(std::declval<NLS::Engine::Serialize::InstanceID>()))>>
    : std::true_type
{
};

static_assert(!NLS::Engine::Serialize::Detail::IsCompleteObjectTargetV<ForwardDeclaredObject>);
static_assert(!NLS::Engine::Serialize::Detail::IsCompleteObjectTargetV<int>);
static_assert(NLS::Engine::Serialize::Detail::IsCompleteObjectTargetV<TestObject>);
static_assert(!IsPPtrDefaultConstructible<ForwardDeclaredObject>::value);
static_assert(!IsPPtrInstanceIDConstructible<ForwardDeclaredObject>::value);
static_assert(!IsPPtrDefaultConstructible<int>::value);
static_assert(!IsPPtrInstanceIDConstructible<int>::value);
static_assert(IsPPtrDefaultConstructible<TestObject>::value);
static_assert(IsPPtrInstanceIDConstructible<TestObject>::value);
static_assert(!std::is_move_constructible_v<NLS::Render::Resources::Texture>);
static_assert(!std::is_move_assignable_v<NLS::Render::Resources::Texture>);
static_assert(!std::is_move_constructible_v<NLS::Render::Resources::Texture2D>);
static_assert(!std::is_move_assignable_v<NLS::Render::Resources::Texture2D>);
}

TEST(PPtrTests, DefaultsToNullInstanceID)
{
    const NLS::Engine::Serialize::PPtr<TestObject> pointer;

    EXPECT_TRUE(pointer.IsNull());
    EXPECT_EQ(pointer.GetInstanceID(), NLS::Engine::Serialize::InstanceID_None);
}

TEST(PPtrTests, StoresAndComparesInstanceID)
{
    NLS::Engine::Serialize::PPtr<TestObject> first(NLS::Engine::Serialize::InstanceID {42});
    NLS::Engine::Serialize::PPtr<TestObject> same(NLS::Engine::Serialize::InstanceID {42});
    NLS::Engine::Serialize::PPtr<TestObject> other(NLS::Engine::Serialize::InstanceID {77});

    EXPECT_FALSE(first.IsNull());
    EXPECT_EQ(first.GetInstanceID(), 42);
    EXPECT_EQ(first, same);
    EXPECT_NE(first, other);

    first.SetInstanceID(NLS::Engine::Serialize::InstanceID_None);
    EXPECT_TRUE(first.IsNull());
}

TEST(PPtrTests, ResourcePPtrMetaTypeNamesDoNotDependOnExternalReflectionIncludes)
{
    EXPECT_STREQ(
        NLS::Engine::Serialize::PPtr<NLS::Render::Resources::Mesh>::StaticMetaTypeName(),
        "NLS::Engine::Serialize::PPtr<NLS::Render::Resources::Mesh>");
    EXPECT_STREQ(
        NLS::Engine::Serialize::PPtr<NLS::Render::Resources::Material>::StaticMetaTypeName(),
        "NLS::Engine::Serialize::PPtr<NLS::Render::Resources::Material>");
    EXPECT_STREQ(
        NLS::Engine::Serialize::PPtr<NLS::Render::Resources::Shader>::StaticMetaTypeName(),
        "NLS::Engine::Serialize::PPtr<NLS::Render::Resources::Shader>");
    EXPECT_STREQ(
        NLS::Engine::Serialize::PPtr<NLS::Render::Resources::Texture>::StaticMetaTypeName(),
        "NLS::Engine::Serialize::PPtr<NLS::Render::Resources::Texture>");
    EXPECT_STREQ(
        NLS::Engine::Serialize::PPtr<NLS::Render::Resources::Texture2D>::StaticMetaTypeName(),
        "NLS::Engine::Serialize::PPtr<NLS::Render::Resources::Texture2D>");
    EXPECT_STREQ(
        NLS::Engine::Serialize::PPtr<NLS::Render::Resources::TextureCube>::StaticMetaTypeName(),
        "NLS::Engine::Serialize::PPtr<NLS::Render::Resources::TextureCube>");
}

TEST(PPtrTests, SerializedIdentifierKeepsUnityFieldsSeparateFromRuntimePointer)
{
    NLS::Engine::Serialize::SerializedObjectIdentifier identifier;
    identifier.serializedFileIndex = 1;
    identifier.localIdentifierInFile = 2100000;

    EXPECT_EQ(identifier.localIdentifierInFile, 2100000);
    EXPECT_EQ(identifier.serializedFileIndex, 1);
}

TEST(PPtrTests, ObjectIdentifierKeepsBuildPipelineFieldsSeparateFromRuntimePointer)
{
    auto identifier = NLS::Engine::Serialize::ObjectIdentifier::Asset(
        NLS::Engine::Serialize::AssetId(NLS::Guid::Parse("dddddddd-dddd-4ddd-9ddd-dddddddddddd")),
        2100000,
        "Assets/Materials/Default.mat");

    EXPECT_TRUE(identifier.IsAsset());
    EXPECT_EQ(identifier.localIdentifierInFile, 2100000);
    EXPECT_EQ(identifier.guid.ToString(), "dddddddd-dddd-4ddd-9ddd-dddddddddddd");
    EXPECT_EQ(identifier.fileType, NLS::Engine::Serialize::FileType::SerializedAssetType);
    EXPECT_EQ(identifier.filePath, "Assets/Materials/Default.mat");
}

TEST(PPtrTests, PersistentManagerMapsInstanceIDToObjectIdentifier)
{
    auto& persistentManager = NLS::Engine::Serialize::PersistentManager::Instance();
    persistentManager.Clear();

    const auto identifier = NLS::Engine::Serialize::ObjectIdentifier::Asset(
        NLS::Engine::Serialize::AssetId(NLS::Guid::Parse("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa")),
        4300000,
        "Library/Artifacts/Hero/47b24ab4b128645b99328e0a68370de1202b0ba370eafc30e8bb0b0b7cf8b5ae");

    const auto instanceID = persistentManager.ObjectIdentifierToInstanceID(identifier);
    ASSERT_NE(instanceID, NLS::Engine::Serialize::InstanceID_None);

    NLS::Engine::Serialize::ObjectIdentifier roundTripped;
    ASSERT_TRUE(persistentManager.InstanceIDToObjectIdentifier(instanceID, roundTripped));
    EXPECT_EQ(roundTripped, identifier);

    NLS::Engine::Serialize::LocalSerializedObjectIdentifier localIdentifier;
    persistentManager.InstanceIDToLocalSerializedObjectIdentifier(instanceID, localIdentifier);
    EXPECT_EQ(localIdentifier.localSerializedFileIndex, 1);
    EXPECT_EQ(localIdentifier.localIdentifierInFile, 4300000);

    EXPECT_EQ(persistentManager.ObjectIdentifierToInstanceID(identifier), instanceID);
}

TEST(PPtrTests, PersistentManagerTreatsAssetPathAsHintNotIdentity)
{
    auto& persistentManager = NLS::Engine::Serialize::PersistentManager::Instance();
    persistentManager.Clear();

    const auto stalePath = NLS::Engine::Serialize::ObjectIdentifier::Asset(
        NLS::Engine::Serialize::AssetId(NLS::Guid::Parse("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee")),
        4300000,
        "Library/Artifacts/Stale/47b24ab4b128645b99328e0a68370de1202b0ba370eafc30e8bb0b0b7cf8b5ae");
    const auto currentPath = NLS::Engine::Serialize::ObjectIdentifier::Asset(
        NLS::Engine::Serialize::AssetId(NLS::Guid::Parse("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee")),
        4300000,
        "Library/Artifacts/Current/47b24ab4b128645b99328e0a68370de1202b0ba370eafc30e8bb0b0b7cf8b5ae");

    const auto instanceID = persistentManager.ObjectIdentifierToInstanceID(stalePath);

    EXPECT_EQ(persistentManager.ObjectIdentifierToInstanceID(currentPath), instanceID);

    NLS::Engine::Serialize::ObjectIdentifier roundTripped;
    ASSERT_TRUE(persistentManager.InstanceIDToObjectIdentifier(instanceID, roundTripped));
    EXPECT_EQ(roundTripped.filePath, "Library/Artifacts/Current/47b24ab4b128645b99328e0a68370de1202b0ba370eafc30e8bb0b0b7cf8b5ae");
}

TEST(PPtrTests, PersistentManagerLocalSerializedIdentifierRoundTripsExternalAssetIdentity)
{
    auto& persistentManager = NLS::Engine::Serialize::PersistentManager::Instance();
    persistentManager.Clear();

    const auto identifier = NLS::Engine::Serialize::ObjectIdentifier::Asset(
        NLS::Engine::Serialize::AssetId(NLS::Guid::Parse("99999999-aaaa-4bbb-8ccc-dddddddddddd")),
        2100000,
        "Library/Artifacts/External/47b24ab4b128645b99328e0a68370de1202b0ba370eafc30e8bb0b0b7cf8b5ae");

    const auto instanceID = persistentManager.ObjectIdentifierToInstanceID(identifier);
    ASSERT_NE(instanceID, NLS::Engine::Serialize::InstanceID_None);

    NLS::Engine::Serialize::LocalSerializedObjectIdentifier localIdentifier;
    persistentManager.InstanceIDToLocalSerializedObjectIdentifier(instanceID, localIdentifier);
    EXPECT_GT(localIdentifier.localSerializedFileIndex, 0);
    EXPECT_EQ(localIdentifier.localIdentifierInFile, identifier.localIdentifierInFile);

    NLS::Engine::Serialize::InstanceID recoveredInstanceID = NLS::Engine::Serialize::InstanceID_None;
    persistentManager.LocalSerializedObjectIdentifierToInstanceID(localIdentifier, recoveredInstanceID);
    EXPECT_EQ(recoveredInstanceID, instanceID);

    NLS::Engine::Serialize::ObjectIdentifier recovered;
    ASSERT_TRUE(persistentManager.InstanceIDToObjectIdentifier(recoveredInstanceID, recovered));
    EXPECT_EQ(recovered.guid, identifier.guid);
    EXPECT_EQ(recovered.localIdentifierInFile, identifier.localIdentifierInFile);
    EXPECT_EQ(recovered.fileType, identifier.fileType);
    EXPECT_EQ(recovered.filePath, identifier.filePath);
    EXPECT_TRUE(recovered.IsAsset());
}

TEST(PPtrTests, PersistentManagerRejectsUnknownExternalSerializedFileIndex)
{
    auto& persistentManager = NLS::Engine::Serialize::PersistentManager::Instance();
    persistentManager.Clear();

    NLS::Engine::Serialize::InstanceID recoveredInstanceID = NLS::Engine::Serialize::InstanceID_None;
    persistentManager.LocalSerializedObjectIdentifierToInstanceID({77, 2100000}, recoveredInstanceID);

    EXPECT_EQ(recoveredInstanceID, NLS::Engine::Serialize::InstanceID_None);
}

TEST(PPtrTests, PersistentManagerRegisterInstanceIDKeepsBidirectionalMapsConsistent)
{
    auto& persistentManager = NLS::Engine::Serialize::PersistentManager::Instance();
    persistentManager.Clear();

    const auto first = NLS::Engine::Serialize::ObjectIdentifier::Asset(
        NLS::Engine::Serialize::AssetId(NLS::Guid::Parse("11111111-1111-4111-8111-111111111111")),
        2100000,
        "Library/Artifacts/First.asset");
    const auto second = NLS::Engine::Serialize::ObjectIdentifier::Asset(
        NLS::Engine::Serialize::AssetId(NLS::Guid::Parse("22222222-2222-4222-8222-222222222222")),
        2200000,
        "Library/Artifacts/Second.asset");

    persistentManager.RegisterInstanceID(NLS::Engine::Serialize::InstanceID {42}, first);
    persistentManager.RegisterInstanceID(NLS::Engine::Serialize::InstanceID {42}, second);

    NLS::Engine::Serialize::ObjectIdentifier roundTripped;
    ASSERT_TRUE(persistentManager.InstanceIDToObjectIdentifier(NLS::Engine::Serialize::InstanceID {42}, roundTripped));
    EXPECT_EQ(roundTripped, second);
    EXPECT_NE(persistentManager.ObjectIdentifierToInstanceID(first), NLS::Engine::Serialize::InstanceID {42});
    EXPECT_EQ(persistentManager.ObjectIdentifierToInstanceID(second), NLS::Engine::Serialize::InstanceID {42});
}

TEST(PPtrTests, PersistentManagerExplicitInstanceIDIsReservedInObjectRegistry)
{
    auto& persistentManager = NLS::Engine::Serialize::PersistentManager::Instance();
    persistentManager.Clear();

    const auto identifier = NLS::Engine::Serialize::ObjectIdentifier::Asset(
        NLS::Engine::Serialize::AssetId(NLS::Guid::Parse("12121212-1212-4121-8121-121212121212")),
        2100000,
        "Library/Artifacts/Reserved.asset");

    persistentManager.RegisterInstanceID(NLS::Engine::Serialize::InstanceID {42}, identifier);
    TestObject object;

    EXPECT_NE(object.GetInstanceID(), NLS::Engine::Serialize::InstanceID {42});
    EXPECT_EQ(persistentManager.ObjectIdentifierToInstanceID(identifier), NLS::Engine::Serialize::InstanceID {42});
}

TEST(PPtrTests, PersistentManagerReleasesOrphanedReservedInstanceIDWhenIdentifierIsRemapped)
{
    auto& persistentManager = NLS::Engine::Serialize::PersistentManager::Instance();
    persistentManager.Clear();

    const auto identifier = NLS::Engine::Serialize::ObjectIdentifier::Asset(
        NLS::Engine::Serialize::AssetId(NLS::Guid::Parse("13131313-1313-4131-8131-131313131313")),
        2100000,
        "Library/Artifacts/Remapped.asset");

    const auto originalInstanceID = persistentManager.ObjectIdentifierToInstanceID(identifier);
    ASSERT_NE(originalInstanceID, NLS::Engine::Serialize::InstanceID_None);

    persistentManager.RegisterInstanceID(NLS::Engine::Serialize::InstanceID {42}, identifier);

    EXPECT_EQ(persistentManager.ObjectIdentifierToInstanceID(identifier), NLS::Engine::Serialize::InstanceID {42});
    TestObject object;
    EXPECT_NE(object.GetInstanceID(), originalInstanceID);
    EXPECT_NE(object.GetInstanceID(), NLS::Engine::Serialize::InstanceID {42});
}

TEST(PPtrTests, ObjectRegistryHandlesMaxExplicitInstanceID)
{

    TestObject maxObject(std::numeric_limits<NLS::Engine::Serialize::InstanceID>::max());
    TestObject nextObject;

    EXPECT_EQ(maxObject.GetInstanceID(), std::numeric_limits<NLS::Engine::Serialize::InstanceID>::max());
    EXPECT_NE(nextObject.GetInstanceID(), NLS::Engine::Serialize::InstanceID_None);
    EXPECT_NE(nextObject.GetInstanceID(), maxObject.GetInstanceID());
}

TEST(PPtrTests, BindingLoadedObjectIdentifierMakesExistingPPtrDereferenceObject)
{
    auto& persistentManager = NLS::Engine::Serialize::PersistentManager::Instance();
    persistentManager.Clear();

    const auto identifier = NLS::Engine::Serialize::ObjectIdentifier::Asset(
        NLS::Engine::Serialize::AssetId(NLS::Guid::Parse("33333333-3333-4333-8333-333333333333")),
        2100000,
        "Library/Artifacts/Hero/materials/8ca977f3a8a054ff6767e381b334be9e47456f725e02f84e11a3b5b1f3f4218b");

    NLS::Engine::Serialize::PPtr<NLS::Render::Resources::Material> reference(
        persistentManager.ObjectIdentifierToInstanceID(identifier));
    EXPECT_EQ(reference.Get(), nullptr);

    NLS::Render::Resources::Material material;
    const auto oldInstanceID = material.GetInstanceID();

    EXPECT_EQ(persistentManager.BindObjectIdentifier(material, identifier), reference.GetInstanceID());

    EXPECT_NE(material.GetInstanceID(), oldInstanceID);
    EXPECT_EQ(material.GetInstanceID(), reference.GetInstanceID());
    EXPECT_EQ(reference.Get(), &material);
    EXPECT_EQ(NLS::Object::IDToPointer(oldInstanceID), nullptr);
}

TEST(PPtrTests, BindingObjectIdentifierRejectsExistingLiveObjectOwner)
{
    auto& persistentManager = NLS::Engine::Serialize::PersistentManager::Instance();
    persistentManager.Clear();

    const auto identifier = NLS::Engine::Serialize::ObjectIdentifier::Asset(
        NLS::Engine::Serialize::AssetId(NLS::Guid::Parse("44444444-4444-4444-8444-444444444444")),
        2100000,
        "Library/Artifacts/AlreadyLoaded.asset");

    NLS::Render::Resources::Material first;
    ASSERT_NE(persistentManager.BindObjectIdentifier(first, identifier), NLS::Engine::Serialize::InstanceID_None);

    NLS::Render::Resources::Material second;
    const auto secondID = second.GetInstanceID();

    EXPECT_EQ(persistentManager.BindObjectIdentifier(second, identifier), NLS::Engine::Serialize::InstanceID_None);
    EXPECT_EQ(second.GetInstanceID(), secondID);
    EXPECT_EQ(NLS::Object::IDToPointer(first.GetInstanceID()), &first);
}

TEST(PPtrTests, ResolvedObjectReferenceHelperBindsLoadedObjectWithoutChangingPersistentIdentity)
{
    auto& persistentManager = NLS::Engine::Serialize::PersistentManager::Instance();
    persistentManager.Clear();

    const auto identifier = NLS::Engine::Serialize::ObjectIdentifier::Asset(
        NLS::Engine::Serialize::AssetId(NLS::Guid::Parse("55555555-5555-4555-8555-555555555555")),
        2100000,
        "Library/Artifacts/Stale/47b24ab4b128645b99328e0a68370de1202b0ba370eafc30e8bb0b0b7cf8b5ae");
    auto reference = NLS::Engine::Serialize::PPtr<NLS::Render::Resources::Material>(
        persistentManager.ObjectIdentifierToInstanceID(identifier));

    NLS::Render::Resources::Material material;
    ASSERT_TRUE(NLS::Engine::Serialize::BindResolvedObjectReference(material, reference));

    NLS::Engine::Serialize::ObjectIdentifier roundTripped;
    ASSERT_TRUE(persistentManager.InstanceIDToObjectIdentifier(reference.GetInstanceID(), roundTripped));
    EXPECT_EQ(roundTripped, identifier);
    EXPECT_EQ(reference.Get(), &material);
}

TEST(PPtrTests, ResolvedObjectReferenceHelperRejectsConflictingLoadedObject)
{
    auto& persistentManager = NLS::Engine::Serialize::PersistentManager::Instance();
    persistentManager.Clear();

    const auto identifier = NLS::Engine::Serialize::ObjectIdentifier::Asset(
        NLS::Engine::Serialize::AssetId(NLS::Guid::Parse("66666666-6666-4666-8666-666666666666")),
        2100000,
        "Library/Artifacts/AlreadyLoaded.material");

    auto reference = NLS::Engine::Serialize::PPtr<NLS::Render::Resources::Material>(
        persistentManager.ObjectIdentifierToInstanceID(identifier));
    NLS::Render::Resources::Material first;
    ASSERT_TRUE(NLS::Engine::Serialize::BindResolvedObjectReference(first, reference));

    NLS::Render::Resources::Material second;
    EXPECT_FALSE(NLS::Engine::Serialize::BindResolvedObjectReference(second, reference));
    EXPECT_EQ(reference.Get(), &first);
}

TEST(PPtrTests, PersistentManagerAllocatesStableUniqueInstanceIDsAcrossThreads)
{
    auto& persistentManager = NLS::Engine::Serialize::PersistentManager::Instance();
    persistentManager.Clear();

    constexpr int kThreadCount = 8;
    constexpr int kReferencesPerThread = 64;
    std::vector<std::thread> threads;
    std::vector<NLS::Engine::Serialize::InstanceID> instanceIDs(kThreadCount * kReferencesPerThread);
    std::atomic<bool> start {false};

    for (int threadIndex = 0; threadIndex < kThreadCount; ++threadIndex)
    {
        threads.emplace_back([&, threadIndex]()
        {
            while (!start.load(std::memory_order_acquire))
            {
            }

            for (int referenceIndex = 0; referenceIndex < kReferencesPerThread; ++referenceIndex)
            {
                const auto flatIndex = threadIndex * kReferencesPerThread + referenceIndex;
                const auto identifier = NLS::Engine::Serialize::ObjectIdentifier::Asset(
                    NLS::Engine::Serialize::AssetId(NLS::Guid::NewDeterministic(
                        "PersistentManager.Concurrent." + std::to_string(flatIndex))),
                    2100000 + flatIndex,
                    "Library/Artifacts/Concurrent/" + std::to_string(flatIndex) + ".asset");
                instanceIDs[flatIndex] = persistentManager.ObjectIdentifierToInstanceID(identifier);
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& thread : threads)
        thread.join();

    std::unordered_set<NLS::Engine::Serialize::InstanceID> uniqueIDs;
    for (const auto instanceID : instanceIDs)
    {
        EXPECT_NE(instanceID, NLS::Engine::Serialize::InstanceID_None);
        EXPECT_TRUE(uniqueIDs.insert(instanceID).second) << "Duplicate InstanceID " << instanceID;
        NLS::Engine::Serialize::ObjectIdentifier roundTripped;
        EXPECT_TRUE(persistentManager.InstanceIDToObjectIdentifier(instanceID, roundTripped));
        EXPECT_TRUE(roundTripped.IsAsset());
    }
}

TEST(ObjectSystemTests, ObjectRegistersAndUnregistersInstanceID)
{

    NLS::Engine::Serialize::InstanceID instanceID = NLS::Engine::Serialize::InstanceID_None;
    {
        TestObject object;
        instanceID = object.GetInstanceID();

        ASSERT_NE(instanceID, NLS::Engine::Serialize::InstanceID_None);
        EXPECT_EQ(NLS::Object::IDToPointer(instanceID), &object);
    }

    EXPECT_EQ(NLS::Object::IDToPointer(instanceID), nullptr);
}

TEST(ObjectSystemTests, NamedObjectStoresDisplayName)
{

    TestNamedObject object("Hero Material");

    EXPECT_EQ(object.GetName(), "Hero Material");
    object.SetName("Renamed");
    EXPECT_EQ(object.GetName(), "Renamed");
}

TEST(PPtrTests, AssignsObjectInstanceIDAndDereferencesThroughRegistry)
{

    TestObject object;
    NLS::Engine::Serialize::PPtr<TestObject> pointer(&object);

    EXPECT_EQ(pointer.GetInstanceID(), object.GetInstanceID());
    EXPECT_EQ(pointer.Get(), &object);
    EXPECT_EQ(static_cast<TestObject*>(pointer), &object);
    EXPECT_EQ(pointer.operator->(), &object);

    pointer = nullptr;
    EXPECT_TRUE(pointer.IsNull());
    EXPECT_EQ(pointer.Get(), nullptr);
}

TEST(PPtrTests, RejectsIncompatibleObjectTypeOnDereference)
{

    OtherTestObject other;
    NLS::Engine::Serialize::PPtr<TestObject> pointer(other.GetInstanceID());

    EXPECT_FALSE(pointer.IsNull());
    EXPECT_EQ(pointer.Get(), nullptr);
    EXPECT_EQ(static_cast<TestObject*>(pointer), nullptr);
}

TEST(ObjectSystemTests, RenderResourcesAreNamedObjectsWithInstanceIDs)
{

    NLS::Render::Resources::Material material;
    NLS::Render::Resources::Mesh mesh({}, {}, 0u);

    EXPECT_TRUE((std::is_base_of_v<NLS::NamedObject, NLS::Render::Resources::Material>));
    EXPECT_TRUE((std::is_base_of_v<NLS::NamedObject, NLS::Render::Resources::Mesh>));
    EXPECT_TRUE((std::is_base_of_v<NLS::NamedObject, NLS::Render::Resources::Texture2D>));

    EXPECT_EQ(NLS::Object::IDToPointer(material.GetInstanceID()), &material);
    EXPECT_EQ(NLS::Object::IDToPointer(mesh.GetInstanceID()), &mesh);
}

TEST(ObjectSystemTests, RenderResourcesReportConcreteObjectTypes)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);

    NLS::Render::Resources::Material material;
    NLS::Render::Resources::Mesh mesh({}, {}, 0u);
    NLS::Render::Resources::Texture texture;
    auto texture2D = NLS::Render::Resources::Texture2D::WrapExternal(nullptr, 4u, 4u);
    NLS::Render::Resources::TextureCube textureCube;

    ASSERT_TRUE(material.GetType().IsValid());
    ASSERT_TRUE(mesh.GetType().IsValid());
    ASSERT_TRUE(texture.GetType().IsValid());
    ASSERT_NE(texture2D, nullptr);
    ASSERT_TRUE(texture2D->GetType().IsValid());
    ASSERT_TRUE(textureCube.GetType().IsValid());
    EXPECT_EQ(material.GetType().GetName(), "NLS::Render::Resources::Material");
    EXPECT_EQ(mesh.GetType().GetName(), "NLS::Render::Resources::Mesh");
    EXPECT_EQ(texture.GetType().GetName(), "NLS::Render::Resources::Texture");
    EXPECT_EQ(texture2D->GetType().GetName(), "NLS::Render::Resources::Texture2D");
    EXPECT_EQ(textureCube.GetType().GetName(), "NLS::Render::Resources::TextureCube");

    NLS::Core::ServiceLocator::Remove<NLS::Render::Context::Driver>();
}
