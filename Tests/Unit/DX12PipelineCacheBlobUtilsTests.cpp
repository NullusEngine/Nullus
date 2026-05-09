#include <gtest/gtest.h>

#include <filesystem>

#if defined(_WIN32)
#include <d3d12.h>
#endif

#include "Rendering/RHI/Backends/DX12/DX12PipelineCacheBlobUtils.h"

namespace
{
    std::filesystem::path MakePsoBlobTestRoot()
    {
        auto root = std::filesystem::temp_directory_path() / "NullusDX12PipelineCacheBlobUtilsTests";
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        return root;
    }
}

TEST(DX12PipelineCacheBlobUtilsTests, BuildsStableSanitizedBlobPathsFromPipelineKey)
{
    NLS::Render::RHI::PipelineCacheKey key;
    key.backend = NLS::Render::RHI::NativeBackendType::DX12;
    key.hash = 0x1234ABCDu;
    key.stableDebugName = "Lighting PSO/Main";

    const auto paths = NLS::Render::RHI::DX12::BuildDX12PipelineCacheBlobPaths(
        key,
        "graphics",
        "CacheRoot");

    EXPECT_EQ(paths.directory, std::filesystem::path("CacheRoot") / "DX12PipelineState");
    EXPECT_EQ(paths.blobPath.filename(), std::filesystem::path("graphics_1234abcd_Lighting_PSO_Main.pso"));
    EXPECT_EQ(paths.temporaryPath.filename(), std::filesystem::path("graphics_1234abcd_Lighting_PSO_Main.pso.tmp"));
}

TEST(DX12PipelineCacheBlobUtilsTests, WritesBlobThroughTemporaryFileAndReadsItBack)
{
    const auto root = MakePsoBlobTestRoot();
    const auto blobPath = root / "Nested" / "Pipeline.pso";
    const std::vector<uint8_t> blob = { 0x01u, 0x20u, 0x30u, 0xFFu };

    ASSERT_TRUE(NLS::Render::RHI::DX12::WriteDX12PipelineCacheBlobAtomically(blobPath, blob.data(), blob.size()));

    const auto readBack = NLS::Render::RHI::DX12::ReadDX12PipelineCacheBlob(blobPath);
    EXPECT_EQ(readBack, blob);
    EXPECT_FALSE(std::filesystem::exists(blobPath.string() + ".tmp"));

    std::filesystem::remove_all(root);
}

TEST(DX12PipelineCacheBlobUtilsTests, RejectsEmptyBlobWrites)
{
    const auto root = MakePsoBlobTestRoot();
    const auto blobPath = root / "Empty.pso";

    EXPECT_FALSE(NLS::Render::RHI::DX12::WriteDX12PipelineCacheBlobAtomically(blobPath, nullptr, 0u));
    EXPECT_FALSE(std::filesystem::exists(blobPath));

    std::filesystem::remove_all(root);
}

#if defined(_WIN32)
TEST(DX12PipelineCacheBlobUtilsTests, BuildsD3D12CachedPipelineStateViewOnlyWhenBlobExists)
{
    const std::vector<uint8_t> blob = { 0x10u, 0x20u, 0x30u };

    const D3D12_CACHED_PIPELINE_STATE cachedState =
        NLS::Render::RHI::DX12::BuildDX12CachedPipelineStateView(blob);
    const D3D12_CACHED_PIPELINE_STATE emptyState =
        NLS::Render::RHI::DX12::BuildDX12CachedPipelineStateView({});

    EXPECT_EQ(cachedState.pCachedBlob, blob.data());
    EXPECT_EQ(cachedState.CachedBlobSizeInBytes, blob.size());
    EXPECT_EQ(emptyState.pCachedBlob, nullptr);
    EXPECT_EQ(emptyState.CachedBlobSizeInBytes, 0u);
}

TEST(DX12PipelineCacheBlobUtilsTests, RetriesPipelineCreationOnlyWhenCachedBlobWasProvided)
{
    EXPECT_TRUE(NLS::Render::RHI::DX12::ShouldRetryDX12PipelineCreationWithoutCachedBlob(true, E_INVALIDARG));
    EXPECT_FALSE(NLS::Render::RHI::DX12::ShouldRetryDX12PipelineCreationWithoutCachedBlob(false, E_INVALIDARG));
    EXPECT_FALSE(NLS::Render::RHI::DX12::ShouldRetryDX12PipelineCreationWithoutCachedBlob(true, S_OK));
}
#endif
