#include "BuildPipelineInternal.h"

#include "BuildFileUtils.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <sstream>

#include "Assets/ArtifactManifest.h"
namespace NLS::Build::Detail
{
PlatformFamily DetectHostPlatform()
{
#if defined(_WIN32)
    return PlatformFamily::Windows;
#elif defined(__APPLE__)
    return PlatformFamily::MacOS;
#elif defined(__linux__)
    return PlatformFamily::Linux;
#else
    return PlatformFamily::Unknown;
#endif
}

Architecture DetectHostArchitecture()
{
#if defined(_M_ARM64) || defined(__aarch64__)
    return Architecture::Arm64;
#else
    return Architecture::X64;
#endif
}

GraphicsBackendKind DetectHostBackend()
{
#if defined(_WIN32)
    return GraphicsBackendKind::DX12;
#elif defined(__APPLE__)
    return GraphicsBackendKind::Metal;
#elif defined(__linux__)
    return GraphicsBackendKind::Vulkan;
#else
    return GraphicsBackendKind::None;
#endif
}

std::string DefaultShaderTarget(const PlatformFamily platform)
{
    switch (platform)
    {
    case PlatformFamily::Windows: return "dxil";
    case PlatformFamily::Linux:
    case PlatformFamily::Android: return "spirv";
    case PlatformFamily::MacOS:
    case PlatformFamily::IOS: return "metal";
    case PlatformFamily::Web: return "wgsl";
    case PlatformFamily::Unknown: break;
    }
    return {};
}

std::string DefaultMinimumOSVersion(const PlatformFamily platform)
{
    switch (platform)
    {
    case PlatformFamily::Windows: return "10.0.19045"; // Windows 10 22H2
    case PlatformFamily::Linux: return "glibc-2.35";  // Ubuntu 22.04
    case PlatformFamily::MacOS: return "13.0";
    case PlatformFamily::Android: return "26";
    case PlatformFamily::IOS: return "16.0";
    case PlatformFamily::Web: break;
    case PlatformFamily::Unknown: break;
    }
    return {};
}

std::string ReadIniValue(const std::string& iniText, const std::string& key, const std::string& fallback)
{
    std::stringstream stream(iniText);
    std::string line;
    std::string result = fallback;
    while (std::getline(stream, line))
    {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        if (line.empty() || line.front() == ';' || line.front() == '#')
            continue;
        const auto separator = line.find('=');
        if (separator == std::string::npos)
            continue;
        auto name = line.substr(0u, separator);
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t'))
            name.pop_back();
        if (name != key)
            continue;
        auto value = line.substr(separator + 1u);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
            value.erase(value.begin());
        result = value;
    }
    return result;
}

std::string SetIniValue(std::string iniText, const std::string& key, const std::string& value)
{
    std::stringstream stream(iniText);
    std::string line;
    std::vector<std::string> lines;
    bool replaced = false;
    while (std::getline(stream, line))
    {
        while (!line.empty() && line.back() == '\r')
            line.pop_back();
        auto name = line.substr(0u, line.find('='));
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t'))
            name.pop_back();
        if (name == key && line.find('=') != std::string::npos)
        {
            lines.push_back(key + "=" + value);
            replaced = true;
            continue;
        }
        lines.push_back(line);
    }
    if (!replaced)
        lines.push_back(key + "=" + value);

    std::string result;
    for (const auto& entry : lines)
    {
        result += entry;
        result += "\n";
    }
    return result;
}

std::string HashDirectoryListing(const std::filesystem::path& root, const bool recursive,
    const std::string_view excludedRelativePath)
{
    NLS::Core::Assets::Sha256Hasher hasher;
    std::error_code error;
    if (!std::filesystem::is_directory(root, error))
    {
        const auto digest = hasher.Finalize();
        return NLS::Core::Assets::ToHexString(digest);
    }

    for (const auto& info : EnumerateFilesWithSize(root, recursive))
    {
        std::error_code relativeError;
        const auto relative = std::filesystem::relative(info.path, root, relativeError);
        if (relativeError || relative.generic_string() == excludedRelativePath)
            continue;
        std::error_code timeError;
        const auto writeTime = std::filesystem::last_write_time(info.path, timeError);
        const auto stamp = timeError ? 0ll : static_cast<long long>(writeTime.time_since_epoch().count());
        const auto line = relative.generic_string() + "|" + std::to_string(info.size) + "|" +
            std::to_string(stamp) + "\n";
        hasher.Update(reinterpret_cast<const uint8_t*>(line.data()), line.size());
    }
    return NLS::Core::Assets::ToHexString(hasher.Finalize());
}

std::string ReadTextFileOrEmpty(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    std::stringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}
} // namespace NLS::Build::Detail
