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
