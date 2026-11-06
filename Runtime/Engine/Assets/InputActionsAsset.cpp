#include "InputActionsAsset.h"
#include <Core/Inputs/InputSerialization.h>
#include <Core/Assets/NativeArtifactContainer.h>
#include <Rendering/Assets/ArtifactFileReader.h>
#include <Core/ServiceLocator.h>
#include <Core/ResourceManagement/ShaderManager.h>
#include "RuntimeAssetDatabase.h"
#include <algorithm>
#include <mutex>
#include <unordered_map>

namespace NLS::Engine::Assets
{
bool InputActionsAsset::Reload(const std::filesystem::path& path, std::string contentHash, std::vector<std::string>& diagnostics)
{
    Render::Assets::Detail::ArtifactFileReader file(path);
    if (!file.IsOpen() || file.Size() > 64u * 1024u * 1024u) { diagnostics.emplace_back("Input artifact is unreadable or too large"); return false; }
    std::vector<uint8_t> bytes(static_cast<size_t>(file.Size()));
    const Render::Assets::Detail::ArtifactFileReadRange range{bytes.data(), bytes.size()};
    if (!file.Read(0, std::span<const Render::Assets::Detail::ArtifactFileReadRange>(&range, 1), nullptr)) { diagnostics.emplace_back("Cannot read input artifact"); return false; }
    auto container = Core::Assets::ReadNativeArtifactContainer(bytes, Core::Assets::ArtifactType::InputActions, 1);
    if (!container) { diagnostics.emplace_back("Input artifact metadata or hash is invalid"); return false; }
    Input::InputDefinition definition;
    if (!Core::Inputs::ParseInputDefinition(std::string_view(reinterpret_cast<const char*>(container->payload.data()), container->payload.size()), definition, diagnostics)) return false;
    auto compiled = Input::CompiledInput::Compile(std::move(definition), diagnostics);
    if (!compiled) return false;
    m_definition = std::move(compiled);
    m_contentHash = std::move(contentHash);
    return true;
}
void InputActionsAsset::ApplyTo(Input::InputUser& user) const { user.SetDefinition(m_definition); }
std::shared_ptr<InputActionsAsset> LoadInputActionsAsset(const Serialize::ObjectIdentifier& identifier, std::string& error)
{
    if (!identifier.IsAsset() || !Core::ServiceLocator::Contains<RuntimeAssetDatabase>()) { error = "Input Actions require a stable runtime asset reference"; return {}; }
    const auto* entry = Core::ServiceLocator::Get<RuntimeAssetDatabase>().ResolveByLocalIdentifierInFile(Core::Assets::AssetId(identifier.guid), identifier.localIdentifierInFile);
    if (!entry || entry->artifactType != Core::Assets::ArtifactType::InputActions) { error = "Input Actions are absent from the runtime manifest"; return {}; }
    auto path = std::filesystem::path(entry->artifactPath);
    if (path.is_relative())
    {
        auto assets = std::filesystem::path(Core::ResourceManagement::ShaderManager::ProjectAssetsRoot());
        if (assets.filename().empty()) assets = assets.parent_path();
        path = assets.parent_path() / path;
    }
    static std::mutex mutex;
    static std::unordered_map<std::string, std::weak_ptr<InputActionsAsset>> cache;
    const auto key = Core::ResourceManagement::ShaderManager::ProjectAssetsRoot() + ":" + identifier.guid.ToString() + ":" + std::to_string(identifier.localIdentifierInFile);
    std::lock_guard lock(mutex);
    std::erase_if(cache, [](const auto& item) { return item.second.expired(); });
    auto asset = cache[key].lock();
    if (!asset) asset = std::make_shared<InputActionsAsset>();
    std::vector<std::string> diagnostics;
    if ((!asset->GetDefinition() || asset->GetContentHash() != entry->contentHash) && !asset->Reload(path, entry->contentHash, diagnostics)) { error = diagnostics.empty() ? "Cannot load Input Actions" : diagnostics.front(); return {}; }
    if (Serialize::PersistentManager::Instance().BindObjectIdentifier(*asset, identifier) == NLS::InstanceID_None) { error = "Input Actions reference is bound to another object"; return {}; }
    cache[key] = asset;
    error.clear();
    return asset;
}
}
