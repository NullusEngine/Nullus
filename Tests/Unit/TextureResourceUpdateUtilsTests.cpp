#include <gtest/gtest.h>

#include "Rendering/Resources/TextureResourceUpdateUtils.h"

namespace
{
    NLS::Render::RHI::RHITextureDesc MakeExistingDesc(
        uint32_t width,
        uint32_t height,
        NLS::Render::RHI::TextureFormat format)
    {
        NLS::Render::RHI::RHITextureDesc desc;
        desc.extent.width = width;
        desc.extent.height = height;
        desc.extent.depth = 1;
        desc.dimension = NLS::Render::RHI::TextureDimension::Texture2D;
        desc.format = format;
        return desc;
    }
}

TEST(TextureResourceUpdateUtilsTests, DoesNotRecreateWhenDescriptorAlreadyMatchesAndNoInitialDataIsProvided)
{
    const auto existingDesc = MakeExistingDesc(1, 1, NLS::Render::RHI::TextureFormat::RGBA8);

    EXPECT_FALSE(NLS::Render::Resources::ShouldRecreateRHITexture(
        existingDesc,
        1,
        1,
        NLS::Render::RHI::TextureFormat::RGBA8,
        nullptr));
}

TEST(TextureResourceUpdateUtilsTests, RecreatesWhenInitialDataIsProvidedEvenIfDimensionsMatch)
{
    const auto existingDesc = MakeExistingDesc(1, 1, NLS::Render::RHI::TextureFormat::RGBA8);
    const uint32_t whitePixel = 0xFFFFFFFFu;

    EXPECT_TRUE(NLS::Render::Resources::ShouldRecreateRHITexture(
        existingDesc,
        1,
        1,
        NLS::Render::RHI::TextureFormat::RGBA8,
        &whitePixel));
}

TEST(TextureResourceUpdateUtilsTests, RecreatesWhenFormatChangesEvenIfDimensionsMatch)
{
    const auto existingDesc = MakeExistingDesc(1, 1, NLS::Render::RHI::TextureFormat::RGBA8);

    EXPECT_TRUE(NLS::Render::Resources::ShouldRecreateRHITexture(
        existingDesc,
        1,
        1,
        NLS::Render::RHI::TextureFormat::RGBA16F,
        nullptr));
}

TEST(TextureResourceUpdateUtilsTests, RecreatesWhenDimensionsChange)
{
    const auto existingDesc = MakeExistingDesc(1, 1, NLS::Render::RHI::TextureFormat::RGBA8);

    EXPECT_TRUE(NLS::Render::Resources::ShouldRecreateRHITexture(
        existingDesc,
        2,
        1,
        NLS::Render::RHI::TextureFormat::RGBA8,
        nullptr));
}
