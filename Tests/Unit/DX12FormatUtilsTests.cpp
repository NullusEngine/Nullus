#include <gtest/gtest.h>

#include "Rendering/RHI/Backends/DX12/DX12FormatUtils.h"
#include "Rendering/RHI/Backends/DX12/DX12TextureViewUtils.h"
#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/RHI/RHITypes.h"

#if defined(_WIN32)

using namespace NLS::Render::RHI;
using namespace NLS::Render::RHI::DX12;

TEST(DX12FormatUtilsTests, DescribesFirstScopeTextureFormats)
{
    const auto* rgba8 = GetTextureFormatDescriptor(TextureFormat::RGBA8);
    ASSERT_NE(rgba8, nullptr);
    EXPECT_EQ(rgba8->family, TextureFormatFamily::Uncompressed);
    EXPECT_EQ(rgba8->blockWidth, 1u);
    EXPECT_EQ(rgba8->blockHeight, 1u);
    EXPECT_EQ(rgba8->bytesPerBlock, 4u);
    EXPECT_TRUE(rgba8->supportsSrgbView);

    const auto* rgba16f = GetTextureFormatDescriptor(TextureFormat::RGBA16F);
    ASSERT_NE(rgba16f, nullptr);
    EXPECT_EQ(rgba16f->family, TextureFormatFamily::Uncompressed);
    EXPECT_EQ(rgba16f->bytesPerBlock, 8u);
    EXPECT_TRUE(rgba16f->isHDR);

    const auto* bc1 = GetTextureFormatDescriptor(TextureFormat::BC1);
    ASSERT_NE(bc1, nullptr);
    EXPECT_EQ(bc1->family, TextureFormatFamily::BC);
    EXPECT_TRUE(bc1->isCompressed);
    EXPECT_EQ(bc1->blockWidth, 4u);
    EXPECT_EQ(bc1->blockHeight, 4u);
    EXPECT_EQ(bc1->bytesPerBlock, 8u);
    EXPECT_TRUE(bc1->supportsSrgbView);

    const auto* bc7 = GetTextureFormatDescriptor(TextureFormat::BC7);
    ASSERT_NE(bc7, nullptr);
    EXPECT_EQ(bc7->family, TextureFormatFamily::BC);
    EXPECT_EQ(bc7->bytesPerBlock, 16u);
    EXPECT_TRUE(bc7->supportsSrgbView);

    const auto* bc5 = GetTextureFormatDescriptor(TextureFormat::BC5);
    ASSERT_NE(bc5, nullptr);
    EXPECT_EQ(bc5->family, TextureFormatFamily::BC);
    EXPECT_EQ(bc5->bytesPerBlock, 16u);
    EXPECT_FALSE(bc5->supportsSrgbView);

    const auto* bc6h = GetTextureFormatDescriptor(TextureFormat::BC6H);
    ASSERT_NE(bc6h, nullptr);
    EXPECT_EQ(bc6h->family, TextureFormatFamily::BC);
    EXPECT_TRUE(bc6h->isCompressed);
    EXPECT_TRUE(bc6h->isHDR);
    EXPECT_FALSE(bc6h->supportsUpload);

    const auto* astc = GetTextureFormatDescriptor(TextureFormat::ASTC4x4);
    ASSERT_NE(astc, nullptr);
    EXPECT_EQ(astc->family, TextureFormatFamily::ASTC);
    EXPECT_TRUE(astc->isCompressed);
    EXPECT_FALSE(astc->sampled);

    const auto* etc2 = GetTextureFormatDescriptor(TextureFormat::ETC2RGBA8);
    ASSERT_NE(etc2, nullptr);
    EXPECT_EQ(etc2->family, TextureFormatFamily::ETC2);
    EXPECT_TRUE(etc2->isCompressed);
    EXPECT_FALSE(etc2->supportsUpload);
}

TEST(DX12FormatUtilsTests, CalculatesBlockCompressedPitchAndSliceSizes)
{
    EXPECT_EQ(CalculateTextureRowPitch(TextureFormat::RGBA8, 7u), 28u);
    EXPECT_EQ(CalculateTextureSlicePitch(TextureFormat::RGBA8, 7u, 3u, 1u), 84u);

    EXPECT_EQ(CalculateTextureRowPitch(TextureFormat::BC1, 7u), 16u);
    EXPECT_EQ(CalculateTextureSlicePitch(TextureFormat::BC1, 7u, 5u, 1u), 32u);

    EXPECT_EQ(CalculateTextureRowPitch(TextureFormat::BC7, 9u), 48u);
    EXPECT_EQ(CalculateTextureSlicePitch(TextureFormat::BC7, 9u, 5u, 1u), 96u);

    EXPECT_EQ(GetTextureFormatBytesPerPixel(TextureFormat::BC1), 0u);
    EXPECT_EQ(GetTextureFormatBytesPerPixel(TextureFormat::BC7), 0u);
}

TEST(DX12FormatUtilsTests, MapsSrgbCapableFormatsToExpectedDxgiVariants)
{
    EXPECT_EQ(ToDXGIFormat(TextureFormat::RGBA8, TextureColorSpace::Linear), DXGI_FORMAT_R8G8B8A8_UNORM);
    EXPECT_EQ(ToDXGIFormat(TextureFormat::RGBA8, TextureColorSpace::SRGB), DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    EXPECT_EQ(ToDXGIFormat(TextureFormat::BC1, TextureColorSpace::Linear), DXGI_FORMAT_BC1_UNORM);
    EXPECT_EQ(ToDXGIFormat(TextureFormat::BC1, TextureColorSpace::SRGB), DXGI_FORMAT_BC1_UNORM_SRGB);
    EXPECT_EQ(ToDXGIFormat(TextureFormat::BC3, TextureColorSpace::SRGB), DXGI_FORMAT_BC3_UNORM_SRGB);
    EXPECT_EQ(ToDXGIFormat(TextureFormat::BC7, TextureColorSpace::SRGB), DXGI_FORMAT_BC7_UNORM_SRGB);
    EXPECT_EQ(ToDXGIFormat(TextureFormat::BC5, TextureColorSpace::Linear), DXGI_FORMAT_BC5_UNORM);
    EXPECT_EQ(ToDXGIFormat(TextureFormat::BC5, TextureColorSpace::SRGB), DXGI_FORMAT_UNKNOWN);

    EXPECT_EQ(ToDX12ResourceFormat(TextureFormat::BC1, TextureColorSpace::SRGB), DXGI_FORMAT_BC1_UNORM_SRGB);
}

TEST(DX12FormatUtilsTests, MapsDepth32FToTypelessResourceAndSampleableSrvFormat)
{
    EXPECT_EQ(ToDXGIFormat(TextureFormat::Depth32F), DXGI_FORMAT_D32_FLOAT);
    EXPECT_EQ(ToDX12ResourceFormat(TextureFormat::Depth32F), DXGI_FORMAT_R32_TYPELESS);
    EXPECT_EQ(ToDX12OptimizedClearFormat(TextureFormat::Depth32F), DXGI_FORMAT_D32_FLOAT);

    RHITextureDesc textureDesc{};
    textureDesc.dimension = TextureDimension::Texture2D;
    textureDesc.format = TextureFormat::Depth32F;
    textureDesc.usage = static_cast<TextureUsageFlags>(
        static_cast<uint32_t>(TextureUsageFlags::Sampled) |
        static_cast<uint32_t>(TextureUsageFlags::DepthStencilAttachment));

    RHITextureViewDesc viewDesc{};
    viewDesc.viewType = TextureViewType::Texture2D;
    viewDesc.format = TextureFormat::Depth32F;

    const auto descriptors = BuildDX12TextureViewDescriptorSet(textureDesc, viewDesc);
    ASSERT_TRUE(descriptors.hasSrv);
    EXPECT_EQ(descriptors.srvDesc.Format, DXGI_FORMAT_R32_FLOAT);
    ASSERT_TRUE(descriptors.hasDsv);
    EXPECT_EQ(descriptors.dsvDesc.Format, DXGI_FORMAT_D32_FLOAT);
}

TEST(DX12FormatUtilsTests, Depth32FCapabilityUsesTypelessResourceAndSrvSupportForSampling)
{
    const auto capability = BuildDX12TextureFormatCapability(
        TextureFormat::Depth32F,
        D3D12_FORMAT_SUPPORT1_TEXTURE2D |
            D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL,
        D3D12_FORMAT_SUPPORT2_NONE,
        false);

    EXPECT_TRUE(capability.sampled);
    EXPECT_FALSE(capability.storage);
}

TEST(DX12FormatUtilsTests, BuildsTextureViewDescriptorsUsingTextureColorSpace)
{
    RHITextureDesc textureDesc{};
    textureDesc.extent.width = 16u;
    textureDesc.extent.height = 16u;
    textureDesc.extent.depth = 1u;
    textureDesc.dimension = TextureDimension::Texture2D;
    textureDesc.format = TextureFormat::BC1;
    textureDesc.colorSpace = TextureColorSpace::SRGB;
    textureDesc.mipLevels = 4u;
    textureDesc.usage = TextureUsageFlags::Sampled;

    RHITextureViewDesc viewDesc{};
    viewDesc.viewType = TextureViewType::Texture2D;
    viewDesc.format = textureDesc.format;
    viewDesc.colorSpace = textureDesc.colorSpace;
    viewDesc.subresourceRange.mipLevelCount = 0u;
    viewDesc.subresourceRange.arrayLayerCount = 0u;

    const auto descriptors = BuildDX12TextureViewDescriptorSet(textureDesc, viewDesc);
    ASSERT_TRUE(descriptors.hasSrv);
    EXPECT_EQ(descriptors.srvDesc.Format, DXGI_FORMAT_BC1_UNORM_SRGB);
    EXPECT_EQ(descriptors.srvDesc.Texture2D.MipLevels, 4u);

    textureDesc.format = TextureFormat::BC5;
    textureDesc.colorSpace = TextureColorSpace::Linear;
    viewDesc.format = textureDesc.format;
    viewDesc.colorSpace = textureDesc.colorSpace;

    const auto linearDescriptors = BuildDX12TextureViewDescriptorSet(textureDesc, viewDesc);
    ASSERT_TRUE(linearDescriptors.hasSrv);
    EXPECT_EQ(linearDescriptors.srvDesc.Format, DXGI_FORMAT_BC5_UNORM);
}

TEST(DX12FormatUtilsTests, DefaultTextureViewDescriptorInheritsTextureFormatAndColorSpace)
{
    RHITextureDesc textureDesc{};
    textureDesc.extent.width = 16u;
    textureDesc.extent.height = 16u;
    textureDesc.extent.depth = 1u;
    textureDesc.dimension = TextureDimension::Texture2D;
    textureDesc.format = TextureFormat::BC7;
    textureDesc.colorSpace = TextureColorSpace::SRGB;
    textureDesc.mipLevels = 3u;
    textureDesc.usage = TextureUsageFlags::Sampled;

    const RHITextureViewDesc defaultViewDesc{};
    const auto descriptors = BuildDX12TextureViewDescriptorSet(textureDesc, defaultViewDesc);

    ASSERT_TRUE(descriptors.hasSrv);
    EXPECT_EQ(descriptors.srvDesc.Format, DXGI_FORMAT_BC7_UNORM_SRGB);
}

TEST(DX12FormatUtilsTests, ReportsDx12FormatCapabilitiesPerFormat)
{
    const auto rgba8 = BuildDX12TextureFormatCapability(
        TextureFormat::RGBA8,
        D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE,
        D3D12_FORMAT_SUPPORT2_NONE,
        true);
    EXPECT_TRUE(rgba8.sampled);
    EXPECT_TRUE(rgba8.upload);
    EXPECT_TRUE(rgba8.supportsSrgbView);

    const auto supported = BuildDX12TextureFormatCapability(
        TextureFormat::BC7,
        D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE,
        D3D12_FORMAT_SUPPORT2_NONE,
        true);
    EXPECT_TRUE(supported.sampled);
    EXPECT_TRUE(supported.upload);
    EXPECT_TRUE(supported.supportsSrgbView);
    EXPECT_TRUE(supported.requiresAlignedTopLevelBlocks);
    EXPECT_TRUE(supported.diagnosticReason.empty());

    const auto unsupported = BuildDX12TextureFormatCapability(
        TextureFormat::BC1,
        D3D12_FORMAT_SUPPORT1_NONE,
        D3D12_FORMAT_SUPPORT2_NONE,
        false,
        "driver rejected BC1");
    EXPECT_FALSE(unsupported.sampled);
    EXPECT_FALSE(unsupported.upload);
    EXPECT_EQ(unsupported.diagnosticReason, "driver rejected BC1");
}

#endif
