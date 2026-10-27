#include "BuildFileUtils.h"

#include <algorithm>
#include <fstream>
#include <functional>
#include <sstream>
#include <system_error>

namespace NLS::Build
{
std::vector<std::filesystem::path> EnumerateFiles(const std::filesystem::path& root, const bool recursive)
{
    std::vector<std::filesystem::path> files;
    std::error_code error;
    if (!std::filesystem::is_directory(root, error))
        return files;
    if (recursive)
    {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root, error))
        {
            if (error)
                break;
            if (entry.is_regular_file())
                files.push_back(entry.path());
        }
    }
    else
    {
        for (const auto& entry : std::filesystem::directory_iterator(root, error))
        {
            if (error)
                break;
            if (entry.is_regular_file())
                files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::vector<DirectoryFileInfo> EnumerateFilesWithSize(const std::filesystem::path& root, const bool recursive)
{
    std::vector<DirectoryFileInfo> files;
    for (const auto& path : EnumerateFiles(root, recursive))
    {
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        if (error)
            continue;
        files.push_back({path, size});
    }
    return files;
}

bool EnsureDirectory(const std::filesystem::path& directory, std::string* error)
{
    std::error_code directoryError;
    std::filesystem::create_directories(directory, directoryError);
    if (directoryError)
    {
        if (error != nullptr)
            *error = "cannot create directory '" + directory.string() + "'";
        return false;
    }
    return true;
}

void RemoveDirectoryIfExists(const std::filesystem::path& directory)
{
    std::error_code error;
    std::filesystem::remove_all(directory, error);
}

bool CopyFileEnsuringDirectory(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    std::string* error)
{
    if (!EnsureDirectory(destination.parent_path(), error))
        return false;
    std::error_code copyError;
    std::filesystem::copy_file(
        source,
        destination,
        std::filesystem::copy_options::overwrite_existing,
        copyError);
    if (copyError)
    {
        if (error != nullptr)
            *error = "cannot copy '" + source.string() + "' to '" + destination.string() + "'";
        return false;
    }
    return true;
}

bool CopyFileIfChanged(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    bool* copied,
    std::string* error)
{
    if (copied != nullptr)
        *copied = false;
    std::error_code sourceError;
    const auto sourceSize = std::filesystem::file_size(source, sourceError);
    if (sourceError)
    {
        if (error != nullptr)
            *error = "source file missing '" + source.string() + "'";
        return false;
    }
    std::error_code destinationError;
    const auto destinationSize = std::filesystem::file_size(destination, destinationError);
    if (!destinationError && destinationSize == sourceSize)
    {
        const auto sourceTime = std::filesystem::last_write_time(source, sourceError);
        const auto destinationTime = std::filesystem::last_write_time(destination, destinationError);
        if (!sourceError && !destinationError && sourceTime == destinationTime)
            return true;
    }
    if (!CopyFileEnsuringDirectory(source, destination, error))
        return false;
    if (copied != nullptr)
        *copied = true;
    return true;
}

bool CopyTreeFiltered(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    const std::function<bool(const std::string&)>& predicate,
    size_t* copiedCount,
    std::string* error)
{
    if (copiedCount != nullptr)
        *copiedCount = 0u;
    for (const auto& file : EnumerateFiles(source, true))
    {
        std::error_code relativeError;
        const auto relative = std::filesystem::relative(file, source, relativeError);
        if (relativeError)
            continue;
        const auto relativeText = relative.generic_string();
        if (predicate && !predicate(relativeText))
            continue;
        if (!CopyFileEnsuringDirectory(file, destination / relative, error))
            return false;
        if (copiedCount != nullptr)
            ++(*copiedCount);
    }
    return true;
}

std::string NormalizePortablePath(const std::filesystem::path& path)
{
    return path.lexically_normal().generic_string();
}

std::string ToForwardSlashes(std::string value)
{
    std::replace(value.begin(), value.end(), '\\', '/');
    while (value.size() > 1u && value.back() == '/')
        value.pop_back();
    return value;
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
} // namespace NLS::Build
