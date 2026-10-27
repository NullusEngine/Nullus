#include "IBuildPlatform.h"

#include "BuildReport.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <sstream>

namespace NLS::Build
{
namespace
{
std::string AndroidCapabilityReason()
{
    std::string reason =
        "Android requires the NDK/Gradle project, lifecycle input, and script host adaptation.";
    const char* environment[] = {"ANDROID_NDK_HOME", "ANDROID_NDK_ROOT", "ANDROID_HOME", "ANDROID_SDK_ROOT"};
    for (const auto* variable : environment)
    {
        const char* value = std::getenv(variable);
        if (value == nullptr || value[0] == '\0')
            continue;
        std::error_code error;
        if (std::filesystem::is_directory(value, error))
        {
            reason += std::string(" Detected ") + variable + " at " + value +
                "; the APK/AAB packaging backend is still missing.";
            return reason;
        }
    }
    reason += " No Android SDK/NDK environment variable points at an existing directory.";
    return reason;
}

std::string IOSCapabilityReason()
{
    std::string reason =
        "iOS requires Xcode archives, Metal, AOT scripting, and provisioning profiles.";
#if defined(__APPLE__)
    std::error_code error;
    if (std::filesystem::is_regular_file("/usr/bin/xcodebuild", error))
    {
        reason += " Xcode was detected, but the archive/asset-injection backend is still missing.";
        return reason;
    }
    reason += " Xcode was not found; install Xcode and accept its license.";
#else
    reason += " The iOS adapter requires a macOS host with Xcode.";
#endif
    return reason;
}
} // namespace

BuildPlatformRegistry& BuildPlatformRegistry::Get()
{
    static BuildPlatformRegistry registry;
    return registry;
}

void BuildPlatformRegistry::Register(std::shared_ptr<IBuildPlatform> adapter)
{
    if (adapter == nullptr)
        return;
    const auto id = adapter->GetAdapterId();
    for (auto& existing : m_adapters)
    {
        if (existing != nullptr && existing->GetAdapterId() == id)
        {
            existing = std::move(adapter);
            return;
        }
    }
    m_adapters.push_back(std::move(adapter));
}

void BuildPlatformRegistry::Unregister(const std::string& adapterId)
{
    m_adapters.erase(
        std::remove_if(
            m_adapters.begin(),
            m_adapters.end(),
            [&adapterId](const std::shared_ptr<IBuildPlatform>& adapter)
            {
                return adapter == nullptr || adapter->GetAdapterId() == adapterId;
            }),
        m_adapters.end());
}

void BuildPlatformRegistry::RegisterDeclaredTarget(PlatformCapability capability)
{
    for (auto& existing : m_declared)
    {
        if (existing.platform == capability.platform &&
            existing.architecture == capability.architecture &&
            existing.graphicsBackend == capability.graphicsBackend)
        {
            existing = std::move(capability);
            return;
        }
    }
    m_declared.push_back(std::move(capability));
}

void BuildPlatformRegistry::Clear()
{
    m_adapters.clear();
    m_declared.clear();
}

IBuildPlatform* BuildPlatformRegistry::Find(const BuildTarget& target) const
{
    for (const auto& adapter : m_adapters)
    {
        if (adapter == nullptr)
            continue;
        const auto capability = adapter->GetCapability(target);
        if (capability.adapterAvailable &&
            capability.platform == target.platform &&
            capability.architecture == target.architecture &&
            capability.graphicsBackend == target.graphicsBackend)
        {
            return adapter.get();
        }
    }
    return nullptr;
}

std::vector<PlatformCapability> BuildPlatformRegistry::DescribeAll() const
{
    std::vector<PlatformCapability> capabilities;
    for (const auto& adapter : m_adapters)
    {
        if (adapter == nullptr)
            continue;
        // Adapters describe the target they are asked about; enumerate the
        // roadmap targets so unimplemented variants still show up.
        static const std::vector<BuildTarget> probes {
            [] { BuildTarget t; t.platform = PlatformFamily::Windows; t.architecture = Architecture::X64; t.graphicsBackend = GraphicsBackendKind::DX12; return t; }(),
            [] { BuildTarget t; t.platform = PlatformFamily::Linux; t.architecture = Architecture::X64; t.graphicsBackend = GraphicsBackendKind::Vulkan; return t; }(),
            [] { BuildTarget t; t.platform = PlatformFamily::MacOS; t.architecture = Architecture::Arm64; t.graphicsBackend = GraphicsBackendKind::Metal; return t; }(),
            [] { BuildTarget t; t.platform = PlatformFamily::MacOS; t.architecture = Architecture::X64; t.graphicsBackend = GraphicsBackendKind::Metal; return t; }(),
            [] { BuildTarget t; t.platform = PlatformFamily::Windows; t.architecture = Architecture::Arm64; t.graphicsBackend = GraphicsBackendKind::DX12; return t; }(),
            [] { BuildTarget t; t.platform = PlatformFamily::Linux; t.architecture = Architecture::Arm64; t.graphicsBackend = GraphicsBackendKind::Vulkan; return t; }(),
            [] { BuildTarget t; t.platform = PlatformFamily::Android; t.architecture = Architecture::Arm64; t.graphicsBackend = GraphicsBackendKind::Vulkan; return t; }(),
            [] { BuildTarget t; t.platform = PlatformFamily::IOS; t.architecture = Architecture::Arm64; t.graphicsBackend = GraphicsBackendKind::Metal; return t; }(),
            [] { BuildTarget t; t.platform = PlatformFamily::Web; t.architecture = Architecture::Unknown; t.graphicsBackend = GraphicsBackendKind::WebGPU; return t; }(),
        };
        for (const auto& target : probes)
        {
            auto capability = adapter->GetCapability(target);
            if (capability.adapterAvailable &&
                capability.platform == target.platform &&
                capability.architecture == target.architecture &&
                capability.graphicsBackend == target.graphicsBackend)
            {
                capabilities.push_back(std::move(capability));
            }
        }
    }
    capabilities.insert(capabilities.end(), m_declared.begin(), m_declared.end());
    return capabilities;
}

std::string BuildPlatformRegistry::FormatCapabilities() const
{
    std::ostringstream stream;
    stream << "Build platform capabilities\n";
    const auto capabilities = DescribeAll();
    for (const auto& capability : capabilities)
    {
        stream << "  " << (capability.id.empty()
                ? std::string(ToString(capability.platform)) + "-" +
                    ToString(capability.architecture) + "-" + ToString(capability.graphicsBackend)
                : capability.id)
               << ": " << (capability.adapterAvailable ? "available" : "not implemented");
        if (!capability.toolchainAvailable && capability.adapterAvailable)
            stream << " (toolchain missing)";
        if (!capability.reason.empty())
            stream << " - " << capability.reason;
        stream << "\n";
    }
    return stream.str();
}

void RegisterDefaultBuildPlatforms()
{
    static bool registered = false;
    if (registered)
        return;
    registered = true;
    auto& registry = BuildPlatformRegistry::Get();

    RegisterWindowsDesktopPlatform();
    RegisterLinuxDesktopPlatform();
    RegisterMacOSDesktopPlatform();

    // Declared roadmap targets. Registering them keeps the editor honest: a
    // project building for these platforms sees the missing capability instead
    // of silently producing an undeliverable payload.
    const auto declare = [&registry](
        const PlatformFamily platform,
        const Architecture architecture,
        const GraphicsBackendKind backend,
        std::string reason)
    {
        PlatformCapability capability;
        capability.platform = platform;
        capability.architecture = architecture;
        capability.graphicsBackend = backend;
        capability.adapterAvailable = false;
        capability.toolchainAvailable = false;
        capability.id = std::string(ToString(platform)) + "-" + ToString(architecture) + "-" + ToString(backend);
        capability.reason = std::move(reason);
        registry.RegisterDeclaredTarget(std::move(capability));
    };

    declare(PlatformFamily::Windows, Architecture::Arm64, GraphicsBackendKind::DX12,
        "Windows ARM64 requires its own player template and device validation.");
    declare(PlatformFamily::Linux, Architecture::Arm64, GraphicsBackendKind::Vulkan,
        "Linux ARM64 requires its own player template and device validation.");
    declare(PlatformFamily::Android, Architecture::Arm64, GraphicsBackendKind::Vulkan,
        AndroidCapabilityReason());
    declare(PlatformFamily::IOS, Architecture::Arm64, GraphicsBackendKind::Metal,
        IOSCapabilityReason());
    declare(PlatformFamily::Web, Architecture::Unknown, GraphicsBackendKind::WebGPU,
        "Web requires WebGPU, browser file access, an async main loop, and memory budgets.");
}
} // namespace NLS::Build
