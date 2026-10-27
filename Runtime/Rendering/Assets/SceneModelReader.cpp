#include "SceneModelReader.h"
#include "AnimationArtifacts.h"
#include <Rendering/Resources/Parsers/AssimpParser.h>
#include <Rendering/Resources/Parsers/FbxSdkParser.h>
#include <AnimationCompiler/GltfAnimationImporter.h>
#include <algorithm>
#include <sstream>

namespace NLS::Render::Assets
{
SceneImportSettings ReadSceneImportSettings(const std::map<std::string,std::string>& values)
{
    SceneImportSettings s;
    auto text=[&](const char* key,std::string fallback={}){auto found=values.find(key);return found==values.end()?fallback:found->second;};
    auto flag=[&](const char* key,bool fallback){const auto value=text(key);return value.empty()?fallback:value=="1"||value=="true"||value=="True";};
    auto number=[&](const char* key,double fallback){try{const auto value=text(key);return value.empty()?fallback:std::stod(value);}catch(...){return fallback;}};
    s.globalScale=number("MODEL_GLOBAL_SCALE",1);s.axisConversion=text("MODEL_AXIS_CONVERSION");s.unitConversion=text("MODEL_UNIT_CONVERSION");s.hierarchyPolicy=text("MODEL_HIERARCHY_POLICY","preserve");
    s.importSkeleton=flag("MODEL_IMPORT_SKELETON",true);s.importAnimations=flag("MODEL_IMPORT_ANIMATIONS",true);s.importMaterials=flag("MODEL_IMPORT_MATERIALS",true);
    s.importNormals=flag("MODEL_IMPORT_NORMALS",true);s.importTangents=flag("MODEL_IMPORT_TANGENTS",true);s.importUvs=flag("MODEL_IMPORT_UVS",true);
    s.importMorphTargets=flag("MODEL_IMPORT_MORPH_TARGETS",true);s.importCameras=flag("MODEL_IMPORT_CAMERAS",false);s.importLights=flag("MODEL_IMPORT_LIGHTS",false);
    s.animationSampleRate=number("MODEL_ANIMATION_SAMPLE_RATE",0);s.animationTargetSkeleton=text("MODEL_ANIMATION_TARGET_SKELETON");
    std::istringstream takes(text("MODEL_ANIMATION_TAKES"));std::string take;while(std::getline(takes,take,';'))if(!take.empty())s.animationTakes.push_back(take);
    const auto reader=text("MODEL_FBX_READER");
    if(reader=="assimp")s.fbxReaderSelection=Animation::FbxReaderSelection::Assimp;
    else if(reader=="autodesk")s.fbxReaderSelection=Animation::FbxReaderSelection::Autodesk;
    s.lodGroup=text("LOD_GROUP","None");s.importMeshLODs=flag("IMPORT_MESH_LODS",false);s.minLOD=uint32_t(std::max(0.,number("MIN_LOD",0)));s.autoComputeLODScreenSize=flag("AUTO_COMPUTE_LOD_SCREEN_SIZE",true);
    return s;
}
Animation::ImportSettings AnimationSettingsForAsset(const NLS::Core::Assets::AssetMeta& meta)
{ImportedScene scene;scene.sourceAssetId=meta.id;scene.importSettings=ReadSceneImportSettings(meta.settings);return AnimationSettingsForScene(scene);}
bool ReadModelSource(const std::filesystem::path& source,const NLS::Core::Assets::AssetMeta& meta,
    Resources::Parsers::EModelParserFlags flags,std::vector<Resources::Parsers::ParsedMeshData>& meshes,
    std::vector<std::string>& materials,std::vector<std::string>& dependencies,ImportedScene& scene,std::string& error)
{
    using namespace Resources::Parsers;
    auto extension=source.extension().string();std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char c){return char(std::tolower(c));});
    const auto format=extension==".fbx"?SceneModelSourceFormat::Fbx:extension==".obj"?SceneModelSourceFormat::Obj:SceneModelSourceFormat::Gltf;
    const auto settings=AnimationSettingsForAsset(meta);bool loaded=false,fallback=false;
    auto import=[&](auto& parser)
    {
        meshes.clear();materials.clear();dependencies.clear();
        if(!parser.LoadModelData(source.string(),meshes,materials,flags,&dependencies,false,settings))return false;
        scene=ImportParsedModelScene(meshes,materials,source,format,meta.id,source.stem().string());scene.importSettings=ReadSceneImportSettings(meta.settings);
        parser.PopulateImportedSceneData(source,format,scene);return true;
    };
    if(format==SceneModelSourceFormat::Fbx&&settings.fbxReader!=Animation::FbxReaderSelection::Assimp)
    {
        FbxSdkParser parser;loaded=import(parser);
        if(!loaded&&settings.fbxReader==Animation::FbxReaderSelection::Autodesk){error="The selected Autodesk FBX reader is unavailable or rejected the file";return false;}
        fallback=!loaded;
    }
    if(!loaded){AssimpParser parser;loaded=import(parser);}
    if(!loaded&&format!=SceneModelSourceFormat::Gltf){error="The selected model reader rejected the source";return false;}
    if(format==SceneModelSourceFormat::Gltf&&(!loaded||scene.animationData))
    {
        AnimationCompiler::GltfSourcePayload payload;if(!AnimationCompiler::ReadGltfSourcePayload(source,settings,payload,error))return false;
        scene=ImportGltfSceneJson(payload.json,meta.id,source.stem().string(),ReadSceneImportSettings(meta.settings));
        meshes.clear();materials.clear();dependencies=std::move(payload.externalFiles);
        for(const auto& primitive:payload.primitives)
        {
            ParsedMeshData parsed;parsed.sourceKey=primitive.id;parsed.materialIndex=primitive.material;
            const auto separator=primitive.id.find('/',5);parsed.sourceMeshIndex=uint32_t(std::stoul(primitive.id.substr(5,separator-5)));
            parsed.indices=primitive.indices;parsed.vertices.resize(primitive.vertices.size());
            for(size_t i=0;i<parsed.vertices.size();++i)
            {
                const auto& input=primitive.vertices[i];auto& output=parsed.vertices[i];
                std::copy_n(input.position,3,output.position);std::copy_n(input.texCoords,2,output.texCoords);std::copy_n(input.normals,3,output.normals);
                std::copy_n(input.tangent,3,output.tangent);std::copy_n(input.bitangent,3,output.bitangent);
            }
            meshes.push_back(std::move(parsed));
        }
        for(const auto& material:scene.materials)materials.push_back(material.name);
        for(auto& texture:scene.textures)if(auto image=payload.embeddedImages.find(texture.sourceKey);image!=payload.embeddedImages.end())
        {texture.embeddedBytes=std::move(image->second);texture.embeddedCompressed=true;texture.embeddedFormat=texture.mimeType=="image/jpeg"?"jpg":"png";}
    }
    if(fallback)scene.diagnostics.push_back({"fbx-reader-fallback","Autodesk FBX reader was unavailable or rejected the file; the whole source was imported with Assimp"});
    return AttachAnimationSceneData(source,scene,error);
}
}
