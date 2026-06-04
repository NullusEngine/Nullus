#include <gtest/gtest.h>

#include <any>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <type_traits>
#include <utility>

#include "GameObject.h"
#include "Components/LightComponent.h"
#include "Components/MeshRenderer.h"
#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "Components/TransformComponent.h"
#include "Debug/Logger.h"
#include "Engine/Assets/RuntimeAssetDatabase.h"
#include "Core/Assets/ArtifactManifest.h"
#include "Core/ServiceLocator.h"
#include "Core/ResourceManagement/MaterialManager.h"
#include "Core/ResourceManagement/MeshManager.h"
#include "Core/ResourceManagement/ShaderManager.h"
#include "Assets/PrefabEditorWorkflow.h"
#include "Rendering/Assets/MeshArtifact.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/Mesh.h"
#include "Rendering/Settings/DriverSettings.h"
#include "Rendering/Settings/EGraphicsBackend.h"
#include "Serialize/ObjectGraphDocument.h"
#include "Reflection/Field.h"
#include "Reflection/MetaParserFieldMethodSample.h"
#include "Reflection/Method.h"
#include "Reflection/RuntimeMetaProperties.h"
#include "Reflection/Type.h"
#include "SceneSystem/Scene.h"
#include "SceneSystem/SceneManager.h"
#include "Serialize/ObjectGraphInstantiator.h"
#include "Serialize/ObjectGraphDocument.h"
#include "Serialize/ObjectGraphReader.h"
#include "Serialize/ObjectGraphSerializer.h"
#include "Serialize/ObjectGraphWriter.h"

namespace
{
std::string ReadTextFile(const std::filesystem::path& path)
{
    std::ifstream file(path);
    std::ostringstream stream;
    stream << file.rdbuf();
    return stream.str();
}

NLS::Engine::Serialize::ObjectGraphDocument MakeSimpleSceneDocument()
{
    using namespace NLS::Engine::Serialize;

    const auto sceneId = ObjectId(NLS::Guid::Parse("bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb"));
    const auto playerId = ObjectId(NLS::Guid::Parse("cccccccc-cccc-4ccc-cccc-cccccccccccc"));
    const auto transformId = ObjectId(NLS::Guid::Parse("dddddddd-dddd-4ddd-9ddd-dddddddddddd"));

    ObjectGraphDocument document;
    document.documentId = NLS::Guid::Parse("aaaaaaaa-aaaa-4aaa-aaaa-aaaaaaaaaaaa");
    document.root = sceneId;

    ObjectRecord scene;
    scene.id = sceneId;
    scene.localIdentifierInFile = MakeLocalIdentifierInFile(sceneId);
    scene.typeName = "NLS::Engine::SceneSystem::Scene";
    scene.debugName = "Main Scene";
    scene.properties.push_back({"gameObjects", PropertyValue::Array({PropertyValue::OwnedReference(playerId)})});

    ObjectRecord player;
    player.id = playerId;
    player.localIdentifierInFile = MakeLocalIdentifierInFile(playerId);
    player.typeName = "NLS::Engine::GameObject";
    player.debugName = "Player";
    player.debugPath = "/Player";
    player.properties.push_back({"name", PropertyValue::String("Player")});
    player.properties.push_back({"tag", PropertyValue::String("Player")});
    player.properties.push_back({
        "parent",
        PropertyValue::ObjectReference(ObjectIdentifier::LocalObject(MakeLocalIdentifierInFile(sceneId)))
    });
    player.properties.push_back({"components", PropertyValue::Array({PropertyValue::OwnedReference(transformId)})});

    ObjectRecord transform;
    transform.id = transformId;
    transform.localIdentifierInFile = MakeLocalIdentifierInFile(transformId);
    transform.typeName = "NLS::Engine::Components::TransformComponent";
    transform.debugName = "Transform";

    document.objects.push_back(std::move(player));
    document.objects.push_back(std::move(transform));
    document.objects.push_back(std::move(scene));
    return document;
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

NLS::Engine::Serialize::PropertyRecord* FindMutableProperty(
    NLS::Engine::Serialize::ObjectRecord& record,
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

const NLS::Engine::Serialize::ObjectRecord* FindObjectRecord(
    const NLS::Engine::Serialize::ObjectGraphDocument& document,
    const NLS::Engine::Serialize::ObjectId& id)
{
    const auto found = std::find_if(
        document.objects.begin(),
        document.objects.end(),
        [&id](const NLS::Engine::Serialize::ObjectRecord& record)
        {
            return record.id == id;
        });
    return found != document.objects.end() ? &*found : nullptr;
}

template <typename T>
NLS::Engine::Serialize::PPtr<T> MakePPtr(const NLS::Engine::Serialize::ObjectIdentifier& identifier)
{
    return NLS::Engine::Serialize::PPtr<T>(
        NLS::Engine::Serialize::PersistentManager::Instance().ObjectIdentifierToInstanceID(identifier));
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

class ScopedTempDirectory final
{
public:
    explicit ScopedTempDirectory(std::filesystem::path path)
        : m_path(std::move(path))
    {
    }

    ~ScopedTempDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
    }

    const std::filesystem::path& Path() const
    {
        return m_path;
    }

private:
    std::filesystem::path m_path;
};

class ScopedMeshManagerContext final
{
public:
    ScopedMeshManagerContext(
        NLS::Core::ResourceManagement::MeshManager& meshManager,
        const std::string& projectAssetsRoot,
        const std::string& engineAssetsRoot)
        : m_meshManager(meshManager)
    {
        NLS::Core::ResourceManagement::MeshManager::ProvideAssetPaths(projectAssetsRoot, engineAssetsRoot);
        NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::MeshManager>(m_meshManager);
    }

    ~ScopedMeshManagerContext()
    {
        m_meshManager.UnloadResources();
        NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::MeshManager>();
        NLS::Core::ResourceManagement::MeshManager::ProvideAssetPaths({}, {});
    }

    ScopedMeshManagerContext(const ScopedMeshManagerContext&) = delete;
    ScopedMeshManagerContext& operator=(const ScopedMeshManagerContext&) = delete;

private:
    NLS::Core::ResourceManagement::MeshManager& m_meshManager;
};

class ScopedLogListener final
{
public:
    explicit ScopedLogListener(std::function<void(const NLS::Debug::LogData&)> callback)
        : m_listener(NLS::Debug::Logger::LogEvent += std::move(callback))
    {
    }

    ~ScopedLogListener()
    {
        NLS::Debug::Logger::LogEvent -= m_listener;
    }

    ScopedLogListener(const ScopedLogListener&) = delete;
    ScopedLogListener& operator=(const ScopedLogListener&) = delete;

private:
    NLS::ListenerID m_listener = NLS::InvalidListenerID;
};

template<typename T>
class ScopedServiceOverride
{
public:
    explicit ScopedServiceOverride(T& service)
    {
        m_hadPrevious = NLS::Core::ServiceLocator::Contains<T>();
        if (m_hadPrevious)
        {
            try
            {
                m_previous = &NLS::Core::ServiceLocator::Get<T>();
            }
            catch (const std::bad_any_cast&)
            {
                m_hadPrevious = false;
                NLS::Core::ServiceLocator::Remove<T>();
            }
        }

        NLS::Core::ServiceLocator::Provide<T>(service);
    }

    ~ScopedServiceOverride()
    {
        if (m_hadPrevious && m_previous)
            NLS::Core::ServiceLocator::Provide<T>(*m_previous);
        else
            NLS::Core::ServiceLocator::Remove<T>();
    }

    ScopedServiceOverride(const ScopedServiceOverride&) = delete;
    ScopedServiceOverride& operator=(const ScopedServiceOverride&) = delete;

private:
    bool m_hadPrevious = false;
    T* m_previous = nullptr;
};

class ScopedSceneObjectGraphTestDriver final
{
public:
    ScopedSceneObjectGraphTestDriver()
        : m_driver([]()
        {
            NLS::Render::Settings::DriverSettings settings;
            settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
            settings.enableExplicitRHI = false;
            return settings;
        }())
    {
        if (NLS::Core::ServiceLocator::Contains<NLS::Render::Context::Driver>())
        {
            try
            {
                m_previousDriver = &NLS_SERVICE(NLS::Render::Context::Driver);
            }
            catch (const std::bad_any_cast&)
            {
                NLS::Core::ServiceLocator::Remove<NLS::Render::Context::Driver>();
            }
        }

        NLS::Core::ServiceLocator::Provide(m_driver);
    }

    ~ScopedSceneObjectGraphTestDriver()
    {
        if (m_previousDriver != nullptr)
            NLS::Core::ServiceLocator::Provide(*m_previousDriver);
        else
            NLS::Core::ServiceLocator::Remove<NLS::Render::Context::Driver>();
    }

    ScopedSceneObjectGraphTestDriver(const ScopedSceneObjectGraphTestDriver&) = delete;
    ScopedSceneObjectGraphTestDriver& operator=(const ScopedSceneObjectGraphTestDriver&) = delete;

private:
    NLS::Render::Context::Driver* m_previousDriver = nullptr;
    NLS::Render::Context::Driver m_driver;
};
}

TEST(SceneObjectGraphSerializationTests, SerializationMetadataTagsExposeIntent)
{
    EXPECT_EQ(NLS::meta::SerializeField().GetIntent(), NLS::meta::SerializationFieldIntent::Value);
    EXPECT_EQ(NLS::meta::Transient().GetIntent(), NLS::meta::SerializationFieldIntent::Transient);
    EXPECT_EQ(NLS::meta::OwnedReference().GetIntent(), NLS::meta::SerializationFieldIntent::OwnedReference);
    EXPECT_EQ(NLS::meta::ObjectReference().GetIntent(), NLS::meta::SerializationFieldIntent::ObjectReference);
    EXPECT_EQ(NLS::meta::AssetReference().GetIntent(), NLS::meta::SerializationFieldIntent::AssetReference);
    EXPECT_TRUE(NLS::meta::EditorOnly().IsEditorOnly());
    EXPECT_TRUE(NLS::meta::RuntimeOnly().IsRuntimeOnly());
    EXPECT_EQ(NLS::meta::FormerlySerializedAs("oldName").name, "oldName");
    EXPECT_EQ(NLS::meta::StableTypeName("Engine.MeshRenderer").name, "Engine.MeshRenderer");
    EXPECT_EQ(NLS::meta::FormerlyTypeName("Engine.ModelRenderer").name, "Engine.ModelRenderer");
}

TEST(SceneObjectGraphSerializationTests, SceneIsReflectedObjectRoot)
{
    const auto sceneType = NLS::meta::Type::GetFromName("NLS::Engine::SceneSystem::Scene");
    const auto objectType = NLS_TYPEOF(NLS::Object);

    ASSERT_TRUE(sceneType.IsValid());
    ASSERT_TRUE(objectType.IsValid());
    EXPECT_TRUE(sceneType.DerivesFrom(objectType));
    EXPECT_TRUE((std::is_base_of_v<NLS::Object, NLS::Engine::SceneSystem::Scene>));
}

TEST(SceneObjectGraphSerializationTests, GameObjectReflectionDoesNotExposeWorldId)
{
    const auto gameObjectType = NLS::meta::Type::GetFromName("NLS::Engine::GameObject");

    ASSERT_TRUE(gameObjectType.IsValid());
    EXPECT_FALSE(gameObjectType.GetField("worldID").IsValid());
    EXPECT_FALSE(gameObjectType.GetMethod("GetWorldID").IsValid());
    EXPECT_FALSE(gameObjectType.GetMethod("SetWorldID").IsValid());
}

TEST(SceneObjectGraphSerializationTests, GameObjectConstructionUsesNameAndTagOnly)
{
    NLS::Engine::GameObject actor("Serializable Actor", "Gameplay");

    EXPECT_EQ(actor.GetName(), "Serializable Actor");
    EXPECT_EQ(actor.GetTag(), "Gameplay");
    EXPECT_NE(actor.GetTransform(), nullptr);
}

TEST(SceneObjectGraphSerializationTests, GameObjectLayerSerializesAndLoadsThroughClampingSetter)
{
    using namespace NLS::Engine::Serialize;

    auto document = MakeSimpleSceneDocument();
    auto& player = document.objects.front();
    ASSERT_EQ(player.typeName, "NLS::Engine::GameObject");
    player.properties.push_back({"layer", PropertyValue::Integer(99)});

    auto scene = ObjectGraphInstantiator::InstantiateScene(document);
    ASSERT_NE(scene, nullptr);
    const auto& gameObjects = scene->GetGameObjects();
    ASSERT_EQ(gameObjects.size(), 1u);
    ASSERT_NE(gameObjects.front(), nullptr);
    EXPECT_EQ(gameObjects.front()->GetLayer(), 31);

    gameObjects.front()->SetLayer(5);
    const auto serialized = ObjectGraphSerializer::SerializeScene(*scene);
    const auto gameObjectRecord = std::find_if(
        serialized.objects.begin(),
        serialized.objects.end(),
        [](const ObjectRecord& record)
        {
            return record.typeName == "NLS::Engine::GameObject";
        });
    ASSERT_NE(gameObjectRecord, serialized.objects.end());

    const auto* layerProperty = FindProperty(*gameObjectRecord, "layer");
    ASSERT_NE(layerProperty, nullptr);
    ASSERT_EQ(layerProperty->value.GetKind(), PropertyValue::Kind::Integer);
    EXPECT_EQ(layerProperty->value.GetInteger(), 5);
}

TEST(SceneObjectGraphSerializationTests, NewSceneObjectGraphOutputContainsNoLegacySceneShape)
{
    const auto output = NLS::Engine::Serialize::ObjectGraphWriter::Write(MakeSimpleSceneDocument());

    EXPECT_NE(output.find("\"format\": \"Nullus.ObjectGraph.Scene\""), std::string::npos);
    EXPECT_NE(output.find("\"type\": \"NLS::Engine::SceneSystem::Scene\""), std::string::npos);
    EXPECT_NE(output.find("\"type\": \"NLS::Engine::GameObject\""), std::string::npos);
    EXPECT_NE(output.find("\"$owned\""), std::string::npos);
    EXPECT_NE(output.find("\"fileID\""), std::string::npos);
    EXPECT_EQ(output.find("\"$ref\""), std::string::npos);
    EXPECT_EQ(output.find("worldID"), std::string::npos);
    EXPECT_EQ(output.find("SerializedSceneData"), std::string::npos);
    EXPECT_EQ(output.find("SerializedActorData"), std::string::npos);
    EXPECT_EQ(output.find("SerializedComponentData"), std::string::npos);
}

TEST(SceneObjectGraphSerializationTests, SimpleSceneObjectGraphMatchesGoldenOutput)
{
    const auto output = NLS::Engine::Serialize::ObjectGraphWriter::Write(MakeSimpleSceneDocument());
    const auto goldenPath =
        std::filesystem::path(NLS_ROOT_DIR) /
        "Tests/Unit/Fixtures/ObjectGraph/simple_scene.objectgraph.json";

    EXPECT_EQ(output, ReadTextFile(goldenPath));
}

TEST(SceneObjectGraphSerializationTests, SceneRoundTripsGameObjectsComponentsParentsAndCaches)
{
    using namespace NLS::Engine;
    using namespace NLS::Engine::Components;
    using namespace NLS::Engine::Serialize;

    NLS::Engine::SceneSystem::Scene source;
    auto& parent = source.CreateGameObject("Parent", "Root");
    auto& child = source.CreateGameObject("Child", "Gameplay");
    child.SetParent(parent);
    child.SetActive(false);
    auto* light = child.AddComponent<LightComponent>();
    ASSERT_NE(light, nullptr);
    light->SetIntensity(3.5f);

    const auto document = ObjectGraphSerializer::SerializeScene(source);
    ASSERT_FALSE(document.Validate().HasErrors());

    auto loaded = ObjectGraphInstantiator::InstantiateScene(document);
    ASSERT_NE(loaded, nullptr);

    const auto& actors = loaded->GetGameObjects();
    ASSERT_EQ(actors.size(), 2u);

    auto* loadedParent = loaded->FindGameObjectByName("Parent");
    auto* loadedChild = loaded->FindGameObjectByName("Child");
    ASSERT_NE(loadedParent, nullptr);
    ASSERT_NE(loadedChild, nullptr);

    EXPECT_EQ(loadedParent->GetTag(), "Root");
    EXPECT_EQ(loadedChild->GetTag(), "Gameplay");
    EXPECT_FALSE(loadedChild->IsSelfActive());
    EXPECT_EQ(loadedChild->GetParent(), loadedParent);
    ASSERT_EQ(loadedParent->GetChildren().size(), 1u);
    EXPECT_EQ(loadedParent->GetChildren()[0], loadedChild);

    const auto& components = loadedChild->GetComponents();
    ASSERT_EQ(components.size(), 2u);
    EXPECT_NE(dynamic_cast<TransformComponent*>(components[0].get()), nullptr);
    auto* loadedLight = dynamic_cast<LightComponent*>(components[1].get());
    ASSERT_NE(loadedLight, nullptr);
    EXPECT_EQ(loadedLight->gameobject(), loadedChild);
    EXPECT_FLOAT_EQ(loadedLight->GetIntensity(), 3.5f);

    const auto& fastAccess = loaded->GetFastAccessComponents();
    ASSERT_EQ(fastAccess.lights.size(), 1u);
    EXPECT_EQ(fastAccess.lights[0], loadedLight);
}

TEST(SceneObjectGraphSerializationTests, SceneSerializationAssignsUniqueComponentIdsForDuplicateNamedGameObjects)
{
    using namespace NLS::Engine::Components;
    using namespace NLS::Engine::Serialize;

    NLS::Engine::SceneSystem::Scene source;
    source.CreateGameObject("Duplicate", "Gameplay").AddComponent<LightComponent>();
    source.CreateGameObject("Duplicate", "Gameplay").AddComponent<LightComponent>();

    const auto document = ObjectGraphSerializer::SerializeScene(source);

    EXPECT_FALSE(document.Validate().HasErrors());
}

TEST(SceneObjectGraphSerializationTests, ObjectGraphSerializerWritesOrdinaryFieldsThroughReflection)
{
    NLS::meta::MetaParserFieldMethodSample object;
    object.SetValue(42);

    const auto record = NLS::Engine::Serialize::ObjectGraphSerializer::SerializeObjectRecord(
        object,
        NLS::Engine::Serialize::ObjectId(NLS::Guid::NewDeterministic("Reflected.Serializable.Object")));

    ASSERT_EQ(record.properties.size(), 1u);
    EXPECT_EQ(record.properties[0].name, "value");
    ASSERT_EQ(record.properties[0].value.GetKind(), NLS::Engine::Serialize::PropertyValue::Kind::Integer);
    EXPECT_EQ(record.properties[0].value.GetInteger(), 42);
}

TEST(SceneObjectGraphSerializationTests, ResourceRendererReferencesSerializeAsUnityObjectReferences)
{
    using namespace NLS::Engine::Components;
    using namespace NLS::Engine::Serialize;

    PersistentManager::Instance().Clear();
    MeshFilter meshFilter;
    meshFilter.SetMeshReference(MakePPtr<MeshFilter::Mesh>(ObjectIdentifier::Asset(
        AssetId(NLS::Guid::Parse("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa")),
        MakeLocalIdentifierInFile(
            NLS::Guid::Parse("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"),
            "Library/Artifacts/Hero/meshes/body.nmesh"),
        "Library/Artifacts/Hero/meshes/body.nmesh")));

    const auto meshRecord = ObjectGraphSerializer::SerializeObjectRecord(
        meshFilter,
        ObjectId(NLS::Guid::Parse("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb")));
    const auto* mesh = FindProperty(meshRecord, "mesh");

    ASSERT_NE(mesh, nullptr);
    ASSERT_EQ(mesh->value.GetKind(), PropertyValue::Kind::ObjectReference);
    EXPECT_EQ(mesh->value.GetObjectReference().filePath, "Library/Artifacts/Hero/meshes/body.nmesh");
    EXPECT_NE(mesh->value.GetObjectReference().localIdentifierInFile, 0);

    MeshRenderer MeshRenderer;
    MeshRenderer.SetMaterialReferences({
        MakePPtr<MeshRenderer::Material>(ObjectIdentifier::Asset(
            AssetId(NLS::Guid::Parse("cccccccc-cccc-4ccc-8ccc-cccccccccccc")),
            MakeLocalIdentifierInFile(
                NLS::Guid::Parse("cccccccc-cccc-4ccc-8ccc-cccccccccccc"),
                "Library/Artifacts/Hero/materials/body.nmat"),
            "Library/Artifacts/Hero/materials/body.nmat"))
    });

    const auto materialRecord = ObjectGraphSerializer::SerializeObjectRecord(
        MeshRenderer,
        ObjectId(NLS::Guid::Parse("dddddddd-dddd-4ddd-8ddd-dddddddddddd")));
    const auto* materials = FindProperty(materialRecord, "materials");

    ASSERT_NE(materials, nullptr);
    ASSERT_EQ(materials->value.GetKind(), PropertyValue::Kind::Array);
    ASSERT_EQ(materials->value.GetArray().size(), 1u);
    ASSERT_EQ(materials->value.GetArray()[0].GetKind(), PropertyValue::Kind::ObjectReference);
    EXPECT_EQ(materials->value.GetArray()[0].GetObjectReference().filePath, "Library/Artifacts/Hero/materials/body.nmat");
    EXPECT_NE(materials->value.GetArray()[0].GetObjectReference().localIdentifierInFile, 0);
}

#if 0
TEST(SceneObjectGraphSerializationTests, PolicyAwareSceneInstantiationPreservesMalformedDeferredMeshReference)
{
    using namespace NLS::Engine::Serialize;

    PersistentManager::Instance().Clear();

    auto document = MakeSimpleSceneDocument();
    auto& gameObject = document.objects.front();
    const auto meshFilterId = ObjectId(NLS::Guid::Parse("eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee"));
    auto* components = FindMutableProperty(gameObject, "components");
    ASSERT_NE(components, nullptr);
    components->value = PropertyValue::Array({
        PropertyValue::OwnedReference(meshFilterId),
        components->value.GetArray().front()
    });

    ObjectRecord meshFilter;
    meshFilter.id = meshFilterId;
    meshFilter.localIdentifierInFile = MakeLocalIdentifierInFile(meshFilterId);
    meshFilter.typeName = "NLS::Engine::Components::MeshFilter";
    meshFilter.debugName = "MeshFilter";
    meshFilter.properties.push_back({"mesh", PropertyValue::String("legacy malformed mesh reference")});
    document.objects.insert(document.objects.begin() + 1, std::move(meshFilter));

    LoadPolicy policy;
    policy.deferAssetReferenceResolution = true;
    policy.invalidReferencePolicy = InvalidReferencePolicy::Preserve;

    SceneInstantiationResult result;
    EXPECT_NO_THROW(result = ObjectGraphInstantiator::InstantiateScene(document, policy));

    ASSERT_NE(result.scene, nullptr);
    auto* loadedObject = result.scene->FindGameObjectByName("Player");
    ASSERT_NE(loadedObject, nullptr);
    auto* loadedMeshFilter = loadedObject->GetComponent<NLS::Engine::Components::MeshFilter>();
    ASSERT_NE(loadedMeshFilter, nullptr);
    EXPECT_TRUE(loadedMeshFilter->GetMeshReference().IsNull());
}

TEST(SceneObjectGraphSerializationTests, ReflectedMeshReferenceWriteClearsMeshFilterRuntimeMeshReference)
{
    using namespace NLS::Engine::Components;
    using namespace NLS::Engine::Serialize;

    PersistentManager::Instance().Clear();

    NLS::Render::Resources::Mesh meshResource({}, {}, 0u);

    MeshFilter renderer;
    renderer.SetMesh(&meshResource);
    ASSERT_EQ(renderer.ResolveMesh(), &meshResource);

    const auto type = NLS_TYPEOF(MeshFilter);
    const auto& mesh = type.GetField("mesh");
    ASSERT_TRUE(mesh.IsValid());

    auto instance = NLS::meta::Variant(renderer, NLS::meta::variant_policy::NoCopy {});
    auto empty = NLS::Engine::Serialize::PPtr<MeshFilter::Mesh> {};
    ASSERT_TRUE(mesh.SetValue(instance, empty));

    EXPECT_EQ(renderer.ResolveMesh(), nullptr);
    EXPECT_TRUE(renderer.GetMeshReference().IsNull());
}

TEST(SceneObjectGraphSerializationTests, DeferredMeshRendererInstantiationKeepsResolvedMaterialArtifactPathHint)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Engine::Assets;
    using namespace NLS::Engine::Serialize;

    PersistentManager::Instance().Clear();

    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("72727272-7272-4272-8272-727272727272"));
    RuntimeAssetManifest manifest;
    manifest.entries.push_back({
        assetId,
        "material:body",
        ArtifactType::Material,
        "Nullus.Material",
        "Library/Artifacts/Hero/materials/body.nmat",
        "sha256:material",
        {}
    });
    RuntimeAssetDatabase runtimeAssets(manifest);
    NLS::Core::ServiceLocator::Provide<RuntimeAssetDatabase>(runtimeAssets);

    auto document = MakeSimpleSceneDocument();
    auto& gameObject = document.objects.front();
    const auto meshRendererId = ObjectId(NLS::Guid::Parse("83838383-8383-4383-8383-838383838383"));
    auto* components = FindMutableProperty(gameObject, "components");
    ASSERT_NE(components, nullptr);
    components->value = PropertyValue::Array({
        PropertyValue::OwnedReference(meshRendererId),
        components->value.GetArray().front()
    });

    ObjectRecord meshRenderer;
    meshRenderer.id = meshRendererId;
    meshRenderer.localIdentifierInFile = MakeLocalIdentifierInFile(meshRendererId);
    meshRenderer.typeName = "NLS::Engine::Components::MeshRenderer";
    meshRenderer.debugName = "MeshRenderer";
    meshRenderer.properties.push_back({
        "materials",
        PropertyValue::Array({
            PropertyValue::ObjectReference(ObjectIdentifier::Asset(
                NLS::Engine::Serialize::AssetId(assetId.GetGuid()),
                MakeLocalIdentifierInFile(assetId.GetGuid(), "material:body"),
                "material:body"))
        })
    });
    document.objects.insert(document.objects.begin() + 1, std::move(meshRenderer));

    LoadPolicy policy;
    policy.deferAssetReferenceResolution = true;

    auto result = ObjectGraphInstantiator::InstantiateScene(document, policy);
    ASSERT_NE(result.scene, nullptr);
    auto* loadedObject = result.scene->FindGameObjectByName("Player");
    ASSERT_NE(loadedObject, nullptr);
    auto* loadedRenderer = loadedObject->GetComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(loadedRenderer, nullptr);

    const auto paths = loadedRenderer->GetMaterialPaths();
    ASSERT_EQ(paths.size(), 1u);
    EXPECT_EQ(paths[0], "Library/Artifacts/Hero/materials/body.nmat")
        << "Deferred prefab/model preview must expose artifact material paths so RenderScene suppresses unresolved slots instead of drawing the default material.";
    EXPECT_EQ(loadedRenderer->GetMaterialAtIndex(0), nullptr);

    NLS::Core::ServiceLocator::Remove<RuntimeAssetDatabase>();
}

TEST(SceneObjectGraphSerializationTests, SetMeshPathSynchronizesMeshPPtrForInspectorAndPrefabSerialization)
{
    using namespace NLS::Engine::Components;
    using namespace NLS::Engine::Serialize;

    PersistentManager::Instance().Clear();

    NLS::Core::ResourceManagement::MeshManager meshManager;
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::MeshManager>(meshManager);
    auto* meshResource = new NLS::Render::Resources::Mesh({}, {}, 0u);
    meshManager.RegisterResource("Library/Artifacts/Hero/body.nmesh", meshResource);

    MeshFilter renderer;
    renderer.SetMeshPath("Library/Artifacts/Hero/body.nmesh");

    ASSERT_EQ(renderer.ResolveMesh(), meshResource);
    ASSERT_FALSE(renderer.GetMeshReference().IsNull());
    ASSERT_EQ(renderer.GetMeshReference().Get(), meshResource);

    ObjectIdentifier identifier;
    ASSERT_TRUE(PersistentManager::Instance().InstanceIDToObjectIdentifier(
        renderer.GetMeshReference().GetInstanceID(),
        identifier));
    EXPECT_TRUE(identifier.IsAsset());
    EXPECT_EQ(identifier.filePath, "Library/Artifacts/Hero/body.nmesh");

    const auto meshRecord = ObjectGraphSerializer::SerializeObjectRecord(
        renderer,
        ObjectId(NLS::Guid::Parse("11111111-2222-4333-8444-555555555555")));
    const auto* mesh = FindProperty(meshRecord, "mesh");
    ASSERT_NE(mesh, nullptr);
    ASSERT_EQ(mesh->value.GetKind(), PropertyValue::Kind::ObjectReference);
    EXPECT_EQ(mesh->value.GetObjectReference().filePath, "Library/Artifacts/Hero/body.nmesh");
    EXPECT_NE(mesh->value.GetObjectReference().localIdentifierInFile, 0);

    meshManager.UnloadResources();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::MeshManager>();
}

TEST(SceneObjectGraphSerializationTests, SetMeshPathBindsMeshPPtrWhenDeferredPathResolvesLater)
{
    using namespace NLS::Engine::Components;
    using namespace NLS::Engine::Serialize;

    PersistentManager::Instance().Clear();

    NLS::Core::ResourceManagement::MeshManager meshManager;
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::MeshManager>(meshManager);

    MeshFilter renderer;
    renderer.SetMeshPath("Library/Artifacts/Hero/deferred.nmesh");

    EXPECT_EQ(renderer.ResolveMesh(), nullptr);
    EXPECT_TRUE(renderer.GetMeshReference().IsNull());

    auto* meshResource = new NLS::Render::Resources::Mesh({}, {}, 0u);
    meshManager.RegisterResource("Library/Artifacts/Hero/deferred.nmesh", meshResource);

    ASSERT_EQ(renderer.ResolveMesh(), meshResource);
    ASSERT_FALSE(renderer.GetMeshReference().IsNull());
    ASSERT_EQ(renderer.GetMeshReference().Get(), meshResource);

    ObjectIdentifier identifier;
    ASSERT_TRUE(PersistentManager::Instance().InstanceIDToObjectIdentifier(
        renderer.GetMeshReference().GetInstanceID(),
        identifier));
    EXPECT_TRUE(identifier.IsAsset());
    EXPECT_EQ(identifier.filePath, "Library/Artifacts/Hero/deferred.nmesh");

    meshManager.UnloadResources();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::MeshManager>();
}

TEST(SceneObjectGraphSerializationTests, DeferredMeshPathResolvePreservesExistingMeshAssetIdentity)
{
    using namespace NLS::Engine::Components;
    using namespace NLS::Engine::Serialize;

    PersistentManager::Instance().Clear();

    NLS::Core::ResourceManagement::MeshManager meshManager;
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::MeshManager>(meshManager);

    auto* meshResource = new NLS::Render::Resources::Mesh({}, {}, 0u);
    const auto realIdentifier = ObjectIdentifier::Asset(
        AssetId(NLS::Guid::Parse("31313131-3131-4131-8131-313131313131")),
        MakeLocalIdentifierInFile(
            NLS::Guid::Parse("31313131-3131-4131-8131-313131313131"),
            "mesh:Hero/body"),
        "Library/Artifacts/Hero/body.nmesh");
    ASSERT_NE(
        PersistentManager::Instance().BindObjectIdentifier(*meshResource, realIdentifier),
        InstanceID_None);
    meshManager.RegisterResource("Library/Artifacts/Hero/deferred-alias.nmesh", meshResource);

    MeshFilter renderer;
    renderer.SetMeshPath("Library/Artifacts/Hero/deferred-alias.nmesh");

    ASSERT_EQ(renderer.ResolveMesh(), meshResource);
    ASSERT_FALSE(renderer.GetMeshReference().IsNull());
    EXPECT_EQ(renderer.GetMeshReference().Get(), meshResource);

    ObjectIdentifier roundTripped;
    ASSERT_TRUE(PersistentManager::Instance().InstanceIDToObjectIdentifier(
        renderer.GetMeshReference().GetInstanceID(),
        roundTripped));
    EXPECT_EQ(roundTripped, realIdentifier);

    meshManager.UnloadResources();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::MeshManager>();
}

TEST(SceneObjectGraphSerializationTests, SetMeshSynchronizesTransientMeshPPtrForInspectorDisplay)
{
    using namespace NLS::Engine::Components;
    using namespace NLS::Engine::Serialize;

    PersistentManager::Instance().Clear();

    NLS::Render::Resources::Mesh meshResource({}, {}, 0u);

    MeshFilter meshFilter;
    meshFilter.SetMesh(&meshResource);

    EXPECT_EQ(meshFilter.ResolveMesh(), &meshResource);
    ASSERT_FALSE(meshFilter.GetMeshReference().IsNull());
    EXPECT_EQ(meshFilter.GetMeshReference().Get(), &meshResource);

    ObjectIdentifier identifier;
    EXPECT_FALSE(PersistentManager::Instance().InstanceIDToObjectIdentifier(
        meshFilter.GetMeshReference().GetInstanceID(),
        identifier));

    const auto meshRecord = ObjectGraphSerializer::SerializeObjectRecord(
        meshFilter,
        ObjectId(NLS::Guid::Parse("22222222-3333-4444-8555-666666666666")));
    const auto* mesh = FindProperty(meshRecord, "mesh");
    ASSERT_NE(mesh, nullptr);
    ASSERT_EQ(mesh->value.GetKind(), PropertyValue::Kind::ObjectReference);
    EXPECT_FALSE(mesh->value.GetObjectReference().IsValid());
    EXPECT_EQ(meshFilter.GetMeshReference().Get(), &meshResource);
}

TEST(SceneObjectGraphSerializationTests, SetResolvedMeshPreservesExistingMeshPPtrIdentityWhenResolvingPrefabReference)
{
    using namespace NLS::Engine::Components;
    using namespace NLS::Engine::Serialize;

    PersistentManager::Instance().Clear();

    NLS::Render::Resources::Mesh meshResource({}, {}, 0u);

    const auto assetGuid = NLS::Guid::Parse("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    const auto reference = ObjectIdentifier::Asset(
        AssetId(assetGuid),
        MakeLocalIdentifierInFile(assetGuid, "mesh:body"),
        "Library/Artifacts/Hero/body.nmesh");

    MeshFilter renderer;
    renderer.SetMeshReference(MakePPtr<MeshFilter::Mesh>(reference));
    renderer.SetResolvedMeshFromReference(&meshResource);

    ASSERT_EQ(renderer.ResolveMesh(), &meshResource);
    ASSERT_EQ(renderer.GetMeshReference().Get(), &meshResource);

    ObjectIdentifier identifier;
    ASSERT_TRUE(PersistentManager::Instance().InstanceIDToObjectIdentifier(
        renderer.GetMeshReference().GetInstanceID(),
        identifier));
    EXPECT_EQ(identifier.guid, reference.guid);
    EXPECT_EQ(identifier.localIdentifierInFile, reference.localIdentifierInFile);
    EXPECT_EQ(identifier.filePath, reference.filePath);
}

TEST(SceneObjectGraphSerializationTests, BuiltInPrimitivePPtrFallbackPathSurvivesRuntimeAssetDatabaseMiss)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Engine::Assets;
    using namespace NLS::Engine::Components;
    using namespace NLS::Engine::Serialize;

    PersistentManager::Instance().Clear();

    RuntimeAssetManifest manifest;
    RuntimeAssetDatabase runtimeAssets(manifest);
    NLS::Core::ServiceLocator::Provide<RuntimeAssetDatabase>(runtimeAssets);

    const auto assetGuid = NLS::Guid::Parse("abababab-abab-4aba-8bab-abababababab");
    const auto reference = ObjectIdentifier::Asset(
        NLS::Engine::Serialize::AssetId(assetGuid),
        MakeLocalIdentifierInFile(assetGuid, "mesh:Cube"),
        "builtin:Primitive/Cube");

    MeshFilter renderer;
    renderer.SetMeshReference(MakePPtr<MeshFilter::Mesh>(reference));

    ObjectIdentifier identifier;
    ASSERT_TRUE(PersistentManager::Instance().InstanceIDToObjectIdentifier(
        renderer.GetMeshReference().GetInstanceID(),
        identifier));
    EXPECT_EQ(ResolveAssetReferencePath(identifier), "builtin:Primitive/Cube");

    NLS::Core::ServiceLocator::Remove<RuntimeAssetDatabase>();
}

TEST(SceneObjectGraphSerializationTests, ReflectedMaterialReferencesWriteClearsRendererRuntimeMaterialCache)
{
    using namespace NLS::Engine::Components;
    using namespace NLS::Engine::Serialize;

    PersistentManager::Instance().Clear();

    NLS::Render::Resources::Material material;
    MeshRenderer renderer;
    renderer.SetMaterialAtIndex(0u, material);
    ASSERT_EQ(renderer.GetMaterialAtIndex(0u), &material);

    const auto type = NLS_TYPEOF(MeshRenderer);
    const auto& materials = type.GetField("materials");
    ASSERT_TRUE(materials.IsValid());

    auto instance = NLS::meta::Variant(renderer, NLS::meta::variant_policy::NoCopy {});
    auto empty = NLS::Array<NLS::Engine::Serialize::PPtr<MeshRenderer::Material>> {};
    ASSERT_TRUE(materials.SetValue(instance, empty));

    EXPECT_EQ(renderer.GetMaterialAtIndex(0u), nullptr);
    EXPECT_TRUE(renderer.GetMaterialReferences().empty());
}

TEST(SceneObjectGraphSerializationTests, MeshFilterLazyResolutionPreservesUnityReferenceIdentity)
{
    using namespace NLS::Engine::Components;
    using namespace NLS::Engine::Serialize;

    PersistentManager::Instance().Clear();
    MeshFilter meshFilter;
    const auto reference = ObjectIdentifier::Asset(
        AssetId(NLS::Guid::Parse("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa")),
        MakeLocalIdentifierInFile(
            NLS::Guid::Parse("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"),
            "Library/Artifacts/Hero/meshes/body.nmesh"),
        "Library/Artifacts/Hero/meshes/body.nmesh");
    meshFilter.SetMeshReference(MakePPtr<MeshFilter::Mesh>(reference));

    EXPECT_EQ(meshFilter.ResolveMesh(), nullptr);

    const auto meshRecord = ObjectGraphSerializer::SerializeObjectRecord(
        meshFilter,
        ObjectId(NLS::Guid::Parse("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb")));
    const auto* mesh = FindProperty(meshRecord, "mesh");

    ASSERT_NE(mesh, nullptr);
    ASSERT_EQ(mesh->value.GetKind(), PropertyValue::Kind::ObjectReference);
    EXPECT_EQ(mesh->value.GetObjectReference().guid, reference.guid);
    EXPECT_EQ(mesh->value.GetObjectReference().localIdentifierInFile, reference.localIdentifierInFile);
    EXPECT_EQ(mesh->value.GetObjectReference().fileType, reference.fileType);
    EXPECT_EQ(mesh->value.GetObjectReference().filePath, reference.filePath);
}

TEST(SceneObjectGraphSerializationTests, MeshFilterLazyResolutionUsesRuntimeAssetIdentityBeforePathHint)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Engine::Assets;
    using namespace NLS::Engine::Components;
    using namespace NLS::Engine::Serialize;

    PersistentManager::Instance().Clear();
    NLS::Core::ResourceManagement::MeshManager meshManager;
    NLS::Core::ResourceManagement::MaterialManager materialManager;
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::MeshManager>(meshManager);
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::MaterialManager>(materialManager);

    auto* mesh = new NLS::Render::Resources::Mesh({}, {}, 0u);
    meshManager.RegisterResource("Library/Artifacts/Hero/meshes/body.nmesh", mesh);

    auto* material = new NLS::Render::Resources::Material();
    material->path = "Library/Artifacts/Hero/materials/body.nmat";
    materialManager.RegisterResource("Library/Artifacts/Hero/materials/body.nmat", material);

    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("51515151-5151-4151-8151-515151515151"));
    RuntimeAssetManifest manifest;
    manifest.entries.push_back({
        assetId,
        "mesh:body",
        ArtifactType::Mesh,
        "Nullus.Mesh",
        "Library/Artifacts/Hero/meshes/body.nmesh",
        "sha256:model",
        {}
    });
    manifest.entries.push_back({
        assetId,
        "material:body",
        ArtifactType::Material,
        "Nullus.Material",
        "Library/Artifacts/Hero/materials/body.nmat",
        "sha256:material",
        {}
    });
    RuntimeAssetDatabase runtimeAssets(manifest);
    NLS::Core::ServiceLocator::Provide<RuntimeAssetDatabase>(runtimeAssets);

    MeshFilter meshFilter;
    const auto staleModelReference = ObjectIdentifier::Asset(
        NLS::Engine::Serialize::AssetId(assetId.GetGuid()),
        MakeLocalIdentifierInFile(assetId.GetGuid(), "mesh:body"),
        "Library/Artifacts/Stale/wrong.nmesh");
    meshFilter.SetMeshReference(MakePPtr<MeshFilter::Mesh>(staleModelReference));
    EXPECT_EQ(meshFilter.ResolveMesh(), mesh);
    EXPECT_EQ(meshFilter.GetModelPath(), "Library/Artifacts/Hero/meshes/body.nmesh");
    EXPECT_EQ(ResolveObjectIdentifier(meshFilter.GetMeshReference()).filePath, "Library/Artifacts/Hero/meshes/body.nmesh");
    EXPECT_EQ(meshFilter.GetMeshReference().Get(), mesh);

    MeshRenderer MeshRenderer;
    const auto staleMaterialReference = ObjectIdentifier::Asset(
        NLS::Engine::Serialize::AssetId(assetId.GetGuid()),
        MakeLocalIdentifierInFile(assetId.GetGuid(), "material:body"),
        "Library/Artifacts/Stale/wrong.nmat");
    MeshRenderer.SetMaterialReferences({
        MakePPtr<MeshRenderer::Material>(staleMaterialReference)
    });
    ASSERT_EQ(MeshRenderer.ResolveMaterials()[0], material);
    EXPECT_EQ(MeshRenderer.GetMaterialPaths()[0], "Library/Artifacts/Hero/materials/body.nmat");
    EXPECT_EQ(ResolveObjectIdentifier(MeshRenderer.GetMaterialReferences()[0]).filePath, "Library/Artifacts/Hero/materials/body.nmat");
    EXPECT_EQ(MeshRenderer.GetMaterialReferences()[0].Get(), material);

    meshManager.UnloadResources();
    materialManager.UnloadResources();
    NLS::Core::ServiceLocator::Remove<RuntimeAssetDatabase>();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::MeshManager>();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::MaterialManager>();
}

TEST(SceneObjectGraphSerializationTests, MeshFilterDoesNotWarnForColdExistingMeshArtifactPath)
{
    using namespace NLS::Engine::Components;
    using namespace NLS::Engine::Serialize;

    PersistentManager::Instance().Clear();

    const ScopedTempDirectory root(
        std::filesystem::temp_directory_path() /
        ("nullus_mesh_filter_lazy_load_" + NLS::Guid::New().ToString()));
    const auto projectAssetsRoot = (root.Path() / "Assets").string() + "/";

    const auto meshPath = std::string("Library/Artifacts/Hero/meshes/mesh%3Aparser%2Fmesh%2F0.nmesh");
    const auto fullMeshPath = root.Path() / meshPath;
    std::filesystem::create_directories(fullMeshPath.parent_path());

    NLS::Render::Assets::MeshArtifactData artifact;
    artifact.vertices.resize(3u);
    artifact.indices = {0u, 1u, 2u};
    const auto bytes = NLS::Render::Assets::SerializeMeshArtifact(artifact);
    {
        std::ofstream output(fullMeshPath, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    NLS::Core::ResourceManagement::MeshManager meshManager;
    const ScopedMeshManagerContext meshManagerContext(meshManager, projectAssetsRoot, "App/Assets/Engine/");

    const auto reference = ObjectIdentifier::Asset(
        AssetId(NLS::Guid::Parse("89898989-8989-4989-8989-898989898989")),
        MakeLocalIdentifierInFile(
            NLS::Guid::Parse("89898989-8989-4989-8989-898989898989"),
            "mesh:parser/mesh/0"),
        meshPath);

    MeshFilter meshFilter;
    meshFilter.SetMeshReference(MakePPtr<MeshFilter::Mesh>(reference));

    bool sawFailedResolveWarning = false;
    const ScopedLogListener listener(
        [&sawFailedResolveWarning](const NLS::Debug::LogData& log)
        {
            if (log.logLevel == NLS::Debug::ELogLevel::LOG_WARNING &&
                log.message.find("Failed to resolve mesh filter mesh path during reflection load") != std::string::npos)
            {
                sawFailedResolveWarning = true;
            }
        });

    EXPECT_FALSE(meshManager.IsResourceRegistered(meshPath));
    EXPECT_EQ(meshFilter.ResolveMesh(), nullptr);

    EXPECT_FALSE(sawFailedResolveWarning);
    EXPECT_EQ(meshFilter.GetMeshReference().Get(), nullptr);
    EXPECT_EQ(meshFilter.GetModelPath(), meshPath);
    EXPECT_FALSE(meshManager.IsResourceRegistered(meshPath));
}

TEST(SceneObjectGraphSerializationTests, MeshFilterDoesNotWarnForColdBuiltinPrimitiveAliasPath)
{
    using namespace NLS::Engine::Components;
    using namespace NLS::Engine::Serialize;

    PersistentManager::Instance().Clear();
    const ScopedSceneObjectGraphTestDriver driverContext;

    const ScopedTempDirectory root(
        std::filesystem::temp_directory_path() /
        ("nullus_mesh_filter_builtin_lazy_load_" + NLS::Guid::New().ToString()));
    const auto projectAssetsRoot = (root.Path() / "Project" / "Assets").string() + "/";
    const auto engineAssetsRoot = (root.Path() / "EngineAssets").string() + "/";
    const auto artifactPath = root.Path() / "EngineAssets" / "Library" / "BuiltinArtifacts" / "Models" / "Cube.nmesh";
    std::filesystem::create_directories(artifactPath.parent_path());

    NLS::Render::Assets::MeshArtifactData artifact;
    artifact.vertices.resize(3u);
    artifact.indices = {0u, 1u, 2u};
    const auto bytes = NLS::Render::Assets::SerializeMeshArtifact(artifact);
    {
        std::ofstream output(artifactPath, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    NLS::Core::ResourceManagement::MeshManager meshManager;
    const ScopedMeshManagerContext meshManagerContext(meshManager, projectAssetsRoot, engineAssetsRoot);

    const auto meshPath = std::string("builtin:Primitive/Cube");
    const auto reference = ObjectIdentifier::Asset(
        AssetId(NLS::Guid::Parse("19191919-1919-4919-8919-191919191919")),
        MakeLocalIdentifierInFile(
            NLS::Guid::Parse("19191919-1919-4919-8919-191919191919"),
            "mesh:Cube"),
        meshPath);

    MeshFilter meshFilter;
    meshFilter.SetMeshReference(MakePPtr<MeshFilter::Mesh>(reference));

    bool sawFailedResolveWarning = false;
    const ScopedLogListener listener(
        [&sawFailedResolveWarning](const NLS::Debug::LogData& log)
        {
            if (log.logLevel == NLS::Debug::ELogLevel::LOG_WARNING &&
                log.message.find("Failed to resolve mesh filter mesh path during reflection load") != std::string::npos)
            {
                sawFailedResolveWarning = true;
            }
        });

    EXPECT_FALSE(meshManager.IsResourceRegistered(meshPath));
    auto* resolvedMesh = meshFilter.ResolveMesh();

    ASSERT_NE(resolvedMesh, nullptr);
    EXPECT_FALSE(sawFailedResolveWarning);
    EXPECT_EQ(resolvedMesh->GetVertexCount(), 3u);
    EXPECT_EQ(meshFilter.GetMeshReference().Get(), resolvedMesh);
    EXPECT_EQ(meshFilter.GetModelPath(), meshPath);
    EXPECT_TRUE(meshManager.IsResourceRegistered(meshPath));
}

TEST(SceneObjectGraphSerializationTests, MeshFilterStillWarnsOnceForMissingColdMeshArtifactPath)
{
    using namespace NLS::Engine::Components;
    using namespace NLS::Engine::Serialize;

    PersistentManager::Instance().Clear();

    const ScopedTempDirectory root(
        std::filesystem::temp_directory_path() /
        ("nullus_mesh_filter_missing_lazy_load_" + NLS::Guid::New().ToString()));
    const auto projectAssetsRoot = (root.Path() / "Assets").string() + "/";

    NLS::Core::ResourceManagement::MeshManager meshManager;
    const ScopedMeshManagerContext meshManagerContext(meshManager, projectAssetsRoot, "App/Assets/Engine/");

    const auto meshPath = std::string("Library/Artifacts/Hero/meshes/missing.nmesh");
    const auto reference = ObjectIdentifier::Asset(
        AssetId(NLS::Guid::Parse("97979797-9797-4979-8979-979797979797")),
        MakeLocalIdentifierInFile(
            NLS::Guid::Parse("97979797-9797-4979-8979-979797979797"),
            "mesh:parser/mesh/missing"),
        meshPath);

    MeshFilter meshFilter;
    meshFilter.SetMeshReference(MakePPtr<MeshFilter::Mesh>(reference));

    int failedResolveWarnings = 0;
    const ScopedLogListener listener(
        [&failedResolveWarnings](const NLS::Debug::LogData& log)
        {
            if (log.logLevel == NLS::Debug::ELogLevel::LOG_WARNING &&
                log.message.find("Failed to resolve mesh filter mesh path during reflection load") != std::string::npos)
            {
                ++failedResolveWarnings;
            }
        });

    EXPECT_EQ(meshFilter.ResolveMesh(), nullptr);
    EXPECT_EQ(meshFilter.ResolveMesh(), nullptr);
    EXPECT_EQ(failedResolveWarnings, 1);
    EXPECT_FALSE(meshManager.IsResourceRegistered(meshPath));
}

TEST(SceneObjectGraphSerializationTests, MeshRendererKeepsUnityReferenceIdentityWithoutPathHint)
{
    using namespace NLS::Engine::Components;
    using namespace NLS::Engine::Serialize;

    PersistentManager::Instance().Clear();
    MeshRenderer renderer;
    renderer.SetMaterialReferences({
        MakePPtr<MeshRenderer::Material>(ObjectIdentifier::Asset(
            AssetId(NLS::Guid::Parse("eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee")),
            2100000))
    });

    const auto record = ObjectGraphSerializer::SerializeObjectRecord(
        renderer,
        ObjectId(NLS::Guid::Parse("ffffffff-ffff-4fff-8fff-ffffffffffff")));
    const auto* materials = FindProperty(record, "materials");

    ASSERT_NE(materials, nullptr);
    ASSERT_EQ(materials->value.GetKind(), PropertyValue::Kind::Array);
    ASSERT_EQ(materials->value.GetArray().size(), 1u);
    ASSERT_EQ(materials->value.GetArray()[0].GetKind(), PropertyValue::Kind::ObjectReference);
    const auto& reference = materials->value.GetArray()[0].GetObjectReference();
    EXPECT_EQ(reference.guid, NLS::Guid::Parse("eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee"));
    EXPECT_EQ(reference.localIdentifierInFile, 2100000);
    EXPECT_EQ(reference.filePath, "");
}

TEST(SceneObjectGraphSerializationTests, MeshFilterRejectsResolvedMeshThatConflictsWithBoundPPtrIdentity)
{
    using namespace NLS::Engine::Components;
    using namespace NLS::Engine::Serialize;

    PersistentManager::Instance().Clear();

    NLS::Render::Resources::Mesh firstMesh({}, {}, 0u);
    NLS::Render::Resources::Mesh secondMesh({}, {}, 0u);

    const auto reference = ObjectIdentifier::Asset(
        AssetId(NLS::Guid::Parse("12121212-1212-4121-8121-121212121212")),
        MakeLocalIdentifierInFile(
            NLS::Guid::Parse("12121212-1212-4121-8121-121212121212"),
            "mesh:body"),
        "Library/Artifacts/Hero/body.nmesh");

    MeshFilter renderer;
    renderer.SetMeshReference(MakePPtr<MeshFilter::Mesh>(reference));
    renderer.SetResolvedMeshFromReference(&firstMesh);
    ASSERT_EQ(renderer.ResolveMesh(), &firstMesh);
    ASSERT_EQ(renderer.GetMeshReference().Get(), &firstMesh);

    renderer.SetResolvedMeshFromReference(&secondMesh);
    EXPECT_EQ(renderer.ResolveMesh(), &firstMesh);
    EXPECT_EQ(renderer.GetMeshReference().Get(), &firstMesh);
}

TEST(SceneObjectGraphSerializationTests, MeshFilterTransientResolvedMeshPreservesBoundPPtrIdentity)
{
    using namespace NLS::Engine::Components;
    using namespace NLS::Engine::Serialize;

    PersistentManager::Instance().Clear();

    auto mesh = std::make_shared<NLS::Render::Resources::Mesh>(
        std::vector<NLS::Render::Geometry::Vertex> {},
        std::vector<uint32_t> {},
        0u);

    const auto reference = ObjectIdentifier::Asset(
        AssetId(NLS::Guid::Parse("46464646-4646-4646-8646-464646464646")),
        MakeLocalIdentifierInFile(
            NLS::Guid::Parse("46464646-4646-4646-8646-464646464646"),
            "mesh:body"),
        "Library/Artifacts/Hero/body.nmesh");

    MeshFilter renderer;
    renderer.SetMeshReference(MakePPtr<MeshFilter::Mesh>(reference));
    renderer.SetResolvedTransientMeshFromReference(mesh);

    ASSERT_EQ(renderer.ResolveMesh(), mesh.get());
    EXPECT_EQ(renderer.GetMeshReference().Get(), mesh.get());

    ObjectIdentifier rebound;
    ASSERT_TRUE(PersistentManager::Instance().InstanceIDToObjectIdentifier(
        renderer.GetMeshReference().GetInstanceID(),
        rebound));
    EXPECT_EQ(rebound.guid, reference.guid);
    EXPECT_EQ(rebound.localIdentifierInFile, reference.localIdentifierInFile);
    EXPECT_EQ(rebound.filePath, reference.filePath);
}

TEST(SceneObjectGraphSerializationTests, MeshFilterTransientMeshRemainsRenderableWhenAssetPPtrAlreadyBound)
{
    using namespace NLS::Engine::Components;
    using namespace NLS::Engine::Serialize;

    PersistentManager::Instance().Clear();

    NLS::Render::Resources::Mesh existingMesh({}, {}, 0u);
    auto previewMesh = std::make_shared<NLS::Render::Resources::Mesh>(
        std::vector<NLS::Render::Geometry::Vertex> {},
        std::vector<uint32_t> {},
        0u);

    const auto reference = ObjectIdentifier::Asset(
        AssetId(NLS::Guid::Parse("58585858-5858-4858-8858-585858585858")),
        MakeLocalIdentifierInFile(
            NLS::Guid::Parse("58585858-5858-4858-8858-585858585858"),
            "mesh:body"),
        "Library/Artifacts/Hero/body.nmesh");

    MeshFilter existingInstance;
    existingInstance.SetMeshReference(MakePPtr<MeshFilter::Mesh>(reference));
    existingInstance.SetResolvedMeshFromReference(&existingMesh);
    ASSERT_EQ(existingInstance.ResolveMesh(), &existingMesh);

    MeshFilter previewInstance;
    previewInstance.SetMeshReference(MakePPtr<MeshFilter::Mesh>(reference));
    previewInstance.SetResolvedTransientMeshFromReference(previewMesh);

    EXPECT_TRUE(previewInstance.HasResolvedTransientMesh());
    EXPECT_EQ(previewInstance.ResolveMesh(), previewMesh.get())
        << "Scene View drag preview must stay renderable even when another prefab instance already owns the asset PPtr identity.";
    EXPECT_EQ(previewInstance.GetMeshReference().Get(), &existingMesh)
        << "Transient preview binding must not steal the persistent asset identity from an existing prefab instance.";
}

TEST(SceneObjectGraphSerializationTests, MeshFilterLazyBindConflictDoesNotPoisonRetryPath)
{
    using namespace NLS::Engine::Components;
    using namespace NLS::Engine::Serialize;

    PersistentManager::Instance().Clear();

    NLS::Core::ResourceManagement::MeshManager meshManager;
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::MeshManager>(meshManager);

    auto* firstMesh = new NLS::Render::Resources::Mesh({}, {}, 0u);
    auto* secondMesh = new NLS::Render::Resources::Mesh({}, {}, 0u);

    const auto reference = ObjectIdentifier::Asset(
        AssetId(NLS::Guid::Parse("67676767-6767-4676-8676-676767676767")),
        MakeLocalIdentifierInFile(
            NLS::Guid::Parse("67676767-6767-4676-8676-676767676767"),
            "mesh:body"),
        "Library/Artifacts/Hero/body.nmesh");

    MeshFilter owner;
    owner.SetMeshReference(MakePPtr<MeshFilter::Mesh>(reference));
    owner.SetResolvedMeshFromReference(firstMesh);

    MeshFilter retrying;
    retrying.SetMeshReference(MakePPtr<MeshFilter::Mesh>(reference));
    meshManager.RegisterResource("Library/Artifacts/Hero/body.nmesh", secondMesh);
    EXPECT_EQ(retrying.ResolveMesh(), firstMesh);
    EXPECT_EQ(retrying.GetMeshReference().Get(), firstMesh);

    owner.SetMesh(nullptr);
    delete firstMesh;
    retrying.SetMeshReference(MakePPtr<MeshFilter::Mesh>(reference));

    EXPECT_EQ(retrying.ResolveMesh(), secondMesh);
    EXPECT_EQ(retrying.GetMeshReference().Get(), secondMesh);

    meshManager.UnloadResources();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::MeshManager>();
}

TEST(SceneObjectGraphSerializationTests, MeshFilterRetriesPreviouslyMissingMeshPathAfterResourceRegistration)
{
    using namespace NLS::Engine::Components;
    using namespace NLS::Engine::Serialize;

    PersistentManager::Instance().Clear();

    NLS::Core::ResourceManagement::MeshManager meshManager;
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::MeshManager>(meshManager);

    const auto path = std::string("Library/Artifacts/Hero/retry.nmesh");
    const auto reference = ObjectIdentifier::Asset(
        AssetId(NLS::Guid::Parse("78787878-7878-4878-8878-787878787878")),
        MakeLocalIdentifierInFile(
            NLS::Guid::Parse("78787878-7878-4878-8878-787878787878"),
            "mesh:body"),
        path);

    MeshFilter renderer;
    renderer.SetMeshReference(MakePPtr<MeshFilter::Mesh>(reference));
    EXPECT_EQ(renderer.ResolveMesh(), nullptr);

    auto* mesh = new NLS::Render::Resources::Mesh({}, {}, 0u);
    meshManager.RegisterResource(path, mesh);

    EXPECT_EQ(renderer.ResolveMesh(), mesh);
    EXPECT_EQ(renderer.GetMeshReference().Get(), mesh);

    meshManager.UnloadResources();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::MeshManager>();
}

TEST(SceneObjectGraphSerializationTests, MeshRendererRejectsResolvedMaterialThatConflictsWithBoundPPtrIdentity)
{
    using namespace NLS::Engine::Components;
    using namespace NLS::Engine::Serialize;

    PersistentManager::Instance().Clear();

    const auto reference = ObjectIdentifier::Asset(
        AssetId(NLS::Guid::Parse("23232323-2323-4323-8323-232323232323")),
        MakeLocalIdentifierInFile(
            NLS::Guid::Parse("23232323-2323-4323-8323-232323232323"),
            "material:body"),
        "Library/Artifacts/Hero/body.nmat");

    NLS::Render::Resources::Material first;
    first.path = "Library/Artifacts/Hero/body.nmat";
    NLS::Render::Resources::Material second;
    second.path = "Library/Artifacts/Hero/other.nmat";

    MeshRenderer renderer;
    renderer.SetMaterialReferences({
        MakePPtr<MeshRenderer::Material>(reference)
    });
    renderer.SetResolvedMaterialFromReference(0u, first);
    ASSERT_EQ(renderer.GetMaterialAtIndex(0), &first);
    ASSERT_EQ(renderer.GetMaterialReferences()[0].Get(), &first);

    renderer.SetResolvedMaterialFromReference(0u, second);
    EXPECT_EQ(renderer.GetMaterialAtIndex(0), &first);
    EXPECT_EQ(renderer.GetMaterialReferences()[0].Get(), &first);
    ASSERT_EQ(renderer.GetMaterialPaths().size(), 1u);
    EXPECT_EQ(renderer.GetMaterialPaths()[0], "Library/Artifacts/Hero/body.nmat");
}

TEST(SceneObjectGraphSerializationTests, MeshRendererResolvedMaterialRemainsRenderableWhenAssetPPtrAlreadyBound)
{
    using namespace NLS::Engine::Components;
    using namespace NLS::Engine::Serialize;

    PersistentManager::Instance().Clear();

    const auto reference = ObjectIdentifier::Asset(
        AssetId(NLS::Guid::Parse("67676767-6767-4767-8767-676767676767")),
        MakeLocalIdentifierInFile(
            NLS::Guid::Parse("67676767-6767-4767-8767-676767676767"),
            "material:body"),
        "Library/Artifacts/Hero/body.nmat");

    NLS::Render::Resources::Material existingMaterial;
    existingMaterial.path = "Library/Artifacts/Hero/body.nmat";
    NLS::Render::Resources::Material previewMaterial;
    previewMaterial.path = "Library/Artifacts/Hero/body.nmat";

    MeshRenderer existingInstance;
    existingInstance.SetMaterialReferences({
        MakePPtr<MeshRenderer::Material>(reference)
    });
    existingInstance.SetResolvedMaterialFromReference(0u, existingMaterial);
    ASSERT_EQ(existingInstance.GetMaterialAtIndex(0u), &existingMaterial);

    MeshRenderer previewInstance;
    previewInstance.SetMaterialReferences({
        MakePPtr<MeshRenderer::Material>(reference)
    });
    previewInstance.SetResolvedMaterialFromReference(0u, previewMaterial);

    EXPECT_EQ(previewInstance.GetMaterialAtIndex(0u), &previewMaterial)
        << "Scene View drag/drop must stay renderable even when another prefab instance already owns the material asset PPtr identity.";
    EXPECT_EQ(previewInstance.GetMaterialReferences()[0].Get(), &existingMaterial)
        << "Renderable material binding must not steal the persistent asset identity from an existing prefab instance.";
    ASSERT_EQ(previewInstance.GetMaterialPaths().size(), 1u);
    EXPECT_EQ(previewInstance.GetMaterialPaths()[0], "Library/Artifacts/Hero/body.nmat");
}

TEST(SceneObjectGraphSerializationTests, MeshRendererLazyBindConflictDoesNotPoisonRetryPath)
{
    using namespace NLS::Engine::Components;
    using namespace NLS::Engine::Serialize;

    PersistentManager::Instance().Clear();

    NLS::Core::ResourceManagement::MaterialManager materialManager;
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::MaterialManager>(materialManager);

    auto* first = new NLS::Render::Resources::Material();
    first->path = "Library/Artifacts/Hero/body.nmat";
    auto* second = new NLS::Render::Resources::Material();
    second->path = "Library/Artifacts/Hero/body.nmat";

    const auto reference = ObjectIdentifier::Asset(
        AssetId(NLS::Guid::Parse("56565656-5656-4656-8656-565656565656")),
        MakeLocalIdentifierInFile(
            NLS::Guid::Parse("56565656-5656-4656-8656-565656565656"),
            "material:body"),
        "Library/Artifacts/Hero/body.nmat");

    MeshRenderer owner;
    owner.SetMaterialReferences({
        MakePPtr<MeshRenderer::Material>(reference)
    });
    owner.SetResolvedMaterialFromReference(0u, *first);

    MeshRenderer retrying;
    retrying.SetMaterialReferences({
        MakePPtr<MeshRenderer::Material>(reference)
    });
    materialManager.RegisterResource("Library/Artifacts/Hero/body.nmat", second);
    EXPECT_EQ(retrying.ResolveMaterials()[0], nullptr);

    owner.RemoveAllMaterials();
    delete first;

    EXPECT_EQ(retrying.ResolveMaterials()[0], second);
    EXPECT_EQ(retrying.GetMaterialReferences()[0].Get(), second);

    materialManager.UnloadResources();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::MaterialManager>();
}

TEST(SceneObjectGraphSerializationTests, MeshRendererRetriesPreviouslyMissingMaterialPathAfterResourceRegistration)
{
    using namespace NLS::Engine::Components;
    using namespace NLS::Engine::Serialize;

    PersistentManager::Instance().Clear();

    NLS::Core::ResourceManagement::MaterialManager materialManager;
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::MaterialManager>(materialManager);

    const auto path = std::string("Library/Artifacts/Hero/retry.nmat");
    const auto reference = ObjectIdentifier::Asset(
        AssetId(NLS::Guid::Parse("89898989-8989-4898-8898-898989898989")),
        MakeLocalIdentifierInFile(
            NLS::Guid::Parse("89898989-8989-4898-8898-898989898989"),
            "material:body"),
        path);

    MeshRenderer renderer;
    renderer.SetMaterialReferences({
        MakePPtr<MeshRenderer::Material>(reference)
    });
    EXPECT_EQ(renderer.ResolveMaterials()[0], nullptr);

    auto* material = new NLS::Render::Resources::Material();
    material->path = path;
    materialManager.RegisterResource(path, material);

    EXPECT_EQ(renderer.ResolveMaterials()[0], material);
    EXPECT_EQ(renderer.GetMaterialReferences()[0].Get(), material);

    materialManager.UnloadResources();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::MaterialManager>();
}

TEST(SceneObjectGraphSerializationTests, MeshRendererPathHintsPreservePrefabMaterialPPtrs)
{
    using namespace NLS::Engine::Components;
    using namespace NLS::Engine::Serialize;

    PersistentManager::Instance().Clear();

    const auto reference = ObjectIdentifier::Asset(
        AssetId(NLS::Guid::Parse("91919191-9191-4919-8919-919191919191")),
        MakeLocalIdentifierInFile(
            NLS::Guid::Parse("91919191-9191-4919-8919-919191919191"),
            "material:body"),
        "Library/Artifacts/Hero/materials/body.nmat");

    MeshRenderer renderer;
    renderer.SetMaterialReferences({
        MakePPtr<MeshRenderer::Material>(reference)
    });
    const auto before = renderer.GetMaterialReferences();

    renderer.SetMaterialPathHints({"Library/Artifacts/Hero/materials/body.nmat"});

    ASSERT_EQ(renderer.GetMaterialReferences().size(), before.size());
    EXPECT_EQ(renderer.GetMaterialReferences()[0].GetInstanceID(), before[0].GetInstanceID());
    ASSERT_EQ(renderer.GetMaterialPaths().size(), 1u);
    EXPECT_EQ(renderer.GetMaterialPaths()[0], "Library/Artifacts/Hero/materials/body.nmat");
}

#endif

TEST(SceneObjectGraphSerializationTests, MeshRendererPathHintsPreserveEquivalentResolvedMaterial)
{
    using namespace NLS::Engine::Components;
    using namespace NLS::Engine::Serialize;

    PersistentManager::Instance().Clear();

    const ScopedTempDirectory root(
        std::filesystem::temp_directory_path() /
        ("nullus_material_hint_alias_" + NLS::Guid::New().ToString()));
    const auto projectAssetsRoot = (root.Path() / "Project" / "Assets").string() + "/";
    const auto engineAssetsRoot = (root.Path() / "EngineAssets").string() + "/";
    NLS::Core::ResourceManagement::MaterialManager::ProvideAssetPaths(projectAssetsRoot, engineAssetsRoot);

    const auto libraryPath = std::string("Library/Artifacts/Hero/materials/body.nmat");
    const auto absolutePath = NLS::Core::ResourceManagement::MaterialManager::ResolveResourcePath(libraryPath);

    NLS::Render::Resources::Material material;
    material.path = absolutePath;

    MeshRenderer renderer;
    renderer.SetMaterialAtIndex(0u, material);
    ASSERT_EQ(renderer.GetMaterialAtIndex(0u), &material);

    renderer.SetMaterialPathHints({ libraryPath });

    EXPECT_EQ(renderer.GetMaterialAtIndex(0u), &material)
        << "Renderer resolution must not clear an already bound material just because the same artifact is named by a Library/absolute path alias.";
    ASSERT_EQ(renderer.GetMaterialPaths().size(), 1u);
    EXPECT_EQ(renderer.GetMaterialPaths()[0], libraryPath);
}

TEST(SceneObjectGraphSerializationTests, MeshRendererResolvesEquivalentCachedMaterialPathHint)
{
    using namespace NLS::Engine::Components;

    NLS::Engine::Serialize::PersistentManager::Instance().Clear();

    const ScopedTempDirectory root(
        std::filesystem::temp_directory_path() /
        ("nullus_material_resolve_alias_" + NLS::Guid::New().ToString()));
    const auto projectAssetsRoot = (root.Path() / "Project" / "Assets").string() + "/";
    const auto engineAssetsRoot = (root.Path() / "EngineAssets").string() + "/";
    NLS::Core::ResourceManagement::MaterialManager::ProvideAssetPaths(projectAssetsRoot, engineAssetsRoot);

    NLS::Core::ResourceManagement::MaterialManager materialManager;
    const ScopedServiceOverride<NLS::Core::ResourceManagement::MaterialManager> materialManagerScope(materialManager);

    const auto libraryPath = std::string("Library/Artifacts/Hero/materials/body.nmat");
    const auto absolutePath = NLS::Core::ResourceManagement::MaterialManager::ResolveResourcePath(libraryPath);

    auto* material = new NLS::Render::Resources::Material();
    material->path = absolutePath;
    materialManager.RegisterResource(absolutePath, material);

    MeshRenderer renderer;
    renderer.SetMaterialPathHints({ libraryPath });

    EXPECT_EQ(renderer.ResolveMaterialAtIndex(0u), material)
        << "A formal or preview prefab storing a Library material path must reuse the equivalent absolute-path cached material instead of staying invisible.";
    EXPECT_EQ(renderer.GetMaterialAtIndex(0u), material);

    materialManager.UnloadResources();
    NLS::Core::ResourceManagement::MaterialManager::ProvideAssetPaths({}, {});
}

TEST(SceneObjectGraphSerializationTests, MeshRendererFillWithMaterialSerializesSingleFallbackMaterialPath)
{
    NLS::Engine::Components::MeshRenderer renderer;
    NLS::Render::Resources::Material material;
    material.path = ":Materials\\Default.mat";

    renderer.FillWithMaterial(material);

    const auto paths = renderer.GetMaterialPaths();
    ASSERT_EQ(paths.size(), 1u);
    EXPECT_EQ(paths[0], ":Materials\\Default.mat");
}

TEST(SceneObjectGraphSerializationTests, MeshRendererFallbackDoesNotOverwriteDeferredMaterialPathHints)
{
    NLS::Engine::Components::MeshRenderer renderer;
    renderer.SetMaterialPathHints({"Library/Artifacts/Hero/materials/body.nmat"});

    NLS::Render::Resources::Material fallback;
    fallback.path = ":Materials\\Default.mat";
    renderer.FillEmptySlotsWithMaterial(fallback);

    const auto paths = renderer.GetMaterialPaths();
    ASSERT_EQ(paths.size(), 1u);
    EXPECT_EQ(paths[0], "Library/Artifacts/Hero/materials/body.nmat");
    EXPECT_EQ(renderer.GetMaterialAtIndex(0), &fallback);
}

TEST(SceneObjectGraphSerializationTests, SceneManagerTracksDirtyStateAroundSceneSaveAndLoad)
{
    NLS::Engine::SceneSystem::SceneManager sceneManager;

    EXPECT_FALSE(sceneManager.HasUnsavedSceneChanges());

    sceneManager.MarkCurrentSceneDirty();
    EXPECT_TRUE(sceneManager.HasUnsavedSceneChanges());

    sceneManager.LoadEmptyScene();
    EXPECT_FALSE(sceneManager.HasUnsavedSceneChanges());
}

TEST(SceneObjectGraphSerializationTests, LoadEmptyLightedSceneDoesNotCreateValidationCube)
{
    const ScopedSceneObjectGraphTestDriver driverContext;

    NLS::Core::ResourceManagement::MeshManager meshManager;
    NLS::Core::ResourceManagement::MaterialManager materialManager;
    NLS::Core::ResourceManagement::ShaderManager shaderManager;
    const ScopedServiceOverride<NLS::Core::ResourceManagement::MeshManager> meshManagerScope(meshManager);
    const ScopedServiceOverride<NLS::Core::ResourceManagement::MaterialManager> materialManagerScope(materialManager);
    const ScopedServiceOverride<NLS::Core::ResourceManagement::ShaderManager> shaderManagerScope(shaderManager);

    NLS::Engine::SceneSystem::SceneManager sceneManager;
    sceneManager.LoadEmptyLightedScene();

    auto* scene = sceneManager.GetCurrentScene();
    ASSERT_NE(scene, nullptr);
    EXPECT_EQ(scene->FindGameObjectByName("Validation Cube"), nullptr);

    materialManager.UnloadResources();
    meshManager.UnloadResources();
}

TEST(SceneObjectGraphSerializationTests, SceneManagerSavesAndLoadsScenesThroughObjectGraphFiles)
{
    using namespace NLS::Engine::Components;

    const auto scenePath =
        std::filesystem::temp_directory_path() /
        ("nullus_scene_manager_object_graph_" + NLS::Guid::New().ToString() + ".scene");

    NLS::Engine::SceneSystem::SceneManager sourceManager;
    auto& actor = sourceManager.GetCurrentScene()->CreateGameObject("Persisted Light", "Gameplay");
    auto* light = actor.AddComponent<LightComponent>();
    ASSERT_NE(light, nullptr);
    light->SetIntensity(6.25f);

    sourceManager.MarkCurrentSceneDirty();
    ASSERT_TRUE(sourceManager.SaveCurrentScene(scenePath.string()));
    EXPECT_FALSE(sourceManager.HasUnsavedSceneChanges());
    EXPECT_EQ(sourceManager.GetCurrentSceneSourcePath(), scenePath.string());

    const auto savedText = ReadTextFile(scenePath);
    EXPECT_NE(savedText.find("\"format\": \"Nullus.ObjectGraph.Scene\""), std::string::npos);
    EXPECT_EQ(savedText.find("SerializedSceneData"), std::string::npos);
    EXPECT_EQ(savedText.find("worldID"), std::string::npos);
    ASSERT_TRUE(NLS::Engine::Serialize::ObjectGraphReader::Read(savedText).has_value());

    NLS::Engine::SceneSystem::SceneManager loadedManager;
    ASSERT_TRUE(loadedManager.LoadScene(scenePath.string(), true));
    EXPECT_FALSE(loadedManager.HasUnsavedSceneChanges());
    EXPECT_EQ(loadedManager.GetCurrentSceneSourcePath(), scenePath.string());

    auto* loadedActor = loadedManager.GetCurrentScene()->FindGameObjectByName("Persisted Light");
    ASSERT_NE(loadedActor, nullptr);
    EXPECT_EQ(loadedActor->GetTag(), "Gameplay");

    auto* loadedLight = loadedActor->GetComponent<LightComponent>();
    ASSERT_NE(loadedLight, nullptr);
    EXPECT_FLOAT_EQ(loadedLight->GetIntensity(), 6.25f);

    std::filesystem::remove(scenePath);
}

TEST(SceneObjectGraphSerializationTests, SceneManagerRoundTripsGeneratedModelPrefabInstanceHierarchy)
{
    using namespace NLS::Engine::Serialize;

    const auto scenePath =
        std::filesystem::temp_directory_path() /
        ("nullus_generated_model_prefab_scene_" + NLS::Guid::New().ToString() + ".scene");

    NLS::Engine::Assets::PrefabArtifact artifact;
    artifact.assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("f1010101-0101-4101-8101-010101010101"));
    artifact.generatedModelPrefab = true;
    artifact.graph.format = "Nullus.ObjectGraph.Prefab";
    artifact.graph.version = 1;
    artifact.graph.documentId = NLS::Guid::NewDeterministic("GeneratedModel.SceneRoundTrip.Document");

    const auto rootId = ObjectId(NLS::Guid::NewDeterministic("GeneratedModel.SceneRoundTrip.Root"));
    const auto childId = ObjectId(NLS::Guid::NewDeterministic("GeneratedModel.SceneRoundTrip.Child"));
    artifact.graph.root = rootId;

    ObjectRecord root;
    root.id = rootId;
    root.localIdentifierInFile = MakeLocalIdentifierInFile(rootId);
    root.typeName = "NLS::Engine::GameObject";
    root.debugName = "ImportedModelRoot";
    root.debugPath = "/ImportedModelRoot";
    root.properties.push_back({"name", PropertyValue::String("ImportedModelRoot")});
    root.properties.push_back({"tag", PropertyValue::String("Model")});
    root.properties.push_back({"components", PropertyValue::Array({})});
    root.properties.push_back({"children", PropertyValue::Array({PropertyValue::OwnedReference(childId)})});
    root.properties.push_back({"parent", PropertyValue::Null()});

    ObjectRecord child;
    child.id = childId;
    child.localIdentifierInFile = MakeLocalIdentifierInFile(childId);
    child.typeName = "NLS::Engine::GameObject";
    child.debugName = "ImportedModelMeshNode";
    child.debugPath = "/ImportedModelRoot/ImportedModelMeshNode";
    child.properties.push_back({"name", PropertyValue::String("ImportedModelMeshNode")});
    child.properties.push_back({"tag", PropertyValue::String("Model")});
    child.properties.push_back({"components", PropertyValue::Array({})});
    child.properties.push_back({"children", PropertyValue::Array({})});
    child.properties.push_back({
        "parent",
        PropertyValue::ObjectReference(
            ObjectIdentifier::LocalObject(MakeLocalIdentifierInFile(rootId)))
    });

    artifact.graph.objects.push_back(std::move(root));
    artifact.graph.objects.push_back(std::move(child));

    NLS::Engine::SceneSystem::Scene sourceScene;
    auto instantiate = NLS::Editor::Assets::PrefabEditorWorkflow().InstantiatePrefab({
        &artifact,
        artifact.assetId,
        "prefab:ImportedModelRoot"
    }, sourceScene);
    ASSERT_EQ(instantiate.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);
    ASSERT_TRUE(instantiate.instance.has_value());
    ASSERT_NE(instantiate.instance->instanceRoot, nullptr);
    ASSERT_EQ(instantiate.instance->instanceRoot->GetChildren().size(), 1u);

    ASSERT_TRUE(NLS::Engine::SceneSystem::SceneManager::SaveSceneToPath(sourceScene, scenePath.string()));

    NLS::Engine::SceneSystem::SceneManager loadedManager;
    ASSERT_TRUE(loadedManager.LoadScene(scenePath.string(), true));

    auto* loadedRoot = loadedManager.GetCurrentScene()->FindGameObjectByName("ImportedModelRoot");
    ASSERT_NE(loadedRoot, nullptr);
    EXPECT_EQ(loadedRoot->GetChildren().size(), 1u);

    auto* loadedChild = loadedManager.GetCurrentScene()->FindGameObjectByName("ImportedModelMeshNode");
    ASSERT_NE(loadedChild, nullptr);
    EXPECT_EQ(loadedChild->GetParent(), loadedRoot);

    std::filesystem::remove(scenePath);
}

TEST(SceneObjectGraphSerializationTests, SceneDocumentCanPersistGeneratedModelPrefabRootReferenceMetadata)
{
    using namespace NLS::Engine::Serialize;

    NLS::Engine::Assets::PrefabArtifact artifact;
    artifact.assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("f5010101-0101-4101-8101-010101010101"));
    artifact.generatedModelPrefab = true;
    artifact.graph.format = "Nullus.ObjectGraph.Prefab";
    artifact.graph.version = 1;
    artifact.graph.documentId = NLS::Guid::NewDeterministic("GeneratedModel.ScenePrefabMetadata.Document");

    const auto rootId = ObjectId(NLS::Guid::NewDeterministic("GeneratedModel.ScenePrefabMetadata.Root"));
    const auto childId = ObjectId(NLS::Guid::NewDeterministic("GeneratedModel.ScenePrefabMetadata.Child"));
    artifact.graph.root = rootId;

    ObjectRecord root;
    root.id = rootId;
    root.localIdentifierInFile = MakeLocalIdentifierInFile(rootId);
    root.typeName = "NLS::Engine::GameObject";
    root.debugName = "GeneratedScenePrefabRoot";
    root.debugPath = "/GeneratedScenePrefabRoot";
    root.properties.push_back({"name", PropertyValue::String("GeneratedScenePrefabRoot")});
    root.properties.push_back({"tag", PropertyValue::String("Model")});
    root.properties.push_back({"components", PropertyValue::Array({})});
    root.properties.push_back({"children", PropertyValue::Array({PropertyValue::OwnedReference(childId)})});
    root.properties.push_back({"parent", PropertyValue::Null()});

    ObjectRecord child;
    child.id = childId;
    child.localIdentifierInFile = MakeLocalIdentifierInFile(childId);
    child.typeName = "NLS::Engine::GameObject";
    child.debugName = "GeneratedScenePrefabMeshNode";
    child.debugPath = "/GeneratedScenePrefabRoot/GeneratedScenePrefabMeshNode";
    child.properties.push_back({"name", PropertyValue::String("GeneratedScenePrefabMeshNode")});
    child.properties.push_back({"tag", PropertyValue::String("Model")});
    child.properties.push_back({"components", PropertyValue::Array({})});
    child.properties.push_back({"children", PropertyValue::Array({})});
    child.properties.push_back({
        "parent",
        PropertyValue::ObjectReference(
            ObjectIdentifier::LocalObject(MakeLocalIdentifierInFile(rootId)))
    });

    artifact.graph.objects.push_back(std::move(root));
    artifact.graph.objects.push_back(std::move(child));

    NLS::Engine::SceneSystem::Scene sourceScene;
    auto instantiate = NLS::Editor::Assets::PrefabEditorWorkflow().InstantiatePrefab({
        &artifact,
        artifact.assetId,
        "prefab:GeneratedScenePrefabRoot"
    }, sourceScene);
    ASSERT_EQ(instantiate.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);
    ASSERT_TRUE(instantiate.instance.has_value());

    auto document = ObjectGraphSerializer::SerializeScene(sourceScene);
    const auto* sceneRecord = FindObjectRecord(document, document.root);
    ASSERT_NE(sceneRecord, nullptr);
    auto* rootRecord = const_cast<ObjectRecord*>(FindObjectRecord(document, sceneRecord->properties.front().value.GetArray().front().GetObjectId()));
    ASSERT_NE(rootRecord, nullptr);
    rootRecord->properties.push_back({
        "scenePrefab",
        PropertyValue::ObjectReference(
            ObjectIdentifier::Asset(
                AssetId(artifact.assetId.GetGuid()),
                MakeLocalIdentifierInFile(artifact.assetId.GetGuid(), "prefab:GeneratedScenePrefabRoot"),
                "prefab:GeneratedScenePrefabRoot"))
    });

    const auto output = ObjectGraphWriter::Write(document);
    EXPECT_NE(output.find("\"scenePrefab\""), std::string::npos);
    EXPECT_NE(output.find("\"filePath\": \"prefab:GeneratedScenePrefabRoot\""), std::string::npos);
}

TEST(SceneObjectGraphSerializationTests, SceneManagerLoadPreservesMalformedDeferredMeshReference)
{
    using namespace NLS::Engine::Serialize;

    const auto scenePath =
        std::filesystem::temp_directory_path() /
        ("nullus_scene_manager_deferred_mesh_" + NLS::Guid::New().ToString() + ".scene");

    PersistentManager::Instance().Clear();

    auto document = MakeSimpleSceneDocument();
    auto& gameObject = document.objects.front();
    const auto meshFilterId = ObjectId(NLS::Guid::Parse("eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee"));
    auto* components = FindMutableProperty(gameObject, "components");
    ASSERT_NE(components, nullptr);
    components->value = PropertyValue::Array({
        PropertyValue::OwnedReference(meshFilterId),
        components->value.GetArray().front()
    });

    ObjectRecord meshFilter;
    meshFilter.id = meshFilterId;
    meshFilter.localIdentifierInFile = MakeLocalIdentifierInFile(meshFilterId);
    meshFilter.typeName = "NLS::Engine::Components::MeshFilter";
    meshFilter.debugName = "MeshFilter";
    meshFilter.properties.push_back({"mesh", PropertyValue::String("legacy malformed mesh reference")});
    document.objects.insert(document.objects.begin() + 1, std::move(meshFilter));

    {
        std::ofstream output(scenePath);
        output << ObjectGraphWriter::Write(document);
    }

    NLS::Engine::SceneSystem::SceneManager manager;
    ASSERT_TRUE(manager.LoadScene(scenePath.string(), true));

    auto* loadedObject = manager.GetCurrentScene()->FindGameObjectByName("Player");
    ASSERT_NE(loadedObject, nullptr);
    auto* loadedMeshFilter = loadedObject->GetComponent<NLS::Engine::Components::MeshFilter>();
    ASSERT_NE(loadedMeshFilter, nullptr);
    EXPECT_TRUE(loadedMeshFilter->GetMeshReference().IsNull());

    std::filesystem::remove(scenePath);
}

TEST(SceneObjectGraphSerializationTests, SceneManagerSharedSavePathWritesStandaloneScenesWithoutMutatingCurrentScene)
{
    using namespace NLS::Engine::Components;

    const auto scenePath =
        std::filesystem::temp_directory_path() /
        ("nullus_scene_manager_shared_save_" + NLS::Guid::New().ToString() + ".scene");

    NLS::Engine::SceneSystem::SceneManager sceneManager;
    sceneManager.GetCurrentScene()->CreateGameObject("Current Scene Object", "Editor");
    sceneManager.MarkCurrentSceneDirty();

    NLS::Engine::SceneSystem::Scene newScene;
    newScene.CreateGameObject("Standalone Scene Object", "Asset").AddComponent<LightComponent>();

    ASSERT_TRUE(NLS::Engine::SceneSystem::SceneManager::SaveSceneToPath(newScene, scenePath.string()));
    EXPECT_TRUE(sceneManager.HasUnsavedSceneChanges());
    EXPECT_EQ(sceneManager.GetCurrentSceneSourcePath(), "");

    const auto savedText = ReadTextFile(scenePath);
    EXPECT_NE(savedText.find("\"format\": \"Nullus.ObjectGraph.Scene\""), std::string::npos);
    EXPECT_NE(savedText.find("Standalone Scene Object"), std::string::npos);
    EXPECT_EQ(savedText.find("Current Scene Object"), std::string::npos);

    NLS::Engine::SceneSystem::SceneManager loadedManager;
    ASSERT_TRUE(loadedManager.LoadScene(scenePath.string(), true));
    EXPECT_NE(loadedManager.GetCurrentScene()->FindGameObjectByName("Standalone Scene Object"), nullptr);
    EXPECT_EQ(loadedManager.GetCurrentScene()->FindGameObjectByName("Current Scene Object"), nullptr);

    std::filesystem::remove(scenePath);
}

TEST(SceneObjectGraphSerializationTests, SceneManagerUnloadsCurrentSceneBeforeLoadedActorsAreCreated)
{
    using namespace NLS::Engine;

    const auto scenePath =
        std::filesystem::temp_directory_path() /
        ("nullus_scene_manager_event_order_" + NLS::Guid::New().ToString() + ".scene");

    SceneSystem::Scene sourceScene;
    sourceScene.CreateGameObject("Loaded Actor", "Gameplay");
    const auto document = Serialize::ObjectGraphSerializer::SerializeScene(sourceScene);
    std::ofstream output(scenePath);
    output << Serialize::ObjectGraphWriter::Write(document);
    output.close();

    SceneSystem::SceneManager manager;
    std::vector<std::string> events;
    auto unloadListener = manager.SceneUnloadEvent += [&events]
    {
        events.push_back("unload");
    };
    auto createdListener = GameObject::CreatedEvent += [&events](GameObject& actor)
    {
        if (actor.GetName() == "Loaded Actor")
            events.push_back("created");
    };

    ASSERT_TRUE(manager.LoadScene(scenePath.string(), true));

    GameObject::CreatedEvent -= createdListener;
    manager.SceneUnloadEvent -= unloadListener;
    std::filesystem::remove(scenePath);

    ASSERT_GE(events.size(), 2u);
    EXPECT_EQ(events[0], "unload");
    EXPECT_EQ(events[1], "created");
}
