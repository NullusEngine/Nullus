#include "AnimationAsset.h"
#include <AnimationCompiler/AnimationCompiler.h>
#include <Assets/NativeArtifactContainer.h>
#include <Assets/ContentSource.h>
#include <Rendering/Assets/ArtifactFileReader.h>
#include <Core/ServiceLocator.h>
#include <Core/ResourceManagement/ShaderManager.h>
#include "Assets/RuntimeAssetDatabase.h"
#include <mutex>
#include <unordered_map>
#include <fstream>

namespace NLS::Engine::Assets
{
bool AnimationAsset::Reload(const std::filesystem::path& path,Core::Assets::ArtifactType type,std::string hash,std::string& error)
{
    Render::Assets::Detail::ArtifactFileReader file(path);
    if(!file.IsOpen()||file.Size()>uint64_t(2)*1024*1024*1024){error="Cannot read animation artifact or artifact exceeds the size limit";return false;}
    std::vector<uint8_t> bytes(size_t(file.Size()));
    const Render::Assets::Detail::ArtifactFileReadRange range{bytes.data(),bytes.size()};
    if(!file.Read(0,std::span<const Render::Assets::Detail::ArtifactFileReadRange>(&range,1),nullptr)){error="Cannot read animation artifact bytes";return false;}
    auto container=Core::Assets::ReadNativeArtifactContainer(bytes,type,1);
    if(!container){error="Animation asset is not a compatible native artifact";return false;}
    if(type==Core::Assets::ArtifactType::AnimationGraph)
    {auto program=AnimationCompiler::DeserializeProgram(container->payload,error);if(!program)return false;m_program=std::move(program);m_model.reset();}
    else
    {auto model=Animation::DeserializeModel(container->payload,error);if(!model)return false;m_model=std::move(model);m_program.reset();}
    m_hash=std::move(hash);return true;
}
std::shared_ptr<AnimationAsset> LoadAnimationAsset(const Serialize::ObjectIdentifier& identifier,std::string& error)
{
    if(!identifier.IsAsset()||!Core::ServiceLocator::Contains<RuntimeAssetDatabase>()){error="Animation asset requires a runtime manifest and stable reference";return {};}
    const auto* entry=Core::ServiceLocator::Get<RuntimeAssetDatabase>().ResolveByLocalIdentifierInFile(Core::Assets::AssetId(identifier.guid),identifier.localIdentifierInFile);
    if(!entry){error="Animation asset reference is not in the runtime manifest";return {};}
    const auto type=entry->artifactType;
    if(type!=Core::Assets::ArtifactType::Skeleton&&type!=Core::Assets::ArtifactType::Skin&&type!=Core::Assets::ArtifactType::SkeletalMesh&&type!=Core::Assets::ArtifactType::AnimationClip&&type!=Core::Assets::ArtifactType::AnimationGraph)
    {error="Reference does not resolve to an animation artifact";return {};}
    auto path=std::filesystem::path(entry->artifactPath);
    if(path.is_relative()){auto assets=std::filesystem::path(Core::ResourceManagement::ShaderManager::ProjectAssetsRoot());if(assets.filename().empty())assets=assets.parent_path();path=assets.parent_path()/path;}
    static std::mutex mutex;static std::unordered_map<std::string,std::weak_ptr<AnimationAsset>> cache;
    const auto key=Core::ResourceManagement::ShaderManager::ProjectAssetsRoot()+":"+identifier.guid.ToString()+":"+std::to_string(identifier.localIdentifierInFile);
    std::lock_guard lock(mutex);std::erase_if(cache,[](const auto& item){return item.second.expired();});auto asset=cache[key].lock();
    if(!asset)asset=std::make_shared<AnimationAsset>();
    if(asset->ContentHash()!=entry->contentHash&&!asset->Reload(path,type,entry->contentHash,error))return {};
    if(Serialize::PersistentManager::Instance().BindObjectIdentifier(*asset,identifier)==NLS::InstanceID_None){error="Animation reference is bound to another object";return {};}
    cache[key]=asset;error.clear();return asset;
}
}
