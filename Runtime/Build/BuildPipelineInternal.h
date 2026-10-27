#pragma once

#include "BuildTypes.h"

#include <filesystem>
#include <string>

namespace NLS::Build
{
namespace Detail
{
// The editor's build-profile panel consumes the host detection through the
// NLS_Build shared library, so these three helpers cross the module boundary.
NLS_BUILD_API PlatformFamily DetectHostPlatform();
NLS_BUILD_API Architecture DetectHostArchitecture();
NLS_BUILD_API GraphicsBackendKind DetectHostBackend();
std::string DefaultShaderTarget(PlatformFamily platform);
std::string DefaultMinimumOSVersion(PlatformFamily platform);

// Minimal INI helpers for the project settings document (.nullus is an INI).
std::string ReadIniValue(const std::string& iniText, const std::string& key, const std::string& fallback);
std::string SetIniValue(std::string iniText, const std::string& key, const std::string& value);

// Deterministic listings: relative path, size, and last-write time.
std::string HashDirectoryListing(const std::filesystem::path& root, bool recursive);
std::string ReadTextFileOrEmpty(const std::filesystem::path& path);
} // namespace Detail
} // namespace NLS::Build
