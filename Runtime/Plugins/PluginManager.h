#pragma once
#include "PluginHost.h"
#include "Manifest.h"
#include "Core/Assets/PluginAssetPath.h"
#include "Platform/Process/InterprocessFileLock.h"

namespace NLS::Plugins
{
class NLS_PLUGINS_API PluginCatalog
{
public:
    void Discover(const std::filesystem::path& root);
    void Add(PluginManifest manifest);
    const std::vector<PluginManifest>& Candidates() const { return m_candidates; }
    const std::vector<std::string>& Diagnostics() const { return m_diagnostics; }
private:
    std::vector<PluginManifest> m_candidates;
    std::vector<std::string> m_diagnostics;
};

struct ProjectPluginConfiguration
{
    struct LockedPackage { std::string id, version, source, hash; };
    std::vector<ResolveRequest> dependencies;
    std::map<std::string, bool> enabled;
    std::map<std::string, std::string> locked;
    std::string fingerprint;
    std::vector<LockedPackage> packages;
};
NLS_PLUGINS_API void AddLockedProjectPackages(PluginCatalog& catalog, const std::filesystem::path& project,
    const ProjectPluginConfiguration& config, std::vector<std::shared_ptr<Platform::Process::InterprocessFileLock>>& leases);
enum class ProjectSwitchResult { Reuse, RestartRequired, Invalid };

class PluginManager;
class NLS_PLUGINS_API PluginSession
{
public:
    uint64_t Generation() const { return generation; }
    const std::filesystem::path& ProjectRoot() const { return projectRoot; }
    const std::vector<PluginManifest>& Active() const { return active; }
private:
    friend class PluginManager;
    std::filesystem::path projectRoot,engineRoot;
    std::vector<PluginManifest> active;
    std::map<std::string,std::string> fingerprints;
    Core::Assets::PluginContentRoots contentRoots;
    uint64_t generation=1;
    bool prePlay=false,closed=false,player=false;
    std::vector<std::shared_ptr<Platform::Process::InterprocessFileLock>> cacheLeases;
};

class NLS_PLUGINS_API PluginManager
{
public:
    PluginManager(PluginHost& host, Core::Modules::HostKind kind);
    PluginCatalog& Catalog() { return m_catalog; }
    void AddSourceFactory(const std::string& target, std::function<std::unique_ptr<Core::Modules::IModule>()> factory,std::string inputHash = {});
    bool OpenProject(const std::filesystem::path& project, const std::filesystem::path& enginePlugins = {});
    bool OpenPlayer(const std::filesystem::path& installRoot);
    bool LoadSelection(const std::vector<ResolveRequest>& requests, const std::map<std::string, std::string>& locked = {});
    bool Advance(Core::Modules::LoadingPhase phase);
    void DispatchRuntime(uint32_t event, double deltaSeconds = 0);
    void Pump();
    void Close();
    ProjectSwitchResult CompareProject(const std::vector<PluginManifest>& selection) const;
    const std::vector<PluginManifest>& Active() const { return m_session.active; }
    const PluginSession& Session() const { return m_session; }
    const std::string& LastError() const { return m_error; }
    uint64_t SessionGeneration() const { return m_session.generation; }
    const std::filesystem::path& ProjectRoot() const { return m_session.projectRoot; }
    const std::filesystem::path& EnginePluginRoot() const { return m_session.engineRoot; }
    Core::Assets::PluginContentRoots& ContentRoots() { return m_session.contentRoots; }
    static ProjectPluginConfiguration ReadConfiguration(const std::filesystem::path& project);
private:
    PluginHost& m_host;
    Core::Modules::HostKind m_kind;
    PluginCatalog m_catalog;
    struct SourceFactory { std::function<std::unique_ptr<Core::Modules::IModule>()> create; std::string inputHash; };
    std::map<std::string,SourceFactory> m_sources;
    PluginSession m_session;
    std::string m_error;
};
}
