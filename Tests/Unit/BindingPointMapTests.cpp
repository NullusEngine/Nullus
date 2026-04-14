#include <gtest/gtest.h>

#include "Rendering/RHI/BindingPointMap.h"

TEST(BindingPointMapTests, MapsBindingSpacesToStableExplicitDescriptorSetIndices)
{
    using namespace NLS::Render::RHI;

    EXPECT_EQ(BindingPointMap::GetDescriptorSetIndex(BindingPointMap::kFrameBindingSpace), 0u);
    EXPECT_EQ(BindingPointMap::GetDescriptorSetIndex(BindingPointMap::kMaterialBindingSpace), 1u);
    EXPECT_EQ(BindingPointMap::GetDescriptorSetIndex(BindingPointMap::kObjectBindingSpace), 2u);
    EXPECT_EQ(BindingPointMap::GetDescriptorSetIndex(BindingPointMap::kPassBindingSpace), 3u);
    EXPECT_EQ(BindingPointMap::GetDescriptorSetIndex(17u), 17u);
}

TEST(BindingPointMapTests, ExposesNamedExplicitDescriptorSetIndices)
{
    using namespace NLS::Render::RHI;

    EXPECT_EQ(BindingPointMap::kFrameDescriptorSet, 0u);
    EXPECT_EQ(BindingPointMap::kMaterialDescriptorSet, 1u);
    EXPECT_EQ(BindingPointMap::kObjectDescriptorSet, 2u);
    EXPECT_EQ(BindingPointMap::kPassDescriptorSet, 3u);
}
