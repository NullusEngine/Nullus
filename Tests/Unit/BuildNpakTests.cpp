#include <gtest/gtest.h>

#include "Assets/NpakArchive.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <random>
#include <vector>

namespace
{
using NLS::Core::Assets::NpakArchive;
using NLS::Core::Assets::NpakWriteOptions;
using NLS::Core::Assets::NpakWriter;

std::filesystem::path MakeTemporaryDirectory(const std::string& name)
{
    const auto directory = std::filesystem::temp_directory_path() /
        ("nullus-npak-" + name + "-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    return directory;
}

std::vector<uint8_t> MakePatternBytes(const size_t count, const uint32_t seed)
{
    std::vector<uint8_t> bytes(count);
    std::mt19937 generator(seed);
    std::uniform_int_distribution<int> distribution(0, 255);
    for (auto& byte : bytes)
        byte = static_cast<uint8_t>(distribution(generator));
    return bytes;
}

std::vector<uint8_t> ReadWholeFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        return {};
    const auto size = input.tellg();
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    input.seekg(0, std::ios::beg);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return bytes;
}
} // namespace

TEST(NpakArchiveTests, RoundTripsMultipleFilesAndReportsSize)
{
    const auto directory = MakeTemporaryDirectory("roundtrip");
    const auto archivePath = directory / "base.npak";

    const auto large = MakePatternBytes(1024u * 1024u + 137u, 7u);
    const auto small = MakePatternBytes(64u, 11u);

    NpakWriter writer;
    ASSERT_TRUE(writer.AddFile("Artifacts/ab/" + std::string(64u, 'a'), large));
    ASSERT_TRUE(writer.AddFile("Assets/Scripts/Example.lua", small));

    NpakWriteOptions options;
    options.blockSize = 256u * 1024u;
    std::string error;
    ASSERT_TRUE(writer.Write(archivePath, options, &error)) << error;

    auto archive = NpakArchive::Open(archivePath, &error);
    ASSERT_TRUE(archive.has_value()) << error;
    EXPECT_EQ(archive->GetEntries().size(), 2u);
    EXPECT_EQ(archive->Size("Artifacts/ab/" + std::string(64u, 'a')), large.size());

    std::vector<uint8_t> readBack;
    ASSERT_TRUE(archive->ReadAll("Artifacts/ab/" + std::string(64u, 'a'), readBack, &error)) << error;
    EXPECT_EQ(readBack, large);

    std::vector<uint8_t> smallBack;
    ASSERT_TRUE(archive->ReadAll("Assets/Scripts/Example.lua", smallBack, &error)) << error;
    EXPECT_EQ(smallBack, small);

    std::error_code cleanupError;
    std::filesystem::remove_all(directory, cleanupError);
}

TEST(NpakArchiveTests, SameInputProducesByteIdenticalArchives)
{
    const auto directory = MakeTemporaryDirectory("deterministic");
    const auto first = directory / "first.npak";
    const auto second = directory / "second.npak";

    std::vector<uint8_t> bytes = MakePatternBytes(300u * 1024u, 3u);
    for (const auto& path : {first, second})
    {
        NpakWriter writer;
        ASSERT_TRUE(writer.AddFile("Artifacts/ab/" + std::string(64u, 'b'), bytes));
        ASSERT_TRUE(writer.AddFile("Assets/Config.json", std::vector<uint8_t> {'{', '}'}));
        std::string error;
        ASSERT_TRUE(writer.Write(path, {}, &error)) << error;
    }

    EXPECT_EQ(ReadWholeFile(first), ReadWholeFile(second));
    std::error_code cleanupError;
    std::filesystem::remove_all(directory, cleanupError);
}

TEST(NpakArchiveTests, DeduplicatesIdenticalContent)
{
    const auto directory = MakeTemporaryDirectory("dedupe");
    const auto archivePath = directory / "dedupe.npak";

    const std::vector<uint8_t> shared = MakePatternBytes(4096u, 5u);
    NpakWriter writer;
    ASSERT_TRUE(writer.AddFile("Artifacts/aa/" + std::string(64u, 'c'), shared));
    ASSERT_TRUE(writer.AddFile("Artifacts/bb/" + std::string(64u, 'd'), shared));
    EXPECT_EQ(writer.GetAddedFileCount(), 2u);
    EXPECT_EQ(writer.GetStoredFileCount(), 1u);
    EXPECT_EQ(writer.GetDeduplicatedFileCount(), 1u);

    std::string error;
    ASSERT_TRUE(writer.Write(archivePath, {}, &error)) << error;

    auto archive = NpakArchive::Open(archivePath, &error);
    ASSERT_TRUE(archive.has_value()) << error;
    const auto& info = archive->GetInfo();
    EXPECT_EQ(info.rawBytes, 4096u);
    // Random payload is incompressible, so one stored copy plus the header and
    // indexes must stay well below the size of two independent copies.
    EXPECT_LT(std::filesystem::file_size(archivePath), 2u * 4096u);
    // The two entries point at the same first block.
    ASSERT_EQ(archive->GetEntries().size(), 2u);
    EXPECT_EQ(archive->GetEntries()[0].firstBlock, archive->GetEntries()[1].firstBlock);
    std::error_code cleanupError;
    std::filesystem::remove_all(directory, cleanupError);
}

TEST(NpakArchiveTests, ReadsAcrossBlockBoundaries)
{
    const auto directory = MakeTemporaryDirectory("cross-block");
    const auto archivePath = directory / "blocks.npak";

    std::vector<uint8_t> bytes(700u * 1024u);
    for (size_t index = 0u; index < bytes.size(); ++index)
        bytes[index] = static_cast<uint8_t>((index * 31u) & 0xFFu);

    NpakWriter writer;
    ASSERT_TRUE(writer.AddFile("Artifacts/ab/" + std::string(64u, 'e'), bytes));
    NpakWriteOptions options;
    options.blockSize = 64u * 1024u;
    std::string error;
    ASSERT_TRUE(writer.Write(archivePath, options, &error)) << error;

    auto archive = NpakArchive::Open(archivePath, &error);
    ASSERT_TRUE(archive.has_value()) << error;

    // Start near the end of block 1 and read into block 3.
    const uint64_t offset = 60u * 1024u;
    const size_t count = 200u * 1024u;
    std::vector<uint8_t> slice(count);
    ASSERT_TRUE(archive->ReadAt(
        "Artifacts/ab/" + std::string(64u, 'e'),
        offset,
        slice.data(),
        slice.size(),
        &error)) << error;
    EXPECT_TRUE(std::equal(slice.begin(), slice.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset)));

    // A read that ends exactly at the file end also succeeds.
    std::array<uint8_t, 16u> tail {};
    ASSERT_TRUE(archive->ReadAt(
        "Artifacts/ab/" + std::string(64u, 'e'),
        bytes.size() - tail.size(),
        tail.data(),
        tail.size(),
        &error)) << error;
    EXPECT_TRUE(std::equal(tail.begin(), tail.end(), bytes.end() - static_cast<std::ptrdiff_t>(tail.size())));

    std::error_code cleanupError;
    std::filesystem::remove_all(directory, cleanupError);
}

TEST(NpakArchiveTests, RejectsOutOfRangeReadsAndUnknownFiles)
{
    const auto directory = MakeTemporaryDirectory("range");
    const auto archivePath = directory / "range.npak";
    NpakWriter writer;
    ASSERT_TRUE(writer.AddFile("file.bin", MakePatternBytes(32u, 1u)));
    std::string error;
    ASSERT_TRUE(writer.Write(archivePath, {}, &error)) << error;

    auto archive = NpakArchive::Open(archivePath, &error);
    ASSERT_TRUE(archive.has_value()) << error;

    std::array<uint8_t, 8u> buffer {};
    EXPECT_FALSE(archive->ReadAt("missing.bin", 0u, buffer.data(), buffer.size()));
    EXPECT_FALSE(archive->ReadAt("file.bin", 30u, buffer.data(), buffer.size()));
    EXPECT_TRUE(archive->ReadAt("file.bin", 32u, buffer.data(), 0u));
    std::error_code cleanupError;
    std::filesystem::remove_all(directory, cleanupError);
}

TEST(NpakArchiveTests, DetectsTruncationAndCorruption)
{
    const auto directory = MakeTemporaryDirectory("corrupt");
    const auto archivePath = directory / "corrupt.npak";
    NpakWriter writer;
    ASSERT_TRUE(writer.AddFile("payload.bin", MakePatternBytes(256u * 1024u, 9u)));
    std::string error;
    ASSERT_TRUE(writer.Write(archivePath, {}, &error)) << error;

    auto bytes = ReadWholeFile(archivePath);
    ASSERT_FALSE(bytes.empty());

    // Truncated header.
    {
        const auto truncatedPath = directory / "truncated.npak";
        std::ofstream output(truncatedPath, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()), 32);
        output.close();
        EXPECT_FALSE(NpakArchive::Open(truncatedPath, &error).has_value());
    }

    // Flip a byte inside the first compressed block; the block checksum fails.
    {
        const auto corruptedPath = directory / "corrupted.npak";
        auto corrupted = bytes;
        corrupted[corrupted.size() / 2u] ^= 0xFFu;
        std::ofstream output(corruptedPath, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(corrupted.data()), static_cast<std::streamsize>(corrupted.size()));
        output.close();

        auto archive = NpakArchive::Open(corruptedPath, &error);
        ASSERT_TRUE(archive.has_value()) << error;
        std::vector<uint8_t> readBack;
        EXPECT_FALSE(archive->ReadAll("payload.bin", readBack, &error));
        EXPECT_FALSE(archive->VerifyPayload("payload.bin", &error));
    }

    // Unknown format version is rejected before any index is trusted.
    {
        const auto versionedPath = directory / "versioned.npak";
        auto versioned = bytes;
        versioned[4] = 0xFFu;
        std::ofstream output(versionedPath, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(versioned.data()), static_cast<std::streamsize>(versioned.size()));
        output.close();
        EXPECT_FALSE(NpakArchive::Open(versionedPath, &error).has_value());
        EXPECT_NE(error.find("version"), std::string::npos);
    }

    std::error_code cleanupError;
    std::filesystem::remove_all(directory, cleanupError);
}

TEST(NpakArchiveTests, RejectsDuplicateAndUnsafePaths)
{
    NpakWriter writer;
    std::string error;
    EXPECT_TRUE(writer.AddFile("Assets/File.lua", std::vector<uint8_t> {'a'}));
    EXPECT_FALSE(writer.AddFile("Assets/File.lua", std::vector<uint8_t> {'b'}));
    EXPECT_FALSE(writer.AddFile("../escape.bin", std::vector<uint8_t> {'c'}));
    EXPECT_FALSE(writer.AddFile("/absolute.bin", std::vector<uint8_t> {'d'}));
    EXPECT_FALSE(writer.AddFile("Assets\\Windows.lua", std::vector<uint8_t> {'e'}));
    EXPECT_FALSE(writer.AddFile("C:/drive.bin", std::vector<uint8_t> {'f'}));
    EXPECT_FALSE(writer.AddFile("Assets//double.bin", std::vector<uint8_t> {'g'}));
}

TEST(NpakArchiveTests, BlockCacheRespectsBudgetAndReportsStats)
{
    const auto directory = MakeTemporaryDirectory("cache");
    const auto archivePath = directory / "cache.npak";
    NpakWriter writer;
    ASSERT_TRUE(writer.AddFile("payload.bin", MakePatternBytes(512u * 1024u, 13u)));
    NpakWriteOptions options;
    options.blockSize = 64u * 1024u;
    options.enableCompression = false;
    std::string error;
    ASSERT_TRUE(writer.Write(archivePath, options, &error)) << error;

    auto archive = NpakArchive::Open(archivePath, &error);
    ASSERT_TRUE(archive.has_value()) << error;
    archive->SetBlockCacheBudget(128u * 1024u);

    std::vector<uint8_t> buffer(16u * 1024u);
    for (uint64_t offset = 0u; offset < 512u * 1024u; offset += buffer.size())
        ASSERT_TRUE(archive->ReadAt("payload.bin", offset, buffer.data(), buffer.size(), &error)) << error;

    const auto stats = archive->GetCacheStats();
    EXPECT_LE(stats.cachedBytes, 128u * 1024u);
    EXPECT_GT(stats.evictions, 0u);
    EXPECT_GT(stats.decodedBlocks, 0u);

    archive->ClearBlockCache();
    EXPECT_EQ(archive->GetCacheStats().cachedBytes, 0u);
    std::error_code cleanupError;
    std::filesystem::remove_all(directory, cleanupError);
}

TEST(NpakArchiveTests, StoresIncompressibleBlocksWithoutCompression)
{
    const auto directory = MakeTemporaryDirectory("store");
    const auto archivePath = directory / "store.npak";
    NpakWriter writer;
    ASSERT_TRUE(writer.AddFile("payload.bin", MakePatternBytes(200u * 1024u, 17u)));
    NpakWriteOptions options;
    options.enableCompression = false;
    std::string error;
    ASSERT_TRUE(writer.Write(archivePath, options, &error)) << error;

    auto archive = NpakArchive::Open(archivePath, &error);
    ASSERT_TRUE(archive.has_value()) << error;
    EXPECT_EQ(std::string(archive->GetInfo().codecName), "store");
    EXPECT_EQ(archive->GetInfo().storedBytes, archive->GetInfo().rawBytes);

    std::vector<uint8_t> readBack;
    ASSERT_TRUE(archive->ReadAll("payload.bin", readBack, &error)) << error;
    EXPECT_EQ(readBack.size(), 200u * 1024u);
    std::error_code cleanupError;
    std::filesystem::remove_all(directory, cleanupError);
}
