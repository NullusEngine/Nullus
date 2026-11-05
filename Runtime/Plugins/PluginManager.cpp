#include "PluginManager.h"
#include "ScriptModuleAdapter.h"
#include "Core/Assets/ContentHash.h"
#include "SDK/NullusPlugin.h"
#include "RuntimeDeployment.h"
#include "ProjectPluginState.h"
#include "Platform/Process/InterprocessFileLock.h"
#include "Platform/Modules/NativeLibrary.h"
#include <Json/json.hpp>
#include <algorithm>
#include <fstream>
#include <set>

namespace NLS::Plugins
{
using namespace Core::Modules;
namespace
{
nlohmann::json ReadJson(const std::filesystem::path& path)
{
    if (!std::filesystem::exists(path)) return nlohmann::json::object();
    if (std::filesystem::file_size(path)>8*1024*1024) throw std::runtime_error("plugin/project metadata size limit exceeded");
    std::ifstream stream(path); if (!stream) throw std::runtime_error("cannot read " + path.generic_string());
    return nlohmann::json::parse(stream);
}
bool Includes(const std::vector<std::string>& values, const std::string& value)
{ return values.empty() || std::find(values.begin(), values.end(), value) != values.end(); }
}
void PluginCatalog::Add(PluginManifest manifest)
{
    for (auto& existing : m_candidates)
        if (existing.id == manifest.id && existing.source == manifest.source)
        { existing = std::move(manifest); return; }
    m_candidates.push_back(std::move(manifest));
}
void PluginCatalog::Discover(const std::filesystem::path& root)
{
    if (!std::filesystem::is_directory(root)) return;
    std::vector<std::filesystem::path> manifests;
    if (std::filesystem::is_regular_file(root / "plugin.json")) manifests.push_back(root / "plugin.json");
    else for (const auto& entry : std::filesystem::directory_iterator(root))
        if (entry.is_directory() && std::filesystem::is_regular_file(entry.path() / "plugin.json")) manifests.push_back(entry.path() / "plugin.json");
    std::sort(manifests.begin(), manifests.end());
    for (const auto& path : manifests)
        try { Add(PluginManifest::Read(path)); }
        catch (const std::exception& e) { m_diagnostics.push_back(path.generic_string() + ": " + e.what()); }
}
PluginManager::PluginManager(PluginHost& host, HostKind kind) : m_host(host), m_kind(kind) {}
void PluginManager::AddSourceFactory(const std::string& target, std::function<std::unique_ptr<IModule>()> factory,std::string inputHash)
{
    if (!m_sources.emplace(target,SourceFactory{std::move(factory),std::move(inputHash)}).second) throw std::logic_error("duplicate source plugin target: " + target);
}
ProjectPluginConfiguration PluginManager::ReadConfiguration(const std::filesystem::path& project)
{
    Platform::Process::InterprocessFileLock projectLock(project / "Packages/.write.lock");
    RecoverProjectPluginStateLocked(project);
    ProjectPluginConfiguration config;
    config.fingerprint = ProjectPluginStateFingerprintLocked(project);
    const auto manifest = ReadJson(project / "Packages/manifest.json");
    const auto validateSchema=[](const nlohmann::json& value) {
        if (!value.empty() && value.value("schemaVersion",0)!=1) throw std::runtime_error("unsupported project plugin schema version");
    };
    validateSchema(manifest);
    if (manifest.contains("dependencies"))
        for (auto it = manifest["dependencies"].begin(); it != manifest["dependencies"].end(); ++it)
        {
            const auto& value = it.value();
            config.dependencies.push_back({it.key(), value.is_string() ? value.get<std::string>() : value.at("version").get<std::string>(),
                value.is_object() ? value.value("source", "") : ""});
        }
    const auto plugins = ReadJson(project / "Packages/plugins.json");
    validateSchema(plugins);
    if (plugins.contains("enabled")) config.enabled = plugins.at("enabled").get<std::map<std::string, bool>>();
    const auto lock = ReadJson(project / "Packages/packages-lock.json");
    validateSchema(lock);
    if (lock.contains("packages"))
        for (const auto& package : lock["packages"])
        {
            const auto id = package.at("id").get<std::string>(), version = package.at("version").get<std::string>();
            config.locked[id] = version;
            config.packages.push_back({id,version,package.at("source").get<std::string>(),package.at("hash").get<std::string>()});
        }
    return config;
}
void AddLockedProjectPackages(PluginCatalog& catalog, const std::filesystem::path& project,
    const ProjectPluginConfiguration& config, std::vector<std::shared_ptr<Platform::Process::InterprocessFileLock>>& leases)
{
    for (const auto& package : config.packages)
    {
        if (package.hash.size() != 64 || !std::all_of(package.hash.begin(),package.hash.end(),[](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
            throw std::runtime_error("invalid locked package content hash");
        const auto cache = project / "Library/PackageCache";
        leases.push_back(std::make_shared<Platform::Process::InterprocessFileLock>(cache / ".locks" / (package.hash + ".lock"),false));
        const auto root = cache / package.hash / "content";
        if (Core::Assets::HashContentDirectory(root) != package.hash) throw std::runtime_error("locked plugin cache integrity mismatch: " + package.id);
        auto manifest = PluginManifest::Read(root / "plugin.json");
        if (manifest.id != package.id || manifest.version.String() != package.version) throw std::runtime_error("locked plugin identity mismatch");
        manifest.source = package.source; catalog.Add(std::move(manifest));
    }
}
bool PluginManager::OpenProject(const std::filesystem::path& project, const std::filesystem::path& enginePlugins)
{
    try
    {
        if (!m_session.active.empty() && std::filesystem::weakly_canonical(project) != m_session.projectRoot)
            throw std::runtime_error("project switch requires comparison and a new application lifecycle; restart required");
        m_session.projectRoot = std::filesystem::weakly_canonical(project);
        m_session.engineRoot = enginePlugins;
        m_catalog.Discover(enginePlugins); m_catalog.Discover(m_session.projectRoot / "Plugins");
        auto config = ReadConfiguration(m_session.projectRoot);
        if (!enginePlugins.empty())
        {
            const auto defaults = ReadJson(enginePlugins / "defaults.json");
            if (defaults.contains("enabled"))
                for (auto it = defaults["enabled"].begin(); it != defaults["enabled"].end(); ++it)
                    if (!config.enabled.count(it.key())) config.enabled[it.key()] = it.value().get<bool>();
        }
        AddLockedProjectPackages(m_catalog,m_session.projectRoot,config,m_session.cacheLeases);
        if (ReadConfiguration(m_session.projectRoot).fingerprint != config.fingerprint) throw std::runtime_error("plugin configuration changed while opening project; retry");
        auto requests = config.dependencies;
        requests.erase(std::remove_if(requests.begin(), requests.end(), [&](const auto& r) {
            auto it = config.enabled.find(r.id); return it == config.enabled.end() || !it->second;
        }), requests.end());
        for (const auto& [id, enabled] : config.enabled)
            if (enabled && std::none_of(requests.begin(), requests.end(), [&](const auto& r) { return r.id == id; })) requests.push_back({id, "*", {}});
        auto resolved = ResolvePlugins(requests, m_catalog.Candidates(), HostPlatform(), config.locked);
        if (!resolved.succeeded) throw std::runtime_error(resolved.error);
        for (const auto& selected : resolved.ordered)
            if (auto it = config.enabled.find(selected.id); it != config.enabled.end() && !it->second)
                throw std::runtime_error("enabled plugin requires explicitly disabled dependency: " + selected.id);
        return LoadSelection(requests, config.locked);
    }
    catch (const std::exception& e) { m_error = e.what(); return false; }
}
bool PluginManager::LoadSelection(const std::vector<ResolveRequest>& requests, const std::map<std::string, std::string>& locked)
{
    auto resolved = ResolvePlugins(requests, m_catalog.Candidates(), HostPlatform(), locked);
    if (!resolved.succeeded) { m_error = resolved.error; return false; }
    if (!m_session.active.empty())
    {
        if (CompareProject(resolved.ordered) != ProjectSwitchResult::Reuse) { m_error = "plugin selection changed; restart required"; return false; }
        return true;
    }
    try
    {
        // Validate the entire target selection before loading any binary code.
        const auto contentLease=std::make_shared<decltype(m_session.cacheLeases)>(m_session.cacheLeases);
        std::map<std::string,std::string> privateLibraries;
        for (const auto& plugin : resolved.ordered)
        {
            if (m_kind == HostKind::Game && !plugin.modules.empty() &&
                std::none_of(plugin.modules.begin(),plugin.modules.end(),[](const auto& m) {
                    return (m.descriptor.hosts & static_cast<uint32_t>(HostKind::Game)) &&
                        (m.descriptor.type == ModuleType::Runtime || m.descriptor.type == ModuleType::ThirdParty);
                }) && std::none_of(plugin.content.begin(),plugin.content.end(),[](const auto& c) { return !c.editorOnly; })) continue;
            if (!Includes(plugin.architectures, HostArchitecture()) || !Satisfies(SemVersion::Parse(NLS_ENGINE_VERSION_STRING), plugin.engineVersion))
                throw std::runtime_error("incompatible engine or architecture: " + plugin.id);
            if (m_kind == HostKind::Game && !plugin.canPackage) throw std::runtime_error("plugin cannot run in Player: " + plugin.id);
            for (const auto& module : plugin.modules)
            {
                if (!(module.descriptor.hosts & static_cast<uint32_t>(m_kind))) continue;
                if (m_kind == HostKind::Game && (module.descriptor.type == ModuleType::Editor || module.descriptor.type == ModuleType::Developer)) continue;
                if (!module.sourceTarget.empty())
                {
                    const auto found=m_sources.find(module.sourceTarget);
                    if (found==m_sources.end()) throw std::runtime_error("source plugin was not compiled for this host: " + module.sourceTarget);
                    if (!found->second.inputHash.empty() && found->second.inputHash != (m_session.player ? module.sourceBuildHash : HashPluginBuildInputs(plugin.root)))
                        throw std::runtime_error("source plugin host build is stale: " + module.sourceTarget);
                }
                if (!module.binary.empty())
                {
                    auto binary=module.binary;
                    if (std::filesystem::path(binary).extension().empty()) binary += HostPlatform()=="Windows" ? ".dll" : HostPlatform()=="macOS" ? ".dylib" : ".so";
                    Platform::NativeLibrary::ValidateTarget(ResolvePluginPath(plugin.root,binary),HostPlatform(),HostArchitecture());
                }
                if (module.scriptBackend.empty()) for (const auto& dependency : module.runtimeDependencies)
                {
                    auto name=std::filesystem::u8path(dependency).filename().string();
                    std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c) { return c>='A'&&c<='Z' ? c+'a'-'A' : c; });
                    const auto hash=HashPluginFile(ResolvePluginPath(plugin.root,dependency));
                    const auto [entry,inserted]=privateLibraries.emplace(name,hash);
                    if (!inserted && entry->second!=hash) throw std::runtime_error("conflicting native dependency versions in one process: " + name);
                }
            }
            for (const auto& mount : plugin.content)
            {
                if (m_kind == HostKind::Game && mount.editorOnly) continue;
                const auto physical = ResolvePluginPath(plugin.root, mount.path);
                if (!std::filesystem::is_directory(physical)) throw std::runtime_error("plugin content directory missing: " + physical.generic_string());
                std::set<std::string> aliases;
                for (const auto& file : std::filesystem::recursive_directory_iterator(physical))
                {
                    if (file.is_symlink()) throw std::runtime_error("plugin content symbolic link is not supported");
                    const auto rel = file.path().lexically_relative(physical).generic_u8string();
                    const auto key = Core::Assets::PluginAssetCollisionKey(std::string(reinterpret_cast<const char*>(rel.data()), rel.size()));
                    if (!aliases.insert(key).second) throw std::runtime_error("plugin content path normalization/case conflict: " + key);
                }
                m_session.contentRoots.roots.push_back({physical, m_session.projectRoot / "Assets/Plugins" / plugin.id / mount.name,
                    m_session.projectRoot / "Library/Plugins" / plugin.id / mount.name,
                    "nullus.plugin.asset.v1:" + plugin.id + ":" + mount.name, mount.editorOnly});
                m_session.contentRoots.roots.back().contentLease=contentLease;
                auto& inputs = m_session.contentRoots.roots.back().importInputs;
                inputs.push_back(plugin.root / "plugin.json");
                for (const auto& module : plugin.modules)
                    if (!module.binary.empty())
                    {
                        auto binary = module.binary;
                        if (std::filesystem::path(binary).extension().empty()) binary += HostPlatform() == "Windows" ? ".dll" : HostPlatform() == "macOS" ? ".dylib" : ".so";
                        const auto path = ResolvePluginPath(plugin.root,binary);
                        if (std::filesystem::is_regular_file(path)) inputs.push_back(path);
                    }
                if (std::filesystem::is_regular_file(plugin.root / "Binaries/build-info.json")) inputs.push_back(plugin.root / "Binaries/build-info.json");
            }
        }
        for (const auto& plugin : resolved.ordered)
            for (const auto& module : plugin.modules)
            {
                auto descriptor = module.descriptor;
                if (!(descriptor.hosts & static_cast<uint32_t>(m_kind))) continue;
                if (m_kind == HostKind::Game && (descriptor.type == ModuleType::Editor || descriptor.type == ModuleType::Developer)) continue;
                for (const auto& dep : plugin.dependencies)
                {
                    if (!Includes(dep.platforms, HostPlatform())) continue;
                    const auto found = std::find_if(resolved.ordered.begin(), resolved.ordered.end(), [&](const auto& p) { return p.id == dep.id; });
                    if (found == resolved.ordered.end()) continue;
                    if (!dep.optional && descriptor.type == ModuleType::Runtime && !found->modules.empty() &&
                        std::none_of(found->modules.begin(),found->modules.end(),[](const auto& m) {
                            return m.descriptor.type == ModuleType::Runtime || m.descriptor.type == ModuleType::ThirdParty;
                        })) throw std::runtime_error("Runtime plugin dependency only provides Editor/Developer modules: " + plugin.id + " -> " + dep.id);
                    for (const auto& dm : found->modules)
                        if ((dm.descriptor.hosts & static_cast<uint32_t>(m_kind)) &&
                            dm.descriptor.type != ModuleType::Editor && dm.descriptor.type != ModuleType::Developer)
                            descriptor.dependencies.push_back(dm.descriptor.id);
                }
                if (!module.binary.empty())
                {
                    auto relative = module.binary;
#ifdef _WIN32
                    if (std::filesystem::path(relative).extension().empty()) relative += ".dll";
#elif defined(__APPLE__)
                    if (std::filesystem::path(relative).extension().empty()) relative += ".dylib";
#else
                    if (std::filesystem::path(relative).extension().empty()) relative += ".so";
#endif
                    if (!m_host.RegisterBinary(descriptor, ResolvePluginPath(plugin.root, relative))) throw std::runtime_error(m_host.LastError());
                }
                else if (!module.scriptBackend.empty())
                {
                    const auto path = ResolvePluginPath(plugin.root,module.script);
                    if (!m_host.Modules().Register(descriptor,[backend=module.scriptBackend,path,entry=module.entry] {
                        return std::make_unique<ScriptModuleAdapter>(backend,path,entry);
                    })) throw std::runtime_error(m_host.Modules().LastError());
                }
                else
                {
                    auto factory = m_sources.find(module.sourceTarget);
                    if (factory == m_sources.end()) throw std::runtime_error("source plugin was not compiled for this host: " + module.sourceTarget);
                    const auto expected = m_session.player ? module.sourceBuildHash : HashPluginBuildInputs(plugin.root);
                    if (!factory->second.inputHash.empty() && expected != factory->second.inputHash)
                        throw std::runtime_error("source plugin host build is stale: " + module.sourceTarget);
                    if (!m_host.Modules().Register(descriptor, factory->second.create)) throw std::runtime_error(m_host.Modules().LastError());
                }
            }
        if (!m_host.Modules().Validate()) throw std::runtime_error(m_host.Modules().LastError());
        for (const auto& plugin : resolved.ordered)
            m_session.fingerprints[plugin.id] = Core::Assets::HashContentDirectory(plugin.root,100000,UINT64_C(4)*1024*1024*1024,true);
        m_session.active = std::move(resolved.ordered); m_session.closed = false; return Advance(LoadingPhase::PreEngine);
    }
    catch (const std::exception& e) { m_error = e.what(); return false; }
}
bool PluginManager::OpenPlayer(const std::filesystem::path& installRoot)
{
    try
    {
        m_session.projectRoot = std::filesystem::weakly_canonical(installRoot);
        m_session.player = true;
        const auto descriptor = ReadJson(m_session.projectRoot / "Player.info");
        if (!descriptor.empty() && std::filesystem::is_regular_file(m_session.projectRoot / "Plugins/runtime-plugins.json"))
            if (descriptor.value("pluginManifestPath",std::string{}) != "Plugins/runtime-plugins.json" ||
                descriptor.value("pluginManifestHash",std::string{}) != HashPluginFile(m_session.projectRoot / "Plugins/runtime-plugins.json"))
                throw std::runtime_error("Player descriptor/plugin manifest integrity mismatch");
        const auto runtime = ReadJson(m_session.projectRoot / "Plugins/runtime-plugins.json");
        std::vector<ResolveRequest> requests;
        if (!runtime.empty())
        {
            if (runtime.at("schemaVersion").get<uint32_t>() != 1) throw std::runtime_error("unsupported runtime plugin manifest");
            if (runtime.at("platform").get<std::string>() != HostPlatform() || runtime.at("architecture").get<std::string>() != HostArchitecture())
                throw std::runtime_error("runtime plugin deployment target mismatch");
            for (auto it = runtime.at("files").begin(); it != runtime.at("files").end(); ++it)
                if (HashPluginFile(ResolvePluginPath(m_session.projectRoot,it.key())) != it.value().get<std::string>())
                    throw std::runtime_error("runtime plugin content integrity mismatch: " + it.key());
            for (const auto& plugin : runtime.at("plugins"))
            {
                const auto manifestPath = ResolvePluginPath(m_session.projectRoot, plugin.at("manifest").get<std::string>());
                auto manifest = PluginManifest::Read(manifestPath);
                if (manifest.id != plugin.at("id").get<std::string>() || manifest.version.String() != plugin.at("version").get<std::string>())
                    throw std::runtime_error("runtime plugin identity mismatch");
                requests.push_back({manifest.id, manifest.version.String(), manifest.source});
                m_catalog.Add(std::move(manifest));
            }
        }
        return LoadSelection(requests);
    }
    catch (const std::exception& e) { m_error = e.what(); return false; }
}
bool PluginManager::Advance(LoadingPhase phase)
{
    if (m_session.closed) return false;
    if (!m_host.Modules().StartThrough(phase)) { m_error = m_host.Modules().LastError(); return false; }
    return true;
}
void PluginManager::DispatchRuntime(uint32_t event, double deltaSeconds)
{
    if (m_session.closed) return;
    if (!m_session.prePlay) { if (!Advance(LoadingPhase::PrePlay)) return; m_session.prePlay = true; }
    for (auto& lease : m_host.Extensions().FindKind("runtime.play"))
    { ExtensionCall call{event, deltaSeconds}; lease.Invoke(call); }
}
void PluginManager::Pump() { m_host.Modules().Pump(); }
void PluginManager::Close()
{
    if (m_session.closed) return;
    m_session.closed = true; ++m_session.generation; m_host.Modules().ShutdownAll();
}
ProjectSwitchResult PluginManager::CompareProject(const std::vector<PluginManifest>& selection) const
{
    if (selection.size() != m_session.active.size()) return ProjectSwitchResult::RestartRequired;
    for (const auto& current : m_session.active)
    {
        auto next = std::find_if(selection.begin(), selection.end(), [&](const auto& p) { return p.id == current.id; });
        if (next == selection.end() || next->version != current.version || next->source != current.source || next->raw != current.raw)
            return ProjectSwitchResult::RestartRequired;
        try
        {
            if (m_session.fingerprints.at(current.id) != Core::Assets::HashContentDirectory(next->root,100000,UINT64_C(4)*1024*1024*1024,true))
                return ProjectSwitchResult::RestartRequired;
        }
        catch (...) { return ProjectSwitchResult::RestartRequired; }
    }
    return ProjectSwitchResult::Reuse;
}
}
