#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

namespace NLS::Render::Sprites
{
struct SpriteSortKey
{
    int32_t layer = 0, order = 0;
    float depth = 0;
    uint64_t stableId = 0;
};
inline bool SpriteSortLess(const SpriteSortKey& a, const SpriteSortKey& b)
{
    if (a.layer != b.layer) return a.layer < b.layer;
    if (a.order != b.order) return a.order < b.order;
    if (a.depth != b.depth) return a.depth > b.depth;
    return a.stableId < b.stableId;
}
struct SpriteBatchKey
{
    uint64_t material = 0, texture = 0, state = 0;
    bool operator==(const SpriteBatchKey&) const = default;
};
struct SpriteBatchItem
{
    SpriteSortKey sorting;
    SpriteBatchKey binding;
    uint32_t sourceIndex = 0;
    bool visible = true;
};
struct SpriteBatch
{
    SpriteBatchKey binding;
    uint32_t first = 0, count = 0;
};
struct SpriteBatchResult
{
    std::vector<SpriteBatchItem> sorted;
    std::vector<SpriteBatch> batches;
    uint32_t submitted = 0, culled = 0, stateBreaks = 0, capacityBreaks = 0, orderingBreaks = 0;
    uint32_t materialBreaks = 0, textureBreaks = 0;
};

// Sorting precedes batching. In particular A/B/A never becomes A/A/B.
inline SpriteBatchResult BuildSpriteBatches(std::span<const SpriteBatchItem> source,
    uint32_t maxSpritesPerBatch = 16384, std::span<const SpriteSortKey> externalDraws = {})
{
    SpriteBatchResult result;
    maxSpritesPerBatch = (std::max)(1u, maxSpritesPerBatch);
    result.submitted = uint32_t(source.size());
    for (const auto& item : source)
    {
        if (!item.visible || !std::isfinite(item.sorting.depth)) { ++result.culled; continue; }
        result.sorted.push_back(item);
    }
    std::stable_sort(result.sorted.begin(), result.sorted.end(), [](const auto& a, const auto& b)
    {
        return SpriteSortLess(a.sorting, b.sorting);
    });
    std::vector<SpriteSortKey> barriers;
    for (const auto& key : externalDraws) if (std::isfinite(key.depth)) barriers.push_back(key);
    std::stable_sort(barriers.begin(), barriers.end(), SpriteSortLess);
    size_t barrier = 0;
    for (uint32_t index = 0; index < result.sorted.size(); ++index)
    {
        const auto& item = result.sorted[index];
        bool orderingBreak = false;
        while (barrier < barriers.size() && SpriteSortLess(barriers[barrier], item.sorting))
        {
            orderingBreak |= index > 0 && !SpriteSortLess(barriers[barrier], result.sorted[index-1].sorting);
            ++barrier;
        }
        const bool stateBreak = !result.batches.empty() && !(result.batches.back().binding == item.binding);
        if (!result.batches.empty())
        {
            result.materialBreaks += result.batches.back().binding.material != item.binding.material;
            result.textureBreaks += result.batches.back().binding.texture != item.binding.texture;
        }
        const bool capacityBreak = !result.batches.empty() && result.batches.back().count == maxSpritesPerBatch;
        if (result.batches.empty() || stateBreak || capacityBreak || orderingBreak)
        {
            result.stateBreaks += stateBreak;
            result.capacityBreaks += !stateBreak && capacityBreak;
            result.orderingBreaks += orderingBreak;
            result.batches.push_back({item.binding, index, 0});
        }
        ++result.batches.back().count;
    }
    return result;
}
}
