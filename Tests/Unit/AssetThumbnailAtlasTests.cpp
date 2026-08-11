#include "Assets/AssetThumbnailAtlas.h"

#include <gtest/gtest.h>

namespace NLS::Editor::Assets
{
TEST(AssetThumbnailAtlasTests, AllocatesGutterAndUvInsidePage)
{
    AssetThumbnailAtlas atlas;
    const auto result = atlas.Allocate("a", 96u, 72u, "linear-96x72", 1u);

    ASSERT_TRUE(result.allocation.has_value());
    const auto& allocation = *result.allocation;
    EXPECT_EQ(allocation.x, 2u);
    EXPECT_EQ(allocation.y, 2u);
    EXPECT_EQ(allocation.width, 96u);
    EXPECT_EQ(allocation.height, 72u);
    EXPECT_FLOAT_EQ(allocation.uv.u0, 2.0f / 2048.0f);
    EXPECT_FLOAT_EQ(allocation.uv.v0, 2.0f / 2048.0f);
    EXPECT_FLOAT_EQ(allocation.uv.u1, 98.0f / 2048.0f);
    EXPECT_FLOAT_EQ(allocation.uv.v1, 74.0f / 2048.0f);
    EXPECT_EQ(atlas.GetPageCount(), 1u);
    EXPECT_EQ(atlas.GetResidentBytes(), AssetThumbnailAtlas::kPageSize * AssetThumbnailAtlas::kPageSize * 4ull);
}

TEST(AssetThumbnailAtlasTests, DoesNotEvictCurrentFrameAllocations)
{
    AssetThumbnailAtlas atlas;
    for (size_t index = 0u; index < 576u; ++index)
    {
        const auto result = atlas.Allocate(
            "asset-" + std::to_string(index),
            160u,
            160u,
            "linear-160",
            7u);
        ASSERT_TRUE(result.allocation.has_value());
    }

    EXPECT_EQ(atlas.GetPageCount(), AssetThumbnailAtlas::kMaxPages);
    const auto result = atlas.Allocate("active", 160u, 160u, "linear-160", 7u);
    EXPECT_FALSE(result.allocation.has_value());
    EXPECT_TRUE(result.evictedKeys.empty());
}

TEST(AssetThumbnailAtlasTests, EvictsLeastRecentlyUsedSlotWhenPageIsFull)
{
    AssetThumbnailAtlas atlas;
    for (size_t index = 0u; index < 576u; ++index)
    {
        const auto result = atlas.Allocate(
            "asset-" + std::to_string(index),
            160u,
            160u,
            "linear-160",
            1u);
        ASSERT_TRUE(result.allocation.has_value());
    }

    ASSERT_EQ(atlas.GetAllocationCount(), 576u);
    const auto result = atlas.Allocate("replacement", 160u, 160u, "linear-160", 2u);
    ASSERT_TRUE(result.allocation.has_value());
    ASSERT_EQ(result.evictedKeys.size(), 1u);
    EXPECT_EQ(result.evictedKeys.front(), "asset-0");
    EXPECT_EQ(atlas.GetAllocationCount(), 576u);
    EXPECT_EQ(atlas.GetPageCount(), AssetThumbnailAtlas::kMaxPages);
}

TEST(AssetThumbnailAtlasTests, ReusesExistingAllocationForSameSizeClass)
{
    AssetThumbnailAtlas atlas;
    const auto first = atlas.Allocate("asset", 96u, 96u, "linear-96x96", 1u);
    ASSERT_TRUE(first.allocation.has_value());
    const auto second = atlas.Allocate("asset", 96u, 96u, "linear-96x96", 2u);

    ASSERT_TRUE(second.allocation.has_value());
    EXPECT_EQ(second.allocation->pageKey, first.allocation->pageKey);
    EXPECT_EQ(second.allocation->x, first.allocation->x);
    EXPECT_EQ(second.allocation->y, first.allocation->y);
    EXPECT_EQ(atlas.GetAllocationCount(), 1u);
}

TEST(AssetThumbnailAtlasTests, SharesPageAcrossAspectRatiosWithinSizeClass)
{
    AssetThumbnailAtlas atlas;
    const auto wide = atlas.Allocate("wide", 96u, 72u, "linear-96", 1u);
    const auto square = atlas.Allocate("square", 80u, 80u, "linear-96", 2u);

    ASSERT_TRUE(wide.allocation.has_value());
    ASSERT_TRUE(square.allocation.has_value());
    EXPECT_EQ(wide.allocation->pageKey, square.allocation->pageKey);
    EXPECT_EQ(atlas.GetPageCount(), 1u);
    ASSERT_TRUE(AssetThumbnailAtlas::SizeClassForDimensions(72u, 64u).has_value());
    EXPECT_EQ(*AssetThumbnailAtlas::SizeClassForDimensions(72u, 64u), 72u);
    ASSERT_TRUE(AssetThumbnailAtlas::SizeClassForDimensions(96u, 72u).has_value());
    EXPECT_EQ(*AssetThumbnailAtlas::SizeClassForDimensions(96u, 72u), 96u);
    EXPECT_FALSE(AssetThumbnailAtlas::SizeClassForDimensions(161u, 96u).has_value());
}

TEST(AssetThumbnailAtlasTests, ResetInvalidatesPageGeneration)
{
    AssetThumbnailAtlas atlas;
    const auto first = atlas.Allocate("first", 96u, 96u, "linear-96x96", 1u);
    ASSERT_TRUE(first.allocation.has_value());

    const auto firstPageKey = first.allocation->pageKey;
    const auto firstGeneration = first.allocation->pageGeneration;
    atlas.Reset();

    const auto second = atlas.Allocate("second", 96u, 96u, "linear-96x96", 2u);
    ASSERT_TRUE(second.allocation.has_value());
    EXPECT_NE(second.allocation->pageKey, firstPageKey);
    EXPECT_NE(second.allocation->pageGeneration, firstGeneration);
}
}
