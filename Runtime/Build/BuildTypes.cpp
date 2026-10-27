#include "BuildTypes.h"

#include <algorithm>
#include <cctype>

namespace NLS::Build
{
namespace
{
std::string ToLower(std::string value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}
} // namespace

const char* ToString(const BuildMode value)
{
    switch (value)
    {
    case BuildMode::Development: return "Development";
    case BuildMode::Shipping: return "Shipping";
    }
    return "Development";
}

const char* ToString(const PlatformFamily value)
{
    switch (value)
    {
    case PlatformFamily::Windows: return "windows";
    case PlatformFamily::Linux: return "linux";
    case PlatformFamily::MacOS: return "macos";
    case PlatformFamily::Android: return "android";
    case PlatformFamily::IOS: return "ios";
    case PlatformFamily::Web: return "web";
    case PlatformFamily::Unknown: break;
    }
    return "unknown";
}

const char* ToString(const Architecture value)
{
    switch (value)
    {
    case Architecture::X64: return "x64";
    case Architecture::Arm64: return "arm64";
    case Architecture::Unknown: break;
    }
    return "unknown";
}

const char* ToString(const GraphicsBackendKind value)
{
    switch (value)
    {
    case GraphicsBackendKind::DX12: return "dx12";
    case GraphicsBackendKind::Vulkan: return "vulkan";
    case GraphicsBackendKind::Metal: return "metal";
    case GraphicsBackendKind::WebGPU: return "webgpu";
    case GraphicsBackendKind::OpenGL: return "opengl";
    case GraphicsBackendKind::None: break;
    }
    return "none";
}

const char* ToString(const OutputFormat value)
{
    switch (value)
    {
    case OutputFormat::Zip: return "zip";
    case OutputFormat::TarZst: return "tar.zst";
    case OutputFormat::AppImage: return "appimage";
    case OutputFormat::Dmg: return "dmg";
    case OutputFormat::Apk: return "apk";
    case OutputFormat::Aab: return "aab";
    case OutputFormat::Ipa: return "ipa";
    case OutputFormat::Directory: return "directory";
    }
    return "zip";
}

const char* ToString(const ScriptBackend value)
{
    switch (value)
    {
    case ScriptBackend::CSharp: return "csharp";
    case ScriptBackend::Lua: return "lua";
    }
    return "lua";
}

BuildMode BuildModeFromString(const std::string& value, const BuildMode fallback)
{
    const auto lowered = ToLower(value);
    if (lowered == "development" || lowered == "dev" || lowered == "debug")
        return BuildMode::Development;
    if (lowered == "shipping" || lowered == "release")
        return BuildMode::Shipping;
    return fallback;
}

PlatformFamily PlatformFamilyFromString(const std::string& value, const PlatformFamily fallback)
{
    const auto lowered = ToLower(value);
    if (lowered == "windows" || lowered == "win" || lowered == "win64")
        return PlatformFamily::Windows;
    if (lowered == "linux")
        return PlatformFamily::Linux;
    if (lowered == "macos" || lowered == "mac" || lowered == "osx")
        return PlatformFamily::MacOS;
    if (lowered == "android")
        return PlatformFamily::Android;
    if (lowered == "ios")
        return PlatformFamily::IOS;
    if (lowered == "web" || lowered == "wasm")
        return PlatformFamily::Web;
    return fallback;
}

Architecture ArchitectureFromString(const std::string& value, const Architecture fallback)
{
    const auto lowered = ToLower(value);
    if (lowered == "x64" || lowered == "x86_64" || lowered == "amd64")
        return Architecture::X64;
    if (lowered == "arm64" || lowered == "aarch64")
        return Architecture::Arm64;
    return fallback;
}

GraphicsBackendKind GraphicsBackendKindFromString(
    const std::string& value,
    const GraphicsBackendKind fallback)
{
    const auto lowered = ToLower(value);
    if (lowered == "dx12" || lowered == "d3d12")
        return GraphicsBackendKind::DX12;
    if (lowered == "vulkan" || lowered == "vk")
        return GraphicsBackendKind::Vulkan;
    if (lowered == "metal")
        return GraphicsBackendKind::Metal;
    if (lowered == "webgpu")
        return GraphicsBackendKind::WebGPU;
    if (lowered == "opengl" || lowered == "gl")
        return GraphicsBackendKind::OpenGL;
    if (lowered == "none")
        return GraphicsBackendKind::None;
    return fallback;
}

OutputFormat OutputFormatFromString(const std::string& value, const OutputFormat fallback)
{
    const auto lowered = ToLower(value);
    if (lowered == "zip")
        return OutputFormat::Zip;
    if (lowered == "tar.zst" || lowered == "tarzst" || lowered == "tarball")
        return OutputFormat::TarZst;
    if (lowered == "appimage")
        return OutputFormat::AppImage;
    if (lowered == "dmg")
        return OutputFormat::Dmg;
    if (lowered == "apk")
        return OutputFormat::Apk;
    if (lowered == "aab")
        return OutputFormat::Aab;
    if (lowered == "ipa")
        return OutputFormat::Ipa;
    if (lowered == "directory" || lowered == "folder")
        return OutputFormat::Directory;
    return fallback;
}

ScriptBackend ScriptBackendFromString(const std::string& value, const ScriptBackend fallback)
{
    const auto lowered = ToLower(value);
    if (lowered == "csharp" || lowered == "c#" || lowered == "dotnet")
        return ScriptBackend::CSharp;
    if (lowered == "lua")
        return ScriptBackend::Lua;
    return fallback;
}

std::string DefaultArtifactPlatform(const PlatformFamily platform)
{
    switch (platform)
    {
    case PlatformFamily::Windows: return "win64";
    case PlatformFamily::Linux: return "linux";
    case PlatformFamily::MacOS: return "macos";
    case PlatformFamily::Android: return "android";
    case PlatformFamily::IOS: return "ios";
    case PlatformFamily::Web: return "web";
    case PlatformFamily::Unknown: break;
    }
    return "unknown";
}

std::string BuildTargetId(const BuildTarget& target)
{
    std::string id = ToString(target.platform);
    id += "-";
    id += ToString(target.architecture);
    id += "-";
    id += ToString(target.graphicsBackend);
    return id;
}

std::string BuildModeConfiguration(const BuildMode mode)
{
    // Development keeps debug symbols and developer connections under the
    // optimized RelWithDebInfo configuration; Shipping uses Release.
    return mode == BuildMode::Shipping ? "Release" : "RelWithDebInfo";
}
} // namespace NLS::Build
