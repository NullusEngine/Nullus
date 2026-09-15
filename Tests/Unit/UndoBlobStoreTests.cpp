#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "Guid.h"
#include "Undo/EditorUndoService.h"
#include "Undo/UndoBlobStore.h"

namespace NLS::Editor::Undo::Tests
{
namespace
{
class ScopedBlobStoreRoot final
{
public:
    ScopedBlobStoreRoot()
        : m_root(std::filesystem::temp_directory_path() /
            ("nullus_undo_blobs_" + NLS::Guid::New().ToString()))
    {
    }

    ~ScopedBlobStoreRoot()
    {
        std::error_code error;
        std::filesystem::remove_all(m_root, error);
    }

    [[nodiscard]] const std::filesystem::path& Get() const noexcept { return m_root; }

private:
    std::filesystem::path m_root;
};

[[nodiscard]] std::vector<uint8_t> MakeSnapshot(const size_t byteCount)
{
    std::vector<uint8_t> result(byteCount);
    for (size_t index = 0u; index < result.size(); ++index)
        result[index] = static_cast<uint8_t>((index * 31u + 17u) & 0xffu);
    return result;
}

[[nodiscard]] std::filesystem::path BlobPath(
    const std::filesystem::path& root,
    const UndoBlobReference& reference)
{
    const auto key = std::string(reference.GetStorageKey());
    return root / key.substr(0u, 2u) / key;
}
} // namespace

TEST(UndoBlobStoreTests, StoresDeduplicatesLoadsAndCleansLargeSnapshots)
{
    ScopedBlobStoreRoot root;
    UndoBlobStore store({root.Get()});
    const auto snapshot = MakeSnapshot(UndoBlobStoreOptions {}.spillThresholdBytes + 257u);

    ASSERT_TRUE(store.Initialize().Succeeded());
    EXPECT_TRUE(store.ShouldStoreOutOfLine(snapshot.size()));
    EXPECT_FALSE(store.ShouldStoreOutOfLine(UndoBlobStoreOptions {}.spillThresholdBytes));

    const auto first = store.Store(snapshot);
    ASSERT_TRUE(first.Succeeded()) << first.message;
    EXPECT_FALSE(first.deduplicated);
    EXPECT_TRUE(first.reference.IsValid());
    EXPECT_EQ(first.reference.GetStorageKey().size(), 64u);
    EXPECT_TRUE(std::filesystem::exists(BlobPath(root.Get(), first.reference)));

    const auto second = store.Store(snapshot);
    ASSERT_TRUE(second.Succeeded()) << second.message;
    EXPECT_TRUE(second.deduplicated);
    EXPECT_EQ(second.reference, first.reference);

    const auto statistics = store.GetStatistics();
    EXPECT_EQ(statistics.blobCount, 1u);
    EXPECT_EQ(statistics.storedByteCount, snapshot.size());
    EXPECT_EQ(statistics.referencedBlobCount, 1u);
    EXPECT_EQ(statistics.referencedByteCount, snapshot.size());
    EXPECT_EQ(statistics.logicalRetainedByteCount, snapshot.size() * 2u);
    EXPECT_EQ(statistics.totalReferenceCount, 2u);

    const auto loaded = store.Load(first.reference);
    ASSERT_TRUE(loaded.Succeeded()) << loaded.message;
    EXPECT_EQ(loaded.bytes, snapshot);

    ASSERT_TRUE(store.Release(first.reference).Succeeded());
    EXPECT_EQ(store.GetStatistics().totalReferenceCount, 1u);
    ASSERT_TRUE(store.Release(second.reference).Succeeded());
    EXPECT_EQ(store.GetStatistics().totalReferenceCount, 0u);

    const auto cleanup = store.CleanupUnreferenced();
    ASSERT_TRUE(cleanup.Succeeded()) << cleanup.message;
    EXPECT_EQ(cleanup.removedBlobCount, 1u);
    EXPECT_EQ(cleanup.removedByteCount, snapshot.size());
    EXPECT_FALSE(std::filesystem::exists(BlobPath(root.Get(), first.reference)));
    EXPECT_EQ(store.GetStatistics().blobCount, 0u);
}

TEST(UndoBlobStoreTests, ReopenedStoreCanRetainAnOpaqueReference)
{
    ScopedBlobStoreRoot root;
    const auto snapshot = MakeSnapshot(4099u);
    UndoBlobReference reference;

    {
        UndoBlobStore original({root.Get()});
        const auto written = original.Store(snapshot);
        ASSERT_TRUE(written.Succeeded()) << written.message;
        reference = written.reference;
        ASSERT_TRUE(original.Release(reference).Succeeded());
    }

    UndoBlobStore reopened({root.Get()});
    const auto retained = reopened.Retain(reference);
    ASSERT_TRUE(retained.Succeeded()) << retained.message;
    const auto loaded = reopened.Load(reference);
    ASSERT_TRUE(loaded.Succeeded()) << loaded.message;
    EXPECT_EQ(loaded.bytes, snapshot);
    ASSERT_TRUE(reopened.Release(reference).Succeeded());

    const auto cleanup = reopened.CleanupUnreferenced();
    EXPECT_TRUE(cleanup.Succeeded()) << cleanup.message;
    EXPECT_EQ(cleanup.removedBlobCount, 1u);
}

TEST(UndoBlobStoreTests, RejectsCorruptDataAndAllowsItsUnreferencedCleanup)
{
    ScopedBlobStoreRoot root;
    UndoBlobStore store({root.Get()});
    const auto written = store.Store(MakeSnapshot(1024u));
    ASSERT_TRUE(written.Succeeded()) << written.message;

    const auto path = BlobPath(root.Get(), written.reference);
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(output);
        output << "external modification";
        ASSERT_TRUE(output.good());
    }

    const auto loaded = store.Load(written.reference);
    EXPECT_EQ(loaded.status, UndoBlobStoreStatus::Corrupt);
    ASSERT_TRUE(store.Release(written.reference).Succeeded());

    const auto cleanup = store.CleanupUnreferenced();
    ASSERT_TRUE(cleanup.Succeeded()) << cleanup.message;
    EXPECT_EQ(cleanup.removedBlobCount, 1u);
    EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(UndoBlobStoreTests, RejectsAnInvalidRootAndDoesNotProduceFiles)
{
    ScopedBlobStoreRoot parent;
    std::filesystem::create_directories(parent.Get());
    const auto invalidRoot = parent.Get() / "not-a-directory";
    {
        std::ofstream output(invalidRoot, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(output);
        output << "file";
    }

    UndoBlobStore store({invalidRoot});
    const auto written = store.Store(MakeSnapshot(4u));
    EXPECT_EQ(written.status, UndoBlobStoreStatus::IoError);
    EXPECT_FALSE(written.reference.IsValid());
}

TEST(UndoBlobStoreTests, DetectsDoubleReleaseWithoutChangingRetention)
{
    ScopedBlobStoreRoot root;
    UndoBlobStore store({root.Get()});
    const auto written = store.Store(MakeSnapshot(4u));
    ASSERT_TRUE(written.Succeeded()) << written.message;

    ASSERT_TRUE(store.Release(written.reference).Succeeded());
    const auto doubleRelease = store.Release(written.reference);
    EXPECT_EQ(doubleRelease.status, UndoBlobStoreStatus::ReferenceUnderflow);
    EXPECT_EQ(store.GetStatistics().totalReferenceCount, 0u);

    const auto retain = store.Retain(written.reference);
    ASSERT_TRUE(retain.Succeeded()) << retain.message;
    EXPECT_EQ(store.GetStatistics().totalReferenceCount, 1u);
    ASSERT_TRUE(store.Release(written.reference).Succeeded());
}

TEST(UndoBlobStoreTests, ServicesUseIsolatedSessionsForTheSameProjectBlobRoot)
{
    ScopedBlobStoreRoot root;
    const auto configuredRoot = root.Get() / "Library" / "Editor" / "UndoBlobs";
    const auto snapshot = MakeSnapshot(4099u);

    EditorUndoService first;
    const auto firstConfigured = first.ConfigureBlobStore(configuredRoot);
    ASSERT_TRUE(firstConfigured.Succeeded()) << firstConfigured.message;
    const auto firstStore = first.GetSnapshotBlobStore();
    ASSERT_NE(firstStore, nullptr);
    const auto written = firstStore->Store(snapshot);
    ASSERT_TRUE(written.Succeeded()) << written.message;

    EditorUndoService second;
    const auto secondConfigured = second.ConfigureBlobStore(configuredRoot);
    ASSERT_TRUE(secondConfigured.Succeeded()) << secondConfigured.message;
    const auto secondStore = second.GetSnapshotBlobStore();
    ASSERT_NE(secondStore, nullptr);
    EXPECT_NE(firstStore->GetRoot(), secondStore->GetRoot());

    const auto loaded = firstStore->Load(written.reference);
    ASSERT_TRUE(loaded.Succeeded()) << loaded.message;
    EXPECT_EQ(loaded.bytes, snapshot);

    ASSERT_TRUE(firstStore->Release(written.reference).Succeeded());
    first.ClearAll();
    second.ClearAll();
}

TEST(UndoBlobStoreTests, ConfigureBlobStoreReclaimsOnlyStaleLeasedSiblingSessions)
{
    ScopedBlobStoreRoot root;
    const auto configuredRoot = root.Get() / "Library" / "Editor" / "UndoBlobs";
    const auto staleSession = configuredRoot / ".s-0011223344556677";
    const auto recentSession = configuredRoot / ".s-8899aabbccddeeff";
    const auto unleasedSession = configuredRoot / ".s-1122334455667788";
    const auto staleLease = staleSession / ".lease";
    const auto recentLease = recentSession / ".lease";

    std::error_code error;
    ASSERT_TRUE(std::filesystem::create_directories(staleSession, error)) << error.message();
    ASSERT_FALSE(error);
    ASSERT_TRUE(std::filesystem::create_directories(recentSession, error)) << error.message();
    ASSERT_FALSE(error);
    ASSERT_TRUE(std::filesystem::create_directories(unleasedSession, error)) << error.message();
    ASSERT_FALSE(error);
    {
        std::ofstream stale(staleLease, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(stale);
        stale << "stale";
        ASSERT_TRUE(stale.good());
    }
    {
        std::ofstream recent(recentLease, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(recent);
        recent << "recent";
        ASSERT_TRUE(recent.good());
    }

    std::filesystem::last_write_time(
        staleLease,
        std::filesystem::file_time_type::clock::now() - std::chrono::hours(48),
        error);
    ASSERT_FALSE(error) << error.message();

    std::filesystem::path currentSession;
    {
        EditorUndoService service;
        const auto configured = service.ConfigureBlobStore(configuredRoot);
        ASSERT_TRUE(configured.Succeeded()) << configured.message;
        const auto currentStore = service.GetSnapshotBlobStore();
        ASSERT_NE(currentStore, nullptr);
        currentSession = currentStore->GetRoot();
        EXPECT_TRUE(std::filesystem::is_regular_file(currentSession / ".lease"));

        EXPECT_FALSE(std::filesystem::exists(staleSession));
        EXPECT_TRUE(std::filesystem::exists(recentSession));
        // A missing marker may belong to an older editor version; preserve it.
        EXPECT_TRUE(std::filesystem::exists(unleasedSession));
    }

    EXPECT_FALSE(std::filesystem::exists(currentSession / ".lease"));
}

TEST(UndoBlobStoreTests, CleanupRemovesOnlyAbandonedManagedStagingDirectories)
{
    ScopedBlobStoreRoot root;
    const std::string storageKey(64u, 'a');
    const auto shard = root.Get() / storageKey.substr(0u, 2u);
    const auto abandonedStaging = shard / ("." + storageKey + ".pending.interrupted-write");
    const auto unrelatedDirectory = shard / ".unrelated-directory";
    std::error_code error;
    ASSERT_TRUE(std::filesystem::create_directories(abandonedStaging, error)) << error.message();
    ASSERT_FALSE(error);
    ASSERT_TRUE(std::filesystem::create_directories(unrelatedDirectory, error)) << error.message();
    ASSERT_FALSE(error);

    {
        std::ofstream payload(abandonedStaging / "payload", std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(payload);
        payload << "incomplete snapshot";
        ASSERT_TRUE(payload.good());
    }

    UndoBlobStore store({root.Get()});
    const auto cleanup = store.CleanupUnreferenced();
    ASSERT_TRUE(cleanup.Succeeded()) << cleanup.message;
    EXPECT_FALSE(std::filesystem::exists(abandonedStaging));
    EXPECT_TRUE(std::filesystem::exists(unrelatedDirectory));
    EXPECT_EQ(store.GetStatistics().blobCount, 0u);
}
} // namespace NLS::Editor::Undo::Tests
