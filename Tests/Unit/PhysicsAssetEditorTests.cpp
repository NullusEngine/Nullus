#include <gtest/gtest.h>
#include <Assets/AssetDatabaseFacade.h>
#include <Assets/ImportProgressTracker.h>
#include <Core/Assets/AssetMeta.h>
#include <Core/Assets/ArtifactDatabase.h>
#include <Core/ResourceManagement/ShaderManager.h>
#include <Engine/Assets/PhysicsMaterial.h>
#include <Engine/Assets/CollisionMeshAsset.h>
#include <Engine/Assets/PhysicsShapeAsset.h>
#include <Engine/Components/PhysicsColliderComponent.h>
#include <Engine/SceneSystem/Scene.h>
#include <Engine/Serialize/ObjectGraphDocument.h>
#include <fstream>
#include <algorithm>

namespace
{
using namespace NLS;
namespace CA=Core::Assets;
namespace EA=Engine::Assets;
namespace ED=Editor::Assets;
namespace ES=Engine::Serialize;
const std::string Hull=R"({"version":1,"kind":"convex","vertices":[[0,0,0],[2,0,0],[0,2,0],[0,0,2]]})";
class PhysicsAssetEditorTests : public testing::Test
{
protected:
    std::filesystem::path parent=std::filesystem::absolute(std::filesystem::path(NLS_BUILD_DIR)/"physics-validation/assets/editor-fixtures").lexically_normal();
    std::filesystem::path root;
    std::string oldProject,oldEngine;
    void SetUp() override
    {
        root=parent/Guid::New().ToString(); std::filesystem::create_directories(root/"Assets");
        oldProject=Core::ResourceManagement::ShaderManager::ProjectAssetsRoot(); oldEngine=Core::ResourceManagement::ShaderManager::EngineAssetsRoot();
        Core::ResourceManagement::ShaderManager::ProvideAssetPaths((root/"Assets").generic_string()+"/",oldEngine);
    }
    void TearDown() override
    {
        Core::ResourceManagement::ShaderManager::ProvideAssetPaths(oldProject,oldEngine); ES::PersistentManager::Instance().Clear();
        ASSERT_EQ(root.parent_path(),parent); std::error_code error; std::filesystem::remove_all(root,error); EXPECT_FALSE(error)<<error.message();
    }
    void Write(const std::string& name,const std::string& text)
    { std::ofstream file(root/"Assets"/name,std::ios::binary|std::ios::trunc); file<<text; ASSERT_TRUE(file); }
    CA::AssetMeta Make(const std::string& name,const std::string& text)
    {
        Write(name,text); auto meta=CA::AssetMeta::CreateForAsset(root/"Assets"/name);
        if(!meta.Save(CA::GetAssetMetaPath(root/"Assets"/name))) throw std::runtime_error("Cannot save physics editor meta"); return meta;
    }
    CA::ArtifactManifest Manifest(CA::AssetId id)
    {
        CA::ArtifactDatabase database; if(!database.Load(root/"Library/ArtifactDB")) throw std::runtime_error("Cannot load editor physics ArtifactDB");
        auto manifest=database.BuildManifestForSource(id,"editor"); if(!manifest) throw std::runtime_error("Missing editor physics manifest"); return *manifest;
    }
    static std::string Diagnostics(const ED::AssetDatabaseFacade& facade)
    { std::string text; for(const auto& error:facade.GetDiagnostics()) text+=error.code+":"+error.message+"\n"; return text; }
    static ES::ObjectIdentifier Reference(CA::AssetId id,const std::string& prefix)
    {
        ES::ObjectIdentifier reference; reference.guid=id.GetGuid(); reference.fileType=ES::FileType::SerializedAssetType;
        reference.localIdentifierInFile=ES::MakeLocalIdentifierInFile(reference.guid,prefix+":main"); return reference;
    }
};
}

TEST_F(PhysicsAssetEditorTests, NativeImporterPersistsBothPhysicsResourcesAndReopensTypedAssets)
{
    const auto mesh=Make("Hull.collisionmesh",Hull),material=Make("Surface.physmat",R"({"version":1,"friction2D":0.2})");
    const auto metaTime=std::filesystem::last_write_time(CA::GetAssetMetaPath(root/"Assets/Hull.collisionmesh"));
    {
        ED::AssetDatabaseFacade facade(std::vector<std::filesystem::path>{root}); ASSERT_TRUE(facade.Refresh());
        ASSERT_TRUE(facade.ImportAsset("Assets/Hull.collisionmesh"))<<Diagnostics(facade);
        ASSERT_TRUE(facade.ImportAsset("Assets/Surface.physmat"))<<Diagnostics(facade);
    }
    EXPECT_EQ(Manifest(mesh.id).subAssets[0].artifactType,CA::ArtifactType::CollisionMesh);
    EXPECT_EQ(Manifest(material.id).subAssets[0].artifactType,CA::ArtifactType::PhysicsMaterial);
    EXPECT_EQ(std::filesystem::last_write_time(CA::GetAssetMetaPath(root/"Assets/Hull.collisionmesh")),metaTime);
    ED::AssetDatabaseFacade reopened(std::vector<std::filesystem::path>{root}); ASSERT_TRUE(reopened.Refresh());
    EXPECT_FALSE(reopened.ResolveArtifactPathAtPath("Assets/Hull.collisionmesh","collision-mesh:main").empty());
    std::string error; auto collision=EA::LoadCollisionMeshAsset(Reference(mesh.id,"collision-mesh"),error); ASSERT_TRUE(collision)<<error;
    auto surface=EA::LoadPhysicsMaterial(Reference(material.id,"physics-material"),error); ASSERT_TRUE(surface)<<error;
    EXPECT_FLOAT_EQ(surface->Capture()->friction2D,0.2f); EXPECT_EQ(collision->Capture()->GetKind(),Physics::CollisionMeshKind::Convex);
}

TEST_F(PhysicsAssetEditorTests, InvalidReimportRetainsThePreviousSuccessfulArtifact)
{
    const auto mesh=Make("Hull.collisionmesh",Hull); ED::AssetDatabaseFacade facade(std::vector<std::filesystem::path>{root}); ASSERT_TRUE(facade.Refresh());
    ASSERT_TRUE(facade.ImportAsset("Assets/Hull.collisionmesh"))<<Diagnostics(facade); const auto previous=Manifest(mesh.id).subAssets[0];
    Write("Hull.collisionmesh",R"({"version":1,"kind":"convex","vertices":[[0,0,0]]})"); EXPECT_FALSE(facade.ReimportAsset("Assets/Hull.collisionmesh"));
    EXPECT_EQ(Manifest(mesh.id).subAssets[0].contentHash,previous.contentHash); EXPECT_TRUE(std::filesystem::exists(root/previous.artifactPath));
}

TEST_F(PhysicsAssetEditorTests, SourceAndMetaChangesDuringCookingOrFinalPublicationCannotReplaceGoodArtifacts)
{
    const auto mesh=Make("Hull.collisionmesh",Hull); ED::AssetDatabaseFacade facade(std::vector<std::filesystem::path>{root}); ASSERT_TRUE(facade.Refresh());
    ASSERT_TRUE(facade.ImportAsset("Assets/Hull.collisionmesh"))<<Diagnostics(facade);
    for(const auto phase:{ED::ImportPhase::SourceParse,ED::ImportPhase::ArtifactWrite}) for(bool changeMeta:{false,true})
    {
        const auto previous=Manifest(mesh.id).subAssets[0]; ED::ImportProgressTracker progress; bool changed=false;
        progress.Subscribe([&](const ED::ImportProgressEvent& event)
        {
            if(changed || event.phase!=phase || (event.message!="Cooking collision geometry and physics properties" && event.message!="Publishing physics artifact")) return;
            changed=true;
            if(changeMeta)
            {
                auto changedMeta=mesh; changedMeta.settings["physics.revision"]="changed-during-cook";
                EXPECT_TRUE(changedMeta.Save(CA::GetAssetMetaPath(root/"Assets/Hull.collisionmesh")));
            }
            else
            {
                auto changedSource=Hull; changedSource.insert(changedSource.size()-1,",\"bakeScale\":[2,1,1]"); Write("Hull.collisionmesh",changedSource);
            }
        });
        EXPECT_FALSE(facade.ReimportAsset("Assets/Hull.collisionmesh",progress))<<"Stale import unexpectedly succeeded"; EXPECT_TRUE(changed);
        EXPECT_EQ(Manifest(mesh.id).subAssets[0].contentHash,previous.contentHash); EXPECT_TRUE(std::filesystem::exists(root/previous.artifactPath));
        Write("Hull.collisionmesh",Hull); ASSERT_TRUE(mesh.Save(CA::GetAssetMetaPath(root/"Assets/Hull.collisionmesh")));
        ASSERT_TRUE(facade.ReimportAsset("Assets/Hull.collisionmesh"))<<Diagnostics(facade);
    }
}

TEST_F(PhysicsAssetEditorTests, CancellationDuringFinalPublicationRetainsArtifactsAndEndsTheJob)
{
    const auto mesh=Make("Hull.collisionmesh",Hull); ED::AssetDatabaseFacade facade(std::vector<std::filesystem::path>{root}); ASSERT_TRUE(facade.Refresh());
    ASSERT_TRUE(facade.ImportAsset("Assets/Hull.collisionmesh"))<<Diagnostics(facade); const auto previous=Manifest(mesh.id).subAssets[0];
    ED::ImportProgressTracker progress; bool cancelled=false; ED::ImportJobTerminalStatus terminal=ED::ImportJobTerminalStatus::None;
    progress.Subscribe([&](const ED::ImportProgressEvent& event)
    {
        if(event.phase==ED::ImportPhase::Finished) terminal=event.terminalStatus;
        if(event.message=="Publishing physics artifact")
        { const auto token=progress.GetCancellationToken(event.jobId); ASSERT_TRUE(token); token->get().Cancel(); cancelled=true; }
    });
    EXPECT_FALSE(facade.ReimportAsset("Assets/Hull.collisionmesh",progress)); EXPECT_TRUE(cancelled); EXPECT_FALSE(progress.HasRunningJobs());
    EXPECT_EQ(terminal,ED::ImportJobTerminalStatus::Cancelled); EXPECT_EQ(Manifest(mesh.id).subAssets[0].contentHash,previous.contentHash);
}

TEST_F(PhysicsAssetEditorTests, ShapeImporterPublishesReferencesReloadsAndPreservesThePreviousGroupOnInvalidSource)
{
    const auto mesh=Make("Hull.collisionmesh",Hull),material=Make("Surface.physmat",R"({"version":1,"restitution":0.1})");
    const auto text="{\"version\":1,\"shapes\":[{\"id\":7,\"type\":\"box\",\"position\":[-3,0,0]},{\"id\":19,\"type\":\"mesh\",\"mesh\":{\"guid\":\""+
        mesh.id.ToString()+"\"},\"material\":{\"guid\":\""+material.id.ToString()+"\"}}]}";
    const auto shape=Make("Compound.physshape",text); ED::AssetDatabaseFacade facade(std::vector<std::filesystem::path>{root}); ASSERT_TRUE(facade.Refresh());
    for(const auto* path:{"Assets/Hull.collisionmesh","Assets/Surface.physmat","Assets/Compound.physshape"}) ASSERT_TRUE(facade.ImportAsset(path))<<Diagnostics(facade);
    const auto first=Manifest(shape.id); EXPECT_EQ(first.subAssets[0].artifactType,CA::ArtifactType::PhysicsShape);
    EXPECT_EQ(std::count_if(first.dependencies.begin(),first.dependencies.end(),[](const auto& dep){return dep.kind==CA::AssetDependencyKind::ImportedArtifact;}),2);
    Engine::SceneSystem::Scene scene; auto* collider=scene.CreateGameObject("Compound").AddComponent<Engine::Components::PhysicsColliderComponent>();
    collider->SetShape(Engine::Components::PhysicsColliderComponent::EColliderShape::SHAPE_ASSET);
    collider->SetShapeResource(ES::PPtr<EA::PhysicsShapeAsset>(ES::PersistentManager::Instance().ObjectIdentifierToInstanceID(Reference(shape.id,"physics-shape"))));
    scene.SyncTransforms(); ASSERT_TRUE(collider->IsQueryReady())<<collider->LastError(); const auto old=collider->GetShapeHandles();
    Write("Compound.physshape",R"({"version":1,"shapes":[{"id":19,"type":"sphere","radius":1,"position":[3,0,0]}]})");
    ASSERT_TRUE(facade.ReimportAsset("Assets/Compound.physshape"))<<Diagnostics(facade); scene.SyncTransforms();
    ASSERT_EQ(collider->GetShapeHandles().size(),1u); for(auto handle:old) EXPECT_FALSE(scene.GetPhysicsWorld()->IsValid(handle));
    Physics::QueryHit hit; ASSERT_TRUE(scene.GetPhysicsWorld()->Read()->Raycast({3,4,0},{0,-1,0},8,hit)); EXPECT_EQ(hit.subShapeId,19u); EXPECT_NEAR(hit.position.y,1,1e-4f);
    const auto good=Manifest(shape.id).subAssets[0]; const auto handles=collider->GetShapeHandles();
    Write("Compound.physshape",R"({"version":1,"shapes":[{"id":19,"type":"sphere","radius":-1}]})");
    EXPECT_FALSE(facade.ReimportAsset("Assets/Compound.physshape")); scene.SyncTransforms();
    EXPECT_EQ(Manifest(shape.id).subAssets[0].contentHash,good.contentHash); EXPECT_EQ(collider->GetShapeHandles(),handles); EXPECT_TRUE(collider->IsQueryReady());
}

TEST_F(PhysicsAssetEditorTests, ShapeFinalPublicationRejectsSourceChangesAndCancellation)
{
    const std::string shapeText=R"({"version":1,"shapes":[{"id":7,"type":"box"}]})";
    const auto shape=Make("Compound.physshape",shapeText); ED::AssetDatabaseFacade facade(std::vector<std::filesystem::path>{root}); ASSERT_TRUE(facade.Refresh());
    ASSERT_TRUE(facade.ImportAsset("Assets/Compound.physshape"))<<Diagnostics(facade);
    for(bool cancel:{false,true})
    {
        const auto old=Manifest(shape.id).subAssets[0]; ED::ImportProgressTracker progress; bool touched=false;
        progress.Subscribe([&](const ED::ImportProgressEvent& event)
        {
            if(touched || event.message!="Publishing physics artifact") return; touched=true;
            if(cancel) { const auto token=progress.GetCancellationToken(event.jobId); ASSERT_TRUE(token); token->get().Cancel(); }
            else Write("Compound.physshape",R"({"version":1,"shapes":[{"id":7,"type":"sphere"}]})");
        });
        EXPECT_FALSE(facade.ReimportAsset("Assets/Compound.physshape",progress)); EXPECT_TRUE(touched); EXPECT_FALSE(progress.HasRunningJobs());
        EXPECT_EQ(Manifest(shape.id).subAssets[0].contentHash,old.contentHash); Write("Compound.physshape",shapeText);
    }
}
