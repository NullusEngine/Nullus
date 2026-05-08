#include <gtest/gtest.h>

#include "Serialize/ObjectGraphDocument.h"
#include "Serialize/ObjectGraphReader.h"
#include "Serialize/ObjectGraphWriter.h"
#include "Serialize/ObjectId.h"

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
    record.typeName = type;
    return record;
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
    root.properties.push_back({"target", PropertyValue::ObjectReference(missingId)});
    document.root = rootId;
    document.objects.push_back(std::move(root));

    const auto diagnostics = document.Validate();

    EXPECT_TRUE(diagnostics.HasErrors());
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

    auto child = MakeRecord(childId, "NLS::Engine::GameObject");
    child.debugName = "Child";
    child.properties.push_back({"name", PropertyValue::String("Child")});
    child.properties.push_back({"target", PropertyValue::ObjectReference(rootId)});
    child.properties.push_back({"material", PropertyValue::AssetReference({assetId, "Material", "Assets/Materials/Default.mat"})});

    auto root = MakeRecord(rootId, "NLS::Engine::SceneSystem::Scene");
    root.debugName = "Main Scene";
    root.properties.push_back({"gameObjects", PropertyValue::Array({PropertyValue::OwnedReference(childId)})});

    document.objects.push_back(std::move(child));
    document.objects.push_back(std::move(root));

    const auto first = ObjectGraphWriter::Write(document);
    const auto second = ObjectGraphWriter::Write(document);

    EXPECT_EQ(first, second);
    EXPECT_NE(first.find("\"format\": \"Nullus.ObjectGraph.Scene\""), std::string::npos);
    EXPECT_NE(first.find("\"documentId\": \"aaaaaaaa-aaaa-4aaa-aaaa-aaaaaaaaaaaa\""), std::string::npos);
    EXPECT_LT(first.find("\"id\": \"bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb\""), first.find("\"id\": \"cccccccc-cccc-4ccc-cccc-cccccccccccc\""));
    EXPECT_NE(first.find("\"$owned\": \"cccccccc-cccc-4ccc-cccc-cccccccccccc\""), std::string::npos);
    EXPECT_NE(first.find("\"$ref\": \"bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb\""), std::string::npos);
    EXPECT_NE(first.find("\"$asset\": \"dddddddd-dddd-4ddd-9ddd-dddddddddddd\""), std::string::npos);
    EXPECT_EQ(first.find("worldID"), std::string::npos);
    EXPECT_EQ(first.find("SerializedSceneData"), std::string::npos);
    EXPECT_EQ(first.find("SerializedActorData"), std::string::npos);
    EXPECT_EQ(first.find("SerializedComponentData"), std::string::npos);
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

TEST(ObjectGraphDocumentTests, ReadsObjectGraphJsonBackIntoDocumentModel)
{
    using namespace NLS::Engine::Serialize;

    const std::string json = R"({
  "documentId": "aaaaaaaa-aaaa-4aaa-aaaa-aaaaaaaaaaaa",
  "format": "Nullus.ObjectGraph.Scene",
  "objects": [
    {
      "debugName": "Root",
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
      "id": "cccccccc-cccc-4ccc-cccc-cccccccccccc",
      "properties": {
        "parent": {
          "$ref": "bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb"
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
    EXPECT_FALSE(result->Validate().HasErrors());
}

TEST(ObjectGraphDocumentTests, ValidatesInvalidAssetReferences)
{
    using namespace NLS::Engine::Serialize;

    ObjectGraphDocument document;
    const auto rootId = MakeObjectId("AssetReferenceRoot");
    auto root = MakeRecord(rootId, "RootType");
    root.properties.push_back({"material", PropertyValue::AssetReference({
        AssetId(),
        "Material",
        "Assets/Materials/Missing.mat"
    })});

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
      "id": "cccccccc-cccc-4ccc-cccc-cccccccccccc",
      "properties": {
        "asset": {
          "$asset": "dddddddd-dddd-4ddd-9ddd-dddddddddddd",
          "pathHint": "Assets/Plugin/Missing.asset",
          "type": "PluginAsset"
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
    EXPECT_EQ(unknown.properties[0].value.GetKind(), PropertyValue::Kind::AssetReference);
    EXPECT_EQ(unknown.properties[0].value.GetAssetReference().asset.GetGuid().ToString(), "dddddddd-dddd-4ddd-9ddd-dddddddddddd");
}
