#include <gtest/gtest.h>

#include "Rendering/RHI/Backends/DX12/DX12ExplicitDeviceFactory.h"
#include "Rendering/RHI/Backends/DX12/DX12TextureViewUtils.h"
#include "Rendering/RHI/Core/RHIDevice.h"

#if defined(_WIN32)
TEST(DX12TextureViewUtilsTests, BuildsCubeSrvAndSkipsRenderTargetViewsForSampledCubemap)
{
    NLS::Render::RHI::RHITextureDesc textureDesc;
    textureDesc.dimension = NLS::Render::RHI::TextureDimension::TextureCube;
    textureDesc.format = NLS::Render::RHI::TextureFormat::RGBA8;
    textureDesc.usage = NLS::Render::RHI::TextureUsageFlags::Sampled;
    textureDesc.mipLevels = 1;
    textureDesc.arrayLayers = 6;

    NLS::Render::RHI::RHITextureViewDesc viewDesc;
    viewDesc.viewType = NLS::Render::RHI::TextureViewType::Cube;
    viewDesc.format = NLS::Render::RHI::TextureFormat::RGBA8;
    viewDesc.subresourceRange.mipLevelCount = 1;

    const auto descriptors = NLS::Render::RHI::DX12::BuildDX12TextureViewDescriptorSet(textureDesc, viewDesc);

    ASSERT_TRUE(descriptors.hasSrv);
    EXPECT_EQ(descriptors.srvDesc.ViewDimension, D3D12_SRV_DIMENSION_TEXTURECUBE);
    EXPECT_EQ(descriptors.srvDesc.TextureCube.MipLevels, 1u);
    EXPECT_FALSE(descriptors.hasRtv);
    EXPECT_FALSE(descriptors.hasDsv);
}

TEST(DX12TextureViewUtilsTests, Builds2DViewsForColorAttachmentTextures)
{
    NLS::Render::RHI::RHITextureDesc textureDesc;
    textureDesc.dimension = NLS::Render::RHI::TextureDimension::Texture2D;
    textureDesc.format = NLS::Render::RHI::TextureFormat::RGBA8;
    textureDesc.usage = static_cast<NLS::Render::RHI::TextureUsageFlags>(
        static_cast<uint32_t>(NLS::Render::RHI::TextureUsageFlags::Sampled) |
        static_cast<uint32_t>(NLS::Render::RHI::TextureUsageFlags::ColorAttachment));

    NLS::Render::RHI::RHITextureViewDesc viewDesc;
    viewDesc.viewType = NLS::Render::RHI::TextureViewType::Texture2D;
    viewDesc.format = NLS::Render::RHI::TextureFormat::RGBA8;

    const auto descriptors = NLS::Render::RHI::DX12::BuildDX12TextureViewDescriptorSet(textureDesc, viewDesc);

    ASSERT_TRUE(descriptors.hasSrv);
    EXPECT_EQ(descriptors.srvDesc.ViewDimension, D3D12_SRV_DIMENSION_TEXTURE2D);
    ASSERT_TRUE(descriptors.hasRtv);
    EXPECT_EQ(descriptors.rtvDesc.ViewDimension, D3D12_RTV_DIMENSION_TEXTURE2D);
    EXPECT_FALSE(descriptors.hasDsv);
}

TEST(DX12TextureViewUtilsTests, BuildsArraySrvAndRtvFor2DArrayColorTexture)
{
    NLS::Render::RHI::RHITextureDesc textureDesc;
    textureDesc.dimension = NLS::Render::RHI::TextureDimension::Texture2DArray;
    textureDesc.format = NLS::Render::RHI::TextureFormat::RG16F;
    textureDesc.usage = static_cast<NLS::Render::RHI::TextureUsageFlags>(
        static_cast<uint32_t>(NLS::Render::RHI::TextureUsageFlags::Sampled) |
        static_cast<uint32_t>(NLS::Render::RHI::TextureUsageFlags::ColorAttachment));
    textureDesc.arrayLayers = 4;
    textureDesc.mipLevels = 2;

    NLS::Render::RHI::RHITextureViewDesc viewDesc;
    viewDesc.viewType = NLS::Render::RHI::TextureViewType::Texture2DArray;
    viewDesc.format = NLS::Render::RHI::TextureFormat::RG16F;
    viewDesc.subresourceRange.baseMipLevel = 1;
    viewDesc.subresourceRange.mipLevelCount = 1;
    viewDesc.subresourceRange.baseArrayLayer = 1;
    viewDesc.subresourceRange.arrayLayerCount = 2;

    const auto descriptors = NLS::Render::RHI::DX12::BuildDX12TextureViewDescriptorSet(textureDesc, viewDesc);

    ASSERT_TRUE(descriptors.hasSrv);
    EXPECT_EQ(descriptors.srvDesc.Format, DXGI_FORMAT_R16G16_FLOAT);
    EXPECT_EQ(descriptors.srvDesc.ViewDimension, D3D12_SRV_DIMENSION_TEXTURE2DARRAY);
    EXPECT_EQ(descriptors.srvDesc.Texture2DArray.MostDetailedMip, 1u);
    EXPECT_EQ(descriptors.srvDesc.Texture2DArray.MipLevels, 1u);
    EXPECT_EQ(descriptors.srvDesc.Texture2DArray.FirstArraySlice, 1u);
    EXPECT_EQ(descriptors.srvDesc.Texture2DArray.ArraySize, 2u);

    ASSERT_TRUE(descriptors.hasRtv);
    EXPECT_EQ(descriptors.rtvDesc.Format, DXGI_FORMAT_R16G16_FLOAT);
    EXPECT_EQ(descriptors.rtvDesc.ViewDimension, D3D12_RTV_DIMENSION_TEXTURE2DARRAY);
    EXPECT_EQ(descriptors.rtvDesc.Texture2DArray.MipSlice, 1u);
    EXPECT_EQ(descriptors.rtvDesc.Texture2DArray.FirstArraySlice, 1u);
    EXPECT_EQ(descriptors.rtvDesc.Texture2DArray.ArraySize, 2u);
}

TEST(DX12TextureViewUtilsTests, Builds3DSrvForTexture3D)
{
    NLS::Render::RHI::RHITextureDesc textureDesc;
    textureDesc.dimension = NLS::Render::RHI::TextureDimension::Texture3D;
    textureDesc.format = NLS::Render::RHI::TextureFormat::R32F;
    textureDesc.usage = NLS::Render::RHI::TextureUsageFlags::Sampled;
    textureDesc.extent.depth = 8;

    NLS::Render::RHI::RHITextureViewDesc viewDesc;
    viewDesc.viewType = NLS::Render::RHI::TextureViewType::Auto;
    viewDesc.format = NLS::Render::RHI::TextureFormat::R32F;

    const auto descriptors = NLS::Render::RHI::DX12::BuildDX12TextureViewDescriptorSet(textureDesc, viewDesc);

    ASSERT_TRUE(descriptors.hasSrv);
    EXPECT_EQ(descriptors.srvDesc.Format, DXGI_FORMAT_R32_FLOAT);
    EXPECT_EQ(descriptors.srvDesc.ViewDimension, D3D12_SRV_DIMENSION_TEXTURE3D);
    EXPECT_EQ(descriptors.srvDesc.Texture3D.MipLevels, 1u);
    EXPECT_FALSE(descriptors.hasRtv);
    EXPECT_FALSE(descriptors.hasDsv);
}

TEST(DX12TextureViewUtilsTests, ReturnsInvalidSrvHandleForZeroSizedTextureView)
{
    const auto device = NLS::Render::Backend::CreateDX12RhiDevice(false);
    if (device == nullptr || !device->IsBackendReady())
        GTEST_SKIP() << "DX12 device is not available in this environment";

    NLS::Render::RHI::RHITextureDesc textureDesc;
    textureDesc.extent.width = 0;
    textureDesc.extent.height = 0;
    textureDesc.extent.depth = 1;
    textureDesc.dimension = NLS::Render::RHI::TextureDimension::Texture2D;
    textureDesc.format = NLS::Render::RHI::TextureFormat::RGBA8;
    textureDesc.usage = static_cast<NLS::Render::RHI::TextureUsageFlags>(
        static_cast<uint32_t>(NLS::Render::RHI::TextureUsageFlags::Sampled) |
        static_cast<uint32_t>(NLS::Render::RHI::TextureUsageFlags::ColorAttachment));
    textureDesc.debugName = "ZeroSizedDX12Texture";

    const auto texture = device->CreateTexture(textureDesc);
    ASSERT_NE(texture, nullptr);

    NLS::Render::RHI::RHITextureViewDesc viewDesc;
    viewDesc.viewType = NLS::Render::RHI::TextureViewType::Texture2D;
    viewDesc.format = NLS::Render::RHI::TextureFormat::RGBA8;
    viewDesc.debugName = "ZeroSizedDX12TextureView";

    const auto view = device->CreateTextureView(texture, viewDesc);
    ASSERT_NE(view, nullptr);

    const auto srvHandle = view->GetNativeShaderResourceView();
    EXPECT_FALSE(srvHandle.IsValid());
    EXPECT_EQ(srvHandle, nullptr);
}
#endif
