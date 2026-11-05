#pragma once
#include "PackageManager.h"

namespace NLS::Packages
{
struct RemotePackageOptions
{
    std::filesystem::path gitExecutable, curlExecutable, caCertificate;
    std::vector<std::string> allowedRedirectOrigins;
};
class NLS_PACKAGES_API GitPackageSource final : public IPackageSource
{
public:
    explicit GitPackageSource(RemotePackageOptions options);
    void Fetch(const PackageRequest&, const std::filesystem::path&, const PackageLimits&) override;
private:
    RemotePackageOptions m_options;
};
class NLS_PACKAGES_API RegistryPackageSource final : public IPackageCatalogSource
{
public:
    explicit RegistryPackageSource(RemotePackageOptions options);
    ~RegistryPackageSource();
    std::vector<Plugins::PluginManifest> Candidates(const PackageRequest&) override;
    void Fetch(const PackageRequest&, const std::filesystem::path&, const PackageLimits&) override;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
NLS_PACKAGES_API RemotePackageOptions DefaultRemotePackageOptions();
NLS_PACKAGES_API void ConfigureRemotePackageSources(PackageManager& manager, RemotePackageOptions options = DefaultRemotePackageOptions());
}
