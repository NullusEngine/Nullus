#include <gtest/gtest.h>

#include "Assets/AssetId.h"
#include "Assets/ArtifactManifest.h"
#include "Engine/Assets/PrefabAsset.h"
#include "Engine/Assets/RuntimeAssetDatabase.h"
#include "Guid.h"
#include "Rendering/SceneRendererMaterialBinding.h"
#include "Serialize/ObjectGraphDocument.h"

namespace
{
NLS::Core::Assets::AssetId MakeAssetId(const char* guid)
{
    return NLS::Core::Assets::AssetId(NLS::Guid::Parse(guid));
}

NLS::Core::Assets::ImportedArtifact MakeArtifact(
    NLS::Core::Assets::AssetId owner,
    std::string subAssetKey,
    NLS::Core::Assets::ArtifactType type,
    std::string loaderId,
    std::string artifactPath)
{
    return {
        owner,
        std::move(subAssetKey),
        type,
        std::move(loaderId),
        "win64",
        std::move(artifactPath),
        "sha256:" + owner.ToString()
    };
}

NLS::Core::Assets::ArtifactManifest MakeManifest(
    NLS::Core::Assets::AssetId owner,
    std::string primarySubAssetKey,
    std::initializer_list<NLS::Core::Assets::ImportedArtifact> artifacts)
{
    NLS::Core::Assets::ArtifactManifest manifest;
    manifest.sourceAssetId = owner;
    manifest.targetPlatform = "win64";
    manifest.primarySubAssetKey = std::move(primarySubAssetKey);
    manifest.subAssets.assign(artifacts.begin(), artifacts.end());
    return manifest;
}
}

TEST(AssetManifestTests, RuntimeManifestIndexesEntriesByGuidAndSubAsset)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Engine::Assets;

    const auto model = MakeAssetId("11111111-1111-4111-8111-111111111111");
    RuntimeAssetManifest manifest;
    manifest.schemaVersion = 1u;
    manifest.targetPlatform = "win64";
    manifest.entries.push_back({
        model,
        "mesh:body",
        ArtifactType::Mesh,
        "mesh",
        "Artifacts/mesh.nmesh",
        "sha256:mesh",
        {}
    });

    RuntimeAssetDatabase database(manifest);

    const auto* entry = database.Resolve(model, "mesh:body");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->artifactPath, "Artifacts/mesh.nmesh");
    EXPECT_EQ(entry->loaderId, "mesh");
    EXPECT_EQ(database.Resolve(model, "mesh:missing"), nullptr);
}

TEST(AssetManifestTests, RuntimeAssetDatabaseMaintainsIndexedLookupForRuntimeRefs)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Engine::Assets;

    const auto model = MakeAssetId("12111111-1111-4111-8111-111111111111");
    RuntimeAssetManifest manifest;
    manifest.schemaVersion = 1u;
    manifest.targetPlatform = "win64";

    for (uint32_t index = 0u; index < 1024u; ++index)
    {
        manifest.entries.push_back({
            model,
            "material:" + std::to_string(index),
            ArtifactType::Material,
            "material",
            "Artifacts/material" + std::to_string(index) + ".nmat",
            "sha256:material" + std::to_string(index),
            {}
        });
    }

    RuntimeAssetDatabase database(manifest);

    EXPECT_EQ(database.GetIndexedEntryCount(), manifest.entries.size());
    const auto* entry = database.Resolve({model, "material:1023"});
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->artifactPath, "Artifacts/material1023.nmat");

    const auto* localEntry = database.ResolveByLocalIdentifierInFile(
        model,
        NLS::Engine::Serialize::MakeLocalIdentifierInFile(model.GetGuid(), "material:1023"));
    ASSERT_NE(localEntry, nullptr);
    EXPECT_EQ(localEntry->artifactPath, "Artifacts/material1023.nmat");
    EXPECT_EQ(database.ResolveByLocalIdentifierInFile(model, 123456789), nullptr);
}

TEST(AssetManifestTests, BuildManifestIncludesDependencyClosureForModelPrefab)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Engine::Assets;

    const auto model = MakeAssetId("22222222-2222-4222-8222-222222222222");
    const auto material = MakeAssetId("33333333-3333-4333-8333-333333333333");
    const auto texture = MakeAssetId("44444444-4444-4444-8444-444444444444");

    auto modelManifest = MakeManifest(model, "prefab:HeroScene", {
        MakeArtifact(model, "prefab:HeroScene", ArtifactType::Prefab, "prefab", "Artifacts/Hero/prefab.nprefab"),
        MakeArtifact(model, "mesh:body", ArtifactType::Mesh, "mesh", "Artifacts/Hero/body.nmesh"),
        MakeArtifact(model, "skeleton:hero", ArtifactType::Skeleton, "skeleton", "Artifacts/Hero/hero.nskeleton"),
        MakeArtifact(model, "skin:body", ArtifactType::Skin, "skin", "Artifacts/Hero/body.nskin"),
        MakeArtifact(model, "animation:idle", ArtifactType::AnimationClip, "animation", "Artifacts/Hero/idle.nanim"),
        MakeArtifact(model, "morph-target:smile", ArtifactType::MorphTarget, "morph-target", "Artifacts/Hero/smile.nmorph")
    });
    modelManifest.dependencies.push_back({AssetDependencyKind::ImportedArtifact, material.ToString(), "material:body"});

    auto materialManifest = MakeManifest(material, "material:body", {
        MakeArtifact(material, "material:body", ArtifactType::Material, "material", "Artifacts/Hero/body.nmat")
    });
    materialManifest.dependencies.push_back({AssetDependencyKind::ImportedArtifact, texture.ToString(), "texture:basecolor"});

    auto textureManifest = MakeManifest(texture, "texture:basecolor", {
        MakeArtifact(texture, "texture:basecolor", ArtifactType::Texture, "texture", "Artifacts/Hero/basecolor.ntex")
    });

    RuntimeManifestBuilder builder;
    builder.AddArtifactManifest(modelManifest);
    builder.AddArtifactManifest(materialManifest);
    builder.AddArtifactManifest(textureManifest);

    const auto result = builder.Build({{model, "prefab:HeroScene"}}, "win64");

    ASSERT_FALSE(result.diagnostics.HasErrors());
    EXPECT_EQ(result.manifest.entries.size(), 8u);
    EXPECT_NE(RuntimeAssetDatabase(result.manifest).Resolve(model, "prefab:HeroScene"), nullptr);
    EXPECT_NE(RuntimeAssetDatabase(result.manifest).Resolve(model, "mesh:body"), nullptr);
    EXPECT_NE(RuntimeAssetDatabase(result.manifest).Resolve(model, "skeleton:hero"), nullptr);
    EXPECT_NE(RuntimeAssetDatabase(result.manifest).Resolve(model, "skin:body"), nullptr);
    EXPECT_NE(RuntimeAssetDatabase(result.manifest).Resolve(model, "animation:idle"), nullptr);
    EXPECT_NE(RuntimeAssetDatabase(result.manifest).Resolve(model, "morph-target:smile"), nullptr);
    EXPECT_NE(RuntimeAssetDatabase(result.manifest).Resolve(material, "material:body"), nullptr);
    EXPECT_NE(RuntimeAssetDatabase(result.manifest).Resolve(texture, "texture:basecolor"), nullptr);
}

TEST(AssetManifestTests, BuildManifestIncludesNestedPrefabDependencyClosure)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Engine::Assets;

    const auto parent = MakeAssetId("66666666-6666-4666-8666-666666666666");
    const auto child = MakeAssetId("77777777-7777-4777-8777-777777777777");

    auto parentManifest = MakeManifest(parent, "prefab:Parent", {
        MakeArtifact(parent, "prefab:Parent", ArtifactType::Prefab, "prefab", "Artifacts/Parent/prefab.nprefab")
    });
    parentManifest.dependencies.push_back({AssetDependencyKind::NestedPrefab, child.ToString(), "prefab:Child"});

    auto childManifest = MakeManifest(child, "prefab:Child", {
        MakeArtifact(child, "prefab:Child", ArtifactType::Prefab, "prefab", "Artifacts/Child/prefab.nprefab")
    });

    RuntimeManifestBuilder builder;
    builder.AddArtifactManifest(parentManifest);
    builder.AddArtifactManifest(childManifest);

    const auto result = builder.Build({{parent, "prefab:Parent"}}, "win64");

    ASSERT_FALSE(result.diagnostics.HasErrors());
    EXPECT_NE(RuntimeAssetDatabase(result.manifest).Resolve(parent, "prefab:Parent"), nullptr);
    EXPECT_NE(RuntimeAssetDatabase(result.manifest).Resolve(child, "prefab:Child"), nullptr);
}

TEST(AssetManifestTests, RuntimeManifestBuildSelectsArtifactManifestByTargetPlatform)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Engine::Assets;

    const auto model = MakeAssetId("68686868-6868-4868-8868-686868686868");

    auto winManifest = MakeManifest(model, "prefab:Hero", {
        MakeArtifact(model, "prefab:Hero", ArtifactType::Prefab, "prefab", "Artifacts/Win/Hero.nprefab")
    });
    winManifest.targetPlatform = "win64";
    winManifest.subAssets.front().targetPlatform = "win64";

    auto editorManifest = MakeManifest(model, "prefab:Hero", {
        MakeArtifact(model, "prefab:Hero", ArtifactType::Prefab, "prefab", "Artifacts/Editor/Hero.nprefab")
    });
    editorManifest.targetPlatform = "editor-windows";
    editorManifest.subAssets.front().targetPlatform = "editor-windows";

    RuntimeManifestBuilder builder;
    builder.AddArtifactManifest(winManifest);
    builder.AddArtifactManifest(editorManifest);

    const auto result = builder.Build({{model, "prefab:Hero"}}, "editor-windows");

    ASSERT_FALSE(result.diagnostics.HasErrors());
    const RuntimeAssetDatabase database(result.manifest);
    const auto* entry = database.Resolve(model, "prefab:Hero");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->artifactPath, "Artifacts/Editor/Hero.nprefab");
}

TEST(AssetManifestTests, RuntimeManifestBuildRejectsMissingRootSubAsset)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Engine::Assets;

    const auto model = MakeAssetId("69696969-6969-4969-8969-696969696969");
    auto manifest = MakeManifest(model, "prefab:Hero", {
        MakeArtifact(model, "prefab:Hero", ArtifactType::Prefab, "prefab", "Artifacts/Hero/Hero.nprefab"),
        MakeArtifact(model, "mesh:body", ArtifactType::Mesh, "mesh", "Artifacts/Hero/body.nmesh")
    });

    RuntimeManifestBuilder builder;
    builder.AddArtifactManifest(manifest);

    const auto result = builder.Build({{model, "prefab:Missing"}}, "win64");

    ASSERT_TRUE(result.diagnostics.HasErrors());
    ASSERT_FALSE(result.diagnostics.GetItems().empty());
    EXPECT_EQ(result.diagnostics.GetItems().front().code, "runtime-manifest-missing-root-subasset");
    EXPECT_TRUE(result.manifest.entries.empty());
    EXPECT_TRUE(result.manifest.prefabEntries.empty());
}

TEST(AssetManifestTests, RuntimeManifestBuildRejectsSourceOnlyArtifactPathsAndIncompleteMetadata)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Engine::Assets;

    const auto model = MakeAssetId("70707070-7070-4070-8070-707070707070");
    auto manifest = MakeManifest(model, "prefab:Hero", {
        MakeArtifact(model, "prefab:Hero", ArtifactType::Prefab, "prefab", "Assets/Models/Hero.gltf")
    });

    RuntimeManifestBuilder builder;
    builder.AddArtifactManifest(manifest);

    const auto result = builder.Build({{model, "prefab:Hero"}}, "win64");

    ASSERT_TRUE(result.diagnostics.HasErrors());
    ASSERT_FALSE(result.diagnostics.GetItems().empty());
    EXPECT_EQ(result.diagnostics.GetItems().front().code, "runtime-manifest-source-artifact-path");
    EXPECT_TRUE(result.manifest.entries.empty());
}

TEST(AssetManifestTests, RuntimeManifestBuildRejectsAuthoringPathsAndEmptyArtifactMetadata)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Engine::Assets;

    const auto sourceAsset = MakeAssetId("72727272-7272-4272-8272-727272727272");
    auto authoringPathManifest = MakeManifest(sourceAsset, "material:Hero", {
        MakeArtifact(sourceAsset, "material:Hero", ArtifactType::Material, "material", "Assets/Materials/Hero.mat")
    });

    RuntimeManifestBuilder authoringPathBuilder;
    authoringPathBuilder.AddArtifactManifest(authoringPathManifest);

    const auto authoringPathResult = authoringPathBuilder.Build({{sourceAsset, "material:Hero"}}, "win64");
    ASSERT_TRUE(authoringPathResult.diagnostics.HasErrors());
    ASSERT_FALSE(authoringPathResult.diagnostics.GetItems().empty());
    EXPECT_EQ(authoringPathResult.diagnostics.GetItems().front().code, "runtime-manifest-source-artifact-path");
    EXPECT_TRUE(authoringPathResult.manifest.entries.empty());

    const auto incomplete = MakeAssetId("73737373-7373-4373-8373-737373737373");
    auto incompleteManifest = MakeManifest(incomplete, "material:Hero", {
        MakeArtifact(incomplete, "material:Hero", ArtifactType::Material, "", "Artifacts/Hero/Hero.nmat")
    });
    incompleteManifest.subAssets.front().contentHash.clear();

    RuntimeManifestBuilder incompleteBuilder;
    incompleteBuilder.AddArtifactManifest(incompleteManifest);

    const auto incompleteResult = incompleteBuilder.Build({{incomplete, "material:Hero"}}, "win64");
    ASSERT_TRUE(incompleteResult.diagnostics.HasErrors());
    ASSERT_FALSE(incompleteResult.diagnostics.GetItems().empty());
    EXPECT_EQ(incompleteResult.diagnostics.GetItems().front().code, "runtime-manifest-incomplete-artifact-metadata");
    EXPECT_TRUE(incompleteResult.manifest.entries.empty());
}

TEST(AssetManifestTests, RuntimeManifestBuildRejectsAbsoluteAndTraversalArtifactPaths)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Engine::Assets;

    const auto absoluteAsset = MakeAssetId("75757575-7575-4575-8575-757575757575");
    auto absoluteManifest = MakeManifest(absoluteAsset, "material:Hero", {
        MakeArtifact(
            absoluteAsset,
            "material:Hero",
            ArtifactType::Material,
            "material",
            (std::filesystem::temp_directory_path() / "Hero.nmat").generic_string())
    });

    RuntimeManifestBuilder absoluteBuilder;
    absoluteBuilder.AddArtifactManifest(absoluteManifest);
    const auto absoluteResult = absoluteBuilder.Build({{absoluteAsset, "material:Hero"}}, "win64");
    ASSERT_TRUE(absoluteResult.diagnostics.HasErrors());
    ASSERT_FALSE(absoluteResult.diagnostics.GetItems().empty());
    EXPECT_EQ(absoluteResult.diagnostics.GetItems().front().code, "runtime-manifest-source-artifact-path");

    const auto traversalAsset = MakeAssetId("76767676-7676-4676-8676-767676767676");
    auto traversalManifest = MakeManifest(traversalAsset, "material:Hero", {
        MakeArtifact(
            traversalAsset,
            "material:Hero",
            ArtifactType::Material,
            "material",
            "../Assets/Materials/Hero.nmat")
    });

    RuntimeManifestBuilder traversalBuilder;
    traversalBuilder.AddArtifactManifest(traversalManifest);
    const auto traversalResult = traversalBuilder.Build({{traversalAsset, "material:Hero"}}, "win64");
    ASSERT_TRUE(traversalResult.diagnostics.HasErrors());
    ASSERT_FALSE(traversalResult.diagnostics.GetItems().empty());
    EXPECT_EQ(traversalResult.diagnostics.GetItems().front().code, "runtime-manifest-source-artifact-path");

    const auto driveRelativeAsset = MakeAssetId("77777777-7777-4777-8777-777777777777");
    auto driveRelativeManifest = MakeManifest(driveRelativeAsset, "material:Hero", {
        MakeArtifact(
            driveRelativeAsset,
            "material:Hero",
            ArtifactType::Material,
            "material",
            "C:Artifacts/Hero.nmat")
    });

    RuntimeManifestBuilder driveRelativeBuilder;
    driveRelativeBuilder.AddArtifactManifest(driveRelativeManifest);
    const auto driveRelativeResult = driveRelativeBuilder.Build({{driveRelativeAsset, "material:Hero"}}, "win64");
    ASSERT_TRUE(driveRelativeResult.diagnostics.HasErrors());
    ASSERT_FALSE(driveRelativeResult.diagnostics.GetItems().empty());
    EXPECT_EQ(driveRelativeResult.diagnostics.GetItems().front().code, "runtime-manifest-source-artifact-path");
}

TEST(AssetManifestTests, RuntimeManifestBuildIncludesGeneratedPrefabSiblingArtifacts)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Engine::Assets;

    const auto model = MakeAssetId("74747474-7474-4474-8474-747474747474");
    auto manifest = MakeManifest(model, "prefab:Hero", {
        MakeArtifact(model, "prefab:Hero", ArtifactType::Prefab, "prefab", "Artifacts/Hero/Hero.nprefab"),
        MakeArtifact(model, "mesh:Body", ArtifactType::Mesh, "mesh", "Artifacts/Hero/Body.nmesh"),
        MakeArtifact(model, "material:Body", ArtifactType::Material, "material", "Artifacts/Hero/Body.nmat")
    });

    RuntimeManifestBuilder builder;
    builder.AddArtifactManifest(manifest);

    const auto result = builder.Build({{model, "prefab:Hero"}}, "win64");

    ASSERT_FALSE(result.diagnostics.HasErrors());
    const RuntimeAssetDatabase database(result.manifest);
    EXPECT_NE(database.Resolve(model, "prefab:Hero"), nullptr);
    EXPECT_NE(database.Resolve(model, "mesh:Body"), nullptr);
    EXPECT_NE(database.Resolve(model, "material:Body"), nullptr);
}

TEST(AssetManifestTests, RuntimeManifestBuildRejectsArtifactTargetPlatformMismatch)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Engine::Assets;

    const auto model = MakeAssetId("71717171-7171-4171-8171-717171717171");
    auto manifest = MakeManifest(model, "prefab:Hero", {
        MakeArtifact(model, "prefab:Hero", ArtifactType::Prefab, "prefab", "Artifacts/Win/Hero.nprefab")
    });
    manifest.subAssets.front().targetPlatform = "editor-windows";

    RuntimeManifestBuilder builder;
    builder.AddArtifactManifest(manifest);

    const auto result = builder.Build({{model, "prefab:Hero"}}, "win64");

    ASSERT_TRUE(result.diagnostics.HasErrors());
    ASSERT_FALSE(result.diagnostics.GetItems().empty());
    EXPECT_EQ(result.diagnostics.GetItems().front().code, "runtime-manifest-artifact-platform-mismatch");
    EXPECT_TRUE(result.manifest.entries.empty());
}

TEST(AssetManifestTests, RuntimeResolverRejectsSourceOnlyPaths)
{
    using namespace NLS::Engine::Assets;

    EXPECT_FALSE(IsRuntimePackagedAssetPath("Assets/Models/Hero.gltf"));
    EXPECT_FALSE(IsRuntimePackagedAssetPath("Assets/Models/Hero.glb"));
    EXPECT_FALSE(IsRuntimePackagedAssetPath("Assets/Models/Hero.fbx"));
    EXPECT_FALSE(IsRuntimePackagedAssetPath("Assets/Models/Hero.obj"));
    EXPECT_FALSE(IsRuntimePackagedAssetPath("Assets/Models/Hero.gltf.meta"));
    EXPECT_FALSE(IsRuntimePackagedAssetPath("Library/SourceAssetDatabase.json"));
    EXPECT_TRUE(IsRuntimePackagedAssetPath("Artifacts/Hero/body.nmesh"));
}

TEST(AssetManifestTests, SceneMaterialBindingsRequireObjectReferenceIdentity)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Engine::Assets;
    using namespace NLS::Engine::Rendering;
    using namespace NLS::Engine::Serialize;

    const auto material = MakeAssetId("56565656-5656-4656-8656-565656565656");

    RuntimeAssetManifest manifest;
    manifest.entries.push_back({
        material,
        "material:logo",
        ArtifactType::Material,
        "material",
        "Artifacts/UI/logo.nmat",
        "sha256:logo",
        {}
    });

    RuntimeAssetDatabase database(manifest);

    PrefabArtifact prefab;
    ObjectRecord meshRenderer;
    meshRenderer.id = ObjectId(NLS::Guid::Parse("57575757-5757-4757-8757-575757575757"));
    meshRenderer.localIdentifierInFile = MakeLocalIdentifierInFile(meshRenderer.id);
    meshRenderer.typeName = "NLS::Engine::Components::MeshRenderer";
    meshRenderer.debugName = "String MeshRenderer";
    meshRenderer.properties.push_back({"materials", PropertyValue::Array({
        PropertyValue::String("Materials/Logo.mat")
    })});
    prefab.graph.objects.push_back(std::move(meshRenderer));

    const auto bindings = ResolveSceneRendererMaterialBindings(prefab, database);
    ASSERT_EQ(bindings.size(), 1u);
    EXPECT_FALSE(bindings.front().reference.assetId.IsValid());
    EXPECT_TRUE(bindings.front().reference.subAssetKey.empty());
    EXPECT_FALSE(bindings.front().resolved);
    EXPECT_TRUE(bindings.front().artifactPath.empty());
}
