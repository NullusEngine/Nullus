#include <gtest/gtest.h>

#include "Rendering/RHI/Backends/DX12/DX12TextureUploadUtils.h"

TEST(DX12TextureUploadUtilsTests, BuildsSingleSubresourcePlanFor2DTexture)
{
    NLS::Render::RHI::RHITextureDesc desc;
    desc.extent.width = 4;
    desc.extent.height = 2;
    desc.extent.depth = 1;
    desc.dimension = NLS::Render::RHI::TextureDimension::Texture2D;
    desc.format = NLS::Render::RHI::TextureFormat::RGBA8;
    desc.mipLevels = 1;

    const auto plan = NLS::Render::RHI::DX12::BuildDX12TextureUploadPlan(desc);

    ASSERT_EQ(plan.subresources.size(), 1u);
    EXPECT_EQ(plan.totalBytes, 32u);

    const auto& subresource = plan.subresources[0];
    EXPECT_EQ(subresource.mipLevel, 0u);
    EXPECT_EQ(subresource.arrayLayer, 0u);
    EXPECT_EQ(subresource.width, 4u);
    EXPECT_EQ(subresource.height, 2u);
    EXPECT_EQ(subresource.depth, 1u);
    EXPECT_EQ(subresource.dataOffset, 0u);
    EXPECT_EQ(subresource.rowPitch, 16u);
    EXPECT_EQ(subresource.slicePitch, 32u);
}

TEST(DX12TextureUploadUtilsTests, BuildsPerFacePlanForCubeTexture)
{
    NLS::Render::RHI::RHITextureDesc desc;
    desc.extent.width = 2;
    desc.extent.height = 2;
    desc.extent.depth = 1;
    desc.dimension = NLS::Render::RHI::TextureDimension::TextureCube;
    desc.format = NLS::Render::RHI::TextureFormat::RGBA8;
    desc.mipLevels = 1;

    const auto plan = NLS::Render::RHI::DX12::BuildDX12TextureUploadPlan(desc);

    ASSERT_EQ(plan.subresources.size(), 6u);
    EXPECT_EQ(plan.totalBytes, 96u);

    for (size_t face = 0; face < plan.subresources.size(); ++face)
    {
        const auto& subresource = plan.subresources[face];
        EXPECT_EQ(subresource.mipLevel, 0u);
        EXPECT_EQ(subresource.arrayLayer, face);
        EXPECT_EQ(subresource.width, 2u);
        EXPECT_EQ(subresource.height, 2u);
        EXPECT_EQ(subresource.depth, 1u);
        EXPECT_EQ(subresource.dataOffset, face * 16u);
        EXPECT_EQ(subresource.rowPitch, 8u);
        EXPECT_EQ(subresource.slicePitch, 16u);
    }
}

TEST(DX12TextureUploadUtilsTests, PacksMipChainInDx12SubresourceOrder)
{
    NLS::Render::RHI::RHITextureDesc desc;
    desc.extent.width = 4;
    desc.extent.height = 4;
    desc.extent.depth = 1;
    desc.dimension = NLS::Render::RHI::TextureDimension::Texture2D;
    desc.format = NLS::Render::RHI::TextureFormat::RGBA8;
    desc.mipLevels = 3;

    const auto plan = NLS::Render::RHI::DX12::BuildDX12TextureUploadPlan(desc);

    ASSERT_EQ(plan.subresources.size(), 3u);
    EXPECT_EQ(plan.totalBytes, 84u);

    EXPECT_EQ(plan.subresources[0].mipLevel, 0u);
    EXPECT_EQ(plan.subresources[0].dataOffset, 0u);
    EXPECT_EQ(plan.subresources[0].rowPitch, 16u);
    EXPECT_EQ(plan.subresources[0].slicePitch, 64u);

    EXPECT_EQ(plan.subresources[1].mipLevel, 1u);
    EXPECT_EQ(plan.subresources[1].dataOffset, 64u);
    EXPECT_EQ(plan.subresources[1].rowPitch, 8u);
    EXPECT_EQ(plan.subresources[1].slicePitch, 16u);

    EXPECT_EQ(plan.subresources[2].mipLevel, 2u);
    EXPECT_EQ(plan.subresources[2].dataOffset, 80u);
    EXPECT_EQ(plan.subresources[2].rowPitch, 4u);
    EXPECT_EQ(plan.subresources[2].slicePitch, 4u);
}
