#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include "MaterialCompiler/MaterialCooker.h"
#include "MaterialCompiler/MaterialGraph.h"
#include "Assets/AssetMeta.h"
#include "Assets/NativeArtifactContainer.h"
#include "Assets/SourceAssetDatabase.h"
#include "Assets/ArtifactDatabase.h"
#include "Rendering/ShaderLab/ShaderLabParser.h"
#include "Rendering/Assets/TextureArtifact.h"

int main(int argc,char** argv)
{
    namespace M=NLS::MaterialCompiler;namespace A=NLS::Core::Assets;namespace R=NLS::Render;
    if(argc<5){std::cerr<<"Usage: NullusMaterialCompile <project> <engine-assets> <release-folder> <material.mat>... [--budget N] [--target platform] [--variant A,B]\n";return 2;}
    try
    {
        const auto project=std::filesystem::absolute(argv[1]).lexically_normal();
        const auto engine=std::filesystem::absolute(argv[2]).lexically_normal();
        M::MaterialCookOptions options;options.outputRoot=std::filesystem::absolute(argv[3]);
        options.compiler.generatedPath=project/"Library"/"MaterialCook"/"Generated"/"Material.hlsl";
        options.compiler.cacheDatabasePath=(project/"Library"/"MaterialCook"/"Cache.tsv").string();
        options.compiler.variantBudget=128;options.compiler.generateGlsl=false;
        options.compiler.includeDirectories={(engine/"Shaders").string()};
        std::vector<std::string> paths;
        for(int i=4;i<argc;++i)
        {
            const std::string argument=argv[i];
            if(argument=="--budget" && i+1<argc)options.compiler.variantBudget=std::stoull(argv[++i]);
            else if(argument=="--target" && i+1<argc)options.targetPlatform=argv[++i];
            else if(argument=="--variant" && i+1<argc)
            {std::istringstream names(argv[++i]);std::string name;R::ShaderLab::ShaderLabKeywordSet keywords;
                while(std::getline(names,name,','))if(!name.empty())keywords.Enable(name);options.variantCollection.push_back(std::move(keywords));}
            else if(argument.starts_with("--")){std::cerr<<"Unknown or incomplete option: "<<argument<<'\n';return 2;}
            else paths.push_back(argument);
        }
        if(options.targetPlatform=="win64")options.compiler.targets={R::ShaderCompiler::ShaderTargetPlatform::DXIL};
        else if(options.targetPlatform=="linux" || options.targetPlatform=="macos")options.compiler.targets={R::ShaderCompiler::ShaderTargetPlatform::SPIRV};
        A::SourceAssetDatabase database;
        const std::vector<A::SourceAssetRoot> roots={{project/"Assets",false},{engine,true}};
        database.ScanRoots(roots);
        const auto resolve=[&](std::string path)
        {
            if(std::filesystem::path(path).is_absolute())return std::filesystem::path(path).lexically_normal();
            if(path.starts_with("Assets/Engine/"))return (engine/path.substr(14)).lexically_normal();
            return (project/path).lexically_normal();
        };
        const auto physical=[&](const R::Assets::MaterialAssetReference& reference)
        {
            if(!reference.guid.empty())if(const auto id=NLS::Guid::TryParse(reference.guid))
                if(const auto* record=database.FindById(A::AssetId(*id)))return record->absolutePath;
            return resolve(reference.path);
        };
        const auto read=[](const std::filesystem::path& path)
        {std::ifstream file(path,std::ios::binary);if(!file)throw std::runtime_error("Cannot read "+path.string());
            return std::string{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};};
        auto closure=std::make_shared<M::MaterialDependencySnapshot>();closure->context="material-command-line-cook";
        std::string error;
        const auto readMaterial=[&](const R::Assets::MaterialAssetReference& reference)->std::optional<R::Assets::MaterialAsset>
        {
            const auto path=physical(reference);const auto bytes=read(path);
            const auto native=A::ReadNativeArtifactContainer(std::vector<uint8_t>(bytes.begin(),bytes.end()),A::ArtifactType::Material,1);
            if(!native){error="Expected a native .mat asset: "+path.string();return std::nullopt;}
            auto asset=R::Assets::DeserializeMaterialAsset(std::string_view(reinterpret_cast<const char*>(native->payload.data()),native->payload.size()),error);
            if(asset)
            {const auto meta=A::AssetMeta::Load(A::GetAssetMetaPath(path));
                asset->identity={meta?meta->id.ToString():native->metadata.sourceAssetId.ToString(),"material:main",path.lexically_relative(project).generic_string()};
                const auto key="material:"+asset->identity.Key();closure->sources[key]={key,path,R::Assets::SerializeMaterialAsset(*asset),{},0,{}};}
            return asset;
        };
        std::vector<M::MaterialCookEntry> materials;std::filesystem::path shaderPath;
        for(const auto& path:paths)
        {
            const auto asset=readMaterial({{}, {},path});if(!asset)throw std::runtime_error(error);
            const auto resolved=R::Assets::MaterialAssetResolver::Resolve(*asset,readMaterial,error);if(!resolved)throw std::runtime_error(error);
            const auto program=physical(resolved->shader);
            if(!shaderPath.empty() && shaderPath!=program)throw std::runtime_error("Cook one shared shader family per invocation");
            shaderPath=program;
            materials.push_back({A::AssetId(NLS::Guid::Parse(asset->identity.guid)),asset->identity.path,*resolved});
        }
        if(shaderPath.empty())throw std::runtime_error("Select at least one material");
        const auto shaderSource=read(shaderPath);const auto shaderMeta=A::AssetMeta::Load(A::GetAssetMetaPath(shaderPath));
        options.shaderId=shaderMeta?shaderMeta->id:A::AssetId(NLS::Guid::NewDeterministic(shaderPath.generic_string()));
        options.compiler.sourcePath=shaderPath;options.compiler.sourceIdentity=materials.front().resolved.shader.path;
        const auto key="file:"+shaderPath.generic_string();closure->sources[key]={key,shaderPath,shaderSource,{},0,{}};
        R::ShaderLab::ShaderLabAssetDesc shader;R::Resources::MaterialParameterSchema schema;
        if(shaderPath.extension()==".shadergraph")
        {
            M::MaterialGraphAsset graph;if(!M::DeserializeMaterialGraph(shaderSource,graph,error))throw std::runtime_error(error);
            const auto generated=M::GenerateMaterialGraphShader(graph,[&](const auto& reference)->std::optional<M::MaterialGraphAsset>
            {const auto path=physical(reference);const auto source=read(path);M::MaterialGraphAsset function;
                if(!M::DeserializeMaterialGraph(source,function,error))return std::nullopt;
                const auto id="file:"+path.generic_string();closure->sources[id]={id,path,source,{},0,{}};return function;});
            if(!generated.Succeeded())throw std::runtime_error(generated.diagnostics.front().message);shader=generated.shader;schema=generated.schema;
        }
        else
        {const auto parsed=R::ShaderLab::ParseShaderLabSource(shaderSource,shaderPath.generic_string());if(!parsed.Succeeded())throw std::runtime_error(parsed.DiagnosticsToString());
            shader=parsed.asset;schema=M::BuildMaterialParameterSchema(shader,options.shaderId.ToString());}
        options.compiler.dependencies=closure;
        options.textureProvider=[&](std::string_view path)->std::optional<M::MaterialCookTexture>
        {
            A::ArtifactDatabase imported;
            if(imported.Load(project/"Library"/"ArtifactDB"))
                for(const auto& record:imported.GetRecords())
                    if(record.artifactType==A::ArtifactType::Texture && record.sourcePath==path && record.status==A::ArtifactRecordStatus::UpToDate)
                    {const auto bytes=read(resolve(record.artifactPath));return M::MaterialCookTexture{record.sourceAssetId,record.subAssetKey,
                        std::vector<uint8_t>(bytes.begin(),bytes.end())};}
            const auto bytes=read(resolve(std::string(path)));const std::vector<uint8_t> data(bytes.begin(),bytes.end());
            const auto texture=R::Assets::DeserializeTextureArtifact(data);if(!texture)return std::nullopt;
            const auto native=A::ReadNativeArtifactContainer(data,A::ArtifactType::Texture,4);
            const auto id=native && native->metadata.sourceAssetId.IsValid()?native->metadata.sourceAssetId:A::AssetId(NLS::Guid::NewDeterministic(path));
            return M::MaterialCookTexture{id,native && !native->metadata.subAssetKey.empty()?native->metadata.subAssetKey:"texture:main",data};
        };
        const auto result=M::CookMaterialFamily(shader,schema,materials,options);
        if(!result.Succeeded())throw std::runtime_error(result.error);
        std::cout<<result.databasePath.string()<<"\nSnapshot: "<<result.snapshotId<<'\n';return 0;
    }
    catch(const std::exception& ex){std::cerr<<ex.what()<<'\n';return 1;}
}
