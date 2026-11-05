#include "Assets/SpriteAsset.h"
#include "Assets/RuntimeAssetDatabase.h"
#include <Assets/NativeArtifactContainer.h>
#include <Core/ServiceLocator.h>
#include <Core/ResourceManagement/ShaderManager.h>
#include <Rendering/Assets/ArtifactFileReader.h>
#include <mutex>
#include <unordered_map>

namespace NLS::Engine::Assets
{
bool SpriteAsset::Reload(const std::filesystem::path& path, std::string hash, std::string& error)
{
    Render::Assets::Detail::ArtifactFileReader file(path);
    if (!file.IsOpen() || file.Size() > 1024 * 1024)
    { error = "Cannot read sprite artifact or it exceeds the size limit"; return false; }
    std::vector<uint8_t> bytes(size_t(file.Size()));
    const Render::Assets::Detail::ArtifactFileReadRange range {bytes.data(), bytes.size()};
    if (!file.Read(0, std::span(&range, 1), nullptr))
    { error = "Cannot read sprite artifact bytes"; return false; }
    const auto container = Core::Assets::ReadNativeArtifactContainer(bytes,
        Core::Assets::ArtifactType::Sprite, Core::Assets::SpriteArtifactVersion);
    if (!container) { error = "Invalid sprite native artifact"; return false; }
    auto sprite = Core::Assets::DeserializeSpriteArtifact(container->payload, error);
    if (!sprite) return false;
    if (container->metadata.subAssetKey != sprite->SubAssetKey() ||
        container->metadata.sourceAssetId != sprite->textureAssetId)
    { error = "Sprite payload identity does not match its artifact metadata"; return false; }
    m_data = std::make_shared<const Core::Assets::SpriteArtifact>(std::move(*sprite));
    m_hash = std::move(hash);
    error.clear();
    return true;
}

std::shared_ptr<SpriteAsset> LoadSpriteAsset(const Serialize::ObjectIdentifier& identifier, std::string& error)
{
    if (!identifier.IsAsset() || !Core::ServiceLocator::Contains<RuntimeAssetDatabase>())
    { error = "Sprite requires a stable asset reference and runtime manifest"; return {}; }
    const auto* entry = Core::ServiceLocator::Get<RuntimeAssetDatabase>().ResolveByLocalIdentifierInFile(
        Core::Assets::AssetId(identifier.guid), identifier.localIdentifierInFile);
    if (!entry || entry->artifactType != Core::Assets::ArtifactType::Sprite)
    { error = "Sprite reference is missing from the runtime manifest"; return {}; }
    auto root = std::filesystem::path(Core::ResourceManagement::ShaderManager::ProjectAssetsRoot());
    if (root.filename().empty()) root = root.parent_path();
    auto path = std::filesystem::path(entry->artifactPath);
    if (path.is_relative()) path = root.parent_path() / path;
    static std::mutex mutex;
    static std::unordered_map<std::string, std::weak_ptr<SpriteAsset>> cache;
    const auto key = root.generic_string() + ":" + identifier.guid.ToString() + ":" +
        std::to_string(identifier.localIdentifierInFile);
    std::lock_guard lock(mutex);
    std::erase_if(cache, [](const auto& item) { return item.second.expired(); });
    auto asset = cache[key].lock();
    if (!asset) asset = std::make_shared<SpriteAsset>();
    if (asset->ContentHash() != entry->contentHash && !asset->Reload(path, entry->contentHash, error)) return {};
    if (Serialize::PersistentManager::Instance().BindObjectIdentifier(*asset, identifier) == NLS::InstanceID_None)
    { error = "Sprite reference is already bound to another object"; return {}; }
    cache[key] = asset;
    error.clear();
    return asset;
}
}
