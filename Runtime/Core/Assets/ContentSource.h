#pragma once

#include "CoreDef.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace NLS::Core::Assets
{
class NpakArchive;

// Read-only content interface shared by directory installs and package
// installs. Paths are package-relative logical paths ("Artifacts/ab/<hash>").
// Implementations must be safe for concurrent readers.
class NLS_CORE_API IContentSource
{
public:
    virtual ~IContentSource() = default;

    virtual const char* GetTypeName() const = 0;
    virtual bool Exists(std::string_view path) const = 0;
    virtual std::optional<uint64_t> GetSize(std::string_view path) const = 0;
    virtual bool ReadAt(
        std::string_view path,
        uint64_t offset,
        void* destination,
        size_t byteCount,
        std::string* diagnostics) const = 0;

    bool ReadAll(
        std::string_view path,
        std::vector<uint8_t>& destination,
        std::string* diagnostics = nullptr) const;
    bool ExistsAndReadAll(
        std::string_view path,
        std::vector<uint8_t>& destination,
        std::string* diagnostics = nullptr) const;
};

// Unpacked directory tree. Used for editor installs, loose raw assets, and
// development runs.
class NLS_CORE_API DirectoryContentSource final : public IContentSource
{
public:
    explicit DirectoryContentSource(std::filesystem::path root);

    const std::filesystem::path& GetRoot() const { return m_root; }

    const char* GetTypeName() const override { return "directory"; }
    bool Exists(std::string_view path) const override;
    std::optional<uint64_t> GetSize(std::string_view path) const override;
    bool ReadAt(
        std::string_view path,
        uint64_t offset,
        void* destination,
        size_t byteCount,
        std::string* diagnostics) const override;

private:
    std::filesystem::path ResolveHostPath(std::string_view path) const;

    std::filesystem::path m_root;
};

// Package-backed content. Range reads decode only the touched blocks.
class NLS_CORE_API PackageContentSource final : public IContentSource
{
public:
    explicit PackageContentSource(std::shared_ptr<const NpakArchive> archive);

    const std::shared_ptr<const NpakArchive>& GetArchive() const { return m_archive; }

    const char* GetTypeName() const override { return "package"; }
    bool Exists(std::string_view path) const override;
    std::optional<uint64_t> GetSize(std::string_view path) const override;
    bool ReadAt(
        std::string_view path,
        uint64_t offset,
        void* destination,
        size_t byteCount,
        std::string* diagnostics) const override;

private:
    std::shared_ptr<const NpakArchive> m_archive;
};

// Global mount table used by the runtime and the packaging tools.
//
// Mounts map an absolute host root (for example the install directory or the
// packaged data root) onto a content source. Resolution picks the longest
// matching host root. On Windows the comparison is case-insensitive because
// drive-letter casing is not stable across tools.
class NLS_CORE_API ContentSourceTable
{
public:
    static ContentSourceTable& Get();

    struct Resolution
    {
        std::shared_ptr<const IContentSource> source;
        std::string relativePath;
        std::string mountRoot;
    };

    void Mount(std::string hostRoot, std::shared_ptr<const IContentSource> source);
    void Clear();
    bool IsEmpty() const;
    size_t GetMountCount() const;
    std::vector<std::string> GetMountRoots() const;

    std::optional<Resolution> Resolve(const std::filesystem::path& hostPath) const;
    // Resolves either an absolute host path or a logical content path such as
    // "Artifacts/ab/<hash>" that a runtime manifest may carry.
    std::optional<Resolution> ResolveContentPath(const std::filesystem::path& hostOrLogicalPath) const;

    bool Exists(const std::filesystem::path& hostOrLogicalPath) const;
    std::optional<uint64_t> GetSize(const std::filesystem::path& hostOrLogicalPath) const;
    bool ReadAt(
        const std::filesystem::path& hostOrLogicalPath,
        uint64_t offset,
        void* destination,
        size_t byteCount,
        std::string* diagnostics = nullptr) const;
    bool ReadAll(
        const std::filesystem::path& hostOrLogicalPath,
        std::vector<uint8_t>& destination,
        std::string* diagnostics = nullptr) const;

private:
    ContentSourceTable() = default;

    mutable std::mutex m_mutex;
    std::vector<std::pair<std::string, std::shared_ptr<const IContentSource>>> m_mounts;
};

// Convenience helpers for the player runtime and packaging tools.
NLS_CORE_API bool MountContentPackage(
    const std::filesystem::path& archivePath,
    const std::filesystem::path& hostRoot,
    std::string* diagnostics = nullptr);
NLS_CORE_API bool MountContentDirectory(
    const std::filesystem::path& directory,
    const std::filesystem::path& hostRoot,
    std::string* diagnostics = nullptr);
NLS_CORE_API void ClearContentMounts();
NLS_CORE_API bool HasContentMounts();
NLS_CORE_API std::string NormalizeContentPath(const std::filesystem::path& path);
} // namespace NLS::Core::Assets
