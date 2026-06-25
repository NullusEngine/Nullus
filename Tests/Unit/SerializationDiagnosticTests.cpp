#include <gtest/gtest.h>

#include "Core/ServiceLocator.h"
#include "Engine/Assets/RuntimeAssetDatabase.h"
#include "GameObject.h"
#include "Serialize/ObjectGraphInstantiator.h"
#include "Serialize/ObjectGraphDocument.h"
#include "Serialize/SerializationDiagnostic.h"

TEST(SerializationDiagnosticTests, DiagnosticStoresCodeSeverityAndMessage)
{
    NLS::Engine::Serialize::SerializationDiagnostic diagnostic(
        NLS::Engine::Serialize::SerializationDiagnosticCode::InvalidGuid,
        NLS::Engine::Serialize::SerializationDiagnosticSeverity::Error,
        "Invalid object id");

    EXPECT_EQ(diagnostic.GetCode(), NLS::Engine::Serialize::SerializationDiagnosticCode::InvalidGuid);
    EXPECT_EQ(diagnostic.GetSeverity(), NLS::Engine::Serialize::SerializationDiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.GetMessage(), "Invalid object id");
    EXPECT_TRUE(diagnostic.IsError());
}

TEST(SerializationDiagnosticTests, DiagnosticListReportsErrors)
{
    NLS::Engine::Serialize::SerializationDiagnosticList diagnostics;

    EXPECT_FALSE(diagnostics.HasErrors());

    diagnostics.Add({
        NLS::Engine::Serialize::SerializationDiagnosticCode::MissingAsset,
        NLS::Engine::Serialize::SerializationDiagnosticSeverity::Warning,
        "Missing editor asset"
    });
    EXPECT_FALSE(diagnostics.HasErrors());

    diagnostics.Add({
        NLS::Engine::Serialize::SerializationDiagnosticCode::DuplicateObjectId,
        NLS::Engine::Serialize::SerializationDiagnosticSeverity::Error,
        "Duplicate object id"
    });
    EXPECT_TRUE(diagnostics.HasErrors());
    EXPECT_EQ(diagnostics.GetItems().size(), 2u);
}

namespace
{
NLS::Engine::Serialize::ObjectId MakeDiagnosticObjectId(const char* label)
{
    return NLS::Engine::Serialize::ObjectId(NLS::Guid::NewDeterministic(label));
}

bool HasDiagnostic(
    const NLS::Engine::Serialize::SerializationDiagnosticList& diagnostics,
    NLS::Engine::Serialize::SerializationDiagnosticCode code,
    NLS::Engine::Serialize::SerializationDiagnosticSeverity severity)
{
    for (const auto& diagnostic : diagnostics.GetItems())
    {
        if (diagnostic.GetCode() == code && diagnostic.GetSeverity() == severity)
            return true;
    }
    return false;
}

NLS::Engine::Serialize::ObjectGraphDocument MakeUnknownTypeSceneDocument()
{
    using namespace NLS::Engine::Serialize;

    ObjectGraphDocument document;
    document.documentId = NLS::Guid::NewDeterministic("UnknownType.Document");
    const auto sceneId = MakeDiagnosticObjectId("UnknownType.Scene");
    const auto unknownId = MakeDiagnosticObjectId("UnknownType.Object");
    document.root = sceneId;

    ObjectRecord scene;
    scene.id = sceneId;
    scene.localIdentifierInFile = MakeLocalIdentifierInFile(sceneId);
    scene.typeName = "NLS::Engine::SceneSystem::Scene";
    scene.properties.push_back({"gameObjects", PropertyValue::Array({PropertyValue::OwnedReference(unknownId)})});

    ObjectRecord unknown;
    unknown.id = unknownId;
    unknown.localIdentifierInFile = MakeLocalIdentifierInFile(unknownId);
    unknown.typeName = "Plugin.UnknownGameObject";
    unknown.debugName = "Unknown Plugin Object";
    unknown.properties.push_back({"raw", PropertyValue::String("preserve me")});

    document.objects.push_back(std::move(scene));
    document.objects.push_back(std::move(unknown));
    return document;
}
}

TEST(SerializationDiagnosticTests, EditorLoadPolicyPreservesUnknownTypesAsWarnings)
{
    using namespace NLS::Engine::Serialize;

    LoadPolicy policy;
    policy.unknownTypePolicy = UnknownTypePolicy::Preserve;

    const auto result = ObjectGraphInstantiator::AnalyzeDocument(MakeUnknownTypeSceneDocument(), policy);

    EXPECT_FALSE(result.diagnostics.HasErrors());
    EXPECT_TRUE(HasDiagnostic(
        result.diagnostics,
        SerializationDiagnosticCode::UnknownType,
        SerializationDiagnosticSeverity::Warning));
}

TEST(SerializationDiagnosticTests, RuntimeLoadPolicyFailsUnknownTypes)
{
    using namespace NLS::Engine::Serialize;

    LoadPolicy policy;
    policy.unknownTypePolicy = UnknownTypePolicy::Fail;

    const auto result = ObjectGraphInstantiator::AnalyzeDocument(MakeUnknownTypeSceneDocument(), policy);

    EXPECT_TRUE(result.diagnostics.HasErrors());
    EXPECT_TRUE(HasDiagnostic(
        result.diagnostics,
        SerializationDiagnosticCode::UnknownType,
        SerializationDiagnosticSeverity::Error));
}

TEST(SerializationDiagnosticTests, RuntimeManifestMissReportsMissingObjectReferenceAsset)
{
    using namespace NLS::Engine::Serialize;

    const auto assetGuid = NLS::Guid::Parse("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    const auto assetId = AssetId(assetGuid);
    const auto sceneId = MakeDiagnosticObjectId("RuntimeManifestMiss.Scene");
    const auto meshFilterId = MakeDiagnosticObjectId("RuntimeManifestMiss.MeshFilter");

    ObjectGraphDocument document;
    document.documentId = NLS::Guid::NewDeterministic("RuntimeManifestMiss.Document");
    document.root = sceneId;

    ObjectRecord scene;
    scene.id = sceneId;
    scene.localIdentifierInFile = MakeLocalIdentifierInFile(sceneId);
    scene.typeName = "NLS::Engine::SceneSystem::Scene";
    scene.properties.push_back({"gameObjects", PropertyValue::Array({})});

    ObjectRecord meshFilter;
    meshFilter.id = meshFilterId;
    meshFilter.localIdentifierInFile = MakeLocalIdentifierInFile(meshFilterId);
    meshFilter.typeName = "NLS::Engine::Components::MeshFilter";
    meshFilter.properties.push_back({"mesh", PropertyValue::ObjectReference(ObjectIdentifier::Asset(
        assetId,
        MakeLocalIdentifierInFile(assetGuid, "mesh:Missing"),
        "Library/Artifacts/Stale/810e1589298eb04e0ac7e97825f72bbc40830ac4e68fee7ac9e101bcbf91226a"))});

    document.objects.push_back(std::move(scene));
    document.objects.push_back(std::move(meshFilter));

    NLS::Engine::Assets::RuntimeAssetManifest manifest;
    manifest.entries.push_back({
        NLS::Core::Assets::AssetId(assetGuid),
        "mesh:Existing",
        NLS::Core::Assets::ArtifactType::Mesh,
        "Nullus.Mesh",
        "Library/Artifacts/Hero/meshes/4067e1eedc9fa569187268ab2e5a0d73f20ab6a3d9e04aa05b288cbd2524e13a",
        "sha256:existing",
        {}
    });
    NLS::Engine::Assets::RuntimeAssetDatabase runtimeAssets(manifest);
    NLS::Core::ServiceLocator::Provide<NLS::Engine::Assets::RuntimeAssetDatabase>(runtimeAssets);

    LoadPolicy policy;
    policy.missingAssetPolicy = MissingAssetPolicy::Fail;
    const auto result = ObjectGraphInstantiator::AnalyzeDocument(document, policy);

    EXPECT_TRUE(result.diagnostics.HasErrors());
    EXPECT_TRUE(HasDiagnostic(
        result.diagnostics,
        SerializationDiagnosticCode::MissingAsset,
        SerializationDiagnosticSeverity::Error));

    NLS::Core::ServiceLocator::Remove<NLS::Engine::Assets::RuntimeAssetDatabase>();
}

TEST(SerializationDiagnosticTests, RuntimeManifestMissReportsMissingBasePrefabReference)
{
    using namespace NLS::Engine::Serialize;

    const auto prefabGuid = NLS::Guid::Parse("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
    const auto prefabId = AssetId(prefabGuid);
    const auto sceneId = MakeDiagnosticObjectId("RuntimeBasePrefabManifestMiss.Scene");

    ObjectGraphDocument document;
    document.documentId = NLS::Guid::NewDeterministic("RuntimeBasePrefabManifestMiss.Document");
    document.root = sceneId;
    document.basePrefab = ObjectIdentifier::Asset(
        prefabId,
        MakeLocalIdentifierInFile(prefabGuid, "prefab:Missing"),
        "Library/Artifacts/Stale/ad7189f52d1b009817ada49470ab67b27a324e90eb750ce87396ee704ea90539");

    ObjectRecord scene;
    scene.id = sceneId;
    scene.localIdentifierInFile = MakeLocalIdentifierInFile(sceneId);
    scene.typeName = "NLS::Engine::SceneSystem::Scene";
    scene.properties.push_back({"gameObjects", PropertyValue::Array({})});
    document.objects.push_back(std::move(scene));

    NLS::Engine::Assets::RuntimeAssetManifest manifest;
    manifest.entries.push_back({
        NLS::Core::Assets::AssetId(prefabGuid),
        "prefab:Existing",
        NLS::Core::Assets::ArtifactType::Prefab,
        "Nullus.Prefab",
        "Library/Artifacts/Hero/670d35a0d13abf40dfcf953b26cff38db2ba16c57287f484aa491e4fcb490772",
        "sha256:existing",
        {}
    });
    NLS::Engine::Assets::RuntimeAssetDatabase runtimeAssets(manifest);
    NLS::Core::ServiceLocator::Provide<NLS::Engine::Assets::RuntimeAssetDatabase>(runtimeAssets);

    LoadPolicy policy;
    policy.missingAssetPolicy = MissingAssetPolicy::Fail;
    const auto result = ObjectGraphInstantiator::AnalyzeDocument(document, policy);

    EXPECT_TRUE(result.diagnostics.HasErrors());
    EXPECT_TRUE(HasDiagnostic(
        result.diagnostics,
        SerializationDiagnosticCode::MissingAsset,
        SerializationDiagnosticSeverity::Error));

    NLS::Core::ServiceLocator::Remove<NLS::Engine::Assets::RuntimeAssetDatabase>();
}

TEST(SerializationDiagnosticTests, SceneInstantiationWithRuntimePolicyRejectsUnknownTypes)
{
    using namespace NLS::Engine::Serialize;

    LoadPolicy policy;
    policy.unknownTypePolicy = UnknownTypePolicy::Fail;

    const auto result = ObjectGraphInstantiator::InstantiateScene(MakeUnknownTypeSceneDocument(), policy);

    EXPECT_EQ(result.scene, nullptr);
    EXPECT_TRUE(result.diagnostics.HasErrors());
    EXPECT_TRUE(HasDiagnostic(
        result.diagnostics,
        SerializationDiagnosticCode::UnknownType,
        SerializationDiagnosticSeverity::Error));
}

TEST(SerializationDiagnosticTests, RuntimeLoadPolicyRejectsMalformedPPtrPropertyShape)
{
    using namespace NLS::Engine::Serialize;

    const auto sceneId = MakeDiagnosticObjectId("MalformedPPtr.Scene");
    const auto gameObjectId = MakeDiagnosticObjectId("MalformedPPtr.GameObject");
    const auto meshFilterId = MakeDiagnosticObjectId("MalformedPPtr.MeshFilter");

    ObjectGraphDocument document;
    document.documentId = NLS::Guid::NewDeterministic("MalformedPPtr.Document");
    document.root = sceneId;

    ObjectRecord scene;
    scene.id = sceneId;
    scene.localIdentifierInFile = MakeLocalIdentifierInFile(sceneId);
    scene.typeName = "NLS::Engine::SceneSystem::Scene";
    scene.properties.push_back({"gameObjects", PropertyValue::Array({PropertyValue::OwnedReference(gameObjectId)})});

    ObjectRecord gameObject;
    gameObject.id = gameObjectId;
    gameObject.localIdentifierInFile = MakeLocalIdentifierInFile(gameObjectId);
    gameObject.typeName = "NLS::Engine::GameObject";
    gameObject.properties.push_back({"name", PropertyValue::String("Malformed PPtr")});
    gameObject.properties.push_back({"tag", PropertyValue::String("")});
    gameObject.properties.push_back({"parent", PropertyValue::Null()});
    gameObject.properties.push_back({"components", PropertyValue::Array({PropertyValue::OwnedReference(meshFilterId)})});

    ObjectRecord meshFilter;
    meshFilter.id = meshFilterId;
    meshFilter.localIdentifierInFile = MakeLocalIdentifierInFile(meshFilterId);
    meshFilter.typeName = "NLS::Engine::Components::MeshFilter";
    meshFilter.properties.push_back({"mesh", PropertyValue::String("Assets/Meshes/Hero.fbx")});

    document.objects.push_back(std::move(scene));
    document.objects.push_back(std::move(gameObject));
    document.objects.push_back(std::move(meshFilter));

    LoadPolicy policy;
    policy.invalidReferencePolicy = InvalidReferencePolicy::Fail;
    const auto result = ObjectGraphInstantiator::InstantiateScene(document, policy);

    EXPECT_EQ(result.scene, nullptr);
    EXPECT_TRUE(result.diagnostics.HasErrors());
    EXPECT_TRUE(HasDiagnostic(
        result.diagnostics,
        SerializationDiagnosticCode::InvalidPropertyType,
        SerializationDiagnosticSeverity::Error));
}
