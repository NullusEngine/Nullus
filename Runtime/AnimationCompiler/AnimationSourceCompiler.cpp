#include "AnimationSourceCompiler.h"
#include "GltfAnimationImporter.h"
#include <Assets/AssetMeta.h>
#include <Assets/NativeArtifactContainer.h>
#include <fstream>
#include <set>
#include <cmath>

namespace NLS::AnimationCompiler
{
using namespace Animation;
using namespace Core::Assets;
namespace
{
std::vector<uint8_t> Bytes(const std::filesystem::path& path)
{std::ifstream file(path,std::ios::binary);return {std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};}
struct Resolver
{
    const std::filesystem::path& root;
    const ArtifactDatabase& database;
    std::string target;
    std::vector<AssetDependencyRecord>& dependencies;
    std::map<std::string,NativeArtifactContainer> cache;
    const NativeArtifactContainer* Get(const std::string& reference,std::string& error)
    {
        if(auto found=cache.find(reference);found!=cache.end())return &found->second;
        const auto delimiter=reference.find('#');const auto identity=reference.substr(0,delimiter);const auto key=delimiter==std::string::npos?std::string{}:reference.substr(delimiter+1);
        auto guid=NLS::Guid::TryParse(identity);if(!guid){auto meta=AssetMeta::Load(GetAssetMetaPath(root/identity));if(meta)guid=meta->id.GetGuid();}
        if(!guid){error="Animation reference requires a source GUID or asset path: "+reference;return nullptr;}
        const auto id=AssetId(*guid);const ArtifactDatabaseRecord* record=key.empty()?nullptr:database.Find(id,key,target);
        if(!record)for(const auto* candidate:database.FindBySource(id))
            if(candidate->targetPlatform==target&&(key.empty()?candidate->subAssetKey==candidate->primarySubAssetKey:candidate->subAssetKey==key)){record=candidate;break;}
        if(!record){error="Animation dependency has no imported artifact: "+reference;return nullptr;}
        auto path=std::filesystem::path(record->artifactPath);if(path.is_relative())path=root/path;const auto bytes=Bytes(path);
        auto container=ReadNativeArtifactContainer(bytes,record->artifactType,1);if(!container){error="Animation dependency artifact is invalid: "+reference;return nullptr;}
        dependencies.push_back({AssetDependencyKind::ImportedArtifact,id.ToString(),record->subAssetKey});
        dependencies.push_back({AssetDependencyKind::SourceAssetGuid,id.ToString(),{}});
        dependencies.push_back({AssetDependencyKind::SourceFileHash,path.generic_string(),record->contentHash});
        return &cache.emplace(reference,std::move(*container)).first->second;
    }
    std::shared_ptr<Model> ModelFor(const std::string& reference,std::string& error)
    {auto* container=Get(reference,error);return container?DeserializeModel(container->payload,error):nullptr;}
    bool ClipFor(const std::string& reference,Resources& resources,std::string& error)
    {
        if(resources.clips.contains(reference))return true;auto model=ModelFor(reference,error);if(!model||model->clips.size()!=1){if(error.empty())error="Animation reference must identify one Clip: "+reference;return false;}
        if(!resources.skeleton)resources.skeleton=std::make_shared<Skeleton>(model->skeleton);
        else if(model->skeleton.id!=resources.skeleton->id)
        {if(!BindAnimationSkeleton(*model,*resources.skeleton,error))return false;}
        resources.clips[reference]=std::make_shared<Clip>(model->clips.front());return true;
    }
};
}
bool CompileSource(const std::filesystem::path& root,const ArtifactDatabase& database,std::string_view target,
    const std::filesystem::path& path,std::string_view source,SourceCompileResult& result,std::string& error)
{
    result={};Resolver resolve{root,database,std::string(target),result.dependencies};Resources resources;
    const auto extension=path.extension().string();const auto name=path.stem().string();result.payload.displayName=name;result.payload.relativePath=name;
    if(extension==".animgraph")
    {
        GraphAsset graph;if(!Deserialize(source,graph,error))return false;
        auto skeleton=resolve.ModelFor(graph.skeleton,error);if(!skeleton)return false;resources.skeleton=std::make_shared<Skeleton>(skeleton->skeleton);
        graph.skeleton=resources.skeleton->id;
        for(const auto& node:graph.nodes)
        {
            if(!node.clip.empty()&&!resolve.ClipFor(node.clip,resources,error))return false;
            if(!node.blendSpace.empty()&&!resources.blendSpaces.contains(node.blendSpace))
            {
                auto* container=resolve.Get(node.blendSpace,error);if(!container)return false;BlendSpaceAsset blend;
                if(!Deserialize(std::string_view(reinterpret_cast<const char*>(container->payload.data()),container->payload.size()),blend,error))return false;
                blend.skeleton=resources.skeleton->id;for(const auto& sample:blend.samples)if(!resolve.ClipFor(sample.clip,resources,error))return false;resources.blendSpaces[node.blendSpace]=std::move(blend);
            }
            if(!node.mask.empty()&&!resources.masks.contains(node.mask))
            {
                auto* container=resolve.Get(node.mask,error);if(!container)return false;BoneMaskAsset mask;
                if(!Deserialize(std::string_view(reinterpret_cast<const char*>(container->payload.data()),container->payload.size()),mask,error))return false;mask.skeleton=resources.skeleton->id;resources.masks[node.mask]=std::move(mask);
            }
        }
        const auto compiled=Compile(graph,resources);if(!compiled.Succeeded()){error=compiled.diagnostics.empty()?"Animation graph compilation failed":compiled.diagnostics.front().message;return false;}
        result.program=compiled.program;result.payload.payload=SerializeProgram(*compiled.program);result.payload.artifactType=ArtifactType::AnimationGraph;result.payload.loaderId="animation-graph";result.payload.subAssetKey="animation-graph:"+name;
    }
    else if(extension==".animclip")
    {
        ClipAsset asset;if(!Deserialize(source,asset,error)||!resolve.ClipFor(asset.source,resources,error))return false;
        asset.skeleton=resources.skeleton->id;auto clip=CompileClip(asset,resources,error);if(!clip)return false;
        auto model=std::make_shared<Model>();model->skeleton=*resources.skeleton;model->clips.push_back(*clip);model->reader="Nullus.AuthoredClip.v1";
        result.model=model;result.payload.payload=SerializeModel(*model);result.payload.artifactType=ArtifactType::AnimationClip;result.payload.loaderId="animation";result.payload.subAssetKey="animation:"+name;
    }
    else if(extension==".blendspace")
    {
        BlendSpaceAsset blend;if(!Deserialize(source,blend,error))return false;
        for(const auto& sample:blend.samples)if(!resolve.ClipFor(sample.clip,resources,error))return false;
        if(!resources.skeleton){error="BlendSpace requires samples";return false;}blend.skeleton=resources.skeleton->id;
        if(!CompileBlendSpace(blend,resources,error))return false;
        const auto text=Serialize(blend);result.payload.payload.assign(text.begin(),text.end());result.payload.artifactType=ArtifactType::BlendSpace;result.payload.loaderId="blend-space";result.payload.subAssetKey="blend-space:"+name;
    }
    else if(extension==".bonemask")
    {
        BoneMaskAsset mask;if(!Deserialize(source,mask,error))return false;auto model=resolve.ModelFor(mask.skeleton,error);if(!model)return false;
        if(!std::isfinite(mask.defaultWeight)||mask.defaultWeight<0||mask.defaultWeight>1){error="Bone mask default weight is invalid";return false;}
        for(const auto& [bone,weight]:mask.weights)if(model->skeleton.Find(bone)==NoBone||!std::isfinite(weight)||weight<0||weight>1){error="Bone mask refers to an invalid bone or weight: "+bone;return false;}
        const auto text=Serialize(mask);result.payload.payload.assign(text.begin(),text.end());result.payload.artifactType=ArtifactType::BoneMask;result.payload.loaderId="bone-mask";result.payload.subAssetKey="bone-mask:"+name;
    }
    else {error="Unsupported animation source extension";return false;}
    result.payload.dependencies=result.dependencies;return true;
}
}
