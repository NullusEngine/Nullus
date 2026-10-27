#include <gtest/gtest.h>
#include <AnimationCompiler/GltfAnimationImporter.h>
#include <Animation/Graph.h>
#include <Rendering/Assets/SceneModelReader.h>
#include <Rendering/Assets/AnimationArtifacts.h>
#include <Rendering/Resources/Parsers/AssimpParser.h>
#include <Assets/AssetMeta.h>
#include <Assets/ContentSource.h>
#include <Assets/NpakArchive.h>
#include <Rendering/Assets/ShaderArtifact.h>
#include <Rendering/Resources/Loaders/ShaderLoader.h>
#include <Rendering/Resources/IndexedObjectDataShaderSupport.h>
#include <Json/json.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace
{
using Json=nlohmann::json;
struct AnimationFixture:testing::Test
{
    std::filesystem::path root;
    void SetUp()override{root=std::filesystem::temp_directory_path()/("NullusAnimation-"+NLS::Guid::New().ToString());std::filesystem::create_directories(root);}
    void TearDown()override{std::error_code ignored;std::filesystem::remove_all(root,ignored);}
    std::filesystem::path Gltf(bool glb=false,bool sparse=false)
    {
        std::vector<uint8_t> bytes;Json views=Json::array(),accessors=Json::array();
        auto accessor=[&](auto values,uint32_t component,const char* type,size_t count)
        {
            while(bytes.size()%4)bytes.push_back(0);const auto start=bytes.size();const auto* begin=reinterpret_cast<const uint8_t*>(values.data());bytes.insert(bytes.end(),begin,begin+values.size()*sizeof(values[0]));
            views.push_back({{"buffer",0},{"byteOffset",start},{"byteLength",bytes.size()-start}});accessors.push_back({{"bufferView",views.size()-1},{"componentType",component},{"type",type},{"count",count}});return accessors.size()-1;
        };
        auto positions=accessor(std::vector<float>{0,0,0,1,0,0,0,1,0},5126,"VEC3",3);
        auto joints=accessor(std::vector<uint16_t>{0,0,0,0,0,0,0,0,0,0,0,0},5123,"VEC4",3);
        auto weights=accessor(std::vector<uint16_t>{65535,0,0,0,65535,0,0,0,65535,0,0,0},5123,"VEC4",3);accessors[weights]["normalized"]=true;
        auto times=accessor(std::vector<float>{0,1},5126,"SCALAR",2);
        auto translations=accessor(std::vector<float>{0,0,0,2,0,0},5126,"VEC3",2);
        if(sparse)
        {
            auto indices=accessor(std::vector<uint8_t>{1},5121,"SCALAR",1),values=accessor(std::vector<float>{3,0,0},5126,"VEC3",1);
            accessors[translations]["sparse"]={{"count",1},{"indices",{{"bufferView",accessors[indices]["bufferView"]},{"componentType",5121}}},{"values",{{"bufferView",accessors[values]["bufferView"]}}}};
        }
        Json j{{"asset",{{"version","2.0"}}},{"buffers",Json::array({{{"byteLength",bytes.size()}}})},{"bufferViews",views},{"accessors",accessors},
            {"nodes",Json::array({{{"name","root"}},{{"name","mesh"},{"mesh",0},{"skin",0}}})},
            {"skins",Json::array({{{"name","Skin"},{"joints",{0}}}})},
            {"meshes",Json::array({{{"name","Triangle"},{"primitives",Json::array({{{"attributes",{{"POSITION",positions},{"JOINTS_0",joints},{"WEIGHTS_0",weights}}}}})}}})},
            {"animations",Json::array({{{"name","Move"},{"samplers",Json::array({{{"input",times},{"output",translations}}})},{"channels",Json::array({{{"sampler",0},{"target",{{"node",0},{"path","translation"}}}}})}}})}};
        const auto path=root/(glb?"triangle.glb":"triangle.gltf");
        if(!glb)
        {
            j["buffers"][0]["uri"]="triangle%20data.bin";std::ofstream data(root/"triangle data.bin",std::ios::binary);data.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());data.close();std::ofstream(path)<<j.dump();
        }
        else
        {
            auto json=j.dump();while(json.size()%4)json+=' ';while(bytes.size()%4)bytes.push_back(0);
            std::ofstream file(path,std::ios::binary);auto u32=[&](uint32_t v){file.write(reinterpret_cast<const char*>(&v),4);};
            u32(0x46546c67);u32(2);u32(uint32_t(28+json.size()+bytes.size()));u32(uint32_t(json.size()));u32(0x4e4f534a);file.write(json.data(),json.size());u32(uint32_t(bytes.size()));u32(0x004e4942);file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
        }
        return path;
    }
};
TEST_F(AnimationFixture,GltfAndGlbImportSameSkeletonSkinAndAnimation)
{
    for(bool glb:{false,true})
    {
        std::string error;NLS::Animation::ImportSettings settings;settings.sourceId="fixture";
        auto model=NLS::AnimationCompiler::ImportGltfAnimation(Gltf(glb),settings,error);ASSERT_TRUE(model)<<error;ASSERT_EQ(model->meshes.size(),1u);ASSERT_EQ(model->clips.size(),1u);ASSERT_EQ(model->skeleton.bones.size(),1u);
        NLS::Animation::Pose pose;NLS::Animation::SamplingContext cache;ASSERT_TRUE(NLS::Animation::Sample(model->skeleton,model->clips[0],.5,pose,cache,error));EXPECT_NEAR(pose[0].translation.x,1,1e-6);
        EXPECT_EQ(model->meshes[0].vertices[0].influence.weights[0],65535u);
    }
}
TEST_F(AnimationFixture,PackagedShaderDependencyLoadsWithoutALooseArtifact)
{
    namespace Assets=NLS::Core::Assets;namespace R=NLS::Render::Assets;
    R::ShaderArtifact shader;shader.subAssetKey="shader:Pose";shader.sourcePath="Shaders/Pose.hlsl";
    const auto bytes=R::SerializeShaderArtifact(shader);ASSERT_TRUE(R::DeserializeShaderArtifact(bytes));
    Assets::NpakWriter writer;std::string error;ASSERT_TRUE(writer.AddFile("Shaders/Pose.native",bytes,&error))<<error;
    ASSERT_TRUE(writer.Write(root/"content.npak",{},&error))<<error;
    struct MountCleanup{~MountCleanup(){Assets::ClearContentMounts();}}cleanup;
    ASSERT_TRUE(Assets::MountContentPackage(root/"content.npak",root/"Player",&error))<<error;
    const auto path=root/"Player/Shaders/Pose.native";ASSERT_FALSE(std::filesystem::exists(path));
    auto loaded=R::LoadShaderArtifact(path);ASSERT_TRUE(loaded);EXPECT_EQ(loaded->sourcePath,shader.sourcePath);EXPECT_EQ(loaded->subAssetKey,shader.subAssetKey);
}
TEST_F(AnimationFixture,SparseAccessorOverridesAnimationKeys)
{
    std::string error;auto model=NLS::AnimationCompiler::ImportGltfAnimation(Gltf(false,true),{},error);ASSERT_TRUE(model)<<error;
    NLS::Animation::Pose pose;NLS::Animation::SamplingContext cache;ASSERT_TRUE(NLS::Animation::Sample(model->skeleton,model->clips[0],.5,pose,cache,error));EXPECT_NEAR(pose[0].translation.x,1.5,1e-6);
}
TEST_F(AnimationFixture,LegacySpirvObjectDrawReflectionLoadsWithTheIndexedObjectAbi)
{
    namespace R=NLS::Render;namespace S=R::Resources;
    R::Assets::ShaderArtifact artifact;artifact.sourcePath="Legacy.hlsl";artifact.subAssetKey="shader:Legacy";
    S::ShaderConstantBufferDesc buffer;buffer.name="g_NLSObjectDrawConstants";buffer.bindingSpace=3;buffer.bindingIndex=1;buffer.byteSize=16;
    for(const auto* name:{"objectIndex","objectFlags","objectPadding0","objectPadding1"})buffer.members.push_back({name,S::UniformType::UNIFORM_INT,uint32_t(buffer.members.size()*4),4,1});
    artifact.reflection.constantBuffers.push_back(buffer);
    S::ShaderPropertyDesc objectData;objectData.name="ObjectData";objectData.kind=S::ShaderResourceKind::StructuredBuffer;objectData.bindingSpace=3;objectData.bindingIndex=0;objectData.byteSize=64;artifact.reflection.properties.push_back(objectData);
    const auto bytes=R::Assets::SerializeShaderArtifact(artifact);const auto path=root/"Legacy.native";{std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());}
    auto* shader=S::Loaders::ShaderLoader::Create(path.string());ASSERT_NE(shader,nullptr);
    EXPECT_TRUE(S::ValidateIndexedObjectDataShader(*shader).IsCompatible());S::Loaders::ShaderLoader::Destroy(shader);
}
TEST_F(AnimationFixture,AnimatedFbxRetainsMeshWeightsAndTake)
{
    const auto path=std::filesystem::path(NLS_ROOT_DIR)/"ThirdParty/assimp/test/models/FBX/animation_with_skeleton.fbx";
    ASSERT_TRUE(std::filesystem::exists(path));NLS::Render::Resources::Parsers::AssimpParser parser;
    std::vector<NLS::Render::Resources::Parsers::ParsedMeshData> meshes;std::vector<std::string> materials;
    NLS::Animation::ImportSettings settings;settings.sourceId="fbx-fixture";
    ASSERT_TRUE(parser.LoadModelData(path.string(),meshes,materials,NLS::Render::Resources::Parsers::EModelParserFlags::TRIANGULATE,nullptr,false,settings));
    NLS::Render::Assets::ImportedScene scene;ASSERT_TRUE(parser.PopulateImportedSceneData(path,NLS::Render::Assets::SceneModelSourceFormat::Fbx,scene));ASSERT_TRUE(scene.animationData);
    std::string error;ASSERT_TRUE(scene.animationData->Validate(error))<<error;EXPECT_FALSE(scene.animationData->clips.empty());EXPECT_FALSE(scene.animationData->meshes.empty());EXPECT_FALSE(scene.animationData->skins.empty());
}
TEST_F(AnimationFixture,ExplicitUnavailableFbxReaderFailsWithoutSilentFallback)
{
#if !NLS_HAS_AUTODESK_FBX_SDK
    auto meta=NLS::Core::Assets::AssetMeta::CreateForAsset("hero.fbx");meta.settings["MODEL_FBX_READER"]="autodesk";
    NLS::Render::Assets::ImportedScene scene;std::vector<NLS::Render::Resources::Parsers::ParsedMeshData> meshes;std::vector<std::string> materials,dependencies;std::string error;
    EXPECT_FALSE(NLS::Render::Assets::ReadModelSource("hero.fbx",meta,NLS::Render::Resources::Parsers::EModelParserFlags::TRIANGULATE,meshes,materials,dependencies,scene,error));EXPECT_NE(error.find("Autodesk"),std::string::npos);
#endif
}
TEST_F(AnimationFixture,AsciiFbxSupportsMultipleTakesSelectionAndSampleRate)
{
    const auto path=std::filesystem::path(NLS_ROOT_DIR)/"Templates/Animation/LinearSkinTwoTakes.fbx";
    NLS::Render::Resources::Parsers::AssimpParser parser;std::vector<NLS::Render::Resources::Parsers::ParsedMeshData> meshes;std::vector<std::string> materials;
    NLS::Animation::ImportSettings settings;settings.sourceId="ascii-rig";settings.sampleRate=12;
    ASSERT_TRUE(parser.LoadModelData(path.string(),meshes,materials,NLS::Render::Resources::Parsers::EModelParserFlags::TRIANGULATE,nullptr,false,settings));
    NLS::Render::Assets::ImportedScene scene;ASSERT_TRUE(parser.PopulateImportedSceneData(path,NLS::Render::Assets::SceneModelSourceFormat::Fbx,scene));ASSERT_TRUE(scene.animationData);
    ASSERT_EQ(scene.animationData->clips.size(),2u);EXPECT_FALSE(scene.animationData->skins.empty());EXPECT_EQ(scene.animationData->clips[0].tracks[0].translation.times.size(),13u);
    settings.takes={scene.animationData->clips[0].name};ASSERT_TRUE(parser.LoadModelData(path.string(),meshes,materials,NLS::Render::Resources::Parsers::EModelParserFlags::TRIANGULATE,nullptr,false,settings));
    ASSERT_TRUE(parser.PopulateImportedSceneData(path,NLS::Render::Assets::SceneModelSourceFormat::Fbx,scene));ASSERT_EQ(scene.animationData->clips.size(),1u);EXPECT_EQ(scene.animationData->clips[0].name,settings.takes[0]);
}
TEST_F(AnimationFixture,FbxCentimeterAndMeterSourcesProduceTheSameDeformedVertices)
{
    const auto path=std::filesystem::path(NLS_ROOT_DIR)/"Templates/Animation/LinearSkinTwoTakes.fbx";
    std::ifstream source(path);std::string text{std::istreambuf_iterator<char>(source),std::istreambuf_iterator<char>()};
    auto replace=[&](const std::string& from,const std::string& to){size_t pos=0;while((pos=text.find(from,pos))!=std::string::npos){text.replace(pos,from.size(),to);pos+=to.size();}};
    replace("\"Number\", \"\",100","\"Number\", \"\",1");replace("a: -0.5,0,0,0.5,0,0,0,1,0","a: -50,0,0,50,0,0,0,100,0");
    replace("KeyValueFloat: *2 { a: 0,1 }","KeyValueFloat: *2 { a: 0,100 }");replace("KeyValueFloat: *2 { a: 0,-1 }","KeyValueFloat: *2 { a: 0,-100 }");
    const auto centimeters=root/"Centimeters.fbx";std::ofstream(centimeters)<<text;
    auto load=[&](const std::filesystem::path& input)
    {
        NLS::Render::Resources::Parsers::AssimpParser parser;std::vector<NLS::Render::Resources::Parsers::ParsedMeshData> meshes;std::vector<std::string> materials;
        NLS::Animation::ImportSettings settings;settings.sourceId="rig";
        if(!parser.LoadModelData(input.string(),meshes,materials,NLS::Render::Resources::Parsers::EModelParserFlags::TRIANGULATE,nullptr,false,settings))return std::shared_ptr<NLS::Animation::Model>{};
        NLS::Render::Assets::ImportedScene scene;if(!parser.PopulateImportedSceneData(input,NLS::Render::Assets::SceneModelSourceFormat::Fbx,scene))return std::shared_ptr<NLS::Animation::Model>{};return scene.animationData;
    };
    auto meters=load(path),cm=load(centimeters);ASSERT_TRUE(meters);ASSERT_TRUE(cm);ASSERT_EQ(cm->meshes.size(),meters->meshes.size());
    EXPECT_EQ(meters->skeleton.Signature(),cm->skeleton.Signature());
    for(double time:{0.,.25,.5,1.})
    {
        std::array<std::vector<NLS::Animation::Vertex>,2> output;size_t index=0;
        for(const auto& model:{meters,cm})
        {NLS::Animation::Pose pose;NLS::Animation::SamplingContext sampling;std::string error;ASSERT_TRUE(NLS::Animation::Sample(model->skeleton,model->clips[0],time,pose,sampling,error));std::vector<NLS::Maths::Matrix4> transforms;ASSERT_TRUE(NLS::Animation::LocalToModel(model->skeleton,pose,transforms,error));NLS::Animation::SkinningPalette palette;ASSERT_TRUE(NLS::Animation::BuildPalette(transforms,model->skins[0],palette,error));NLS::Animation::SkinVertices(model->meshes[0].vertices,palette,output[index++]);}
        ASSERT_EQ(output[0].size(),output[1].size());for(size_t i=0;i<output[0].size();++i)for(size_t axis=0;axis<3;++axis)EXPECT_NEAR(output[0][i].position[axis],output[1][i].position[axis],1e-5);
    }
}
TEST_F(AnimationFixture,AnimationOnlyFbxBindsACompatibleSkeletonAndRejectsChangedProportions)
{
    namespace A=NLS::Animation;namespace P=NLS::Render::Resources::Parsers;
    const auto path=std::filesystem::path(NLS_ROOT_DIR)/"Templates/Animation/LinearSkinTwoTakes.fbx";
    std::ifstream source(path);std::string text{std::istreambuf_iterator<char>(source),std::istreambuf_iterator<char>()};
    auto eraseBlock=[&](const std::string& token)
    {const auto begin=text.find(token);if(begin==std::string::npos)return;auto cursor=text.find('{',begin);int depth=0;do{if(text[cursor]=='{')++depth;else if(text[cursor]=='}')--depth;++cursor;}while(depth&&cursor<text.size());text.erase(begin,cursor-begin);};
    eraseBlock("Geometry: 200,");eraseBlock("Deformer: 300,");eraseBlock("Deformer: 301,");eraseBlock("Model: 101,");
    std::istringstream lines(text);std::string line,animationOnly;while(std::getline(lines,line))
    {if(line.find("C: ")!=std::string::npos&&(line.find(",200,")!=std::string::npos||line.find(",300,")!=std::string::npos||line.find(",301,")!=std::string::npos||line.find(",101,")!=std::string::npos||line.ends_with(",101")||line.ends_with(",300")))continue;animationOnly+=line+"\n";}
    const auto onlyPath=root/"AnimationOnly.fbx";std::ofstream(onlyPath)<<animationOnly;
    auto load=[&](const std::filesystem::path& input,const A::ImportSettings& settings)
    {P::AssimpParser parser;std::vector<P::ParsedMeshData> meshes;std::vector<std::string> materials;NLS::Render::Assets::ImportedScene scene;if(!parser.LoadModelData(input.string(),meshes,materials,P::EModelParserFlags::TRIANGULATE,nullptr,false,settings))return std::shared_ptr<A::Model>{};if(!parser.PopulateImportedSceneData(input,NLS::Render::Assets::SceneModelSourceFormat::Fbx,scene))return std::shared_ptr<A::Model>{};return scene.animationData;};
    auto character=load(path,{});ASSERT_TRUE(character);A::ImportSettings settings;settings.importSkeleton=false;settings.targetSkeleton=std::make_shared<A::Skeleton>(character->skeleton);
    auto animation=load(onlyPath,settings);ASSERT_TRUE(animation);EXPECT_TRUE(animation->meshes.empty());EXPECT_EQ(animation->clips.size(),2u);EXPECT_EQ(animation->skeleton.Signature(),character->skeleton.Signature());
    auto changed=std::make_shared<A::Skeleton>(character->skeleton);changed->bones[1].reference.translation.x=20;settings.targetSkeleton=changed;EXPECT_FALSE(load(onlyPath,settings));
}
TEST_F(AnimationFixture,DuplicateSkeletalPathsAreRejected)
{
    const auto path=Gltf();std::ifstream input(path);Json json;input>>json;input.close();json["nodes"].push_back({{"name","root"}});std::ofstream(path)<<json.dump();
    std::string error;EXPECT_FALSE(NLS::AnimationCompiler::ImportGltfAnimation(path,{},error));EXPECT_NE(error.find("Ambiguous"),std::string::npos);
}
TEST_F(AnimationFixture,ReimportPreservesClipReferencesWhenTakesAreInsertedAndReordered)
{
    namespace R=NLS::Render::Assets;namespace C=NLS::Core::Assets;
    R::ImportedScene scene;scene.sourceAssetId=C::AssetId::New();scene.animationData=std::make_shared<NLS::Animation::Model>();
    for(const auto& [id,name]:std::vector<std::pair<std::string,std::string>>{{"animation/0","Run"},{"animation/1","Walk"},{"animation/2","Idle"}})
    {NLS::Animation::Clip clip;clip.id=id;clip.name=name;scene.animationData->clips.push_back(clip);}
    C::ArtifactManifest previous;previous.subAssets={{scene.sourceAssetId,"animation:animation/0",C::ArtifactType::AnimationClip,"animation","editor","","","Idle"},
        {scene.sourceAssetId,"animation:animation/1",C::ArtifactType::AnimationClip,"animation","editor","","","Walk"}};
    std::string error;ASSERT_TRUE(R::StabilizeAnimationIdentities(scene,&previous,error))<<error;
    EXPECT_EQ(scene.animationData->clips[1].id,"animation/1");EXPECT_EQ(scene.animationData->clips[2].id,"animation/0");EXPECT_NE(scene.animationData->clips[0].id,"animation/0");
}
TEST_F(AnimationFixture,MeshNodeMatrixAndGlobalScaleUseTheSameSkinningSpace)
{
    const auto path=Gltf();std::ifstream input(path);Json json;input>>json;input.close();
    json["nodes"][1]["matrix"]={1,0,0,0,0,1,0,0,0,0,1,0,3,0,0,1};std::ofstream(path)<<json.dump();
    auto meta=NLS::Core::Assets::AssetMeta::CreateForAsset(path);meta.settings["MODEL_GLOBAL_SCALE"]="2";
    NLS::Render::Assets::ImportedScene scene;std::vector<NLS::Render::Resources::Parsers::ParsedMeshData> meshes;std::vector<std::string> materials,dependencies;std::string error;
    ASSERT_TRUE(NLS::Render::Assets::ReadModelSource(path,meta,NLS::Render::Resources::Parsers::EModelParserFlags::TRIANGULATE,meshes,materials,dependencies,scene,error))<<error;
    auto node=std::find_if(scene.nodes.begin(),scene.nodes.end(),[](const auto& n){return n.sourceKey=="node/1";});ASSERT_NE(node,scene.nodes.end());ASSERT_EQ(node->translation.size(),3u);EXPECT_DOUBLE_EQ(node->translation[0],6);
    ASSERT_TRUE(scene.animationData);EXPECT_NEAR(scene.animationData->skins[0].meshFromSkeleton.data[3],-6,1e-6);
}
}
