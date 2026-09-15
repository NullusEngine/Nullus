#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace NLS::Editor::Undo
{
// History commands keep this value object instead of a file path. Its storage
// key is a SHA-256 digest and can only be created by UndoBlobStore.
class UndoBlobReference final
{
public:
    UndoBlobReference() = default;

    [[nodiscard]] bool IsValid() const noexcept { return !m_storageKey.empty(); }
    [[nodiscard]] std::string_view GetStorageKey() const noexcept { return m_storageKey; }
    [[nodiscard]] uint64_t GetByteCount() const noexcept { return m_byteCount; }
    [[nodiscard]] bool operator==(const UndoBlobReference& other) const noexcept = default;

private:
    explicit UndoBlobReference(std::string storageKey, const uint64_t byteCount)
        : m_storageKey(std::move(storageKey))
        , m_byteCount(byteCount)
    {
    }

    std::string m_storageKey;
    uint64_t m_byteCount = 0u;

    friend class UndoBlobStore;
};

enum class UndoBlobStoreStatus : uint8_t
{
    Success,
    InvalidArgument,
    IoError,
    NotFound,
    Corrupt,
    NotRetained,
    ReferenceUnderflow
};

struct UndoBlobStoreResult
{
    UndoBlobStoreStatus status = UndoBlobStoreStatus::Success;
    std::string message;

    [[nodiscard]] bool Succeeded() const noexcept { return status == UndoBlobStoreStatus::Success; }
};

struct UndoBlobStoreWriteResult : UndoBlobStoreResult
{
    UndoBlobReference reference;
    bool deduplicated = false;
};

struct UndoBlobStoreLoadResult : UndoBlobStoreResult
{
    std::vector<uint8_t> bytes;
};

struct UndoBlobStoreCleanupResult : UndoBlobStoreResult
{
    size_t removedBlobCount = 0u;
    uint64_t removedByteCount = 0u;
};

struct UndoBlobStoreStatistics
{
    // Physical data tracked below the configured root. A zero-reference blob
    // remains counted until CleanupUnreferenced succeeds.
    size_t blobCount = 0u;
    uint64_t storedByteCount = 0u;
    size_t referencedBlobCount = 0u;
    uint64_t referencedByteCount = 0u;
    uint64_t logicalRetainedByteCount = 0u;
    uint64_t totalReferenceCount = 0u;
};

struct UndoBlobStoreOptions
{
    // The owner supplies a project-local path such as Library/Editor/UndoBlobs.
    // An empty root is rejected at the first filesystem operation.
    std::filesystem::path root;

    // The command layer uses this as its default decision point. Store() still
    // accepts any size so callers can explicitly spill smaller snapshots.
    uint64_t spillThresholdBytes = 1u * 1024u * 1024u;
};

// A content-addressed store for snapshots too large to keep inside undo
// commands. Store() acquires one reference. Copying UndoBlobReference does not
// acquire another reference; callers must pair every ownership transfer with
// Retain() and Release(). The store is safe to call from multiple threads.
class UndoBlobStore final
{
public:
    explicit UndoBlobStore(UndoBlobStoreOptions options);
    ~UndoBlobStore() = default;

    UndoBlobStore(const UndoBlobStore&) = delete;
    UndoBlobStore& operator=(const UndoBlobStore&) = delete;

    [[nodiscard]] UndoBlobStoreResult Initialize();
    [[nodiscard]] UndoBlobStoreWriteResult Store(std::span<const uint8_t> bytes);
    [[nodiscard]] UndoBlobStoreWriteResult Store(const std::vector<uint8_t>& bytes)
    {
        return Store(std::span<const uint8_t>(bytes));
    }
    [[nodiscard]] UndoBlobStoreResult Retain(const UndoBlobReference& reference);
    [[nodiscard]] UndoBlobStoreResult Release(const UndoBlobReference& reference);
    [[nodiscard]] UndoBlobStoreLoadResult Load(const UndoBlobReference& reference) const;
    [[nodiscard]] UndoBlobStoreCleanupResult CleanupUnreferenced();

    [[nodiscard]] bool ShouldStoreOutOfLine(const size_t byteCount) const noexcept;
    [[nodiscard]] const std::filesystem::path& GetRoot() const noexcept { return m_options.root; }
    [[nodiscard]] UndoBlobStoreStatistics GetStatistics() const;

private:
    struct BlobEntry
    {
        uint64_t byteCount = 0u;
        uint64_t referenceCount = 0u;
    };

    [[nodiscard]] UndoBlobStoreResult EnsureInitializedLocked();
    [[nodiscard]] UndoBlobStoreResult ValidateReferenceLocked(const UndoBlobReference& reference) const;
    [[nodiscard]] UndoBlobStoreResult ValidateBlobFileLocked(
        const std::string& storageKey,
        uint64_t expectedByteCount) const;
    [[nodiscard]] std::filesystem::path BlobPathForKey(const std::string& storageKey) const;

    UndoBlobStoreOptions m_options;
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, BlobEntry> m_entries;
    bool m_initialized = false;
};
} // namespace NLS::Editor::Undo
