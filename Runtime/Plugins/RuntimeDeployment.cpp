#include "RuntimeDeployment.h"
#include "ApplicationPlugins.h"
#include "Core/Assets/ArtifactManifest.h"
#include "Core/Assets/ContentHash.h"
#include "SDK/NullusPlugin.h"
#include "ProjectPluginState.h"
#include "Platform/Process/InterprocessFileLock.h"
#include "Platform/Modules/NativeLibrary.h"
#include <Json/json.hpp>
#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <set>

namespace NLS::Plugins
{
using Json = nlohmann::json;
namespace
{
std::string Read(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary); return file ? std::string((std::istreambuf_iterator<char>(file)), {}) : std::string{};
}
void Write(const std::filesystem::path& path, const std::string& text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc); file.write(text.data(), text.size()); file.close();
    if (!file) throw std::runtime_error("cannot write runtime plugin file: " + path.generic_string());
}
bool Includes(const std::vector<std::string>& list, const std::string& value)
{ return list.empty() || std::find(list.begin(), list.end(), value) != list.end(); }
std::string Utf8(const std::filesystem::path& path)
{ const auto value = path.generic_u8string(); return {reinterpret_cast<const char*>(value.data()),value.size()}; }
}
std::string HashPluginFile(const std::filesystem::path& path)
{
    return Core::Assets::HashContentFile(path);
}
std::string HashPluginBuildInputs(const std::filesystem::path& root)
{
    // An indirect script/tool can change generated code without touching a
    // .cpp or .cmake file. Artifact freshness uses the same complete input set.
    return HashPluginBuildApprovalInputs(root);
}
std::string HashPluginBuildApprovalInputs(const std::filesystem::path& root)
{
    std::map<std::string,std::filesystem::path> files; uint64_t bytes=0;
    constexpr uint64_t maxBytes=UINT64_C(4)*1024*1024*1024;
    for (auto it=std::filesystem::recursive_directory_iterator(root);it!=std::filesystem::recursive_directory_iterator();++it)
    {
        if (it->path().filename()==".git") { if (it->is_directory()) it.disable_recursion_pending(); continue; }
        if (it->is_symlink()) throw std::runtime_error("source build approval refuses symbolic links");
        if (!it->is_regular_file()) { if (!it->is_directory()) throw std::runtime_error("source build approval requires regular files"); continue; }
        const auto size=it->file_size();
        if (files.size()>=100000 || size>maxBytes-bytes) throw std::runtime_error("source build approval input limit exceeded");
        bytes+=size; files.emplace(Utf8(it->path().lexically_relative(root)),it->path());
    }
    std::string input;
    for (const auto& [relative,path] : files) input+=relative+":"+HashPluginFile(path)+"\n";
    return Core::Assets::ToHexString(Core::Assets::ComputeSha256(reinterpret_cast<const uint8_t*>(input.data()),input.size()));
}
bool StageRuntimePlugins(const std::filesystem::path& project, const std::filesystem::path& installRoot,
    const std::string& platform, const std::string& architecture, std::string* error,const std::string& configuration)
{
    try
    {
        const auto runtimePath = installRoot / "Plugins/runtime-plugins.json";
        if (std::filesystem::exists(runtimePath)) throw std::runtime_error("runtime plugin deployment requires fresh staging");
        const auto config = PluginManager::ReadConfiguration(project);
        const auto configBefore = config.fingerprint;
        PluginCatalog catalog; catalog.Discover(project / "Plugins");
        std::vector<std::shared_ptr<Platform::Process::InterprocessFileLock>> cacheLeases;
        AddLockedProjectPackages(catalog,project,config,cacheLeases);
        auto requests = config.dependencies;
        requests.erase(std::remove_if(requests.begin(), requests.end(), [&](const auto& r) {
            const auto it = config.enabled.find(r.id); return it == config.enabled.end() || !it->second;
        }), requests.end());
        for (const auto& [id, enabled] : config.enabled)
            if (enabled && std::none_of(requests.begin(), requests.end(), [&](const auto& r) { return r.id == id; })) requests.push_back({id,"*",{}});
        auto resolved = ResolvePlugins(requests, catalog.Candidates(), platform, config.locked);
        if (!resolved.succeeded) throw std::runtime_error(resolved.error);
        std::map<std::filesystem::path,std::string> sourceFingerprints;
        for (const auto& plugin : resolved.ordered) sourceFingerprints.emplace(plugin.root,
            Core::Assets::HashContentDirectory(plugin.root,100000,UINT64_C(4)*1024*1024*1024,true));
        Json runtime{{"schemaVersion",1},{"platform",platform},{"architecture",architecture},{"engineVersion",NLS_ENGINE_VERSION_STRING},
            {"plugins",Json::array()},{"files",Json::object()}};
        std::set<std::string> runtimeIds;
        for (const auto& plugin : resolved.ordered)
        {
            if (std::any_of(plugin.content.begin(),plugin.content.end(),[](const auto& c) { return !c.editorOnly; })) runtimeIds.insert(plugin.id);
            for (const auto& module : plugin.modules)
                if ((module.descriptor.type == Core::Modules::ModuleType::Runtime || module.descriptor.type == Core::Modules::ModuleType::ThirdParty) &&
                    (module.descriptor.hosts & static_cast<uint32_t>(Core::Modules::HostKind::Game))) runtimeIds.insert(plugin.id);
        }
        auto copy = [&](const std::filesystem::path& source, const std::filesystem::path& relative) {
            const auto before = HashPluginFile(source); const auto destination = installRoot / relative;
            if (std::filesystem::is_symlink(source)) throw std::runtime_error("plugin deployment refuses symbolic links");
            std::filesystem::create_directories(destination.parent_path());
            std::filesystem::copy_file(source,destination,std::filesystem::copy_options::overwrite_existing);
            if (HashPluginFile(destination) != before || HashPluginFile(source) != before) throw std::runtime_error("plugin changed while freezing deployment");
            runtime["files"][Utf8(relative)] = before;
        };
        const auto inspectDependencies = [&](const std::filesystem::path& binary,const std::set<std::string>& declared) {
            if (platform != "Windows") return;
            for (const auto& dependency : Platform::NativeLibrary::WindowsImports(binary))
            {
                const bool debugRuntime = dependency == "ucrtbased.dll" ||
                    ((dependency.rfind("vcruntime",0)==0 || dependency.rfind("msvcp",0)==0 || dependency.rfind("msvcr",0)==0) && dependency.ends_with("d.dll"));
                if (configuration == "Release" && debugRuntime) throw std::runtime_error("Shipping Binary Plugin imports a debug C/C++ runtime: " + dependency);
                if (dependency.rfind("nls_",0)==0) throw std::runtime_error("Binary Plugin imports an engine implementation library: " + dependency);
                if (!declared.count(dependency) && !Platform::NativeLibrary::IsWindowsSystemLibrary(dependency))
                    throw std::runtime_error("undeclared/missing Binary Plugin dependency: " + dependency);
            }
        };
        for (const auto& plugin : resolved.ordered)
        {
            if (!runtimeIds.count(plugin.id)) continue;
            if (!plugin.canPackage || !Includes(plugin.architectures,architecture) || !Satisfies(SemVersion::Parse(NLS_ENGINE_VERSION_STRING),plugin.engineVersion))
                throw std::runtime_error("plugin cannot be packaged for target: " + plugin.id);
            const auto enabled = config.enabled.find(plugin.id);
            if (enabled != config.enabled.end() && !enabled->second) throw std::runtime_error("runtime dependency is disabled: " + plugin.id);
            auto portable = Json::parse(plugin.raw); portable["modules"] = Json::array(); portable["dependencies"] = Json::array(); portable["content"] = Json::array();
            portable.erase("cmake"); portable["platforms"] = Json::array({platform}); portable["architectures"] = Json::array({architecture});
            const auto prefix = std::filesystem::path("Plugins") / plugin.id;
            const auto original = Json::parse(plugin.raw);
            for (size_t i = 0; i < plugin.modules.size(); ++i)
            {
                const auto& module = plugin.modules[i];
                if (module.descriptor.type != Core::Modules::ModuleType::Runtime && module.descriptor.type != Core::Modules::ModuleType::ThirdParty) continue;
                if (!(module.descriptor.hosts & static_cast<uint32_t>(Core::Modules::HostKind::Game))) continue;
                std::set<std::string> declaredDependencies;
                for (const auto& dependency : module.runtimeDependencies)
                {
                    auto name=std::filesystem::u8path(dependency).filename().string();
                    std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c) { return c>='A'&&c<='Z' ? c+'a'-'A' : c; });
                    declaredDependencies.insert(std::move(name));
                }
                for (const auto& dependency : module.runtimeDependencies) copy(ResolvePluginPath(plugin.root,dependency),prefix / dependency);
                if (!module.scriptBackend.empty())
                {
                    copy(ResolvePluginPath(plugin.root,module.script),prefix / module.script);
                    portable["modules"].push_back(original["modules"][i]); continue;
                }
                if (module.binary.empty())
                {
                    const auto recordPath = installRoot / "Game.source-plugins.json";
                    if (!std::filesystem::is_regular_file(recordPath)) throw std::runtime_error("runtime Source Plugin needs a target-toolchain Game build record: " + module.descriptor.id);
                    const auto record = Json::parse(Read(recordPath));
                    const auto& built = record.at("plugins").at(plugin.id);
                    if (record.at("platform") != platform || record.at("architecture") != architecture ||
                        (!configuration.empty() && record.at("configuration") != configuration) ||
                        record.at("engineVersion") != NLS_ENGINE_VERSION_STRING || built.at("version") != plugin.version.String() ||
                        built.at("inputHash") != HashPluginBuildInputs(plugin.root))
                        throw std::runtime_error("stale/incompatible Source Plugin Game build: " + plugin.id);
                    const auto targets = built.at("targets").get<std::vector<std::string>>();
                    if (std::find(targets.begin(),targets.end(),module.sourceTarget) == targets.end()) throw std::runtime_error("Source Plugin target is absent from Game build");
                    bool binaryMatched = false;
                    for (const auto& file : std::filesystem::directory_iterator(installRoot))
                        if (file.is_regular_file() && (file.path().extension() == ".exe" || file.path().filename() == "Game") &&
                            HashPluginFile(file.path()) == record.at("hostBinaryHash").get<std::string>()) binaryMatched = true;
                    if (!binaryMatched) throw std::runtime_error("Source Plugin record does not match staged Game executable");
                    runtime["files"]["Game.source-plugins.json"] = HashPluginFile(recordPath);
                    auto manifestModule = original["modules"][i]; manifestModule["sourceBuildHash"] = built.at("inputHash");
                    portable["modules"].push_back(std::move(manifestModule)); continue;
                }
                auto relative = module.binary;
                if (std::filesystem::path(relative).extension().empty()) relative += platform == "Windows" ? ".dll" : platform == "macOS" ? ".dylib" : ".so";
                Platform::NativeLibrary::ValidateTarget(ResolvePluginPath(plugin.root,relative),platform,architecture);
                inspectDependencies(ResolvePluginPath(plugin.root,relative),declaredDependencies);
                for (const auto& dependency : module.runtimeDependencies)
                    if (std::filesystem::path(dependency).extension() == ".dll")
                    { Platform::NativeLibrary::ValidateTarget(ResolvePluginPath(plugin.root,dependency),platform,architecture); inspectDependencies(ResolvePluginPath(plugin.root,dependency),declaredDependencies); }
                copy(ResolvePluginPath(plugin.root,relative),prefix / std::filesystem::u8path(relative));
                auto manifestModule = original["modules"][i]; manifestModule["binary"] = relative;
                portable["modules"].push_back(std::move(manifestModule));
            }
            for (const auto& dep : plugin.dependencies)
            {
                if (!Includes(dep.platforms,platform)) continue;
                if (runtimeIds.count(dep.id)) portable["dependencies"].push_back({{"id",dep.id},{"version",dep.version},{"optional",dep.optional}});
                else if (!dep.optional) throw std::runtime_error("Runtime plugin depends on Editor-only plugin: " + plugin.id + " -> " + dep.id);
            }
            for (size_t i = 0; i < plugin.content.size(); ++i)
            {
                const auto& mount = plugin.content[i]; if (mount.editorOnly) continue;
                const auto content = ResolvePluginPath(plugin.root,mount.path);
                const auto marker = prefix / mount.path / ".nullus-mount";
                Write(installRoot / marker,plugin.id + ":" + mount.name);
                runtime["files"][Utf8(marker)] = HashPluginFile(installRoot / marker);
                std::set<std::string> contentNames;
                for (const auto& entry : std::filesystem::recursive_directory_iterator(content))
                    contentNames.insert(Core::Assets::NormalizePluginAssetPath(Utf8(entry.path().lexically_relative(content))));
                for (const auto& keep : mount.runtimeKeep)
                    if (!contentNames.count(keep)) throw std::runtime_error("runtime keep path is missing: " + keep);
                for (const auto& entry : std::filesystem::recursive_directory_iterator(content))
                    if (entry.is_regular_file())
                    {
                        const auto relative = Core::Assets::NormalizePluginAssetPath(Utf8(entry.path().lexically_relative(content)));
                        if (std::any_of(mount.runtimeKeep.begin(),mount.runtimeKeep.end(),[&](const auto& keep) { return relative == keep || relative.rfind(keep + "/",0) == 0; }))
                            copy(entry.path(),prefix / mount.path / std::filesystem::u8path(relative));
                    }
                portable["content"].push_back(original["content"][i]);
            }
            const auto relativeManifest = prefix / "plugin.json";
            Write(installRoot / relativeManifest,portable.dump(2));
            runtime["files"][relativeManifest.generic_string()] = HashPluginFile(installRoot / relativeManifest);
            runtime["plugins"].push_back({{"id",plugin.id},{"version",plugin.version.String()},{"manifest",relativeManifest.generic_string()}});
        }
        {
            Platform::Process::InterprocessFileLock lock(project / "Packages/.write.lock");
            RecoverProjectPluginStateLocked(project);
            if (configBefore != ProjectPluginStateFingerprintLocked(project)) throw std::runtime_error("project plugin inputs changed during deployment freeze");
        }
        for (const auto& [root,fingerprint] : sourceFingerprints)
            if (Core::Assets::HashContentDirectory(root,100000,UINT64_C(4)*1024*1024*1024,true) != fingerprint)
                throw std::runtime_error("plugin source/content changed during deployment freeze");
        Write(runtimePath,runtime.dump(2)); return true;
    }
    catch (const std::exception& e) { if (error) *error = e.what(); return false; }
}
int RunRuntimePluginSelfTest(const std::filesystem::path& root,std::function<void(PluginManager&)> registerSources)
{
    try
    {
        ApplicationPlugins plugins(Core::Modules::HostKind::Game,root,{},true,std::move(registerSources));
        auto& manager = plugins.Manager();
        Core::Modules::AdvanceHostPhase(Core::Modules::LoadingPhase::PostEngine);
        manager.DispatchRuntime(NLS_PLUGIN_EVENT_BEGIN);
        for (int i = 0; i < 3; ++i) manager.DispatchRuntime(NLS_PLUGIN_EVENT_TICK,1.0/60.0);
        auto extensions = plugins.Host().Extensions().FindKind("runtime.play");
        if (extensions.empty()) throw std::runtime_error("no runtime extensions were loaded");
        auto sample = plugins.Host().Extensions().Find("org.nullus.sample.runtime.counter");
        if (sample)
        {
            Core::Modules::ExtensionCall query{0};
            if (!sample.Invoke(query) || query.value != 3) throw std::runtime_error("runtime sample did not receive ticks");
            query = {97}; if (!sample.Invoke(query) || !query.value) throw std::runtime_error("runtime sample content could not be read");
        }
        manager.DispatchRuntime(NLS_PLUGIN_EVENT_END);
        std::printf("[plugin-self-test] OK: %zu runtime extension(s), 3 ticks, deployed content.\n",extensions.size());
        return 0;
    }
    catch (const std::exception& e) { std::fprintf(stderr,"[plugin-self-test] %s\n",e.what()); return 1; }
}
}
