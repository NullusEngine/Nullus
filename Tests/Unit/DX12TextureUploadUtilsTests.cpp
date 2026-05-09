#include <gtest/gtest.h>

#include <array>

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

TEST(DX12TextureUploadUtilsTests, Builds3DTexturePlanWithoutTreatingDepthAsArrayLayers)
{
    NLS::Render::RHI::RHITextureDesc desc;
    desc.extent.width = 4;
    desc.extent.height = 4;
    desc.extent.depth = 4;
    desc.dimension = NLS::Render::RHI::TextureDimension::Texture3D;
    desc.format = NLS::Render::RHI::TextureFormat::R8;
    desc.mipLevels = 3;

    const auto plan = NLS::Render::RHI::DX12::BuildDX12TextureUploadPlan(desc);

    ASSERT_EQ(plan.subresources.size(), 3u);
    EXPECT_EQ(plan.totalBytes, 73u);

    EXPECT_EQ(plan.subresources[0].arrayLayer, 0u);
    EXPECT_EQ(plan.subresources[0].width, 4u);
    EXPECT_EQ(plan.subresources[0].height, 4u);
    EXPECT_EQ(plan.subresources[0].depth, 4u);
    EXPECT_EQ(plan.subresources[0].rowPitch, 4u);
    EXPECT_EQ(plan.subresources[0].slicePitch, 64u);

    EXPECT_EQ(plan.subresources[1].width, 2u);
    EXPECT_EQ(plan.subresources[1].height, 2u);
    EXPECT_EQ(plan.subresources[1].depth, 2u);
    EXPECT_EQ(plan.subresources[1].dataOffset, 64u);
    EXPECT_EQ(plan.subresources[1].rowPitch, 2u);
    EXPECT_EQ(plan.subresources[1].slicePitch, 8u);

    EXPECT_EQ(plan.subresources[2].width, 1u);
    EXPECT_EQ(plan.subresources[2].height, 1u);
    EXPECT_EQ(plan.subresources[2].depth, 1u);
    EXPECT_EQ(plan.subresources[2].dataOffset, 72u);
    EXPECT_EQ(plan.subresources[2].rowPitch, 1u);
    EXPECT_EQ(plan.subresources[2].slicePitch, 1u);
}

TEST(DX12TextureUploadUtilsTests, Builds2DArrayTexturePlanFromArrayLayers)
{
    NLS::Render::RHI::RHITextureDesc desc;
    desc.extent.width = 2;
    desc.extent.height = 2;
    desc.extent.depth = 1;
    desc.dimension = NLS::Render::RHI::TextureDimension::Texture2DArray;
    desc.format = NLS::Render::RHI::TextureFormat::RG8;
    desc.mipLevels = 1;
    desc.arrayLayers = 3;

    const auto plan = NLS::Render::RHI::DX12::BuildDX12TextureUploadPlan(desc);

    ASSERT_EQ(plan.subresources.size(), 3u);
    EXPECT_EQ(plan.totalBytes, 24u);
    for (size_t layer = 0; layer < plan.subresources.size(); ++layer)
    {
        EXPECT_EQ(plan.subresources[layer].arrayLayer, layer);
        EXPECT_EQ(plan.subresources[layer].dataOffset, layer * 8u);
        EXPECT_EQ(plan.subresources[layer].rowPitch, 4u);
        EXPECT_EQ(plan.subresources[layer].slicePitch, 8u);
    }
}

TEST(DX12TextureUploadUtilsTests, DescribesPackedRgb8UploadAsOpaqueRgba8Expansion)
{
    const auto semantics = NLS::Render::RHI::DX12::GetDX12TextureUploadFormatSemantics(
        NLS::Render::RHI::TextureFormat::RGB8);

    EXPECT_EQ(semantics.sourceBytesPerPixel, 3u);
    EXPECT_EQ(semantics.nativeBytesPerPixel, 4u);
    EXPECT_TRUE(semantics.expandsPackedRgbToRgba8);
    EXPECT_EQ(semantics.expandedAlpha, 255u);
    EXPECT_NE(semantics.description.find("RGB8"), std::string::npos);
    EXPECT_NE(semantics.description.find("RGBA8"), std::string::npos);
}

TEST(DX12TextureUploadUtilsTests, BuildsRgb8PlanWithSeparateSourceAndNativeRowPitch)
{
    NLS::Render::RHI::RHITextureDesc desc;
    desc.extent.width = 2;
    desc.extent.height = 1;
    desc.extent.depth = 1;
    desc.dimension = NLS::Render::RHI::TextureDimension::Texture2D;
    desc.format = NLS::Render::RHI::TextureFormat::RGB8;
    desc.mipLevels = 1;

    const auto plan = NLS::Render::RHI::DX12::BuildDX12TextureUploadPlan(desc);

    ASSERT_EQ(plan.subresources.size(), 1u);
    EXPECT_EQ(plan.totalBytes, 6u);
    EXPECT_EQ(plan.nativeTotalBytes, 8u);
    EXPECT_EQ(plan.subresources[0].rowPitch, 6u);
    EXPECT_EQ(plan.subresources[0].slicePitch, 6u);
    EXPECT_EQ(plan.subresources[0].nativeRowPitch, 8u);
    EXPECT_EQ(plan.subresources[0].nativeSlicePitch, 8u);
}

TEST(DX12TextureUploadUtilsTests, ExpandsPackedRgb8RowsToOpaqueRgba8Rows)
{
    const std::array<uint8_t, 6> source{
        10u, 20u, 30u,
        40u, 50u, 60u
    };
    std::array<uint8_t, 8> destination{};

    const auto result = NLS::Render::RHI::DX12::CopyDX12TextureUploadRow(
        NLS::Render::RHI::TextureFormat::RGB8,
        source.data(),
        source.size(),
        destination.data(),
        destination.size(),
        2u);

    ASSERT_TRUE(result.succeeded);
    EXPECT_EQ(result.sourceBytesConsumed, 6u);
    EXPECT_EQ(result.destinationBytesWritten, 8u);
    EXPECT_EQ(destination, (std::array<uint8_t, 8>{
        10u, 20u, 30u, 255u,
        40u, 50u, 60u, 255u
    }));
}

TEST(DX12TextureUploadUtilsTests, CopiesNativeMatchingUploadRowsDirectly)
{
    const std::array<uint8_t, 8> source{
        10u, 20u, 30u, 40u,
        50u, 60u, 70u, 80u
    };
    std::array<uint8_t, 8> destination{};

    const auto result = NLS::Render::RHI::DX12::CopyDX12TextureUploadRow(
        NLS::Render::RHI::TextureFormat::RGBA8,
        source.data(),
        source.size(),
        destination.data(),
        destination.size(),
        2u);

    ASSERT_TRUE(result.succeeded);
    EXPECT_EQ(result.sourceBytesConsumed, source.size());
    EXPECT_EQ(result.destinationBytesWritten, destination.size());
    EXPECT_EQ(destination, source);
}

TEST(DX12TextureUploadUtilsTests, BuildsInitialBufferUploadRequest)
{
    std::array<uint32_t, 4> data{ 1u, 2u, 3u, 4u };

    NLS::Render::RHI::RHIBufferDesc desc;
    desc.size = sizeof(data);
    desc.debugName = "InitialVertexBuffer";

    const auto request = NLS::Render::RHI::DX12::BuildDX12InitialBufferUploadRequest(desc, data.data());

    EXPECT_EQ(request.resourceKind, NLS::Render::RHI::DX12::DX12InitialUploadResourceKind::Buffer);
    EXPECT_EQ(request.data, data.data());
    EXPECT_EQ(request.dataSize, sizeof(data));
    EXPECT_EQ(request.destinationOffset, 0u);
    EXPECT_TRUE(request.texturePlan.subresources.empty());
    EXPECT_EQ(request.debugName, "InitialVertexBuffer");
}

TEST(DX12TextureUploadUtilsTests, BuildsInitialTextureUploadRequestFromTexturePlan)
{
    std::array<uint8_t, 84> data{};

    NLS::Render::RHI::RHITextureDesc desc;
    desc.extent.width = 4;
    desc.extent.height = 4;
    desc.extent.depth = 1;
    desc.dimension = NLS::Render::RHI::TextureDimension::Texture2D;
    desc.format = NLS::Render::RHI::TextureFormat::RGBA8;
    desc.mipLevels = 3;
    desc.debugName = "InitialTexture";

    const auto request = NLS::Render::RHI::DX12::BuildDX12InitialTextureUploadRequest(desc, data.data());

    EXPECT_EQ(request.resourceKind, NLS::Render::RHI::DX12::DX12InitialUploadResourceKind::Texture);
    EXPECT_EQ(request.data, data.data());
    EXPECT_EQ(request.dataSize, data.size());
    EXPECT_EQ(request.destinationOffset, 0u);
    EXPECT_EQ(request.texturePlan.subresources.size(), 3u);
    EXPECT_EQ(request.texturePlan.totalBytes, 84u);
    EXPECT_EQ(request.debugName, "InitialTexture");
}

TEST(DX12TextureUploadUtilsTests, BuildsInitialBufferUploadRequestFromExplicitUploadDesc)
{
    std::array<uint32_t, 4> data{ 1u, 2u, 3u, 4u };

    NLS::Render::RHI::RHIBufferDesc desc;
    desc.size = sizeof(data);
    desc.debugName = "InitialVertexBuffer";

    NLS::Render::RHI::RHIBufferUploadDesc uploadDesc;
    uploadDesc.data = data.data();
    uploadDesc.dataSize = sizeof(uint32_t) * 2u;
    uploadDesc.destinationOffset = sizeof(uint32_t);
    uploadDesc.debugName = "InitialVertexBufferSubsetUpload";

    const auto request = NLS::Render::RHI::DX12::BuildDX12InitialBufferUploadRequest(desc, uploadDesc);

    EXPECT_EQ(request.resourceKind, NLS::Render::RHI::DX12::DX12InitialUploadResourceKind::Buffer);
    EXPECT_EQ(request.data, uploadDesc.data);
    EXPECT_EQ(request.dataSize, uploadDesc.dataSize);
    EXPECT_EQ(request.destinationOffset, uploadDesc.destinationOffset);
    EXPECT_TRUE(request.texturePlan.subresources.empty());
    EXPECT_EQ(request.debugName, uploadDesc.debugName);
}

TEST(DX12TextureUploadUtilsTests, BuildsInitialTextureUploadRequestFromExplicitUploadDesc)
{
    std::array<uint8_t, 84> data{};

    NLS::Render::RHI::RHITextureDesc desc;
    desc.extent.width = 4;
    desc.extent.height = 4;
    desc.extent.depth = 1;
    desc.dimension = NLS::Render::RHI::TextureDimension::Texture2D;
    desc.format = NLS::Render::RHI::TextureFormat::RGBA8;
    desc.mipLevels = 3;
    desc.debugName = "InitialTexture";

    NLS::Render::RHI::RHITextureUploadDesc uploadDesc;
    uploadDesc.data = data.data();
    uploadDesc.dataSize = data.size();
    uploadDesc.debugName = "InitialTextureUpload";

    const auto request = NLS::Render::RHI::DX12::BuildDX12InitialTextureUploadRequest(desc, uploadDesc);

    EXPECT_EQ(request.resourceKind, NLS::Render::RHI::DX12::DX12InitialUploadResourceKind::Texture);
    EXPECT_EQ(request.data, uploadDesc.data);
    EXPECT_EQ(request.dataSize, uploadDesc.dataSize);
    EXPECT_EQ(request.destinationOffset, 0u);
    EXPECT_EQ(request.texturePlan.subresources.size(), 3u);
    EXPECT_EQ(request.texturePlan.totalBytes, 84u);
    EXPECT_EQ(request.debugName, uploadDesc.debugName);
}
