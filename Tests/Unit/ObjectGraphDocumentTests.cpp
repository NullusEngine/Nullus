#include <gtest/gtest.h>

#include <Reflection/ExternalReflectionRegistration.h>
#include <Reflection/ReflectionDatabase.h>

#include <stdexcept>

#include "Serialize/ObjectGraphDocument.h"
#include "Serialize/ObjectGraphReader.h"
#include "Serialize/ObjectGraphSerializer.h"
#include "Serialize/ObjectGraphWriter.h"
#include "Serialize/ObjectId.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/Mesh.h"
#include "Rendering/Resources/Texture.h"
#include "Rendering/Resources/Texture2D.h"
#include "Rendering/Resources/TextureCube.h"

namespace NLS::Engine::Serialize::Tests
{
struct ObjectReferenceSerializationFixture : NLS::Object
{
    static constexpr const char* StaticMetaTypeName()
    {
        return "NLS::Engine::Serialize::Tests::ObjectReferenceSerializationFixture";
    }

    static constexpr NLS::meta::TypeKey StaticMetaTypeKey()
    {
        return NLS::meta::HashTypeKey("NLS::Engine::Serialize::Tests::ObjectReferenceSerializationFixture");
    }

    const char* GetObjectTypeName() const override
    {
        return StaticMetaTypeName();
    }

    NLS::Engine::Serialize::PPtr<NLS::Render::Resources::Material> reference;
};

struct AdditionalPPtrSerializationFixture : NLS::Object
{
    static constexpr const char* StaticMetaTypeName()
    {
        return "NLS::Engine::Serialize::Tests::AdditionalPPtrSerializationFixture";
    }

    static constexpr NLS::meta::TypeKey StaticMetaTypeKey()
    {
        return NLS::meta::HashTypeKey("NLS::Engine::Serialize::Tests::AdditionalPPtrSerializationFixture");
    }

    const char* GetObjectTypeName() const override
    {
        return StaticMetaTypeName();
    }

    NLS::Engine::Serialize::PPtr<NLS::Render::Resources::Mesh> mesh;
    NLS::Engine::Serialize::PPtr<NLS::Render::Resources::Texture> texture;
    NLS::Array<NLS::Engine::Serialize::PPtr<NLS::Render::Resources::Texture2D>> textures;
    NLS::Array<NLS::Engine::Serialize::PPtr<NLS::Render::Resources::TextureCube>> cubeMaps;
};

NLS_META_EXTERNAL_TYPE_NAME(NLS::Engine::Serialize::Tests::ObjectReferenceSerializationFixture)
NLS_META_EXTERNAL_TYPE_NAME(NLS::Engine::Serialize::Tests::AdditionalPPtrSerializationFixture)

inline void RegisterObjectGraphDocumentTestReflection(
    NLS::meta::ReflectionDatabase& db,
    NLS::meta::ReflectionRegistrationPhase phase)
{
    NLS_META_EXTERNAL_BEGIN(NLS::Engine::Serialize::Tests::ObjectReferenceSerializationFixture)
        NLS_META_EXTERNAL_FIELD(NLS::Engine::Serialize::PPtr<NLS::Render::Resources::Material>, reference);
    NLS_META_EXTERNAL_END();

    NLS_META_EXTERNAL_BEGIN(NLS::Engine::Serialize::Tests::AdditionalPPtrSerializationFixture)
        NLS_META_EXTERNAL_FIELD(NLS::Engine::Serialize::PPtr<NLS::Render::Resources::Mesh>, mesh);
        NLS_META_EXTERNAL_FIELD(NLS::Engine::Serialize::PPtr<NLS::Render::Resources::Texture>, texture);
        NLS_META_EXTERNAL_FIELD(NLS::Array<NLS::Engine::Serialize::PPtr<NLS::Render::Resources::Texture2D>>, textures);
        NLS_META_EXTERNAL_FIELD(NLS::Array<NLS::Engine::Serialize::PPtr<NLS::Render::Resources::TextureCube>>, cubeMaps);
    NLS_META_EXTERNAL_END();
}
}

NLS_META_EXTERNAL_MODULE(NLS::Engine::Serialize::Tests::RegisterObjectGraphDocumentTestReflection)

TEST(ObjectGraphDocumentTests, ObjectIdAndAssetIdWrapGuidWithoutInterchange)
{
    const auto objectGuid = NLS::Guid::NewDeterministic("Object");
    const auto assetGuid = NLS::Guid::NewDeterministic("Asset");

    const NLS::Engine::Serialize::ObjectId objectId(objectGuid);
    const NLS::Engine::Serialize::AssetId assetId(assetGuid);

    EXPECT_TRUE(objectId.IsValid());
    EXPECT_TRUE(assetId.IsValid());
    EXPECT_EQ(objectId.GetGuid(), objectGuid);
    EXPECT_EQ(assetId.GetGuid(), assetGuid);
    EXPECT_NE(objectId.GetGuid(), assetId.GetGuid());
}

TEST(ObjectGraphDocumentTests, EmptyStrongIdsAreInvalid)
{
    EXPECT_FALSE(NLS::Engine::Serialize::ObjectId().IsValid());
    EXPECT_FALSE(NLS::Engine::Serialize::AssetId().IsValid());
}

TEST(ObjectGraphDocumentTests, ObjectIdentifierUsesUnityLikeIdentifierShape)
{
    using namespace NLS::Engine::Serialize;

    const auto sceneObject = ObjectIdentifier::LocalObject(12345);
    EXPECT_EQ(sceneObject.localIdentifierInFile, 12345);
    EXPECT_FALSE(sceneObject.guid.IsValid());
    EXPECT_EQ(sceneObject.fileType, FileType::NonAssetType);

    const auto asset = ObjectIdentifier::Asset(
        AssetId(NLS::Guid::Parse("dddddddd-dddd-4ddd-9ddd-dddddddddddd")),
        MakeLocalIdentifierInFile(
            NLS::Guid::Parse("dddddddd-dddd-4ddd-9ddd-dddddddddddd"),
            "Assets/Materials/Default.mat"),
        "Assets/Materials/Default.mat");
    EXPECT_NE(asset.localIdentifierInFile, 0);
    EXPECT_EQ(asset.guid.ToString(), "dddddddd-dddd-4ddd-9ddd-dddddddddddd");
    EXPECT_EQ(asset.fileType, FileType::SerializedAssetType);
    EXPECT_EQ(asset.filePath, "Assets/Materials/Default.mat");
    EXPECT_EQ(asset.ToAssetId().GetGuid().ToString(), "dddddddd-dddd-4ddd-9ddd-dddddddddddd");
}

TEST(ObjectGraphDocumentTests, ObjectIdentifierValidityRequiresCompleteUnityIdentifierShape)
{
    using namespace NLS::Engine::Serialize;

    ObjectIdentifier empty;
    EXPECT_FALSE(empty.IsValid());
    EXPECT_FALSE(empty.IsLocalObject());
    EXPECT_FALSE(empty.IsAsset());

    const auto localObject = ObjectIdentifier::LocalObject(12345);
    EXPECT_TRUE(localObject.IsValid());
    EXPECT_TRUE(localObject.IsLocalObject());
    EXPECT_FALSE(localObject.IsAsset());

    auto localObjectWithPath = localObject;
    localObjectWithPath.filePath = "Assets/Materials/Default.mat";
    EXPECT_FALSE(localObjectWithPath.IsValid());
    EXPECT_FALSE(localObjectWithPath.IsLocalObject());

    const auto asset = ObjectIdentifier::Asset(
        AssetId(NLS::Guid::Parse("dddddddd-dddd-4ddd-9ddd-dddddddddddd")),
        2100000,
        "Assets/Materials/Default.mat");
    EXPECT_TRUE(asset.IsValid());
    EXPECT_FALSE(asset.IsLocalObject());
    EXPECT_TRUE(asset.IsAsset());

    const auto assetWithoutFileID = ObjectIdentifier::Asset(
        AssetId(NLS::Guid::Parse("dddddddd-dddd-4ddd-9ddd-dddddddddddd")),
        0,
        "Assets/Materials/Default.mat");
    EXPECT_FALSE(assetWithoutFileID.IsValid());
    EXPECT_FALSE(assetWithoutFileID.IsAsset());

    auto assetWithNonAssetFileType = asset;
    assetWithNonAssetFileType.fileType = FileType::NonAssetType;
    EXPECT_FALSE(assetWithNonAssetFileType.IsValid());
    EXPECT_FALSE(assetWithNonAssetFileType.IsAsset());

    auto assetWithUnknownFileType = asset;
    assetWithUnknownFileType.fileType = static_cast<FileType>(99);
    EXPECT_FALSE(assetWithUnknownFileType.IsValid());
    EXPECT_FALSE(assetWithUnknownFileType.IsAsset());
}

TEST(ObjectGraphDocumentTests, TryMakeObjectReferenceRequiresExistingObjectRecord)
{
    using namespace NLS::Engine::Serialize;

    const auto rootId = ObjectId(NLS::Guid::NewDeterministic("Root"));
    const auto missingId = ObjectId(NLS::Guid::NewDeterministic("Missing"));

    ObjectGraphDocument document;
    document.root = rootId;
    ObjectRecord root;
    root.id = rootId;
    root.localIdentifierInFile = MakeLocalIdentifierInFile(rootId);
    root.typeName = "RootType";
    document.objects.push_back(std::move(root));

    auto rootReference = document.TryMakeObjectReference(rootId);
    ASSERT_TRUE(rootReference.has_value());
    EXPECT_EQ(rootReference->localIdentifierInFile, MakeLocalIdentifierInFile(rootId));

    EXPECT_FALSE(document.TryMakeObjectReference(missingId).has_value());
}

namespace
{
NLS::Engine::Serialize::ObjectId MakeObjectId(const char* label)
{
    return NLS::Engine::Serialize::ObjectId(NLS::Guid::NewDeterministic(label));
}

NLS::Engine::Serialize::ObjectRecord MakeRecord(const NLS::Engine::Serialize::ObjectId& id, const char* type)
{
    NLS::Engine::Serialize::ObjectRecord record;
    record.id = id;
    record.localIdentifierInFile = NLS::Engine::Serialize::MakeLocalIdentifierInFile(id);
    record.typeName = type;
    return record;
}

const NLS::Engine::Serialize::PropertyRecord* FindProperty(
    const NLS::Engine::Serialize::ObjectRecord& record,
    const char* name)
{
    const auto found = std::find_if(
        record.properties.begin(),
        record.properties.end(),
        [name](const NLS::Engine::Serialize::PropertyRecord& property)
        {
            return property.name == name;
        });
    return found != record.properties.end() ? &*found : nullptr;
}

bool ContainsDiagnostic(
    const NLS::Engine::Serialize::SerializationDiagnosticList& diagnostics,
    NLS::Engine::Serialize::SerializationDiagnosticCode code)
{
    for (const auto& diagnostic : diagnostics.GetItems())
    {
        if (diagnostic.GetCode() == code)
            return true;
    }
    return false;
}
}

TEST(ObjectGraphDocumentTests, ValidatesDuplicateIdsInvalidIdsAndMissingRoot)
{
    using namespace NLS::Engine::Serialize;

    ObjectGraphDocument document;
    const auto rootId = MakeObjectId("Root");
    document.root = rootId;
    document.objects.push_back(MakeRecord(rootId, "RootType"));
    document.objects.push_back(MakeRecord(rootId, "DuplicateRootType"));
    document.objects.push_back(MakeRecord(ObjectId(), "InvalidIdType"));

    const auto diagnostics = document.Validate();

    EXPECT_TRUE(diagnostics.HasErrors());
    EXPECT_TRUE(ContainsDiagnostic(diagnostics, SerializationDiagnosticCode::DuplicateObjectId));
    EXPECT_TRUE(ContainsDiagnostic(diagnostics, SerializationDiagnosticCode::InvalidGuid));
}

TEST(ObjectGraphDocumentTests, ValidatesMissingRootObject)
{
    using namespace NLS::Engine::Serialize;

    ObjectGraphDocument document;
    document.root = MakeObjectId("MissingRoot");
    document.objects.push_back(MakeRecord(MakeObjectId("OtherObject"), "OtherType"));

    const auto diagnostics = document.Validate();

    EXPECT_TRUE(diagnostics.HasErrors());
    EXPECT_TRUE(ContainsDiagnostic(diagnostics, SerializationDiagnosticCode::MissingObject));
}

TEST(ObjectGraphDocumentTests, ValidatesMissingObjectReferences)
{
    using namespace NLS::Engine::Serialize;

    ObjectGraphDocument document;
    const auto rootId = MakeObjectId("RootWithReference");
    const auto missingId = MakeObjectId("MissingReference");
    auto root = MakeRecord(rootId, "RootType");
    root.properties.push_back({
        "target",
        PropertyValue::ObjectReference(ObjectIdentifier::LocalObject(MakeLocalIdentifierInFile(missingId)))
    });
    document.root = rootId;
    document.objects.push_back(std::move(root));

    const auto diagnostics = document.Validate();

    EXPECT_TRUE(diagnostics.HasErrors());
    EXPECT_TRUE(ContainsDiagnostic(diagnostics, SerializationDiagnosticCode::DanglingReference));
}

TEST(ObjectGraphDocumentTests, ValidatesDanglingReferencesOnStrippedPrefabPlaceholders)
{
    using namespace NLS::Engine::Serialize;

    ObjectGraphDocument document;
    const auto sceneId = MakeObjectId("SceneRoot");
    const auto strippedId = MakeObjectId("StrippedPrefabRoot");
    const auto missingParentId = MakeObjectId("MissingStrippedParent");

    auto sceneRoot = MakeRecord(sceneId, "SceneType");
    sceneRoot.properties.push_back({"gameObjects", PropertyValue::Array({
        PropertyValue::OwnedReference(strippedId)
    })});

    auto strippedRoot = MakeRecord(strippedId, "GameObjectType");
    strippedRoot.state = ObjectRecordState::Stripped;
    strippedRoot.properties.push_back({
        "parent",
        PropertyValue::ObjectReference(ObjectIdentifier::LocalObject(MakeLocalIdentifierInFile(missingParentId)))
    });

    document.root = sceneId;
    document.objects.push_back(std::move(sceneRoot));
    document.objects.push_back(std::move(strippedRoot));

    const auto diagnostics = document.Validate();

    EXPECT_TRUE(diagnostics.HasErrors());
    EXPECT_TRUE(ContainsDiagnostic(diagnostics, SerializationDiagnosticCode::DanglingReference));
}

TEST(ObjectGraphDocumentTests, ValidatesPrefabInstanceAddedObjectsAndInsertTargets)
{
    using namespace NLS::Engine::Serialize;

    ObjectGraphDocument document;
    const auto sceneId = MakeObjectId("SceneWithPrefabInstance");
    const auto prefabRootId = MakeObjectId("PrefabInstanceRoot");
    const auto addedId = MakeObjectId("PrefabInstanceAddedObject");
    const auto missingId = MakeObjectId("MissingPrefabInstanceAddedObject");

    auto sceneRoot = MakeRecord(sceneId, "SceneType");
    sceneRoot.properties.push_back({"gameObjects", PropertyValue::Array({
        PropertyValue::OwnedReference(prefabRootId)
    })});

    auto prefabRoot = MakeRecord(prefabRootId, "GameObjectType");
    prefabRoot.state = ObjectRecordState::Stripped;

    ObjectRecord added = MakeRecord(addedId, "GameObjectType");
    added.localIdentifierInFile = 0;
    added.properties.push_back({
        "parent",
        PropertyValue::ObjectReference(ObjectIdentifier::LocalObject(MakeLocalIdentifierInFile(missingId)))
    });

    PrefabInstanceRecord instance;
    instance.instanceRoot = prefabRootId;
    instance.sourcePrefab = ObjectIdentifier::Asset(
        AssetId(NLS::Guid::Parse("dddddddd-dddd-4ddd-9ddd-dddddddddddd")),
        100100000,
        "prefab:BrokenAddedObject");
    instance.addedObjects.push_back(std::move(added));
    instance.modifications.push_back(PatchOperation::InsertOwned(prefabRootId, "children", missingId, 0u));

    document.root = sceneId;
    document.objects.push_back(std::move(sceneRoot));
    document.objects.push_back(std::move(prefabRoot));
    document.prefabInstances.push_back(std::move(instance));

    const auto diagnostics = document.Validate();

    EXPECT_TRUE(diagnostics.HasErrors());
    EXPECT_TRUE(ContainsDiagnostic(diagnostics, SerializationDiagnosticCode::InvalidPropertyType));
    EXPECT_TRUE(ContainsDiagnostic(diagnostics, SerializationDiagnosticCode::MissingObject));
    EXPECT_TRUE(ContainsDiagnostic(diagnostics, SerializationDiagnosticCode::DanglingReference));
}

TEST(ObjectGraphDocumentTests, ValidatesOwnershipCyclesAndOrphans)
{
    using namespace NLS::Engine::Serialize;

    ObjectGraphDocument document;
    const auto rootId = MakeObjectId("OwnershipRoot");
    const auto childId = MakeObjectId("OwnershipChild");
    const auto orphanId = MakeObjectId("OwnershipOrphan");

    auto root = MakeRecord(rootId, "RootType");
    root.properties.push_back({"children", PropertyValue::Array({
        PropertyValue::OwnedReference(childId)
    })});

    auto child = MakeRecord(childId, "ChildType");
    child.properties.push_back({"parentCycle", PropertyValue::OwnedReference(rootId)});

    document.root = rootId;
    document.objects.push_back(std::move(root));
    document.objects.push_back(std::move(child));
    document.objects.push_back(MakeRecord(orphanId, "OrphanType"));

    const auto diagnostics = document.Validate();

    EXPECT_TRUE(diagnostics.HasErrors());
    EXPECT_TRUE(ContainsDiagnostic(diagnostics, SerializationDiagnosticCode::OwnershipCycle));
    EXPECT_TRUE(ContainsDiagnostic(diagnostics, SerializationDiagnosticCode::OrphanedOwnedObject));
}

TEST(ObjectGraphDocumentTests, WritesDeterministicObjectGraphJsonWithReferenceShapes)
{
    using namespace NLS::Engine::Serialize;

    const auto documentId = NLS::Guid::Parse("aaaaaaaa-aaaa-4aaa-aaaa-aaaaaaaaaaaa");
    const auto rootId = ObjectId(NLS::Guid::Parse("bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb"));
    const auto childId = ObjectId(NLS::Guid::Parse("cccccccc-cccc-4ccc-cccc-cccccccccccc"));
    const auto assetId = AssetId(NLS::Guid::Parse("dddddddd-dddd-4ddd-9ddd-dddddddddddd"));

    ObjectGraphDocument document;
    document.documentId = documentId;
    document.root = rootId;

    auto root = MakeRecord(rootId, "NLS::Engine::SceneSystem::Scene");
    root.debugName = "Main Scene";
    root.properties.push_back({"gameObjects", PropertyValue::Array({PropertyValue::OwnedReference(childId)})});
    document.objects.push_back(std::move(root));

    auto child = MakeRecord(childId, "NLS::Engine::GameObject");
    child.debugName = "Child";
    child.properties.push_back({"name", PropertyValue::String("Child")});
    const auto rootReference = document.TryMakeObjectReference(rootId);
    ASSERT_TRUE(rootReference.has_value());
    child.properties.push_back({
        "target",
        PropertyValue::ObjectReference(*rootReference)
    });
    child.properties.push_back({"material", PropertyValue::ObjectReference(ObjectIdentifier::Asset(
        assetId,
        MakeLocalIdentifierInFile(assetId.GetGuid(), "Assets/Materials/Default.mat"),
        "Assets/Materials/Default.mat"))});

    document.objects.push_back(std::move(child));

    const auto first = ObjectGraphWriter::Write(document);
    const auto second = ObjectGraphWriter::Write(document);

    EXPECT_EQ(first, second);
    EXPECT_NE(first.find("\"format\": \"Nullus.ObjectGraph.Scene\""), std::string::npos);
    EXPECT_NE(first.find("\"documentId\": \"aaaaaaaa-aaaa-4aaa-aaaa-aaaaaaaaaaaa\""), std::string::npos);
    EXPECT_LT(first.find("\"id\": \"bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb\""), first.find("\"id\": \"cccccccc-cccc-4ccc-cccc-cccccccccccc\""));
    EXPECT_NE(first.find("\"$owned\": \"cccccccc-cccc-4ccc-cccc-cccccccccccc\""), std::string::npos);
    EXPECT_NE(first.find("\"fileID\""), std::string::npos);
    EXPECT_NE(first.find("\"guid\": \"dddddddd-dddd-4ddd-9ddd-dddddddddddd\""), std::string::npos);
    EXPECT_NE(first.find("\"type\": 2"), std::string::npos);
    EXPECT_NE(first.find("\"filePath\": \"Assets/Materials/Default.mat\""), std::string::npos);
    EXPECT_EQ(first.find("\"expectedType\""), std::string::npos);
    EXPECT_EQ(first.find("\"pathHint\""), std::string::npos);
    EXPECT_EQ(first.find("\"$ref\""), std::string::npos);
    EXPECT_EQ(first.find("\"$asset\""), std::string::npos);
    EXPECT_EQ(first.find("worldID"), std::string::npos);
    EXPECT_EQ(first.find("SerializedSceneData"), std::string::npos);
    EXPECT_EQ(first.find("SerializedActorData"), std::string::npos);
    EXPECT_EQ(first.find("SerializedComponentData"), std::string::npos);
}

TEST(ObjectGraphDocumentTests, ReflectedPPtrSerializesAsUnityObjectReference)
{
    using namespace NLS::Engine::Serialize;

    PersistentManager::Instance().Clear();
    NLS::meta::ReflectionDatabase::Instance();
    Tests::ObjectReferenceSerializationFixture fixture;
    const auto identifier = ObjectIdentifier::Asset(
        AssetId(NLS::Guid::Parse("dddddddd-dddd-4ddd-9ddd-dddddddddddd")),
        MakeLocalIdentifierInFile(
            NLS::Guid::Parse("dddddddd-dddd-4ddd-9ddd-dddddddddddd"),
            "Assets/Materials/Default.mat"),
        "Assets/Materials/Default.mat");
    fixture.reference = PPtr<NLS::Render::Resources::Material>(
        PersistentManager::Instance().ObjectIdentifierToInstanceID(identifier));

    const auto objectId = ObjectId(NLS::Guid::Parse("bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb"));
    const auto record = ObjectGraphSerializer::SerializeObjectRecord(fixture, objectId);

    const auto reference = std::find_if(
        record.properties.begin(),
        record.properties.end(),
        [](const PropertyRecord& property)
        {
            return property.name == "reference";
        });

    ASSERT_NE(reference, record.properties.end());
    ASSERT_EQ(reference->value.GetKind(), PropertyValue::Kind::ObjectReference);
    EXPECT_NE(reference->value.GetObjectReference().localIdentifierInFile, 0);
    EXPECT_EQ(reference->value.GetObjectReference().guid.ToString(), "dddddddd-dddd-4ddd-9ddd-dddddddddddd");
    EXPECT_EQ(reference->value.GetObjectReference().fileType, FileType::SerializedAssetType);
    EXPECT_EQ(reference->value.GetObjectReference().filePath, "Assets/Materials/Default.mat");
}

TEST(ObjectGraphDocumentTests, ReflectedPPtrSerializesLiveObjectWithoutPersistentIdentifierAsEmptyReference)
{
    using namespace NLS::Engine::Serialize;

    PersistentManager::Instance().Clear();
    NLS::ObjectTestAccess::ClearObjectRegistry();
    NLS::meta::ReflectionDatabase::Instance();

    Tests::ObjectReferenceSerializationFixture fixture;
    NLS::Render::Resources::Material material;
    fixture.reference = PPtr<NLS::Render::Resources::Material>(&material);

    const auto record = ObjectGraphSerializer::SerializeObjectRecord(
        fixture,
        ObjectId(NLS::Guid::Parse("eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee")));

    const auto reference = std::find_if(
        record.properties.begin(),
        record.properties.end(),
        [](const PropertyRecord& property)
        {
            return property.name == "reference";
        });

    ASSERT_NE(reference, record.properties.end());
    ASSERT_EQ(reference->value.GetKind(), PropertyValue::Kind::ObjectReference);
    const auto& objectReference = reference->value.GetObjectReference();
    EXPECT_FALSE(objectReference.IsValid());
    EXPECT_EQ(objectReference.localIdentifierInFile, 0);
    EXPECT_FALSE(objectReference.guid.IsValid());
    EXPECT_EQ(objectReference.fileType, FileType::NonAssetType);
    EXPECT_TRUE(objectReference.filePath.empty());
    EXPECT_EQ(fixture.reference.Get(), &material);
}

TEST(ObjectGraphDocumentTests, ReflectedPPtrHandlerSerializesRegisteredObjectResourceTypes)
{
    using namespace NLS::Engine::Serialize;

    PersistentManager::Instance().Clear();
    Tests::AdditionalPPtrSerializationFixture fixture;

    const auto meshIdentifier = ObjectIdentifier::Asset(
        AssetId(NLS::Guid::Parse("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa")),
        4300000,
        "Assets/Meshes/Hero.mesh");
    const auto textureIdentifier = ObjectIdentifier::Asset(
        AssetId(NLS::Guid::Parse("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb")),
        2800000,
        "Assets/Textures/Hero.png");
    const auto baseTextureIdentifier = ObjectIdentifier::Asset(
        AssetId(NLS::Guid::Parse("dddddddd-dddd-4ddd-8ddd-dddddddddddd")),
        2800001,
        "Assets/Textures/Base.asset");
    const auto cubeMapIdentifier = ObjectIdentifier::Asset(
        AssetId(NLS::Guid::Parse("eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee")),
        8900000,
        "Assets/Textures/Skybox.cubemap");

    fixture.mesh = PPtr<NLS::Render::Resources::Mesh>(
        PersistentManager::Instance().ObjectIdentifierToInstanceID(meshIdentifier));
    fixture.texture = PPtr<NLS::Render::Resources::Texture>(
        PersistentManager::Instance().ObjectIdentifierToInstanceID(baseTextureIdentifier));
    fixture.textures.push_back(PPtr<NLS::Render::Resources::Texture2D>(
        PersistentManager::Instance().ObjectIdentifierToInstanceID(textureIdentifier)));
    fixture.cubeMaps.push_back(PPtr<NLS::Render::Resources::TextureCube>(
        PersistentManager::Instance().ObjectIdentifierToInstanceID(cubeMapIdentifier)));

    const auto record = ObjectGraphSerializer::SerializeObjectRecord(
        fixture,
        ObjectId(NLS::Guid::Parse("cccccccc-cccc-4ccc-8ccc-cccccccccccc")));

    const auto* mesh = FindProperty(record, "mesh");
    ASSERT_NE(mesh, nullptr);
    ASSERT_EQ(mesh->value.GetKind(), PropertyValue::Kind::ObjectReference);
    EXPECT_EQ(mesh->value.GetObjectReference(), meshIdentifier);

    const auto* texture = FindProperty(record, "texture");
    ASSERT_NE(texture, nullptr);
    ASSERT_EQ(texture->value.GetKind(), PropertyValue::Kind::ObjectReference);
    EXPECT_EQ(texture->value.GetObjectReference(), baseTextureIdentifier);

    const auto* textures = FindProperty(record, "textures");
    ASSERT_NE(textures, nullptr);
    ASSERT_EQ(textures->value.GetKind(), PropertyValue::Kind::Array);
    ASSERT_EQ(textures->value.GetArray().size(), 1u);
    ASSERT_EQ(textures->value.GetArray()[0].GetKind(), PropertyValue::Kind::ObjectReference);
    EXPECT_EQ(textures->value.GetArray()[0].GetObjectReference(), textureIdentifier);

    const auto* cubeMaps = FindProperty(record, "cubeMaps");
    ASSERT_NE(cubeMaps, nullptr);
    ASSERT_EQ(cubeMaps->value.GetKind(), PropertyValue::Kind::Array);
    ASSERT_EQ(cubeMaps->value.GetArray().size(), 1u);
    ASSERT_EQ(cubeMaps->value.GetArray()[0].GetKind(), PropertyValue::Kind::ObjectReference);
    EXPECT_EQ(cubeMaps->value.GetArray()[0].GetObjectReference(), cubeMapIdentifier);
}

TEST(ObjectGraphDocumentTests, WritesReadableIndentedObjectGraphJson)
{
    using namespace NLS::Engine::Serialize;

    ObjectGraphDocument document;
    document.documentId = NLS::Guid::Parse("aaaaaaaa-aaaa-4aaa-aaaa-aaaaaaaaaaaa");
    document.root = ObjectId(NLS::Guid::Parse("bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb"));
    document.objects.push_back(MakeRecord(document.root, "RootType"));

    const auto output = ObjectGraphWriter::Write(document);

    EXPECT_NE(output.find('\n'), std::string::npos);
    EXPECT_NE(output.find("    \"documentId\""), std::string::npos);
    EXPECT_EQ(output.back(), '\n');
}

TEST(ObjectGraphDocumentTests, RemoveObjectPatchOperationRoundTripsThroughObjectGraphJson)
{
    using namespace NLS::Engine::Serialize;

    ObjectGraphDocument document;
    document.documentId = NLS::Guid::Parse("aaaaaaaa-aaaa-4aaa-aaaa-aaaaaaaaaaaa");
    document.root = ObjectId(NLS::Guid::Parse("bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb"));
    const auto removedId = ObjectId(NLS::Guid::Parse("cccccccc-cccc-4ccc-cccc-cccccccccccc"));

    document.objects.push_back(MakeRecord(document.root, "RootType"));
    document.objects.push_back(MakeRecord(removedId, "ChildType"));

    PatchOperation removeObject;
    removeObject.type = PatchOperationType::RemoveObject;
    removeObject.target = removedId;
    document.overrides.push_back(removeObject);

    const auto output = ObjectGraphWriter::Write(document);

    EXPECT_NE(output.find("\"op\": \"removeObject\""), std::string::npos);
    EXPECT_NE(output.find("\"target\": \"cccccccc-cccc-4ccc-cccc-cccccccccccc\""), std::string::npos);
    EXPECT_EQ(output.find("\"op\": \"unknown\""), std::string::npos);

    const auto loaded = ObjectGraphReader::Read(output);
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->overrides.size(), 1u);
    EXPECT_EQ(loaded->overrides[0].type, PatchOperationType::RemoveObject);
    EXPECT_EQ(loaded->overrides[0].target.GetGuid().ToString(), "cccccccc-cccc-4ccc-cccc-cccccccccccc");
}

TEST(ObjectGraphDocumentTests, ReadsObjectGraphJsonBackIntoDocumentModel)
{
    using namespace NLS::Engine::Serialize;

    const std::string json = R"({
  "documentId": "aaaaaaaa-aaaa-4aaa-aaaa-aaaaaaaaaaaa",
  "format": "Nullus.ObjectGraph.Scene",
  "objects": [
    {
      "debugName": "Root",
      "fileID": 12345,
      "id": "bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb",
      "properties": {
        "children": [
          {
            "$owned": "cccccccc-cccc-4ccc-cccc-cccccccccccc"
          }
        ]
      },
      "state": "Alive",
      "type": "RootType"
    },
    {
      "fileID": 67890,
      "id": "cccccccc-cccc-4ccc-cccc-cccccccccccc",
      "properties": {
        "parent": {
          "fileID": 12345,
          "guid": "",
          "type": 0
        }
      },
      "state": "Alive",
      "type": "ChildType"
    }
  ],
  "root": "bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb",
  "version": 1
})";

    auto result = ObjectGraphReader::Read(json);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->documentId.ToString(), "aaaaaaaa-aaaa-4aaa-aaaa-aaaaaaaaaaaa");
    EXPECT_EQ(result->root.GetGuid().ToString(), "bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb");
    ASSERT_EQ(result->objects.size(), 2u);
    EXPECT_EQ(result->objects[0].properties[0].value.GetKind(), PropertyValue::Kind::Array);
    EXPECT_EQ(result->objects[0].properties[0].value.GetArray()[0].GetKind(), PropertyValue::Kind::OwnedReference);
    EXPECT_EQ(result->objects[1].properties[0].value.GetKind(), PropertyValue::Kind::ObjectReference);
    const auto resolvedParent = result->ResolveObjectReference(
        result->objects[1].properties[0].value.GetObjectReference());
    ASSERT_TRUE(resolvedParent.has_value());
    EXPECT_EQ(resolvedParent->GetGuid().ToString(), "bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb");
    EXPECT_FALSE(result->Validate().HasErrors());
}

TEST(ObjectGraphDocumentTests, ReaderRejectsObjectRecordsWithoutFileID)
{
    using namespace NLS::Engine::Serialize;

    const std::string json = R"({
  "documentId": "aaaaaaaa-aaaa-4aaa-aaaa-aaaaaaaaaaaa",
  "format": "Nullus.ObjectGraph.Scene",
  "objects": [
    {
      "id": "bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb",
      "properties": {},
      "state": "Alive",
      "type": "RootType"
    }
  ],
  "root": "bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb",
  "version": 1
})";

    EXPECT_FALSE(ObjectGraphReader::Read(json).has_value());
}

TEST(ObjectGraphDocumentTests, ReaderRejectsUnknownObjectRecordState)
{
    using namespace NLS::Engine::Serialize;

    const std::string json = R"({
  "documentId": "aaaaaaaa-aaaa-4aaa-aaaa-aaaaaaaaaaaa",
  "format": "Nullus.ObjectGraph.Scene",
  "objects": [
    {
      "fileID": 12345,
      "id": "bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb",
      "properties": {},
      "state": "PreviewOnly",
      "type": "RootType"
    }
  ],
  "root": "bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb",
  "version": 1
})";

    EXPECT_FALSE(ObjectGraphReader::Read(json).has_value());
}

TEST(ObjectGraphDocumentTests, ReaderRejectsUnknownObjectIdentifierFileTypeValue)
{
    using namespace NLS::Engine::Serialize;

    const std::string json = R"({
  "documentId": "aaaaaaaa-aaaa-4aaa-aaaa-aaaaaaaaaaaa",
  "format": "Nullus.ObjectGraph.Scene",
  "objects": [
    {
      "fileID": 12345,
      "id": "bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb",
      "properties": {
        "asset": {
          "fileID": 0,
          "guid": "dddddddd-dddd-4ddd-9ddd-dddddddddddd",
          "type": 99
        }
      },
      "state": "Alive",
      "type": "RootType"
    }
  ],
  "root": "bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb",
  "version": 1
})";

    EXPECT_FALSE(ObjectGraphReader::Read(json).has_value());
}

TEST(ObjectGraphDocumentTests, ReaderRejectsAssetObjectReferenceWithoutFileID)
{
    using namespace NLS::Engine::Serialize;

    const std::string json = R"({
  "documentId": "aaaaaaaa-aaaa-4aaa-aaaa-aaaaaaaaaaaa",
  "format": "Nullus.ObjectGraph.Scene",
  "objects": [
    {
      "fileID": 12345,
      "id": "bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb",
      "properties": {
        "asset": {
          "fileID": 0,
          "guid": "dddddddd-dddd-4ddd-9ddd-dddddddddddd",
          "type": 2
        }
      },
      "state": "Alive",
      "type": "RootType"
    }
  ],
  "root": "bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb",
  "version": 1
})";

    EXPECT_FALSE(ObjectGraphReader::Read(json).has_value());
}

TEST(ObjectGraphDocumentTests, ReaderRejectsLegacyDollarRefObjectReferenceShape)
{
    using namespace NLS::Engine::Serialize;

    const std::string json = R"({
  "documentId": "aaaaaaaa-aaaa-4aaa-aaaa-aaaaaaaaaaaa",
  "format": "Nullus.ObjectGraph.Scene",
  "objects": [
    {
      "fileID": 12345,
      "id": "bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb",
      "properties": {
        "target": {
          "$ref": "cccccccc-cccc-4ccc-8ccc-cccccccccccc"
        }
      },
      "state": "Alive",
      "type": "RootType"
    }
  ],
  "root": "bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb",
  "version": 1
})";

    EXPECT_FALSE(ObjectGraphReader::Read(json).has_value());
}

TEST(ObjectGraphDocumentTests, ReaderRejectsLegacyDollarAssetObjectReferenceShape)
{
    using namespace NLS::Engine::Serialize;

    const std::string json = R"({
  "documentId": "aaaaaaaa-aaaa-4aaa-aaaa-aaaaaaaaaaaa",
  "format": "Nullus.ObjectGraph.Scene",
  "objects": [
    {
      "fileID": 12345,
      "id": "bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb",
      "properties": {
        "material": {
          "$asset": "dddddddd-dddd-4ddd-9ddd-dddddddddddd",
          "type": "Material",
          "pathHint": "Assets/Materials/Default.mat"
        }
      },
      "state": "Alive",
      "type": "RootType"
    }
  ],
  "root": "bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb",
  "version": 1
})";

    EXPECT_FALSE(ObjectGraphReader::Read(json).has_value());
}

TEST(ObjectGraphDocumentTests, ReaderRejectsHalfShapedObjectReferenceWithoutFileID)
{
    using namespace NLS::Engine::Serialize;

    const std::string json = R"({
  "documentId": "aaaaaaaa-aaaa-4aaa-aaaa-aaaaaaaaaaaa",
  "format": "Nullus.ObjectGraph.Scene",
  "objects": [
    {
      "fileID": 12345,
      "id": "bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb",
      "properties": {
        "material": {
          "guid": "dddddddd-dddd-4ddd-9ddd-dddddddddddd",
          "type": 2,
          "filePath": "Assets/Materials/Default.mat"
        }
      },
      "state": "Alive",
      "type": "RootType"
    }
  ],
  "root": "bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb",
  "version": 1
})";

    EXPECT_FALSE(ObjectGraphReader::Read(json).has_value());
}

TEST(ObjectGraphDocumentTests, ReaderRejectsGuidOnlyObjectReferenceLikeShape)
{
    using namespace NLS::Engine::Serialize;

    const std::string json = R"({
  "documentId": "aaaaaaaa-aaaa-4aaa-aaaa-aaaaaaaaaaaa",
  "format": "Nullus.ObjectGraph.Scene",
  "objects": [
    {
      "fileID": 12345,
      "id": "bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb",
      "properties": {
        "material": {
          "guid": "dddddddd-dddd-4ddd-9ddd-dddddddddddd",
          "type": "Material"
        }
      },
      "state": "Alive",
      "type": "RootType"
    }
  ],
  "root": "bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb",
  "version": 1
})";

    EXPECT_FALSE(ObjectGraphReader::Read(json).has_value());
}

TEST(ObjectGraphDocumentTests, ReaderRejectsObjectReferenceWithNonIntegerFileID)
{
    using namespace NLS::Engine::Serialize;

    const std::string json = R"({
  "documentId": "aaaaaaaa-aaaa-4aaa-aaaa-aaaaaaaaaaaa",
  "format": "Nullus.ObjectGraph.Scene",
  "objects": [
    {
      "fileID": 12345,
      "id": "bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb",
      "properties": {
        "material": {
          "fileID": "2100000",
          "guid": "dddddddd-dddd-4ddd-9ddd-dddddddddddd",
          "type": 2,
          "filePath": "Assets/Materials/Default.mat"
        }
      },
      "state": "Alive",
      "type": "RootType"
    }
  ],
  "root": "bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb",
  "version": 1
})";

    EXPECT_FALSE(ObjectGraphReader::Read(json).has_value());
}

TEST(ObjectGraphDocumentTests, WriterRejectsInvalidAssetObjectReferences)
{
    using namespace NLS::Engine::Serialize;

    ObjectGraphDocument document;
    const auto rootId = MakeObjectId("WriterRejectsInvalidAssetRoot");
    auto root = MakeRecord(rootId, "RootType");
    root.properties.push_back({"material", PropertyValue::ObjectReference(ObjectIdentifier::Asset(
        AssetId(NLS::Guid::Parse("dddddddd-dddd-4ddd-9ddd-dddddddddddd")),
        0,
        "Assets/Materials/Broken.mat"))});

    document.root = rootId;
    document.objects.push_back(std::move(root));

    EXPECT_THROW(
        (void)ObjectGraphWriter::Write(document),
        std::invalid_argument);
}

TEST(ObjectGraphDocumentTests, WriterRejectsUnknownObjectIdentifierFileTypeValue)
{
    using namespace NLS::Engine::Serialize;

    ObjectGraphDocument document;
    const auto rootId = MakeObjectId("WriterRejectsUnknownFileTypeRoot");
    auto root = MakeRecord(rootId, "RootType");
    auto reference = ObjectIdentifier::Asset(
        AssetId(NLS::Guid::Parse("dddddddd-dddd-4ddd-9ddd-dddddddddddd")),
        MakeLocalIdentifierInFile(
            NLS::Guid::Parse("dddddddd-dddd-4ddd-9ddd-dddddddddddd"),
            "Assets/Materials/Broken.mat"),
        "Assets/Materials/Broken.mat");
    reference.fileType = static_cast<FileType>(99);
    root.properties.push_back({"material", PropertyValue::ObjectReference(reference)});

    document.root = rootId;
    document.objects.push_back(std::move(root));

    EXPECT_THROW(
        (void)ObjectGraphWriter::Write(document),
        std::invalid_argument);
}

TEST(ObjectGraphDocumentTests, ReaderRejectsLocalObjectReferenceWithFilePath)
{
    using namespace NLS::Engine::Serialize;

    const std::string json = R"({
  "documentId": "aaaaaaaa-aaaa-4aaa-aaaa-aaaaaaaaaaaa",
  "format": "Nullus.ObjectGraph.Scene",
  "objects": [
    {
      "fileID": 12345,
      "id": "bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb",
      "properties": {
        "target": {
          "fileID": 12345,
          "guid": "",
          "type": 0,
          "filePath": "Assets/Materials/Default.mat"
        }
      },
      "state": "Alive",
      "type": "RootType"
    }
  ],
  "root": "bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb",
  "version": 1
})";

    EXPECT_FALSE(ObjectGraphReader::Read(json).has_value());
}

TEST(ObjectGraphDocumentTests, ValidatesInvalidAssetReferences)
{
    using namespace NLS::Engine::Serialize;

    ObjectGraphDocument document;
    const auto rootId = MakeObjectId("AssetReferenceRoot");
    auto root = MakeRecord(rootId, "RootType");
    root.properties.push_back({"material", PropertyValue::ObjectReference(
        ObjectIdentifier::Asset(AssetId(), 1, "Assets/Materials/Missing.mat"))});

    document.root = rootId;
    document.objects.push_back(std::move(root));

    const auto diagnostics = document.Validate();

    EXPECT_TRUE(diagnostics.HasErrors());
    EXPECT_TRUE(ContainsDiagnostic(diagnostics, SerializationDiagnosticCode::MissingAsset));
}

TEST(ObjectGraphDocumentTests, ValidatesExternalAssetReferencesDoNotUseNonAssetFileType)
{
    using namespace NLS::Engine::Serialize;

    ObjectGraphDocument document;
    const auto rootId = MakeObjectId("AssetReferenceRootWithWrongFileType");
    auto root = MakeRecord(rootId, "RootType");
    auto reference = ObjectIdentifier::Asset(
        AssetId(NLS::Guid::Parse("dddddddd-dddd-4ddd-9ddd-dddddddddddd")),
        MakeLocalIdentifierInFile(
            NLS::Guid::Parse("dddddddd-dddd-4ddd-9ddd-dddddddddddd"),
            "Assets/Materials/Default.mat"),
        "Assets/Materials/Default.mat");
    reference.fileType = FileType::NonAssetType;
    root.properties.push_back({"material", PropertyValue::ObjectReference(reference)});

    document.root = rootId;
    document.objects.push_back(std::move(root));

    const auto diagnostics = document.Validate();

    EXPECT_TRUE(diagnostics.HasErrors());
    EXPECT_TRUE(ContainsDiagnostic(diagnostics, SerializationDiagnosticCode::MissingAsset));
}

TEST(ObjectGraphDocumentTests, ValidatesExternalAssetReferencesRequireFileID)
{
    using namespace NLS::Engine::Serialize;

    ObjectGraphDocument document;
    const auto rootId = MakeObjectId("AssetReferenceRootWithoutFileID");
    auto root = MakeRecord(rootId, "RootType");
    root.properties.push_back({"material", PropertyValue::ObjectReference(ObjectIdentifier::Asset(
        AssetId(NLS::Guid::Parse("dddddddd-dddd-4ddd-9ddd-dddddddddddd")),
        0,
        "Assets/Materials/Default.mat"))});

    document.root = rootId;
    document.objects.push_back(std::move(root));

    const auto diagnostics = document.Validate();

    EXPECT_TRUE(diagnostics.HasErrors());
    EXPECT_TRUE(ContainsDiagnostic(diagnostics, SerializationDiagnosticCode::MissingAsset));
}

TEST(ObjectGraphDocumentTests, ValidatesLocalObjectReferencesDoNotCarryFilePath)
{
    using namespace NLS::Engine::Serialize;

    ObjectGraphDocument document;
    const auto rootId = MakeObjectId("LocalReferenceRootWithPath");
    auto root = MakeRecord(rootId, "RootType");
    auto reference = ObjectIdentifier::LocalObject(root.localIdentifierInFile);
    reference.filePath = "Assets/Materials/Default.mat";
    root.properties.push_back({"target", PropertyValue::ObjectReference(reference)});

    document.root = rootId;
    document.objects.push_back(std::move(root));

    const auto diagnostics = document.Validate();

    EXPECT_TRUE(diagnostics.HasErrors());
    EXPECT_TRUE(ContainsDiagnostic(diagnostics, SerializationDiagnosticCode::MissingAsset));
}

TEST(ObjectGraphDocumentTests, PreservesUnknownTypeRecordsThroughReaderWriterRoundTrip)
{
    using namespace NLS::Engine::Serialize;

    const std::string json = R"({
  "documentId": "aaaaaaaa-aaaa-4aaa-aaaa-aaaaaaaaaaaa",
  "format": "Nullus.ObjectGraph.Scene",
  "objects": [
    {
      "fileID": 12345,
      "id": "bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb",
      "properties": {
        "gameObjects": [
          {
            "$owned": "cccccccc-cccc-4ccc-cccc-cccccccccccc"
          }
        ]
      },
      "state": "Alive",
      "type": "NLS::Engine::SceneSystem::Scene"
    },
    {
      "debugName": "Plugin Component",
      "fileID": 67890,
      "id": "cccccccc-cccc-4ccc-cccc-cccccccccccc",
      "properties": {
        "asset": {
          "fileID": 24680,
          "guid": "dddddddd-dddd-4ddd-9ddd-dddddddddddd",
          "type": 2,
          "filePath": "Assets/Plugin/Missing.asset"
        },
        "rawNumber": 7
      },
      "state": "Alive",
      "type": "Plugin.UnknownComponent"
    }
  ],
  "root": "bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb",
  "version": 1
})";

    const auto loaded = ObjectGraphReader::Read(json);
    ASSERT_TRUE(loaded.has_value());

    const auto roundTripped = ObjectGraphWriter::Write(*loaded);
    const auto loadedAgain = ObjectGraphReader::Read(roundTripped);
    ASSERT_TRUE(loadedAgain.has_value());
    ASSERT_EQ(loadedAgain->objects.size(), 2u);

    const auto& unknown = loadedAgain->objects[1];
    EXPECT_EQ(unknown.typeName, "Plugin.UnknownComponent");
    EXPECT_EQ(unknown.debugName, "Plugin Component");
    ASSERT_EQ(unknown.properties.size(), 2u);
    EXPECT_EQ(unknown.properties[0].name, "asset");
    EXPECT_EQ(unknown.properties[0].value.GetKind(), PropertyValue::Kind::ObjectReference);
    EXPECT_EQ(unknown.properties[0].value.GetObjectReference().guid.ToString(), "dddddddd-dddd-4ddd-9ddd-dddddddddddd");
}
