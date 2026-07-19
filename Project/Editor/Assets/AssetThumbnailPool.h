#pragma once

#include "Assets/AssetThumbnail.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace NLS::Editor::Assets
{
class AssetThumbnailPool final : public std::enable_shared_from_this<AssetThumbnailPool>
{
public:
    using ResolveTextureId = std::function<void*(
        const std::shared_ptr<NLS::Render::RHI::RHITextureView>&)>;
    using RetireTextureView = std::function<void(
        const std::shared_ptr<NLS::Render::RHI::RHITextureView>&,
        bool immediate)>;

    explicit AssetThumbnailPool(size_t maxResidentTextures = 256u);
    ~AssetThumbnailPool();

    AssetThumbnailPool(const AssetThumbnailPool&) = delete;
    AssetThumbnailPool& operator=(const AssetThumbnailPool&) = delete;

    void SetTextureCallbacks(
        ResolveTextureId resolveTextureId,
        RetireTextureView retireTextureView);
    [[nodiscard]] AssetThumbnail MakeThumbnail(
        std::string cacheKey,
        uint64_t generation = 0u);

    bool Publish(
        const std::string& cacheKey,
        uint64_t generation,
        AssetThumbnailGpuTexture texture);
    void SetStatus(
        const std::string& cacheKey,
        uint64_t generation,
        ThumbnailRenderStatus status);
    [[nodiscard]] AssetThumbnailResolvedTexture Resolve(
        const std::string& cacheKey,
        uint64_t generation,
        uint64_t frameNumber);

    void Prune(uint64_t frameNumber);
    void Remove(const std::string& cacheKey, bool immediate = false);
    void Clear(bool immediate = false);

    [[nodiscard]] size_t GetResidentTextureCount() const;
    [[nodiscard]] ThumbnailRenderStatus GetStatus(
        const std::string& cacheKey,
        uint64_t generation) const;

private:
    struct Entry
    {
        uint64_t generation = 0u;
        uint64_t lastUsedFrame = 0u;
        ThumbnailRenderStatus status = ThumbnailRenderStatus::NotReady;
        AssetThumbnailGpuTexture texture;
        void* textureId = nullptr;
    };

    struct RetiredEntry
    {
        uint64_t retireFrame = 0u;
        AssetThumbnailGpuTexture texture;
    };

    void RetireEntry(Entry& entry, bool immediate);

    size_t m_maxResidentTextures = 0u;
    uint64_t m_currentFrame = 0u;
    std::unordered_map<std::string, Entry> m_entries;
    std::vector<RetiredEntry> m_retiredEntries;
    ResolveTextureId m_resolveTextureId;
    RetireTextureView m_retireTextureView;
};
}
