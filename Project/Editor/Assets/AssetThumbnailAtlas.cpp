#include "Assets/AssetThumbnailAtlas.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <utility>

namespace NLS::Editor::Assets
{
namespace
{
std::atomic_uint64_t g_nextAtlasInstanceId {1u};
}

AssetThumbnailAtlas::AssetThumbnailAtlas()
    : m_instanceId(g_nextAtlasInstanceId.fetch_add(1u, std::memory_order_relaxed))
{
    if (m_instanceId == 0u)
        m_instanceId = g_nextAtlasInstanceId.fetch_add(1u, std::memory_order_relaxed);
}

std::optional<uint32_t> AssetThumbnailAtlas::SizeClassForDimensions(
    const uint32_t width,
    const uint32_t height)
{
    const uint32_t dimension = (std::max)(width, height);
    if (dimension == 0u)
        return std::nullopt;
    if (dimension <= 72u)
        return 72u;
    if (dimension <= 96u)
        return 96u;
    if (dimension <= 128u)
        return 128u;
    if (dimension <= 160u)
        return 160u;
    return std::nullopt;
}

AssetThumbnailAtlas::Page AssetThumbnailAtlas::MakePage(
    std::string bucketKey,
    const uint32_t width,
    const uint32_t height,
    const size_t pageIndex) const
{
    Page page;
    page.bucketKey = std::move(bucketKey);
    page.cellWidth = width + kGutter * 2u;
    page.cellHeight = height + kGutter * 2u;
    page.columns = kPageSize / page.cellWidth;
    page.rows = kPageSize / page.cellHeight;
    page.generation = m_nextPageGeneration++;
    page.pageKey = "asset-browser-thumbnail-atlas-" + std::to_string(m_instanceId) +
        "-p" + std::to_string(pageIndex) + "-g" + std::to_string(page.generation);
    page.slots.resize(static_cast<size_t>(page.columns) * static_cast<size_t>(page.rows));
    return page;
}

void AssetThumbnailAtlas::ResetPage(
    Page& page,
    std::string bucketKey,
    const uint32_t width,
    const uint32_t height,
    const size_t pageIndex)
{
    page = MakePage(std::move(bucketKey), width, height, pageIndex);
}

size_t AssetThumbnailAtlas::FindFreeSlot(const Page& page)
{
    for (size_t index = 0u; index < page.slots.size(); ++index)
    {
        if (page.slots[index].assetKey.empty())
            return index;
    }
    return std::numeric_limits<size_t>::max();
}

AssetThumbnailAtlas::Allocation AssetThumbnailAtlas::BuildAllocation(
    const Page& page,
    const size_t pageIndex,
    const size_t slotIndex,
    const uint32_t width,
    const uint32_t height)
{
    const uint32_t column = static_cast<uint32_t>(slotIndex % page.columns);
    const uint32_t row = static_cast<uint32_t>(slotIndex / page.columns);
    const uint32_t cellX = column * page.cellWidth;
    const uint32_t cellY = row * page.cellHeight;
    const uint32_t x = cellX + kGutter;
    const uint32_t y = cellY + kGutter;

    (void)pageIndex;
    Allocation allocation;
    allocation.pageKey = page.pageKey;
    allocation.x = x;
    allocation.y = y;
    allocation.width = width;
    allocation.height = height;
    allocation.pageGeneration = page.generation;
    allocation.uv = {
        static_cast<float>(x) / static_cast<float>(kPageSize),
        static_cast<float>(y) / static_cast<float>(kPageSize),
        static_cast<float>(x + width) / static_cast<float>(kPageSize),
        static_cast<float>(y + height) / static_cast<float>(kPageSize)
    };
    return allocation;
}

std::optional<std::pair<size_t, size_t>> AssetThumbnailAtlas::FindEvictableSlot(
    const uint64_t frameNumber,
    const std::string& bucketKey,
    const uint32_t cellWidth,
    const uint32_t cellHeight,
    std::vector<std::string>& evictedKeys) const
{
    size_t selectedPage = std::numeric_limits<size_t>::max();
    size_t selectedSlot = std::numeric_limits<size_t>::max();
    uint64_t selectedFrame = std::numeric_limits<uint64_t>::max();
    for (size_t pageIndex = 0u; pageIndex < m_pages.size(); ++pageIndex)
    {
        const auto& page = m_pages[pageIndex];
        if (page.bucketKey != bucketKey ||
            page.cellWidth != cellWidth ||
            page.cellHeight != cellHeight)
        {
            continue;
        }
        for (size_t slotIndex = 0u; slotIndex < page.slots.size(); ++slotIndex)
        {
            const auto& slot = page.slots[slotIndex];
            if (slot.assetKey.empty() || slot.lastUsedFrame >= frameNumber)
                continue;
            if (slot.lastUsedFrame < selectedFrame)
            {
                selectedFrame = slot.lastUsedFrame;
                selectedPage = pageIndex;
                selectedSlot = slotIndex;
            }
        }
    }
    if (selectedPage == std::numeric_limits<size_t>::max())
        return std::nullopt;

    evictedKeys.push_back(m_pages[selectedPage].slots[selectedSlot].assetKey);
    return std::make_pair(selectedPage, selectedSlot);
}

void AssetThumbnailAtlas::RemoveRecord(const std::string& assetKey)
{
    const auto found = m_allocations.find(assetKey);
    if (found == m_allocations.end())
        return;
    auto& page = m_pages[found->second.pageIndex];
    if (found->second.slotIndex < page.slots.size())
        page.slots[found->second.slotIndex] = {};
    m_allocations.erase(found);
}

AssetThumbnailAtlas::AllocateResult AssetThumbnailAtlas::Allocate(
    const std::string& assetKey,
    const uint32_t width,
    const uint32_t height,
    const std::string& bucketKey,
    const uint64_t frameNumber)
{
    AllocateResult result;
    if (assetKey.empty() || bucketKey.empty() || width == 0u || height == 0u ||
        width > kPageSize - kGutter * 2u || height > kPageSize - kGutter * 2u)
    {
        return result;
    }

    const auto sizeClass = SizeClassForDimensions(width, height);
    if (!sizeClass.has_value())
        return result;

    if (const auto existing = m_allocations.find(assetKey); existing != m_allocations.end())
    {
        const auto& allocation = existing->second.allocation;
        auto& page = m_pages[existing->second.pageIndex];
        if (allocation.width == width && allocation.height == height && page.bucketKey == bucketKey)
        {
            existing->second.lastUsedFrame = frameNumber;
            page.slots[existing->second.slotIndex].lastUsedFrame = frameNumber;
            result.allocation = allocation;
            return result;
        }
        RemoveRecord(assetKey);
    }

    const uint32_t cellWidth = *sizeClass + kGutter * 2u;
    const uint32_t cellHeight = *sizeClass + kGutter * 2u;
    size_t pageIndex = std::numeric_limits<size_t>::max();
    size_t slotIndex = std::numeric_limits<size_t>::max();
    for (size_t index = 0u; index < m_pages.size(); ++index)
    {
        const auto& page = m_pages[index];
        if (page.bucketKey != bucketKey || page.cellWidth != cellWidth || page.cellHeight != cellHeight)
            continue;
        const auto freeSlot = FindFreeSlot(page);
        if (freeSlot != std::numeric_limits<size_t>::max())
        {
            pageIndex = index;
            slotIndex = freeSlot;
            break;
        }
    }

    if (pageIndex == std::numeric_limits<size_t>::max() && m_pages.size() < kMaxPages)
    {
        m_pages.push_back(MakePage(bucketKey, *sizeClass, *sizeClass, m_pages.size()));
        pageIndex = m_pages.size() - 1u;
        slotIndex = 0u;
    }

    if (pageIndex == std::numeric_limits<size_t>::max())
    {
        std::vector<std::string> evictedKeys;
        const auto evictablePage = FindEvictableSlot(
            frameNumber,
            bucketKey,
            cellWidth,
            cellHeight,
            evictedKeys);
        if (!evictablePage.has_value())
            return result;

        pageIndex = evictablePage->first;
        auto& page = m_pages[pageIndex];
        result.evictedKeys = std::move(evictedKeys);
        if (!result.evictedKeys.empty())
            RemoveRecord(result.evictedKeys.front());
        slotIndex = evictablePage->second;
        if (slotIndex >= page.slots.size() || !page.slots[slotIndex].assetKey.empty())
            return result;
    }

    auto& page = m_pages[pageIndex];
    auto allocation = BuildAllocation(page, pageIndex, slotIndex, width, height);
    page.slots[slotIndex] = { assetKey, frameNumber };
    m_allocations.insert_or_assign(assetKey, AllocationRecord { allocation, pageIndex, slotIndex, frameNumber });
    result.allocation = std::move(allocation);
    return result;
}

bool AssetThumbnailAtlas::Touch(const std::string& assetKey, const uint64_t frameNumber)
{
    const auto found = m_allocations.find(assetKey);
    if (found == m_allocations.end())
        return false;
    found->second.lastUsedFrame = frameNumber;
    auto& page = m_pages[found->second.pageIndex];
    if (found->second.slotIndex >= page.slots.size())
        return false;
    page.slots[found->second.slotIndex].lastUsedFrame = frameNumber;
    return true;
}

bool AssetThumbnailAtlas::Release(const std::string& assetKey)
{
    if (m_allocations.find(assetKey) == m_allocations.end())
        return false;
    RemoveRecord(assetKey);
    return true;
}

void AssetThumbnailAtlas::Reset()
{
    m_allocations.clear();
    m_pages.clear();
}

const AssetThumbnailAtlas::Allocation* AssetThumbnailAtlas::Find(const std::string& assetKey) const
{
    const auto found = m_allocations.find(assetKey);
    return found == m_allocations.end() ? nullptr : &found->second.allocation;
}
}
