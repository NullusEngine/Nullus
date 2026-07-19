#include "Assets/AssetThumbnail.h"

#include "Assets/AssetThumbnailPool.h"

#include <utility>

namespace NLS::Editor::Assets
{
AssetThumbnail::AssetThumbnail(
    std::weak_ptr<AssetThumbnailPool> pool,
    std::string cacheKey,
    const uint64_t generation)
    : m_pool(std::move(pool))
    , m_cacheKey(std::move(cacheKey))
    , m_generation(generation)
{
}

AssetThumbnailResolvedTexture AssetThumbnail::Resolve(const uint64_t frameNumber) const
{
    const auto pool = m_pool.lock();
    return pool != nullptr
        ? pool->Resolve(m_cacheKey, m_generation, frameNumber)
        : AssetThumbnailResolvedTexture {};
}

bool AssetThumbnail::IsValid() const
{
    return !m_cacheKey.empty() && !m_pool.expired();
}
}
