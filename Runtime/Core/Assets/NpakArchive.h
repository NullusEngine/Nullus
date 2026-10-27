#pragma once

#include "CoreDef.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace NLS::Core::Assets
{
// Versioned, deterministic content container for shipped games.
//
// The container stores native artifact bytes unchanged. Every file is split
// into fixed-size blocks; each block is compressed independently so the
// runtime can answer range reads without unpacking the whole package. Identical
// file contents are stored once and shared by every referencing entry.
//
// Determinism: entries are sorted by logical path, blocks follow that order,
// and the index never records host paths or build timestamps, so the same
// inputs produce byte-identical packages.
constexpr uint32_t NpakFormatVersion = 1u;

struct NpakWriteOptions
{
    // Block size for independent compression. 256 KiB balances seek latency
    // and compression ratio for artifact payloads.
    uint32_t blockSize = 256u * 1024u;
    // Zstandard level. The format is pinned by NpkContainerVersion.
    int compressionLevel = 3;
    // Storing incompressible blocks is always allowed; disabling compression
    // forces a store-only package (used by validation tooling).
    bool enableCompression = true;
};

struct NpakEntry
{
    std::string path;
    uint64_t size = 0u;
    uint64_t dataOffset = 0u;
    uint64_t firstBlock = 0u;
    uint32_t blockCount = 0u;
    std::array<uint8_t, 32u> contentHash {};
};

struct NpakArchiveInfo
{
    uint16_t formatVersion = 0u;
    uint32_t codecId = 0u;
    uint32_t codecVersion = 0u;
    uint32_t blockSize = 0u;
    uint64_t fileCount = 0u;
    uint64_t blockCount = 0u;
    uint64_t totalSize = 0u;
    uint64_t storedBytes = 0u;
    uint64_t rawBytes = 0u;
    std::string codecName;
};

struct NpakCacheStats
{
    uint64_t hits = 0u;
    uint64_t misses = 0u;
    uint64_t evictions = 0u;
    uint64_t decodedBlocks = 0u;
    uint64_t cachedBytes = 0u;
};

enum class NpakCodec : uint32_t
{
    Store = 1u,
    Zstandard = 2u
};

// Read-only package handle. Opening validates the header, index bounds, and
// index checksum before any file is served.
class NLS_CORE_API NpakArchive
{
public:
    NpakArchive() = default;
    ~NpakArchive() = default;
    NpakArchive(NpakArchive&& other) noexcept;
    NpakArchive& operator=(NpakArchive&& other) noexcept;
    NpakArchive(const NpakArchive&) = delete;
    NpakArchive& operator=(const NpakArchive&) = delete;

    static std::optional<NpakArchive> Open(
        const std::filesystem::path& path,
        std::string* diagnostics = nullptr);

    bool Exists(std::string_view path) const;
    const NpakEntry* Find(std::string_view path) const;
    const std::vector<NpakEntry>& GetEntries() const { return m_entries; }
    const NpakArchiveInfo& GetInfo() const { return m_info; }
    const std::filesystem::path& GetPath() const { return m_path; }

    std::optional<uint64_t> Size(std::string_view path) const;
    bool ReadAt(
        std::string_view path,
        uint64_t offset,
        void* destination,
        size_t byteCount,
        std::string* diagnostics = nullptr) const;
    bool ReadAt(
        const NpakEntry& entry,
        uint64_t offset,
        void* destination,
        size_t byteCount,
        std::string* diagnostics = nullptr) const;
    bool ReadAll(
        std::string_view path,
        std::vector<uint8_t>& destination,
        std::string* diagnostics = nullptr) const;

    // Re-reads every block of the entry and compares it with the recorded
    // content hash. Used by validation tooling and tests.
    bool VerifyPayload(std::string_view path, std::string* diagnostics = nullptr) const;

    void SetBlockCacheBudget(uint64_t bytes);
    uint64_t GetBlockCacheBudget() const { return m_cacheBudgetBytes; }
    void ClearBlockCache();
    NpakCacheStats GetCacheStats() const;

private:
    struct BlockRecord
    {
        uint64_t offset = 0u;
        uint32_t storedSize = 0u;
        uint32_t rawSize = 0u;
        uint32_t codec = 0u;
        uint32_t checksum = 0u;
    };

    bool Load(const std::filesystem::path& path, std::string* diagnostics);
    bool DecodeBlock(uint64_t blockIndex, std::shared_ptr<const std::vector<uint8_t>>& decodedStorage) const;
    std::shared_ptr<const std::vector<uint8_t>> TryGetCached(uint64_t blockIndex) const;

    std::filesystem::path m_path;
    NpakArchiveInfo m_info;
    std::vector<NpakEntry> m_entries;
    std::unordered_map<std::string, size_t> m_entryIndex;
    std::vector<BlockRecord> m_blocks;

    mutable std::mutex m_ioMutex;
    mutable std::ifstream m_stream;

    mutable std::mutex m_cacheMutex;
    mutable std::unordered_map<uint64_t, std::shared_ptr<const std::vector<uint8_t>>> m_cache;
    mutable std::unordered_map<uint64_t, uint64_t> m_cacheStamp;
    mutable uint64_t m_cacheClock = 0u;
    mutable NpakCacheStats m_cacheStats;
    mutable uint64_t m_cacheBudgetBytes = 8u * 1024u * 1024u;
};

// Streaming writer. Files may be added from memory or from disk; disk sources
// are hashed and then streamed block by block so large textures do not need to
// be resident. Write() commits through a temporary file and renames on success,
// so a failed or cancelled build never destroys an existing package.
class NLS_CORE_API NpakWriter
{
public:
    bool AddFile(
        std::string archivePath,
        std::span<const uint8_t> bytes,
        std::string* diagnostics = nullptr);
    bool AddFileFromDisk(
        std::string archivePath,
        const std::filesystem::path& source,
        std::string* diagnostics = nullptr);

    // Reports: invalid or duplicate path, unreadable source, unwritable target.
    bool Write(
        const std::filesystem::path& destination,
        const NpakWriteOptions& options = {},
        std::string* diagnostics = nullptr);

    size_t GetAddedFileCount() const { return m_entries.size(); }
    size_t GetStoredFileCount() const;
    size_t GetDeduplicatedFileCount() const;
    void Clear();

private:
    struct SourceEntry
    {
        std::string path;
        std::vector<uint8_t> bytes;
        std::filesystem::path sourceFile;
        bool fromDisk = false;
    };

    std::vector<SourceEntry> m_entries;
};

// Shared helpers.
NLS_CORE_API bool IsValidNpakPath(const std::string& path);
NLS_CORE_API uint32_t NpakChecksum(const uint8_t* bytes, size_t byteCount);
NLS_CORE_API uint32_t NpakChecksum(std::span<const uint8_t> bytes);
NLS_CORE_API const char* NpakCodecName(uint32_t codecId);
} // namespace NLS::Core::Assets
