#include <gtest/gtest.h>
#include "Animation/AnimationDocument.h"
#include "Assets/EditorAssetDatabase.h"
#include "Assets/AssetDatabaseFacade.h"
#include <Components/AnimatorComponent.h>
#include <Components/SkinnedMeshRenderer.h>
#include <Engine/SceneSystem/Scene.h>
#include <Engine/Assets/ModelPrefabBuilder.h>
#include <Rendering/Assets/SceneImportPipeline.h>
#include <Rendering/Assets/SceneModelReader.h>
#include <Assets/AssetMeta.h>
#include <Assets/NativeArtifactContainer.h>
#include <Assets/ArtifactWriter.h>
#include <AnimationCompiler/AnimationSourceCompiler.h>
#include <AnimationCompiler/GltfAnimationImporter.h>
#include <Components/CameraComponent.h>
#include <Components/LightComponent.h>
#include <Rendering/Entities/Camera.h>
#include <Rendering/Geometry/BoundingSphereUtils.h>
#include <Serialize/ObjectGraphSerializer.h>
#include <Serialize/ObjectGraphInstantiator.h>
#include <Serialize/ObjectGraphWriter.h>
#include <Serialize/ObjectGraphReader.h>
#include <fstream>

namespace
{
namespace A=NLS::Animation;namespace C=NLS::AnimationCompiler;namespace E=NLS::Engine;namespace T=NLS::Editor::AnimationTools;
std::shared_ptr<A::Model> Model()
{
    auto m=std::make_shared<A::Model>();m->skeleton.id="rig";m->skeleton.bones={{"root","root",A::NoBone,{}}};
    A::Clip c;c.id="Move";c.name="Move";c.skeleton="rig";c.duration=1;A::BoneTrack track;track.bone="root";track.translation.times={0,1};track.translation.values={{0,0,0,0},{2,0,0,0}};c.tracks={track};m->clips={c};return m;
}
TEST(AnimationDocument,UndoRedoRestoresTypedGraphAndSavedAppliedAreIndependent)
{
    NLS::Editor::Undo::EditorUndoService undo;T::AnimationDocument document(&undo);auto asset=std::get<A::GraphAsset>(document.Value());asset.name="Changed";
    ASSERT_TRUE(document.Edit(asset,"Rename"));EXPECT_EQ(std::get<A::GraphAsset>(document.Value()).name,"Changed");ASSERT_TRUE(undo.RequestUndo().Succeeded());EXPECT_NE(std::get<A::GraphAsset>(document.Value()).name,"Changed");ASSERT_TRUE(undo.RequestRedo().Succeeded());EXPECT_EQ(std::get<A::GraphAsset>(document.Value()).name,"Changed");
    document.MarkApplied("snapshot");EXPECT_EQ(document.State().applied,document.State().source);EXPECT_NE(document.State().saved,document.State().source);
}
TEST(AnimationDocument,ClosedDocumentMakesOldUndoCommandStale)
{
    NLS::Editor::Undo::EditorUndoService undo;
    {T::AnimationDocument document(&undo);auto asset=std::get<A::GraphAsset>(document.Value());asset.name="Old";ASSERT_TRUE(document.Edit(asset,"Rename"));}
    EXPECT_FALSE(undo.RequestUndo().Succeeded());
}
TEST(AnimationScene,GameplayParametersAdvanceOneAnimationFrameAndPublishNotifiesOnce)
{
    E::SceneSystem::Scene scene;auto& actor=scene.CreateGameObject("Character");auto* animator=actor.AddComponent<E::Components::AnimatorComponent>();auto model=Model();model->clips[0].notifies={{1,"Footstep","",.25,.25,.01f}};
    std::string error;ASSERT_TRUE(animator->SetModelClip(model,0,error));size_t events=0;animator->NotifyEvent.AddListener([&](const auto& event){if(event.name=="Footstep")++events;});
    scene.Update(.5f);EXPECT_NEAR(animator->CaptureFrame()->local[0].translation.x,1,1e-6);EXPECT_EQ(events,1u);animator->Finalize();EXPECT_EQ(events,1u);
    animator->Seek(.1f);EXPECT_EQ(events,1u);scene.Update(0);EXPECT_EQ(events,1u);
}
TEST(AnimationScene,ProgramRejectionLeavesLastPublishedPose)
{
    E::Components::AnimatorComponent animator;auto model=Model();std::string error;ASSERT_TRUE(animator.SetModelClip(model,0,error));animator.Evaluate(.3);auto frame=animator.CaptureFrame();EXPECT_FALSE(animator.SetCompiledProgram({},error));EXPECT_EQ(frame,animator.CaptureFrame());
}
TEST(AnimationScene,GeneratedCharacterPrefabContainsAnimatorAndSkinnedRenderer)
{
    namespace Assets=NLS::Core::Assets;namespace Render=NLS::Render;
    const auto path=std::filesystem::path(NLS_ROOT_DIR)/"Templates/Animation/TwoBoneCharacter.gltf";
    auto meta=Assets::AssetMeta::Load(Assets::GetAssetMetaPath(path));ASSERT_TRUE(meta);
    Render::Assets::ImportedScene scene;std::vector<Render::Resources::Parsers::ParsedMeshData> meshes;std::vector<std::string> materials,dependencies;std::string error;
    ASSERT_TRUE(Render::Assets::ReadModelSource(path,*meta,Render::Resources::Parsers::EModelParserFlags::TRIANGULATE,meshes,materials,dependencies,scene,error))<<error;
    ASSERT_TRUE(scene.animationData);const auto generated=Render::Assets::GenerateSceneSubAssets(scene);
    auto result=E::Assets::BuildGeneratedModelPrefab(scene,generated,{});
    bool animator=false,skinned=false;for(const auto& object:result.artifact.graph.objects)
    {animator|=object.typeName.find("AnimatorComponent")!=std::string::npos;skinned|=object.typeName.find("SkinnedMeshRenderer")!=std::string::npos;}
    EXPECT_TRUE(animator);EXPECT_TRUE(skinned);for(const auto& issue:result.diagnostics.GetItems())EXPECT_EQ(issue.GetMessage().find("runtime-skinning-component-missing"),std::string::npos);
}
TEST(AnimationSourceCompiler,AuthoredClipAndGraphResolveFrozenNativeDependencies)
{
    namespace Assets=NLS::Core::Assets;
    const auto root=std::filesystem::temp_directory_path()/("NullusAnimationSource-"+NLS::Guid::New().ToString());std::filesystem::create_directories(root/"Assets");
    struct Cleanup{std::filesystem::path root;~Cleanup(){std::error_code ignored;std::filesystem::remove_all(root,ignored);}}cleanup{root};
    auto model=Model();const auto guid=Assets::AssetId::New();Assets::ArtifactDatabase database;Assets::ArtifactManifest manifest;manifest.sourceAssetId=guid;manifest.targetPlatform="editor";manifest.primarySubAssetKey="animation:Move";
    auto publish=[&](const char* key,Assets::ArtifactType type,const A::Model& data)
    {
        Assets::NativeArtifactMetadata metadata;metadata.artifactType=type;metadata.sourceAssetId=guid;metadata.subAssetKey=key;metadata.schemaName="animation";
        auto bytes=Assets::WriteNativeArtifactContainer(metadata,A::SerializeModel(data));const auto hash=Assets::BuildArtifactStorageFileName(bytes.data(),bytes.size());const auto path=root/"Library/Artifacts"/hash.substr(0,2)/hash;std::filesystem::create_directories(path.parent_path());std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());file.close();
        manifest.subAssets.push_back({guid,key,type,type==Assets::ArtifactType::Skeleton?"skeleton":"animation","editor",path.lexically_relative(root).generic_string(),hash,key});
    };
    publish("animation:Move",Assets::ArtifactType::AnimationClip,*model);auto skeleton=*model;skeleton.clips.clear();publish("skeleton:Rig",Assets::ArtifactType::Skeleton,skeleton);database.UpsertManifest(manifest,"Assets/Character.glb",Assets::ArtifactRecordStatus::UpToDate);
    A::ClipAsset clip;clip.id="trimmed";clip.source=guid.ToString()+"#animation:Move";clip.start=.25;clip.end=.75;C::SourceCompileResult result;std::string error;
    ASSERT_TRUE(C::CompileSource(root,database,"editor",root/"Assets/Move.animclip",C::Serialize(clip),result,error))<<error;ASSERT_TRUE(result.model);EXPECT_DOUBLE_EQ(result.model->clips[0].duration,.5);
    auto graph=C::CreateGraph();graph.skeleton=guid.ToString()+"#skeleton:Rig";graph.nodes[0].kind=A::NodeKind::SequencePlayer;graph.nodes[0].clip=guid.ToString()+"#animation:Move";
    ASSERT_TRUE(C::CompileSource(root,database,"editor",root/"Assets/Hero.animgraph",C::Serialize(graph),result,error))<<error;ASSERT_TRUE(result.program);EXPECT_FALSE(result.dependencies.empty());
    auto restored=C::DeserializeProgram(result.payload.payload,error);ASSERT_TRUE(restored)<<error;A::AnimationInstance instance;ASSERT_TRUE(instance.SetProgram(restored,error));ASSERT_TRUE(instance.Tick(.5,error));EXPECT_NEAR(instance.Capture()->local[0].translation.x,1,1e-6);
}
TEST(AnimationScene,SceneAndPrefabRoundTripKeepSharedAnimatorReferencesAndPlaybackSettings)
{
    namespace S=E::Serialize;
    E::SceneSystem::Scene scene;
    // Allocate the renderers first to exercise forward component references.
    auto& body=scene.CreateGameObject("Body");auto* bodyRenderer=body.AddComponent<E::Components::SkinnedMeshRenderer>();
    auto& gear=scene.CreateGameObject("Gear");auto* gearRenderer=gear.AddComponent<E::Components::SkinnedMeshRenderer>();
    auto& actor=scene.CreateGameObject("Character");auto* animator=actor.AddComponent<E::Components::AnimatorComponent>();
    body.SetParent(actor);gear.SetParent(actor);animator->SetPlaybackSpeed(-.75f);animator->SetLooping(false);
    bodyRenderer->SetAnimatorReference(S::PPtr<E::Components::Component>(animator));
    gearRenderer->SetAnimatorReference(S::PPtr<E::Components::Component>(animator));
    const auto document=S::ObjectGraphSerializer::SerializeScene(scene);
    ASSERT_FALSE(document.Validate().HasErrors());
    auto loaded=S::ObjectGraphInstantiator::InstantiateScene(document,S::LoadPolicy{});
    ASSERT_TRUE(loaded.scene);ASSERT_FALSE(loaded.diagnostics.HasErrors());
    auto verify=[&](E::SceneSystem::Scene& restored)
    {
        E::Components::AnimatorComponent* controller=nullptr;
        std::vector<E::Components::SkinnedMeshRenderer*> renderers;
        for(auto* object:restored.GetGameObjects())
        {if(auto* c=object->GetComponent<E::Components::AnimatorComponent>())controller=c;if(auto* r=object->GetComponent<E::Components::SkinnedMeshRenderer>())renderers.push_back(r);}
        ASSERT_NE(controller,nullptr);EXPECT_NE(controller,animator);EXPECT_FLOAT_EQ(controller->GetPlaybackSpeed(),-.75f);EXPECT_FALSE(controller->GetLooping());
        ASSERT_EQ(renderers.size(),2u);for(auto* renderer:renderers)EXPECT_EQ(renderer->GetAnimatorReference().Get(),controller);
    };
    verify(*loaded.scene);
    const auto prefab=S::ObjectGraphSerializer::SerializePrefab(actor);
    E::SceneSystem::Scene prefabScene;auto first=S::ObjectGraphInstantiator::InstantiatePrefab(prefab,prefabScene);
    ASSERT_NE(first.root,nullptr);verify(prefabScene);
    auto second=S::ObjectGraphInstantiator::InstantiatePrefab(prefab,prefabScene);ASSERT_NE(second.root,nullptr);
    auto* secondAnimator=second.root->GetComponent<E::Components::AnimatorComponent>();
    ASSERT_NE(secondAnimator,first.root->GetComponent<E::Components::AnimatorComponent>());
    for(auto* child:second.root->GetChildren())EXPECT_EQ(child->GetComponent<E::Components::SkinnedMeshRenderer>()->GetAnimatorReference().Get(),secondAnimator);
}
TEST(AnimationDocument,ColdStartupPlansAllAuthoringAssetsInDependencyOrder)
{
    const auto root=std::filesystem::temp_directory_path()/("NullusAnimationPlan-"+NLS::Guid::New().ToString());std::filesystem::create_directories(root/"Assets");
    struct Cleanup{std::filesystem::path root;~Cleanup(){std::error_code ignored;std::filesystem::remove_all(root,ignored);}}cleanup{root};
    for(const char* name:{"Hero.animgraph","Walk.animclip","Locomotion.blendspace","UpperBody.bonemask"})std::ofstream(root/"Assets"/name)<<"{}";
    NLS::Editor::Assets::AssetDatabaseFacade database(std::vector<std::filesystem::path>{root/"Assets"});ASSERT_TRUE(database.Refresh());
    const auto plan=NLS::Editor::Assets::AssetPreimportScheduler{}.BuildPlan(database);
    ASSERT_EQ(plan.assetPaths.size(),4u);EXPECT_EQ(std::filesystem::path(plan.assetPaths.front()).extension(),".animclip");EXPECT_EQ(std::filesystem::path(plan.assetPaths.back()).extension(),".animgraph");
}
TEST(AnimationScene,DeferredSceneLoadingRetainsAnimationAssetReferences)
{
    namespace S=E::Serialize;
    E::SceneSystem::Scene source;auto& object=source.CreateGameObject("Character");object.AddComponent<E::Components::AnimatorComponent>();object.AddComponent<E::Components::SkinnedMeshRenderer>();
    auto document=S::ObjectGraphSerializer::SerializeScene(source);
    const S::AssetId guid(NLS::Guid::New());const auto graph=S::ObjectIdentifier::Asset(guid,101,"animation-graph:Hero");const auto mesh=S::ObjectIdentifier::Asset(guid,102,"skinned-mesh:Body");
    for(auto& record:document.objects)for(auto& property:record.properties)
    {if(record.typeName==E::Components::AnimatorComponent::StaticMetaTypeName()&&property.name=="animation")property.value=S::PropertyValue::ObjectReference(graph);if(record.typeName==E::Components::SkinnedMeshRenderer::StaticMetaTypeName()&&property.name=="mesh")property.value=S::PropertyValue::ObjectReference(mesh);}
    S::LoadPolicy policy;policy.deferAssetReferenceResolution=true;
    auto result=S::ObjectGraphInstantiator::InstantiateScene(document,policy);ASSERT_TRUE(result.scene);
    for(auto* restored:result.scene->GetGameObjects())
    {
        auto* animator=restored->GetComponent<E::Components::AnimatorComponent>();auto* skin=restored->GetComponent<E::Components::SkinnedMeshRenderer>();ASSERT_NE(animator,nullptr);ASSERT_NE(skin,nullptr);
        S::ObjectIdentifier actual;ASSERT_TRUE(S::PersistentManager::Instance().InstanceIDToObjectIdentifier(animator->GetAnimationReference().GetInstanceID(),actual));EXPECT_EQ(actual,graph);
        ASSERT_TRUE(S::PersistentManager::Instance().InstanceIDToObjectIdentifier(skin->GetMeshReference().GetInstanceID(),actual));EXPECT_EQ(actual,mesh);
    }
}
TEST(AnimationScene,AnimationSnapshotRunsAfterGameplayCallbacksAndRespectsDisabledComponents)
{
    E::SceneSystem::Scene scene;auto* animator=scene.CreateGameObject("Character").AddComponent<E::Components::AnimatorComponent>();
    C::Resources resources;resources.skeleton=std::make_shared<A::Skeleton>(Model()->skeleton);resources.clips["Move"]=std::make_shared<A::Clip>(Model()->clips[0]);
    auto graph=C::CreateGraph();graph.parameters={{1,"SampleTime",A::ParameterType::Float,{}}};graph.nodes[0].kind=A::NodeKind::SequenceEvaluator;graph.nodes[0].clip="Move";graph.nodes[0].parameter="SampleTime";
    auto compiled=C::Compile(graph,resources);ASSERT_TRUE(compiled.Succeeded());std::string error;ASSERT_TRUE(animator->SetCompiledProgram(compiled.program,error));
    scene.UpdateGameplay(.1f);ASSERT_TRUE(animator->SetFloat("SampleTime",.75f));scene.UpdateAnimations(.1f);EXPECT_NEAR(animator->CaptureFrame()->local[0].translation.x,1.5f,1e-6);
    const auto frame=animator->CaptureFrame();animator->SetEnabled(false);scene.Update(.1f);EXPECT_EQ(animator->CaptureFrame(),frame);
}
TEST(AnimationScene,DistributedSampleCameraContainsTheAnimatedBounds)
{
    const auto root=std::filesystem::path(NLS_ROOT_DIR)/"Templates/Animation";
    std::ifstream file(root/"AnimationSample.scene");const std::string text{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
    auto document=E::Serialize::ObjectGraphReader::Read(text);ASSERT_TRUE(document);
    E::Serialize::LoadPolicy policy;policy.deferAssetReferenceResolution=true;
    auto loaded=E::Serialize::ObjectGraphInstantiator::InstantiateScene(*document,policy);ASSERT_TRUE(loaded.scene);
    ASSERT_EQ(loaded.scene->GetFastAccessComponents().lights.size(),1u);EXPECT_EQ(loaded.scene->GetFastAccessComponents().lights[0]->GetLightType(),NLS::Render::Settings::ELightType::AMBIENT_SPHERE);
    auto* camera=loaded.scene->FindMainCamera();ASSERT_NE(camera,nullptr);camera->GetCamera()->CacheMatrices(1280,720);
    A::ImportSettings settings;std::string error;auto model=C::ImportGltfAnimation(root/"TwoBoneCharacter.gltf",settings,error);ASSERT_TRUE(model)<<error;
    for(double time:{0.,.25,.5,.75,1.})
    {
        A::Pose pose;A::SamplingContext sampler;ASSERT_TRUE(A::Sample(model->skeleton,model->clips[1],time,pose,sampler,error));
        std::vector<NLS::Maths::Matrix4> transforms;ASSERT_TRUE(A::LocalToModel(model->skeleton,pose,transforms,error));
        A::SkinningPalette palette;ASSERT_TRUE(A::BuildPalette(transforms,model->skins[0],palette,error));
        std::vector<A::Vertex> skinned;A::SkinVertices(model->meshes[0].vertices,palette,skinned);
        std::vector<NLS::Render::Geometry::Vertex> vertices(skinned.size());for(size_t i=0;i<skinned.size();++i)std::copy_n(skinned[i].position,3,vertices[i].position);
        const auto bounds=NLS::Render::Geometry::ComputeBounds(vertices);
        EXPECT_TRUE(camera->GetCamera()->GetFrustum().BoundsInFrustum(bounds,NLS::Maths::Matrix4::Identity))<<"time="<<time<<" center="<<bounds.center.x<<","<<bounds.center.y<<","<<bounds.center.z<<" size="<<bounds.size.x<<","<<bounds.size.y<<","<<bounds.size.z;
    }
}
}
