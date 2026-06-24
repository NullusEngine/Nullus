#include <gtest/gtest.h>

#include "Assets/AssetId.h"
#include "Core/Assets/ArtifactManifest.h"
#include "Engine/Assets/PrefabAsset.h"
#include "Engine/Assets/ModelPrefabBuilder.h"
#include "Engine/SceneSystem/Scene.h"
#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "Guid.h"
#include "Rendering/Assets/ImportedScene.h"
#include "Rendering/Assets/SceneImportPipeline.h"
#include "Serialize/ObjectGraphSerializer.h"
#include "Serialize/ObjectGraphDocument.h"
#include "Serialize/ObjectGraphWriter.h"
#include "Serialize/SerializationDiagnostic.h"

namespace
{
const NLS::Engine::Serialize::ObjectRecord* FindRecord(
    const NLS::Engine::Serialize::ObjectGraphDocument& document,
    const std::string& debugName,
    const std::string& typeName)
{
    for (const auto& record : document.objects)
    {
        if (record.debugName == debugName && record.typeName == typeName)
            return &record;
    }
    return nullptr;
}

const NLS::Engine::Serialize::ObjectRecord* FindRecordById(
    const NLS::Engine::Serialize::ObjectGraphDocument& document,
    const NLS::Engine::Serialize::ObjectId& id)
{
    for (const auto& record : document.objects)
    {
        if (record.id == id)
            return &record;
    }
    return nullptr;
}

std::string LibraryArtifactPath(const std::string& hash)
{
    return (std::filesystem::path("Library") / "Artifacts" /
        NLS::Core::Assets::BuildArtifactStorageRelativePath(hash)).generic_string();
}

NLS::Engine::Serialize::ObjectId MakeGeneratedModelPrefabObjectId(
    const NLS::Render::Assets::ImportedScene& scene,
    const std::string& suffix)
{
    return NLS::Engine::Serialize::ObjectId(NLS::Guid::NewDeterministic(
        "GeneratedModelPrefab:" + scene.sourceAssetId.ToString() + ":" + scene.sceneKey + ":" + suffix));
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

bool ContainsResolvedAsset(
    const std::vector<NLS::Engine::Assets::PrefabResolvedAsset>& assets,
    const std::string& expectedType,
    const std::string& subAssetKey)
{
    for (const auto& asset : assets)
    {
        if (asset.expectedType == expectedType && asset.subAssetKey == subAssetKey)
            return true;
    }
    return false;
}

const NLS::Engine::Assets::PrefabResolvedAsset* FindResolvedAsset(
    const std::vector<NLS::Engine::Assets::PrefabResolvedAsset>& assets,
    const std::string& expectedType,
    const std::string& subAssetKey)
{
    for (const auto& asset : assets)
    {
        if (asset.expectedType == expectedType && asset.subAssetKey == subAssetKey)
            return &asset;
    }
    return nullptr;
}

NLS::Engine::Serialize::PropertyValue MakeObjectReference(
    NLS::Core::Assets::AssetId assetId,
    std::string filePath)
{
    const auto localIdentifierInFile = NLS::Engine::Serialize::MakeLocalIdentifierInFile(
        assetId.GetGuid(),
        filePath);
    return NLS::Engine::Serialize::PropertyValue::ObjectReference(
        NLS::Engine::Serialize::ObjectIdentifier::Asset(
            NLS::Engine::Serialize::AssetId(assetId.GetGuid()),
            localIdentifierInFile,
            std::move(filePath)));
}
}

TEST(AssetPrefabPipelineTests, PrefabArtifactTracksBaseChainResolvedAssetsAndInstanceMap)
{
    NLS::Engine::Assets::PrefabArtifact artifact;
    artifact.assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("33333333-3333-4333-8333-333333333333"));
    artifact.graph.format = "Nullus.ObjectGraph.Prefab";
    artifact.graph.version = 1;

    const auto basePrefabId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("44444444-4444-4444-8444-444444444444"));
    const auto materialId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("55555555-5555-4555-8555-555555555555"));
    artifact.baseChain.push_back(basePrefabId);
    artifact.resolvedAssets.push_back({materialId, "Material", "material:Hero", "Library/Artifacts/Hero/47b24ab4b128645b99328e0a68370de1202b0ba370eafc30e8bb0b0b7cf8b5ae"});

    const auto sourceObject = NLS::Engine::Serialize::ObjectId(
        NLS::Guid::Parse("66666666-6666-4666-8666-666666666666"));
    const auto runtimeObject = NLS::Engine::Serialize::ObjectId(
        NLS::Guid::Parse("77777777-7777-4777-8777-777777777777"));
    artifact.sourceToRuntimeObject.emplace(sourceObject, runtimeObject);

    ASSERT_EQ(artifact.baseChain.size(), 1u);
    EXPECT_EQ(artifact.baseChain.front(), basePrefabId);
    ASSERT_EQ(artifact.resolvedAssets.size(), 1u);
    EXPECT_EQ(artifact.resolvedAssets.front().assetId, materialId);
    EXPECT_EQ(artifact.resolvedAssets.front().expectedType, "Material");
    EXPECT_EQ(artifact.resolvedAssets.front().subAssetKey, "material:Hero");
    EXPECT_EQ(artifact.resolvedAssets.front().artifactPath, "Library/Artifacts/Hero/47b24ab4b128645b99328e0a68370de1202b0ba370eafc30e8bb0b0b7cf8b5ae");

    const auto* mapped = artifact.FindRuntimeObject(sourceObject);
    ASSERT_NE(mapped, nullptr);
    EXPECT_EQ(*mapped, runtimeObject);
    EXPECT_EQ(
        artifact.FindRuntimeObject(NLS::Engine::Serialize::ObjectId(NLS::Guid::NewDeterministic("Missing"))),
        nullptr);
}

TEST(AssetPrefabPipelineTests, PrefabArtifactValidationUsesObjectGraphDiagnostics)
{
    NLS::Engine::Assets::PrefabArtifact artifact;
    artifact.graph.format = "Nullus.ObjectGraph.Prefab";
    artifact.graph.version = 1;
    artifact.graph.documentId = NLS::Guid::NewDeterministic("BrokenPrefab");
    artifact.graph.root = NLS::Engine::Serialize::ObjectId(NLS::Guid::NewDeterministic("MissingRoot"));
    artifact.graph.overrides.push_back(NLS::Engine::Serialize::PatchOperation::ReplaceProperty(
        NLS::Engine::Serialize::ObjectId(NLS::Guid::NewDeterministic("MissingOverrideTarget")),
        "name",
        NLS::Engine::Serialize::PropertyValue::String("Broken")));

    const auto diagnostics = artifact.Validate();

    ASSERT_TRUE(diagnostics.HasErrors());
    ASSERT_GE(diagnostics.GetItems().size(), 2u);

    bool foundMissingRoot = false;
    bool foundInvalidOverride = false;
    for (const auto& diagnostic : diagnostics.GetItems())
    {
        foundMissingRoot |= diagnostic.GetCode() == NLS::Engine::Serialize::SerializationDiagnosticCode::MissingObject;
        foundInvalidOverride |= diagnostic.GetCode() == NLS::Engine::Serialize::SerializationDiagnosticCode::InvalidPrefabOverride;
    }

    EXPECT_TRUE(foundMissingRoot);
    EXPECT_TRUE(foundInvalidOverride);
}

TEST(AssetPrefabPipelineTests, FallbackResolvedAssetDoesNotInferArtifactTypeFromLibraryPathExtension)
{
    NLS::Engine::Serialize::ObjectGraphDocument graph;
    graph.format = "Nullus.ObjectGraph.Prefab";
    graph.version = 1;

    NLS::Engine::Serialize::ObjectRecord record;
    record.id = NLS::Engine::Serialize::ObjectId(NLS::Guid::NewDeterministic("extensionless-artifact-type-owner"));
    record.typeName = "TestComponent";
    record.debugName = "Test";

    const auto shaderId = NLS::Core::Assets::AssetId(NLS::Guid::NewDeterministic("extensionless-artifact-type-shader"));
    const auto artifactPath = LibraryArtifactPath("47b24ab4b128645b99328e0a68370de1202b0ba370eafc30e8bb0b0b7cf8b5ae");
    record.properties.push_back({
        "asset",
        NLS::Engine::Serialize::PropertyValue::ObjectReference(
            NLS::Engine::Serialize::ObjectIdentifier::Asset(
                NLS::Engine::Serialize::AssetId(shaderId.GetGuid()),
                4800000,
                artifactPath))});
    graph.objects.push_back(std::move(record));

    const auto resolvedAssets = NLS::Engine::Assets::BuildPrefabResolvedAssetsFromReferences(graph);
    ASSERT_EQ(resolvedAssets.size(), 1u);
    EXPECT_EQ(resolvedAssets.front().assetId, shaderId);
    EXPECT_EQ(resolvedAssets.front().expectedType, "Asset");
    EXPECT_EQ(resolvedAssets.front().subAssetKey, artifactPath);
    EXPECT_EQ(resolvedAssets.front().artifactPath, artifactPath);
}

TEST(AssetPrefabPipelineTests, FallbackResolvedAssetKeepsSubAssetHintOutOfArtifactPath)
{
    NLS::Engine::Serialize::ObjectGraphDocument graph;
    graph.format = "Nullus.ObjectGraph.Prefab";
    graph.version = 1;

    NLS::Engine::Serialize::ObjectRecord renderer;
    renderer.id = NLS::Engine::Serialize::ObjectId(NLS::Guid::NewDeterministic("subasset-hint-owner"));
    renderer.typeName = "NLS::Engine::Components::MeshRenderer";
    renderer.debugName = "Renderer";

    const auto materialId = NLS::Core::Assets::AssetId(NLS::Guid::NewDeterministic("subasset-hint-material"));
    renderer.properties.push_back({
        "materials",
        NLS::Engine::Serialize::PropertyValue::Array({
            NLS::Engine::Serialize::PropertyValue::ObjectReference(
                NLS::Engine::Serialize::ObjectIdentifier::Asset(
                    NLS::Engine::Serialize::AssetId(materialId.GetGuid()),
                    4800000,
                    "material:Body"))
        })});
    graph.objects.push_back(std::move(renderer));

    const auto resolvedAssets = NLS::Engine::Assets::BuildPrefabResolvedAssetsFromReferences(graph);
    ASSERT_EQ(resolvedAssets.size(), 1u);
    EXPECT_EQ(resolvedAssets.front().assetId, materialId);
    EXPECT_EQ(resolvedAssets.front().expectedType, "Material");
    EXPECT_EQ(resolvedAssets.front().subAssetKey, "material:Body");
    EXPECT_TRUE(resolvedAssets.front().artifactPath.empty());
}

TEST(AssetPrefabPipelineTests, FallbackResolvedAssetPreservesContentStorageArtifactPath)
{
    NLS::Engine::Serialize::ObjectGraphDocument graph;
    graph.format = "Nullus.ObjectGraph.Prefab";
    graph.version = 1;

    NLS::Engine::Serialize::ObjectRecord filter;
    filter.id = NLS::Engine::Serialize::ObjectId(NLS::Guid::NewDeterministic("content-artifact-owner"));
    filter.typeName = "NLS::Engine::Components::MeshFilter";
    filter.debugName = "MeshFilter";

    const auto meshId = NLS::Core::Assets::AssetId(NLS::Guid::NewDeterministic("content-artifact-mesh"));
    const std::string artifactPath =
        LibraryArtifactPath("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    filter.properties.push_back({
        "mesh",
        NLS::Engine::Serialize::PropertyValue::ObjectReference(
            NLS::Engine::Serialize::ObjectIdentifier::Asset(
                NLS::Engine::Serialize::AssetId(meshId.GetGuid()),
                4800000,
                artifactPath))});
    graph.objects.push_back(std::move(filter));

    const auto resolvedAssets = NLS::Engine::Assets::BuildPrefabResolvedAssetsFromReferences(graph);
    ASSERT_EQ(resolvedAssets.size(), 1u);
    EXPECT_EQ(resolvedAssets.front().assetId, meshId);
    EXPECT_EQ(resolvedAssets.front().expectedType, "Mesh");
    EXPECT_EQ(resolvedAssets.front().subAssetKey, artifactPath);
    EXPECT_EQ(resolvedAssets.front().artifactPath, artifactPath);
}

TEST(AssetPrefabPipelineTests, FallbackResolvedAssetRejectsEscapingContentStorageArtifactPath)
{
    NLS::Engine::Serialize::ObjectGraphDocument graph;
    graph.format = "Nullus.ObjectGraph.Prefab";
    graph.version = 1;

    const auto meshId = NLS::Core::Assets::AssetId(NLS::Guid::NewDeterministic("escaping-content-artifact-mesh"));
    const std::string hashName = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    for (const auto& path : {
        std::string("../") + LibraryArtifactPath(hashName),
        (std::filesystem::temp_directory_path() / hashName).string()
    })
    {
        NLS::Engine::Serialize::ObjectGraphDocument localGraph = graph;
        NLS::Engine::Serialize::ObjectRecord filter;
        filter.id = NLS::Engine::Serialize::ObjectId(NLS::Guid::NewDeterministic("escaping-content-artifact-owner"));
        filter.typeName = "NLS::Engine::Components::MeshFilter";
        filter.debugName = "MeshFilter";
        filter.properties.push_back({
            "mesh",
            NLS::Engine::Serialize::PropertyValue::ObjectReference(
                NLS::Engine::Serialize::ObjectIdentifier::Asset(
                    NLS::Engine::Serialize::AssetId(meshId.GetGuid()),
                    4800000,
                    path))});
        localGraph.objects.push_back(std::move(filter));

        const auto resolvedAssets = NLS::Engine::Assets::BuildPrefabResolvedAssetsFromReferences(localGraph);
        ASSERT_EQ(resolvedAssets.size(), 1u);
        EXPECT_EQ(resolvedAssets.front().assetId, meshId);
        EXPECT_EQ(resolvedAssets.front().expectedType, "Mesh");
        EXPECT_EQ(resolvedAssets.front().subAssetKey, path);
        EXPECT_TRUE(resolvedAssets.front().artifactPath.empty()) << path;
    }
}

TEST(AssetPrefabPipelineTests, ImportsPrefabSourceDocumentAndCapturesBaseDependency)
{
    NLS::Engine::Serialize::ObjectGraphDocument document;
    document.format = "Nullus.ObjectGraph.Prefab";
    document.version = 1;
    document.documentId = NLS::Guid::Parse("88888888-8888-4888-8888-888888888888");
    document.root = NLS::Engine::Serialize::ObjectId(
        NLS::Guid::Parse("99999999-9999-4999-8999-999999999999"));
    document.basePrefab = NLS::Engine::Serialize::ObjectIdentifier::Asset(
        NLS::Engine::Serialize::AssetId(NLS::Guid::Parse("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa")),
        NLS::Engine::Serialize::MakeLocalIdentifierInFile(
            NLS::Guid::Parse("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"),
            "Assets/Prefabs/Base.prefab"),
        "Assets/Prefabs/Base.prefab");

    NLS::Engine::Serialize::ObjectRecord root;
    root.id = document.root;
    root.localIdentifierInFile = NLS::Engine::Serialize::MakeLocalIdentifierInFile(document.root);
    root.typeName = "NLS::Engine::GameObject";
    root.debugName = "ImportedPrefab";
    document.objects.push_back(root);

    const auto text = NLS::Engine::Serialize::ObjectGraphWriter::Write(document);
    const auto assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"));

    const auto result = NLS::Engine::Assets::ImportPrefabArtifact(text, assetId);

    ASSERT_FALSE(result.diagnostics.HasErrors());
    EXPECT_EQ(result.artifact.assetId, assetId);
    EXPECT_EQ(result.artifact.graph.format, "Nullus.ObjectGraph.Prefab");
    ASSERT_EQ(result.artifact.baseChain.size(), 1u);
    EXPECT_EQ(result.artifact.baseChain.front().GetGuid().ToString(), "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
}

TEST(AssetPrefabPipelineTests, ImportPrefabSourceReportsObjectGraphDiagnostics)
{
    NLS::Engine::Serialize::ObjectGraphDocument document;
    document.format = "Nullus.ObjectGraph.Prefab";
    document.version = 1;
    document.documentId = NLS::Guid::Parse("cccccccc-cccc-4ccc-8ccc-cccccccccccc");
    document.root = NLS::Engine::Serialize::ObjectId(
        NLS::Guid::Parse("dddddddd-dddd-4ddd-8ddd-dddddddddddd"));
    document.overrides.push_back(NLS::Engine::Serialize::PatchOperation::ReplaceProperty(
        NLS::Engine::Serialize::ObjectId(NLS::Guid::Parse("eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee")),
        "name",
        NLS::Engine::Serialize::PropertyValue::String("Broken")));

    const auto text = NLS::Engine::Serialize::ObjectGraphWriter::Write(document);
    const auto result = NLS::Engine::Assets::ImportPrefabArtifact(
        text,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("ffffffff-ffff-4fff-8fff-ffffffffffff")));

    ASSERT_TRUE(result.diagnostics.HasErrors());
    bool foundInvalidOverride = false;
    bool foundMissingRoot = false;
    for (const auto& diagnostic : result.diagnostics.GetItems())
    {
        foundInvalidOverride |= diagnostic.GetCode() == NLS::Engine::Serialize::SerializationDiagnosticCode::InvalidPrefabOverride;
        foundMissingRoot |= diagnostic.GetCode() == NLS::Engine::Serialize::SerializationDiagnosticCode::MissingObject;
    }
    EXPECT_TRUE(foundInvalidOverride);
    EXPECT_TRUE(foundMissingRoot);
}

TEST(AssetPrefabPipelineTests, SerializesGameObjectSelectionAsPrefabSource)
{
    NLS::Engine::GameObject root("WorkshopLamp", "Prop");
    root.AddComponent<NLS::Engine::Components::LightComponent>()->SetIntensity(3.0f);
    NLS::Engine::GameObject child("Bulb", "Part");
    child.SetParent(root);

    const auto prefab = NLS::Engine::Serialize::ObjectGraphSerializer::SerializePrefab(root);
    child.DetachFromParent();
    const auto text = NLS::Engine::Serialize::ObjectGraphWriter::Write(prefab.graph);
    const auto assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("10101010-1010-4010-8010-101010101010"));

    const auto result = NLS::Engine::Assets::ImportPrefabArtifact(text, assetId);

    ASSERT_FALSE(result.diagnostics.HasErrors());
    EXPECT_EQ(result.artifact.assetId, assetId);
    EXPECT_EQ(result.artifact.graph.format, "Nullus.ObjectGraph.Prefab");
    ASSERT_EQ(result.artifact.graph.objects.size(), 5u);

    const auto* importedRoot = FindRecord(
        result.artifact.graph,
        "WorkshopLamp",
        "NLS::Engine::GameObject");
    ASSERT_NE(importedRoot, nullptr);
    const auto* children = FindProperty(*importedRoot, "children");
    ASSERT_NE(children, nullptr);
    ASSERT_EQ(children->value.GetKind(), NLS::Engine::Serialize::PropertyValue::Kind::Array);
    ASSERT_EQ(children->value.GetArray().size(), 1u);
}

TEST(AssetPrefabPipelineTests, ImportPrefabSourceRebuildsResolvedAssetsFromExternalReferences)
{
    using namespace NLS::Engine::Serialize;

    const auto prefabId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("11111111-2222-4333-8444-555555555555"));
    const auto meshId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("12121212-1212-4212-8212-121212121212"));
    const std::string meshArtifactPath = "mesh:mesh/0";

    ObjectGraphDocument document;
    document.format = "Nullus.ObjectGraph.Prefab";
    document.version = 1;
    document.documentId = NLS::Guid::NewDeterministic("ImportResolvedAssets.Document");
    document.root = ObjectId(NLS::Guid::NewDeterministic("ImportResolvedAssets.Root"));
    document.objects.push_back({
        document.root,
        "NLS::Engine::GameObject",
        "Renderable",
        "",
        ObjectRecordState::Alive,
        {
            {"name", PropertyValue::String("Renderable")},
            {"tag", PropertyValue::String("Prefab")},
            {"components", PropertyValue::Array({})},
            {"children", PropertyValue::Array({})},
            {"parent", PropertyValue::Null()},
            {"mesh", MakeObjectReference(meshId, meshArtifactPath)}
        },
        MakeLocalIdentifierInFile(document.root)
    });

    const auto result = NLS::Engine::Assets::ImportPrefabArtifact(
        ObjectGraphWriter::Write(document),
        prefabId);

    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_EQ(result.artifact.resolvedAssets.size(), 1u);
    EXPECT_EQ(result.artifact.resolvedAssets.front().assetId, meshId);
    EXPECT_EQ(result.artifact.resolvedAssets.front().expectedType, "Mesh");
    EXPECT_EQ(result.artifact.resolvedAssets.front().subAssetKey, meshArtifactPath);
    EXPECT_TRUE(result.artifact.resolvedAssets.front().artifactPath.empty());
}

TEST(AssetPrefabPipelineTests, InstantiatesPrefabArtifactWithStableSourceToInstanceMap)
{
    NLS::Engine::GameObject root("Crate", "Prop");
    root.AddComponent<NLS::Engine::Components::LightComponent>()->SetIntensity(2.0f);
    auto artifact = NLS::Engine::Assets::ImportPrefabArtifact(
        NLS::Engine::Serialize::ObjectGraphWriter::Write(
            NLS::Engine::Serialize::ObjectGraphSerializer::SerializePrefab(root).graph),
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("20202020-2020-4020-8020-202020202020"))).artifact;

    NLS::Engine::SceneSystem::Scene scene;
    const auto instance = NLS::Engine::Assets::InstantiatePrefabArtifact(artifact, scene);

    ASSERT_FALSE(instance.diagnostics.HasErrors());
    ASSERT_NE(instance.root, nullptr);
    EXPECT_EQ(instance.root->GetName(), "Crate");
    ASSERT_EQ(scene.GetGameObjects().size(), 1u);
    EXPECT_EQ(scene.GetGameObjects()[0], instance.root);
    EXPECT_GE(instance.sourceToInstance.size(), 2u);
    EXPECT_EQ(artifact.sourceToRuntimeObject.size(), instance.sourceToInstance.size());
    for (const auto& mapping : instance.sourceToInstance)
    {
        EXPECT_TRUE(mapping.first.IsValid());
        EXPECT_TRUE(mapping.second.IsValid());
        EXPECT_NE(mapping.first, mapping.second);
        const auto* runtimeObject = artifact.FindRuntimeObject(mapping.first);
        ASSERT_NE(runtimeObject, nullptr);
        EXPECT_EQ(*runtimeObject, mapping.second);
    }
}

TEST(AssetPrefabPipelineTests, AmbiguousEmptyAssetReferenceHintDoesNotResolveToFirstSubAsset)
{
    using namespace NLS::Engine::Serialize;

    const auto assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("21212121-2121-4121-8121-212121212121"));

    ObjectGraphDocument document;
    document.format = "Nullus.ObjectGraph.Prefab";
    document.version = 1;
    document.documentId = NLS::Guid::NewDeterministic("AmbiguousAssetHint.Prefab");
    document.root = ObjectId(NLS::Guid::NewDeterministic("AmbiguousAssetHint.Root"));

    ObjectRecord root;
    root.id = document.root;
    root.localIdentifierInFile = MakeLocalIdentifierInFile(document.root);
    root.typeName = "NLS::Engine::GameObject";
    root.debugName = "AmbiguousRoot";
    root.properties.push_back({"name", PropertyValue::String("AmbiguousRoot")});
    root.properties.push_back({"tag", PropertyValue::String("Prefab")});
    root.properties.push_back({"active", PropertyValue::Bool(true)});

    const auto meshFilterId = ObjectId(NLS::Guid::NewDeterministic("AmbiguousAssetHint.MeshFilter"));
    root.properties.push_back({"components", PropertyValue::Array({PropertyValue::OwnedReference(meshFilterId)})});
    document.objects.push_back(root);

    ObjectRecord meshFilter;
    meshFilter.id = meshFilterId;
    meshFilter.localIdentifierInFile = MakeLocalIdentifierInFile(meshFilterId);
    meshFilter.typeName = "NLS::Engine::Components::MeshFilter";
    meshFilter.debugName = "AmbiguousMeshFilter";
    meshFilter.properties.push_back({"mesh", MakeObjectReference(assetId, {})});
    document.objects.push_back(meshFilter);

    NLS::Engine::Assets::PrefabArtifact artifact;
    artifact.assetId = assetId;
    artifact.graph = document;
    artifact.resolvedAssets.push_back({assetId, "Mesh", "mesh:mesh/0", "Library/Artifacts/Hero/ea4e3c8fc80d89dfbc263a6c298c1d0efa764f39a42fcca4ea90847841365f92"});
    artifact.resolvedAssets.push_back({assetId, "Mesh", "mesh:mesh/1", "Library/Artifacts/Hero/bc07797835c36eb2155fbdcdff46c71eb131ff80d3eed53488f4ec7ec9c6ad60"});

    NLS::Engine::SceneSystem::Scene scene;
    const auto instance = NLS::Engine::Assets::InstantiatePrefabArtifact(artifact, scene);

    ASSERT_FALSE(instance.diagnostics.HasErrors());
    ASSERT_NE(instance.root, nullptr);
    auto* meshFilterComponent = instance.root->GetComponent<NLS::Engine::Components::MeshFilter>();
    ASSERT_NE(meshFilterComponent, nullptr);
    EXPECT_TRUE(meshFilterComponent->GetModelPath().empty());
}

TEST(AssetPrefabPipelineTests, ImportPrefabSourceKeepsAmbiguousEmptyAssetReferenceUnresolved)
{
    using namespace NLS::Engine::Serialize;

    const auto prefabId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("22222222-2121-4121-8121-212121212121"));
    const auto meshAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("23232323-2323-4323-8323-232323232323"));

    ObjectGraphDocument document;
    document.format = "Nullus.ObjectGraph.Prefab";
    document.version = 1;
    document.documentId = NLS::Guid::NewDeterministic("AmbiguousImportAssetHint.Prefab");
    document.root = ObjectId(NLS::Guid::NewDeterministic("AmbiguousImportAssetHint.Root"));

    ObjectRecord root;
    root.id = document.root;
    root.localIdentifierInFile = MakeLocalIdentifierInFile(document.root);
    root.typeName = "NLS::Engine::GameObject";
    root.debugName = "AmbiguousImportRoot";
    root.properties.push_back({"name", PropertyValue::String("AmbiguousImportRoot")});
    root.properties.push_back({"tag", PropertyValue::String("Prefab")});
    root.properties.push_back({"active", PropertyValue::Bool(true)});

    const auto meshFilterId = ObjectId(NLS::Guid::NewDeterministic("AmbiguousImportAssetHint.MeshFilter"));
    root.properties.push_back({"components", PropertyValue::Array({PropertyValue::OwnedReference(meshFilterId)})});
    document.objects.push_back(root);

    ObjectRecord meshFilter;
    meshFilter.id = meshFilterId;
    meshFilter.localIdentifierInFile = MakeLocalIdentifierInFile(meshFilterId);
    meshFilter.typeName = "NLS::Engine::Components::MeshFilter";
    meshFilter.debugName = "AmbiguousImportMeshFilter";
    meshFilter.properties.push_back({"mesh", MakeObjectReference(meshAssetId, {})});
    document.objects.push_back(meshFilter);

    std::vector<NLS::Engine::Assets::PrefabResolvedAsset> existingResolvedAssets;
    existingResolvedAssets.push_back(
        {meshAssetId, "Mesh", "mesh:mesh/0", "Library/Artifacts/Hero/ea4e3c8fc80d89dfbc263a6c298c1d0efa764f39a42fcca4ea90847841365f92"});
    existingResolvedAssets.push_back(
        {meshAssetId, "Mesh", "mesh:mesh/1", "Library/Artifacts/Hero/bc07797835c36eb2155fbdcdff46c71eb131ff80d3eed53488f4ec7ec9c6ad60"});

    auto importResult = NLS::Engine::Assets::ImportPrefabArtifact(
        ObjectGraphWriter::Write(document),
        prefabId,
        std::move(existingResolvedAssets));

    ASSERT_FALSE(importResult.diagnostics.HasErrors());
    ASSERT_EQ(importResult.artifact.resolvedAssets.size(), 1u);
    EXPECT_TRUE(importResult.artifact.resolvedAssets.front().subAssetKey.empty());
    EXPECT_TRUE(importResult.artifact.resolvedAssets.front().artifactPath.empty());

    NLS::Engine::SceneSystem::Scene scene;
    const auto instance = NLS::Engine::Assets::InstantiatePrefabArtifact(
        importResult.artifact,
        scene);

    ASSERT_FALSE(instance.diagnostics.HasErrors());
    ASSERT_NE(instance.root, nullptr);
    auto* meshFilterComponent = instance.root->GetComponent<NLS::Engine::Components::MeshFilter>();
    ASSERT_NE(meshFilterComponent, nullptr);
    EXPECT_TRUE(meshFilterComponent->GetModelPath().empty());
}

TEST(AssetPrefabPipelineTests, ValidatesPrefabBaseChainsAndMissingBases)
{
    const auto rootId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("30303030-3030-4030-8030-303030303030"));
    const auto variantId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("40404040-4040-4040-8040-404040404040"));
    const auto missingBaseId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("50505050-5050-4050-8050-505050505050"));

    NLS::Engine::Assets::PrefabArtifact root;
    root.assetId = rootId;
    root.graph.format = "Nullus.ObjectGraph.Prefab";
    root.graph.version = 1;
    root.graph.documentId = NLS::Guid::NewDeterministic("PrefabBase.Root");
    root.graph.root = NLS::Engine::Serialize::ObjectId(NLS::Guid::NewDeterministic("PrefabBase.Root.Object"));
    root.graph.objects.push_back({
        root.graph.root,
        "NLS::Engine::GameObject",
        "RootPrefab",
        "",
        NLS::Engine::Serialize::ObjectRecordState::Alive,
        {
            {"name", NLS::Engine::Serialize::PropertyValue::String("RootPrefab")},
            {"tag", NLS::Engine::Serialize::PropertyValue::String({})},
            {"components", NLS::Engine::Serialize::PropertyValue::Array({})},
            {"children", NLS::Engine::Serialize::PropertyValue::Array({})},
            {"parent", NLS::Engine::Serialize::PropertyValue::Null()}
        },
        NLS::Engine::Serialize::MakeLocalIdentifierInFile(root.graph.root)
    });

    NLS::Engine::Assets::PrefabArtifact variant = root;
    variant.assetId = variantId;
    variant.baseChain = {rootId};

    const auto validDiagnostics = NLS::Engine::Assets::ValidatePrefabBaseChains({root, variant});
    EXPECT_FALSE(validDiagnostics.HasErrors());

    variant.baseChain = {missingBaseId};
    const auto missingDiagnostics = NLS::Engine::Assets::ValidatePrefabBaseChains({root, variant});
    ASSERT_TRUE(missingDiagnostics.HasErrors());
    EXPECT_EQ(missingDiagnostics.GetItems()[0].GetCode(), NLS::Engine::Serialize::SerializationDiagnosticCode::MissingAsset);

    root.baseChain = {variantId};
    variant.baseChain = {rootId};
    const auto cycleDiagnostics = NLS::Engine::Assets::ValidatePrefabBaseChains({root, variant});
    ASSERT_TRUE(cycleDiagnostics.HasErrors());
    EXPECT_EQ(cycleDiagnostics.GetItems()[0].GetCode(), NLS::Engine::Serialize::SerializationDiagnosticCode::InvalidPrefabOverride);
}

TEST(AssetPrefabPipelineTests, ReportsUnresolvedAssetReferencesInPrefabArtifacts)
{
    NLS::Engine::Assets::PrefabArtifact artifact;
    artifact.assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("60606060-6060-4060-8060-606060606060"));
    artifact.graph.format = "Nullus.ObjectGraph.Prefab";
    artifact.graph.version = 1;
    artifact.graph.documentId = NLS::Guid::NewDeterministic("Prefab.UnresolvedAsset.Document");
    artifact.graph.root = NLS::Engine::Serialize::ObjectId(NLS::Guid::NewDeterministic("Prefab.UnresolvedAsset.Root"));
    artifact.graph.objects.push_back({
        artifact.graph.root,
        "NLS::Engine::GameObject",
        "BrokenAssetPrefab",
        "",
        NLS::Engine::Serialize::ObjectRecordState::Alive,
        {
            {"name", NLS::Engine::Serialize::PropertyValue::String("BrokenAssetPrefab")},
            {"tag", NLS::Engine::Serialize::PropertyValue::String({})},
            {"components", NLS::Engine::Serialize::PropertyValue::Array({})},
            {"children", NLS::Engine::Serialize::PropertyValue::Array({})},
            {"parent", NLS::Engine::Serialize::PropertyValue::Null()},
            {"material", NLS::Engine::Serialize::PropertyValue::ObjectReference(
                NLS::Engine::Serialize::ObjectIdentifier::Asset(
                    NLS::Engine::Serialize::AssetId(NLS::Guid::Parse("70707070-7070-4070-8070-707070707070")),
                    NLS::Engine::Serialize::MakeLocalIdentifierInFile(
                        NLS::Guid::Parse("70707070-7070-4070-8070-707070707070"),
                        "material:Missing"),
                    "material:Missing"))}
        },
        NLS::Engine::Serialize::MakeLocalIdentifierInFile(artifact.graph.root)
    });

    const auto diagnostics = artifact.Validate();

    ASSERT_TRUE(diagnostics.HasErrors());
    bool foundUnresolvedAsset = false;
    for (const auto& diagnostic : diagnostics.GetItems())
        foundUnresolvedAsset |= diagnostic.GetCode() == NLS::Engine::Serialize::SerializationDiagnosticCode::MissingAsset;
    EXPECT_TRUE(foundUnresolvedAsset);
}

TEST(AssetPrefabPipelineTests, NormalizesPrefabOverridePatchesDeterministically)
{
    using namespace NLS::Engine::Serialize;

    const auto rootId = ObjectId(NLS::Guid::Parse("71717171-7171-4171-8171-717171717171"));
    const auto lightId = ObjectId(NLS::Guid::Parse("72727272-7272-4272-8272-727272727272"));
    const auto childId = ObjectId(NLS::Guid::Parse("73737373-7373-4373-8373-737373737373"));

    std::vector<PatchOperation> patches;
    patches.push_back(PatchOperation::MoveOwned(rootId, "children", childId, 2u));
    patches.push_back(PatchOperation::ReplaceProperty(rootId, "tag", PropertyValue::String("Prop")));
    patches.push_back(PatchOperation::ReplaceProperty(rootId, "name", PropertyValue::String("Crate")));
    patches.push_back(PatchOperation::ReplaceProperty(rootId, "name", PropertyValue::String("FinalCrate")));
    patches.push_back(PatchOperation::InsertOwned(rootId, "components", lightId, 1u));

    const auto normalized = NLS::Engine::Assets::NormalizePrefabOverridePatches(patches);

    ASSERT_EQ(normalized.size(), 4u);
    EXPECT_EQ(normalized[0].type, PatchOperationType::ReplaceProperty);
    EXPECT_EQ(normalized[0].property, "name");
    EXPECT_EQ(normalized[0].value.GetString(), "FinalCrate");
    EXPECT_EQ(normalized[1].type, PatchOperationType::ReplaceProperty);
    EXPECT_EQ(normalized[1].property, "tag");
    EXPECT_EQ(normalized[2].type, PatchOperationType::InsertOwned);
    EXPECT_EQ(normalized[2].property, "components");
    EXPECT_EQ(normalized[3].type, PatchOperationType::MoveOwned);
    EXPECT_EQ(normalized[3].property, "children");
}

TEST(AssetPrefabPipelineTests, ExtractsNestedPrefabDependenciesAndValidatesCycles)
{
    using namespace NLS::Engine::Serialize;

    const auto parentId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("81818181-8181-4181-8181-818181818181"));
    const auto childId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("82828282-8282-4282-8282-828282828282"));
    const auto missingId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("83838383-8383-4383-8383-838383838383"));

    NLS::Engine::Assets::PrefabArtifact parent;
    parent.assetId = parentId;
    parent.graph.format = "Nullus.ObjectGraph.Prefab";
    parent.graph.version = 1;
    parent.graph.documentId = NLS::Guid::NewDeterministic("Nested.Parent.Document");
    parent.graph.root = ObjectId(NLS::Guid::NewDeterministic("Nested.Parent.Root"));
    parent.graph.objects.push_back({
        parent.graph.root,
        "NLS::Engine::GameObject",
        "Parent",
        "",
        ObjectRecordState::Alive,
        {
            {"name", PropertyValue::String("Parent")},
            {"tag", PropertyValue::String({})},
            {"components", PropertyValue::Array({})},
            {"children", PropertyValue::Array({})},
            {"parent", PropertyValue::Null()},
            {"nestedPrefab", MakeObjectReference(childId, "prefab:Child")}
        },
        MakeLocalIdentifierInFile(parent.graph.root)
    });

    const auto dependencies = NLS::Engine::Assets::ExtractNestedPrefabDependencies(parent);
    ASSERT_EQ(dependencies.size(), 1u);
    EXPECT_EQ(dependencies.front(), childId);

    NLS::Engine::Assets::PrefabArtifact child = parent;
    child.assetId = childId;
    child.graph.objects.front().properties.back().value = MakeObjectReference(parentId, "prefab:Parent");

    const auto cycleDiagnostics = NLS::Engine::Assets::ValidateNestedPrefabDependencies({parent, child});
    ASSERT_TRUE(cycleDiagnostics.HasErrors());
    EXPECT_EQ(
        cycleDiagnostics.GetItems()[0].GetCode(),
        NLS::Engine::Serialize::SerializationDiagnosticCode::InvalidPrefabOverride);

    child.graph.objects.front().properties.back().value = MakeObjectReference(missingId, "prefab:Missing");
    const auto missingDiagnostics = NLS::Engine::Assets::ValidateNestedPrefabDependencies({parent, child});
    ASSERT_TRUE(missingDiagnostics.HasErrors());
    EXPECT_EQ(
        missingDiagnostics.GetItems()[0].GetCode(),
        NLS::Engine::Serialize::SerializationDiagnosticCode::MissingAsset);
}

TEST(AssetPrefabPipelineTests, BuildsGeneratedModelPrefabHierarchyWithRendererAssetReferences)
{
    NLS::Render::Assets::ImportedScene scene;
    scene.sourceAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("11111111-1111-4111-8111-111111111111"));
    scene.sceneKey = "HeroScene";
    scene.nodes.push_back({"node/root", "Hero", "", "", ""});
    scene.nodes.push_back({"node/body", "Body", "node/root", "mesh/body", ""});
    NLS::Render::Assets::ImportedScenePrimitive bodyPrimitive;
    bodyPrimitive.materialKey = "converted-material/body";
    NLS::Render::Assets::ImportedSceneNamedRecord bodyMesh;
    bodyMesh.sourceKey = "mesh/body";
    bodyMesh.name = "BodyMesh";
    bodyMesh.primitives.push_back(std::move(bodyPrimitive));
    scene.meshes.push_back(std::move(bodyMesh));
    scene.materials.push_back({"converted-material/body", "BodyMaterial"});

    NLS::Core::Assets::ArtifactManifest manifest;
    manifest.sourceAssetId = scene.sourceAssetId;
    manifest.primarySubAssetKey = "prefab:HeroScene";
    manifest.subAssets.push_back({
        scene.sourceAssetId,
        "mesh:mesh/body",
        NLS::Core::Assets::ArtifactType::Mesh,
        "mesh",
        "editor-windows",
        "Library/Artifacts/Hero/6df3bbd26c5f75f55442bdd89bd9cbe1dd2e92b3a92ca5eadedab558aab77af2",
        "mesh-hash"
    });
    manifest.subAssets.push_back({
        scene.sourceAssetId,
        "material:converted-material/body",
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        "editor-windows",
        "Library/Artifacts/Hero/44d7db23f1c8b23240a9771c22d8237067b92bea1d377a6dc37bd9a8bae325ae",
        "material-hash"
    });

    auto result = NLS::Engine::Assets::BuildGeneratedModelPrefab(
        scene,
        NLS::Render::Assets::GenerateSceneSubAssets(scene),
        manifest);

    ASSERT_FALSE(result.diagnostics.HasErrors());
    EXPECT_EQ(result.artifact.graph.format, "Nullus.ObjectGraph.Prefab");
    EXPECT_TRUE(result.artifact.assetId.IsValid());
    EXPECT_FALSE(result.artifact.graph.Validate().HasErrors());

    const auto* hero = FindRecord(result.artifact.graph, "Hero", "NLS::Engine::GameObject");
    ASSERT_NE(hero, nullptr);
    const auto* body = FindRecord(result.artifact.graph, "Body", "NLS::Engine::GameObject");
    ASSERT_NE(body, nullptr);

    const auto* bodyParent = FindProperty(*body, "parent");
    ASSERT_NE(bodyParent, nullptr);
    ASSERT_EQ(bodyParent->value.GetKind(), NLS::Engine::Serialize::PropertyValue::Kind::ObjectReference);
    const auto resolvedBodyParent = result.artifact.graph.ResolveObjectReference(
        bodyParent->value.GetObjectReference());
    ASSERT_TRUE(resolvedBodyParent.has_value());
    EXPECT_EQ(*resolvedBodyParent, hero->id);

    const auto* meshFilter = FindRecord(
        result.artifact.graph,
        "Body MeshFilter",
        "NLS::Engine::Components::MeshFilter");
    ASSERT_NE(meshFilter, nullptr);
    const auto* meshProperty = FindProperty(*meshFilter, "mesh");
    ASSERT_NE(meshProperty, nullptr);
    ASSERT_EQ(meshProperty->value.GetKind(), NLS::Engine::Serialize::PropertyValue::Kind::ObjectReference);
    EXPECT_EQ(meshProperty->value.GetObjectReference().filePath, "mesh:mesh/body");

    const auto* meshRenderer = FindRecord(
        result.artifact.graph,
        "Body MeshRenderer",
        "NLS::Engine::Components::MeshRenderer");
    ASSERT_NE(meshRenderer, nullptr);
    EXPECT_EQ(FindProperty(*meshRenderer, "mesh"), nullptr);
    const auto* frustumBehaviour = FindProperty(*meshRenderer, "frustumBehaviour");
    ASSERT_NE(frustumBehaviour, nullptr);
    ASSERT_EQ(frustumBehaviour->value.GetKind(), NLS::Engine::Serialize::PropertyValue::Kind::String);
    EXPECT_EQ(frustumBehaviour->value.GetString(), "CULL_MODEL");

    const auto* materials = FindProperty(*meshRenderer, "materials");
    ASSERT_NE(materials, nullptr);
    ASSERT_EQ(materials->value.GetKind(), NLS::Engine::Serialize::PropertyValue::Kind::Array);
    ASSERT_EQ(materials->value.GetArray().size(), 1u);
    ASSERT_EQ(
        materials->value.GetArray()[0].GetKind(),
        NLS::Engine::Serialize::PropertyValue::Kind::ObjectReference);
    EXPECT_EQ(
        materials->value.GetArray()[0].GetObjectReference().filePath,
        "material:converted-material/body");

    EXPECT_TRUE(ContainsResolvedAsset(result.artifact.resolvedAssets, "Mesh", "mesh:mesh/body"));
    EXPECT_TRUE(ContainsResolvedAsset(result.artifact.resolvedAssets, "Material", "material:converted-material/body"));
    EXPECT_EQ(FindResolvedAsset(result.artifact.resolvedAssets, "Model", "model:HeroScene"), nullptr);
    EXPECT_EQ(result.artifact.sourceToRuntimeObject.size(), result.artifact.graph.objects.size());
}

TEST(AssetPrefabPipelineTests, GeneratedModelPrefabUsesSceneKeyForSingleParserRootNodeName)
{
    NLS::Render::Assets::ImportedScene scene;
    scene.sourceAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("45454545-4545-4545-8545-454545454545"));
    scene.sceneKey = "NewSponza_Main_Yup_003";
    scene.nodes.push_back({"parser/node/0", "RootNode", "", "", ""});
    scene.nodes.push_back({"parser/node/1", "SponzaMesh", "parser/node/0", "parser/mesh/0", ""});

    NLS::Render::Assets::ImportedScenePrimitive primitive;
    primitive.materialKey = "parser/material/21";
    NLS::Render::Assets::ImportedSceneNamedRecord mesh;
    mesh.sourceKey = "parser/mesh/0";
    mesh.name = "SponzaMesh";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));
    scene.materials.push_back({"parser/material/21", "dirt_decal"});

    NLS::Core::Assets::ArtifactManifest manifest;
    manifest.sourceAssetId = scene.sourceAssetId;
    manifest.primarySubAssetKey = "prefab:NewSponza_Main_Yup_003";
    manifest.subAssets.push_back({
        scene.sourceAssetId,
        "mesh:parser/mesh/0",
        NLS::Core::Assets::ArtifactType::Mesh,
        "mesh",
        "editor-windows",
        "Library/Artifacts/Sponza/36eee85124b95361c55a48634e6956a87607d0b6a69bfd04ffcd04f145ffa8d7",
        "mesh-hash"
    });
    manifest.subAssets.push_back({
        scene.sourceAssetId,
        "material:parser/material/21",
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        "editor-windows",
        "Library/Artifacts/Sponza/11263f783dca8a1eaeb7d290f4dda830529ec0a0276bff5f721e2b80202ad928",
        "material-hash"
    });

    const auto result = NLS::Engine::Assets::BuildGeneratedModelPrefab(
        scene,
        NLS::Render::Assets::GenerateSceneSubAssets(scene),
        manifest);

    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_FALSE(result.artifact.graph.Validate().HasErrors());

    const auto expectedRootId = MakeGeneratedModelPrefabObjectId(scene, "node:parser/node/0");
    const auto expectedRootTransformId = MakeGeneratedModelPrefabObjectId(scene, "component:parser/node/0:transform");
    EXPECT_EQ(result.artifact.graph.root, expectedRootId);
    EXPECT_NE(result.artifact.sourceToRuntimeObject.find(expectedRootId), result.artifact.sourceToRuntimeObject.end());
    EXPECT_NE(
        result.artifact.sourceToRuntimeObject.find(expectedRootTransformId),
        result.artifact.sourceToRuntimeObject.end());

    const auto* root = FindRecordById(result.artifact.graph, result.artifact.graph.root);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->id, expectedRootId);
    EXPECT_EQ(root->typeName, "NLS::Engine::GameObject");
    EXPECT_EQ(root->debugName, scene.sceneKey);

    const auto* rootTransform = FindRecordById(result.artifact.graph, expectedRootTransformId);
    ASSERT_NE(rootTransform, nullptr);
    EXPECT_EQ(rootTransform->debugName, scene.sceneKey + " Transform");

    const auto* rootName = FindProperty(*root, "name");
    ASSERT_NE(rootName, nullptr);
    ASSERT_EQ(rootName->value.GetKind(), NLS::Engine::Serialize::PropertyValue::Kind::String);
    EXPECT_EQ(rootName->value.GetString(), scene.sceneKey);
    EXPECT_EQ(FindRecord(result.artifact.graph, "RootNode", "NLS::Engine::GameObject"), nullptr);

    const auto* child = FindRecord(result.artifact.graph, "SponzaMesh", "NLS::Engine::GameObject");
    ASSERT_NE(child, nullptr);
    const auto* childParent = FindProperty(*child, "parent");
    ASSERT_NE(childParent, nullptr);
    ASSERT_EQ(childParent->value.GetKind(), NLS::Engine::Serialize::PropertyValue::Kind::ObjectReference);
    const auto resolvedChildParent = result.artifact.graph.ResolveObjectReference(
        childParent->value.GetObjectReference());
    ASSERT_TRUE(resolvedChildParent.has_value());
    EXPECT_EQ(*resolvedChildParent, root->id);
}

TEST(AssetPrefabPipelineTests, GeneratedModelPrefabKeepsChildParserRootNodeName)
{
    NLS::Render::Assets::ImportedScene scene;
    scene.sourceAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("56565656-5656-4656-8656-565656565656"));
    scene.sceneKey = "AuthoredChildRootNode";
    scene.nodes.push_back({"parser/node/0", "AuthoredRoot", "", "", ""});
    scene.nodes.push_back({"parser/node/1", "RootNode", "parser/node/0", "", ""});

    const auto result = NLS::Engine::Assets::BuildGeneratedModelPrefab(
        scene,
        NLS::Render::Assets::GenerateSceneSubAssets(scene),
        {});

    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_FALSE(result.artifact.graph.Validate().HasErrors());

    const auto* root = FindRecordById(result.artifact.graph, result.artifact.graph.root);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->debugName, "AuthoredRoot");

    const auto* child = FindRecord(result.artifact.graph, "RootNode", "NLS::Engine::GameObject");
    ASSERT_NE(child, nullptr);
    const auto* childName = FindProperty(*child, "name");
    ASSERT_NE(childName, nullptr);
    ASSERT_EQ(childName->value.GetKind(), NLS::Engine::Serialize::PropertyValue::Kind::String);
    EXPECT_EQ(childName->value.GetString(), "RootNode");
}

TEST(AssetPrefabPipelineTests, GeneratedModelPrefabInstantiationResolvesSubAssetHintsToArtifacts)
{
    NLS::Render::Assets::ImportedScene scene;
    scene.sourceAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("90909090-9090-4090-8090-909090909090"));
    scene.sceneKey = "Hero";
    scene.nodes.push_back({"node/body", "Body", "", "mesh/body", ""});

    NLS::Render::Assets::ImportedScenePrimitive primitive;
    primitive.materialKey = "material/body";
    NLS::Render::Assets::ImportedSceneNamedRecord mesh;
    mesh.sourceKey = "mesh/body";
    mesh.name = "BodyMesh";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));
    scene.materials.push_back({"material/body", "BodyMaterial"});

    const auto meshSubAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("91919191-9191-4191-8191-919191919191"));
    const auto materialSubAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("92929292-9292-4292-8292-929292929292"));

    NLS::Core::Assets::ArtifactManifest manifest;
    manifest.sourceAssetId = scene.sourceAssetId;
    manifest.subAssets.push_back({
        meshSubAssetId,
        "mesh:mesh/body",
        NLS::Core::Assets::ArtifactType::Mesh,
        "mesh",
        "editor-windows",
        "Library/Artifacts/Hero/meshes/db7ffec2d25e80c7b075bc30a992e27e5f392f809146715c3cdf514a6fba8beb",
        "mesh-hash"
    });
    manifest.subAssets.push_back({
        materialSubAssetId,
        "material:material/body",
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        "editor-windows",
        "Library/Artifacts/Hero/materials/8ca977f3a8a054ff6767e381b334be9e47456f725e02f84e11a3b5b1f3f4218b",
        "material-hash"
    });

    auto result = NLS::Engine::Assets::BuildGeneratedModelPrefab(
        scene,
        NLS::Render::Assets::GenerateSceneSubAssets(scene),
        manifest);

    ASSERT_FALSE(result.diagnostics.HasErrors());

    NLS::Engine::SceneSystem::Scene runtimeScene;
    NLS::Engine::Serialize::LoadPolicy policy;
    policy.deferAssetReferenceResolution = true;
    const auto instance = NLS::Engine::Assets::InstantiatePrefabArtifact(result.artifact, runtimeScene, policy);

    ASSERT_FALSE(instance.diagnostics.HasErrors());
    ASSERT_NE(instance.root, nullptr);

    auto* meshFilter = instance.root->GetComponent<NLS::Engine::Components::MeshFilter>();
    ASSERT_NE(meshFilter, nullptr);
    EXPECT_EQ(meshFilter->GetModelPath(), "Library/Artifacts/Hero/meshes/db7ffec2d25e80c7b075bc30a992e27e5f392f809146715c3cdf514a6fba8beb");

    auto* meshRenderer = instance.root->GetComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(meshRenderer, nullptr);
    EXPECT_EQ(
        meshRenderer->GetFrustumBehaviour(),
        NLS::Engine::Components::MeshRenderer::EFrustumBehaviour::CULL_MODEL);

    const auto materials = meshRenderer->GetMaterialPaths();
    ASSERT_EQ(materials.size(), 1u);
    EXPECT_EQ(materials[0], "Library/Artifacts/Hero/materials/8ca977f3a8a054ff6767e381b334be9e47456f725e02f84e11a3b5b1f3f4218b");
}

TEST(AssetPrefabPipelineTests, GeneratedModelPrefabPreservesSparseMaterialSlots)
{
    NLS::Render::Assets::ImportedScene scene;
    scene.sourceAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("73737373-7373-4373-8373-737373737373"));
    scene.sceneKey = "SparseHero";
    scene.nodes.push_back({"node/body", "Body", "", "mesh/body", ""});

    NLS::Render::Assets::ImportedScenePrimitive primitive;
    primitive.materialKey = "parser/material/1";
    NLS::Render::Assets::ImportedSceneNamedRecord mesh;
    mesh.sourceKey = "mesh/body";
    mesh.name = "BodyMesh";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));
    scene.materials.push_back({"parser/material/0", "UnusedMaterial"});
    scene.materials.push_back({"parser/material/1", "VisibleMaterial"});

    NLS::Core::Assets::ArtifactManifest manifest;
    manifest.sourceAssetId = scene.sourceAssetId;
    manifest.subAssets.push_back({
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("74747474-7474-4474-8474-747474747474")),
        "mesh:mesh/body",
        NLS::Core::Assets::ArtifactType::Mesh,
        "mesh",
        "editor-windows",
        "Library/Artifacts/Sparse/meshes/db7ffec2d25e80c7b075bc30a992e27e5f392f809146715c3cdf514a6fba8beb",
        "mesh-hash"
    });
    manifest.subAssets.push_back({
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("75757575-7575-4575-8575-757575757575")),
        "material:parser/material/1",
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        "editor-windows",
        "Library/Artifacts/Sparse/materials/2384ba5f4dd69868ad409a745d029b3352710ff25b0d0391a44b6e32697d5f9f",
        "material-hash"
    });

    auto result = NLS::Engine::Assets::BuildGeneratedModelPrefab(
        scene,
        NLS::Render::Assets::GenerateSceneSubAssets(scene),
        manifest);

    ASSERT_FALSE(result.diagnostics.HasErrors());

    const auto* meshRenderer = FindRecord(
        result.artifact.graph,
        "Body MeshRenderer",
        "NLS::Engine::Components::MeshRenderer");
    ASSERT_NE(meshRenderer, nullptr);
    const auto* materials = FindProperty(*meshRenderer, "materials");
    ASSERT_NE(materials, nullptr);
    ASSERT_EQ(materials->value.GetKind(), NLS::Engine::Serialize::PropertyValue::Kind::Array);
    ASSERT_EQ(materials->value.GetArray().size(), 2u);
    ASSERT_EQ(materials->value.GetArray()[0].GetKind(), NLS::Engine::Serialize::PropertyValue::Kind::ObjectReference);
    EXPECT_FALSE(materials->value.GetArray()[0].GetObjectReference().IsValid());
    ASSERT_EQ(materials->value.GetArray()[1].GetKind(), NLS::Engine::Serialize::PropertyValue::Kind::ObjectReference);
    EXPECT_EQ(
        materials->value.GetArray()[1].GetObjectReference().filePath,
        "material:parser/material/1");

    NLS::Engine::SceneSystem::Scene runtimeScene;
    NLS::Engine::Serialize::LoadPolicy policy;
    policy.deferAssetReferenceResolution = true;
    const auto instance = NLS::Engine::Assets::InstantiatePrefabArtifact(result.artifact, runtimeScene, policy);

    ASSERT_FALSE(instance.diagnostics.HasErrors());
    ASSERT_NE(instance.root, nullptr);
    auto* runtimeMeshRenderer = instance.root->GetComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(runtimeMeshRenderer, nullptr);
    const auto runtimeMaterialPaths = runtimeMeshRenderer->GetMaterialPaths();
    ASSERT_EQ(runtimeMaterialPaths.size(), 2u);
    EXPECT_TRUE(runtimeMaterialPaths[0].empty());
    EXPECT_EQ(runtimeMaterialPaths[1], "Library/Artifacts/Sparse/materials/2384ba5f4dd69868ad409a745d029b3352710ff25b0d0391a44b6e32697d5f9f");
}

TEST(AssetPrefabPipelineTests, GeneratedModelPrefabCreatesInactiveHLODProxyNode)
{
    NLS::Render::Assets::ImportedScene scene;
    scene.sourceAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("41414141-4141-4141-8141-414141414141"));
    scene.sceneKey = "HLODCityBlock";
    scene.nodes.push_back({"node/root", "Root", "", "", ""});
    scene.nodes.push_back({"node/cluster", "Cluster", "node/root", "", ""});
    scene.nodes.push_back({"node/childA", "ChildA", "node/cluster", "mesh/childA", ""});
    scene.nodes.push_back({"node/childB", "ChildB", "node/cluster", "mesh/childB", ""});

    NLS::Render::Assets::ImportedScenePrimitive primitiveA;
    primitiveA.materialKey = "material/body";
    NLS::Render::Assets::ImportedScenePrimitive primitiveB;
    primitiveB.materialKey = "material/body";
    NLS::Render::Assets::ImportedSceneNamedRecord childAMesh;
    childAMesh.sourceKey = "mesh/childA";
    childAMesh.name = "ChildAMesh";
    childAMesh.primitiveCount = 1u;
    childAMesh.primitives.push_back(std::move(primitiveA));
    scene.meshes.push_back(std::move(childAMesh));
    NLS::Render::Assets::ImportedSceneNamedRecord childBMesh;
    childBMesh.sourceKey = "mesh/childB";
    childBMesh.name = "ChildBMesh";
    childBMesh.primitiveCount = 1u;
    childBMesh.primitives.push_back(std::move(primitiveB));
    scene.meshes.push_back(std::move(childBMesh));
    scene.materials.push_back({"material/body", "BodyMaterial"});

    NLS::Core::Assets::ArtifactManifest manifest;
    manifest.sourceAssetId = scene.sourceAssetId;
    manifest.subAssets.push_back({
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("42424242-4242-4242-8242-424242424242")),
        "mesh:mesh/childA",
        NLS::Core::Assets::ArtifactType::Mesh,
        "mesh",
        "editor-windows",
        "Library/Artifacts/HLOD/58ba37efcbe39f7c3ed57a6bba7d2583305794be37ad9113fb3113c4cb2a2887",
        "mesh-a-hash"
    });
    manifest.subAssets.push_back({
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("43434343-4343-4343-8343-434343434343")),
        "mesh:mesh/childB",
        NLS::Core::Assets::ArtifactType::Mesh,
        "mesh",
        "editor-windows",
        "Library/Artifacts/HLOD/93930a1419c0821b340a8c2b67983509cfd03be7fa40a911ee14295d94a3bd12",
        "mesh-b-hash"
    });
    manifest.subAssets.push_back({
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("44444444-4444-4444-8444-444444444444")),
        "material:material/body",
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        "editor-windows",
        "Library/Artifacts/HLOD/8ca977f3a8a054ff6767e381b334be9e47456f725e02f84e11a3b5b1f3f4218b",
        "material-hash"
    });
    manifest.subAssets.push_back({
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("45454545-4545-4545-8545-454545454545")),
        "hlod-proxy:node/cluster",
        NLS::Core::Assets::ArtifactType::Mesh,
        "mesh",
        "editor-windows",
        "Library/Artifacts/HLOD/7db0044a982d2592b1d91c8f16b54d922af06b2e0643dbe3552a02027ec3e8ff",
        "proxy-hash"
    });

    const auto result = NLS::Engine::Assets::BuildGeneratedModelPrefab(
        scene,
        NLS::Render::Assets::GenerateSceneSubAssets(scene),
        manifest);

    ASSERT_FALSE(result.diagnostics.HasErrors());
    const auto* clusterRecord = FindRecord(
        result.artifact.graph,
        "Cluster",
        "NLS::Engine::GameObject");
    ASSERT_NE(clusterRecord, nullptr);
    const auto* hlod = FindProperty(*clusterRecord, NLS::Engine::Assets::GeneratedModelPrefabHLODSchema::PropertyName);
    ASSERT_NE(hlod, nullptr);
    const auto* proxyRecord = FindRecord(
        result.artifact.graph,
        "__HLODProxy_Cluster",
        "NLS::Engine::GameObject");
    ASSERT_NE(proxyRecord, nullptr);
    const auto* active = FindProperty(*proxyRecord, "active");
    ASSERT_NE(active, nullptr);
    ASSERT_EQ(active->value.GetKind(), NLS::Engine::Serialize::PropertyValue::Kind::Bool);
    EXPECT_FALSE(active->value.GetBool());

    NLS::Engine::SceneSystem::Scene runtimeScene;
    NLS::Engine::Serialize::LoadPolicy policy;
    policy.deferAssetReferenceResolution = true;
    const auto instance = NLS::Engine::Assets::InstantiatePrefabArtifact(result.artifact, runtimeScene, policy);

    ASSERT_FALSE(instance.diagnostics.HasErrors());
    auto* proxyObject = runtimeScene.FindGameObjectByName("__HLODProxy_Cluster");
    ASSERT_NE(proxyObject, nullptr);
    EXPECT_FALSE(proxyObject->IsSelfActive());
    EXPECT_EQ(proxyObject->GetSourceObjectKey(), "hlod-proxy:node/cluster");
    ASSERT_NE(proxyObject->GetComponent<NLS::Engine::Components::MeshRenderer>(), nullptr);
    auto* proxyFilter = proxyObject->GetComponent<NLS::Engine::Components::MeshFilter>();
    ASSERT_NE(proxyFilter, nullptr);
    EXPECT_EQ(proxyFilter->GetModelPath(), "Library/Artifacts/HLOD/7db0044a982d2592b1d91c8f16b54d922af06b2e0643dbe3552a02027ec3e8ff");
}

TEST(AssetPrefabPipelineTests, GeneratedModelPrefabSplitsMultiPrimitiveMeshIntoPrimitiveRenderers)
{
    NLS::Render::Assets::ImportedScene scene;
    scene.sourceAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("34343434-3434-4434-8434-343434343434"));
    scene.sceneKey = "MultiPrimitiveHero";
    scene.nodes.push_back({"node/body", "Body", "", "mesh/body", ""});

    NLS::Render::Assets::ImportedScenePrimitive firstPrimitive;
    firstPrimitive.materialKey = "parser/material/0";
    NLS::Render::Assets::ImportedScenePrimitive secondPrimitive;
    secondPrimitive.materialKey = "parser/material/1";
    NLS::Render::Assets::ImportedSceneNamedRecord mesh;
    mesh.sourceKey = "mesh/body";
    mesh.name = "BodyMesh";
    mesh.primitiveCount = 2u;
    mesh.primitives.push_back(std::move(firstPrimitive));
    mesh.primitives.push_back(std::move(secondPrimitive));
    scene.meshes.push_back(std::move(mesh));
    scene.materials.push_back({"parser/material/0", "FirstMaterial"});
    scene.materials.push_back({"parser/material/1", "SecondMaterial"});

    NLS::Core::Assets::ArtifactManifest manifest;
    manifest.sourceAssetId = scene.sourceAssetId;
    manifest.subAssets.push_back({
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("35353535-3535-4535-8535-353535353535")),
        "mesh:mesh/body/primitive/0",
        NLS::Core::Assets::ArtifactType::Mesh,
        "mesh",
        "editor-windows",
        "Library/Artifacts/Multi/meshes/dfbb52541534973df96223ed3b893b0730a390e524ff176c54cb15009f4d424a",
        "mesh-hash-0"
    });
    manifest.subAssets.push_back({
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("36363636-3636-4636-8636-363636363636")),
        "mesh:mesh/body/primitive/1",
        NLS::Core::Assets::ArtifactType::Mesh,
        "mesh",
        "editor-windows",
        "Library/Artifacts/Multi/meshes/c2a4c16613ac54d03e2b727948c7e9adc06569346e97749ccddea49d55f0f58b",
        "mesh-hash-1"
    });
    manifest.subAssets.push_back({
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("37373737-3737-4737-8737-373737373737")),
        "material:parser/material/0",
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        "editor-windows",
        "Library/Artifacts/Multi/materials/dcae5a38be96376d6b06a1b70d9e3897ddfbe16937de85e3ffa05c78b878b351",
        "material-hash-0"
    });
    manifest.subAssets.push_back({
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("38383838-3838-4838-8838-383838383838")),
        "material:parser/material/1",
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        "editor-windows",
        "Library/Artifacts/Multi/materials/47ad399b45bcdda2bfbe6ee59e6a6e36ac148a09e62f7ec47862fae4f8e8c07a",
        "material-hash-1"
    });

    auto result = NLS::Engine::Assets::BuildGeneratedModelPrefab(
        scene,
        NLS::Render::Assets::GenerateSceneSubAssets(scene),
        manifest);

    ASSERT_FALSE(result.diagnostics.HasErrors());

    const auto* body = FindRecord(result.artifact.graph, "Body", "NLS::Engine::GameObject");
    ASSERT_NE(body, nullptr);
    EXPECT_EQ(FindRecord(result.artifact.graph, "Body MeshFilter", "NLS::Engine::Components::MeshFilter"), nullptr);
    EXPECT_EQ(FindRecord(result.artifact.graph, "Body MeshRenderer", "NLS::Engine::Components::MeshRenderer"), nullptr);

    const auto* firstMeshFilter = FindRecord(
        result.artifact.graph,
        "Body Primitive 0 MeshFilter",
        "NLS::Engine::Components::MeshFilter");
    ASSERT_NE(firstMeshFilter, nullptr);
    const auto* firstMesh = FindProperty(*firstMeshFilter, "mesh");
    ASSERT_NE(firstMesh, nullptr);
    EXPECT_EQ(firstMesh->value.GetObjectReference().filePath, "mesh:mesh/body/primitive/0");

    const auto* firstMeshRenderer = FindRecord(
        result.artifact.graph,
        "Body Primitive 0 MeshRenderer",
        "NLS::Engine::Components::MeshRenderer");
    ASSERT_NE(firstMeshRenderer, nullptr);
    const auto* firstMaterials = FindProperty(*firstMeshRenderer, "materials");
    ASSERT_NE(firstMaterials, nullptr);
    ASSERT_EQ(firstMaterials->value.GetArray().size(), 1u);
    EXPECT_EQ(firstMaterials->value.GetArray()[0].GetObjectReference().filePath, "material:parser/material/0");

    const auto* secondMeshFilter = FindRecord(
        result.artifact.graph,
        "Body Primitive 1 MeshFilter",
        "NLS::Engine::Components::MeshFilter");
    ASSERT_NE(secondMeshFilter, nullptr);
    const auto* secondMesh = FindProperty(*secondMeshFilter, "mesh");
    ASSERT_NE(secondMesh, nullptr);
    EXPECT_EQ(secondMesh->value.GetObjectReference().filePath, "mesh:mesh/body/primitive/1");

    const auto* secondMeshRenderer = FindRecord(
        result.artifact.graph,
        "Body Primitive 1 MeshRenderer",
        "NLS::Engine::Components::MeshRenderer");
    ASSERT_NE(secondMeshRenderer, nullptr);
    const auto* secondMaterials = FindProperty(*secondMeshRenderer, "materials");
    ASSERT_NE(secondMaterials, nullptr);
    ASSERT_EQ(secondMaterials->value.GetArray().size(), 2u);
    EXPECT_FALSE(secondMaterials->value.GetArray()[0].GetObjectReference().IsValid());
    EXPECT_EQ(secondMaterials->value.GetArray()[1].GetObjectReference().filePath, "material:parser/material/1");

    EXPECT_TRUE(ContainsResolvedAsset(result.artifact.resolvedAssets, "Mesh", "mesh:mesh/body/primitive/0"));
    EXPECT_TRUE(ContainsResolvedAsset(result.artifact.resolvedAssets, "Mesh", "mesh:mesh/body/primitive/1"));
}

TEST(AssetPrefabPipelineTests, DeferredAssetResolutionIgnoresLegacyStringMaterialPaths)
{
    using namespace NLS::Engine::Serialize;

    const auto rootId = ObjectId(NLS::Guid::Parse("91919191-1111-4111-8111-111111111111"));
    const auto meshRendererId = ObjectId(NLS::Guid::Parse("92929292-2222-4222-8222-222222222222"));

    NLS::Engine::Assets::PrefabArtifact artifact;
    artifact.graph.documentId = NLS::Guid::Parse("93939393-3333-4333-8333-333333333333");
    artifact.graph.root = rootId;

    ObjectRecord root;
    root.id = rootId;
    root.localIdentifierInFile = MakeLocalIdentifierInFile(rootId);
    root.typeName = "NLS::Engine::GameObject";
    root.debugName = "Legacy Path Root";
    root.properties.push_back({"name", PropertyValue::String("Legacy Path Root")});
    root.properties.push_back({"components", PropertyValue::Array({
        PropertyValue::OwnedReference(meshRendererId)
    })});

    ObjectRecord meshRenderer;
    meshRenderer.id = meshRendererId;
    meshRenderer.localIdentifierInFile = MakeLocalIdentifierInFile(meshRendererId);
    meshRenderer.typeName = "NLS::Engine::Components::MeshRenderer";
    meshRenderer.debugName = "Legacy Path MeshRenderer";
    meshRenderer.properties.push_back({"materials", PropertyValue::Array({
        PropertyValue::String("Library/Artifacts/Legacy/47b24ab4b128645b99328e0a68370de1202b0ba370eafc30e8bb0b0b7cf8b5ae")
    })});

    artifact.graph.objects.push_back(std::move(root));
    artifact.graph.objects.push_back(std::move(meshRenderer));

    NLS::Engine::SceneSystem::Scene runtimeScene;
    NLS::Engine::Serialize::LoadPolicy policy;
    policy.deferAssetReferenceResolution = true;
    const auto instance = NLS::Engine::Assets::InstantiatePrefabArtifact(artifact, runtimeScene, policy);

    ASSERT_FALSE(instance.diagnostics.HasErrors());
    ASSERT_NE(instance.root, nullptr);
    auto* runtimeMeshRenderer = instance.root->GetComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(runtimeMeshRenderer, nullptr);
    EXPECT_TRUE(runtimeMeshRenderer->GetMaterialPaths().empty());
}

TEST(AssetPrefabPipelineTests, GeneratedModelPrefabInstantiationMapsDeepImportedHierarchy)
{
    NLS::Render::Assets::ImportedScene scene;
    scene.sourceAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("96969696-9696-4696-8696-969696969696"));
    scene.sceneKey = "DeepModel";
    scene.nodes.push_back({"node/floor", "Floor", "", "", ""});
    scene.nodes.push_back({"node/group", "Group", "node/floor", "", ""});
    scene.nodes.push_back({"node/mesh", "MeshNode", "node/group", "mesh/body", ""});

    NLS::Render::Assets::ImportedScenePrimitive primitive;
    primitive.materialKey = "material/body";
    NLS::Render::Assets::ImportedSceneNamedRecord mesh;
    mesh.sourceKey = "mesh/body";
    mesh.name = "BodyMesh";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));
    scene.materials.push_back({"material/body", "BodyMaterial"});

    NLS::Core::Assets::ArtifactManifest manifest;
    manifest.sourceAssetId = scene.sourceAssetId;
    manifest.subAssets.push_back({
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("97979797-9797-4797-8797-979797979797")),
        "mesh:mesh/body",
        NLS::Core::Assets::ArtifactType::Mesh,
        "mesh",
        "editor-windows",
        "Library/Artifacts/Deep/meshes/db7ffec2d25e80c7b075bc30a992e27e5f392f809146715c3cdf514a6fba8beb",
        "mesh-hash"
    });
    manifest.subAssets.push_back({
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("98989898-9898-4898-8898-989898989898")),
        "material:material/body",
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        "editor-windows",
        "Library/Artifacts/Deep/materials/8ca977f3a8a054ff6767e381b334be9e47456f725e02f84e11a3b5b1f3f4218b",
        "material-hash"
    });

    auto result = NLS::Engine::Assets::BuildGeneratedModelPrefab(
        scene,
        NLS::Render::Assets::GenerateSceneSubAssets(scene),
        manifest);

    ASSERT_FALSE(result.diagnostics.HasErrors());

    NLS::Engine::SceneSystem::Scene runtimeScene;
    NLS::Engine::Serialize::LoadPolicy policy;
    policy.deferAssetReferenceResolution = true;
    const auto instance = NLS::Engine::Assets::InstantiatePrefabArtifact(result.artifact, runtimeScene, policy);

    ASSERT_FALSE(instance.diagnostics.HasErrors());
    ASSERT_NE(instance.root, nullptr);
    ASSERT_EQ(instance.root->GetChildren().size(), 1u);
    auto* group = instance.root->GetChildren()[0];
    ASSERT_NE(group, nullptr);
    ASSERT_EQ(group->GetChildren().size(), 1u);
    auto* meshNode = group->GetChildren()[0];
    ASSERT_NE(meshNode, nullptr);
    ASSERT_NE(meshNode->GetComponent<NLS::Engine::Components::MeshRenderer>(), nullptr);

    EXPECT_EQ(instance.sourceByInstanceObject.size(), 3u);
    EXPECT_NE(instance.sourceByInstanceObject.find(instance.root), instance.sourceByInstanceObject.end());
    EXPECT_NE(instance.sourceByInstanceObject.find(group), instance.sourceByInstanceObject.end());
    EXPECT_NE(instance.sourceByInstanceObject.find(meshNode), instance.sourceByInstanceObject.end());

    const auto& fastAccess = runtimeScene.GetFastAccessComponents();
    ASSERT_EQ(fastAccess.modelRenderers.size(), 1u);
    EXPECT_EQ(fastAccess.modelRenderers[0], meshNode->GetComponent<NLS::Engine::Components::MeshRenderer>());
}

TEST(AssetPrefabPipelineTests, GeneratedModelPrefabValidationAllowsResolvedSubAssetIds)
{
    NLS::Render::Assets::ImportedScene scene;
    scene.sourceAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("93939393-9393-4393-8393-939393939393"));
    scene.sceneKey = "Hero";
    scene.nodes.push_back({"node/body", "Body", "", "mesh/body", ""});

    NLS::Render::Assets::ImportedScenePrimitive primitive;
    primitive.materialKey = "material/body";
    NLS::Render::Assets::ImportedSceneNamedRecord mesh;
    mesh.sourceKey = "mesh/body";
    mesh.name = "BodyMesh";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));
    scene.materials.push_back({"material/body", "BodyMaterial"});

    NLS::Core::Assets::ArtifactManifest manifest;
    manifest.sourceAssetId = scene.sourceAssetId;
    manifest.subAssets.push_back({
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("94949494-9494-4494-8494-949494949494")),
        "mesh:mesh/body",
        NLS::Core::Assets::ArtifactType::Mesh,
        "mesh",
        "editor-windows",
        "Library/Artifacts/Hero/meshes/db7ffec2d25e80c7b075bc30a992e27e5f392f809146715c3cdf514a6fba8beb",
        "mesh-hash"
    });
    manifest.subAssets.push_back({
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("95959595-9595-4595-8595-959595959595")),
        "material:material/body",
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        "editor-windows",
        "Library/Artifacts/Hero/materials/8ca977f3a8a054ff6767e381b334be9e47456f725e02f84e11a3b5b1f3f4218b",
        "material-hash"
    });

    const auto result = NLS::Engine::Assets::BuildGeneratedModelPrefab(
        scene,
        NLS::Render::Assets::GenerateSceneSubAssets(scene),
        manifest);

    ASSERT_FALSE(result.diagnostics.HasErrors());
    EXPECT_FALSE(result.artifact.Validate().HasErrors());
}

TEST(AssetPrefabPipelineTests, GeneratedModelPrefabReportsSkinningAndMorphRuntimeCapabilityGaps)
{
    NLS::Render::Assets::ImportedScene scene;
    scene.sourceAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("22222222-2222-4222-8222-222222222222"));
    scene.sceneKey = "AnimatedHero";
    scene.nodes.push_back({"node/root", "AnimatedHero", "", "mesh/body", "skin/hero"});
    scene.meshes.push_back({"mesh/body", "BodyMesh"});
    scene.skins.push_back({"skin/hero", "HeroSkin"});
    scene.animations.push_back({"anim/idle", "Idle"});
    scene.morphTargets.push_back({"morph/smile", "Smile"});

    const auto result = NLS::Engine::Assets::BuildGeneratedModelPrefab(
        scene,
        NLS::Render::Assets::GenerateSceneSubAssets(scene),
        NLS::Core::Assets::ArtifactManifest {});

    ASSERT_FALSE(result.diagnostics.GetItems().empty());

    bool foundSkinningGap = false;
    bool foundMorphGap = false;
    for (const auto& diagnostic : result.diagnostics.GetItems())
    {
        foundSkinningGap |= diagnostic.GetMessage().find("runtime-skinning-component-missing") != std::string::npos;
        foundMorphGap |= diagnostic.GetMessage().find("runtime-morph-component-missing") != std::string::npos;
    }

    EXPECT_TRUE(foundSkinningGap);
    EXPECT_TRUE(foundMorphGap);
}
