#pragma once

#include <filesystem>
#include <vector>

#include "Rendering/RHI/Utils/PipelineCache/PipelineCache.h"

#if defined(_WIN32)
#include <d3d12.h>
#endif

namespace NLS::Render::RHI::DX12
{
    struct NLS_RENDER_API DX12PipelineCacheBlobPaths
    {
        std::filesystem::path directory;
        std::filesystem::path blobPath;
        std::filesystem::path temporaryPath;
    };

    NLS_RENDER_API DX12PipelineCacheBlobPaths BuildDX12PipelineCacheBlobPaths(
        const PipelineCacheKey& key,
        std::string_view pipelineKind,
        const std::filesystem::path& cacheRoot);

    NLS_RENDER_API std::filesystem::path GetDX12PipelineCacheBlobRoot();
    NLS_RENDER_API std::vector<uint8_t> ReadDX12PipelineCacheBlob(const std::filesystem::path& path);
    NLS_RENDER_API bool WriteDX12PipelineCacheBlobAtomically(
        const std::filesystem::path& path,
        const void* data,
        size_t size);

#if defined(_WIN32)
    NLS_RENDER_API D3D12_CACHED_PIPELINE_STATE BuildDX12CachedPipelineStateView(const std::vector<uint8_t>& blob);
    NLS_RENDER_API bool ShouldRetryDX12PipelineCreationWithoutCachedBlob(bool usedCachedBlob, HRESULT result);
#endif
}
