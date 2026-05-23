#include <gtest/gtest.h>

#include "Assets/AssetId.h"
#include "Assets/AssetResolver.h"
#include "Assets/AssetVersion.h"
#include "Assets/ImportDiagnostics.h"
#include "Assets/PrefabEditorWorkflow.h"
#include "GameObject.h"
#include "Guid.h"
#include "SceneSystem/Scene.h"

namespace
{
NLS::Core::Assets::AssetId MakeAssetId(const char* guid)
{
    return NLS::Core::Assets::AssetId(NLS::Guid::Parse(guid));
}

bool ContainsAsset(
    const std::vector<NLS::Core::Assets::AssetId>& assets,
    NLS::Core::Assets::AssetId asset)
{
    return std::find(assets.begin(), assets.end(), asset) != assets.end();
}
}

TEST(AssetDependencyPipelineTests, VersionKeyChangesWhenSourceImportSettingsOrDependenciesChange)
{
    using namespace NLS::Core::Assets;

    AssetVersion baseline;
    baseline.sourceContentHash = "sha256:model";
    baseline.metaHash = "sha256:settings";
    baseline.dependencyHash = "sha256:deps";
    baseline.importerVersion = 1u;
    baseline.postprocessorVersion = 2u;
    baseline.targetPlatform = "editor-windows";
    baseline.artifactHash = "sha256:artifact";

    AssetVersion changedDependency = baseline;
    changedDependency.dependencyHash = "sha256:deps-updated";

    AssetVersion changedTarget = baseline;
    changedTarget.targetPlatform = "win64";

    EXPECT_EQ(baseline.MakeCacheKey(), baseline.MakeCacheKey());
    EXPECT_NE(baseline.MakeCacheKey(), changedDependency.MakeCacheKey());
    EXPECT_NE(baseline.MakeCacheKey(), changedTarget.MakeCacheKey());
}

TEST(AssetDependencyPipelineTests, DependencyGraphMarksOnlyDependentAssetsStale)
{
    using namespace NLS::Core::Assets;

    const auto texture = MakeAssetId("11111111-1111-4111-8111-111111111111");
    const auto material = MakeAssetId("22222222-2222-4222-8222-222222222222");
    const auto model = MakeAssetId("33333333-3333-4333-8333-333333333333");
    const auto prefab = MakeAssetId("44444444-4444-4444-8444-444444444444");
    const auto unrelated = MakeAssetId("55555555-5555-4555-8555-555555555555");

    AssetDependencyGraph graph;
    graph.AddDependency(material, {AssetDependencyKind::SourceAssetGuid, texture.ToString(), ""});
    graph.AddDependency(model, {AssetDependencyKind::ImportedArtifact, material.ToString(), "material:body"});
    graph.AddDependency(prefab, {AssetDependencyKind::PrefabBase, model.ToString(), "prefab:HeroScene"});
    graph.AddAsset(unrelated);

    const auto stale = graph.CollectDependents(texture);

    EXPECT_TRUE(ContainsAsset(stale, material));
    EXPECT_TRUE(ContainsAsset(stale, model));
    EXPECT_TRUE(ContainsAsset(stale, prefab));
    EXPECT_FALSE(ContainsAsset(stale, texture));
    EXPECT_FALSE(ContainsAsset(stale, unrelated));
}

TEST(AssetDependencyPipelineTests, DependencyGraphCanCollectDependentsByEveryDependencyKind)
{
    using namespace NLS::Core::Assets;

    const auto model = MakeAssetId("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    const auto prefab = MakeAssetId("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
    const auto scene = MakeAssetId("cccccccc-cccc-4ccc-8ccc-cccccccccccc");

    AssetDependencyGraph graph;
    graph.AddDependency(model, {AssetDependencyKind::SourceFileHash, "Assets/Models/Hero.gltf", "sha256:model"});
    graph.AddDependency(model, {AssetDependencyKind::PathToGuidMapping, "Assets/Textures/Hero.png", "guid:texture"});
    graph.AddDependency(model, {AssetDependencyKind::BuildTarget, "win64", ""});
    graph.AddDependency(model, {AssetDependencyKind::ImporterVersion, "gltf-scene", "2"});
    graph.AddDependency(model, {AssetDependencyKind::PostprocessorVersion, "model-postprocessor", "5"});
    graph.AddDependency(prefab, {AssetDependencyKind::PrefabBase, model.ToString(), "prefab:HeroScene"});
    graph.AddDependency(scene, {AssetDependencyKind::PrefabOverrideTarget, prefab.ToString(), "object:Body"});
    graph.AddDependency(scene, {AssetDependencyKind::NestedPrefab, prefab.ToString(), "prefab:Nested"});

    EXPECT_TRUE(ContainsAsset(
        graph.CollectDependents(AssetDependencyKind::SourceFileHash, "Assets/Models/Hero.gltf"),
        model));
    EXPECT_TRUE(ContainsAsset(
        graph.CollectDependents(AssetDependencyKind::PathToGuidMapping, "Assets/Textures/Hero.png"),
        model));
    EXPECT_TRUE(ContainsAsset(
        graph.CollectDependents(AssetDependencyKind::BuildTarget, "win64"),
        model));
    EXPECT_TRUE(ContainsAsset(
        graph.CollectDependents(AssetDependencyKind::ImporterVersion, "gltf-scene"),
        model));
    EXPECT_TRUE(ContainsAsset(
        graph.CollectDependents(AssetDependencyKind::PostprocessorVersion, "model-postprocessor"),
        model));

    const auto prefabStale = graph.CollectDependents(AssetDependencyKind::PrefabBase, model.ToString());
    EXPECT_TRUE(ContainsAsset(prefabStale, prefab));
    EXPECT_TRUE(ContainsAsset(prefabStale, scene));

    const auto sceneStale = graph.CollectDependents(AssetDependencyKind::PrefabOverrideTarget, prefab.ToString());
    EXPECT_TRUE(ContainsAsset(sceneStale, scene));

    const auto nestedStale = graph.CollectDependents(AssetDependencyKind::NestedPrefab, prefab.ToString());
    EXPECT_TRUE(ContainsAsset(nestedStale, scene));
}

TEST(AssetDependencyPipelineTests, ResolverKeepsPreviousSuccessfulArtifactAfterFailedImport)
{
    using namespace NLS::Core::Assets;

    const auto model = MakeAssetId("66666666-6666-4666-8666-666666666666");

    AssetResolver resolver;

    ArtifactManifest successful;
    successful.sourceAssetId = model;
    successful.targetPlatform = "editor-windows";
    successful.primarySubAssetKey = "prefab:Hero";
    successful.subAssets.push_back({
        model,
        "prefab:Hero",
        ArtifactType::Prefab,
        "prefab",
        "editor-windows",
        "Library/Artifacts/Hero/prefab.nprefab",
        "sha256:ok"
    });

    resolver.CommitSuccessfulImport(successful);
    ASSERT_EQ(resolver.GetState(model), AssetResolverState::UpToDate);
    ASSERT_NE(resolver.GetCommittedManifest(model), nullptr);

    ImportDiagnosticList diagnostics;
    diagnostics.Add({ImportDiagnosticSeverity::Error, "missing-texture", model, "", "Assets/Textures/Missing.png", true});
    resolver.CommitFailedImport(model, diagnostics);

    EXPECT_EQ(resolver.GetState(model), AssetResolverState::Failed);
    const auto* retained = resolver.GetCommittedManifest(model);
    ASSERT_NE(retained, nullptr);
    ASSERT_NE(retained->FindPrimaryArtifact(), nullptr);
    EXPECT_EQ(retained->FindPrimaryArtifact()->artifactPath, "Library/Artifacts/Hero/prefab.nprefab");
    ASSERT_NE(resolver.GetDiagnostics(model), nullptr);
    EXPECT_TRUE(resolver.GetDiagnostics(model)->HasErrors());
}

TEST(AssetDependencyPipelineTests, HotReloadPolicyClassifiesLoadedAssetRefreshSafety)
{
    using namespace NLS::Core::Assets;

    EXPECT_EQ(GetHotReloadPolicy(ArtifactType::Mesh), AssetHotReloadPolicy::ReloadInPlace);
    EXPECT_EQ(GetHotReloadPolicy(ArtifactType::Material), AssetHotReloadPolicy::ReloadInPlace);
    EXPECT_EQ(GetHotReloadPolicy(ArtifactType::Texture), AssetHotReloadPolicy::ReloadInPlace);
    EXPECT_EQ(GetHotReloadPolicy(ArtifactType::Prefab), AssetHotReloadPolicy::MarkInstancesDirty);
    EXPECT_EQ(GetHotReloadPolicy(ArtifactType::Scene), AssetHotReloadPolicy::RequiresExplicitReload);
}

TEST(AssetDependencyPipelineTests, PrefabWorkflowChangesMarkDependentVariantsScenesAndBuildsStale)
{
    using namespace NLS::Core::Assets;

    const auto basePrefab = MakeAssetId("01010101-0101-4101-8101-010101010101");
    const auto variantPrefab = MakeAssetId("02020202-0202-4202-8202-020202020202");
    const auto nestedConsumerPrefab = MakeAssetId("03030303-0303-4303-8303-030303030303");
    const auto scene = MakeAssetId("04040404-0404-4404-8404-040404040404");
    const auto buildManifest = MakeAssetId("05050505-0505-4505-8505-050505050505");
    const auto unrelated = MakeAssetId("06060606-0606-4606-8606-060606060606");

    AssetDependencyGraph graph;
    graph.AddDependency(variantPrefab, MakePrefabBaseDependency(basePrefab, "prefab:Base"));
    graph.AddDependency(nestedConsumerPrefab, MakeNestedPrefabDependency(basePrefab, "prefab:NestedBase"));
    graph.AddDependency(scene, MakePrefabOverrideTargetDependency(variantPrefab, "prefab:Variant"));
    graph.AddDependency(buildManifest, {AssetDependencyKind::SourceAssetGuid, scene.ToString(), "manifest:Main"});
    graph.AddAsset(unrelated);

    const auto baseEditStale = graph.CollectPrefabDependents(basePrefab);

    EXPECT_TRUE(ContainsAsset(baseEditStale, variantPrefab));
    EXPECT_TRUE(ContainsAsset(baseEditStale, nestedConsumerPrefab));
    EXPECT_TRUE(ContainsAsset(baseEditStale, scene));
    EXPECT_TRUE(ContainsAsset(baseEditStale, buildManifest));
    EXPECT_FALSE(ContainsAsset(baseEditStale, basePrefab));
    EXPECT_FALSE(ContainsAsset(baseEditStale, unrelated));
}

TEST(AssetDependencyPipelineTests, PrefabWorkflowOperationsEmitDependencyChangesAndRefreshRequests)
{
    using namespace NLS::Core::Assets;
    using NLS::Editor::Assets::PrefabEditorOperationStatus;
    using NLS::Editor::Assets::PrefabEditorWorkflow;

    const auto basePrefab = MakeAssetId("07070707-0707-4707-8707-070707070707");
    const auto variantPrefab = MakeAssetId("08080808-0808-4808-8808-080808080808");
    const auto scene = MakeAssetId("09090909-0909-4909-8909-090909090909");
    const auto buildManifest = MakeAssetId("0a0a0a0a-0a0a-4a0a-8a0a-0a0a0a0a0a0a");

    NLS::Engine::GameObject root("Base", "Prefab");
    PrefabEditorWorkflow workflow;
    auto created = workflow.CreatePrefabFromSelection({
        &root,
        {},
        basePrefab,
        "Assets/Prefabs/Base.prefab"
    });
    ASSERT_EQ(created.status, PrefabEditorOperationStatus::Committed);
    ASSERT_TRUE(created.artifact.has_value());

    auto variant = workflow.CreateEditableVariant({
        &*created.artifact,
        basePrefab,
        "prefab:Base",
        "Assets/Prefabs/BaseVariant.prefab",
        variantPrefab,
        false,
        false
    });
    ASSERT_EQ(variant.status, PrefabEditorOperationStatus::Committed);
    ASSERT_EQ(variant.dependencyChanges.size(), 1u);
    EXPECT_EQ(variant.dependencyChanges.front().owner, variantPrefab);
    EXPECT_EQ(variant.dependencyChanges.front().dependency.kind, AssetDependencyKind::PrefabBase);
    EXPECT_EQ(variant.dependencyChanges.front().dependency.value, basePrefab.ToString());

    AssetDependencyGraph graph;
    graph.ApplyDependencyChanges(variant.dependencyChanges);
    graph.AddDependency(buildManifest, {AssetDependencyKind::SourceAssetGuid, scene.ToString(), "manifest:Main"});

    NLS::Engine::SceneSystem::Scene sceneObject;
    auto instantiate = workflow.InstantiatePrefab({
        &*variant.artifact,
        variantPrefab,
        "prefab:BaseVariant",
        scene
    }, sceneObject);
    ASSERT_EQ(instantiate.status, PrefabEditorOperationStatus::Committed);
    ASSERT_TRUE(instantiate.instance.has_value());
    graph.ApplyDependencyChanges(instantiate.dependencyChanges);

    instantiate.instance->localPatches.push_back(
        NLS::Engine::Serialize::PatchOperation::ReplaceProperty(
            created.artifact->graph.root,
            "name",
            NLS::Engine::Serialize::PropertyValue::String("LocalName")));
    const auto revert = workflow.RevertAllOverrides(*instantiate.instance);
    ASSERT_EQ(revert.status, PrefabEditorOperationStatus::Committed);

    const auto sceneStale = graph.CollectDependents(revert.dependencyRefreshRequests);
    EXPECT_TRUE(ContainsAsset(sceneStale, buildManifest));
    EXPECT_FALSE(ContainsAsset(sceneStale, variantPrefab));

    auto unpack = workflow.UnpackPrefabInstance(*instantiate.instance);
    ASSERT_EQ(unpack.status, PrefabEditorOperationStatus::Committed);
    ASSERT_EQ(unpack.dependencyChanges.size(), 1u);
    EXPECT_EQ(unpack.dependencyChanges.front().change, AssetDependencyChangeKind::Remove);
    EXPECT_EQ(unpack.dependencyChanges.front().owner, scene);
    EXPECT_EQ(unpack.dependencyChanges.front().dependency.kind, AssetDependencyKind::PrefabOverrideTarget);

    graph.ApplyDependencyChanges(unpack.dependencyChanges);
    const auto variantEditAfterUnpack = graph.CollectPrefabDependents(variantPrefab);
    EXPECT_FALSE(ContainsAsset(variantEditAfterUnpack, scene));
    EXPECT_FALSE(ContainsAsset(variantEditAfterUnpack, buildManifest));

    NLS::Editor::Assets::PrefabOverrideRecord overrideRecord;
    overrideRecord.sourceObject = created.artifact->graph.root;
    overrideRecord.patch = NLS::Engine::Serialize::PatchOperation::ReplaceProperty(
        created.artifact->graph.root,
        "name",
        NLS::Engine::Serialize::PropertyValue::String("AppliedName"));
    const auto apply = workflow.ApplySelectedOverride(*created.artifact, overrideRecord);
    ASSERT_EQ(apply.status, PrefabEditorOperationStatus::Committed);

    const auto baseEditStale = graph.CollectDependents(apply.dependencyRefreshRequests);
    EXPECT_TRUE(ContainsAsset(baseEditStale, variantPrefab));
    EXPECT_FALSE(ContainsAsset(baseEditStale, scene));
}
