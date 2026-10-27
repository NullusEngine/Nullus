#include "ParticleSystemAsset.h"

#include <mutex>
#include <unordered_map>
#include <Core/ResourceManagement/ShaderManager.h>
#include <Core/ServiceLocator.h>
#include "Assets/RuntimeAssetDatabase.h"

namespace NLS::Engine::Assets
{
bool ParticleSystemAsset::Reload(const std::filesystem::path& path, std::string hash, std::string& error)
{
    auto program = std::make_shared<VFX::CompiledSystem>();
    if (!VFX::LoadCompiledSystemArtifact(path,*program,error)) return false;
    m_program = std::move(program); m_hash = std::move(hash); return true;
}

std::shared_ptr<ParticleSystemAsset> LoadParticleSystemAsset(const Serialize::ObjectIdentifier& identifier, std::string& error)
{
    if (!identifier.IsAsset() || !Core::ServiceLocator::Contains<RuntimeAssetDatabase>())
    { error = "Particle asset requires a stable reference and runtime artifact manifest"; return {}; }
    const auto& database = Core::ServiceLocator::Get<RuntimeAssetDatabase>();
    const auto* entry = database.ResolveByLocalIdentifierInFile(Core::Assets::AssetId(identifier.guid),identifier.localIdentifierInFile);
    if (!entry || entry->artifactType != Core::Assets::ArtifactType::VfxSystem || entry->loaderId != "vfx-system")
    { error = "Particle reference does not resolve to a VFX system artifact"; return {}; }
    auto path = std::filesystem::path(entry->artifactPath);
    if (path.is_relative())
    {
        auto assets = std::filesystem::path(Core::ResourceManagement::ShaderManager::ProjectAssetsRoot());
        if (assets.filename().empty()) assets = assets.parent_path();
        if (assets.empty()) { error = "Particle artifact project root is unavailable"; return {}; }
        path = assets.parent_path()/path;
    }
    struct Cache
    {
        std::mutex mutex;
        std::unordered_map<std::string,std::weak_ptr<ParticleSystemAsset>> assets;
    };
    static Cache cache;
    // Content-addressed filenames change on every reimport. Cache by project
    // root and reference identity so hot reload retains the bound Object marker.
    auto projectRoot = path.lexically_normal();
    const auto portable = Core::Assets::TryMakePortableContentArtifactPath(projectRoot.generic_string());
    if (portable.empty()) { error = "Particle artifact path is not native content storage"; return {}; }
    for (const auto& part : std::filesystem::path(portable)) { (void)part; projectRoot = projectRoot.parent_path(); }
    const auto key = projectRoot.generic_string()+":"+identifier.guid.ToString()+":"+std::to_string(identifier.localIdentifierInFile);
    std::lock_guard lock(cache.mutex);
    std::erase_if(cache.assets,[](const auto& item) { return item.second.expired(); });
    auto asset = cache.assets[key].lock();
    if (asset && asset->ContentHash() == entry->contentHash)
    {
        if (Serialize::PersistentManager::Instance().BindObjectIdentifier(*asset,identifier) == NLS::InstanceID_None)
        { error = "Particle asset reference is already bound to another object"; return {}; }
        error.clear(); return asset;
    }
    if (!asset) asset = std::make_shared<ParticleSystemAsset>();
    if (!asset->Reload(path,entry->contentHash,error)) return {};
    if (Serialize::PersistentManager::Instance().BindObjectIdentifier(*asset,identifier) == NLS::InstanceID_None)
    { error = "Particle asset reference is already bound to another object"; return {}; }
    cache.assets[key] = asset; error.clear(); return asset;
}
}
