#include "Undo/UndoBlobStore.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <fstream>
#include <limits>
#include <mutex>
#include <system_error>
#include <unordered_set>
#include <utility>

#include "Assets/ArtifactManifest.h"

namespace NLS::Editor::Undo
{
namespace
{
constexpr size_t kStorageKeyLength = 64u;
constexpr size_t kStorageShardLength = 2u;
constexpr size_t kWriteChunkBytes = 256u * 1024u;

std::atomic_uint64_t g_temporarySequence = 1u;

[[nodiscard]] UndoBlobStoreResult MakeResult(
    const UndoBlobStoreStatus status,
    std::string message = {})
{
    return {status, std::move(message)};
}

[[nodiscard]] bool IsLowerHexStorageKey(const std::string_view key)
{
    if (key.size() != kStorageKeyLength)
        return false;

    return std::all_of(key.begin(), key.end(), [](const unsigned char character)
    {
        return (character >= '0' && character <= '9') ||
            (character >= 'a' && character <= 'f');
    });
}

[[nodiscard]] bool IsManagedBlobPath(
    const std::filesystem::path& root,
    const std::filesystem::path& path,
    std::string& storageKey)
{
    const auto relative = path.lexically_relative(root);
    if (relative.empty() || relative == "." || relative.has_root_path())
        return false;

    std::vector<std::string> parts;
    for (const auto& part : relative)
        parts.push_back(part.generic_string());
    if (parts.size() != 2u || !IsLowerHexStorageKey(parts[1]))
        return false;
    if (parts[0] != parts[1].substr(0u, kStorageShardLength))
        return false;

    storageKey = std::move(parts[1]);
    return true;
}

[[nodiscard]] bool IsManagedStagingDirectoryPath(
    const std::filesystem::path& root,
    const std::filesystem::path& path)
{
    const auto relative = path.lexically_relative(root);
    if (relative.empty() || relative == "." || relative.has_root_path())
        return false;

    std::vector<std::string> parts;
    for (const auto& part : relative)
        parts.push_back(part.generic_string());
    if (parts.size() != 2u)
        return false;

    constexpr std::string_view pendingMarker = ".pending.";
    const std::string_view directoryName = parts[1];
    constexpr size_t keyOffset = 1u;
    constexpr size_t markerOffset = keyOffset + kStorageKeyLength;
    if (directoryName.size() <= markerOffset + pendingMarker.size() ||
        directoryName.front() != '.')
    {
        return false;
    }

    const auto storageKey = directoryName.substr(keyOffset, kStorageKeyLength);
    return IsLowerHexStorageKey(storageKey) &&
        parts[0] == storageKey.substr(0u, kStorageShardLength) &&
        directoryName.substr(markerOffset, pendingMarker.size()) == pendingMarker;
}

template <typename Visitor>
[[nodiscard]] UndoBlobStoreResult VisitManagedBlobFiles(
    const std::filesystem::path& root,
    Visitor&& visitor)
{
    std::error_code error;
    std::filesystem::directory_iterator shards(root, error);
    const std::filesystem::directory_iterator end;
    if (error)
        return MakeResult(UndoBlobStoreStatus::IoError, "Could not enumerate undo blob root: " + error.message());

    while (shards != end)
    {
        const auto shard = *shards;
        const auto shardStatus = shard.symlink_status(error);
        if (error)
            return MakeResult(UndoBlobStoreStatus::IoError, "Could not inspect undo blob shard: " + error.message());

        if (!std::filesystem::is_symlink(shardStatus) && std::filesystem::is_directory(shardStatus))
        {
            std::filesystem::directory_iterator files(shard.path(), error);
            if (error)
                return MakeResult(UndoBlobStoreStatus::IoError, "Could not enumerate undo blob shard: " + error.message());

            while (files != end)
            {
                const auto file = *files;
                const auto fileStatus = file.symlink_status(error);
                if (error)
                    return MakeResult(UndoBlobStoreStatus::IoError, "Could not inspect undo blob: " + error.message());

                if (!std::filesystem::is_symlink(fileStatus) && std::filesystem::is_regular_file(fileStatus))
                {
                    std::string storageKey;
                    if (IsManagedBlobPath(root, file.path(), storageKey))
                    {
                        const auto visited = visitor(std::move(storageKey), file.path());
                        if (!visited.Succeeded())
                            return visited;
                    }
                }

                files.increment(error);
                if (error)
                    return MakeResult(UndoBlobStoreStatus::IoError, "Could not advance undo blob enumeration: " + error.message());
            }
        }

        shards.increment(error);
        if (error)
            return MakeResult(UndoBlobStoreStatus::IoError, "Could not advance undo blob shard enumeration: " + error.message());
    }

    return MakeResult(UndoBlobStoreStatus::Success);
}

[[nodiscard]] UndoBlobStoreResult CollectAbandonedStagingDirectories(
    const std::filesystem::path& root,
    std::vector<std::filesystem::path>& directories)
{
    std::error_code error;
    std::filesystem::directory_iterator shards(root, error);
    const std::filesystem::directory_iterator end;
    if (error)
        return MakeResult(UndoBlobStoreStatus::IoError, "Could not enumerate undo blob root: " + error.message());

    while (shards != end)
    {
        const auto shard = *shards;
        const auto shardStatus = shard.symlink_status(error);
        if (error)
            return MakeResult(UndoBlobStoreStatus::IoError, "Could not inspect undo blob shard: " + error.message());

        if (!std::filesystem::is_symlink(shardStatus) && std::filesystem::is_directory(shardStatus))
        {
            std::filesystem::directory_iterator entries(shard.path(), error);
            if (error)
                return MakeResult(UndoBlobStoreStatus::IoError, "Could not enumerate undo blob shard: " + error.message());

            while (entries != end)
            {
                const auto entry = *entries;
                const auto entryStatus = entry.symlink_status(error);
                if (error)
                {
                    return MakeResult(
                        UndoBlobStoreStatus::IoError,
                        "Could not inspect undo blob staging directory: " + error.message());
                }

                if (!std::filesystem::is_symlink(entryStatus) &&
                    std::filesystem::is_directory(entryStatus) &&
                    IsManagedStagingDirectoryPath(root, entry.path()))
                {
                    directories.push_back(entry.path());
                }

                entries.increment(error);
                if (error)
                {
                    return MakeResult(
                        UndoBlobStoreStatus::IoError,
                        "Could not advance undo blob staging enumeration: " + error.message());
                }
            }
        }

        shards.increment(error);
        if (error)
            return MakeResult(UndoBlobStoreStatus::IoError, "Could not advance undo blob enumeration: " + error.message());
    }

    return MakeResult(UndoBlobStoreStatus::Success);
}

[[nodiscard]] std::filesystem::path MakeStagingDirectory(
    const std::filesystem::path& blobPath,
    std::error_code& error)
{
    error.clear();
    const auto parent = blobPath.parent_path();
    for (size_t attempt = 0u; attempt < 32u; ++attempt)
    {
        error.clear();
        const auto sequence = g_temporarySequence.fetch_add(1u, std::memory_order_relaxed);
        const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto name = "." + blobPath.filename().string() + ".pending." +
            std::to_string(timestamp) + "." + std::to_string(sequence);
        const auto staging = parent / name;
        if (std::filesystem::create_directory(staging, error))
            return staging;

        if (error)
            return {};
    }

    error = std::make_error_code(std::errc::file_exists);
    return {};
}

void RemoveStagingDirectory(const std::filesystem::path& staging)
{
    if (staging.empty())
        return;

    std::error_code error;
    std::filesystem::remove(staging / "payload", error);
    error.clear();
    std::filesystem::remove(staging, error);
}

[[nodiscard]] UndoBlobStoreResult WriteFileFully(
    const std::filesystem::path& path,
    const std::span<const uint8_t> bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        return MakeResult(UndoBlobStoreStatus::IoError, "Could not open undo blob staging file.");

    size_t offset = 0u;
    while (offset < bytes.size())
    {
        const auto remaining = bytes.size() - offset;
        const auto chunkBytes = std::min(remaining, kWriteChunkBytes);
        output.write(
            reinterpret_cast<const char*>(bytes.data() + offset),
            static_cast<std::streamsize>(chunkBytes));
        if (!output)
        {
            output.close();
            return MakeResult(UndoBlobStoreStatus::IoError, "Could not write undo blob staging file.");
        }
        offset += chunkBytes;
    }

    output.flush();
    if (!output)
    {
        output.close();
        return MakeResult(UndoBlobStoreStatus::IoError, "Could not flush undo blob staging file.");
    }

    output.close();
    return MakeResult(UndoBlobStoreStatus::Success);
}

[[nodiscard]] uint64_t SaturatingAdd(const uint64_t left, const uint64_t right)
{
    if (right > std::numeric_limits<uint64_t>::max() - left)
        return std::numeric_limits<uint64_t>::max();
    return left + right;
}

[[nodiscard]] uint64_t SaturatingMultiply(const uint64_t left, const uint64_t right)
{
    if (left == 0u || right == 0u)
        return 0u;
    if (left > std::numeric_limits<uint64_t>::max() / right)
        return std::numeric_limits<uint64_t>::max();
    return left * right;
}
} // namespace

UndoBlobStore::UndoBlobStore(UndoBlobStoreOptions options)
    : m_options(std::move(options))
{
    m_options.root = m_options.root.lexically_normal();
}

UndoBlobStoreResult UndoBlobStore::Initialize()
{
    std::lock_guard lock(m_mutex);
    return EnsureInitializedLocked();
}

UndoBlobStoreWriteResult UndoBlobStore::Store(const std::span<const uint8_t> bytes)
{
    UndoBlobStoreWriteResult result;
    std::lock_guard lock(m_mutex);

    const auto initialized = EnsureInitializedLocked();
    if (!initialized.Succeeded())
    {
        result.status = initialized.status;
        result.message = initialized.message;
        return result;
    }

    if (bytes.size() > std::numeric_limits<uint64_t>::max())
    {
        result.status = UndoBlobStoreStatus::InvalidArgument;
        result.message = "Undo blob exceeds the supported byte count.";
        return result;
    }

    const auto storageKey = NLS::Core::Assets::BuildArtifactStorageFileName(bytes.data(), bytes.size());
    if (!IsLowerHexStorageKey(storageKey))
    {
        result.status = UndoBlobStoreStatus::IoError;
        result.message = "Could not calculate the undo blob content hash.";
        return result;
    }

    const auto byteCount = static_cast<uint64_t>(bytes.size());
    const auto existing = m_entries.find(storageKey);
    if (existing != m_entries.end())
    {
        const auto valid = ValidateBlobFileLocked(storageKey, existing->second.byteCount);
        if (!valid.Succeeded())
        {
            result.status = valid.status;
            result.message = valid.message;
            return result;
        }
        if (existing->second.byteCount != byteCount ||
            existing->second.referenceCount == std::numeric_limits<uint64_t>::max())
        {
            result.status = UndoBlobStoreStatus::InvalidArgument;
            result.message = "Undo blob reference count or size is invalid.";
            return result;
        }

        ++existing->second.referenceCount;
        result.reference = UndoBlobReference(storageKey, byteCount);
        result.deduplicated = true;
        return result;
    }

    const auto blobPath = BlobPathForKey(storageKey);
    std::error_code error;
    std::filesystem::create_directories(blobPath.parent_path(), error);
    if (error)
    {
        result.status = UndoBlobStoreStatus::IoError;
        result.message = "Could not create undo blob directory: " + error.message();
        return result;
    }

    const bool targetExists = std::filesystem::exists(blobPath, error);
    if (error)
    {
        result.status = UndoBlobStoreStatus::IoError;
        result.message = "Could not inspect undo blob destination: " + error.message();
        return result;
    }
    if (targetExists)
    {
        const auto valid = ValidateBlobFileLocked(storageKey, byteCount);
        if (!valid.Succeeded())
        {
            result.status = valid.status;
            result.message = valid.message;
            return result;
        }

        try
        {
            m_entries.emplace(storageKey, BlobEntry {byteCount, 1u});
        }
        catch (const std::exception& exception)
        {
            result.status = UndoBlobStoreStatus::IoError;
            result.message = "Could not retain deduplicated undo blob: " + std::string(exception.what());
            return result;
        }
        result.reference = UndoBlobReference(storageKey, byteCount);
        result.deduplicated = true;
        return result;
    }

    const auto stagingDirectory = MakeStagingDirectory(blobPath, error);
    if (error || stagingDirectory.empty())
    {
        result.status = UndoBlobStoreStatus::IoError;
        result.message = "Could not create undo blob staging directory: " + error.message();
        return result;
    }

    const auto stagingFile = stagingDirectory / "payload";
    const auto written = WriteFileFully(stagingFile, bytes);
    if (!written.Succeeded())
    {
        RemoveStagingDirectory(stagingDirectory);
        result.status = written.status;
        result.message = written.message;
        return result;
    }

    std::filesystem::create_hard_link(stagingFile, blobPath, error);
    bool deduplicated = false;
    if (error)
    {
        error.clear();
        const bool appearedDuringPublish = std::filesystem::exists(blobPath, error);
        if (error || !appearedDuringPublish)
        {
            const auto message = error
                ? "Could not publish undo blob: " + error.message()
                : "Could not publish undo blob without replacing an existing file.";
            RemoveStagingDirectory(stagingDirectory);
            result.status = UndoBlobStoreStatus::IoError;
            result.message = message;
            return result;
        }

        const auto valid = ValidateBlobFileLocked(storageKey, byteCount);
        RemoveStagingDirectory(stagingDirectory);
        if (!valid.Succeeded())
        {
            result.status = valid.status;
            result.message = valid.message;
            return result;
        }
        deduplicated = true;
    }
    else
    {
        RemoveStagingDirectory(stagingDirectory);
        const auto valid = ValidateBlobFileLocked(storageKey, byteCount);
        if (!valid.Succeeded())
        {
            result.status = valid.status;
            result.message = valid.message;
            return result;
        }
    }

    try
    {
        m_entries.emplace(storageKey, BlobEntry {byteCount, 1u});
    }
    catch (const std::exception& exception)
    {
        // The published data has no reference when bookkeeping fails. It is
        // deliberately left for CleanupUnreferenced rather than risking a
        // delete of a same-key blob published by another store instance.
        result.status = UndoBlobStoreStatus::IoError;
        result.message = "Could not retain published undo blob: " + std::string(exception.what());
        return result;
    }

    result.reference = UndoBlobReference(storageKey, byteCount);
    result.deduplicated = deduplicated;
    return result;
}

UndoBlobStoreResult UndoBlobStore::Retain(const UndoBlobReference& reference)
{
    std::lock_guard lock(m_mutex);
    const auto initialized = EnsureInitializedLocked();
    if (!initialized.Succeeded())
        return initialized;

    const auto validReference = ValidateReferenceLocked(reference);
    if (!validReference.Succeeded())
        return validReference;

    const auto entry = m_entries.find(reference.m_storageKey);
    if (entry == m_entries.end())
        return MakeResult(UndoBlobStoreStatus::NotFound, "Undo blob is not present in this store.");
    if (entry->second.referenceCount == std::numeric_limits<uint64_t>::max())
        return MakeResult(UndoBlobStoreStatus::InvalidArgument, "Undo blob reference count overflow.");

    const auto validFile = ValidateBlobFileLocked(reference.m_storageKey, entry->second.byteCount);
    if (!validFile.Succeeded())
        return validFile;

    ++entry->second.referenceCount;
    return MakeResult(UndoBlobStoreStatus::Success);
}

UndoBlobStoreResult UndoBlobStore::Release(const UndoBlobReference& reference)
{
    std::lock_guard lock(m_mutex);
    const auto initialized = EnsureInitializedLocked();
    if (!initialized.Succeeded())
        return initialized;

    const auto validReference = ValidateReferenceLocked(reference);
    if (!validReference.Succeeded())
        return validReference;

    const auto entry = m_entries.find(reference.m_storageKey);
    if (entry == m_entries.end())
        return MakeResult(UndoBlobStoreStatus::NotFound, "Undo blob is not present in this store.");
    if (entry->second.referenceCount == 0u)
        return MakeResult(UndoBlobStoreStatus::ReferenceUnderflow, "Undo blob has no retained references.");

    --entry->second.referenceCount;
    return MakeResult(UndoBlobStoreStatus::Success);
}

UndoBlobStoreLoadResult UndoBlobStore::Load(const UndoBlobReference& reference) const
{
    UndoBlobStoreLoadResult result;
    std::lock_guard lock(m_mutex);
    const auto initialized = const_cast<UndoBlobStore*>(this)->EnsureInitializedLocked();
    if (!initialized.Succeeded())
    {
        result.status = initialized.status;
        result.message = initialized.message;
        return result;
    }

    const auto validReference = ValidateReferenceLocked(reference);
    if (!validReference.Succeeded())
    {
        result.status = validReference.status;
        result.message = validReference.message;
        return result;
    }

    const auto entry = m_entries.find(reference.m_storageKey);
    if (entry == m_entries.end())
    {
        result.status = UndoBlobStoreStatus::NotFound;
        result.message = "Undo blob is not present in this store.";
        return result;
    }
    if (entry->second.referenceCount == 0u)
    {
        result.status = UndoBlobStoreStatus::NotRetained;
        result.message = "Undo blob was released before it was loaded.";
        return result;
    }

    const auto validFile = ValidateBlobFileLocked(reference.m_storageKey, entry->second.byteCount);
    if (!validFile.Succeeded())
    {
        result.status = validFile.status;
        result.message = validFile.message;
        return result;
    }

    if (entry->second.byteCount > std::numeric_limits<size_t>::max())
    {
        result.status = UndoBlobStoreStatus::IoError;
        result.message = "Undo blob is too large to load into memory.";
        return result;
    }

    result.bytes.resize(static_cast<size_t>(entry->second.byteCount));
    std::ifstream input(BlobPathForKey(reference.m_storageKey), std::ios::binary);
    if (!input)
    {
        result.bytes.clear();
        result.status = UndoBlobStoreStatus::IoError;
        result.message = "Could not open undo blob for reading.";
        return result;
    }

    size_t offset = 0u;
    while (offset < result.bytes.size())
    {
        const auto remaining = result.bytes.size() - offset;
        const auto chunkBytes = std::min(remaining, kWriteChunkBytes);
        input.read(
            reinterpret_cast<char*>(result.bytes.data() + offset),
            static_cast<std::streamsize>(chunkBytes));
        if (input.gcount() != static_cast<std::streamsize>(chunkBytes))
        {
            result.bytes.clear();
            result.status = UndoBlobStoreStatus::IoError;
            result.message = "Could not read the complete undo blob.";
            return result;
        }
        offset += chunkBytes;
    }
    return result;
}

UndoBlobStoreCleanupResult UndoBlobStore::CleanupUnreferenced()
{
    UndoBlobStoreCleanupResult result;
    std::lock_guard lock(m_mutex);
    const auto initialized = EnsureInitializedLocked();
    if (!initialized.Succeeded())
    {
        result.status = initialized.status;
        result.message = initialized.message;
        return result;
    }

    std::vector<std::pair<std::string, std::filesystem::path>> candidates;
    const auto enumerated = VisitManagedBlobFiles(
        m_options.root,
        [this, &candidates](std::string storageKey, const std::filesystem::path& path)
        {
            const auto entry = m_entries.find(storageKey);
            if (entry == m_entries.end() || entry->second.referenceCount == 0u)
                candidates.emplace_back(std::move(storageKey), path);
            return MakeResult(UndoBlobStoreStatus::Success);
        });
    if (!enumerated.Succeeded())
    {
        result.status = enumerated.status;
        result.message = enumerated.message;
        return result;
    }

    std::unordered_set<std::string> zeroReferenceFiles;
    zeroReferenceFiles.reserve(candidates.size());
    for (const auto& [storageKey, path] : candidates)
    {
        (void)path;
        zeroReferenceFiles.insert(storageKey);
    }

    std::error_code error;

    for (const auto& [storageKey, path] : candidates)
    {
        const auto byteCount = std::filesystem::file_size(path, error);
        if (error || byteCount > std::numeric_limits<uint64_t>::max())
        {
            if (result.message.empty())
                result.message = "Could not measure undo blob before cleanup.";
            error.clear();
            result.status = UndoBlobStoreStatus::IoError;
            continue;
        }

        const bool removed = std::filesystem::remove(path, error);
        if (error)
        {
            if (result.message.empty())
                result.message = "Could not remove unreferenced undo blob: " + error.message();
            error.clear();
            result.status = UndoBlobStoreStatus::IoError;
            continue;
        }

        const auto entry = m_entries.find(storageKey);
        if (entry != m_entries.end() && entry->second.referenceCount == 0u)
            m_entries.erase(entry);
        if (removed)
        {
            ++result.removedBlobCount;
            result.removedByteCount = SaturatingAdd(result.removedByteCount, static_cast<uint64_t>(byteCount));
        }
    }

    // A file can disappear outside the editor between cleanup runs. Its zero
    // reference accounting must not continue to consume the blob budget.
    for (auto entry = m_entries.begin(); entry != m_entries.end();)
    {
        if (entry->second.referenceCount == 0u &&
            !zeroReferenceFiles.contains(entry->first))
        {
            entry = m_entries.erase(entry);
        }
        else
        {
            ++entry;
        }
    }

    // Store holds this instance mutex while its staging directories exist.
    // EditorUndoService isolates every Editor instance in a private session
    // root, so matching directories here can only be leftovers from an
    // interrupted publish rather than another editor's active write.
    std::vector<std::filesystem::path> abandonedStagingDirectories;
    const auto collectedStaging = CollectAbandonedStagingDirectories(
        m_options.root,
        abandonedStagingDirectories);
    if (!collectedStaging.Succeeded())
    {
        if (result.message.empty())
            result.message = collectedStaging.message;
        result.status = collectedStaging.status;
        return result;
    }

    for (const auto& staging : abandonedStagingDirectories)
    {
        error.clear();
        std::filesystem::remove_all(staging, error);
        if (error)
        {
            if (result.message.empty())
            {
                result.message = "Could not remove abandoned undo blob staging directory: " +
                    error.message();
            }
            result.status = UndoBlobStoreStatus::IoError;
        }
    }
    return result;
}

bool UndoBlobStore::ShouldStoreOutOfLine(const size_t byteCount) const noexcept
{
    return static_cast<uint64_t>(byteCount) > m_options.spillThresholdBytes;
}

UndoBlobStoreStatistics UndoBlobStore::GetStatistics() const
{
    std::lock_guard lock(m_mutex);
    UndoBlobStoreStatistics statistics;
    for (const auto& [storageKey, entry] : m_entries)
    {
        (void)storageKey;
        ++statistics.blobCount;
        statistics.storedByteCount = SaturatingAdd(statistics.storedByteCount, entry.byteCount);
        statistics.totalReferenceCount = SaturatingAdd(statistics.totalReferenceCount, entry.referenceCount);
        statistics.logicalRetainedByteCount = SaturatingAdd(
            statistics.logicalRetainedByteCount,
            SaturatingMultiply(entry.byteCount, entry.referenceCount));
        if (entry.referenceCount > 0u)
        {
            ++statistics.referencedBlobCount;
            statistics.referencedByteCount = SaturatingAdd(statistics.referencedByteCount, entry.byteCount);
        }
    }
    return statistics;
}

UndoBlobStoreResult UndoBlobStore::EnsureInitializedLocked()
{
    if (m_initialized)
        return MakeResult(UndoBlobStoreStatus::Success);
    if (m_options.root.empty())
        return MakeResult(UndoBlobStoreStatus::InvalidArgument, "Undo blob store root is empty.");

    std::error_code error;
    std::filesystem::create_directories(m_options.root, error);
    if (error)
        return MakeResult(UndoBlobStoreStatus::IoError, "Could not create undo blob root: " + error.message());
    if (!std::filesystem::is_directory(m_options.root, error) || error)
    {
        const auto message = error ? error.message() : "Configured undo blob root is not a directory.";
        return MakeResult(UndoBlobStoreStatus::IoError, "Could not use undo blob root: " + message);
    }

    std::vector<std::pair<std::string, BlobEntry>> discovered;
    const auto enumerated = VisitManagedBlobFiles(
        m_options.root,
        [&discovered](std::string storageKey, const std::filesystem::path& path)
        {
            std::error_code sizeError;
            const auto byteCount = std::filesystem::file_size(path, sizeError);
            if (sizeError || byteCount > std::numeric_limits<uint64_t>::max())
            {
                const auto message = sizeError ? sizeError.message() : "Undo blob size exceeds supported range.";
                return MakeResult(UndoBlobStoreStatus::IoError, "Could not inspect undo blob size: " + message);
            }

            // A malformed or externally corrupted file remains unreferenced.
            // CleanupUnreferenced can remove it; Retain/Load never trust it.
            const auto actualKey = NLS::Core::Assets::BuildArtifactStorageFileNameFromFile(path);
            if (actualKey == storageKey)
                discovered.emplace_back(std::move(storageKey), BlobEntry {static_cast<uint64_t>(byteCount), 0u});
            return MakeResult(UndoBlobStoreStatus::Success);
        });
    if (!enumerated.Succeeded())
        return enumerated;

    try
    {
        std::unordered_map<std::string, BlobEntry> rebuiltEntries;
        rebuiltEntries.reserve(discovered.size());
        for (auto& [storageKey, entry] : discovered)
            rebuiltEntries.emplace(std::move(storageKey), entry);
        m_entries.swap(rebuiltEntries);
    }
    catch (const std::exception& exception)
    {
        return MakeResult(UndoBlobStoreStatus::IoError, "Could not index undo blobs: " + std::string(exception.what()));
    }

    m_initialized = true;
    return MakeResult(UndoBlobStoreStatus::Success);
}

UndoBlobStoreResult UndoBlobStore::ValidateReferenceLocked(const UndoBlobReference& reference) const
{
    if (!reference.IsValid() || !IsLowerHexStorageKey(reference.m_storageKey))
        return MakeResult(UndoBlobStoreStatus::InvalidArgument, "Undo blob reference is invalid.");
    return MakeResult(UndoBlobStoreStatus::Success);
}

UndoBlobStoreResult UndoBlobStore::ValidateBlobFileLocked(
    const std::string& storageKey,
    const uint64_t expectedByteCount) const
{
    const auto path = BlobPathForKey(storageKey);
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error || std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status))
    {
        const auto message = error ? error.message() : "Undo blob is missing or not a regular file.";
        return MakeResult(UndoBlobStoreStatus::NotFound, message);
    }

    const auto byteCount = std::filesystem::file_size(path, error);
    if (error)
        return MakeResult(UndoBlobStoreStatus::IoError, "Could not measure undo blob: " + error.message());
    if (byteCount != expectedByteCount)
        return MakeResult(UndoBlobStoreStatus::Corrupt, "Undo blob byte count no longer matches its reference.");

    const auto actualKey = NLS::Core::Assets::BuildArtifactStorageFileNameFromFile(path);
    if (actualKey != storageKey)
        return MakeResult(UndoBlobStoreStatus::Corrupt, "Undo blob content hash no longer matches its reference.");
    return MakeResult(UndoBlobStoreStatus::Success);
}

std::filesystem::path UndoBlobStore::BlobPathForKey(const std::string& storageKey) const
{
    return m_options.root / storageKey.substr(0u, kStorageShardLength) / storageKey;
}
} // namespace NLS::Editor::Undo
