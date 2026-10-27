#pragma once

#include "BuildDef.h"
#include "BuildTypes.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace NLS::Build
{
struct BuildContext;
struct BuildReport;

// A resolved prebuilt player template (default path) or a source-built
// payload. The adapter owns the knowledge of which engine files compose a
// runnable player for its platform.
struct PlayerTemplateInfo
{
    std::filesystem::path directory;
    std::filesystem::path executablePath;
    std::string executableName;
    std::string engineVersion;
    std::string playerId;
    // Target the player was produced for ("windows-x64-dx12"); empty when the
    // template predates the target descriptor.
    std::string targetId;
    // Runtime files that must be copied into the install root.
    std::vector<std::filesystem::path> runtimeFiles;
    // Files that belong to the separate symbol package.
    std::vector<std::filesystem::path> symbolFiles;
    bool hasManagedAssemblies = false;
    bool hasLuaScripts = false;
    bool sourceBuilt = false;
    std::map<std::string, std::string> metadata;
};

// Platform adapter contract. Every shipping platform implements this
// interface; unimplemented platforms are registered as declared-only
// capabilities so the editor can report exactly what is missing.
class NLS_BUILD_API IBuildPlatform
{
public:
    virtual ~IBuildPlatform() = default;

    virtual std::string GetAdapterId() const = 0;
    virtual PlatformCapability GetCapability(const BuildTarget& target) const = 0;

    // Verifies the host provides everything the target needs (compilers,
    // shader tools, signing tools, SDKs).
    virtual bool CheckToolchain(const BuildContext& context, BuildReport& report, std::string* error) = 0;

    // Locates and describes the prebuilt player the profile selects.
    virtual std::optional<PlayerTemplateInfo> ResolvePlayerTemplate(
        const BuildContext& context,
        BuildReport& report,
        std::string* error) = 0;

    // Copies the player and its runtime dependencies into the staging root.
    virtual bool AssemblePlayer(
        const BuildContext& context,
        const PlayerTemplateInfo& player,
        BuildReport& report,
        std::string* error) = 0;

    // Applies product name, icon, version metadata, and platform layout rules
    // to the staged payload.
    virtual bool FinalizeStagingPlayer(
        const BuildContext& context,
        BuildReport& report,
        std::string* error) = 0;

    // Signs staged binaries. Returns true when signing was performed or when
    // no signing profile is configured.
    virtual bool SignStaging(
        const BuildContext& context,
        BuildReport& report,
        std::string* error) = 0;

    // Produces the distributable artifact (zip, tar.zst, app+dmgy, ...) from
    // the staging directory. Returns the created output root.
    virtual bool PackageOutput(
        const BuildContext& context,
        const std::filesystem::path& stagingDirectory,
        BuildReport& report,
        std::filesystem::path& outputRoot,
        std::string* error) = 0;

    // Final acceptance: runs the packaged player's self-test (or a platform
    // equivalent) against the produced output. Adapters that cannot execute
    // their output locally must return true and record why in the report.
    virtual bool ValidateOutput(
        const BuildContext& context,
        const std::filesystem::path& packageDirectory,
        BuildReport& report,
        std::string* error)
    {
        (void)context;
        (void)packageDirectory;
        (void)report;
        (void)error;
        return true;
    }
};

// Registry of adapters plus declared future targets so capability reporting
// covers every roadmap platform.
class NLS_BUILD_API BuildPlatformRegistry
{
public:
    static BuildPlatformRegistry& Get();

    void Register(std::shared_ptr<IBuildPlatform> adapter);
    void Unregister(const std::string& adapterId);
    void RegisterDeclaredTarget(PlatformCapability capability);
    void Clear();

    IBuildPlatform* Find(const BuildTarget& target) const;
    std::vector<PlatformCapability> DescribeAll() const;
    // Human-readable capability matrix used by `NullusBuild capabilities`.
    std::string FormatCapabilities() const;

private:
    std::vector<std::shared_ptr<IBuildPlatform>> m_adapters;
    std::vector<PlatformCapability> m_declared;
};

NLS_BUILD_API void RegisterDefaultBuildPlatforms();
// Registers the Windows x64 / DX12 desktop adapter implemented in this binary.
NLS_BUILD_API void RegisterWindowsDesktopPlatform();
// Registers Linux x64 (Vulkan/OpenGL) and macOS (Metal) desktop adapters.
NLS_BUILD_API void RegisterLinuxDesktopPlatform();
NLS_BUILD_API void RegisterMacOSDesktopPlatform();
} // namespace NLS::Build
