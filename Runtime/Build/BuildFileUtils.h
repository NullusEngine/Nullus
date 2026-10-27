#pragma once

#include "BuildDef.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace NLS::Build
{
struct DirectoryFileInfo
{
    std::filesystem::path path;
    uint64_t size = 0u;
};

NLS_BUILD_API std::vector<std::filesystem::path> EnumerateFiles(
    const std::filesystem::path& root,
    bool recursive = true);

NLS_BUILD_API std::vector<DirectoryFileInfo> EnumerateFilesWithSize(
    const std::filesystem::path& root,
    bool recursive = true);

NLS_BUILD_API bool EnsureDirectory(const std::filesystem::path& directory, std::string* error = nullptr);
NLS_BUILD_API void RemoveDirectoryIfExists(const std::filesystem::path& directory);
NLS_BUILD_API bool CopyFileEnsuringDirectory(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    std::string* error = nullptr);
NLS_BUILD_API bool CopyFileIfChanged(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    bool* copied = nullptr,
    std::string* error = nullptr);
// Copies only the files matching the predicate (relative path text).
NLS_BUILD_API bool CopyTreeFiltered(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    const std::function<bool(const std::string&)>& predicate,
    size_t* copiedCount = nullptr,
    std::string* error = nullptr);
NLS_BUILD_API std::string NormalizePortablePath(const std::filesystem::path& path);
// Replaces '\\' with '/' and strips trailing separators.
NLS_BUILD_API std::string ToForwardSlashes(std::string value);
NLS_BUILD_API std::string ReadTextFileOrEmpty(const std::filesystem::path& path);
} // namespace NLS::Build
