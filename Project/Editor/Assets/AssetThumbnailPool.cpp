#include "Assets/AssetThumbnailPool.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace NLS::Editor::Assets
{
AssetThumbnailPool::AssetThumbnailPool(const size_t maxResidentTextures)
    : m_maxResidentTextures((std::max)(maxResidentTextures, size_t {1u}))
{
}

AssetThumbnailPool::~AssetThumbnailPool()
{
    Clear(true);
}

void AssetThumbnailPool::SetTextureCallbacks(
    ResolveTextureId resolveTextureId,
    RetireTextureView retireTextureView)
{
    m_resolveTextureId = std::move(resolveTextureId);
    m_retireTextureView = std::move(retireTextureView);
}

AssetThumbnail AssetThumbnailPool::MakeThumbnail(
    std::string cacheKey,
    const uint64_t generation)
{
    return AssetThumbnail(weak_from_this(), std::move(cacheKey), generation);
}

bool AssetThumbnailPool::Publish(
    const std::string& cacheKey,
    const uint64_t generation,
    AssetThumbnailGpuTexture texture)
{
    if (cacheKey.empty() || !texture.IsValid())
        return false;

    auto [iterator, inserted] = m_entries.try_emplace(cacheKey);
    auto& entry = iterator->second;
    if (!inserted && generation < entry.generation)
        return false;
    if (!inserted && generation == entry.generation &&
        entry.texture.texture == texture.texture &&
        entry.texture.textureView == texture.textureView)
    {
        entry.status = ThumbnailRenderStatus::Ready;
        return true;
    }

    if (!inserted)
        RetireEntry(entry, false);
    entry.generation = generation;
    entry.status = ThumbnailRenderStatus::Ready;
    entry.texture = std::move(texture);
    entry.textureId = nullptr;
    return true;
}

void AssetThumbnailPool::SetStatus(
    const std::string& cacheKey,
    const uint64_t generation,
    const ThumbnailRenderStatus status)
{
    if (cacheKey.empty())
        return;

    auto [iterator, inserted] = m_entries.try_emplace(cacheKey);
    auto& entry = iterator->second;
    if (!inserted && generation < entry.generation)
        return;
    if (status != ThumbnailRenderStatus::Ready && entry.status == ThumbnailRenderStatus::Ready)
        return;
    entry.generation = generation;
    entry.status = status;
}

AssetThumbnailResolvedTexture AssetThumbnailPool::Resolve(
    const std::string& cacheKey,
    const uint64_t generation,
    const uint64_t frameNumber)
{
    const auto found = m_entries.find(cacheKey);
    if (found == m_entries.end() ||
        (generation != 0u && found->second.generation != generation))
        return {};

    auto& entry = found->second;
    entry.lastUsedFrame = frameNumber;
    if (entry.status != ThumbnailRenderStatus::Ready || !entry.texture.IsValid())
        return { entry.status };

    if (entry.textureId == nullptr && m_resolveTextureId)
        entry.textureId = m_resolveTextureId(entry.texture.textureView);
    return {
        entry.textureId != nullptr ? ThumbnailRenderStatus::Ready : ThumbnailRenderStatus::NotReady,
        entry.textureId,
        entry.texture.width,
        entry.texture.height
    };
}

void AssetThumbnailPool::Prune(const uint64_t frameNumber)
{
    m_currentFrame = frameNumber;
    constexpr uint64_t kDeferredReleaseFrameCount = 3u;
    m_retiredEntries.erase(
        std::remove_if(
            m_retiredEntries.begin(),
            m_retiredEntries.end(),
            [frameNumber](const RetiredEntry& entry)
            {
                return entry.retireFrame + kDeferredReleaseFrameCount <= frameNumber;
            }),
        m_retiredEntries.end());

    if (m_entries.size() <= m_maxResidentTextures)
        return;

    std::vector<std::pair<std::string, uint64_t>> candidates;
    candidates.reserve(m_entries.size());
    for (const auto& [key, entry] : m_entries)
    {
        if (entry.lastUsedFrame != frameNumber)
            candidates.emplace_back(key, entry.lastUsedFrame);
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right)
    {
        return left.second < right.second;
    });

    size_t residentCount = m_entries.size();
    for (const auto& [key, _] : candidates)
    {
        if (residentCount <= m_maxResidentTextures)
            break;
        Remove(key, false);
        --residentCount;
    }
}

void AssetThumbnailPool::Remove(const std::string& cacheKey, const bool immediate)
{
    const auto found = m_entries.find(cacheKey);
    if (found == m_entries.end())
        return;
    RetireEntry(found->second, immediate);
    m_entries.erase(found);
}

void AssetThumbnailPool::Clear(const bool immediate)
{
    for (auto& [_, entry] : m_entries)
        RetireEntry(entry, immediate);
    m_entries.clear();
    if (immediate)
        m_retiredEntries.clear();
}

size_t AssetThumbnailPool::GetResidentTextureCount() const
{
    return m_entries.size();
}

ThumbnailRenderStatus AssetThumbnailPool::GetStatus(
    const std::string& cacheKey,
    const uint64_t generation) const
{
    const auto found = m_entries.find(cacheKey);
    return found != m_entries.end() &&
        (generation == 0u || found->second.generation == generation)
        ? found->second.status
        : ThumbnailRenderStatus::NotReady;
}

void AssetThumbnailPool::RetireEntry(Entry& entry, const bool immediate)
{
    if (entry.textureId != nullptr && entry.texture.textureView != nullptr && m_retireTextureView)
        m_retireTextureView(entry.texture.textureView, immediate);
    entry.textureId = nullptr;
    if (!immediate && entry.texture.IsValid())
        m_retiredEntries.push_back({ m_currentFrame, entry.texture });
    entry.texture = {};
}
}
