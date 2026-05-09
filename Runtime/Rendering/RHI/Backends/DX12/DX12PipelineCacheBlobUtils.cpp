#include "Rendering/RHI/Backends/DX12/DX12PipelineCacheBlobUtils.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>

namespace NLS::Render::RHI::DX12
{
    namespace
    {
        std::string ToHex(uint64_t value)
        {
            std::ostringstream stream;
            stream << std::hex << value;
            return stream.str();
        }

        std::string SanitizePathPart(std::string value)
        {
            for (char& ch : value)
            {
                const unsigned char byte = static_cast<unsigned char>(ch);
                if (!std::isalnum(byte) && ch != '-' && ch != '_')
                    ch = '_';
            }
            return value.empty() ? std::string("unnamed") : value;
        }
    }

    DX12PipelineCacheBlobPaths BuildDX12PipelineCacheBlobPaths(
        const PipelineCacheKey& key,
        std::string_view pipelineKind,
        const std::filesystem::path& cacheRoot)
    {
        DX12PipelineCacheBlobPaths paths;
        paths.directory = cacheRoot / "DX12PipelineState";

        const std::string kind = SanitizePathPart(std::string(pipelineKind));
        const std::string debugName = SanitizePathPart(key.stableDebugName);
        const std::string fileName = kind + "_" + ToHex(key.hash) + "_" + debugName + ".pso";
        paths.blobPath = paths.directory / fileName;
        paths.temporaryPath = paths.blobPath;
        paths.temporaryPath += ".tmp";
        return paths;
    }

    std::filesystem::path GetDX12PipelineCacheBlobRoot()
    {
#if defined(_WIN32)
        if (const char* localAppData = std::getenv("LOCALAPPDATA"); localAppData != nullptr && *localAppData != '\0')
            return std::filesystem::path(localAppData) / "Nullus" / "PipelineCache";
#endif
        return std::filesystem::temp_directory_path() / "NullusPipelineCache";
    }

    std::vector<uint8_t> ReadDX12PipelineCacheBlob(const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
            return {};

        return std::vector<uint8_t>(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    }

    bool WriteDX12PipelineCacheBlobAtomically(
        const std::filesystem::path& path,
        const void* data,
        size_t size)
    {
        if (data == nullptr || size == 0u)
            return false;

        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error)
            return false;

        auto temporaryPath = path;
        temporaryPath += ".tmp";

        {
            std::ofstream stream(temporaryPath, std::ios::binary | std::ios::trunc);
            if (!stream)
                return false;

            stream.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
            if (!stream)
                return false;
        }

        std::filesystem::rename(temporaryPath, path, error);
        if (!error)
            return true;

        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(temporaryPath, path, error);
        if (!error)
            return true;

        std::filesystem::remove(temporaryPath, error);
        return false;
    }

#if defined(_WIN32)
    D3D12_CACHED_PIPELINE_STATE BuildDX12CachedPipelineStateView(const std::vector<uint8_t>& blob)
    {
        D3D12_CACHED_PIPELINE_STATE state = {};
        if (!blob.empty())
        {
            state.pCachedBlob = blob.data();
            state.CachedBlobSizeInBytes = blob.size();
        }
        return state;
    }

    bool ShouldRetryDX12PipelineCreationWithoutCachedBlob(bool usedCachedBlob, HRESULT result)
    {
        return usedCachedBlob && FAILED(result);
    }
#endif
}
