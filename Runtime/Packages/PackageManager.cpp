#include "PackageManager.h"
#include "Core/Assets/PluginAssetPath.h"
#include "Core/Assets/ArtifactManifest.h"
#include "Core/Assets/ContentHash.h"
#include "Plugins/PluginManager.h"
#include "Plugins/RuntimeDeployment.h"
#include "Plugins/ProjectPluginState.h"
#include "Platform/Process/InterprocessFileLock.h"
#include "Guid.h"
#include <Json/json.hpp>
#include <algorithm>
#include <array>
#include <fstream>
#include <set>
#include <thread>

namespace NLS::Packages
{
using Json = nlohmann::json;
using Platform::Process::InterprocessFileLock;
using Platform::Process::WriteDurableFile;
using Platform::Process::ReplaceFileAtomically;
namespace
{
const std::set<std::string> kProjectFiles{"manifest.json","packages-lock.json","plugins.json","resolved-plugins.json","build-trust.json"};
std::string Read(const std::filesystem::path& path)
{
    if (std::filesystem::file_size(path)>8*1024*1024) throw std::runtime_error("package metadata size limit exceeded");
    std::ifstream file(path,std::ios::binary); if (!file) throw std::runtime_error("cannot read package file: " + path.generic_string());
    return std::string((std::istreambuf_iterator<char>(file)),{});
}
Json ReadJson(const std::filesystem::path& path, Json fallback = Json::object())
{ return std::filesystem::exists(path) ? Json::parse(Read(path)) : fallback; }
std::string Utf8(const std::filesystem::path& path)
{ const auto text = path.generic_u8string(); return {reinterpret_cast<const char*>(text.data()),text.size()}; }
bool IsHash(const std::string& hash)
{ return hash.size() == 64 && std::all_of(hash.begin(),hash.end(),[](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }); }
std::vector<std::pair<std::string,std::filesystem::path>> Files(const std::filesystem::path& root, const PackageLimits& limits)
{
    std::vector<std::pair<std::string,std::filesystem::path>> files; std::set<std::string> aliases;
    uint64_t expanded = 0; uint32_t entries = 0;
    for (auto it = std::filesystem::recursive_directory_iterator(root); it != std::filesystem::recursive_directory_iterator(); ++it)
    {
        if (it->path().filename() == ".git") { if (it->is_directory()) it.disable_recursion_pending(); continue; }
        if (it->is_symlink()) throw std::runtime_error("package symbolic links are not supported");
        const auto name = Core::Assets::NormalizePluginAssetPath(Utf8(it->path().lexically_relative(root)));
        if (!aliases.insert(Core::Assets::PluginAssetCollisionKey(name)).second) throw std::runtime_error("package path normalization/case collision");
        if (++entries > limits.maxFiles) throw std::runtime_error("package file count limit exceeded");
        if (it->is_directory()) continue;
        if (!it->is_regular_file()) throw std::runtime_error("package contains a non-regular file");
        const auto size = it->file_size();
        if (size > limits.maxExpandedBytes - expanded) throw std::runtime_error("package expanded size limit exceeded");
        expanded += size; files.emplace_back(name,it->path());
    }
    std::sort(files.begin(),files.end(),[](auto& a,auto& b) { return a.first < b.first; }); return files;
}
PackageResult Error(const std::exception& e) { PackageResult result; result.error = e.what(); return result; }
Plugins::ResolveRequest DependencyRequest(const std::string& id,const Json& value)
{
    if (value.is_string()) return {id,value.get<std::string>(),{}};
    return {id,value.at("version").get<std::string>(),value.value("source",std::string{})};
}
bool CacheMatches(const std::filesystem::path& root, const std::string& hash, const PackageLimits& limits)
{
    try { return HashPackageTree(root / "content",limits) == hash; } catch (...) { return false; }
}
void PublishDirectory(const std::filesystem::path& source,const std::filesystem::path& destination)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    for (;;)
    {
        std::error_code error; std::filesystem::rename(source,destination,error);
        if (!error) return;
#ifdef _WIN32
        // Antivirus/indexer handles can briefly deny directory rename. Keep
        // staging intact, retry only this condition and never replace a reader.
        if (error == std::errc::permission_denied && std::chrono::steady_clock::now() < deadline)
        { std::this_thread::sleep_for(std::chrono::milliseconds(20)); continue; }
#endif
        throw std::filesystem::filesystem_error("cannot publish immutable package cache",source,destination,error);
    }
}
}
void LocalDirectorySource::Fetch(const PackageRequest& request, const std::filesystem::path& destination, const PackageLimits& limits)
{
    const auto root = std::filesystem::weakly_canonical(std::filesystem::u8path(request.location));
    if (!std::filesystem::is_directory(root)) throw std::runtime_error("local package directory does not exist");
    const auto before = Core::Assets::HashContentDirectory(root,limits.maxFiles,limits.maxExpandedBytes,true);
    std::filesystem::create_directories(destination);
    for (const auto& [name,source] : Files(root,limits))
    {
        if (request.cancel && request.cancel->load()) throw std::runtime_error("package installation cancelled");
        const auto output = destination / std::filesystem::u8path(name); std::filesystem::create_directories(output.parent_path());
        std::filesystem::copy_file(source,output,std::filesystem::copy_options::overwrite_existing);
    }
    for (auto it = std::filesystem::recursive_directory_iterator(root); it != std::filesystem::recursive_directory_iterator(); ++it)
    {
        if (it->path().filename() == ".git") { if (it->is_directory()) it.disable_recursion_pending(); continue; }
        if (it->is_directory()) std::filesystem::create_directories(destination / std::filesystem::u8path(Core::Assets::NormalizePluginAssetPath(Utf8(it->path().lexically_relative(root)))));
    }
    if (before != Core::Assets::HashContentDirectory(root,limits.maxFiles,limits.maxExpandedBytes,true) ||
        before != Core::Assets::HashContentDirectory(destination,limits.maxFiles,limits.maxExpandedBytes))
        throw std::runtime_error("local package changed during installation");
}
std::string HashPackageTree(const std::filesystem::path& root, const PackageLimits& limits)
{
    return Core::Assets::HashContentDirectory(root,limits.maxFiles,limits.maxExpandedBytes);
}
struct PackageManager::Snapshot
{
    Json manifest, lock, plugins;
};
PackageManager::PackageManager(std::filesystem::path project, PackageLimits limits)
    : m_project(std::filesystem::weakly_canonical(std::filesystem::absolute(std::move(project)))), m_limits(limits)
{
    std::filesystem::create_directories(m_project / "Packages");
    if (!Platform::Process::IsLocalTransactionFilesystem(m_project)) throw std::runtime_error("writable package transactions require a supported local filesystem");
    SetProvider(PackageSourceKind::Directory,std::make_shared<LocalDirectorySource>());
    SetProvider(PackageSourceKind::Zip,std::make_shared<ZipPackageSource>());
}
void PackageManager::SetProvider(PackageSourceKind kind, std::shared_ptr<IPackageSource> provider)
{ if (!provider) throw std::invalid_argument("package source cannot be null"); m_providers[kind] = std::move(provider); }
PackageManager::Snapshot PackageManager::ReadSnapshot() const
{
    const auto directory = m_project / "Packages";
    Snapshot snapshot{ReadJson(directory / "manifest.json",{{"schemaVersion",1},{"dependencies",Json::object()}}),
        ReadJson(directory / "packages-lock.json",{{"schemaVersion",1},{"packages",Json::array()}}),
        ReadJson(directory / "plugins.json",{{"schemaVersion",1},{"enabled",Json::object()}})};
    for (const auto* document : {&snapshot.manifest,&snapshot.lock,&snapshot.plugins})
        if (document->value("schemaVersion",0)!=1) throw std::runtime_error("unsupported project package schema version");
    if (!snapshot.manifest.at("dependencies").is_object() || !snapshot.lock.at("packages").is_array() || !snapshot.plugins.at("enabled").is_object())
        throw std::runtime_error("invalid project package metadata layout");
    return snapshot;
}
void PackageManager::Recover()
{ InterprocessFileLock lock(m_project / "Packages/.write.lock"); RecoverLocked(); }
void PackageManager::RecoverLocked()
{
    Plugins::RecoverProjectPluginStateLocked(m_project);
}
void PackageManager::CommitFiles(const std::map<std::string,std::string>& files)
{
    const auto packages = m_project / "Packages";
    const auto id = NLS::Guid::New().ToString(); const auto transaction = packages / ".transactions" / id;
    Json journal{{"phase","prepared"},{"files",Json::array()}};
    for (const auto& [name,data] : files)
    {
        if (!kProjectFiles.count(name)) throw std::invalid_argument("invalid transaction file");
        const bool existed = std::filesystem::exists(packages / name);
        if (existed) WriteDurableFile(transaction / "old" / name,Read(packages / name));
        WriteDurableFile(transaction / "next" / name,data);
        journal["files"].push_back({{"name",name},{"existed",existed}});
    }
    WriteDurableFile(transaction / "state.json",journal.dump());
    const auto temporaryPointer = packages / (".transaction-" + id);
    WriteDurableFile(temporaryPointer,Json{{"id",id}}.dump()); ReplaceFileAtomically(temporaryPointer,packages / ".transaction.json");
    if (m_observer) m_observer("prepared");
    for (const auto& [name,data] : files)
    {
        const auto temporary = packages / (".commit-" + id + "-" + name);
        WriteDurableFile(temporary,data); ReplaceFileAtomically(temporary,packages / name);
        if (m_observer) m_observer("replaced:" + name);
    }
    journal["phase"] = "committed"; WriteDurableFile(transaction / "state.next",journal.dump());
    ReplaceFileAtomically(transaction / "state.next",transaction / "state.json");
    if (m_observer) m_observer("committed");
    std::filesystem::remove(packages / ".transaction.json"); std::filesystem::remove_all(transaction);
}
std::vector<Plugins::PluginManifest> PackageManager::Available(const Snapshot& snapshot) const
{
    Plugins::PluginCatalog catalog; catalog.Discover(m_project / "Plugins");
    for (const auto& root : m_searchRoots) catalog.Discover(root);
    for (auto it = snapshot.manifest.at("dependencies").begin(); it != snapshot.manifest.at("dependencies").end(); ++it)
    {
        const auto& value = it.value();
        if (!value.is_object() || !value.contains("source")) continue;
        if (std::any_of(snapshot.lock.at("packages").begin(),snapshot.lock.at("packages").end(),[&](const auto& p) {
            return p.at("id") == it.key() && p.at("source") == value.at("source");
        })) continue; // Installed sources are consumed from their locked cache.
        std::error_code ignored; const auto source = std::filesystem::u8path(value.at("source").get<std::string>());
        if (std::filesystem::is_directory(source,ignored)) catalog.Discover(source);
    }
    for (const auto& package : snapshot.lock.at("packages"))
    {
        const auto hash = package.at("hash").get<std::string>();
        if (!IsHash(hash)) throw std::runtime_error("invalid package cache key");
        auto candidate = Plugins::PluginManifest::Read(CacheRoot() / hash / "content/plugin.json");
        candidate.source = package.at("source").get<std::string>(); catalog.Add(std::move(candidate));
    }
    return catalog.Candidates();
}
PackageResult PackageManager::CommitResolved(Snapshot snapshot)
{
    const auto candidates = Available(snapshot);
    std::vector<Plugins::ResolveRequest> requests;
    for (auto it = snapshot.manifest.at("dependencies").begin(); it != snapshot.manifest.at("dependencies").end(); ++it)
    {
        requests.push_back(DependencyRequest(it.key(),it.value()));
    }
    std::map<std::string,std::string> locked;
    for (auto it = snapshot.plugins.at("enabled").begin(); it != snapshot.plugins.at("enabled").end(); ++it)
        if (it.value().get<bool>() && std::none_of(requests.begin(),requests.end(),[&](const auto& r) { return r.id == it.key(); }))
            requests.push_back({it.key(),"*",{}});
    for (const auto& package : snapshot.lock.at("packages")) locked[package.at("id").get<std::string>()] = package.at("version").get<std::string>();
    auto resolved = Plugins::ResolvePlugins(requests,candidates,Plugins::HostPlatform(),locked);
    if (!resolved.succeeded) throw std::runtime_error(resolved.error);
    Json packages = Json::array(), selection = Json::array();
    for (const auto& selected : resolved.ordered)
    {
        const auto found = std::find_if(snapshot.lock["packages"].begin(),snapshot.lock["packages"].end(),[&](const auto& p) {
            return p.at("id").template get<std::string>() == selected.id && p.at("version").template get<std::string>() == selected.version.String() && p.at("source").template get<std::string>() == selected.source;
        });
        if (found != snapshot.lock["packages"].end()) packages.push_back(*found);
        bool enabled = snapshot.plugins.at("enabled").value(selected.id,false);
        selection.push_back({{"id",selected.id},{"version",selected.version.String()},
            {"manifest",Utf8(selected.root / "plugin.json")},{"source",selected.source},{"enabled",enabled}});
    }
    snapshot.lock["packages"] = std::move(packages);
    const auto digest = [](const std::string& text) { return Core::Assets::ToHexString(Core::Assets::ComputeSha256(reinterpret_cast<const uint8_t*>(text.data()),text.size())); };
    const Json resolvedFile{{"schemaVersion",1},{"plugins",selection},{"inputs",{
        {"manifest.json",digest(snapshot.manifest.dump(2))},{"packages-lock.json",digest(snapshot.lock.dump(2))},{"plugins.json",digest(snapshot.plugins.dump(2))}}}};
    CommitFiles({{"manifest.json",snapshot.manifest.dump(2)},{"packages-lock.json",snapshot.lock.dump(2)},
        {"plugins.json",snapshot.plugins.dump(2)},{"resolved-plugins.json",resolvedFile.dump(2)}});
    PackageResult result; result.succeeded = true; return result;
}
PackageResult PackageManager::Install(const PackageRequest& request)
{
    try
    {
        auto canonical=request;
        if (canonical.kind==PackageSourceKind::Directory || canonical.kind==PackageSourceKind::Zip)
            canonical.location=Utf8(std::filesystem::weakly_canonical(std::filesystem::absolute(std::filesystem::u8path(canonical.location))));
        return InstallImpl(canonical,false);
    }
    catch (const std::exception& e) { return Error(e); }
}
PackageResult PackageManager::InstallImpl(const PackageRequest& request, bool repairCache)
{
    struct Staging
    {
        std::vector<std::filesystem::path> paths;
        ~Staging() { for (const auto& path : paths) { std::error_code ignored; std::filesystem::remove_all(path,ignored); } }
    } staging;
    try
    {
        auto provider = m_providers.find(request.kind); if (provider == m_providers.end()) throw std::runtime_error("package source provider is not configured");
        Snapshot initial;
        { InterprocessFileLock projectLock(m_project / "Packages/.write.lock"); RecoverLocked(); initial = ReadSnapshot(); }
        std::vector<PackageRequest> downloads;
        if (auto catalogSource = std::dynamic_pointer_cast<IPackageCatalogSource>(provider->second); catalogSource && !repairCache)
        {
            const auto& baseline = initial;
            auto candidates = Available(baseline);
            const auto remote = catalogSource->Candidates(request);
            for (const auto& candidate : remote)
            {
                candidates.erase(std::remove_if(candidates.begin(),candidates.end(),[&](const auto& previous) {
                    return previous.id == candidate.id && previous.version == candidate.version && previous.source == candidate.source;
                }),candidates.end());
                candidates.push_back(candidate);
            }
            std::vector<Plugins::ResolveRequest> requirements{{request.id,request.version,request.location}};
            for (auto it = baseline.manifest.at("dependencies").begin(); it != baseline.manifest.at("dependencies").end(); ++it)
                if (it.key() != request.id) requirements.push_back(DependencyRequest(it.key(),it.value()));
            for (auto it = baseline.plugins.at("enabled").begin(); it != baseline.plugins.at("enabled").end(); ++it)
                if (it.value().template get<bool>() && std::none_of(requirements.begin(),requirements.end(),[&](const auto& r) { return r.id == it.key(); }))
                    requirements.push_back({it.key(),"*",{}});
            std::map<std::string,std::string> locked;
            for (const auto& p : baseline.lock.at("packages")) if (p.at("id") != request.id)
                locked[p.at("id").template get<std::string>()] = p.at("version").template get<std::string>();
            const auto resolved = Plugins::ResolvePlugins(requirements,candidates,Plugins::HostPlatform(),locked);
            if (!resolved.succeeded) throw std::runtime_error(resolved.error);
            for (const auto& selected : resolved.ordered)
            {
                if (selected.source != request.location) continue;
                const auto existing = std::find_if(baseline.lock.at("packages").begin(),baseline.lock.at("packages").end(),[&](const auto& p) {
                    return p.at("id") == selected.id && p.at("version") == selected.version.String() && p.at("source") == selected.source;
                });
                if (selected.id != request.id && existing != baseline.lock.at("packages").end() &&
                    CacheMatches(CacheRoot() / existing->at("hash").template get<std::string>(),existing->at("hash").template get<std::string>(),m_limits)) continue;
                auto exact = request; exact.id = selected.id; exact.version = selected.version.String();
                exact.expectedHash = selected.id == request.id ? request.expectedHash : std::string{};
                exact.expectedArchiveHash = selected.distributionHash; downloads.push_back(std::move(exact));
            }
        }
        else downloads.push_back(request);
        std::vector<Json> records;
        std::vector<std::unique_ptr<InterprocessFileLock>> cacheLeases;
        std::string rootId = request.id, rootVersion, rootHash;
        for (const auto& download : downloads)
        {
            const auto temporary = CacheRoot() / ".staging" / NLS::Guid::New().ToString(); staging.paths.push_back(temporary);
            std::filesystem::create_directories(temporary); provider->second->Fetch(download,temporary / "content",m_limits);
            if (request.cancel && request.cancel->load()) throw std::runtime_error("package installation cancelled");
            const auto manifest = Plugins::PluginManifest::Read(temporary / "content/plugin.json");
            if ((!download.id.empty() && manifest.id != download.id) || !Plugins::Satisfies(manifest.version,download.version)) throw std::runtime_error("downloaded package identity/version mismatch");
            const auto hash = HashPackageTree(temporary / "content",m_limits);
            if (!download.expectedHash.empty() && download.expectedHash != hash) throw std::runtime_error("package content integrity mismatch");
            const auto cache = CacheRoot() / hash;
            std::unique_ptr<InterprocessFileLock> cacheLease;
            if (std::filesystem::exists(cache))
            {
                cacheLease = std::make_unique<InterprocessFileLock>(CacheRoot() / ".locks" / (hash + ".lock"),false);
                if (!CacheMatches(cache,hash,m_limits))
                { if (!repairCache) throw std::runtime_error("existing cache content is corrupt; run repair"); cacheLease.reset(); }
            }
            if (!cacheLease)
            {
                InterprocessFileLock cacheLock(CacheRoot() / ".locks" / (hash + ".lock"),true,
                    repairCache ? std::chrono::milliseconds(0) : std::chrono::seconds(30));
                if (std::filesystem::exists(cache) && !CacheMatches(cache,hash,m_limits))
                {
                    if (!repairCache) throw std::runtime_error("existing cache content is corrupt; run repair");
                    const auto quarantine = CacheRoot() / ".quarantine" / (hash + "-" + NLS::Guid::New().ToString());
                    std::filesystem::create_directories(quarantine.parent_path()); std::filesystem::rename(cache,quarantine);
                    try { PublishDirectory(temporary,cache); }
                    catch (...) { std::error_code ignored; std::filesystem::rename(quarantine,cache,ignored); throw; }
                }
                else if (!std::filesystem::exists(cache)) PublishDirectory(temporary,cache);
            }
            if (!cacheLease) cacheLease = std::make_unique<InterprocessFileLock>(CacheRoot() / ".locks" / (hash + ".lock"),false);
            cacheLeases.push_back(std::move(cacheLease));
            Json dependencies = Json::array(); for (const auto& dep : manifest.dependencies) dependencies.push_back({{"id",dep.id},{"version",dep.version},{"optional",dep.optional}});
            records.push_back({{"id",manifest.id},{"version",manifest.version.String()},{"source",request.location},{"kind",static_cast<uint32_t>(request.kind)},
                {"revision",request.revision},{"hash",hash},{"archiveHash",download.expectedArchiveHash},{"dependencies",dependencies},{"platform",Plugins::HostPlatform()}});
            if (rootId.empty() || manifest.id == rootId) { rootId = manifest.id; rootVersion = manifest.version.String(); rootHash = hash; }
        }
        if (rootVersion.empty()) throw std::runtime_error("package resolver did not select the requested package");
        // Re-read and resolve the latest project generation after downloads.
        // Concurrent intents are merged under the lock; incompatible changes fail
        // without publishing any of this graph's metadata.
        InterprocessFileLock projectLock(m_project / "Packages/.write.lock"); RecoverLocked();
        auto snapshot = ReadSnapshot();
        const auto lockedIdentity = [](const Snapshot& state,const std::string& id) -> Json {
            for (const auto& p : state.lock.at("packages")) if (p.at("id") == id) return p;
            return nullptr;
        };
        for (const auto& record : records)
        {
            const auto id = record.at("id").get<std::string>(); const auto previous = lockedIdentity(initial,id), current = lockedIdentity(snapshot,id);
            if (previous != current && !current.is_null() &&
                (current.at("hash") != record.at("hash") || current.at("source") != record.at("source")))
                throw std::runtime_error("concurrent package update changed " + id + "; resolve and retry");
        }
        auto& packages = snapshot.lock["packages"];
        for (const auto& record : records)
        {
            packages.erase(std::remove_if(packages.begin(),packages.end(),[&](const auto& p) { return p.at("id") == record.at("id"); }),packages.end());
            packages.push_back(record);
            if (!snapshot.plugins["enabled"].contains(record.at("id").template get<std::string>())) snapshot.plugins["enabled"][record.at("id").template get<std::string>()] = false;
        }
        if (!repairCache) snapshot.manifest["dependencies"][rootId] = {{"version",request.version == "*" ? rootVersion : request.version},{"source",request.location}};
        auto result = CommitResolved(std::move(snapshot)); result.id = rootId; result.version = rootVersion; result.contentHash = rootHash; return result;
    }
    catch (const std::exception& e) { return Error(e); }
}
PackageResult PackageManager::Uninstall(const std::string& id)
{
    try
    {
        InterprocessFileLock lock(m_project / "Packages/.write.lock"); RecoverLocked(); auto snapshot = ReadSnapshot();
        for (const auto& package : snapshot.lock.at("packages"))
            if (package.at("id").get<std::string>() != id)
                for (const auto& dependency : package.at("dependencies"))
                    if (dependency.at("id").get<std::string>() == id && !dependency.at("optional").get<bool>()) throw std::runtime_error("package is required by " + package.at("id").get<std::string>());
        snapshot.manifest["dependencies"].erase(id); snapshot.plugins["enabled"].erase(id);
        return CommitResolved(std::move(snapshot));
    }
    catch (const std::exception& e) { return Error(e); }
}
PackageResult PackageManager::SetEnabled(const std::string& id, bool enabled)
{
    try
    {
        InterprocessFileLock lock(m_project / "Packages/.write.lock"); RecoverLocked(); auto snapshot = ReadSnapshot();
        auto candidates = Available(snapshot);
        if (std::none_of(candidates.begin(),candidates.end(),[&](const auto& p) { return p.id == id; })) throw std::runtime_error("plugin is not installed: " + id);
        if (!enabled)
            for (const auto& p : candidates)
                if (p.id != id && snapshot.plugins["enabled"].value(p.id,false))
                    for (const auto& dep : p.dependencies) if (dep.id == id && !dep.optional) throw std::runtime_error("enabled plugin requires dependency: " + p.id);
        if (enabled)
        {
            if (!snapshot.manifest["dependencies"].contains(id))
            {
                const auto selected = std::find_if(candidates.begin(),candidates.end(),[&](const auto& candidate) { return candidate.id == id; });
                snapshot.manifest["dependencies"][id] = {{"version",selected->version.String()},{"source",selected->source}};
            }
            auto resolved = Plugins::ResolvePlugins({{id,"*",{}}},candidates,Plugins::HostPlatform());
            if (!resolved.succeeded) throw std::runtime_error(resolved.error);
            for (const auto& p : resolved.ordered) snapshot.plugins["enabled"][p.id] = true;
        }
        else snapshot.plugins["enabled"][id] = false;
        return CommitResolved(std::move(snapshot));
    }
    catch (const std::exception& e) { return Error(e); }
}
PackageResult PackageManager::Resolve()
{
    try { InterprocessFileLock lock(m_project / "Packages/.write.lock"); RecoverLocked(); return CommitResolved(ReadSnapshot()); }
    catch (const std::exception& e) { return Error(e); }
}
std::vector<Plugins::PluginManifest> PackageManager::List() const
{
    InterprocessFileLock lock(m_project / "Packages/.write.lock");
    const_cast<PackageManager*>(this)->RecoverLocked(); return Available(ReadSnapshot());
}
std::vector<Plugins::PluginManifest> PackageManager::Search(const std::string& query) const
{
    auto items = List(); items.erase(std::remove_if(items.begin(),items.end(),[&](const auto& p) { return p.id.find(query) == std::string::npos && p.name.find(query) == std::string::npos; }),items.end()); return items;
}
PackageResult PackageManager::Validate()
{
    try
    {
        InterprocessFileLock lock(m_project / "Packages/.write.lock"); RecoverLocked(); const auto snapshot = ReadSnapshot();
        for (const auto& package : snapshot.lock.at("packages"))
        {
            const auto hash = package.at("hash").get<std::string>(); if (!IsHash(hash)) throw std::runtime_error("invalid package hash");
            if (HashPackageTree(CacheRoot() / hash / "content",m_limits) != hash) throw std::runtime_error("package cache integrity mismatch: " + package.at("id").get<std::string>());
        }
        PackageResult result; result.succeeded = true; return result;
    }
    catch (const std::exception& e) { return Error(e); }
}
std::vector<Plugins::PluginManifest> PackageManager::SearchRegistry(const std::string& location,const std::string& query,const std::atomic_bool* cancel) const
{
    const auto provider = m_providers.find(PackageSourceKind::Registry);
    const auto source = provider == m_providers.end() ? nullptr : std::dynamic_pointer_cast<IPackageCatalogSource>(provider->second);
    if (!source) throw std::runtime_error("Registry provider is not configured");
    PackageRequest request{PackageSourceKind::Registry,location}; request.cancel = cancel;
    auto results = source->Candidates(request);
    results.erase(std::remove_if(results.begin(),results.end(),[&](const auto& p) { return p.id.find(query) == std::string::npos && p.name.find(query) == std::string::npos; }),results.end());
    std::sort(results.begin(),results.end(),[](const auto& a,const auto& b) { return a.id != b.id ? a.id < b.id : a.version > b.version; });
    return results;
}
PackageResult PackageManager::RestoreOffline() { auto valid = Validate(); return valid.succeeded ? Resolve() : valid; }
PackageResult PackageManager::Repair()
{
    auto result = Validate(); if (result.succeeded) return Resolve();
    // Restore only from recorded providers; never synthesize a successful cache.
    std::vector<PackageRequest> requests;
    try
    {
        { InterprocessFileLock lock(m_project / "Packages/.write.lock"); RecoverLocked();
          const auto snapshot = ReadSnapshot();
          for (const auto& p : snapshot.lock.at("packages")) requests.push_back({static_cast<PackageSourceKind>(p.at("kind").get<uint32_t>()),p.at("source").get<std::string>(),p.at("id").get<std::string>(),p.at("version").get<std::string>(),p.at("revision").get<std::string>(),p.at("hash").get<std::string>()}); }
        // Archive hashes pin Registry metadata as well as the extracted tree.
        { InterprocessFileLock lock(m_project / "Packages/.write.lock"); const auto snapshot = ReadSnapshot();
          for (auto& request : requests) for (const auto& package : snapshot.lock.at("packages"))
              if (package.at("id") == request.id) request.expectedArchiveHash = package.value("archiveHash",std::string{}); }
        for (const auto& request : requests)
        {
            if (CacheMatches(CacheRoot() / request.expectedHash,request.expectedHash,m_limits)) continue;
            auto restored = InstallImpl(request,true); if (!restored.succeeded) return restored;
        }
        return Validate();
    }
    catch (const std::exception& e) { return Error(e); }
}
PackageResult PackageManager::ApproveSourceBuild(const std::string& id)
{
    try
    {
        InterprocessFileLock lock(m_project / "Packages/.write.lock"); RecoverLocked();
        const auto snapshot = ReadSnapshot(); const auto candidates = Available(snapshot);
        Plugins::ResolveRequest request{id,"*",{}}; std::map<std::string,std::string> locked;
        if (snapshot.manifest.at("dependencies").contains(id)) request=DependencyRequest(id,snapshot.manifest.at("dependencies").at(id));
        for (const auto& package : snapshot.lock.at("packages"))
        {
            locked[package.at("id").get<std::string>()]=package.at("version").get<std::string>();
            if (package.at("id")==id && request.source.empty()) request.source=package.at("source").get<std::string>();
        }
        const auto resolved=Plugins::ResolvePlugins({request},candidates,Plugins::HostPlatform(),locked);
        if (!resolved.succeeded) throw std::runtime_error(resolved.error);
        const auto selected=std::find_if(resolved.ordered.begin(),resolved.ordered.end(),[&](const auto& p) { return p.id==id; });
        if (selected==resolved.ordered.end()) throw std::runtime_error("source plugin is not installed");
        if (std::none_of(selected->modules.begin(),selected->modules.end(),[](const auto& m) { return !m.sourceTarget.empty(); }))
            throw std::runtime_error("plugin does not declare a source build");
        const auto hash=Plugins::HashPluginBuildApprovalInputs(selected->root);
        auto trust = ReadJson(m_project / "Packages/build-trust.json",{{"schemaVersion",1},{"packages",Json::object()}});
        if (trust.value("schemaVersion",0)!=1) throw std::runtime_error("unsupported source build trust schema");
        trust["packages"][id] = {{"buildHash",hash},{"source",selected->source},{"inputScope","package-content-v1"}};
        CommitFiles({{"build-trust.json",trust.dump(2)}});
        PackageResult result; result.succeeded = true; result.id = id; return result;
    }
    catch (const std::exception& e) { return Error(e); }
}
}
