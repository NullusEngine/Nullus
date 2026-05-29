#include <gtest/gtest.h>

#include <limits>

#include "Rendering/RHI/Backends/DX12/DX12ExplicitDeviceFactory.h"
#include "Rendering/RHI/Backends/DX12/DX12FormatUtils.h"
#include "Rendering/RHI/Backends/DX12/DX12Resource.h"
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

TEST(DX12TextureViewUtilsTests, ResolvesSingleBarrierSubresourceFromMipAndArrayLayer)
{
    NLS::Render::RHI::RHITextureDesc textureDesc;
    textureDesc.dimension = NLS::Render::RHI::TextureDimension::Texture2DArray;
    textureDesc.mipLevels = 4u;
    textureDesc.arrayLayers = 3u;

    NLS::Render::RHI::RHISubresourceRange singleRange;
    singleRange.baseMipLevel = 2u;
    singleRange.mipLevelCount = 1u;
    singleRange.baseArrayLayer = 1u;
    singleRange.arrayLayerCount = 1u;

    const auto singleIndices =
        NLS::Render::RHI::DX12::BuildDX12BarrierSubresourceIndices(textureDesc, singleRange);
    ASSERT_EQ(singleIndices.size(), 1u);
    EXPECT_EQ(singleIndices[0], 6u);

    UINT resolvedIndex = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    EXPECT_TRUE(NLS::Render::RHI::DX12::TryResolveDX12BarrierSubresourceIndex(
        textureDesc,
        singleRange,
        resolvedIndex));
    EXPECT_EQ(resolvedIndex, 6u);
}

TEST(DX12TextureViewUtilsTests, ExpandsMultiMipAndLayerBarrierSubresourceRange)
{
    NLS::Render::RHI::RHITextureDesc textureDesc;
    textureDesc.dimension = NLS::Render::RHI::TextureDimension::Texture2DArray;
    textureDesc.mipLevels = 4u;
    textureDesc.arrayLayers = 3u;

    NLS::Render::RHI::RHISubresourceRange range;
    range.baseMipLevel = 1u;
    range.mipLevelCount = 2u;
    range.baseArrayLayer = 1u;
    range.arrayLayerCount = 2u;

    const auto indices =
        NLS::Render::RHI::DX12::BuildDX12BarrierSubresourceIndices(textureDesc, range);
    ASSERT_EQ(indices.size(), 4u);
    EXPECT_EQ(indices[0], 5u);
    EXPECT_EQ(indices[1], 6u);
    EXPECT_EQ(indices[2], 9u);
    EXPECT_EQ(indices[3], 10u);
    UINT resolvedIndex = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    EXPECT_FALSE(NLS::Render::RHI::DX12::TryResolveDX12BarrierSubresourceIndex(
        textureDesc,
        range,
        resolvedIndex));
}

TEST(DX12TextureViewUtilsTests, ExpandsDepthStencilBarrierAcrossDepthAndStencilPlanes)
{
    NLS::Render::RHI::RHITextureDesc textureDesc;
    textureDesc.dimension = NLS::Render::RHI::TextureDimension::Texture2DArray;
    textureDesc.format = NLS::Render::RHI::TextureFormat::Depth24Stencil8;
    textureDesc.mipLevels = 4u;
    textureDesc.arrayLayers = 3u;

    NLS::Render::RHI::RHISubresourceRange singleRange;
    singleRange.baseMipLevel = 2u;
    singleRange.mipLevelCount = 1u;
    singleRange.baseArrayLayer = 1u;
    singleRange.arrayLayerCount = 1u;

    const auto indices =
        NLS::Render::RHI::DX12::BuildDX12BarrierSubresourceIndices(textureDesc, singleRange);
    ASSERT_EQ(indices.size(), 2u);
    EXPECT_EQ(indices[0], 6u);
    EXPECT_EQ(indices[1], 18u);
    UINT resolvedIndex = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    EXPECT_FALSE(NLS::Render::RHI::DX12::TryResolveDX12BarrierSubresourceIndex(
        textureDesc,
        singleRange,
        resolvedIndex));
}

TEST(DX12TextureViewUtilsTests, DetectsWholeTextureBarrierRange)
{
    NLS::Render::RHI::RHITextureDesc textureDesc;
    textureDesc.dimension = NLS::Render::RHI::TextureDimension::Texture2DArray;
    textureDesc.mipLevels = 4u;
    textureDesc.arrayLayers = 3u;

    NLS::Render::RHI::RHISubresourceRange allRange;
    allRange.mipLevelCount = 0u;
    allRange.arrayLayerCount = 0u;
    EXPECT_TRUE(NLS::Render::RHI::DX12::DoesDX12BarrierRangeCoverWholeTexture(textureDesc, allRange));

    NLS::Render::RHI::RHISubresourceRange fullRange;
    fullRange.baseMipLevel = 0u;
    fullRange.mipLevelCount = 4u;
    fullRange.baseArrayLayer = 0u;
    fullRange.arrayLayerCount = 3u;
    EXPECT_TRUE(NLS::Render::RHI::DX12::DoesDX12BarrierRangeCoverWholeTexture(textureDesc, fullRange));

    NLS::Render::RHI::RHISubresourceRange halfDefaultFullRange;
    halfDefaultFullRange.baseMipLevel = 0u;
    halfDefaultFullRange.mipLevelCount = 0u;
    halfDefaultFullRange.baseArrayLayer = 0u;
    halfDefaultFullRange.arrayLayerCount = 0u;
    EXPECT_TRUE(NLS::Render::RHI::DX12::DoesDX12BarrierRangeCoverWholeTexture(textureDesc, halfDefaultFullRange));

    halfDefaultFullRange.mipLevelCount = 4u;
    halfDefaultFullRange.arrayLayerCount = 0u;
    EXPECT_TRUE(NLS::Render::RHI::DX12::DoesDX12BarrierRangeCoverWholeTexture(textureDesc, halfDefaultFullRange));

    auto partialRange = fullRange;
    partialRange.arrayLayerCount = 2u;
    EXPECT_FALSE(NLS::Render::RHI::DX12::DoesDX12BarrierRangeCoverWholeTexture(textureDesc, partialRange));
}

TEST(DX12TextureViewUtilsTests, ExpandsHalfSpecifiedBarrierRangesAcrossRemainingMipsOrLayers)
{
    NLS::Render::RHI::RHITextureDesc textureDesc;
    textureDesc.dimension = NLS::Render::RHI::TextureDimension::Texture2DArray;
    textureDesc.mipLevels = 4u;
    textureDesc.arrayLayers = 3u;

    NLS::Render::RHI::RHISubresourceRange mipOnlyRange;
    mipOnlyRange.baseMipLevel = 1u;
    mipOnlyRange.mipLevelCount = 1u;
    mipOnlyRange.baseArrayLayer = 0u;
    mipOnlyRange.arrayLayerCount = 0u;
    const auto mipOnlyIndices =
        NLS::Render::RHI::DX12::BuildDX12BarrierSubresourceIndices(textureDesc, mipOnlyRange);
    ASSERT_EQ(mipOnlyIndices.size(), 3u);
    EXPECT_EQ(mipOnlyIndices[0], 1u);
    EXPECT_EQ(mipOnlyIndices[1], 5u);
    EXPECT_EQ(mipOnlyIndices[2], 9u);

    NLS::Render::RHI::RHISubresourceRange layerOnlyRange;
    layerOnlyRange.baseMipLevel = 2u;
    layerOnlyRange.mipLevelCount = 0u;
    layerOnlyRange.baseArrayLayer = 1u;
    layerOnlyRange.arrayLayerCount = 1u;
    const auto layerOnlyIndices =
        NLS::Render::RHI::DX12::BuildDX12BarrierSubresourceIndices(textureDesc, layerOnlyRange);
    ASSERT_EQ(layerOnlyIndices.size(), 2u);
    EXPECT_EQ(layerOnlyIndices[0], 6u);
    EXPECT_EQ(layerOnlyIndices[1], 7u);
}

TEST(DX12TextureViewUtilsTests, ClampsBarrierRangeEndsWithoutUint32Overflow)
{
    NLS::Render::RHI::RHITextureDesc textureDesc;
    textureDesc.dimension = NLS::Render::RHI::TextureDimension::Texture2D;
    textureDesc.mipLevels = (std::numeric_limits<uint32_t>::max)();
    textureDesc.arrayLayers = 1u;

    NLS::Render::RHI::RHISubresourceRange range;
    range.baseMipLevel = (std::numeric_limits<uint32_t>::max)() - 1u;
    range.mipLevelCount = (std::numeric_limits<uint32_t>::max)();
    range.baseArrayLayer = 0u;
    range.arrayLayerCount = 1u;

    const auto indices =
        NLS::Render::RHI::DX12::BuildDX12BarrierSubresourceIndices(textureDesc, range);
    ASSERT_EQ(indices.size(), 1u);
    EXPECT_EQ(indices[0], (std::numeric_limits<uint32_t>::max)() - 1u);
}

TEST(DX12TextureViewUtilsTests, TreatsTexture3DBarrierRangeAsMipSubresourceNotWSlice)
{
    NLS::Render::RHI::RHITextureDesc textureDesc;
    textureDesc.dimension = NLS::Render::RHI::TextureDimension::Texture3D;
    textureDesc.extent = { 16u, 16u, 8u };
    textureDesc.mipLevels = 4u;
    textureDesc.arrayLayers = 1u;

    NLS::Render::RHI::RHISubresourceRange mipRange;
    mipRange.baseMipLevel = 2u;
    mipRange.mipLevelCount = 1u;
    mipRange.baseArrayLayer = 3u;
    mipRange.arrayLayerCount = 2u;

    const auto indices =
        NLS::Render::RHI::DX12::BuildDX12BarrierSubresourceIndices(textureDesc, mipRange);
    ASSERT_EQ(indices.size(), 1u);
    EXPECT_EQ(indices[0], 2u);

    UINT resolvedIndex = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    EXPECT_TRUE(NLS::Render::RHI::DX12::TryResolveDX12BarrierSubresourceIndex(
        textureDesc,
        mipRange,
        resolvedIndex));
    EXPECT_EQ(resolvedIndex, 2u);

    NLS::Render::RHI::RHISubresourceRange allMipsForPartialWSlices;
    allMipsForPartialWSlices.baseMipLevel = 0u;
    allMipsForPartialWSlices.mipLevelCount = 4u;
    allMipsForPartialWSlices.baseArrayLayer = 3u;
    allMipsForPartialWSlices.arrayLayerCount = 2u;
    EXPECT_TRUE(NLS::Render::RHI::DX12::DoesDX12BarrierRangeCoverWholeTexture(
        textureDesc,
        allMipsForPartialWSlices));
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

TEST(DX12TextureViewUtilsTests, BuildsDepthStencilSrvAndDsvForSampledDepthAttachment)
{
    NLS::Render::RHI::RHITextureDesc textureDesc;
    textureDesc.dimension = NLS::Render::RHI::TextureDimension::Texture2D;
    textureDesc.format = NLS::Render::RHI::TextureFormat::Depth24Stencil8;
    textureDesc.usage = static_cast<NLS::Render::RHI::TextureUsageFlags>(
        static_cast<uint32_t>(NLS::Render::RHI::TextureUsageFlags::Sampled) |
        static_cast<uint32_t>(NLS::Render::RHI::TextureUsageFlags::DepthStencilAttachment));

    NLS::Render::RHI::RHITextureViewDesc viewDesc;
    viewDesc.viewType = NLS::Render::RHI::TextureViewType::Texture2D;
    viewDesc.format = NLS::Render::RHI::TextureFormat::Depth24Stencil8;

    const auto descriptors = NLS::Render::RHI::DX12::BuildDX12TextureViewDescriptorSet(textureDesc, viewDesc);

    ASSERT_TRUE(descriptors.hasSrv);
    EXPECT_EQ(descriptors.srvDesc.Format, DXGI_FORMAT_R24_UNORM_X8_TYPELESS);
    EXPECT_EQ(descriptors.srvDesc.ViewDimension, D3D12_SRV_DIMENSION_TEXTURE2D);
    ASSERT_TRUE(descriptors.hasDsv);
    EXPECT_EQ(descriptors.dsvDesc.Format, DXGI_FORMAT_D24_UNORM_S8_UINT);
    EXPECT_EQ(descriptors.dsvDesc.ViewDimension, D3D12_DSV_DIMENSION_TEXTURE2D);
    EXPECT_FALSE(descriptors.hasRtv);
}

TEST(DX12TextureViewUtilsTests, BuildsReadOnlyDepthStencilDsvFlags)
{
    NLS::Render::RHI::RHITextureDesc textureDesc;
    textureDesc.dimension = NLS::Render::RHI::TextureDimension::Texture2D;
    textureDesc.format = NLS::Render::RHI::TextureFormat::Depth24Stencil8;
    textureDesc.usage = NLS::Render::RHI::TextureUsageFlags::DepthStencilAttachment;

    NLS::Render::RHI::RHITextureViewDesc viewDesc;
    viewDesc.viewType = NLS::Render::RHI::TextureViewType::Texture2D;
    viewDesc.format = NLS::Render::RHI::TextureFormat::Depth24Stencil8;

    const auto descriptors = NLS::Render::RHI::DX12::BuildDX12TextureViewDescriptorSet(
        textureDesc,
        viewDesc,
        true);

    ASSERT_TRUE(descriptors.hasDsv);
    EXPECT_EQ(
        descriptors.dsvDesc.Flags,
        D3D12_DSV_FLAG_READ_ONLY_DEPTH | D3D12_DSV_FLAG_READ_ONLY_STENCIL);

    textureDesc.format = NLS::Render::RHI::TextureFormat::Depth32F;
    viewDesc.format = NLS::Render::RHI::TextureFormat::Depth32F;
    const auto depthOnlyDescriptors = NLS::Render::RHI::DX12::BuildDX12TextureViewDescriptorSet(
        textureDesc,
        viewDesc,
        true);

    ASSERT_TRUE(depthOnlyDescriptors.hasDsv);
    EXPECT_EQ(depthOnlyDescriptors.dsvDesc.Flags, D3D12_DSV_FLAG_READ_ONLY_DEPTH);
}

TEST(DX12TextureViewUtilsTests, UsesTypelessDepthResourceWithDsvClearFormat)
{
    EXPECT_EQ(
        NLS::Render::RHI::DX12::ToDX12ResourceFormat(NLS::Render::RHI::TextureFormat::Depth24Stencil8),
        DXGI_FORMAT_R24G8_TYPELESS);
    EXPECT_EQ(
        NLS::Render::RHI::DX12::ToDX12OptimizedClearFormat(NLS::Render::RHI::TextureFormat::Depth24Stencil8),
        DXGI_FORMAT_D24_UNORM_S8_UINT);
}

TEST(DX12TextureViewUtilsTests, RejectsZeroSizedTextures)
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
    EXPECT_EQ(texture, nullptr);
}

TEST(DX12TextureViewUtilsTests, UploadHeapBuffersKeepGenericReadStateAndRejectWritableStorage)
{
    const auto device = NLS::Render::Backend::CreateDX12RhiDevice(false);
    if (device == nullptr || !device->IsBackendReady())
        GTEST_SKIP() << "DX12 device is not available in this environment";

    const uint32_t vertexData[] = { 1u, 2u, 3u, 4u };
    NLS::Render::RHI::RHIBufferUploadDesc uploadDesc;
    uploadDesc.data = vertexData;
    uploadDesc.dataSize = sizeof(vertexData);

    NLS::Render::RHI::RHIBufferDesc vertexDesc;
    vertexDesc.size = sizeof(vertexData);
    vertexDesc.usage = NLS::Render::RHI::BufferUsageFlags::Vertex;
    vertexDesc.memoryUsage = NLS::Render::RHI::MemoryUsage::CPUToGPU;
    vertexDesc.debugName = "UploadHeapVertexBufferStateTest";
    const auto vertexBuffer = device->CreateBuffer(vertexDesc, uploadDesc);
    ASSERT_NE(vertexBuffer, nullptr);
    EXPECT_EQ(vertexBuffer->GetState(), NLS::Render::RHI::ResourceState::GenericRead);

    NLS::Render::RHI::RHIBufferDesc indexDesc;
    indexDesc.size = sizeof(vertexData);
    indexDesc.usage = NLS::Render::RHI::BufferUsageFlags::Index;
    indexDesc.memoryUsage = NLS::Render::RHI::MemoryUsage::CPUToGPU;
    indexDesc.debugName = "UploadHeapIndexBufferStateTest";
    const auto indexBuffer = device->CreateBuffer(indexDesc, uploadDesc);
    ASSERT_NE(indexBuffer, nullptr);
    EXPECT_EQ(indexBuffer->GetState(), NLS::Render::RHI::ResourceState::GenericRead);

    NLS::Render::RHI::RHIBufferDesc uniformDesc;
    uniformDesc.size = sizeof(vertexData);
    uniformDesc.usage = NLS::Render::RHI::BufferUsageFlags::Uniform;
    uniformDesc.memoryUsage = NLS::Render::RHI::MemoryUsage::CPUToGPU;
    uniformDesc.debugName = "UploadHeapUniformBufferStateTest";
    const auto uniformBuffer = device->CreateBuffer(uniformDesc, uploadDesc);
    ASSERT_NE(uniformBuffer, nullptr);
    EXPECT_EQ(uniformBuffer->GetState(), NLS::Render::RHI::ResourceState::GenericRead);
    EXPECT_EQ(uniformBuffer->GetDesc().memoryUsage, NLS::Render::RHI::MemoryUsage::CPUToGPU);

    NLS::Render::RHI::RHIBufferDesc defaultUniformDesc;
    defaultUniformDesc.size = sizeof(vertexData);
    defaultUniformDesc.usage = NLS::Render::RHI::BufferUsageFlags::Uniform;
    defaultUniformDesc.debugName = "DefaultUniformBufferEffectiveUploadHeapStateTest";
    const auto defaultUniformBuffer = device->CreateBuffer(defaultUniformDesc, uploadDesc);
    ASSERT_NE(defaultUniformBuffer, nullptr);
    EXPECT_EQ(defaultUniformBuffer->GetState(), NLS::Render::RHI::ResourceState::GenericRead);
    EXPECT_EQ(defaultUniformBuffer->GetDesc().memoryUsage, NLS::Render::RHI::MemoryUsage::CPUToGPU);

    NLS::Render::RHI::RHIBufferDesc shaderReadDesc;
    shaderReadDesc.size = sizeof(vertexData);
    shaderReadDesc.usage = NLS::Render::RHI::BufferUsageFlags::ShaderRead;
    shaderReadDesc.memoryUsage = NLS::Render::RHI::MemoryUsage::CPUToGPU;
    shaderReadDesc.debugName = "UploadHeapShaderReadBufferStateTest";
    const auto shaderReadBuffer = device->CreateBuffer(shaderReadDesc, uploadDesc);
    ASSERT_NE(shaderReadBuffer, nullptr);
    EXPECT_EQ(shaderReadBuffer->GetState(), NLS::Render::RHI::ResourceState::GenericRead);
    EXPECT_EQ(shaderReadBuffer->GetDesc().memoryUsage, NLS::Render::RHI::MemoryUsage::CPUToGPU);

    NLS::Render::RHI::RHIBufferDesc storageDesc;
    storageDesc.size = sizeof(vertexData);
    storageDesc.usage = NLS::Render::RHI::BufferUsageFlags::Storage;
    storageDesc.memoryUsage = NLS::Render::RHI::MemoryUsage::CPUToGPU;
    storageDesc.debugName = "UploadHeapStorageBufferStateTest";
    const auto storageBuffer = device->CreateBuffer(storageDesc, uploadDesc);
    EXPECT_EQ(storageBuffer, nullptr);

    NLS::Render::RHI::RHIBufferDesc copySrcDesc;
    copySrcDesc.size = sizeof(vertexData);
    copySrcDesc.usage = NLS::Render::RHI::BufferUsageFlags::CopySrc;
    copySrcDesc.memoryUsage = NLS::Render::RHI::MemoryUsage::CPUToGPU;
    copySrcDesc.debugName = "UploadHeapCopySourceBufferStateTest";
    const auto copySrcBuffer = device->CreateBuffer(copySrcDesc, uploadDesc);
    ASSERT_NE(copySrcBuffer, nullptr);
    EXPECT_EQ(copySrcBuffer->GetState(), NLS::Render::RHI::ResourceState::GenericRead);

    NLS::Render::RHI::RHIBufferDesc copyDstUploadDesc;
    copyDstUploadDesc.size = sizeof(vertexData);
    copyDstUploadDesc.usage = NLS::Render::RHI::BufferUsageFlags::CopyDst;
    copyDstUploadDesc.memoryUsage = NLS::Render::RHI::MemoryUsage::CPUToGPU;
    copyDstUploadDesc.debugName = "UploadHeapCopyDestinationBufferStateTest";
    const auto copyDstUploadBuffer = device->CreateBuffer(copyDstUploadDesc, uploadDesc);
    EXPECT_EQ(copyDstUploadBuffer, nullptr);

    NLS::Render::RHI::RHIBufferDesc readbackDesc;
    readbackDesc.size = sizeof(vertexData);
    readbackDesc.usage = NLS::Render::RHI::BufferUsageFlags::CopyDst;
    readbackDesc.memoryUsage = NLS::Render::RHI::MemoryUsage::GPUToCPU;
    readbackDesc.debugName = "ReadbackHeapCopyDestinationBufferStateTest";
    const auto readbackBuffer = device->CreateBuffer(readbackDesc);
    ASSERT_NE(readbackBuffer, nullptr);
    EXPECT_EQ(readbackBuffer->GetState(), NLS::Render::RHI::ResourceState::CopyDst);
    EXPECT_EQ(readbackBuffer->GetDesc().memoryUsage, NLS::Render::RHI::MemoryUsage::GPUToCPU);

    NLS::Render::RHI::RHIBufferDesc readbackStorageDesc;
    readbackStorageDesc.size = sizeof(vertexData);
    readbackStorageDesc.usage = NLS::Render::RHI::BufferUsageFlags::Storage;
    readbackStorageDesc.memoryUsage = NLS::Render::RHI::MemoryUsage::GPUToCPU;
    readbackStorageDesc.debugName = "ReadbackHeapStorageBufferStateTest";
    const auto readbackStorageBuffer = device->CreateBuffer(readbackStorageDesc);
    EXPECT_EQ(readbackStorageBuffer, nullptr);
}

TEST(DX12TextureViewUtilsTests, TexturesRejectCpuVisibleMemoryUsage)
{
    const auto device = NLS::Render::Backend::CreateDX12RhiDevice(false);
    if (device == nullptr || !device->IsBackendReady())
        GTEST_SKIP() << "DX12 device is not available in this environment";

    const uint32_t pixels[] = { 0xffffffffu, 0xff000000u, 0xffff0000u, 0xff00ff00u };
    NLS::Render::RHI::RHITextureUploadDesc uploadDesc;
    uploadDesc.data = pixels;
    uploadDesc.dataSize = sizeof(pixels);
    uploadDesc.extent = { 2u, 2u, 1u };
    uploadDesc.rowPitch = 8u;
    uploadDesc.slicePitch = sizeof(pixels);

    NLS::Render::RHI::RHITextureDesc desc;
    desc.extent = { 2u, 2u, 1u };
    desc.usage = NLS::Render::RHI::TextureUsageFlags::Sampled;
    desc.memoryUsage = NLS::Render::RHI::MemoryUsage::CPUToGPU;
    desc.debugName = "CpuToGpuTextureShouldBeRejected";
    EXPECT_EQ(device->CreateTexture(desc, uploadDesc), nullptr);

    desc.memoryUsage = NLS::Render::RHI::MemoryUsage::GPUToCPU;
    desc.debugName = "GpuToCpuTextureShouldBeRejected";
    EXPECT_EQ(device->CreateTexture(desc), nullptr);

    desc.memoryUsage = NLS::Render::RHI::MemoryUsage::GPUOnly;
    desc.debugName = "GpuOnlyTextureShouldBeAccepted";
    EXPECT_NE(device->CreateTexture(desc, uploadDesc), nullptr);
}

#endif
