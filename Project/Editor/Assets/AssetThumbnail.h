#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace NLS::Render::RHI
{
class RHITexture;
class RHITextureView;
}

namespace NLS::Editor::Assets
{
class AssetThumbnailPool;

enum class ThumbnailRenderStatus : uint8_t
{
    Ready,
    NotReady,
    Unsupported,
    Failed
};

struct AssetThumbnailGpuTexture
{
    std::shared_ptr<NLS::Render::RHI::RHITexture> texture;
    std::shared_ptr<NLS::Render::RHI::RHITextureView> textureView;
    std::shared_ptr<void> renderTargetLease;
    uint32_t width = 0u;
    uint32_t height = 0u;

    [[nodiscard]] bool IsValid() const
    {
        return texture != nullptr && textureView != nullptr &&
            renderTargetLease != nullptr && width != 0u && height != 0u;
    }
};

struct AssetThumbnailResolvedTexture
{
    ThumbnailRenderStatus status = ThumbnailRenderStatus::NotReady;
    void* textureId = nullptr;
    uint32_t width = 0u;
    uint32_t height = 0u;

    [[nodiscard]] bool IsReady() const
    {
        return status == ThumbnailRenderStatus::Ready && textureId != nullptr;
    }
};

class AssetThumbnail
{
public:
    AssetThumbnail() = default;
    AssetThumbnail(
        std::weak_ptr<AssetThumbnailPool> pool,
        std::string cacheKey,
        uint64_t generation);

    [[nodiscard]] AssetThumbnailResolvedTexture Resolve(uint64_t frameNumber) const;
    [[nodiscard]] bool IsValid() const;

private:
    std::weak_ptr<AssetThumbnailPool> m_pool;
    std::string m_cacheKey;
    uint64_t m_generation = 0u;
};
}
