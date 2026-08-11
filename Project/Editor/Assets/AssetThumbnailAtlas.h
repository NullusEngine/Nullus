#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <string>
#include <unordered_map>
#include <vector>

namespace NLS::Editor::Assets
{
class AssetThumbnailAtlas final
{
public:
    static constexpr uint32_t kPageSize = 2048u;
    static constexpr uint32_t kMaxPages = 4u;
    static constexpr uint32_t kGutter = 2u;
    static constexpr size_t kMaxBytes = 64ull * 1024ull * 1024ull;

    struct UvRect
    {
        float u0 = 0.0f;
        float v0 = 0.0f;
        float u1 = 1.0f;
        float v1 = 1.0f;
    };

    struct Allocation
    {
        std::string pageKey;
        uint32_t x = 0u;
        uint32_t y = 0u;
        uint32_t width = 0u;
        uint32_t height = 0u;
        uint32_t pageGeneration = 0u;
        UvRect uv;

        [[nodiscard]] bool IsValid() const
        {
            return !pageKey.empty() && width != 0u && height != 0u;
        }
    };

    struct AllocateResult
    {
        std::optional<Allocation> allocation;
        std::vector<std::string> evictedKeys;
    };

    AssetThumbnailAtlas();

    AssetThumbnailAtlas(const AssetThumbnailAtlas&) = delete;
    AssetThumbnailAtlas& operator=(const AssetThumbnailAtlas&) = delete;

    // Atlas pages use a bounded set of size classes so thumbnails with
    // different aspect ratios can share a page without moving live entries.
    [[nodiscard]] static std::optional<uint32_t> SizeClassForDimensions(
        uint32_t width,
        uint32_t height);

    [[nodiscard]] AllocateResult Allocate(
        const std::string& assetKey,
        uint32_t width,
        uint32_t height,
        const std::string& bucketKey,
        uint64_t frameNumber);
    bool Touch(const std::string& assetKey, uint64_t frameNumber);
    bool Release(const std::string& assetKey);
    void Reset();

    [[nodiscard]] size_t GetPageCount() const { return m_pages.size(); }
    [[nodiscard]] size_t GetAllocationCount() const { return m_allocations.size(); }
    [[nodiscard]] size_t GetResidentBytes() const { return m_pages.size() * kPageSize * kPageSize * 4ull; }
    [[nodiscard]] const Allocation* Find(const std::string& assetKey) const;

private:
    struct Slot
    {
        std::string assetKey;
        uint64_t lastUsedFrame = 0u;
    };

    struct Page
    {
        std::string bucketKey;
        std::string pageKey;
        uint32_t cellWidth = 0u;
        uint32_t cellHeight = 0u;
        uint32_t columns = 0u;
        uint32_t rows = 0u;
        uint32_t generation = 1u;
        std::vector<Slot> slots;
    };

    struct AllocationRecord
    {
        Allocation allocation;
        size_t pageIndex = 0u;
        size_t slotIndex = 0u;
        uint64_t lastUsedFrame = 0u;
    };

    [[nodiscard]] static Allocation BuildAllocation(
        const Page& page,
        size_t pageIndex,
        size_t slotIndex,
        uint32_t width,
        uint32_t height);
    [[nodiscard]] Page MakePage(
        std::string bucketKey,
        uint32_t width,
        uint32_t height,
        size_t pageIndex) const;
    [[nodiscard]] static size_t FindFreeSlot(const Page& page);
    [[nodiscard]] std::optional<std::pair<size_t, size_t>> FindEvictableSlot(
        uint64_t frameNumber,
        const std::string& bucketKey,
        uint32_t cellWidth,
        uint32_t cellHeight,
        std::vector<std::string>& evictedKeys) const;
    void ResetPage(Page& page, std::string bucketKey, uint32_t width, uint32_t height, size_t pageIndex);
    void RemoveRecord(const std::string& assetKey);

    uint64_t m_instanceId = 0u;
    mutable uint32_t m_nextPageGeneration = 1u;
    std::vector<Page> m_pages;
    std::unordered_map<std::string, AllocationRecord> m_allocations;
};
}
