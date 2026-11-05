#pragma once
#include "PackagesDef.h"
#include "Plugins/Manifest.h"
#include <functional>
#include <memory>
#include <atomic>

namespace NLS::Packages
{
enum class PackageSourceKind { Directory, Zip, Git, Registry };
struct PackageRequest
{
    PackageSourceKind kind = PackageSourceKind::Directory;
    std::string location, id, version = "*", revision, expectedHash;
    std::string expectedArchiveHash;
    const std::atomic_bool* cancel = nullptr;
};
struct PackageResult
{
    bool succeeded = false;
    std::string id, version, contentHash, error;
};
struct PackageLimits
{
    uint64_t maxDownloadBytes = UINT64_C(1024) * 1024 * 1024;
    uint64_t maxExpandedBytes = UINT64_C(4) * 1024 * 1024 * 1024;
    uint32_t maxFiles = 100000;
};
class IPackageSource
{
public:
    virtual ~IPackageSource() = default;
    virtual void Fetch(const PackageRequest& request, const std::filesystem::path& destination, const PackageLimits& limits) = 0;
};
class IPackageCatalogSource : public IPackageSource
{
public:
    virtual std::vector<Plugins::PluginManifest> Candidates(const PackageRequest& request) = 0;
};
class NLS_PACKAGES_API LocalDirectorySource final : public IPackageSource
{
public:
    void Fetch(const PackageRequest&, const std::filesystem::path&, const PackageLimits&) override;
};
class NLS_PACKAGES_API ZipPackageSource final : public IPackageSource
{
public:
    void Fetch(const PackageRequest&, const std::filesystem::path&, const PackageLimits&) override;
};
// Deterministic SHA-256 over canonical relative names, file sizes and bytes.
// The same tree can be restored from a ZIP, directory or pinned Git commit.
NLS_PACKAGES_API std::string HashPackageTree(const std::filesystem::path& root, const PackageLimits& limits = {});

class NLS_PACKAGES_API PackageManager
{
public:
    explicit PackageManager(std::filesystem::path project, PackageLimits limits = {});
    void SetProvider(PackageSourceKind kind, std::shared_ptr<IPackageSource> provider);
    void AddSearchRoot(std::filesystem::path root) { if (!root.empty()) m_searchRoots.push_back(std::move(root)); }
    PackageResult Install(const PackageRequest& request);
    PackageResult Uninstall(const std::string& id);
    PackageResult SetEnabled(const std::string& id, bool enabled);
    PackageResult Resolve();
    PackageResult RestoreOffline();
    std::vector<Plugins::PluginManifest> List() const;
    std::vector<Plugins::PluginManifest> Search(const std::string& query) const;
    std::vector<Plugins::PluginManifest> SearchRegistry(const std::string& location,const std::string& query,const std::atomic_bool* cancel = nullptr) const;
    PackageResult Validate();
    PackageResult Repair();
    PackageResult ApproveSourceBuild(const std::string& id);
    void Recover();
    // Used by crash-injection tests and tool progress reporting. It is invoked
    // after a durable commit point, not as a substitute for actual file IO.
    void SetTransactionObserver(std::function<void(const std::string&)> observer) { m_observer = std::move(observer); }
    const std::filesystem::path& Project() const { return m_project; }
    std::filesystem::path CacheRoot() const { return m_project / "Library/PackageCache"; }
private:
    struct Snapshot;
    Snapshot ReadSnapshot() const;
    PackageResult CommitResolved(Snapshot snapshot);
    PackageResult InstallImpl(const PackageRequest& request, bool repairCache);
    void CommitFiles(const std::map<std::string,std::string>& files);
    void RecoverLocked();
    std::vector<Plugins::PluginManifest> Available(const Snapshot& snapshot) const;
    std::filesystem::path m_project;
    PackageLimits m_limits;
    std::map<PackageSourceKind,std::shared_ptr<IPackageSource>> m_providers;
    std::function<void(const std::string&)> m_observer;
    std::vector<std::filesystem::path> m_searchRoots;
};
}
