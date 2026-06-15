#pragma once

#include "Assets/AssetBrowserPresentation.h"
#include "Assets/AssetThumbnailCache.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <future>
#include <optional>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace NLS::Editor::Assets
{
struct AssetThumbnailGenerationCancelToken
{
    std::atomic_bool cancelled {false};
    uint64_t generation = 0u;
};

enum class AssetThumbnailServiceStatus
{
    Fresh,
    Pending,
    Fallback,
    Failed
};

struct AssetThumbnailServiceResult
{
    AssetThumbnailServiceStatus status = AssetThumbnailServiceStatus::Fallback;
    std::optional<AssetThumbnailCacheEntry> cacheEntry;
    std::filesystem::path imagePath;
    std::string fallbackIcon;
    std::string diagnostic;
};

std::optional<AssetThumbnailRequest> BuildAssetThumbnailRequestForItem(
    const std::filesystem::path& projectRoot,
    const AssetBrowserItem& item,
    uint32_t requestedSize);

class AssetThumbnailService
{
public:
    ~AssetThumbnailService();

    AssetThumbnailServiceResult GetThumbnail(const AssetThumbnailRequest& request);
    std::optional<AssetThumbnailServiceResult> GenerateNextThumbnail();
    bool StartNextThumbnailGeneration();
    std::optional<AssetThumbnailServiceResult> ConsumeCompletedThumbnail();
    bool HasInFlightRequest() const;
    size_t GetQueuedRequestCount() const;
    void ClearQueuedRequests();
    void SupersedeQueuedRequestsForGeneration(const std::string& generationFingerprint);

private:
    struct InFlightThumbnailRequest
    {
        std::string cacheKey;
        uint64_t generation = 0u;
        std::shared_ptr<AssetThumbnailGenerationCancelToken> cancelToken;
        std::future<AssetThumbnailServiceResult> future;
    };

    void WaitForInFlightRequests();
    void ClearPendingQueuedRequests();
    bool HasCurrentGenerationInFlightRequest() const;
    bool AdoptMatchingInFlightRequest(const std::string& cacheKey);

    std::queue<std::string> m_queuedCacheKeys;
    std::unordered_map<std::string, AssetThumbnailRequest> m_queuedRequestsByCacheKey;
    std::string m_generationFingerprint;
    uint64_t m_generationSerial = 0u;
    std::shared_ptr<AssetThumbnailGenerationCancelToken> m_generationCancelToken;
    std::vector<InFlightThumbnailRequest> m_inFlightThumbnails;
};
}
