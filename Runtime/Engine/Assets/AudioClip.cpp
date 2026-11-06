#include "AudioClip.h"
#include "Assets/RuntimeAssetDatabase.h"
#include <Assets/NativeArtifactContainer.h>
#include <Rendering/Assets/ArtifactFileReader.h>
#include <Core/ServiceLocator.h>
#include <Core/ResourceManagement/ShaderManager.h>
#include <algorithm>
#include <mutex>
#include <unordered_map>
namespace NLS::Engine::Assets
{
bool AudioClip::Reload(const std::filesystem::path& path,std::string hash,std::string& error)
{
    Render::Assets::Detail::ArtifactFileReader file(path);
    if (!file.IsOpen() || file.Size()>uint64_t(256)*1024*1024) {error="Audio artifact is unavailable or exceeds the size limit.";return false;}
    std::vector<uint8_t> bytes(static_cast<size_t>(file.Size()));
    const Render::Assets::Detail::ArtifactFileReadRange range{bytes.data(),bytes.size()};
    if (!file.Read(0,std::span<const Render::Assets::Detail::ArtifactFileReadRange>(&range,1),nullptr)) {error="Audio artifact read failed.";return false;}
    const auto container=Core::Assets::ReadNativeArtifactContainer(bytes,Core::Assets::ArtifactType::Audio,1);
    if (!container) {error="Audio artifact is not a compatible native container.";return false;}
    auto pcm=Audio::DecodeAudio(container->payload,error);
    if (!pcm) return false;
    m_pcm=std::move(pcm);m_hash=std::move(hash);return true;
}
std::shared_ptr<AudioClip> LoadAudioClip(const Serialize::ObjectIdentifier& identifier,std::string& error)
{
    if (!identifier.IsAsset() || !Core::ServiceLocator::Contains<RuntimeAssetDatabase>()) {error="AudioClip requires a stable runtime asset reference.";return {};}
    const auto* entry=Core::ServiceLocator::Get<RuntimeAssetDatabase>().ResolveByLocalIdentifierInFile(Core::Assets::AssetId(identifier.guid),identifier.localIdentifierInFile);
    if (!entry || entry->artifactType!=Core::Assets::ArtifactType::Audio) {error="Reference does not resolve to an Audio artifact.";return {};}
    auto path=std::filesystem::path(entry->artifactPath);
    if(path.is_relative())
    { auto assets=std::filesystem::path(Core::ResourceManagement::ShaderManager::ProjectAssetsRoot());if(assets.filename().empty())assets=assets.parent_path();path=assets.parent_path()/path; }
    static std::mutex mutex; static std::unordered_map<std::string,std::weak_ptr<AudioClip>> cache;
    const auto key=Core::ResourceManagement::ShaderManager::ProjectAssetsRoot()+":"+identifier.guid.ToString()+":"+std::to_string(identifier.localIdentifierInFile);
    std::lock_guard lock(mutex);
    std::erase_if(cache,[](const auto& pair){return pair.second.expired();});
    auto clip=cache[key].lock();if(!clip)clip=std::make_shared<AudioClip>();
    if (clip->ContentHash()!=entry->contentHash && !clip->Reload(path,entry->contentHash,error)) return {};
    if (Serialize::PersistentManager::Instance().BindObjectIdentifier(*clip,identifier)==NLS::InstanceID_None) {error="AudioClip reference is bound to another object.";return {};}
    cache[key]=clip;error.clear();return clip;
}
}
