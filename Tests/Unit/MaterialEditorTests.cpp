#include <gtest/gtest.h>
#include <fstream>
#include <thread>
#include "Guid.h"
#include "Assets/AssetMeta.h"
#include "Assets/NativeArtifactContainer.h"
#include "Core/ServiceLocator.h"
#include "Core/ResourceManagement/ShaderManager.h"
#include "Core/ResourceManagement/MaterialManager.h"
#include "Materials/MaterialEditingService.h"
#include "Rendering/ShaderCompiler/ShaderCompiler.h"
#include "Undo/AssetFileUndo.h"
#include "Math/Vector4.h"
#include "Rendering/Resources/MaterialInstance.h"
#include "MaterialCompiler/MaterialGraph.h"
#include "Assets/AssetDatabaseFacade.h"
#include "Assets/EditorAssetPath.h"

namespace
{
struct Project
{
    std::filesystem::path root=std::filesystem::temp_directory_path()/("MaterialEditor_"+NLS::Guid::New().ToString());
    Project(){std::filesystem::create_directories(root/"Assets");}
    ~Project(){std::error_code ec;std::filesystem::remove_all(root,ec);}
};
bool WaitPreview(NLS::Editor::Materials::MaterialEditingService& service,NLS::Editor::Materials::MaterialEditSession& session)
{
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(45);
    do
    {
        service.Pump();
        if (!session.pending.valid() && !session.applyRequested && !session.preparePublication)
            return session.compiled.has_value() && session.error.empty();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }while(std::chrono::steady_clock::now()<deadline);
    return false;
}
}

TEST(MaterialEditorTests, NativeCreationRestoresIdenticalAssetAndMetaOnRedo)
{
    Project project;
    NLS::Editor::Undo::EditorUndoService undo;
    NLS::Render::Assets::MaterialAsset asset;asset.shader.path="Assets/Shader.shader";
    const auto path=project.root/"Assets"/"Created.mat";
    auto command=NLS::Editor::Undo::AssetFile::MakeCreateMaterialCommand(project.root,path,asset);
    ASSERT_NE(command,nullptr);ASSERT_TRUE(undo.Execute(std::move(command)).Succeeded());
    const auto meta=NLS::Core::Assets::AssetMeta::Load(NLS::Core::Assets::GetAssetMetaPath(path));
    ASSERT_TRUE(meta);
    std::ifstream first(path,std::ios::binary);const std::string before{std::istreambuf_iterator<char>(first),std::istreambuf_iterator<char>()};first.close();
    ASSERT_TRUE(undo.RequestUndo().Succeeded());EXPECT_FALSE(std::filesystem::exists(path));
    ASSERT_TRUE(undo.RequestRedo().Succeeded());
    const auto restored=NLS::Core::Assets::AssetMeta::Load(NLS::Core::Assets::GetAssetMetaPath(path));
    ASSERT_TRUE(restored);EXPECT_EQ(restored->id,meta->id);
    std::ifstream second(path,std::ios::binary);const std::string after{std::istreambuf_iterator<char>(second),std::istreambuf_iterator<char>()};
    EXPECT_EQ(before,after);
}

TEST(MaterialEditorTests, CreationUndoRefusesNewDurableParentReferences)
{
    Project project;
    NLS::Editor::Undo::EditorUndoService undo;
    NLS::Render::Assets::MaterialAsset parent;parent.shader.path="Assets/Shader.shader";
    const auto path=project.root/"Assets"/"Parent.mat";
    ASSERT_TRUE(undo.Execute(NLS::Editor::Undo::AssetFile::MakeCreateMaterialCommand(project.root,path,parent)).Succeeded());
    const auto meta=NLS::Core::Assets::AssetMeta::Load(NLS::Core::Assets::GetAssetMetaPath(path));ASSERT_TRUE(meta);
    NLS::Render::Assets::MaterialAsset child;child.parent={meta->id.ToString(),"material:main","Assets/Parent.mat"};
    std::string error;ASSERT_TRUE(NLS::Render::Assets::SaveMaterialAsset(child,project.root/"Assets"/"Child.mat",error));
    EXPECT_FALSE(undo.RequestUndo().Succeeded());EXPECT_TRUE(std::filesystem::exists(path));
}

TEST(MaterialEditorTests, ParameterPreviewAndSaveRemainIndependentFromExplicitSceneApplication)
{
    if (NLS::Render::ShaderCompiler::GetCurrentShaderCompilerToolchainIdentity().compilerPath.empty()) GTEST_SKIP()<<"DXC unavailable";
    Project project;
    {std::ofstream file(project.root/"Assets"/"Shader.shader");file << R"(
Shader "Editor/Test" {
Properties { _Tint("Tint",Color) = (1,0,0,1) }
SubShader { Pass { Name "Forward" Tags { "LightMode"="Forward" }
HLSLPROGRAM
#pragma vertex VSMain
#pragma fragment PSMain
cbuffer MaterialProperties : register(b0,space2) {float4 _Tint;};
float4 VSMain(uint id : SV_VertexID) : SV_Position {return float4(id==1?3:-1,id==2?3:-1,0,1);}
float4 PSMain() : SV_Target0 {return _Tint;}
ENDHLSL
} } }
)";}
    const auto engine=std::filesystem::path(NLS_ROOT_DIR)/"App"/"Assets"/"Engine";
    NLS::Core::ResourceManagement::ShaderManager shaders;
    NLS::Core::ResourceManagement::ShaderManager::ProvideAssetPaths((project.root/"Assets").string(),engine.string());
    NLS::Core::ServiceLocator::Provide(shaders);
    struct Services {~Services(){NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::ShaderManager>();}} services;
    NLS::Render::Assets::MaterialAsset asset;asset.shader.path="Assets/Shader.shader";
    const auto path=project.root/"Assets"/"Material.mat";
    std::string error;ASSERT_TRUE(NLS::Render::Assets::SaveMaterialAsset(asset,path,error))<<error;
    NLS::Editor::Undo::EditorUndoService undo;
    NLS::Editor::Materials::MaterialEditingService service(project.root,engine,&undo);
    const auto session=service.Open(path,error);ASSERT_NE(session,nullptr)<<error;
    ASSERT_TRUE(WaitPreview(service,*session))<<session->error;
    ASSERT_NE(session->preview,nullptr);
    NLS::Render::Resources::Material scene;
    scene.CopyRuntimeFrom(*session->preview);
    session->sceneMaterial=&scene;session->sceneMaterialId=scene.GetInstanceId();
    const auto schema=session->preview->GetShader()->GetMaterialParameterSchema();
    const auto* tint=schema->Find("_Tint");ASSERT_NE(tint,nullptr);
    const auto shaderGeneration=session->preview->GetShader()->GetGeneration();
    ASSERT_TRUE(service.SetParameter(*session,*tint,std::array<float,4>{0,1,0,1}));
    ASSERT_TRUE(service.Save(*session));
    EXPECT_FLOAT_EQ(scene.Get<NLS::Maths::Vector4>("_Tint").x,1);
    ASSERT_TRUE(service.RequestPreview(*session));
    ASSERT_TRUE(WaitPreview(service,*session))<<session->error;
    EXPECT_FLOAT_EQ(session->preview->Get<NLS::Maths::Vector4>("_Tint").y,1);
    EXPECT_EQ(session->preview->GetShader()->GetGeneration(),shaderGeneration);
    EXPECT_FLOAT_EQ(scene.Get<NLS::Maths::Vector4>("_Tint").x,1);
    ASSERT_TRUE(service.ApplyToScene(*session));ASSERT_TRUE(WaitPreview(service,*session))<<session->error;
    EXPECT_TRUE(session->error.empty())<<session->error;
    EXPECT_FLOAT_EQ(scene.Get<NLS::Maths::Vector4>("_Tint").y,1);
    EXPECT_TRUE(session->document.IsApplied());
    const auto snapshot=session->compiled->dependencies.Id();
    ASSERT_TRUE(service.PublishResources(*session))<<session->error;
    const auto cookDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(45);
    while(session->pendingCook.valid() && std::chrono::steady_clock::now()<cookDeadline)
    {service.Pump();std::this_thread::sleep_for(std::chrono::milliseconds(2));}
    EXPECT_FALSE(session->pendingCook.valid());
    EXPECT_TRUE(session->error.empty())<<session->error;EXPECT_FALSE(session->lastRelease.empty());
    EXPECT_EQ(session->compiled->dependencies.Id(),snapshot);
    // Parent publication updates loaded descendants, keeping both persistent
    // and transient local overrides. Scene-facing Material identities survive.
    NLS::Core::ResourceManagement::MaterialManager materials;
    NLS::Core::ServiceLocator::Provide(materials);
    struct MaterialService {~MaterialService(){NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::MaterialManager>();}} materialService;
    auto child=std::make_unique<NLS::Render::Resources::Material>();child->CopyRuntimeFrom(scene);
    auto childAsset=session->document.Asset();
    childAsset.identity={NLS::Guid::New().ToString(),"material:main","Assets/Child.mat"};
    childAsset.shader={};childAsset.parent=session->document.Asset().identity;childAsset.parameters.clear();
    child->SetAssetData(std::make_shared<const NLS::Render::Assets::MaterialAsset>(childAsset));
    const auto childId=child->GetInstanceId();
    auto* childMaterial=materials.RegisterResource("Assets/Child.mat",child.release());
    NLS::Render::Resources::MaterialInstance dynamic(*childMaterial);
    ASSERT_TRUE(dynamic.SetParameter("_Tint",std::array<float,4>{1,0,1,1},error))<<error;
    ASSERT_TRUE(service.SetParameter(*session,*tint,std::array<float,4>{0,0,1,1}));
    ASSERT_TRUE(service.ApplyToScene(*session));ASSERT_TRUE(WaitPreview(service,*session))<<session->error;
    EXPECT_EQ(childMaterial->GetInstanceId(),childId);
    EXPECT_FLOAT_EQ(childMaterial->Get<NLS::Maths::Vector4>("_Tint").z,1);
    EXPECT_FLOAT_EQ(childMaterial->Get<NLS::Maths::Vector4>("_Tint").y,0);
    EXPECT_FLOAT_EQ(dynamic.GetMaterial().Get<NLS::Maths::Vector4>("_Tint").x,1);
    ASSERT_TRUE(dynamic.ClearParameter("_Tint",error))<<error;
    EXPECT_FLOAT_EQ(dynamic.GetMaterial().Get<NLS::Maths::Vector4>("_Tint").x,0);
    session->sceneMaterial=nullptr;
}

TEST(MaterialEditorTests, GraphWorkingSourceCompilesWithoutSavingAndSaveDoesNotApply)
{
    if(NLS::Render::ShaderCompiler::GetCurrentShaderCompilerToolchainIdentity().compilerPath.empty())GTEST_SKIP()<<"DXC unavailable";
    Project project;std::string error;
    auto graph=NLS::MaterialCompiler::CreateDefaultMaterialGraph();graph.shading=NLS::MaterialCompiler::MaterialGraphShading::Unlit;
    const auto path=project.root/"Assets"/"Graph.shadergraph";
    ASSERT_TRUE(NLS::MaterialCompiler::SaveMaterialGraph(graph,path,error))<<error;
    const auto engine=std::filesystem::path(NLS_ROOT_DIR)/"App"/"Assets"/"Engine";
    NLS::Core::ResourceManagement::ShaderManager shaders;NLS::Core::ServiceLocator::Provide(shaders);
    struct Services {~Services(){NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::ShaderManager>();}} services;
    NLS::Editor::Undo::EditorUndoService undo;
    NLS::Editor::Materials::MaterialEditingService service(project.root,engine,&undo);
    const auto session=service.OpenSource(path,error);ASSERT_TRUE(session)<<error;
    const auto initial=session->appliedSource;
    graph.parameters.front().value=std::array<float,4>{0,1,0,1};
    ASSERT_TRUE(session->document.Apply(NLS::MaterialCompiler::SerializeMaterialGraph(graph),"Change graph color"));
    ASSERT_TRUE(service.PreviewSource(*session));ASSERT_TRUE(WaitPreview(service,*session->preview))<<session->preview->error;
    EXPECT_EQ(session->appliedSource,initial);EXPECT_TRUE(session->document.IsDirty());
    ASSERT_TRUE(session->document.Save());EXPECT_EQ(session->appliedSource,initial);
    ASSERT_TRUE(service.ApplySource(*session));ASSERT_TRUE(WaitPreview(service,*session->preview))<<session->preview->error;
    EXPECT_EQ(session->appliedSource,session->document.Asset());
    NLS::Core::ResourceManagement::MaterialManager materials;NLS::Core::ServiceLocator::Provide(materials);
    struct MaterialService {~MaterialService(){NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::MaterialManager>();}}materialService;
    std::vector<NLS::Render::Resources::Material*> family;
    const auto* originalShader=session->preview->preview->GetShader();
    for(int i=0;i<2;++i)
    {
        auto material=std::make_unique<NLS::Render::Resources::Material>();material->CopyRuntimeFrom(*session->preview->preview);
        NLS::Render::Assets::MaterialAsset asset;
        asset.identity={NLS::Guid::New().ToString(),"material:main","Assets/Root"+std::to_string(i)+".mat"};asset.shader.path="Assets/Graph.shadergraph";
        const auto& parameter=material->GetShader()->GetMaterialParameterSchema()->parameters.front();
        asset.parameters.push_back({parameter.id,parameter.name,parameter.type,std::array<float,4>{float(i),0,1,1}});
        material->SetAssetData(std::make_shared<const NLS::Render::Assets::MaterialAsset>(asset));
        family.push_back(materials.RegisterResource(asset.identity.path,material.release()));
    }
    graph.blend=NLS::MaterialCompiler::MaterialGraphBlend::Alpha;
    ASSERT_TRUE(session->document.Apply(NLS::MaterialCompiler::SerializeMaterialGraph(graph),"Change shader family blend"));
    ASSERT_TRUE(service.ApplySource(*session));ASSERT_TRUE(WaitPreview(service,*session->preview))<<session->preview->error;
    EXPECT_NE(family[0]->GetShader(),originalShader);EXPECT_EQ(family[0]->GetShader(),family[1]->GetShader());
    EXPECT_TRUE(family[0]->IsBlendable());EXPECT_TRUE(family[1]->IsBlendable());
    const auto name=NLS::MaterialCompiler::MaterialGraphBindingName(graph.parameters.front().id);
    EXPECT_FLOAT_EQ(family[0]->Get<NLS::Maths::Vector4>(name).x,0);EXPECT_FLOAT_EQ(family[1]->Get<NLS::Maths::Vector4>(name).x,1);
    ASSERT_TRUE(undo.RequestUndo().Succeeded());EXPECT_NE(session->document.Asset(),session->appliedSource);
}

TEST(MaterialEditorTests, GraphInheritanceImporterRequestsExplicitOffVariantAndLoadsFlattenedChild)
{
    namespace M=NLS::MaterialCompiler;namespace A=NLS::Core::Assets;namespace R=NLS::Render;
    if(R::ShaderCompiler::GetCurrentShaderCompilerToolchainIdentity().compilerPath.empty())GTEST_SKIP()<<"DXC unavailable";
    Project project;std::string error;
    auto graph=M::CreateDefaultMaterialGraph("Imported Graph");graph.shading=M::MaterialGraphShading::Unlit;
    M::MaterialGraphParameter feature;feature.id=42;feature.name="Feature";feature.type=M::MaterialGraphType::Bool;feature.value=true;feature.isStatic=true;
    graph.parameters.push_back(feature);
    ASSERT_TRUE(M::SaveMaterialGraph(graph,project.root/"Assets"/"Graph.shadergraph",error))<<error;
    NLS::Editor::Undo::EditorUndoService undo;
    R::Assets::MaterialAsset parent;parent.shader.path="Assets/Graph.shadergraph";
    ASSERT_TRUE(undo.Execute(NLS::Editor::Undo::AssetFile::MakeCreateMaterialCommand(project.root,project.root/"Assets"/"Parent.mat",parent)).Succeeded());
    const auto meta=A::AssetMeta::Load(A::GetAssetMetaPath(project.root/"Assets"/"Parent.mat"));ASSERT_TRUE(meta);
    R::Assets::MaterialAsset child;child.parent={meta->id.ToString(),"material:main","Assets/Parent.mat"};child.staticParameters[M::MaterialGraphBindingName(42,true)]=false;
    ASSERT_TRUE(undo.Execute(NLS::Editor::Undo::AssetFile::MakeCreateMaterialCommand(project.root,project.root/"Assets"/"Child.mat",child)).Succeeded());
    NLS::Editor::Assets::AssetDatabaseFacade database(NLS::Editor::Assets::MakeProjectEditorAssetRoots(project.root));
    ASSERT_TRUE(database.Refresh());
    const auto imported=database.ImportAsset("Assets/Child.mat");
    for(const auto& diagnostic:database.GetDiagnostics())if(diagnostic.severity==A::AssetDiagnosticSeverity::Error)error+=diagnostic.message+"\n";
    ASSERT_TRUE(imported)<<error;
    NLS::Core::ResourceManagement::ShaderManager shaders;NLS::Core::ServiceLocator::Provide(shaders);
    struct Services{~Services(){NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::ShaderManager>();}}services;
    R::Resources::Loaders::MaterialLoader::LoadOptions options;options.artifactDatabasePath=project.root/"Library"/"ArtifactDB";
    options.loadMissingTextures=false;
    auto* material=R::Resources::Loaders::MaterialLoader::Create(database.ResolveArtifactPathAtPath("Assets/Child.mat","material:main").string(),options);
    ASSERT_NE(material,nullptr);ASSERT_TRUE(material->IsValid());
    EXPECT_FALSE(material->IsKeywordEnabled(M::MaterialGraphBindingName(42,true)));
    ASSERT_TRUE(material->GetAssetData());EXPECT_EQ(material->GetAssetData()->parent.guid,meta->id.ToString());
    EXPECT_NE(material->ResolveShaderForLightMode("DepthOnly"),nullptr);
    R::Resources::Loaders::MaterialLoader::Destroy(material);
}
