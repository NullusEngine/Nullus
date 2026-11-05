#include "PluginBuildInputs.h"
#include "Plugins/RuntimeDeployment.h"
#include "Core/Assets/ContentHash.h"
#include "Core/Assets/ArtifactManifest.h"
#include "Core/Assets/PluginAssetPath.h"
#include "Core/Assets/AssetMeta.h"
#include <set>
#include <algorithm>
namespace NLS::Build
{
PluginBuildInputs::PluginBuildInputs(const std::filesystem::path& project,const std::string& platform) : m_project(project)
{
    using namespace Core::Assets;
    const auto configuration = Plugins::PluginManager::ReadConfiguration(project); m_configuration = configuration.fingerprint;
    Plugins::PluginCatalog catalog; catalog.Discover(project / "Plugins"); Plugins::AddLockedProjectPackages(catalog,project,configuration,m_leases);
    auto requests = configuration.dependencies;
    requests.erase(std::remove_if(requests.begin(),requests.end(),[&](const auto& r) { const auto found = configuration.enabled.find(r.id); return found == configuration.enabled.end() || !found->second; }),requests.end());
    for (const auto& [id,enabled] : configuration.enabled)
        if (enabled && std::none_of(requests.begin(),requests.end(),[&](const auto& r) { return r.id == id; })) requests.push_back({id,"*",{}});
    const auto resolved = Plugins::ResolvePlugins(requests,catalog.Candidates(),platform,configuration.locked);
    if (!resolved.succeeded) throw std::runtime_error(resolved.error);
    if (resolved.ordered.empty()) return;
    m_cookRoot = project / "Library/PluginCook" / NLS::Guid::New().ToString();
    std::string fingerprint = m_configuration;
    try
    {
        for (const auto& plugin : resolved.ordered)
        {
            const auto contentHash = HashContentDirectory(plugin.root,100000,UINT64_C(4)*1024*1024*1024,true);
            m_inputs.emplace_back(plugin.root,contentHash); fingerprint += plugin.id + ":" + contentHash + "\n";
            for (const auto& mount : plugin.content)
            {
                if (mount.editorOnly) continue;
                const auto physical = Plugins::ResolvePluginPath(plugin.root,mount.path);
                std::set<std::string> contentNames;
                for (const auto& entry : std::filesystem::recursive_directory_iterator(physical))
                {
                    const auto utf8=entry.path().lexically_relative(physical).generic_u8string();
                    contentNames.insert(NormalizePluginAssetPath({reinterpret_cast<const char*>(utf8.data()),utf8.size()}));
                }
                for (const auto& keep : mount.runtimeKeep)
                    if (!contentNames.count(keep)) throw std::runtime_error("plugin runtime keep path is missing: " + keep);
                const auto logical = std::filesystem::path("Assets/Plugins") / plugin.id / mount.name;
                std::set<std::string> aliases;
                for (const auto& entry : std::filesystem::recursive_directory_iterator(physical))
                {
                    if (entry.is_symlink()) throw std::runtime_error("plugin Cook refuses symbolic links");
                    const auto pathUtf8 = entry.path().lexically_relative(physical).generic_u8string();
                    const auto relative = NormalizePluginAssetPath({reinterpret_cast<const char*>(pathUtf8.data()),pathUtf8.size()});
                    if (!aliases.insert(PluginAssetCollisionKey(relative)).second) throw std::runtime_error("plugin Cook path alias conflict");
                    if (!entry.is_regular_file() || entry.path().extension() == ".meta") continue;
                    const auto target = m_cookRoot / logical / std::filesystem::u8path(relative); std::filesystem::create_directories(target.parent_path());
                    std::filesystem::copy_file(entry.path(),target);
                    const auto loaded = AssetMeta::Load(GetAssetMetaPath(entry.path()));
                    if (!loaded && std::filesystem::is_regular_file(GetAssetMetaPath(entry.path()))) throw std::runtime_error("plugin resource metadata is invalid");
                    auto meta = loaded.value_or(AssetMeta::CreateForAsset(entry.path()));
                    if (!std::filesystem::is_regular_file(GetAssetMetaPath(entry.path()))) meta.id = AssetId(PluginAssetGuid(plugin.id,mount.name,relative));
                    // The ordinary Cook metadata hash now includes plugin code
                    // and import configuration inputs, independently of GUID.
                    meta.settings["nullus.plugin.inputs"] = contentHash;
                    if (!meta.Save(GetAssetMetaPath(target))) throw std::runtime_error("cannot write virtual plugin Cook metadata");
                    const bool retained = std::any_of(mount.runtimeKeep.begin(),mount.runtimeKeep.end(),[&](const auto& keep) {
                        return relative == keep || relative.rfind(keep + "/",0) == 0;
                    });
                    if (retained)
                    {
                        if (meta.assetType != AssetType::Unknown) m_keepGuids.push_back(meta.id.ToString());
                        const auto rawPath=(logical / std::filesystem::u8path(relative)).generic_u8string();
                        m_rawFiles.push_back({std::string(reinterpret_cast<const char*>(rawPath.data()),rawPath.size()),target,entry.file_size(),"plugin-runtime-keep:" + plugin.id});
                    }
                }
            }
        }
        m_fingerprint = ToHexString(ComputeSha256(reinterpret_cast<const uint8_t*>(fingerprint.data()),fingerprint.size()));
        if (!IsCurrent()) throw std::runtime_error("plugin inputs changed while preparing Cook");
    }
    catch (...) { std::error_code ignored; std::filesystem::remove_all(m_cookRoot,ignored); throw; }
}
PluginBuildInputs::~PluginBuildInputs() { if (!m_cookRoot.empty()) { std::error_code ignored; std::filesystem::remove_all(m_cookRoot,ignored); } }
bool PluginBuildInputs::IsCurrent() const
{
    try
    {
        if (Plugins::PluginManager::ReadConfiguration(m_project).fingerprint != m_configuration) return false;
        for (const auto& [root,hash] : m_inputs)
            if (Core::Assets::HashContentDirectory(root,100000,UINT64_C(4)*1024*1024*1024,true) != hash) return false;
        return true;
    }
    catch (...) { return false; }
}
}
