#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include "Components/LightComponent.h"
#include "Components/MeshRenderer.h"
#include "Components/MeshFilter.h"
#include "Components/TransformComponent.h"
#include "Core/ResourceManagement/MaterialManager.h"
#include "Core/ResourceManagement/MeshManager.h"
#include "Core/ServiceLocator.h"
#include "Engine/Assets/RuntimeAssetDatabase.h"
#include "GameObject.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/Mesh.h"
#include "SceneSystem/Scene.h"
#include "Serialize/ObjectGraphInstantiator.h"
#include "Serialize/ObjectGraphBinaryCache.h"
#include "Serialize/ObjectGraphReader.h"
#include "Serialize/ObjectGraphSerializer.h"
#include "Serialize/ObjectGraphWriter.h"
#include "Serialize/PrefabDocument.h"

namespace
{
struct ComponentAddedOwnerState
{
    bool observed = false;
    bool observedActive = true;
    std::string observedName;
    std::string observedTag;
    std::string observedSourceObjectKey;
};

std::string ReadTextFile(const std::filesystem::path& path)
{
    std::ifstream file(path);
    std::ostringstream stream;
    stream << file.rdbuf();
    return stream.str();
}

const NLS::Engine::Serialize::PropertyRecord* FindProperty(
    const NLS::Engine::Serialize::ObjectRecord& record,
    const char* name)
{
    for (const auto& property : record.properties)
    {
        if (property.name == name)
            return &property;
    }
    return nullptr;
}

NLS::Engine::Serialize::ObjectRecord* FindRecord(
    NLS::Engine::Serialize::ObjectGraphDocument& document,
    NLS::Engine::Serialize::ObjectId id)
{
    for (auto& record : document.objects)
    {
        if (record.id == id)
            return &record;
    }
    return nullptr;
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

template <typename T>
NLS::Engine::Serialize::ObjectIdentifier ResolveObjectIdentifier(
    const NLS::Engine::Serialize::PPtr<T>& reference)
{
    NLS::Engine::Serialize::ObjectIdentifier identifier;
    EXPECT_TRUE(NLS::Engine::Serialize::PersistentManager::Instance().InstanceIDToObjectIdentifier(
        reference.GetInstanceID(),
        identifier));
    return identifier;
}
}

TEST(PrefabObjectGraphSerializationTests, PrefabDocumentSavesAndLoadsGameObjectRoot)
{
    using namespace NLS::Engine;
    using namespace NLS::Engine::Serialize;

    GameObject prefabRoot("Lamp", "Prop");
    prefabRoot.AddComponent<Components::LightComponent>()->SetIntensity(2.0f);

    const auto prefab = ObjectGraphSerializer::SerializePrefab(prefabRoot);
    const auto text = ObjectGraphWriter::Write(prefab.graph);
    const auto loaded = ObjectGraphReader::Read(text);

    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->format, "Nullus.ObjectGraph.Prefab");
    ASSERT_FALSE(loaded->Validate().HasErrors());

    const auto* root = FindRecord(const_cast<ObjectGraphDocument&>(*loaded), loaded->root);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->typeName, "NLS::Engine::GameObject");
    ASSERT_NE(FindProperty(*root, "components"), nullptr);
}

TEST(PrefabObjectGraphSerializationTests, PrefabObjectGraphMatchesGoldenOutput)
{
    NLS::Engine::GameObject prefabRoot("Lamp", "Prop");
    prefabRoot.AddComponent<NLS::Engine::Components::LightComponent>()->SetIntensity(2.0f);

    const auto prefab = NLS::Engine::Serialize::ObjectGraphSerializer::SerializePrefab(prefabRoot);
    const auto output = NLS::Engine::Serialize::ObjectGraphWriter::Write(prefab.graph);
    const auto goldenPath =
        std::filesystem::path(NLS_ROOT_DIR) /
        "Tests/Unit/Fixtures/ObjectGraph/simple_prefab.objectgraph.json";

    EXPECT_EQ(output, ReadTextFile(goldenPath));
}

TEST(PrefabObjectGraphSerializationTests, CompactPrefabObjectGraphRoundTripsWithoutPrettyWhitespace)
{
    NLS::Engine::GameObject prefabRoot("Lamp", "Prop");
    prefabRoot.AddComponent<NLS::Engine::Components::LightComponent>()->SetIntensity(2.0f);

    const auto prefab = NLS::Engine::Serialize::ObjectGraphSerializer::SerializePrefab(prefabRoot);
    const auto pretty = NLS::Engine::Serialize::ObjectGraphWriter::Write(prefab.graph);
    const auto compact = NLS::Engine::Serialize::ObjectGraphWriter::WriteCompact(prefab.graph);

    EXPECT_LT(compact.size(), pretty.size());
    EXPECT_EQ(compact.find("\n    "), std::string::npos);

    const auto loaded = NLS::Engine::Serialize::ObjectGraphReader::Read(compact);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->root, prefab.graph.root);
    EXPECT_EQ(loaded->objects.size(), prefab.graph.objects.size());
}

TEST(PrefabObjectGraphSerializationTests, BinaryCachePrefabObjectGraphRoundTrips)
{
    NLS::Engine::GameObject prefabRoot("BinaryLamp", "Prop");
    prefabRoot.AddComponent<NLS::Engine::Components::LightComponent>()->SetIntensity(3.0f);

    auto prefab = NLS::Engine::Serialize::ObjectGraphSerializer::SerializePrefab(prefabRoot);
    prefab.graph.basePrefab = NLS::Engine::Serialize::ObjectIdentifier::Asset(
        NLS::Engine::Serialize::AssetId(NLS::Guid::Parse("34343434-3434-4434-8434-343434343434")),
        NLS::Engine::Serialize::MakeLocalIdentifierInFile(
            NLS::Guid::Parse("34343434-3434-4434-8434-343434343434"),
            "Assets/Prefabs/BinaryBase.prefab"),
        "Assets/Prefabs/BinaryBase.prefab");
    prefab.graph.overrides.push_back(NLS::Engine::Serialize::PatchOperation::ReplaceProperty(
        prefab.graph.root,
        "name",
        NLS::Engine::Serialize::PropertyValue::String("BinaryVariant")));

    const auto bytes = NLS::Engine::Serialize::ObjectGraphBinaryCache::Write(prefab.graph);
    ASSERT_FALSE(bytes.empty());

    const auto decoded = NLS::Engine::Serialize::ObjectGraphBinaryCache::Read(bytes.data(), bytes.size());
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(
        NLS::Engine::Serialize::ObjectGraphWriter::WriteCompact(*decoded),
        NLS::Engine::Serialize::ObjectGraphWriter::WriteCompact(prefab.graph));
}

TEST(PrefabObjectGraphSerializationTests, BinaryCacheRejectsMalformedData)
{
    const std::vector<uint8_t> invalidMagic = {'N', 'O', 'P', 'E'};
    EXPECT_FALSE(NLS::Engine::Serialize::ObjectGraphBinaryCache::Read(
        invalidMagic.data(),
        invalidMagic.size()).has_value());

    NLS::Engine::GameObject prefabRoot("BinaryLamp", "Prop");
    const auto prefab = NLS::Engine::Serialize::ObjectGraphSerializer::SerializePrefab(prefabRoot);
    auto bytes = NLS::Engine::Serialize::ObjectGraphBinaryCache::Write(prefab.graph);
    ASSERT_GT(bytes.size(), 8u);
    bytes.resize(bytes.size() - 3u);

    EXPECT_FALSE(NLS::Engine::Serialize::ObjectGraphBinaryCache::Read(
        bytes.data(),
        bytes.size()).has_value());
}

TEST(PrefabObjectGraphSerializationTests, BinaryCacheRejectsImpossibleVectorCountWithoutThrowing)
{
    NLS::Engine::GameObject prefabRoot("BinaryLamp", "Prop");
    const auto prefab = NLS::Engine::Serialize::ObjectGraphSerializer::SerializePrefab(prefabRoot);
    auto bytes = NLS::Engine::Serialize::ObjectGraphBinaryCache::Write(prefab.graph);
    ASSERT_GT(bytes.size(), 64u);

    auto readU32 = [&bytes](const size_t offset)
    {
        return static_cast<uint32_t>(bytes[offset]) |
            (static_cast<uint32_t>(bytes[offset + 1u]) << 8u) |
            (static_cast<uint32_t>(bytes[offset + 2u]) << 16u) |
            (static_cast<uint32_t>(bytes[offset + 3u]) << 24u);
    };

    constexpr size_t magicSize = 4u;
    constexpr size_t cacheVersionSize = 4u;
    constexpr size_t stringLengthSize = 4u;
    constexpr size_t documentVersionSize = 4u;
    constexpr size_t guidSize = 16u;
    constexpr size_t boolSize = 1u;
    const auto formatLengthOffset = magicSize + cacheVersionSize;
    const auto formatLength = readU32(formatLengthOffset);
    const auto objectCountOffset =
        formatLengthOffset +
        stringLengthSize +
        formatLength +
        documentVersionSize +
        guidSize +
        guidSize +
        boolSize;
    ASSERT_LE(objectCountOffset + 4u, bytes.size());

    bytes[objectCountOffset] = 0xffu;
    bytes[objectCountOffset + 1u] = 0xffu;
    bytes[objectCountOffset + 2u] = 0xffu;
    bytes[objectCountOffset + 3u] = 0xffu;

    std::optional<NLS::Engine::Serialize::ObjectGraphDocument> decoded;
    EXPECT_NO_THROW(decoded = NLS::Engine::Serialize::ObjectGraphBinaryCache::Read(bytes));
    EXPECT_FALSE(decoded.has_value());
}

TEST(PrefabObjectGraphSerializationTests, PrefabInstantiatesWithNewObjectIdsAndSourceToInstanceMapping)
{
    using namespace NLS::Engine;
    using namespace NLS::Engine::Components;
    using namespace NLS::Engine::Serialize;

    GameObject prefabRoot("Lamp", "Prop");
    prefabRoot.AddComponent<LightComponent>()->SetIntensity(4.0f);

    const auto prefab = ObjectGraphSerializer::SerializePrefab(prefabRoot);
    SceneSystem::Scene scene;
    const auto result = ObjectGraphInstantiator::InstantiatePrefab(prefab, scene);

    ASSERT_NE(result.root, nullptr);
    EXPECT_EQ(result.root->GetName(), "Lamp");
    EXPECT_EQ(result.root->GetTag(), "Prop");
    ASSERT_EQ(scene.GetGameObjects().size(), 1u);
    EXPECT_EQ(scene.GetGameObjects()[0], result.root);

    ASSERT_GE(result.sourceToInstance.size(), 2u);
    for (const auto& mapping : result.sourceToInstance)
    {
        EXPECT_TRUE(mapping.first.IsValid());
        EXPECT_TRUE(mapping.second.IsValid());
        EXPECT_NE(mapping.first, mapping.second);
    }

    auto* light = result.root->GetComponent<LightComponent>();
    ASSERT_NE(light, nullptr);
    EXPECT_EQ(light->gameobject(), result.root);
    EXPECT_FLOAT_EQ(light->GetIntensity(), 4.0f);
}

TEST(PrefabObjectGraphSerializationTests, PrefabOverridesReplaceInsertRemoveAndMoveOwnedObjects)
{
    using namespace NLS::Engine;
    using namespace NLS::Engine::Components;
    using namespace NLS::Engine::Serialize;

    GameObject prefabRoot("Base", "Prop");
    prefabRoot.AddComponent<LightComponent>()->SetIntensity(1.0f);

    auto prefab = ObjectGraphSerializer::SerializePrefab(prefabRoot);
    auto* rootRecord = FindRecord(prefab.graph, prefab.graph.root);
    ASSERT_NE(rootRecord, nullptr);
    const auto* componentsProperty = FindProperty(*rootRecord, "components");
    ASSERT_NE(componentsProperty, nullptr);
    ASSERT_EQ(componentsProperty->value.GetKind(), PropertyValue::Kind::Array);
    ASSERT_EQ(componentsProperty->value.GetArray().size(), 2u);

    const auto transformSourceId = componentsProperty->value.GetArray()[0].GetObjectId();
    const auto lightSourceId = componentsProperty->value.GetArray()[1].GetObjectId();
    const auto insertedLightId = ObjectId(NLS::Guid::NewDeterministic("Prefab.Inserted.Light"));

    ObjectRecord insertedLight;
    insertedLight.id = insertedLightId;
    insertedLight.localIdentifierInFile = MakeLocalIdentifierInFile(insertedLightId);
    insertedLight.typeName = "NLS::Engine::Components::LightComponent";
    insertedLight.properties.push_back({"intensity", PropertyValue::Number(8.0)});
    prefab.graph.objects.push_back(std::move(insertedLight));

    prefab.graph.overrides.push_back(PatchOperation::ReplaceProperty(prefab.graph.root, "name", PropertyValue::String("Variant")));
    prefab.graph.overrides.push_back(PatchOperation::InsertOwned(prefab.graph.root, "components", insertedLightId, 1));
    prefab.graph.overrides.push_back(PatchOperation::RemoveOwned(prefab.graph.root, "components", lightSourceId));
    prefab.graph.overrides.push_back(PatchOperation::MoveOwned(prefab.graph.root, "components", transformSourceId, 1));

    SceneSystem::Scene scene;
    const auto result = ObjectGraphInstantiator::InstantiatePrefab(prefab, scene);

    ASSERT_NE(result.root, nullptr);
    EXPECT_EQ(result.root->GetName(), "Variant");

    const auto& components = result.root->GetComponents();
    ASSERT_EQ(components.size(), 2u);
    auto* loadedInsertedLight = dynamic_cast<LightComponent*>(components[0].get());
    ASSERT_NE(loadedInsertedLight, nullptr);
    EXPECT_FLOAT_EQ(loadedInsertedLight->GetIntensity(), 8.0f);
    EXPECT_NE(dynamic_cast<TransformComponent*>(components[1].get()), nullptr);
    EXPECT_EQ(result.root->GetComponent<LightComponent>(), loadedInsertedLight);
}

TEST(PrefabObjectGraphSerializationTests, PrefabVariantPreservesBasePrefabReferenceAndDiagnosesInvalidOverrides)
{
    using namespace NLS::Engine;
    using namespace NLS::Engine::Serialize;

    GameObject prefabRoot("Variant", "Prop");
    auto prefab = ObjectGraphSerializer::SerializePrefab(prefabRoot);
    prefab.graph.basePrefab = ObjectIdentifier::Asset(
        AssetId(NLS::Guid::Parse("33333333-3333-4333-8333-333333333333")),
        MakeLocalIdentifierInFile(
            NLS::Guid::Parse("33333333-3333-4333-8333-333333333333"),
            "Assets/Prefabs/Base.prefab"),
        "Assets/Prefabs/Base.prefab");
    prefab.graph.overrides.push_back(PatchOperation::ReplaceProperty(
        ObjectId(NLS::Guid::NewDeterministic("MissingOverrideTarget")),
        "name",
        PropertyValue::String("Broken")));

    const auto diagnostics = ObjectGraphInstantiator::ValidatePrefab(prefab);
    ASSERT_TRUE(diagnostics.HasErrors());
    ASSERT_FALSE(diagnostics.GetItems().empty());
    EXPECT_EQ(diagnostics.GetItems()[0].GetCode(), SerializationDiagnosticCode::InvalidPrefabOverride);

    const auto text = ObjectGraphWriter::Write(prefab.graph);
    const auto loaded = ObjectGraphReader::Read(text);
    ASSERT_TRUE(loaded.has_value());
    ASSERT_TRUE(loaded->basePrefab.has_value());
    EXPECT_EQ(loaded->basePrefab->guid.ToString(), "33333333-3333-4333-8333-333333333333");
    EXPECT_EQ(loaded->basePrefab->filePath, "Assets/Prefabs/Base.prefab");
    EXPECT_EQ(loaded->basePrefab->fileType, FileType::SerializedAssetType);
}

TEST(PrefabObjectGraphSerializationTests, PrefabValidationRejectsSceneOnlyStrippedRecords)
{
    using namespace NLS::Engine;
    using namespace NLS::Engine::Serialize;

    GameObject prefabRoot("StrippedInPrefab", "Prop");
    auto prefab = ObjectGraphSerializer::SerializePrefab(prefabRoot);
    auto* rootRecord = FindRecord(prefab.graph, prefab.graph.root);
    ASSERT_NE(rootRecord, nullptr);
    rootRecord->state = ObjectRecordState::Stripped;

    const auto diagnostics = ObjectGraphInstantiator::ValidatePrefab(prefab);

    EXPECT_TRUE(diagnostics.HasErrors());
    EXPECT_TRUE(ContainsDiagnostic(diagnostics, SerializationDiagnosticCode::InvalidPrefabOverride));
}

TEST(PrefabObjectGraphSerializationTests, AssetReferencesInstantiateThroughPathHintsForPathBasedComponents)
{
    using namespace NLS::Engine;
    using namespace NLS::Engine::Components;
    using namespace NLS::Engine::Serialize;

    PersistentManager::Instance().Clear();

    NLS::Core::ResourceManagement::MeshManager meshManager;
    NLS::Core::ResourceManagement::MaterialManager materialManager;
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::MeshManager>(meshManager);
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::MaterialManager>(materialManager);

    auto* mesh = new NLS::Render::Resources::Mesh({}, {}, 0u);
    meshManager.RegisterResource("Artifacts/Hero/db7ffec2d25e80c7b075bc30a992e27e5f392f809146715c3cdf514a6fba8beb", mesh);

    auto* material = new NLS::Render::Resources::Material();
    const_cast<std::string&>(material->path) = "Artifacts/Hero/8ca977f3a8a054ff6767e381b334be9e47456f725e02f84e11a3b5b1f3f4218b";
    materialManager.RegisterResource("Artifacts/Hero/8ca977f3a8a054ff6767e381b334be9e47456f725e02f84e11a3b5b1f3f4218b", material);

    const auto rootId = ObjectId(NLS::Guid::Parse("10101010-1010-4010-8010-101010101010"));
    const auto meshFilterId = ObjectId(NLS::Guid::Parse("20202020-2020-4020-8020-202020202020"));
    const auto meshRendererId = ObjectId(NLS::Guid::Parse("21212121-2121-4121-8121-212121212121"));
    const auto assetId = AssetId(NLS::Guid::Parse("40404040-4040-4040-8040-404040404040"));

    PrefabDocument prefab;
    prefab.graph.documentId = NLS::Guid::Parse("50505050-5050-4050-8050-505050505050");
    prefab.graph.root = rootId;

    ObjectRecord root;
    root.id = rootId;
    root.localIdentifierInFile = MakeLocalIdentifierInFile(rootId);
    root.typeName = "NLS::Engine::GameObject";
    root.debugName = "Imported Root";
    root.properties.push_back({"name", PropertyValue::String("Imported Root")});
    root.properties.push_back({"components", PropertyValue::Array({
        PropertyValue::OwnedReference(meshFilterId),
        PropertyValue::OwnedReference(meshRendererId)
    })});

    ObjectRecord meshFilter;
    meshFilter.id = meshFilterId;
    meshFilter.localIdentifierInFile = MakeLocalIdentifierInFile(meshFilterId);
    meshFilter.typeName = "NLS::Engine::Components::MeshFilter";
    meshFilter.properties.push_back({"mesh", PropertyValue::ObjectReference(ObjectIdentifier::Asset(
        assetId,
        MakeLocalIdentifierInFile(assetId.GetGuid(), "mesh:body"),
        "Artifacts/Hero/db7ffec2d25e80c7b075bc30a992e27e5f392f809146715c3cdf514a6fba8beb"))});

    ObjectRecord meshRenderer;
    meshRenderer.id = meshRendererId;
    meshRenderer.localIdentifierInFile = MakeLocalIdentifierInFile(meshRendererId);
    meshRenderer.typeName = "NLS::Engine::Components::MeshRenderer";
    meshRenderer.properties.push_back({"materials", PropertyValue::Array({
        PropertyValue::ObjectReference(ObjectIdentifier::Asset(
            assetId,
            MakeLocalIdentifierInFile(assetId.GetGuid(), "Artifacts/Hero/8ca977f3a8a054ff6767e381b334be9e47456f725e02f84e11a3b5b1f3f4218b"),
            "Artifacts/Hero/8ca977f3a8a054ff6767e381b334be9e47456f725e02f84e11a3b5b1f3f4218b"))
    })});

    prefab.graph.objects.push_back(std::move(root));
    prefab.graph.objects.push_back(std::move(meshFilter));
    prefab.graph.objects.push_back(std::move(meshRenderer));

    SceneSystem::Scene scene;
    const auto result = ObjectGraphInstantiator::InstantiatePrefab(prefab, scene);

    ASSERT_NE(result.root, nullptr);
    auto* loadedMeshFilter = result.root->GetComponent<MeshFilter>();
    ASSERT_NE(loadedMeshFilter, nullptr);
    EXPECT_EQ(loadedMeshFilter->ResolveMesh(), mesh);
    EXPECT_EQ(loadedMeshFilter->GetMeshReference().Get(), mesh);
    EXPECT_EQ(loadedMeshFilter->GetModelPath(), "Artifacts/Hero/db7ffec2d25e80c7b075bc30a992e27e5f392f809146715c3cdf514a6fba8beb");
    auto* loadedMeshRenderer = result.root->GetComponent<MeshRenderer>();
    ASSERT_NE(loadedMeshRenderer, nullptr);
    EXPECT_EQ(
        loadedMeshRenderer->GetFrustumBehaviour(),
        MeshRenderer::EFrustumBehaviour::CULL_MODEL);

    ASSERT_EQ(loadedMeshRenderer->GetMaterialPaths().size(), 1u);
    EXPECT_EQ(loadedMeshRenderer->GetMaterialPaths()[0], "Artifacts/Hero/8ca977f3a8a054ff6767e381b334be9e47456f725e02f84e11a3b5b1f3f4218b");

    meshManager.UnloadResources();
    materialManager.UnloadResources();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::MeshManager>();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::MaterialManager>();
}

TEST(PrefabObjectGraphSerializationTests, ComponentOnCreateObservesRestoredGameObjectStateDuringPrefabInstantiation)
{
    using namespace NLS::Engine;
    using namespace NLS::Engine::Components;
    using namespace NLS::Engine::Serialize;

    const auto rootId = ObjectId(NLS::Guid::Parse("13131313-1313-4313-8313-131313131313"));
    const auto captureId = ObjectId(NLS::Guid::Parse("24242424-2424-4424-8424-242424242424"));
    ComponentAddedOwnerState observed;
    NLS::ListenerID componentAddedListener = NLS::InvalidListenerID;
    const auto createdListener = GameObject::CreatedEvent +=
        [&observed, &componentAddedListener](GameObject& gameObject)
        {
            if (gameObject.GetName() != "Restored Root")
                return;

            componentAddedListener = gameObject.ComponentAddedEvent +=
                [&observed](Component* component)
                {
                    if (component == nullptr || component->GetType() != NLS_TYPEOF(LightComponent))
                        return;

                    auto* owner = component->gameobject();
                    ASSERT_NE(owner, nullptr);
                    observed.observed = true;
                    observed.observedName = owner->GetName();
                    observed.observedTag = owner->GetTag();
                    observed.observedSourceObjectKey = owner->GetSourceObjectKey();
                    observed.observedActive = owner->IsSelfActive();
                };
        };

    PrefabDocument prefab;
    prefab.graph.documentId = NLS::Guid::Parse("53535353-5353-4353-8353-535353535353");
    prefab.graph.root = rootId;

    ObjectRecord root;
    root.id = rootId;
    root.localIdentifierInFile = MakeLocalIdentifierInFile(rootId);
    root.typeName = "NLS::Engine::GameObject";
    root.debugName = "Default Root";
    root.properties.push_back({"name", PropertyValue::String("Restored Root")});
    root.properties.push_back({"tag", PropertyValue::String("RestoredTag")});
    root.properties.push_back({"active", PropertyValue::Bool(false)});
    root.properties.push_back({"sourceObjectKey", PropertyValue::String("source:restored")});
    root.properties.push_back({"components", PropertyValue::Array({PropertyValue::OwnedReference(captureId)})});

    ObjectRecord capture;
    capture.id = captureId;
    capture.localIdentifierInFile = MakeLocalIdentifierInFile(captureId);
    capture.typeName = NLS_TYPEOF(LightComponent).GetName();

    prefab.graph.objects.push_back(std::move(root));
    prefab.graph.objects.push_back(std::move(capture));

    SceneSystem::Scene scene;
    const auto result = ObjectGraphInstantiator::InstantiatePrefab(prefab, scene);
    if (componentAddedListener != NLS::InvalidListenerID && result.root != nullptr)
        result.root->ComponentAddedEvent -= componentAddedListener;
    GameObject::CreatedEvent -= createdListener;

    ASSERT_NE(result.root, nullptr);
    ASSERT_NE(result.root->GetComponent<LightComponent>(), nullptr);
    EXPECT_TRUE(observed.observed);
    EXPECT_EQ(observed.observedName, "Restored Root");
    EXPECT_EQ(observed.observedTag, "RestoredTag");
    EXPECT_EQ(observed.observedSourceObjectKey, "source:restored");
    EXPECT_FALSE(observed.observedActive);
}

TEST(PrefabObjectGraphSerializationTests, BatchExternalAssetBindingPreservesImmediateAndDeferredResourceReferences)
{
    using namespace NLS::Engine;
    using namespace NLS::Engine::Components;
    using namespace NLS::Engine::Serialize;

    PersistentManager::Instance().Clear();

    NLS::Core::ResourceManagement::MeshManager meshManager;
    NLS::Core::ResourceManagement::MaterialManager materialManager;
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::MeshManager>(meshManager);
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::MaterialManager>(materialManager);

    auto* mesh = new NLS::Render::Resources::Mesh({}, {}, 0u);
    meshManager.RegisterResource("Artifacts/Batch/body.nmesh", mesh);

    auto* material = new NLS::Render::Resources::Material();
    const_cast<std::string&>(material->path) = "Artifacts/Batch/body.nmat";
    materialManager.RegisterResource("Artifacts/Batch/body.nmat", material);

    auto makePrefab = []()
    {
        const auto rootId = ObjectId(NLS::Guid::Parse("12121212-1212-4212-8212-121212121212"));
        const auto meshFilterId = ObjectId(NLS::Guid::Parse("22222222-2222-4222-8222-222222222222"));
        const auto meshRendererId = ObjectId(NLS::Guid::Parse("23232323-2323-4232-8232-232323232323"));
        const auto assetId = AssetId(NLS::Guid::Parse("42424242-4242-4242-8242-424242424242"));

        PrefabDocument prefab;
        prefab.graph.documentId = NLS::Guid::Parse("52525252-5252-4252-8252-525252525252");
        prefab.graph.root = rootId;

        ObjectRecord root;
        root.id = rootId;
        root.localIdentifierInFile = MakeLocalIdentifierInFile(rootId);
        root.typeName = "NLS::Engine::GameObject";
        root.debugName = "Batch Root";
        root.properties.push_back({"name", PropertyValue::String("Batch Root")});
        root.properties.push_back({"components", PropertyValue::Array({
            PropertyValue::OwnedReference(meshFilterId),
            PropertyValue::OwnedReference(meshRendererId)
        })});

        ObjectRecord meshFilter;
        meshFilter.id = meshFilterId;
        meshFilter.localIdentifierInFile = MakeLocalIdentifierInFile(meshFilterId);
        meshFilter.typeName = "NLS::Engine::Components::MeshFilter";
        meshFilter.properties.push_back({"mesh", PropertyValue::ObjectReference(ObjectIdentifier::Asset(
            assetId,
            MakeLocalIdentifierInFile(assetId.GetGuid(), "mesh:batch/body"),
            "Artifacts/Batch/body.nmesh"))});

        ObjectRecord meshRenderer;
        meshRenderer.id = meshRendererId;
        meshRenderer.localIdentifierInFile = MakeLocalIdentifierInFile(meshRendererId);
        meshRenderer.typeName = "NLS::Engine::Components::MeshRenderer";
        meshRenderer.properties.push_back({"materials", PropertyValue::Array({
            PropertyValue::ObjectReference(ObjectIdentifier::Asset(
                assetId,
                MakeLocalIdentifierInFile(assetId.GetGuid(), "material:batch/body"),
                "Artifacts/Batch/body.nmat"))
        })});

        prefab.graph.objects.push_back(std::move(root));
        prefab.graph.objects.push_back(std::move(meshFilter));
        prefab.graph.objects.push_back(std::move(meshRenderer));
        return prefab;
    };

    auto verifyInstance = [mesh, material](GameObject* root)
    {
        ASSERT_NE(root, nullptr);
        auto* loadedMeshFilter = root->GetComponent<MeshFilter>();
        ASSERT_NE(loadedMeshFilter, nullptr);
        EXPECT_EQ(loadedMeshFilter->ResolveMesh(), mesh);
        EXPECT_EQ(loadedMeshFilter->GetMeshReference().Get(), mesh);
        EXPECT_EQ(loadedMeshFilter->GetModelPath(), "Artifacts/Batch/body.nmesh");
        EXPECT_EQ(ResolveObjectIdentifier(loadedMeshFilter->GetMeshReference()).filePath, "Artifacts/Batch/body.nmesh");

        auto* loadedMeshRenderer = root->GetComponent<MeshRenderer>();
        ASSERT_NE(loadedMeshRenderer, nullptr);
        ASSERT_EQ(loadedMeshRenderer->GetMaterialPaths().size(), 1u);
        EXPECT_EQ(loadedMeshRenderer->GetMaterialPaths()[0], "Artifacts/Batch/body.nmat");
        EXPECT_EQ(loadedMeshRenderer->ResolveMaterialAtIndex(0u), material);
        ASSERT_EQ(loadedMeshRenderer->GetMaterialReferences().size(), 1u);
        EXPECT_EQ(loadedMeshRenderer->GetMaterialReferences()[0].Get(), material);
        EXPECT_EQ(
            ResolveObjectIdentifier(loadedMeshRenderer->GetMaterialReferences()[0]).filePath,
            "Artifacts/Batch/body.nmat");
    };

    {
        SceneSystem::Scene scene;
        const auto result = ObjectGraphInstantiator::InstantiatePrefab(makePrefab(), scene);
        verifyInstance(result.root);
    }

    PersistentManager::Instance().Clear();

    {
        SceneSystem::Scene scene;
        LoadPolicy policy;
        policy.deferAssetReferenceResolution = true;
        const auto result = ObjectGraphInstantiator::InstantiatePrefab(makePrefab(), scene, policy);
        verifyInstance(result.root);
    }

    meshManager.UnloadResources();
    materialManager.UnloadResources();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::MeshManager>();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::MaterialManager>();
}

TEST(PrefabObjectGraphSerializationTests, DeferredAssetReferenceHintsCanSkipSynchronousResourceCacheLookup)
{
    using namespace NLS::Engine;
    using namespace NLS::Engine::Components;
    using namespace NLS::Engine::Serialize;

    PersistentManager::Instance().Clear();

    NLS::Core::ResourceManagement::MeshManager meshManager;
    NLS::Core::ResourceManagement::MaterialManager materialManager;
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::MeshManager>(meshManager);
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::MaterialManager>(materialManager);

    auto* mesh = new NLS::Render::Resources::Mesh({}, {}, 0u);
    meshManager.RegisterResource("Artifacts/Deferred/body.nmesh", mesh);

    auto* material = new NLS::Render::Resources::Material();
    const_cast<std::string&>(material->path) = "Artifacts/Deferred/body.nmat";
    materialManager.RegisterResource("Artifacts/Deferred/body.nmat", material);

    const auto rootId = ObjectId(NLS::Guid::Parse("61616161-6161-4161-8161-616161616161"));
    const auto meshFilterId = ObjectId(NLS::Guid::Parse("62626262-6262-4262-8262-626262626262"));
    const auto meshRendererId = ObjectId(NLS::Guid::Parse("63636363-6363-4363-8363-636363636363"));
    const auto assetId = AssetId(NLS::Guid::Parse("64646464-6464-4464-8464-646464646464"));

    PrefabDocument prefab;
    prefab.graph.documentId = NLS::Guid::Parse("65656565-6565-4565-8565-656565656565");
    prefab.graph.root = rootId;

    ObjectRecord root;
    root.id = rootId;
    root.localIdentifierInFile = MakeLocalIdentifierInFile(rootId);
    root.typeName = "NLS::Engine::GameObject";
    root.debugName = "Deferred Root";
    root.properties.push_back({"name", PropertyValue::String("Deferred Root")});
    root.properties.push_back({"components", PropertyValue::Array({
        PropertyValue::OwnedReference(meshFilterId),
        PropertyValue::OwnedReference(meshRendererId)
    })});

    ObjectRecord meshFilter;
    meshFilter.id = meshFilterId;
    meshFilter.localIdentifierInFile = MakeLocalIdentifierInFile(meshFilterId);
    meshFilter.typeName = "NLS::Engine::Components::MeshFilter";
    meshFilter.properties.push_back({"mesh", PropertyValue::ObjectReference(ObjectIdentifier::Asset(
        assetId,
        MakeLocalIdentifierInFile(assetId.GetGuid(), "mesh:deferred/body"),
        "Artifacts/Deferred/body.nmesh"))});

    ObjectRecord meshRenderer;
    meshRenderer.id = meshRendererId;
    meshRenderer.localIdentifierInFile = MakeLocalIdentifierInFile(meshRendererId);
    meshRenderer.typeName = "NLS::Engine::Components::MeshRenderer";
    meshRenderer.properties.push_back({"materials", PropertyValue::Array({
        PropertyValue::ObjectReference(ObjectIdentifier::Asset(
            assetId,
            MakeLocalIdentifierInFile(assetId.GetGuid(), "material:deferred/body"),
            "Artifacts/Deferred/body.nmat"))
    })});

    prefab.graph.objects.push_back(std::move(root));
    prefab.graph.objects.push_back(std::move(meshFilter));
    prefab.graph.objects.push_back(std::move(meshRenderer));

    LoadPolicy policy;
    policy.deferAssetReferenceResolution = true;
    policy.skipDeferredAssetReferenceCacheLookup = true;

    NLS::Base::Profiling::PerformanceStageStats stats;
    SceneSystem::Scene scene;
    PrefabInstantiationResult result;
    {
        NLS::Base::Profiling::PerformanceStageStatsCapture capture(stats);
        result = ObjectGraphInstantiator::InstantiatePrefab(prefab, scene, policy);
    }

    ASSERT_NE(result.root, nullptr);
    auto* loadedMeshFilter = result.root->GetComponent<MeshFilter>();
    ASSERT_NE(loadedMeshFilter, nullptr);
    EXPECT_EQ(loadedMeshFilter->GetModelPath(), "Artifacts/Deferred/body.nmesh");
    EXPECT_EQ(ResolveObjectIdentifier(loadedMeshFilter->GetMeshReference()).filePath, "Artifacts/Deferred/body.nmesh");

    auto* loadedMeshRenderer = result.root->GetComponent<MeshRenderer>();
    ASSERT_NE(loadedMeshRenderer, nullptr);
    ASSERT_EQ(loadedMeshRenderer->GetMaterialPaths().size(), 1u);
    EXPECT_EQ(loadedMeshRenderer->GetMaterialPaths()[0], "Artifacts/Deferred/body.nmat");
    ASSERT_EQ(loadedMeshRenderer->GetMaterialReferences().size(), 1u);
    EXPECT_EQ(
        ResolveObjectIdentifier(loadedMeshRenderer->GetMaterialReferences()[0]).filePath,
        "Artifacts/Deferred/body.nmat");

    const auto snapshot = stats.Snapshot();
    auto hasPrefabStage = [&snapshot](const std::string& stageName)
    {
        return std::any_of(
            snapshot.stages.begin(),
            snapshot.stages.end(),
            [&stageName](const NLS::Base::Profiling::PerformanceStageEntry& entry)
            {
                return entry.domain == NLS::Base::Profiling::PerformanceStageDomain::Prefab &&
                    entry.stageName == stageName &&
                    entry.callCount > 0u;
            });
    };
    EXPECT_FALSE(hasPrefabStage("ApplyDeferredAssetReferenceHints"));
    EXPECT_FALSE(hasPrefabStage("ApplyDeferredMeshReferenceHint"));
    EXPECT_FALSE(hasPrefabStage("ApplyDeferredMaterialReferenceHints"));
    EXPECT_FALSE(hasPrefabStage("FindCachedMeshResource"));
    EXPECT_FALSE(hasPrefabStage("FindCachedMaterialResource"));

    meshManager.UnloadResources();
    materialManager.UnloadResources();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::MeshManager>();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::MaterialManager>();
}

TEST(PrefabObjectGraphSerializationTests, RuntimeAssetDatabaseIdentityOverridesStaleObjectReferencePathHint)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Engine;
    using namespace NLS::Engine::Assets;
    using namespace NLS::Engine::Components;
    using namespace NLS::Engine::Serialize;

    PersistentManager::Instance().Clear();
    NLS::Core::ResourceManagement::MeshManager meshManager;
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::MeshManager>(meshManager);

    auto* mesh = new NLS::Render::Resources::Mesh({}, {}, 0u);
    meshManager.RegisterResource("Library/Artifacts/Hero/meshes/db7ffec2d25e80c7b075bc30a992e27e5f392f809146715c3cdf514a6fba8beb", mesh);

    const auto sourceAssetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("51515151-5151-4151-8151-515151515151"));
    constexpr const char* subAssetKey = "mesh:body";
    RuntimeAssetManifest manifest;
    manifest.entries.push_back({
        sourceAssetId,
        subAssetKey,
        ArtifactType::Mesh,
        "Nullus.Mesh",
        "Library/Artifacts/Hero/meshes/db7ffec2d25e80c7b075bc30a992e27e5f392f809146715c3cdf514a6fba8beb",
        "sha256:model",
        {}
    });
    RuntimeAssetDatabase runtimeAssets(manifest);
    NLS::Core::ServiceLocator::Provide<RuntimeAssetDatabase>(runtimeAssets);

    const auto rootId = ObjectId(NLS::Guid::Parse("11111111-1111-4111-8111-111111111111"));
    const auto meshFilterId = ObjectId(NLS::Guid::Parse("22222222-2222-4222-8222-222222222222"));
    const auto meshRendererId = ObjectId(NLS::Guid::Parse("23232323-2323-4232-8232-232323232323"));

    PrefabDocument prefab;
    prefab.graph.documentId = NLS::Guid::Parse("44444444-4444-4444-8444-444444444444");
    prefab.graph.root = rootId;

    ObjectRecord root;
    root.id = rootId;
    root.localIdentifierInFile = MakeLocalIdentifierInFile(rootId);
    root.typeName = "NLS::Engine::GameObject";
    root.debugName = "Root";
    root.properties.push_back({"name", PropertyValue::String("Root")});
    root.properties.push_back({"tag", PropertyValue::String({})});
    root.properties.push_back({"components", PropertyValue::Array({
        PropertyValue::OwnedReference(meshFilterId),
        PropertyValue::OwnedReference(meshRendererId)
    })});

    ObjectRecord meshFilter;
    meshFilter.id = meshFilterId;
    meshFilter.localIdentifierInFile = MakeLocalIdentifierInFile(meshFilterId);
    meshFilter.typeName = "NLS::Engine::Components::MeshFilter";
    meshFilter.debugName = "MeshFilter";
    meshFilter.properties.push_back({"mesh", PropertyValue::ObjectReference(ObjectIdentifier::Asset(
        NLS::Engine::Serialize::AssetId(sourceAssetId.GetGuid()),
        MakeLocalIdentifierInFile(sourceAssetId.GetGuid(), subAssetKey),
        "Library/Artifacts/Stale/810e1589298eb04e0ac7e97825f72bbc40830ac4e68fee7ac9e101bcbf91226a"))});

    ObjectRecord meshRenderer;
    meshRenderer.id = meshRendererId;
    meshRenderer.localIdentifierInFile = MakeLocalIdentifierInFile(meshRendererId);
    meshRenderer.typeName = "NLS::Engine::Components::MeshRenderer";
    meshRenderer.debugName = "MeshRenderer";

    prefab.graph.objects.push_back(std::move(root));
    prefab.graph.objects.push_back(std::move(meshFilter));
    prefab.graph.objects.push_back(std::move(meshRenderer));

    SceneSystem::Scene scene;
    const auto result = ObjectGraphInstantiator::InstantiatePrefab(prefab, scene);

    ASSERT_NE(result.root, nullptr);
    auto* loadedMeshFilter = result.root->GetComponent<MeshFilter>();
    ASSERT_NE(loadedMeshFilter, nullptr);
    EXPECT_EQ(loadedMeshFilter->ResolveMesh(), mesh);
    EXPECT_EQ(loadedMeshFilter->GetModelPath(), "Library/Artifacts/Hero/meshes/db7ffec2d25e80c7b075bc30a992e27e5f392f809146715c3cdf514a6fba8beb");
    EXPECT_EQ(ResolveObjectIdentifier(loadedMeshFilter->GetMeshReference()).filePath, "Library/Artifacts/Hero/meshes/db7ffec2d25e80c7b075bc30a992e27e5f392f809146715c3cdf514a6fba8beb");
    EXPECT_EQ(loadedMeshFilter->GetMeshReference().Get(), mesh);
    auto* loadedMeshRenderer = result.root->GetComponent<MeshRenderer>();
    ASSERT_NE(loadedMeshRenderer, nullptr);
    EXPECT_EQ(
        loadedMeshRenderer->GetFrustumBehaviour(),
        MeshRenderer::EFrustumBehaviour::CULL_MODEL);

    meshManager.UnloadResources();
    NLS::Core::ServiceLocator::Remove<RuntimeAssetDatabase>();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::MeshManager>();
}

TEST(PrefabObjectGraphSerializationTests, NonArrayObjectReferenceIsRejectedForPPtrMaterialArray)
{
    using namespace NLS::Engine;
    using namespace NLS::Engine::Components;
    using namespace NLS::Engine::Serialize;

    PersistentManager::Instance().Clear();
    const auto rootId = ObjectId(NLS::Guid::Parse("11111111-1111-4111-8111-111111111111"));
    const auto MeshRendererId = ObjectId(NLS::Guid::Parse("22222222-2222-4222-8222-222222222222"));
    const auto materialAssetId = AssetId(NLS::Guid::Parse("33333333-3333-4333-8333-333333333333"));

    PrefabDocument prefab;
    prefab.graph.documentId = NLS::Guid::Parse("44444444-4444-4444-8444-444444444444");
    prefab.graph.root = rootId;

    ObjectRecord root;
    root.id = rootId;
    root.localIdentifierInFile = MakeLocalIdentifierInFile(rootId);
    root.typeName = "NLS::Engine::GameObject";
    root.debugName = "Root";
    root.properties.push_back({"name", PropertyValue::String("Root")});
    root.properties.push_back({"tag", PropertyValue::String({})});
    root.properties.push_back({"components", PropertyValue::Array({PropertyValue::OwnedReference(MeshRendererId)})});

    ObjectRecord MeshRenderer;
    MeshRenderer.id = MeshRendererId;
    MeshRenderer.localIdentifierInFile = MakeLocalIdentifierInFile(MeshRendererId);
    MeshRenderer.typeName = "NLS::Engine::Components::MeshRenderer";
    MeshRenderer.debugName = "MeshRenderer";
    const auto preservedReference = ObjectIdentifier::Asset(
        materialAssetId,
        MakeLocalIdentifierInFile(materialAssetId.GetGuid(), "Assets/Materials/Preserved.mat"),
        "Assets/Materials/Preserved.mat");
    MeshRenderer.properties.push_back({"materials", PropertyValue::Array({
        PropertyValue::ObjectReference(preservedReference)
    })});
    MeshRenderer.properties.push_back({"materials", PropertyValue::ObjectReference(ObjectIdentifier::Asset(
        materialAssetId,
        MakeLocalIdentifierInFile(materialAssetId.GetGuid(), "Assets/Materials/WrongShape.mat"),
        "Assets/Materials/WrongShape.mat"))});

    prefab.graph.objects.push_back(std::move(root));
    prefab.graph.objects.push_back(std::move(MeshRenderer));

    SceneSystem::Scene scene;
    const auto result = ObjectGraphInstantiator::InstantiatePrefab(prefab, scene);

    EXPECT_EQ(result.root, nullptr);
    EXPECT_TRUE(result.diagnostics.HasErrors());
    EXPECT_TRUE(ContainsDiagnostic(
        result.diagnostics,
        SerializationDiagnosticCode::InvalidPropertyType));
}
