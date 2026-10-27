#include "AnimationArtifacts.h"
#include <AnimationCompiler/GltfAnimationImporter.h>
#include <algorithm>
#include <map>
#include <set>
#include <Assets/ArtifactDatabase.h>
#include <Assets/AssetMeta.h>
#include <Assets/NativeArtifactContainer.h>
#include <fstream>

namespace NLS::Render::Assets
{
bool StabilizeAnimationIdentities(ImportedScene& scene,const NLS::Core::Assets::ArtifactManifest* previous,std::string& error)
{
    if(!scene.animationData)return true;std::map<std::string,std::vector<std::string>> old;
    if(previous)for(const auto& asset:previous->subAssets)if(asset.artifactType==NLS::Core::Assets::ArtifactType::AnimationClip)
        old[asset.displayName].push_back(asset.subAssetKey);
    scene.animations.clear();std::set<std::string> identities,reserved;
    for(const auto& [name,keys]:old)for(const auto& key:keys){const auto separator=key.find(':');reserved.insert(separator==std::string::npos?key:key.substr(separator+1));}
    for(auto& clip:scene.animationData->clips)
    {
        auto found=old.find(clip.name);
        if(found!=old.end()&&found->second.size()>1){error="Animation Take name is ambiguous during reimport: "+clip.name;return false;}
        if(found!=old.end()&&!found->second.empty())
        {const auto& key=found->second.front();const auto separator=key.find(':');clip.id=separator==std::string::npos?key:key.substr(separator+1);}
        else if(reserved.contains(clip.id)||identities.contains(clip.id))
            clip.id="animation/"+NLS::Guid::NewDeterministic(scene.sourceAssetId.ToString()+":"+clip.name).ToString();
        if(!identities.insert(clip.id).second){error="Animation Clip identity collision during reimport: "+clip.name;return false;}
        scene.animations.push_back({clip.id,clip.name});
    }
    return true;
}
Animation::ImportSettings AnimationSettingsForScene(const ImportedScene& scene)
{
    Animation::ImportSettings settings;settings.sourceId=scene.sourceAssetId.ToString();
    settings.importSkeleton=scene.importSettings.importSkeleton;settings.importAnimations=scene.importSettings.importAnimations;
    settings.sampleRate=scene.importSettings.animationSampleRate;settings.globalScale=scene.importSettings.globalScale;
    settings.axisConversion=scene.importSettings.axisConversion;settings.unitConversion=scene.importSettings.unitConversion;
    settings.takes=scene.importSettings.animationTakes;settings.fbxReader=scene.importSettings.fbxReaderSelection;return settings;
}
bool AttachAnimationSceneData(const std::filesystem::path& source,ImportedScene& scene,std::string& error)
{
    auto extension=source.extension().string();std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char c){return char(std::tolower(c));});
    if((extension==".gltf"||extension==".glb")&&(!scene.skins.empty()||!scene.animations.empty()||scene.animationData))
    {
        scene.animationData=AnimationCompiler::ImportGltfAnimation(source,AnimationSettingsForScene(scene),error);
        if(!error.empty())return false;
    }
    if(!scene.animationData)return true;
    for(auto& node:scene.nodes)for(auto& translation:node.translation)translation*=scene.importSettings.globalScale;
    auto& model=*scene.animationData;model.skeleton.id=scene.sourceAssetId.ToString();
    for(auto& clip:model.clips)clip.skeleton=model.skeleton.id;for(auto& skin:model.skins)skin.skeleton=model.skeleton.id;
    if(!scene.importSettings.importAnimations)model.clips.clear();
    if(!scene.importSettings.importSkeleton){model.skins.clear();model.meshes.clear();}
    if(!scene.importSettings.animationTargetSkeleton.empty())
    {
        auto project=source.parent_path();
        while(!project.empty()&&project.filename()!="Assets")
        {auto parent=project.parent_path();if(parent==project){project.clear();break;}project=parent;}
        project=project.parent_path();
        const auto& reference=scene.importSettings.animationTargetSkeleton;const auto delimiter=reference.find('#');const auto identity=reference.substr(0,delimiter);const auto key=delimiter==std::string::npos?std::string{}:reference.substr(delimiter+1);
        auto guid=NLS::Guid::TryParse(identity);if(!guid){auto meta=NLS::Core::Assets::AssetMeta::Load(NLS::Core::Assets::GetAssetMetaPath(project/identity));if(meta)guid=meta->id.GetGuid();}
        NLS::Core::Assets::ArtifactDatabase database;
        if(project.empty()||!guid||!database.Load(project/"Library"/"ArtifactDB")){error="Animation target skeleton cannot be resolved from the project ArtifactDB";return false;}
        const auto* record=database.Find(NLS::Core::Assets::AssetId(*guid),key,"editor");
        if(!record)for(const auto* candidate:database.FindBySource(NLS::Core::Assets::AssetId(*guid)))if(candidate->artifactType==NLS::Core::Assets::ArtifactType::Skeleton&&(key.empty()||key==candidate->subAssetKey)){record=candidate;break;}
        if(!record){error="Animation target reference does not identify an imported skeleton";return false;}
        auto path=std::filesystem::path(record->artifactPath);if(path.is_relative())path=project/path;
        std::ifstream file(path,std::ios::binary);std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
        auto container=NLS::Core::Assets::ReadNativeArtifactContainer(bytes,NLS::Core::Assets::ArtifactType::Skeleton,1);
        if(!container){error="Animation target skeleton artifact is invalid";return false;}
        auto target=Animation::DeserializeModel(container->payload,error);if(!target||!AnimationCompiler::BindAnimationSkeleton(model,target->skeleton,error))return false;
        scene.dependencies.push_back({NLS::Core::Assets::AssetDependencyKind::ImportedArtifact,guid->ToString(),record->subAssetKey});
    }
    if(!model.Validate(error))return false;
    auto add=[](std::vector<ImportedSceneNamedRecord>& records,const std::string& id,const std::string& name)
    {if(std::none_of(records.begin(),records.end(),[&](const auto& r){return r.sourceKey==id;}))records.push_back({id,name});};
    if(scene.skeletons.empty())add(scene.skeletons,"animation/skeleton",source.stem().string()+" Skeleton");
    for(const auto& skin:model.skins)
    {
        add(scene.skins,skin.id,skin.id);
        for(auto& node:scene.nodes)if(node.sourceKey==skin.meshNode)node.skinKey=skin.id;
    }
    for(const auto& clip:model.clips)add(scene.animations,clip.id,clip.name);
    for(const auto& warning:model.warnings)scene.diagnostics.push_back({"animation-import-quality",warning});
    scene.dependencies.push_back({NLS::Core::Assets::AssetDependencyKind::ImporterVersion,"animation-reader",model.reader});
    return true;
}
std::vector<uint8_t> SerializeAnimationSubAsset(const ImportedScene& scene,const GeneratedSceneSubAsset& asset)
{
    if(!scene.animationData)return {};
    const auto& source=*scene.animationData;Animation::Model model;model.skeleton=source.skeleton;model.reader=source.reader;
    switch(asset.type)
    {
    case ImportedSceneSubAssetType::Skeleton:break;
    case ImportedSceneSubAssetType::AnimationClip:
        for(const auto& c:source.clips)if(c.id==asset.sourceKey)model.clips.push_back(c);
        if(model.clips.empty())return {};
        break;
    case ImportedSceneSubAssetType::Skin:
        for(const auto& s:source.skins)if(s.id==asset.sourceKey)model.skins.push_back(s);
        if(model.skins.empty())model.skins=source.skins;
        break;
    case ImportedSceneSubAssetType::SkeletalMesh:
        for(const auto& mesh:source.meshes)if(mesh.id==asset.sourceKey)
        {model.meshes.push_back(mesh);for(const auto& s:source.skins)if(s.id==mesh.skin)model.skins.push_back(s);}
        if(model.meshes.empty())return {};
        break;
    default:return {};
    }
    return Animation::SerializeModel(model);
}
}
