#include <gtest/gtest.h>

#include "Rendering/RHI/RHITypes.h"

TEST(RHITypesTests, ReportsLayerCountsForExpandedTextureDimensions)
{
    using namespace NLS::Render::RHI;

    EXPECT_EQ(GetTextureLayerCount(TextureDimension::Texture1D), 1u);
    EXPECT_EQ(GetTextureLayerCount(TextureDimension::Texture2D), 1u);
    EXPECT_EQ(GetTextureLayerCount(TextureDimension::Texture2DArray, 4u), 4u);
    EXPECT_EQ(GetTextureLayerCount(TextureDimension::Texture3D, 7u), 1u);
    EXPECT_EQ(GetTextureLayerCount(TextureDimension::TextureCube), 6u);
    EXPECT_EQ(GetTextureLayerCount(TextureDimension::TextureCubeArray, 12u), 12u);
}

TEST(RHITypesTests, ReportsBytesPerPixelForExpandedTextureFormats)
{
    using namespace NLS::Render::RHI;

    EXPECT_EQ(GetTextureFormatBytesPerPixel(TextureFormat::R8), 1u);
    EXPECT_EQ(GetTextureFormatBytesPerPixel(TextureFormat::RG8), 2u);
    EXPECT_EQ(GetTextureFormatBytesPerPixel(TextureFormat::RGB8), 3u);
    EXPECT_EQ(GetTextureFormatBytesPerPixel(TextureFormat::RGBA8), 4u);
    EXPECT_EQ(GetTextureFormatBytesPerPixel(TextureFormat::R16F), 2u);
    EXPECT_EQ(GetTextureFormatBytesPerPixel(TextureFormat::RG16F), 4u);
    EXPECT_EQ(GetTextureFormatBytesPerPixel(TextureFormat::RGBA16F), 8u);
    EXPECT_EQ(GetTextureFormatBytesPerPixel(TextureFormat::R32F), 4u);
    EXPECT_EQ(GetTextureFormatBytesPerPixel(TextureFormat::RG32F), 8u);
    EXPECT_EQ(GetTextureFormatBytesPerPixel(TextureFormat::RGBA32F), 16u);
    EXPECT_EQ(GetTextureFormatBytesPerPixel(TextureFormat::Depth32F), 4u);
    EXPECT_EQ(GetTextureFormatBytesPerPixel(TextureFormat::Depth24Stencil8), 4u);
}

TEST(RHITypesTests, DefaultSamplerDescPreservesExistingBehavior)
{
    using namespace NLS::Render::RHI;

    const SamplerDesc desc{};

    EXPECT_EQ(desc.minFilter, TextureFilter::Linear);
    EXPECT_EQ(desc.magFilter, TextureFilter::Linear);
    EXPECT_EQ(desc.mipFilter, TextureMipFilter::Linear);
    EXPECT_EQ(desc.wrapU, TextureWrap::Repeat);
    EXPECT_EQ(desc.wrapV, TextureWrap::Repeat);
    EXPECT_EQ(desc.wrapW, TextureWrap::Repeat);
    EXPECT_EQ(desc.maxAnisotropy, 1u);
    EXPECT_EQ(desc.minLod, 0.0f);
    EXPECT_EQ(desc.maxLod, SamplerDesc::UnboundedMaxLod);
    EXPECT_EQ(desc.mipLodBias, 0.0f);
    EXPECT_FALSE(desc.compareEnabled);
    EXPECT_EQ(desc.compareFunc, NLS::Render::Settings::EComparaisonAlgorithm::NEVER);
    EXPECT_FLOAT_EQ(desc.borderColor[0], 0.0f);
    EXPECT_FLOAT_EQ(desc.borderColor[1], 0.0f);
    EXPECT_FLOAT_EQ(desc.borderColor[2], 0.0f);
    EXPECT_FLOAT_EQ(desc.borderColor[3], 0.0f);
}
