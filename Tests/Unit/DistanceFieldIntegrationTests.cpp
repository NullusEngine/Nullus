#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include "Assets/AssetMeta.h"
#include "Assets/ArtifactDatabase.h"
#include "Assets/ArtifactWriter.h"
#include "Assets/ContentSource.h"
#include "Assets/NpakArchive.h"
#include "Assets/ExternalAssetImporter.h"
#include "Assets/AssetImporterFacade.h"
#include "Assets/AssetDatabaseFacade.h"
#include "Assets/EditorAssetDragDropBridge.h"
#include "Assets/DistanceFieldSceneBuild.h"
#include "Project/Editor/Assets/DistanceFieldBuildService.h"
#include "Project/Editor/Core/Context.h"
#include "Inspector/AssetTargetAdapter.h"
#include "Inspector/AssetImporterEditing.h"
#include "Serialization/ImporterDraftSerializedBackend.h"
#include "Serialization/SerializedObject.h"
#include "Undo/EditorUndoService.h"
#include "ServiceLocator.h"
#include "Rendering/Assets/DistanceFieldBuildService.h"
#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "SceneSystem/Scene.h"
#include "GameObject.h"
#include "CookTypes.h"
#include "Engine/Assets/RuntimeAssetDatabase.h"
#include "Rendering/Assets/DistanceFieldArtifact.h"
#include "Rendering/Assets/DistanceFieldBuilder.h"

namespace
{
    namespace assets=NLS::Core::Assets;
    namespace df=NLS::Render::DistanceField;
    struct Fixture
    {
        std::filesystem::path root,source,artifacts,database;
        assets::AssetMeta meta;
        explicit Fixture(const char* name)
        {
            root=std::filesystem::current_path()/"build/distance-field-integration"/(std::string(name)+"-"+NLS::Guid::New().ToString());
            source=root/"Assets/Cube.obj"; artifacts=root/"Library/Artifacts"; database=root/"Library/ArtifactDB";
            std::filesystem::create_directories(source.parent_path());
            std::ofstream file(source);
            file << "v -1 -1 -1\nv 1 -1 -1\nv -1 1 -1\nv 1 1 -1\nv -1 -1 1\nv 1 -1 1\nv -1 1 1\nv 1 1 1\n"
                "f 1 3 4 2\nf 5 6 8 7\nf 1 5 7 3\nf 2 4 8 6\nf 1 2 6 5\nf 3 7 8 4\n";
            file.close(); meta=assets::AssetMeta::CreateForAsset(source);
            meta.settings["DISTANCE_FIELD_GENERATION"]="enabled";
            meta.settings["DISTANCE_FIELD_RESOLUTION_SCALE"]="0.125";
            meta.Save(source.string()+".meta");
        }
        NLS::Cook::CookOptions Options() const
        {
            NLS::Cook::CookOptions options; options.projectRoot=root; options.assetsRoot=root/"Assets";
            options.artifactDatabasePath=database; options.artifactRoot=artifacts; options.stagingRoot=root/"Library/Staging";
            options.target.artifactPlatform="win64-dx12"; return options;
        }
        std::optional<assets::ArtifactManifest> Manifest() const
        {
            assets::ArtifactDatabase db;
            if(!db.Load(database)) return {};
            return db.BuildManifestForSource(meta.id,"win64-dx12");
        }
        std::filesystem::path Resolve(const std::string& path) const
        {
            if(std::filesystem::path(path).is_absolute()) return path;
            if(std::filesystem::is_regular_file(root/path)) return root/path;
            return artifacts/path;
        }
    };
    const assets::ImportedArtifact* MainField(const assets::ArtifactManifest& manifest)
    {
        for(const auto& artifact:manifest.subAssets)
            if(artifact.artifactType==assets::ArtifactType::MeshDistanceField && artifact.loaderId=="mesh-distance-field") return &artifact;
        return nullptr;
    }
}

TEST(DistanceFieldIntegrationTests, EditorAndCookUseTheSameFieldBuilder)
{
    Fixture fixture("parity");
    const auto cooked=NLS::Cook::AssetCooker::CookAssets(fixture.Options(),{fixture.source});
    ASSERT_TRUE(cooked.Succeeded()) << cooked.FormatSummary();
    const auto manifest=fixture.Manifest(); ASSERT_TRUE(manifest);
    const auto* field=MainField(*manifest); ASSERT_TRUE(field);
    const auto expected=df::LoadDistanceFieldArtifact(fixture.Resolve(field->artifactPath)); ASSERT_TRUE(expected);
    NLS::Editor::Assets::ExternalModelImportRequest request;
    request.sourcePath=fixture.source; request.meta=fixture.meta; request.sceneKey="Cube";
    request.projectRoot=fixture.root; request.stagingRoot=fixture.root/"Library/EditorStaging"; request.committedRoot=fixture.root/"Library/EditorArtifacts";
    request.prepareOnly=true; request.allowAutoImportMissingTextureFiles=false;
    const auto imported=NLS::Editor::Assets::ImportExternalModelAsset(request);
    ASSERT_TRUE(imported.prepared) << "Editor model preparation failed";
    ASSERT_TRUE(imported.preparedWriteRequest);
    bool found=false;
    for(const auto& payload:imported.preparedWriteRequest->artifacts)
        if(payload.artifactType==assets::ArtifactType::MeshDistanceField && payload.loaderId=="mesh-distance-field")
        {
            auto actual=df::DeserializeDistanceFieldArtifact(payload.payload); ASSERT_TRUE(actual);
            EXPECT_EQ(actual->buildIdentity,expected->buildIdentity);
            EXPECT_EQ(actual->mips[2].bricks,expected->mips[2].bricks);
            found=true;
        }
    EXPECT_TRUE(found);
}

TEST(DistanceFieldIntegrationTests, ProjectSettingInvalidatesCookAndFailedBakePreservesPublishedData)
{
    Fixture fixture("cache");
    fixture.meta.settings["DISTANCE_FIELD_GENERATION"]="inherit"; ASSERT_TRUE(fixture.meta.Save(fixture.source.string()+".meta"));
    { std::ofstream project(fixture.root/"Test.nullus"); project << "generate_mesh_distance_fields=false\n"; }
    auto options=fixture.Options();
    ASSERT_TRUE(NLS::Cook::AssetCooker::CookAssets(options,{fixture.source}).Succeeded());
    auto before=fixture.Manifest(); ASSERT_TRUE(before); EXPECT_FALSE(MainField(*before));
    EXPECT_EQ(NLS::Cook::AssetCooker::CookAssets(options,{fixture.source}).skipped,1u);
    { std::ofstream project(fixture.root/"Test.nullus"); project << "generate_mesh_distance_fields=true\n"; }
    const auto enabled=NLS::Cook::AssetCooker::CookAssets(options,{fixture.source});
    ASSERT_TRUE(enabled.Succeeded()) << enabled.FormatSummary(); EXPECT_EQ(enabled.cooked,1u);
    auto published=fixture.Manifest(); ASSERT_TRUE(published); ASSERT_TRUE(MainField(*published));
    const auto oldHash=MainField(*published)->contentHash;
    fixture.meta.settings["DISTANCE_FIELD_RESOLUTION_SCALE"]="nan"; ASSERT_TRUE(fixture.meta.Save(fixture.source.string()+".meta"));
    const auto failed=NLS::Cook::AssetCooker::CookAssets(options,{fixture.source});
    EXPECT_FALSE(failed.Succeeded());
    auto retained=fixture.Manifest(); ASSERT_TRUE(retained); ASSERT_TRUE(MainField(*retained));
    EXPECT_EQ(MainField(*retained)->contentHash,oldHash);
    EXPECT_TRUE(std::filesystem::is_regular_file(fixture.Resolve(MainField(*retained)->artifactPath)));
}

TEST(DistanceFieldIntegrationTests, EditorReimportPreservesNewDistanceFieldSettings)
{
    Fixture fixture("reimport");
    const auto roots=NLS::Editor::Assets::MakeProjectEditorAssetRoots(fixture.root);
    NLS::Editor::Assets::AssetImporterFacade importer(roots);
    const auto reimport=[&]()
    {
        const bool imported=importer.SaveAndReimport("Assets/Cube.obj");
        for(const auto& diagnostic:importer.GetDiagnostics())
            if(diagnostic.severity==assets::AssetDiagnosticSeverity::Error)
                ADD_FAILURE() << diagnostic.code << ": " << diagnostic.message;
        return imported;
    };
    ASSERT_TRUE(reimport());
    NLS::Editor::Assets::AssetDatabaseFacade before(roots); ASSERT_TRUE(before.Refresh());
    const auto original=before.GetArtifactManifestForAssetPath("Assets/Cube.obj"); ASSERT_TRUE(original); ASSERT_TRUE(MainField(*original));
    fixture.meta.settings["DISTANCE_FIELD_RESOLUTION_SCALE"]="0.25";
    fixture.meta.settings["DISTANCE_FIELD_TWO_SIDED"]="true";
    ASSERT_TRUE(fixture.meta.Save(fixture.source.string()+".meta"));
    ASSERT_TRUE(reimport());
    const auto persisted=assets::AssetMeta::Load(fixture.source.string()+".meta"); ASSERT_TRUE(persisted);
    EXPECT_EQ(persisted->settings.at("DISTANCE_FIELD_RESOLUTION_SCALE"),"0.25");
    EXPECT_EQ(persisted->settings.at("DISTANCE_FIELD_TWO_SIDED"),"true");
    NLS::Editor::Assets::AssetDatabaseFacade after(roots); ASSERT_TRUE(after.Refresh());
    const auto updated=after.GetArtifactManifestForAssetPath("Assets/Cube.obj"); ASSERT_TRUE(updated); ASSERT_TRUE(MainField(*updated));
    EXPECT_NE(MainField(*updated)->contentHash,MainField(*original)->contentHash);
}

TEST(DistanceFieldIntegrationTests, SceneBuildResolvesUnloadedInstancesAndBuildsFieldsAfterProjectEnable)
{
    Fixture fixture("scene-build");
    fixture.meta.settings["DISTANCE_FIELD_GENERATION"] = "inherit";
    ASSERT_TRUE(fixture.meta.Save(fixture.source.string() + ".meta"));
    { std::ofstream project(fixture.root / "Test.nullus"); project << "generate_mesh_distance_fields=false\n"; }
    const auto roots = NLS::Editor::Assets::MakeProjectEditorAssetRoots(fixture.root);
    NLS::Editor::Assets::AssetImporterFacade importer(roots);
    ASSERT_TRUE(importer.SaveAndReimport("Assets/Cube.obj"));
    NLS::Editor::Assets::AssetDatabaseFacade database(roots);
    ASSERT_TRUE(database.Refresh());
    const auto before = database.GetArtifactManifestForAssetPath("Assets/Cube.obj");
    ASSERT_TRUE(before); EXPECT_FALSE(MainField(*before));

    NLS::Engine::SceneSystem::Scene scene;
    const auto add = [&](const char* name)
    {
        auto& object = scene.CreateGameObject(name);
        auto* filter = object.AddComponent<NLS::Engine::Components::MeshFilter>();
        object.AddComponent<NLS::Engine::Components::MeshRenderer>();
        filter->SetDeferredMeshObjectIdentifierHint(NLS::Engine::Serialize::ObjectIdentifier::Asset(
            NLS::Engine::Serialize::AssetId(fixture.meta.id.GetGuid()), 123, "mesh:Cube"));
        return &object;
    };
    auto* first = add("First");
    auto* second = add("Second");
    EXPECT_EQ(NLS::Editor::Assets::CollectSceneDistanceFieldModelAssets(scene, database),
        (std::vector<std::string>{"Assets/Cube.obj"}));
    first->GetComponent<NLS::Engine::Components::MeshRenderer>()->SetAffectDistanceField(false);
    second->GetComponent<NLS::Engine::Components::MeshRenderer>()->SetAffectDistanceField(false);
    EXPECT_TRUE(NLS::Editor::Assets::CollectSceneDistanceFieldModelAssets(scene, database).empty());
    second->GetComponent<NLS::Engine::Components::MeshRenderer>()->SetAffectDistanceField(true);

    { std::ofstream project(fixture.root / "Test.nullus"); project << "generate_mesh_distance_fields=true\n"; }
    const auto paths = NLS::Editor::Assets::CollectSceneDistanceFieldModelAssets(scene, database);
    ASSERT_EQ(paths.size(), 1u);
    ASSERT_TRUE(importer.SaveAndReimport(paths.front()));
    NLS::Editor::Assets::AssetDatabaseFacade rebuilt(roots);
    ASSERT_TRUE(rebuilt.Refresh());
    const auto after = rebuilt.GetArtifactManifestForAssetPath(paths.front());
    ASSERT_TRUE(after); ASSERT_TRUE(MainField(*after));
    for (const auto& artifact : after->subAssets)
        if (artifact.artifactType == assets::ArtifactType::Mesh)
            EXPECT_TRUE(df::FindMeshDistanceFieldSource(
                rebuilt.ResolveArtifactPathAtPath(paths.front(), artifact.subAssetKey).generic_string()));
}

TEST(DistanceFieldIntegrationTests, IndependentBuildReusesPublishedGeometryAndOnlyPatchesDistanceFields)
{
    Fixture fixture("independent");fixture.meta.settings["DISTANCE_FIELD_GENERATION"]="inherit";
    ASSERT_TRUE(fixture.meta.Save(fixture.source.string()+".meta"));
    {std::ofstream project(fixture.root/"Test.nullus");project<<"generate_mesh_distance_fields=false\n";}
    const auto roots=NLS::Editor::Assets::MakeProjectEditorAssetRoots(fixture.root);
    NLS::Editor::Assets::AssetImporterFacade importer(roots);ASSERT_TRUE(importer.SaveAndReimport("Assets/Cube.obj"));
    NLS::Editor::Assets::AssetDatabaseFacade before(roots);ASSERT_TRUE(before.Refresh());
    const auto original=before.GetArtifactManifestForAssetPath("Assets/Cube.obj");ASSERT_TRUE(original);
    ASSERT_TRUE(original->FindSubAsset(df::DistanceFieldSourceKey));EXPECT_FALSE(MainField(*original));
    std::map<std::string,std::string> originalHashes;
    for(const auto& artifact:original->subAssets)originalHashes[artifact.subAssetKey]=artifact.contentHash;
    {std::ofstream project(fixture.root/"Test.nullus");project<<"generate_mesh_distance_fields=true\n";}
#if defined(NLS_ENABLE_TEST_HOOKS)
    NLS::Editor::Assets::ResetAssetDatabaseFullSourceRefreshCountForTesting();
#endif
    NLS::Editor::Assets::DistanceFieldBuildRequest request;request.projectRoot=fixture.root;request.assetPath="Assets/Cube.obj";request.mipMask=4;
    const auto coarse=NLS::Editor::Assets::DistanceFieldBuildService::Build(request);
    ASSERT_TRUE(coarse.success)<<coarse.error;EXPECT_EQ(coarse.legacySourceReads,0u);EXPECT_EQ(coarse.model.stats.builtMips,1u);
#if defined(NLS_ENABLE_TEST_HOOKS)
    EXPECT_EQ(NLS::Editor::Assets::GetAssetDatabaseFullSourceRefreshCountForTesting(),0u);
#endif
    const auto current=[&]()
    {NLS::Editor::Assets::AssetDatabaseFacade database(roots);EXPECT_TRUE(database.RefreshKnownSourceAssets(std::array{fixture.source}));return database.GetArtifactManifestForAssetPath("Assets/Cube.obj");};
    auto published=current();ASSERT_TRUE(published);ASSERT_TRUE(MainField(*published));
    for(const auto& [key,hash]:originalHashes){const auto* artifact=published->FindSubAsset(key);ASSERT_TRUE(artifact);EXPECT_EQ(artifact->contentHash,hash);}
    // A source parser would reject this; derived builds use immutable LOD0 artifacts.
    {std::ofstream source(fixture.source);source<<"not a valid model";}
    request.mipMask=7;
    const auto details=NLS::Editor::Assets::DistanceFieldBuildService::Build(request);
    ASSERT_TRUE(details.success)<<details.error;EXPECT_EQ(details.legacySourceReads,0u);EXPECT_EQ(details.model.stats.builtMips,2u);
    const auto warm=NLS::Editor::Assets::DistanceFieldBuildService::Build(request);
    ASSERT_TRUE(warm.success)<<warm.error;EXPECT_EQ(warm.model.stats.builtMips,0u);EXPECT_EQ(warm.model.meshLoads,0u);
    EXPECT_EQ(warm.model.stats.bvhBuilds,0u);EXPECT_EQ(warm.model.stats.signRays,0u);EXPECT_EQ(warm.publications,0u);
    fixture.meta.settings["DISTANCE_FIELD_RESOLUTION_SCALE"]="0.25";ASSERT_TRUE(fixture.meta.Save(fixture.source.string()+".meta"));
    const auto changed=NLS::Editor::Assets::DistanceFieldBuildService::Build(request);
    ASSERT_TRUE(changed.success)<<changed.error;EXPECT_EQ(changed.legacySourceReads,0u);
    published=current();ASSERT_TRUE(published);
    for(const auto& [key,hash]:originalHashes){const auto* artifact=published->FindSubAsset(key);ASSERT_TRUE(artifact);EXPECT_EQ(artifact->contentHash,hash);}
}

TEST(DistanceFieldIntegrationTests, SupersededDistanceFieldSettingsCannotPublishOldResults)
{
    Fixture fixture("obsolete");
    const auto roots=NLS::Editor::Assets::MakeProjectEditorAssetRoots(fixture.root);
    NLS::Editor::Assets::AssetImporterFacade importer(roots);ASSERT_TRUE(importer.SaveAndReimport("Assets/Cube.obj"));
    NLS::Editor::Assets::AssetDatabaseFacade database(roots);ASSERT_TRUE(database.Refresh());
    const auto before=database.GetArtifactManifestForAssetPath("Assets/Cube.obj");ASSERT_TRUE(before);ASSERT_TRUE(MainField(*before));
    const auto oldHash=MainField(*before)->contentHash;
    fixture.meta.settings["DISTANCE_FIELD_RESOLUTION_SCALE"]="0.25";ASSERT_TRUE(fixture.meta.Save(fixture.source.string()+".meta"));
    NLS::Editor::Assets::DistanceFieldBuildRequest request;request.projectRoot=fixture.root;request.assetPath="Assets/Cube.obj";
    bool superseded=false;
    const auto result=NLS::Editor::Assets::DistanceFieldBuildService::Build(request,nullptr,[&](const auto&)
    {
        if(!superseded){superseded=true;fixture.meta.settings["DISTANCE_FIELD_RESOLUTION_SCALE"]="0.5";EXPECT_TRUE(fixture.meta.Save(fixture.source.string()+".meta"));}
    });
    EXPECT_FALSE(result.success);EXPECT_TRUE(superseded);
    NLS::Editor::Assets::AssetDatabaseFacade after(roots);ASSERT_TRUE(after.Refresh());
    const auto unchanged=after.GetArtifactManifestForAssetPath("Assets/Cube.obj");ASSERT_TRUE(unchanged);ASSERT_TRUE(MainField(*unchanged));
    EXPECT_EQ(MainField(*unchanged)->contentHash,oldHash);
}

TEST(DistanceFieldIntegrationTests, LegacyMetadataTimestampDoesNotForceAFullModelReimport)
{
    Fixture fixture("legacy-derived");fixture.meta.settings["DISTANCE_FIELD_GENERATION"]="inherit";
    ASSERT_TRUE(fixture.meta.Save(fixture.source.string()+".meta"));
    {std::ofstream project(fixture.root/"Test.nullus");project<<"generate_mesh_distance_fields=false\n";}
    const auto roots=NLS::Editor::Assets::MakeProjectEditorAssetRoots(fixture.root);
    NLS::Editor::Assets::AssetImporterFacade importer(roots);ASSERT_TRUE(importer.SaveAndReimport("Assets/Cube.obj"));
    NLS::Editor::Assets::AssetDatabaseFacade database(roots);ASSERT_TRUE(database.Refresh());
    auto legacy=database.GetArtifactManifestForAssetPath("Assets/Cube.obj");ASSERT_TRUE(legacy);
    std::erase_if(legacy->subAssets,[](const auto& a){return a.artifactType==assets::ArtifactType::MeshDistanceFieldSource;});
    const auto metaPath=std::filesystem::path(fixture.source.string()+".meta");
    const auto oldStamp=NLS::Editor::Assets::DistanceFieldBuildService::MetadataStamp(metaPath);
    for(auto& dependency:legacy->dependencies)
        if(dependency.kind==assets::AssetDependencyKind::PathToGuidMapping && dependency.value=="Assets/Cube.obj.meta")dependency.hashOrVersion=oldStamp;
    database.AddArtifactManifest(*legacy);
    std::filesystem::last_write_time(metaPath,std::filesystem::last_write_time(metaPath)+std::chrono::seconds(2));
    {std::ofstream project(fixture.root/"Test.nullus");project<<"generate_mesh_distance_fields=true\n";}
    NLS::Editor::Assets::DistanceFieldBuildRequest request;request.projectRoot=fixture.root;request.assetPath="Assets/Cube.obj";request.mipMask=4;
    const auto built=NLS::Editor::Assets::DistanceFieldBuildService::Build(request);
    ASSERT_TRUE(built.success)<<built.error;EXPECT_EQ(built.legacySourceReads,1u);
    NLS::Editor::Assets::AssetDatabaseFacade after(roots);ASSERT_TRUE(after.RefreshKnownSourceAssets(std::array{fixture.source}));
    const auto published=after.GetArtifactManifestForAssetPath("Assets/Cube.obj");ASSERT_TRUE(published);ASSERT_TRUE(MainField(*published));
    const auto stamp=std::find_if(published->dependencies.begin(),published->dependencies.end(),[](const auto& d)
        {return d.kind==assets::AssetDependencyKind::PathToGuidMapping && d.value=="Assets/Cube.obj.meta";});
    ASSERT_NE(stamp,published->dependencies.end());EXPECT_EQ(stamp->hashOrVersion,oldStamp);
    const auto warm=NLS::Editor::Assets::DistanceFieldBuildService::Build(request);
    ASSERT_TRUE(warm.success)<<warm.error;EXPECT_EQ(warm.legacySourceReads,0u);EXPECT_EQ(warm.model.stats.builtMips,0u);
}

TEST(DistanceFieldIntegrationTests, PrefabFreshnessIgnoresDistanceFieldSettingsAndChecksModelSettings)
{
    Fixture fixture("prefab-freshness");
    const auto roots = NLS::Editor::Assets::MakeProjectEditorAssetRoots(fixture.root);
    NLS::Editor::Assets::AssetImporterFacade importer(roots);
    ASSERT_TRUE(importer.SaveAndReimport("Assets/Cube.obj"));
    NLS::Editor::Assets::AssetDatabaseFacade database(roots);
    ASSERT_TRUE(database.RefreshKnownSourceAssets(std::array{fixture.source}));
    const auto manifest = database.GetArtifactManifestForAssetPath("Assets/Cube.obj");
    ASSERT_TRUE(manifest);
    auto meta = assets::AssetMeta::Load(fixture.source.string() + ".meta");
    ASSERT_TRUE(meta);
    const auto current = [&](const assets::ArtifactManifest& candidate)
    {
        return NLS::Editor::Assets::ManifestDependenciesAreCurrentForTesting(
            candidate, *meta, fixture.root, fixture.source);
    };
    ASSERT_TRUE(current(*manifest));
    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(fixture.root / "Assets");
    ASSERT_TRUE(bridge.TryLoadImportedPrefabArtifact("Assets/Cube.obj", manifest->primarySubAssetKey));
    meta->settings["DISTANCE_FIELD_RESOLUTION_SCALE"] = "0.5";
    ASSERT_TRUE(meta->Save(fixture.source.string() + ".meta"));
    EXPECT_TRUE(current(*manifest));
    EXPECT_TRUE(bridge.TryLoadImportedPrefabArtifact("Assets/Cube.obj", manifest->primarySubAssetKey));

    auto legacy = *manifest;
    for (auto& dependency : legacy.dependencies)
        if (dependency.kind == assets::AssetDependencyKind::PathToGuidMapping &&
            dependency.value == "Assets/Cube.obj.meta")
            dependency.hashOrVersion = NLS::Editor::Assets::DistanceFieldBuildService::MetadataStamp(
                fixture.source.string() + ".meta");
    EXPECT_TRUE(current(legacy));
    meta->settings["GLOBAL_SCALE"] = "2";
    ASSERT_TRUE(meta->Save(fixture.source.string() + ".meta"));
    EXPECT_FALSE(current(*manifest));
    EXPECT_FALSE(bridge.TryLoadImportedPrefabArtifact("Assets/Cube.obj", manifest->primarySubAssetKey));
}

TEST(DistanceFieldIntegrationTests, EditorStartupResolvesFieldsAgainstTheProjectDirectory)
{
    Fixture fixture("startup-field-paths");
    const auto roots = NLS::Editor::Assets::MakeProjectEditorAssetRoots(fixture.root);
    NLS::Editor::Assets::AssetImporterFacade importer(roots);
    ASSERT_TRUE(importer.SaveAndReimport("Assets/Cube.obj"));
    assets::ArtifactDatabase artifacts;
    ASSERT_TRUE(artifacts.Load(fixture.database));
    const auto manifest = artifacts.BuildManifestForSource(fixture.meta.id, "editor");
    ASSERT_TRUE(manifest);
    // Rebase only the host root so no previously registered absolute source can
    // satisfy the lookup. The current directory stays outside this project.
    const auto movedRoot = fixture.root / "Relocated";
    std::filesystem::create_directories(movedRoot / "Library");
    std::filesystem::copy(fixture.artifacts, movedRoot / "Library/Artifacts",
        std::filesystem::copy_options::recursive);
    std::filesystem::copy(fixture.database, movedRoot / "Library/ArtifactDB",
        std::filesystem::copy_options::recursive);
    const auto startup = NLS::Editor::Core::LoadEditorRuntimeAssetDatabaseForTesting(movedRoot, true);
    ASSERT_TRUE(startup);
    uint32_t fields = 0;
    for (const auto& artifact : manifest->subAssets)
    {
        if (artifact.artifactType != assets::ArtifactType::Mesh) continue;
        const auto source = df::FindMeshDistanceFieldSource((movedRoot / artifact.artifactPath).generic_string());
        ASSERT_TRUE(source);
        EXPECT_TRUE(std::filesystem::path(source->mainPath).is_absolute());
        const auto field = df::LoadDistanceFieldArtifact(source->mainPath);
        ASSERT_TRUE(field) << source->mainPath;
        EXPECT_LT(df::SampleMeshDistanceField(*field, {0, 0, 0}, 2).distance, 0);
        for (uint32_t mip = 0; mip < 2; ++mip)
            EXPECT_TRUE(df::LoadDistanceFieldMip(source->mipPaths[mip]));
        ++fields;
    }
    EXPECT_GT(fields, 0u);
}

TEST(DistanceFieldIntegrationTests, InspectorFieldEditPreservesModelDefaultsAndFloatingPointType)
{
    namespace inspector = NLS::Editor::Inspector;
    namespace serialization = NLS::Editor::Serialization;
    Fixture fixture("inspector-field-delta");
    fixture.meta.settings["DISTANCE_FIELD_RESOLUTION_SCALE"] = "1";
    fixture.meta.settings["MODEL_SCALE"] = "1.0";
    auto draft = fixture.meta.settings;
    draft["MODEL_FBX_READER"] = "autodesk-with-assimp-fallback";
    draft["MIN_LOD"] = "0";
    draft["MODEL_SCALE"] = "1";
    const auto target = inspector::InspectorTargetHandle::FromAsset(
        inspector::InspectorTargetKind::AssetImporter, "Assets/Cube.obj", fixture.meta.id.ToString());
    NLS::Editor::Undo::EditorUndoService undo;
    NLS::Core::ServiceLocator::Provide(undo);
    inspector::TestHooks::SeedImporterDraftForTesting(target, draft);
    struct Cleanup
    {
        inspector::InspectorTargetHandle target;
        ~Cleanup()
        {
            inspector::TestHooks::ClearImporterDraftForTesting(target);
            NLS::Core::ServiceLocator::Remove<NLS::Editor::Undo::EditorUndoService>();
        }
    } cleanup{target};
    inspector::InspectorTargetSet targets;
    targets.targets = {target};
    serialization::SerializedObject object(targets, serialization::ImporterDraftSerializedBackend::Create());
    ASSERT_TRUE(object.IsValid()) << object.GetLastError();
    const auto property = object.FindProperty("Importer.Settings.DISTANCE_FIELD_RESOLUTION_SCALE");
    ASSERT_TRUE(property);
    EXPECT_EQ(property.GetNumericType(), serialization::SerializedNumericType::Double);
    ASSERT_TRUE(property.SetDoubleValue(0.5));
    ASSERT_TRUE(object.ApplyModifiedProperties()) << object.GetLastError();
    const auto edited = inspector::ReadImporterEditingState(target).settings;
    auto updated = fixture.meta;
    updated.settings = inspector::TestHooks::BuildImporterSettingsForApplyForTesting(fixture.meta, edited);
    auto expected = fixture.meta.settings;
    expected["DISTANCE_FIELD_RESOLUTION_SCALE"] = "0.5";
    EXPECT_EQ(updated.settings, expected);
    EXPECT_EQ(assets::ComputeModelImportSettingsFingerprint(updated),
        assets::ComputeModelImportSettingsFingerprint(fixture.meta));
    serialization::SerializedObject refreshed(targets, serialization::ImporterDraftSerializedBackend::Create());
    EXPECT_DOUBLE_EQ(refreshed.FindProperty("Importer.Settings.DISTANCE_FIELD_RESOLUTION_SCALE").GetDoubleValue(), 0.5);
    ASSERT_TRUE(undo.RequestUndo().Succeeded());
    ASSERT_TRUE(undo.RequestRedo().Succeeded());
    auto normalEdit = edited;
    normalEdit["MODEL_SCALE"] = "2";
    updated.settings = inspector::TestHooks::BuildImporterSettingsForApplyForTesting(fixture.meta, normalEdit);
    EXPECT_NE(assets::ComputeModelImportSettingsFingerprint(updated),
        assets::ComputeModelImportSettingsFingerprint(fixture.meta));
}

TEST(DistanceFieldIntegrationTests, MeshOnlyPackIncludesIndependentMipsAndLoadsWithoutLooseArtifacts)
{
    if(assets::HasContentMounts()) GTEST_SKIP() << "Another test owns content mounts";
    Fixture fixture("package");
    const auto shader=fixture.root/"Assets/Engine/Shaders/ShaderLab/StandardPBR.shader";
    std::filesystem::create_directories(shader.parent_path());
    std::filesystem::copy_file(std::filesystem::path(NLS_ROOT_DIR)/"App/Assets/Engine/Shaders/ShaderLab/StandardPBR.shader",shader);
    auto shaderMeta=assets::AssetMeta::CreateForAsset(shader);
    shaderMeta.id=assets::AssetId(NLS::Cook::MakeEngineAssetGuid("Assets/Engine/Shaders/ShaderLab/StandardPBR.shader"));
    ASSERT_TRUE(shaderMeta.Save(shader.string()+".meta"));
    const auto cooked=NLS::Cook::AssetCooker::CookAssets(fixture.Options(),{shader,fixture.source});
    ASSERT_TRUE(cooked.Succeeded()) << cooked.FormatSummary();
    auto manifest=fixture.Manifest(); ASSERT_TRUE(manifest); ASSERT_TRUE(MainField(*manifest));
    std::string meshKey;
    for(auto& artifact:manifest->subAssets)
    {
        artifact.artifactPath=std::filesystem::relative(fixture.Resolve(artifact.artifactPath),fixture.root).generic_string();
        if(artifact.artifactType==assets::ArtifactType::Mesh) meshKey=artifact.subAssetKey;
    }
    ASSERT_FALSE(meshKey.empty());
    assets::ArtifactDatabase database; ASSERT_TRUE(database.Load(fixture.database));
    auto shaderManifest=database.BuildManifestForSource(shaderMeta.id,"win64-dx12"); ASSERT_TRUE(shaderManifest);
    for(auto& artifact:shaderManifest->subAssets)
        artifact.artifactPath=std::filesystem::relative(fixture.Resolve(artifact.artifactPath),fixture.root).generic_string();
    NLS::Engine::Assets::RuntimeManifestBuilder builder; builder.AddArtifactManifest(*manifest); builder.AddArtifactManifest(*shaderManifest);
    const auto packed=builder.BuildAssetPacks({{"distance-field","",{fixture.meta.id,meshKey}}},"win64-dx12");
    for(const auto& diagnostic:packed.diagnostics.GetItems())
        if(diagnostic.IsError()) ADD_FAILURE() << diagnostic.code << " key=" << diagnostic.subAssetKey << " path=" << diagnostic.path;
    ASSERT_FALSE(packed.diagnostics.HasErrors());
    ASSERT_EQ(packed.manifest.assetPacks.size(),1u);
    uint32_t fields=0;
    assets::NpakWriter writer; std::string error;
    for(const auto& entry:packed.manifest.assetPacks[0].entries)
    {
        if(entry.artifactType==assets::ArtifactType::MeshDistanceField) ++fields;
        ASSERT_TRUE(writer.AddFileFromDisk(entry.artifactPath,fixture.root/entry.artifactPath,&error)) << error;
    }
    EXPECT_EQ(fields,3u);
    const auto package=fixture.root/"distance-field.npak"; ASSERT_TRUE(writer.Write(package,{},&error)) << error;
    std::filesystem::rename(fixture.artifacts,fixture.root/"LooseArtifactsBackup");
    ASSERT_TRUE(assets::MountContentPackage(package,fixture.root,&error)) << error;
    struct Unmount { ~Unmount(){assets::ClearContentMounts();} } unmount;
    NLS::Engine::Assets::RuntimeAssetDatabase runtime(packed.manifest);
    const auto source=df::FindMeshDistanceFieldSource((fixture.root/manifest->FindSubAsset(meshKey)->artifactPath).generic_string()); ASSERT_TRUE(source);
    EXPECT_EQ(source->contentHash,MainField(*manifest)->contentHash);
    auto asset=df::LoadDistanceFieldArtifact(source->mainPath); ASSERT_TRUE(asset);
    EXPECT_LT(df::SampleMeshDistanceField(*asset,{0,0,0},2).distance,0);
    auto mip=df::LoadDistanceFieldMip(source->mipPaths[0]); ASSERT_TRUE(mip);
    asset->mips[0]=std::move(*mip);
    EXPECT_LT(df::SampleMeshDistanceField(*asset,{0,0,0},0).distance,0);
}
