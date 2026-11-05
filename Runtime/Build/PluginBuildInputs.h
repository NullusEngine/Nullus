#pragma once
#include "BuildDependencyCollector.h"
#include "Plugins/PluginManager.h"
namespace NLS::Build
{
// Metadata-only build adapter: no plugin DLL or source build script is run.
class NLS_BUILD_API PluginBuildInputs
{
public:
    PluginBuildInputs(const std::filesystem::path& project,const std::string& platform);
    ~PluginBuildInputs();
    PluginBuildInputs(const PluginBuildInputs&) = delete;
    bool IsCurrent() const;
    const std::filesystem::path& CookRoot() const { return m_cookRoot; }
    const std::vector<std::string>& KeepGuids() const { return m_keepGuids; }
    const std::vector<CollectedRawFile>& RawFiles() const { return m_rawFiles; }
    const std::string& Fingerprint() const { return m_fingerprint; }
private:
    std::filesystem::path m_project,m_cookRoot;
    std::string m_configuration,m_fingerprint;
    std::vector<std::pair<std::filesystem::path,std::string>> m_inputs;
    std::vector<std::shared_ptr<Platform::Process::InterprocessFileLock>> m_leases;
    std::vector<std::string> m_keepGuids;
    std::vector<CollectedRawFile> m_rawFiles;
};
}
