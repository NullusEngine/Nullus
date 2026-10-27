#include "Assets/ContentSource.h"

#include "Assets/ArtifactManifest.h"
#include "Assets/NpakArchive.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <system_error>

namespace NLS::Core::Assets
{
namespace
{
std::string NormalizePathText(const std::filesystem::path& path)
{
    auto text = path.lexically_normal().generic_string();
    while (text.size() > 1u && text.back() == '/')
        text.pop_back();
    return text;
}

std::string FoldCaseForComparison(std::string text)
{
#if defined(_WIN32)
    std::transform(
        text.begin(),
        text.end(),
        text.begin(),
        [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
#else
    (void)text;
#endif
    return text;
}

bool IsHostPathUnderRoot(const std::string& normalizedPath, const std::string& mountRoot)
{
    if (mountRoot.empty())
        return true;
    const auto path = FoldCaseForComparison(normalizedPath);
    const auto root = FoldCaseForComparison(mountRoot);
    if (root.empty() || path.size() < root.size())
        return false;
    if (path.compare(0u, root.size(), root) != 0)
        return false;
    return path.size() == root.size() || path[root.size()] == '/';
}
} // namespace

bool IContentSource::ReadAll(
    const std::string_view path,
    std::vector<uint8_t>& destination,
    std::string* diagnostics) const
{
    const auto size = GetSize(path);
    if (!size.has_value())
    {
        if (diagnostics != nullptr)
            *diagnostics = "content: file not found '" + std::string(path) + "'";
        return false;
    }
    destination.resize(static_cast<size_t>(*size));
    if (*size == 0u)
        return true;
    return ReadAt(path, 0u, destination.data(), destination.size(), diagnostics);
}

bool IContentSource::ExistsAndReadAll(
    const std::string_view path,
    std::vector<uint8_t>& destination,
    std::string* diagnostics) const
{
    if (!Exists(path))
        return false;
    return ReadAll(path, destination, diagnostics);
}

DirectoryContentSource::DirectoryContentSource(std::filesystem::path root)
    : m_root(std::move(root))
{
}

std::filesystem::path DirectoryContentSource::ResolveHostPath(const std::string_view path) const
{
    if (path.empty())
        return {};
    const std::filesystem::path relative{std::string(path)};
    if (relative.is_absolute() || relative.has_root_name() || relative.has_root_directory())
        return {};
    for (const auto& part : relative)
    {
        if (part == "..")
            return {};
    }
    return m_root / relative;
}

bool DirectoryContentSource::Exists(const std::string_view path) const
{
    const auto hostPath = ResolveHostPath(path);
    if (hostPath.empty())
        return false;
    std::error_code error;
    return std::filesystem::is_regular_file(hostPath, error);
}

std::optional<uint64_t> DirectoryContentSource::GetSize(const std::string_view path) const
{
    const auto hostPath = ResolveHostPath(path);
    if (hostPath.empty())
        return std::nullopt;
    std::error_code error;
    const auto size = std::filesystem::file_size(hostPath, error);
    if (error)
        return std::nullopt;
    return size;
}

bool DirectoryContentSource::ReadAt(
    const std::string_view path,
    const uint64_t offset,
    void* destination,
    const size_t byteCount,
    std::string* diagnostics) const
{
    const auto hostPath = ResolveHostPath(path);
    if (hostPath.empty())
    {
        if (diagnostics != nullptr)
            *diagnostics = "content: invalid path '" + std::string(path) + "'";
        return false;
    }
    std::ifstream stream(hostPath, std::ios::binary);
    if (!stream)
    {
        if (diagnostics != nullptr)
            *diagnostics = "content: cannot open '" + hostPath.string() + "'";
        return false;
    }
    stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!stream)
    {
        if (diagnostics != nullptr)
            *diagnostics = "content: seek failed for '" + hostPath.string() + "'";
        return false;
    }
    if (byteCount > 0u)
    {
        stream.read(reinterpret_cast<char*>(destination), static_cast<std::streamsize>(byteCount));
        if (stream.gcount() != static_cast<std::streamsize>(byteCount))
        {
            if (diagnostics != nullptr)
                *diagnostics = "content: short read for '" + hostPath.string() + "'";
            return false;
        }
    }
    return true;
}

PackageContentSource::PackageContentSource(std::shared_ptr<const NpakArchive> archive)
    : m_archive(std::move(archive))
{
}

bool PackageContentSource::Exists(const std::string_view path) const
{
    return m_archive != nullptr && m_archive->Exists(path);
}

std::optional<uint64_t> PackageContentSource::GetSize(const std::string_view path) const
{
    if (m_archive == nullptr)
        return std::nullopt;
    return m_archive->Size(path);
}

bool PackageContentSource::ReadAt(
    const std::string_view path,
    const uint64_t offset,
    void* destination,
    const size_t byteCount,
    std::string* diagnostics) const
{
    if (m_archive == nullptr)
    {
        if (diagnostics != nullptr)
            *diagnostics = "content: package not mounted";
        return false;
    }
    return m_archive->ReadAt(path, offset, destination, byteCount, diagnostics);
}

ContentSourceTable& ContentSourceTable::Get()
{
    static ContentSourceTable table;
    return table;
}

void ContentSourceTable::Mount(std::string hostRoot, std::shared_ptr<const IContentSource> source)
{
    if (source == nullptr)
        return;
    auto normalizedRoot = NormalizePathText(std::filesystem::path(hostRoot));
    std::lock_guard lock(m_mutex);
    for (auto& mount : m_mounts)
    {
        if (mount.first == normalizedRoot)
        {
            mount.second = std::move(source);
            return;
        }
    }
    m_mounts.emplace_back(std::move(normalizedRoot), std::move(source));
}

void ContentSourceTable::Clear()
{
    std::lock_guard lock(m_mutex);
    m_mounts.clear();
}

bool ContentSourceTable::IsEmpty() const
{
    std::lock_guard lock(m_mutex);
    return m_mounts.empty();
}

size_t ContentSourceTable::GetMountCount() const
{
    std::lock_guard lock(m_mutex);
    return m_mounts.size();
}

std::vector<std::string> ContentSourceTable::GetMountRoots() const
{
    std::lock_guard lock(m_mutex);
    std::vector<std::string> roots;
    roots.reserve(m_mounts.size());
    for (const auto& mount : m_mounts)
        roots.push_back(mount.first);
    return roots;
}

std::optional<ContentSourceTable::Resolution> ContentSourceTable::Resolve(
    const std::filesystem::path& hostPath) const
{
    const auto normalized = NormalizePathText(hostPath);
    if (normalized.empty())
        return std::nullopt;

    std::lock_guard lock(m_mutex);
    const std::pair<std::string, std::shared_ptr<const IContentSource>>* best = nullptr;
    for (const auto& mount : m_mounts)
    {
        if (!IsHostPathUnderRoot(normalized, mount.first))
            continue;
        if (best == nullptr || mount.first.size() > best->first.size())
            best = &mount;
    }
    if (best == nullptr)
        return std::nullopt;

    std::string relative;
    if (best->first.empty())
    {
        relative = normalized;
    }
    else
    {
        relative = normalized.size() > best->first.size()
            ? normalized.substr(best->first.size() + 1u)
            : std::string {};
    }
    if (relative.empty())
        return std::nullopt;

    Resolution resolution;
    resolution.source = best->second;
    resolution.relativePath = std::move(relative);
    resolution.mountRoot = best->first;
    return resolution;
}

std::optional<ContentSourceTable::Resolution> ContentSourceTable::ResolveContentPath(
    const std::filesystem::path& hostOrLogicalPath) const
{
    if (hostOrLogicalPath.empty())
        return std::nullopt;

    if (auto resolved = Resolve(hostOrLogicalPath); resolved.has_value())
        return resolved;

    // Runtime manifests may carry the portable artifact path
    // ("Library/Artifacts/<shard>/<hash>") or the packaged artifact key
    // ("Artifacts/<shard>/<hash>"). Try both before giving up.
    const auto portable = TryMakePortableContentArtifactPath(hostOrLogicalPath.generic_string());
    if (!portable.empty())
    {
        if (auto resolved = Resolve(std::filesystem::path(portable)); resolved.has_value())
            return resolved;
    }
    if (const auto normalized = NormalizePathText(hostOrLogicalPath);
        normalized.rfind("Artifacts/", 0u) == 0u)
    {
        if (auto resolved = Resolve(std::filesystem::path(normalized)); resolved.has_value())
            return resolved;
    }

    // Relative logical paths ("Library/Artifacts/...") are resolved against
    // every mount root. This is the path form runtime manifests carry, and it
    // lets callers avoid rebuilding an absolute host path first.
    if (!hostOrLogicalPath.is_absolute() && !hostOrLogicalPath.has_root_name())
    {
        std::vector<std::string> roots;
        {
            std::lock_guard lock(m_mutex);
            roots.reserve(m_mounts.size());
            for (const auto& mount : m_mounts)
                roots.push_back(mount.first);
        }
        for (const auto& root : roots)
        {
            if (root.empty())
                continue;
            if (auto resolved = Resolve(std::filesystem::path(root) / hostOrLogicalPath);
                resolved.has_value())
            {
                return resolved;
            }
        }
    }
    return std::nullopt;
}

bool ContentSourceTable::Exists(const std::filesystem::path& hostOrLogicalPath) const
{
    const auto resolved = ResolveContentPath(hostOrLogicalPath);
    return resolved.has_value() && resolved->source->Exists(resolved->relativePath);
}

std::optional<uint64_t> ContentSourceTable::GetSize(const std::filesystem::path& hostOrLogicalPath) const
{
    const auto resolved = ResolveContentPath(hostOrLogicalPath);
    if (!resolved.has_value())
        return std::nullopt;
    return resolved->source->GetSize(resolved->relativePath);
}

bool ContentSourceTable::ReadAt(
    const std::filesystem::path& hostOrLogicalPath,
    const uint64_t offset,
    void* destination,
    const size_t byteCount,
    std::string* diagnostics) const
{
    const auto resolved = ResolveContentPath(hostOrLogicalPath);
    if (!resolved.has_value())
        return false;
    return resolved->source->ReadAt(resolved->relativePath, offset, destination, byteCount, diagnostics);
}

bool ContentSourceTable::ReadAll(
    const std::filesystem::path& hostOrLogicalPath,
    std::vector<uint8_t>& destination,
    std::string* diagnostics) const
{
    const auto resolved = ResolveContentPath(hostOrLogicalPath);
    if (!resolved.has_value())
        return false;
    return resolved->source->ReadAll(resolved->relativePath, destination, diagnostics);
}

bool MountContentPackage(
    const std::filesystem::path& archivePath,
    const std::filesystem::path& hostRoot,
    std::string* diagnostics)
{
    auto archive = NpakArchive::Open(archivePath, diagnostics);
    if (!archive.has_value())
        return false;
    ContentSourceTable::Get().Mount(
        NormalizePathText(hostRoot),
        std::make_shared<const PackageContentSource>(
            std::make_shared<const NpakArchive>(std::move(*archive))));
    return true;
}

bool MountContentDirectory(
    const std::filesystem::path& directory,
    const std::filesystem::path& hostRoot,
    std::string* diagnostics)
{
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error))
    {
        if (diagnostics != nullptr)
            *diagnostics = "content: directory missing '" + directory.string() + "'";
        return false;
    }
    ContentSourceTable::Get().Mount(
        NormalizePathText(hostRoot),
        std::make_shared<const DirectoryContentSource>(directory));
    return true;
}

void ClearContentMounts()
{
    ContentSourceTable::Get().Clear();
}

bool HasContentMounts()
{
    return !ContentSourceTable::Get().IsEmpty();
}

std::string NormalizeContentPath(const std::filesystem::path& path)
{
    return NormalizePathText(path);
}
} // namespace NLS::Core::Assets
