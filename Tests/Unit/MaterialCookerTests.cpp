#include <gtest/gtest.h>
#include <algorithm>
#include <fstream>
#include "MaterialCompiler/MaterialCooker.h"
#include "MaterialCompiler/MaterialGraph.h"
#include "Assets/ArtifactDatabase.h"
#include "Assets/NativeArtifactContainer.h"
#include "Core/ResourceManagement/ShaderManager.h"
#include "Core/ServiceLocator.h"
#include "Rendering/Resources/Loaders/MaterialLoader.h"
#include "Rendering/Resources/Shader.h"
#include "Rendering/ShaderCompiler/ShaderCompiler.h"
#include "Rendering/Assets/TextureArtifact.h"

TEST(MaterialCookerTests, PublishesOnlyUsedStaticVariantsAndLoadsWithoutAuthoringSources)
{
    namespace M=NLS::MaterialCompiler;namespace R=NLS::Render;namespace A=NLS::Core::Assets;
    if(R::ShaderCompiler::GetCurrentShaderCompilerToolchainIdentity().compilerPath.empty())GTEST_SKIP()<<"DXC unavailable";
    const auto root=std::filesystem::temp_directory_path()/("MaterialCook_"+NLS::Guid::New().ToString());
    struct Cleanup{std::filesystem::path path;~Cleanup(){std::error_code ec;std::filesystem::remove_all(path,ec);}}cleanup{root};
    std::filesystem::create_directories(root/"Source");
    auto graph=M::CreateDefaultMaterialGraph();graph.shading=M::MaterialGraphShading::Unlit;
    M::MaterialGraphParameter map;map.id=12;map.name="Map";map.type=M::MaterialGraphType::Texture2D;map.value="Assets/Map.texture";
    graph.parameters.push_back(map);
    const auto mapNode=M::AddMaterialGraphNode(graph,"Parameter",M::MaterialGraphType::Texture2D);graph.nodes.back().parameter=12;
    const auto sample=M::AddMaterialGraphNode(graph,"SampleTexture",M::MaterialGraphType::Float4);graph.nodes.back().inputs["Texture"]={mapNode,0};
    M::MaterialGraphParameter feature;feature.id=10;feature.name="Feature";feature.type=M::MaterialGraphType::Bool;feature.value=false;feature.isStatic=true;
    graph.parameters.push_back(feature);
    const auto switchId=M::AddMaterialGraphNode(graph,"StaticSwitch",M::MaterialGraphType::Float4);
    graph.nodes.back().parameter=10;graph.nodes.back().inputs={{"True",{sample,0}},{"False",graph.outputs.at("BaseColor")}};
    graph.outputs["BaseColor"]={switchId,0};
    const auto generated=M::GenerateMaterialGraphShader(graph);ASSERT_TRUE(generated.Succeeded());
    std::string error;ASSERT_TRUE(M::SaveMaterialGraph(graph,root/"Source"/"Graph.shadergraph",error))<<error;
    M::MaterialCookOptions options;options.outputRoot=root/"Release";options.shaderId=A::AssetId::New();
    options.compiler.sourcePath=root/"Source"/"Graph.shadergraph";options.compiler.sourceIdentity="Assets/Graph.shadergraph";
    options.compiler.generatedPath=root/"Build"/"Material.hlsl";options.compiler.cacheDatabasePath=(root/"Build"/"Cache.tsv").string();
    options.compiler.includeDirectories={(std::filesystem::path(NLS_ROOT_DIR)/"App"/"Assets"/"Engine"/"Shaders").string()};
    options.compiler.targets={R::ShaderCompiler::ShaderTargetPlatform::DXIL,R::ShaderCompiler::ShaderTargetPlatform::SPIRV};
    // One used static-keyword combination has static and skinned geometry variants.
    options.compiler.variantBudget=2;options.compiler.generateGlsl=false;
    R::Assets::TextureArtifactData texture;texture.width=texture.height=1;texture.format=R::RHI::TextureFormat::RGBA8;
    R::Assets::TextureArtifactMip mip;mip.width=mip.height=1;mip.rowPitch=mip.slicePitch=4;mip.pixels={255,255,255,255};texture.mips={mip};
    const auto textureId=A::AssetId::New();const auto native=R::Assets::SerializeTextureArtifact(texture);
    options.frozenTextures["Assets/Map.texture"]={textureId,"texture:main",native};
    options.textureProvider=[](std::string_view)->std::optional<M::MaterialCookTexture>{return std::nullopt;};
    M::MaterialCookEntry material;material.id=A::AssetId::New();material.path="Assets/Test.mat";
    material.resolved.shader={options.shaderId.ToString(),{},options.compiler.sourceIdentity};
    material.resolved.staticParameters[M::MaterialGraphBindingName(10,true)]=true;
    const auto cooked=M::CookMaterialFamily(generated.shader,generated.schema,{material},options);
    ASSERT_TRUE(cooked.Succeeded())<<cooked.error;EXPECT_FALSE(cooked.snapshotId.empty());
    std::filesystem::remove_all(root/"Source");std::filesystem::remove_all(root/"Build");
    A::ArtifactDatabase database;ASSERT_TRUE(database.Load(cooked.databasePath));
    const auto* record=database.Find(material.id,"material:main","editor");ASSERT_NE(record,nullptr);
    NLS::Core::ResourceManagement::ShaderManager shaders;NLS::Core::ServiceLocator::Provide(shaders);
    struct Services{~Services(){NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::ShaderManager>();}}services;
    R::Resources::Loaders::MaterialLoader::LoadOptions load;load.artifactDatabasePath=cooked.databasePath;load.loadMissingTextures=false;
    const auto path=options.outputRoot/record->artifactPath;
    A::RegisterRuntimeAuthorizedArtifactPath(A::TryMakePortableContentArtifactPath(path.generic_string()));
    auto* runtime=R::Resources::Loaders::MaterialLoader::Create(path.string(),load);
    ASSERT_NE(runtime,nullptr);ASSERT_TRUE(runtime->IsValid());
    const auto& artifacts=runtime->GetShader()->GetCompiledArtifacts();
    EXPECT_EQ(artifacts.size(),8u);
    const auto staticHash=runtime->GetShaderLabKeywords().Hash();
    auto skinnedKeywords=runtime->GetShaderLabKeywords();skinnedKeywords.Enable("NLS_SKINNED");
    const auto skinnedHash=skinnedKeywords.Hash();
    EXPECT_TRUE(std::any_of(artifacts.begin(),artifacts.end(),[&](const auto& stage){return stage.keywordHash==staticHash;}));
    EXPECT_TRUE(std::any_of(artifacts.begin(),artifacts.end(),[&](const auto& stage){return stage.keywordHash==skinnedHash;}));
    for(const auto& stage:artifacts)EXPECT_TRUE(stage.keywordHash==staticHash || stage.keywordHash==skinnedHash);
    EXPECT_NE(runtime->ResolveShaderForLightMode("Picking"),nullptr);
    const auto* textureRecord=database.Find(textureId,"texture:main","editor");ASSERT_NE(textureRecord,nullptr);
    EXPECT_EQ(runtime->GetTextureResourcePath(M::MaterialGraphBindingName(12)),textureRecord->artifactPath);
    R::Resources::Loaders::MaterialLoader::Destroy(runtime);
    for(const auto& item:std::filesystem::recursive_directory_iterator(options.outputRoot))
        EXPECT_TRUE(item.path().extension()!=".shadergraph" && item.path().extension()!=".hlsl" && item.path().extension()!=".shaderfunc");
    auto second=material;second.id=A::AssetId::New();second.resolved.staticParameters[M::MaterialGraphBindingName(10,true)]=false;
    auto limited=options;limited.outputRoot=root/"TooMany";
    const auto overBudget=M::CookMaterialFamily(generated.shader,generated.schema,{material,second},limited);
    EXPECT_FALSE(overBudget.Succeeded());EXPECT_NE(overBudget.error.find("budget"),std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(limited.outputRoot/"Library"/"ArtifactDB"));
}
