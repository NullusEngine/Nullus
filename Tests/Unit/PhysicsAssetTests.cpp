#include <gtest/gtest.h>
#include <Assets/ArtifactDatabase.h>
#include <Assets/AssetMeta.h>
#include <Assets/NativeArtifactContainer.h>
#include <Core/ResourceManagement/ShaderManager.h>
#include <Core/ServiceLocator.h>
#include <Cook/CookTypes.h>
#include <Engine/Assets/PhysicsAssetCompiler.h>
#include <Engine/Assets/PhysicsAssetReadScope.h>
#include <Engine/Assets/CollisionMeshAsset.h>
#include <Engine/Assets/RuntimeAssetDatabase.h>
#include <Engine/Components/PhysicsColliderComponent.h>
#include <Engine/Components/PhysicsCollider2DComponent.h>
#include <Engine/Components/RigidbodyComponent.h>
#include <Engine/Components/Rigidbody2DComponent.h>
#include <Engine/SceneSystem/Scene.h>
#include <Engine/Serialize/ObjectGraphSerializer.h>
#include <Engine/Serialize/ObjectGraphWriter.h>
#include <Engine/Serialize/ObjectGraphReader.h>
#include <Engine/Serialize/ObjectGraphInstantiator.h>
#include <Platform/Process/Process.h>
#include <fstream>
#include <atomic>

namespace
{
using namespace NLS;
namespace CA=Core::Assets;
namespace EA=Engine::Assets;
namespace ES=Engine::Serialize;
using Engine::Components::PhysicsColliderComponent;
using Engine::Components::PhysicsCollider2DComponent;
using Engine::Components::RigidbodyComponent;
using Engine::Components::Rigidbody2DComponent;
const std::string TetraSource=R"({"version":1,"kind":"convex","vertices":[[0,0,0],[2,0,0],[0,2,0],[0,0,2]]})";
const std::string FloorSource=R"({"version":1,"kind":"triangle","vertices":[[-10,0,-10],[10,0,-10],[10,0,10],[-10,0,10]],"indices":[0,2,1,0,3,2]})";
const std::string BounceSource=R"({"version":1,"staticFriction":0,"dynamicFriction":0,"restitution":0.9,"friction2D":0,"restitution2D":0.9,"restitutionCombine":"maximum"})";
const std::string MixedShapeSource=R"({"version":1,"shapes":[
 {"id":7,"type":"box","position":[-2,0,0],"halfExtents":[2,0.25,0.25],"rotation":[0,0,0.70710678,0.70710678]},
 {"id":19,"type":"sphere","position":[2,0,0],"radius":0.5},
 {"id":31,"type":"capsule","position":[6,0,0],"radius":0.25,"halfHeight":1},
 {"id":43,"type":"cylinder","position":[10,0,0],"radius":0.5,"halfHeight":1}]})";
struct Source
{
    std::filesystem::path path;
    CA::AssetMeta meta;
    ES::ObjectIdentifier Reference(const std::string& prefix) const
    {
        ES::ObjectIdentifier result; result.guid=meta.id.GetGuid(); result.fileType=ES::FileType::SerializedAssetType;
        result.localIdentifierInFile=ES::MakeLocalIdentifierInFile(result.guid,prefix+":main"); result.filePath=path.filename().generic_string(); return result;
    }
};
class PhysicsAssetTests : public testing::Test
{
protected:
    std::filesystem::path fixtureParent=std::filesystem::path(NLS_BUILD_DIR)/"physics-validation/assets/fixtures";
    std::filesystem::path root;
    Cook::CookOptions options;
    std::string oldProject,oldEngine;
    std::unique_ptr<EA::RuntimeAssetDatabase> runtime;
    void SetUp() override
    {
        root=std::filesystem::absolute(fixtureParent/Guid::New().ToString()).lexically_normal(); std::filesystem::create_directories(root/"Assets");
        options.projectRoot=root; options.assetsRoot=root/"Assets"; options.artifactDatabasePath=root/"Library/ArtifactDB";
        options.target.artifactPlatform=Platform::Process::CurrentArtifactPlatformTag();
        options.target.architecture=Physics::CookedCollisionMesh::CompatibilityTag().find("-arm64-")!=std::string::npos?"arm64":"x64";
        oldProject=Core::ResourceManagement::ShaderManager::ProjectAssetsRoot(); oldEngine=Core::ResourceManagement::ShaderManager::EngineAssetsRoot();
        Core::ResourceManagement::ShaderManager::ProvideAssetPaths((root/"Assets").generic_string()+"/",oldEngine);
    }
    void TearDown() override
    {
        Core::ServiceLocator::Remove<EA::RuntimeAssetDatabase>(); runtime.reset(); ES::PersistentManager::Instance().Clear();
        Core::ResourceManagement::ShaderManager::ProvideAssetPaths(oldProject,oldEngine);
        ASSERT_EQ(root.parent_path(),std::filesystem::absolute(fixtureParent).lexically_normal());
        std::error_code error; std::filesystem::remove_all(root,error); EXPECT_FALSE(error)<<error.message();
    }
    static void Write(const std::filesystem::path& path,const std::string& source)
    { std::ofstream file(path,std::ios::binary|std::ios::trunc); file<<source; if(!file) throw std::runtime_error("Cannot write physics fixture"); }
    Source Make(const std::string& name,const std::string& text)
    {
        Source source; source.path=root/"Assets"/name; Write(source.path,text); source.meta=CA::AssetMeta::CreateForAsset(source.path);
        if(!source.meta.Save(CA::GetAssetMetaPath(source.path))) throw std::runtime_error("Cannot save physics fixture meta"); return source;
    }
    CA::ArtifactManifest Manifest(const Source& source)
    {
        CA::ArtifactDatabase database; if(!database.Load(options.artifactDatabasePath)) throw std::runtime_error("Cannot load fixture ArtifactDB");
        auto manifest=database.BuildManifestForSource(source.meta.id,options.target.artifactPlatform);
        if(!manifest) throw std::runtime_error("Missing physics fixture manifest"); return *manifest;
    }
    void Install(const std::vector<Source>& sources)
    {
        EA::RuntimeManifestBuilder builder; std::vector<EA::RuntimeAssetRef> roots;
        for(const auto& source:sources) { auto manifest=Manifest(source); roots.push_back({source.meta.id,manifest.primarySubAssetKey}); builder.AddArtifactManifest(std::move(manifest)); }
        auto built=builder.Build(roots,options.target.artifactPlatform); ASSERT_FALSE(built.diagnostics.HasErrors());
        Core::ServiceLocator::Remove<EA::RuntimeAssetDatabase>(); runtime=std::make_unique<EA::RuntimeAssetDatabase>(std::move(built.manifest));
        Core::ServiceLocator::Provide<EA::RuntimeAssetDatabase>(*runtime);
    }
    void CookSources(const std::vector<Source>& sources)
    {
        std::vector<std::filesystem::path> paths; for(const auto& source:sources) paths.push_back(source.path);
        const auto cooked=Cook::AssetCooker::CookAssets(options,paths); ASSERT_TRUE(cooked.Succeeded())<<cooked.FormatSummary();
    }
    PhysicsColliderComponent* ShapeCollider(Engine::SceneSystem::Scene& scene,const Source& source,const std::string& name="Compound")
    {
        auto* collider=scene.CreateGameObject(name).AddComponent<PhysicsColliderComponent>();
        collider->SetShape(PhysicsColliderComponent::EColliderShape::SHAPE_ASSET);
        collider->SetShapeResource(ES::PPtr<EA::PhysicsShapeAsset>(ES::PersistentManager::Instance().ObjectIdentifierToInstanceID(source.Reference("physics-shape"))));
        return collider;
    }
};
}

TEST_F(PhysicsAssetTests, NativeTypesUseExistingMetadataAndKeepSourceIdentity)
{
    const auto material=Make("Ice.physmat",R"({"version":1})"),mesh=Make("Hull.collisionmesh",TetraSource);
    EXPECT_EQ(material.meta.assetType,CA::AssetType::PhysicsMaterial); EXPECT_EQ(mesh.meta.assetType,CA::AssetType::CollisionMesh);
    EXPECT_EQ(CA::AssetTypeFromString(CA::ToString(material.meta.assetType)),material.meta.assetType);
    const auto materialTime=std::filesystem::last_write_time(CA::GetAssetMetaPath(material.path)); CookSources({material,mesh});
    EXPECT_EQ(std::filesystem::last_write_time(CA::GetAssetMetaPath(material.path)),materialTime);
    const auto manifest=Manifest(mesh); ASSERT_EQ(manifest.subAssets.size(),1u); EXPECT_EQ(manifest.sourceAssetId,mesh.meta.id);
    std::ifstream input(root/manifest.subAssets[0].artifactPath,std::ios::binary); std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)),{});
    const auto native=CA::ReadNativeArtifactContainer(bytes,CA::ArtifactType::CollisionMesh,1); ASSERT_TRUE(native);
    EXPECT_EQ(native->metadata.schemaName,"collision-mesh"); EXPECT_EQ(native->metadata.sourceAssetId,mesh.meta.id);
}

TEST_F(PhysicsAssetTests, MaterialCompilerRejectsMalformedDataWithoutChangingOutput)
{
    std::string error; EA::PhysicsMaterialData data; ASSERT_TRUE(EA::ParsePhysicsMaterial(BounceSource,data,error));
    EXPECT_EQ(data.restitutionCombine,Physics::MaterialCombineMode::Maximum); EXPECT_FLOAT_EQ(data.restitution2D,0.9f);
    for(const auto* invalid:{R"({"version":2})",R"({"version":1,"restitution":2})",R"({"version":1,"dynamicFriction":-1})",
        R"({"version":1,"friction2D":true})",R"({"version":1,"restitution":0,"restitution":1})",R"({"version":1,"unknown":0})"})
    { EXPECT_FALSE(EA::ParsePhysicsMaterial(invalid,data,error)); EXPECT_FLOAT_EQ(data.restitution2D,0.9f); }
    CA::ArtifactPayload first,second; ASSERT_TRUE(EA::CompilePhysicsAsset(CA::AssetType::PhysicsMaterial,R"({"version":1})","editor",first,error));
    ASSERT_TRUE(EA::CompilePhysicsAsset(CA::AssetType::PhysicsMaterial,R"({"dynamicFriction":0.5,"version":1})","editor",second,error));
    EXPECT_EQ(first.payload,second.payload);
}

TEST_F(PhysicsAssetTests, MeshCompilerRejectsBadGeometryVersionsAndForeignTargets)
{
    std::string error; CA::ArtifactPayload result; result.payload={7};
    for(const auto* source:{R"({"version":1,"kind":"triangle","vertices":[[0,0,0],[1,0,0],[0,1,0]],"indices":[0,1,99]})",
        R"({"version":1,"kind":"convex","vertices":[[0,0,0],[1,0,0],[0,1,0],[1,1,0]]})",
        R"({"version":1,"kind":"heightfield","rows":2,"columns":2,"heights":[0]})",
        R"({"version":1,"kind":"convex","vertices":[[1e100,0,0]]})"})
    { EXPECT_FALSE(EA::CompilePhysicsAsset(CA::AssetType::CollisionMesh,source,"editor",result,error)); EXPECT_EQ(result.payload,std::vector<uint8_t>{7}); }
    EXPECT_FALSE(EA::CompilePhysicsAsset(CA::AssetType::CollisionMesh,TetraSource,"foreign-platform",result,error));
    EXPECT_NE(error.find("native host"),std::string::npos);
    const auto mesh=Make("Hull.collisionmesh",TetraSource); options.target.architecture="foreign-cpu";
    const auto wrongCpu=Cook::AssetCooker::CookAssets(options,{mesh.path}); EXPECT_EQ(wrongCpu.failed,1u); EXPECT_EQ(wrongCpu.cooked,0u);
}

TEST_F(PhysicsAssetTests, CacheIncludesBakeSettingsMetadataBackendAndTarget)
{
    auto mesh=Make("Hull.collisionmesh",TetraSource); CookSources({mesh}); const auto original=Manifest(mesh).subAssets[0].contentHash;
    const auto warm=Cook::AssetCooker::CookAssets(options,{mesh.path}); ASSERT_TRUE(warm.Succeeded()); EXPECT_EQ(warm.skipped,1u);
    auto changed=TetraSource; changed.insert(changed.size()-1,",\"bakeScale\":[-2,1,1]"); Write(mesh.path,changed); CookSources({mesh});
    EXPECT_NE(Manifest(mesh).subAssets[0].contentHash,original);
    mesh.meta.settings["physics.sourceRevision"]="2"; ASSERT_TRUE(mesh.meta.Save(CA::GetAssetMetaPath(mesh.path))); CookSources({mesh});
    auto manifest=Manifest(mesh); bool found=false;
    for(auto& dependency:manifest.dependencies) if(dependency.value=="nullus-physics-cooking") { dependency.hashOrVersion="obsolete-backend"; found=true; }
    ASSERT_TRUE(found); CA::ArtifactDatabase database; ASSERT_TRUE(database.Load(options.artifactDatabasePath));
    database.UpsertManifest(manifest,"Assets/Hull.collisionmesh",CA::ArtifactRecordStatus::UpToDate); ASSERT_TRUE(database.Save(options.artifactDatabasePath));
    const auto rebuild=Cook::AssetCooker::CookAssets(options,{mesh.path}); ASSERT_TRUE(rebuild.Succeeded())<<rebuild.FormatSummary(); EXPECT_EQ(rebuild.cooked,1u);
    options.target.artifactPlatform="editor"; CookSources({mesh}); EXPECT_EQ(Manifest(mesh).targetPlatform,"editor");
}

TEST_F(PhysicsAssetTests, FailedAndCancelledCookRetainsTheLastSuccessfulArtifact)
{
    const auto mesh=Make("Hull.collisionmesh",TetraSource); CookSources({mesh}); const auto previous=Manifest(mesh).subAssets[0];
    Write(mesh.path,R"({"version":1,"kind":"convex","vertices":[]})");
    const auto failed=Cook::AssetCooker::CookAssets(options,{mesh.path}); EXPECT_EQ(failed.failed,1u); EXPECT_EQ(Manifest(mesh).subAssets[0].contentHash,previous.contentHash);
    std::atomic_bool cancel=true; options.cancel=&cancel; const auto cancelled=Cook::AssetCooker::CookAssets(options,{mesh.path});
    EXPECT_EQ(cancelled.failed,1u); EXPECT_EQ(Manifest(mesh).subAssets[0].artifactPath,previous.artifactPath); options.cancel=nullptr;
}

TEST_F(PhysicsAssetTests, RuntimeManifestLoadsCookedGeometryAfterSourcesAndDatabaseAreRemoved)
{
    const auto mesh=Make("Floor.collisionmesh",FloorSource),material=Make("Bounce.physmat",BounceSource); CookSources({mesh,material}); Install({mesh,material});
    for(const auto& source:{mesh,material}) { std::filesystem::remove(source.path); std::filesystem::remove(CA::GetAssetMetaPath(source.path)); }
    std::filesystem::rename(options.artifactDatabasePath,root/"Library/RetiredArtifactDB");
    Engine::SceneSystem::Scene scene; auto& floor=scene.CreateGameObject("Cooked floor"); auto* collider=floor.AddComponent<PhysicsColliderComponent>();
    collider->SetShape(PhysicsColliderComponent::EColliderShape::COLLISION_MESH);
    collider->SetCollisionMesh(ES::PPtr<EA::CollisionMeshAsset>(ES::PersistentManager::Instance().ObjectIdentifierToInstanceID(mesh.Reference("collision-mesh"))));
    collider->SetPhysicsMaterial(ES::PPtr<EA::PhysicsMaterial>(ES::PersistentManager::Instance().ObjectIdentifierToInstanceID(material.Reference("physics-material"))));
    auto& ball=scene.CreateGameObject("Ball"); ball.GetTransform()->SetWorldPosition({0,4,0}); auto* body=ball.AddComponent<RigidbodyComponent>();
    auto* sphere=ball.AddComponent<PhysicsColliderComponent>(); sphere->SetShape(PhysicsColliderComponent::EColliderShape::SPHERE); sphere->SetRadius(0.25f);
    sphere->SetPhysicsMaterial(collider->GetPhysicsMaterial()); scene.Play(); scene.SyncTransforms(); ASSERT_TRUE(collider->IsQueryReady())<<collider->LastError();
    Physics::QueryHit hit; ASSERT_TRUE(scene.GetPhysicsWorld()->Read()->Raycast({3,10,0},{0,-1,0},20,hit)); EXPECT_NEAR(hit.position.y,0,1e-4f);
    float rebound=0; for(int i=0;i<150;++i) { scene.FixedUpdate(1.f/120); scene.SimulatePhysics(1.f/120); rebound=(std::max)(rebound,body->GetVelocity().y); }
    EXPECT_GT(rebound,5.f);
}

TEST_F(PhysicsAssetTests, SameMaterialDrivesActualThreeAndTwoDimensionalRebound)
{
    const auto material=Make("Bounce.physmat",BounceSource); CookSources({material}); Install({material});
    const ES::PPtr<EA::PhysicsMaterial> reference(ES::PersistentManager::Instance().ObjectIdentifierToInstanceID(material.Reference("physics-material")));
    Engine::SceneSystem::Scene scene;
    auto& floor=scene.CreateGameObject("Floor"); floor.GetTransform()->SetWorldPosition({0,-0.5f,0});
    auto* floor3=floor.AddComponent<PhysicsColliderComponent>(); floor3->SetSize({10,0.5f,10}); floor3->SetPhysicsMaterial(reference);
    auto* floor2=floor.AddComponent<PhysicsCollider2DComponent>(); floor2->SetSize({20,1}); floor2->SetPhysicsMaterial(reference);
    auto& ball3=scene.CreateGameObject("3D"); ball3.GetTransform()->SetWorldPosition({0,4,0}); auto* body3=ball3.AddComponent<RigidbodyComponent>();
    auto* shape3=ball3.AddComponent<PhysicsColliderComponent>(); shape3->SetShape(PhysicsColliderComponent::EColliderShape::SPHERE); shape3->SetRadius(0.25f); shape3->SetPhysicsMaterial(reference);
    auto& ball2=scene.CreateGameObject("2D"); ball2.GetTransform()->SetWorldPosition({5,4,0}); auto* body2=ball2.AddComponent<Rigidbody2DComponent>();
    auto* shape2=ball2.AddComponent<PhysicsCollider2DComponent>(); shape2->SetShape(PhysicsCollider2DComponent::EColliderShape::CIRCLE); shape2->SetRadius(0.25f); shape2->SetPhysicsMaterial(reference);
    scene.Play(); float rebound3=0,rebound2=0;
    for(int i=0;i<160;++i) { scene.FixedUpdate(1.f/120); scene.SimulatePhysics(1.f/120); rebound3=(std::max)(rebound3,body3->GetVelocity().y); rebound2=(std::max)(rebound2,body2->GetVelocity().y); }
    EXPECT_GT(rebound3,5.f); EXPECT_GT(rebound2,5.f);
}

TEST_F(PhysicsAssetTests, ResourceReloadPreservesIdentityAndRejectsHashOrGuidMismatch)
{
    const auto material=Make("Surface.physmat",BounceSource); CookSources({material}); Install({material});
    std::string error; const auto identifier=material.Reference("physics-material"); auto asset=EA::LoadPhysicsMaterial(identifier,error); ASSERT_TRUE(asset)<<error;
    const auto id=asset->GetInstanceID(); const auto oldHash=asset->ContentHash(); const auto oldData=asset->Capture();
    Write(material.path,R"({"version":1,"restitution2D":0.1})"); CookSources({material}); Install({material});
    auto next=EA::LoadPhysicsMaterial(identifier,error); ASSERT_TRUE(next)<<error; EXPECT_EQ(next,asset); EXPECT_EQ(next->GetInstanceID(),id);
    EXPECT_NE(next->ContentHash(),oldHash); EXPECT_NE(next->Capture(),oldData); EXPECT_FLOAT_EQ(next->Capture()->restitution2D,0.1f);
    const auto valid=next->Capture(); const auto entry=Manifest(material).subAssets[0];
    EXPECT_FALSE(next->Reload(root/entry.artifactPath,"sha256:incorrect",error)); EXPECT_EQ(next->Capture(),valid);
    auto foreign=identifier; foreign.guid=Guid::New(); EXPECT_FALSE(next->Reload(root/entry.artifactPath,entry.contentHash,error,&foreign)); EXPECT_EQ(next->Capture(),valid);
    auto metaReference=identifier; metaReference.fileType=ES::FileType::MetaAssetType;
    auto distinct=EA::LoadPhysicsMaterial(metaReference,error); ASSERT_TRUE(distinct)<<error;
    EXPECT_NE(distinct->GetInstanceID(),next->GetInstanceID());
    EXPECT_EQ(ES::PersistentManager::Instance().ObjectIdentifierToInstanceID(identifier),next->GetInstanceID());
    EXPECT_EQ(ES::PersistentManager::Instance().ObjectIdentifierToInstanceID(metaReference),distinct->GetInstanceID());
    auto wrongTarget=runtime->GetManifest(); wrongTarget.targetPlatform="foreign-platform";
    Core::ServiceLocator::Remove<EA::RuntimeAssetDatabase>(); runtime=std::make_unique<EA::RuntimeAssetDatabase>(std::move(wrongTarget));
    Core::ServiceLocator::Provide<EA::RuntimeAssetDatabase>(*runtime);
    EXPECT_FALSE(EA::LoadPhysicsMaterial(identifier,error)); EXPECT_EQ(next->Capture(),valid);
}

TEST_F(PhysicsAssetTests, AuthoringLookupUsesOneSnapshotAndSeesNextCommittedRevision)
{
    options.target.artifactPlatform="editor"; const auto material=Make("Surface.physmat",BounceSource); CookSources({material});
    std::string error; const auto reference=material.Reference("physics-material"); std::shared_ptr<EA::PhysicsMaterial> resident;
    {
        const EA::PhysicsAssetReadScope scope; resident=EA::LoadPhysicsMaterial(reference,error); ASSERT_TRUE(resident)<<error; const auto before=resident->Capture();
        Write(material.path,R"({"version":1,"restitution2D":0.2})"); CookSources({material});
        auto same=EA::LoadPhysicsMaterial(reference,error); ASSERT_TRUE(same)<<error; EXPECT_EQ(same->Capture(),before);
    }
    auto refreshed=EA::LoadPhysicsMaterial(reference,error); ASSERT_TRUE(refreshed)<<error; EXPECT_EQ(refreshed,resident); EXPECT_FLOAT_EQ(refreshed->Capture()->restitution2D,0.2f);
}

TEST_F(PhysicsAssetTests, SceneRoundTripAndCloneKeepTypedResourceReferencesAndIndependentBodies)
{
    const auto mesh=Make("Hull.collisionmesh",TetraSource),material=Make("Surface.physmat",BounceSource); CookSources({mesh,material}); Install({mesh,material});
    Engine::SceneSystem::Scene scene; auto& object=scene.CreateGameObject("Resource body"); auto* body=object.AddComponent<RigidbodyComponent>(); body->SetUseGravity(false);
    auto* collider=object.AddComponent<PhysicsColliderComponent>(); collider->SetShape(PhysicsColliderComponent::EColliderShape::COLLISION_MESH);
    collider->SetCollisionMesh(ES::PPtr<EA::CollisionMeshAsset>(ES::PersistentManager::Instance().ObjectIdentifierToInstanceID(mesh.Reference("collision-mesh"))));
    collider->SetPhysicsMaterial(ES::PPtr<EA::PhysicsMaterial>(ES::PersistentManager::Instance().ObjectIdentifierToInstanceID(material.Reference("physics-material"))));
    scene.Play(); scene.SyncTransforms(); ASSERT_TRUE(collider->IsQueryReady())<<collider->LastError();
    const auto text=ES::ObjectGraphWriter::Write(ES::ObjectGraphSerializer::SerializeScene(scene));
    EXPECT_NE(text.find(mesh.meta.id.ToString()),std::string::npos); EXPECT_EQ(text.find("m_meshData"),std::string::npos);
    auto document=ES::ObjectGraphReader::Read(text); ASSERT_TRUE(document); auto clone=ES::ObjectGraphInstantiator::InstantiateScene(*document); ASSERT_TRUE(clone);
    auto* copy=clone->FindGameObjectByName("Resource body")->GetComponent<PhysicsColliderComponent>(); ASSERT_NE(copy,nullptr);
    EXPECT_EQ(copy->GetPhysicsMaterial(),collider->GetPhysicsMaterial()); EXPECT_EQ(copy->GetCollisionMesh(),collider->GetCollisionMesh());
    clone->Play(); clone->SyncTransforms(); ASSERT_TRUE(copy->IsQueryReady())<<copy->LastError(); EXPECT_NE(copy->GetBodyHandle(),body->GetBodyHandle());
    Physics::QueryHit hit; ASSERT_TRUE(clone->GetPhysicsWorld()->Read()->Raycast({-2,0.25f,0.25f},{1,0,0},5,hit)); EXPECT_NEAR(hit.distance,2,1e-4f);
}

TEST_F(PhysicsAssetTests, InvalidResourceAndUnsupportedMeshTransformRetainLastValidShape)
{
    const auto mesh=Make("Hull.collisionmesh",TetraSource); CookSources({mesh}); Install({mesh}); Engine::SceneSystem::Scene scene;
    auto& object=scene.CreateGameObject("Collider"); auto* collider=object.AddComponent<PhysicsColliderComponent>(); collider->SetShape(PhysicsColliderComponent::EColliderShape::COLLISION_MESH);
    collider->SetCollisionMesh(ES::PPtr<EA::CollisionMeshAsset>(ES::PersistentManager::Instance().ObjectIdentifierToInstanceID(mesh.Reference("collision-mesh"))));
    scene.SyncTransforms(); ASSERT_TRUE(collider->IsQueryReady())<<collider->LastError(); const auto original=collider->GetShapeHandle();
    object.GetTransform()->SetLocalScale({-1,1,1}); scene.SyncTransforms(); EXPECT_EQ(collider->GetShapeHandle(),original); EXPECT_NE(collider->LastError().find("bakeScale"),std::string::npos);
    object.GetTransform()->SetLocalScale({1,1,1}); auto missing=mesh.Reference("collision-mesh"); missing.guid=Guid::New();
    collider->SetCollisionMesh(ES::PPtr<EA::CollisionMeshAsset>(ES::PersistentManager::Instance().ObjectIdentifierToInstanceID(missing))); scene.SyncTransforms();
    EXPECT_TRUE(collider->IsQueryReady()); EXPECT_EQ(collider->GetShapeHandle(),original); EXPECT_FALSE(collider->LastError().empty());
}

TEST_F(PhysicsAssetTests, ShapeParserValidatesStableIdsTransformsReferencesAndCanonicalOrdering)
{
    std::string error; CA::ArtifactPayload result; result.payload={7};
    for(const auto* source:{R"({"version":1,"shapes":[]})",R"({"version":1,"shapes":[{"id":0,"type":"box"}]})",
        R"({"version":1,"shapes":[{"id":1,"type":"box"},{"id":1,"type":"sphere"}]})",
        R"({"version":1,"shapes":[{"id":1,"type":"box","rotation":[0,0,0,0]}]})",
        R"({"version":1,"shapes":[{"id":1,"type":"box","scale":[1,0,1]}]})",
        R"({"version":1,"shapes":[{"id":1,"type":"box","position":[1e100,0,0]}]})",
        R"({"version":1,"shapes":[{"id":1,"type":"sphere","halfExtents":[1,1,1]}]})",
        R"({"version":1,"shapes":[{"id":1,"type":"mesh","mesh":{"guid":"invalid"}}]})",
        R"({"version":1,"shapes":[{"id":1,"id":2,"type":"box"}]})"})
    { EXPECT_FALSE(EA::CompilePhysicsAsset(CA::AssetType::PhysicsShape,source,"editor",result,error))<<source; EXPECT_EQ(result.payload,std::vector<uint8_t>{7}); }
    CA::ArtifactPayload first,second;
    ASSERT_TRUE(EA::CompilePhysicsAsset(CA::AssetType::PhysicsShape,R"({"version":1,"shapes":[{"id":9,"type":"sphere"},{"id":2,"type":"box","rotation":[0,0,0,2]}]})","editor",first,error))<<error;
    ASSERT_TRUE(EA::CompilePhysicsAsset(CA::AssetType::PhysicsShape,R"({"version":1,"shapes":[{"type":"box","id":2},{"type":"sphere","id":9}]})","editor",second,error))<<error;
    EXPECT_EQ(first.payload,second.payload); const auto parsed=EA::ParsePhysicsShape(std::string(first.payload.begin(),first.payload.end()),error);
    ASSERT_TRUE(parsed); ASSERT_EQ(parsed->parts.size(),2u); EXPECT_EQ(parsed->parts[0].id,2u); EXPECT_EQ(parsed->parts[1].id,9u);
    const auto guid=Guid::New().ToString();
    for(const auto suffix:{std::string(",\"type\":0"),std::string(",\"fileID\":18446744073709551615"),std::string(",\"fileID\":2")})
    {
        const auto source="{\"version\":1,\"shapes\":[{\"id\":1,\"type\":\"mesh\",\"mesh\":{\"guid\":\""+guid+"\""+suffix+"}}]}";
        EXPECT_FALSE(EA::ParsePhysicsShape(source,error));
    }
    std::string tooMany="{\"version\":1,\"shapes\":[";
    for(uint32_t i=1;i<=257;++i) tooMany+=(i==1?"":",")+std::string("{\"id\":")+std::to_string(i)+",\"type\":\"box\"}";
    tooMany+="]}"; EXPECT_FALSE(EA::ParsePhysicsShape(tooMany,error));
}

TEST_F(PhysicsAssetTests, ShapeResourcePublishesActualRotatedPrimitiveGeometryAndReflectedOffsets)
{
    const auto source=Make("Mixed.physshape",MixedShapeSource); EXPECT_EQ(source.meta.assetType,CA::AssetType::PhysicsShape);
    const auto metaTime=std::filesystem::last_write_time(CA::GetAssetMetaPath(source.path)); CookSources({source}); Install({source});
    EXPECT_EQ(std::filesystem::last_write_time(CA::GetAssetMetaPath(source.path)),metaTime);
    Engine::SceneSystem::Scene scene; auto* collider=ShapeCollider(scene,source); scene.SyncTransforms();
    ASSERT_TRUE(collider->IsQueryReady())<<collider->LastError(); ASSERT_EQ(collider->GetShapeHandles().size(),4u);
    const auto original=collider->GetShapeHandles(); Physics::QueryHit hit;
    for(const auto& sample:std::vector<std::pair<float,float>>{{-2,2},{2,0.5f},{6,1.25f},{10,1}})
    {
        ASSERT_TRUE(scene.GetPhysicsWorld()->Read()->Raycast({sample.first,5,0},{0,-1,0},10,hit));
        EXPECT_NEAR(hit.position.y,sample.second,0.02f);
    }
    EXPECT_FALSE(scene.GetPhysicsWorld()->Read()->Raycast({0,5,0},{0,-1,0},10,hit));
    scene.SyncTransforms(); EXPECT_EQ(collider->GetShapeHandles(),original);
    collider->gameobject()->GetTransform()->SetLocalScale({-1,1,1}); scene.SyncTransforms(); ASSERT_TRUE(collider->LastError().empty())<<collider->LastError();
    ASSERT_TRUE(scene.GetPhysicsWorld()->Read()->Raycast({-10,5,0},{0,-1,0},10,hit)); EXPECT_EQ(hit.subShapeId,43u);
    for(auto handle:original) EXPECT_FALSE(scene.GetPhysicsWorld()->IsValid(handle));
    EXPECT_EQ(scene.GetPhysicsWorld()->GetStatistics().bodyCount,1u); EXPECT_EQ(scene.GetPhysicsWorld()->GetStatistics().shapeCount,4u);
}

TEST_F(PhysicsAssetTests, ShapeDependenciesPackageTransitivelyAndLoadWithoutSourcesOrArtifactDatabase)
{
    const auto mesh=Make("Floor.collisionmesh",FloorSource),material=Make("Bounce.physmat",BounceSource);
    const auto source=Make("Floor.physshape","{\"version\":1,\"shapes\":[{\"id\":88,\"type\":\"mesh\",\"mesh\":{\"guid\":\""+mesh.meta.id.ToString()+"\"},\"material\":{\"guid\":\""+material.meta.id.ToString()+"\"}}]}");
    CookSources({mesh,material,source}); EA::RuntimeManifestBuilder builder;
    for(const auto& item:{mesh,material,source}) builder.AddArtifactManifest(Manifest(item));
    const auto built=builder.Build({{source.meta.id,"physics-shape:main"}},options.target.artifactPlatform);
    ASSERT_FALSE(built.diagnostics.HasErrors()); ASSERT_EQ(built.manifest.entries.size(),3u);
    EA::RuntimeManifestBuilder missing; missing.AddArtifactManifest(Manifest(source));
    EXPECT_TRUE(missing.Build({{source.meta.id,"physics-shape:main"}},options.target.artifactPlatform).diagnostics.HasErrors());
    runtime=std::make_unique<EA::RuntimeAssetDatabase>(built.manifest); Core::ServiceLocator::Provide<EA::RuntimeAssetDatabase>(*runtime);
    for(const auto& item:{mesh,material,source}) { std::filesystem::remove(item.path); std::filesystem::remove(CA::GetAssetMetaPath(item.path)); }
    std::filesystem::rename(options.artifactDatabasePath,root/"Library/RetiredArtifactDB");
    Engine::SceneSystem::Scene scene; auto* floor=ShapeCollider(scene,source); auto& ball=scene.CreateGameObject("Ball");
    ball.GetTransform()->SetWorldPosition({0,4,0}); auto* body=ball.AddComponent<RigidbodyComponent>();
    auto* collider=ball.AddComponent<PhysicsColliderComponent>(); collider->SetShape(PhysicsColliderComponent::EColliderShape::SPHERE); collider->SetRadius(0.25f); collider->SetRestitution(0);
    scene.Play(); scene.SyncTransforms(); ASSERT_TRUE(floor->IsQueryReady())<<floor->LastError();
    Physics::QueryHit hit; ASSERT_TRUE(scene.GetPhysicsWorld()->Read()->Raycast({3,5,0},{0,-1,0},10,hit)); EXPECT_EQ(hit.subShapeId,88u);
    float rebound=0; for(int i=0;i<150;++i) { scene.FixedUpdate(1.f/120); scene.SimulatePhysics(1.f/120); rebound=(std::max)(rebound,body->GetVelocity().y); }
    EXPECT_GT(rebound,5.f);
}

TEST_F(PhysicsAssetTests, ShapeReloadAndChildMeshReloadReplaceAllShapesWhileKeepingBodyAndStablePartIds)
{
    const auto mesh=Make("Hull.collisionmesh",TetraSource);
    const auto source=Make("Hull.physshape","{\"version\":1,\"shapes\":[{\"id\":9,\"type\":\"mesh\",\"mesh\":{\"guid\":\""+mesh.meta.id.ToString()+"\"}},{\"id\":2,\"type\":\"box\",\"position\":[-3,0,0]}]}");
    CookSources({mesh,source}); Install({mesh,source}); Engine::SceneSystem::Scene scene; auto* collider=ShapeCollider(scene,source); scene.SyncTransforms();
    ASSERT_TRUE(collider->IsQueryReady())<<collider->LastError(); const auto body=collider->GetBodyHandle(); const auto old=collider->GetShapeHandles();
    const auto shapeHash=Manifest(source).subAssets[0].contentHash;
    auto expanded=TetraSource; expanded.insert(expanded.size()-1,",\"bakeScale\":[2,1,1]"); Write(mesh.path,expanded); CookSources({mesh}); Install({mesh,source}); scene.SyncTransforms();
    EXPECT_EQ(collider->GetBodyHandle(),body); EXPECT_EQ(Manifest(source).subAssets[0].contentHash,shapeHash);
    for(auto handle:old) EXPECT_FALSE(scene.GetPhysicsWorld()->IsValid(handle));
    Physics::QueryHit hit; ASSERT_TRUE(scene.GetPhysicsWorld()->Read()->Raycast({5,0.1f,0.1f},{-1,0,0},8,hit)); EXPECT_EQ(hit.subShapeId,9u); EXPECT_GT(hit.position.x,3.5f);
    const auto before=collider->GetShapeHandles(); const auto changed="{\"version\":1,\"shapes\":[{\"id\":9,\"type\":\"sphere\",\"position\":[3,0,0]},{\"id\":2,\"type\":\"box\",\"position\":[-3,0,0]}]}";
    Write(source.path,changed); CookSources({source}); Install({mesh,source}); scene.SyncTransforms();
    EXPECT_EQ(collider->GetBodyHandle(),body); EXPECT_EQ(collider->GetShapeHandles().size(),2u);
    for(auto handle:before) EXPECT_FALSE(scene.GetPhysicsWorld()->IsValid(handle));
    ASSERT_TRUE(scene.GetPhysicsWorld()->Read()->Raycast({3,3,0},{0,-1,0},6,hit)); EXPECT_EQ(hit.subShapeId,9u); EXPECT_NEAR(hit.position.y,0.5f,0.001f);
}

TEST_F(PhysicsAssetTests, InvalidShapeResourceTransformsAndMissingDependenciesRetainTheWholePreviousGroup)
{
    const auto source=Make("Mixed.physshape",MixedShapeSource); CookSources({source}); Install({source});
    Engine::SceneSystem::Scene scene; auto* collider=ShapeCollider(scene,source); scene.SyncTransforms(); ASSERT_TRUE(collider->IsQueryReady());
    const auto original=collider->GetShapeHandles(); collider->gameobject()->GetTransform()->SetLocalScale({2,1,1}); scene.SyncTransforms();
    EXPECT_EQ(collider->GetShapeHandles(),original); EXPECT_NE(collider->LastError().find("sphere"),std::string::npos);
    collider->gameobject()->GetTransform()->SetLocalScale({1,1,1}); scene.SyncTransforms(); EXPECT_TRUE(collider->LastError().empty());
    const auto broken="{\"version\":1,\"shapes\":[{\"id\":7,\"type\":\"box\"},{\"id\":9,\"type\":\"mesh\",\"mesh\":{\"guid\":\""+Guid::New().ToString()+"\"}}]}";
    Write(source.path,broken); CookSources({source}); // Authoring can retain a dangling reference, but packaging and runtime must diagnose it.
    EA::RuntimeManifestBuilder missing; missing.AddArtifactManifest(Manifest(source));
    EXPECT_TRUE(missing.Build({{source.meta.id,"physics-shape:main"}},options.target.artifactPlatform).diagnostics.HasErrors());
    Core::ServiceLocator::Remove<EA::RuntimeAssetDatabase>(); runtime.reset(); options.target.artifactPlatform="editor"; CookSources({source});
    scene.SyncTransforms(); EXPECT_FALSE(collider->LastError().empty()); EXPECT_EQ(collider->GetShapeHandles(),original);
    EXPECT_TRUE(collider->IsQueryReady()); EXPECT_EQ(scene.GetPhysicsWorld()->GetStatistics().shapeCount,4u);
    Write(source.path,R"({"version":1,"shapes":[{"id":7,"type":"box","rotation":[0,0,0.38268343,0.92387953]}]})"); CookSources({source});
    collider->gameobject()->GetTransform()->SetLocalScale({2,1,1}); scene.SyncTransforms();
    EXPECT_NE(collider->LastError().find("shear"),std::string::npos); EXPECT_EQ(collider->GetShapeHandles(),original);
}

TEST_F(PhysicsAssetTests, CompoundSceneRoundTripCloneDisableAndExternalShapeRemovalKeepIndependentOwnership)
{
    const auto source=Make("Mixed.physshape",MixedShapeSource); CookSources({source}); Install({source}); Engine::SceneSystem::Scene scene;
    auto* collider=ShapeCollider(scene,source); auto* body=collider->gameobject()->AddComponent<RigidbodyComponent>(); body->SetUseGravity(false);
    scene.Play(); scene.SyncTransforms(); ASSERT_TRUE(collider->IsQueryReady())<<collider->LastError();
    const auto original=collider->GetShapeHandles(); const auto handle=body->GetBodyHandle();
    ASSERT_TRUE(scene.GetPhysicsWorld()->Remove(original.back())); EXPECT_FALSE(collider->IsQueryReady()); scene.SyncTransforms();
    ASSERT_TRUE(collider->IsQueryReady())<<collider->LastError(); EXPECT_EQ(collider->GetShapeHandles().size(),4u);
    for(auto shape:original) EXPECT_FALSE(scene.GetPhysicsWorld()->IsValid(shape)); EXPECT_EQ(body->GetBodyHandle(),handle);
    const auto text=ES::ObjectGraphWriter::Write(ES::ObjectGraphSerializer::SerializeScene(scene));
    EXPECT_NE(text.find(source.meta.id.ToString()),std::string::npos); EXPECT_EQ(text.find("m_shapes"),std::string::npos);
    auto document=ES::ObjectGraphReader::Read(text); ASSERT_TRUE(document); auto clone=ES::ObjectGraphInstantiator::InstantiateScene(*document); ASSERT_TRUE(clone);
    auto* copy=clone->FindGameObjectByName("Compound")->GetComponent<PhysicsColliderComponent>(); ASSERT_TRUE(copy); EXPECT_EQ(copy->GetShapeResource(),collider->GetShapeResource());
    clone->Play(); clone->SyncTransforms(); ASSERT_TRUE(copy->IsQueryReady())<<copy->LastError(); EXPECT_NE(copy->GetBodyHandle(),handle);
    collider->SetEnabled(false); scene.SyncTransforms(); EXPECT_EQ(scene.GetPhysicsWorld()->GetStatistics().shapeCount,0u); EXPECT_TRUE(scene.GetPhysicsWorld()->IsValid(handle));
    EXPECT_EQ(clone->GetPhysicsWorld()->GetStatistics().shapeCount,4u); collider->SetEnabled(true); scene.SyncTransforms(); EXPECT_EQ(scene.GetPhysicsWorld()->GetStatistics().shapeCount,4u);
    scene.Stop(); scene.Play(); scene.SyncTransforms(); EXPECT_EQ(scene.GetPhysicsWorld()->GetStatistics().shapeCount,4u); EXPECT_EQ(scene.GetPhysicsWorld()->GetStatistics().bodyCount,1u);
}

TEST_F(PhysicsAssetTests, ShapeMeshReflectionAndConcavityRejectUnsupportedChangesWithoutDiscardingGeometry)
{
    const auto mesh=Make("Floor.collisionmesh",FloorSource);
    const auto source=Make("Floor.physshape","{\"version\":1,\"shapes\":[{\"id\":7,\"type\":\"box\",\"position\":[-20,0,0]},{\"id\":19,\"type\":\"mesh\",\"mesh\":{\"guid\":\""+mesh.meta.id.ToString()+"\"}}]}");
    CookSources({mesh,source}); Install({mesh,source}); Engine::SceneSystem::Scene scene; auto* collider=ShapeCollider(scene,source); scene.SyncTransforms();
    ASSERT_TRUE(collider->IsQueryReady())<<collider->LastError(); const auto old=collider->GetShapeHandles();
    collider->gameobject()->GetTransform()->SetLocalScale({-1,1,1}); scene.SyncTransforms(); EXPECT_NE(collider->LastError().find("bakeScale"),std::string::npos);
    EXPECT_EQ(collider->GetShapeHandles(),old); collider->gameobject()->GetTransform()->SetLocalScale({1,1,1}); collider->SetTrigger(true); scene.SyncTransforms();
    EXPECT_NE(collider->LastError().find("static body"),std::string::npos); EXPECT_EQ(collider->GetShapeHandles(),old);
    collider->SetTrigger(false); scene.SyncTransforms(); EXPECT_TRUE(collider->LastError().empty());
    auto& mover=scene.CreateGameObject("Dynamic"); mover.GetTransform()->SetWorldPosition({0,5,0});
    mover.AddComponent<RigidbodyComponent>(); auto* dynamic=mover.AddComponent<PhysicsColliderComponent>(); scene.Play(); scene.SyncTransforms();
    const auto originalDynamic=dynamic->GetShapeHandles(); ASSERT_EQ(originalDynamic.size(),1u);
    dynamic->SetShapeResource(collider->GetShapeResource()); dynamic->SetShape(PhysicsColliderComponent::EColliderShape::SHAPE_ASSET); scene.SyncTransforms();
    EXPECT_NE(dynamic->LastError().find("static body"),std::string::npos); EXPECT_EQ(dynamic->GetShapeHandles(),originalDynamic);
    EXPECT_EQ(scene.GetPhysicsWorld()->GetStatistics().shapeCount,3u);
}

TEST_F(PhysicsAssetTests, CompoundChildUsesRigidbodyWorldPoseAndCallbackDestructionReleasesEveryPart)
{
    const auto source=Make("Pair.physshape",R"({"version":1,"shapes":[{"id":7,"type":"box","position":[-1,0,0]},{"id":19,"type":"sphere","position":[1,0,0]}]})");
    CookSources({source}); Install({source}); Engine::SceneSystem::Scene scene; auto& rootObject=scene.CreateGameObject("Root");
    rootObject.GetTransform()->SetWorldPosition({10,0,0}); rootObject.GetTransform()->SetWorldRotation(Maths::Quaternion{0,0.70710678f,0,0.70710678f});
    auto* body=rootObject.AddComponent<RigidbodyComponent>(); body->SetUseGravity(false);
    auto* collider=ShapeCollider(scene,source,"Child"); collider->gameobject()->SetParent(rootObject); collider->gameobject()->GetTransform()->SetLocalPosition({0,0,2});
    scene.Play(); scene.SyncTransforms(); ASSERT_TRUE(collider->IsQueryReady())<<collider->LastError(); EXPECT_EQ(collider->GetBodyHandle(),body->GetBodyHandle());
    const auto shapes=collider->GetShapeHandles(); const auto bodyHandle=body->GetBodyHandle(); Physics::QueryHit hit;
    ASSERT_TRUE(scene.GetPhysicsWorld()->Read()->Raycast({12,4,1},{0,-1,0},8,hit)); EXPECT_EQ(hit.subShapeId,7u);
    ASSERT_TRUE(scene.GetPhysicsWorld()->Read()->Raycast({12,4,-1},{0,-1,0},8,hit)); EXPECT_EQ(hit.subShapeId,19u);
    auto& trigger=scene.CreateGameObject("Trigger"); trigger.GetTransform()->SetWorldPosition({12,0,0});
    auto* sensor=trigger.AddComponent<PhysicsColliderComponent>(); sensor->SetSize({2,2,2}); sensor->SetTrigger(true); scene.SyncTransforms();
    bool removed=false; const auto owner=static_cast<uint64_t>(collider->GetInstanceID());
    scene.PhysicsContactEvent.AddListener([&](const Physics::ContactEvent& event)
    {
        if(removed || event.phase!=Physics::ContactPhase::Enter || (event.firstOwner!=owner && event.secondOwner!=owner)) return;
        removed=true; EXPECT_TRUE(scene.DestroyGameObject(rootObject));
        for(auto handle:shapes) EXPECT_TRUE(scene.GetPhysicsWorld()->IsValid(handle));
    });
    scene.FixedUpdate(1.f/60); scene.SimulatePhysics(1.f/60); ASSERT_TRUE(removed); scene.SyncTransforms();
    EXPECT_FALSE(scene.GetPhysicsWorld()->IsValid(bodyHandle)); for(auto handle:shapes) EXPECT_FALSE(scene.GetPhysicsWorld()->IsValid(handle));
    EXPECT_EQ(scene.GetPhysicsWorld()->GetStatistics().bodyCount,1u); EXPECT_EQ(scene.GetPhysicsWorld()->GetStatistics().shapeCount,1u);
}
