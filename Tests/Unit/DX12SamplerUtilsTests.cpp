#include <gtest/gtest.h>

#if defined(_WIN32)
#include "Rendering/RHI/Backends/DX12/DX12SamplerUtils.h"

TEST(DX12SamplerUtilsTests, BuildsDefaultSamplerDescCompatibly)
{
    const NLS::Render::RHI::SamplerDesc desc{};

    const auto nativeDesc = NLS::Render::RHI::DX12::BuildDX12SamplerDesc(desc);

    EXPECT_EQ(nativeDesc.Filter, D3D12_FILTER_MIN_MAG_MIP_LINEAR);
    EXPECT_EQ(nativeDesc.AddressU, D3D12_TEXTURE_ADDRESS_MODE_WRAP);
    EXPECT_EQ(nativeDesc.AddressV, D3D12_TEXTURE_ADDRESS_MODE_WRAP);
    EXPECT_EQ(nativeDesc.AddressW, D3D12_TEXTURE_ADDRESS_MODE_WRAP);
    EXPECT_EQ(nativeDesc.MinLOD, 0.0f);
    EXPECT_EQ(nativeDesc.MaxLOD, D3D12_FLOAT32_MAX);
    EXPECT_EQ(nativeDesc.MipLODBias, 0.0f);
    EXPECT_EQ(nativeDesc.MaxAnisotropy, 1u);
    EXPECT_EQ(nativeDesc.ComparisonFunc, D3D12_COMPARISON_FUNC_NEVER);
}

TEST(DX12SamplerUtilsTests, BuildsAnisotropicComparisonSamplerWithBorderColor)
{
    NLS::Render::RHI::SamplerDesc desc{};
    desc.minFilter = NLS::Render::RHI::TextureFilter::Linear;
    desc.magFilter = NLS::Render::RHI::TextureFilter::Linear;
    desc.mipFilter = NLS::Render::RHI::TextureMipFilter::Linear;
    desc.wrapU = NLS::Render::RHI::TextureWrap::ClampToBorder;
    desc.wrapV = NLS::Render::RHI::TextureWrap::MirrorRepeat;
    desc.wrapW = NLS::Render::RHI::TextureWrap::ClampToEdge;
    desc.maxAnisotropy = 8u;
    desc.minLod = 1.5f;
    desc.maxLod = 5.0f;
    desc.mipLodBias = -0.25f;
    desc.compareEnabled = true;
    desc.compareFunc = NLS::Render::Settings::EComparaisonAlgorithm::LESS_EQUAL;
    desc.borderColor = { 0.1f, 0.2f, 0.3f, 0.4f };

    const auto nativeDesc = NLS::Render::RHI::DX12::BuildDX12SamplerDesc(desc);

    EXPECT_EQ(nativeDesc.Filter, D3D12_FILTER_COMPARISON_ANISOTROPIC);
    EXPECT_EQ(nativeDesc.AddressU, D3D12_TEXTURE_ADDRESS_MODE_BORDER);
    EXPECT_EQ(nativeDesc.AddressV, D3D12_TEXTURE_ADDRESS_MODE_MIRROR);
    EXPECT_EQ(nativeDesc.AddressW, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    EXPECT_EQ(nativeDesc.MaxAnisotropy, 8u);
    EXPECT_EQ(nativeDesc.ComparisonFunc, D3D12_COMPARISON_FUNC_LESS_EQUAL);
    EXPECT_FLOAT_EQ(nativeDesc.MinLOD, 1.5f);
    EXPECT_FLOAT_EQ(nativeDesc.MaxLOD, 5.0f);
    EXPECT_FLOAT_EQ(nativeDesc.MipLODBias, -0.25f);
    EXPECT_FLOAT_EQ(nativeDesc.BorderColor[0], 0.1f);
    EXPECT_FLOAT_EQ(nativeDesc.BorderColor[1], 0.2f);
    EXPECT_FLOAT_EQ(nativeDesc.BorderColor[2], 0.3f);
    EXPECT_FLOAT_EQ(nativeDesc.BorderColor[3], 0.4f);
}

TEST(DX12SamplerUtilsTests, BuildsPointMipFilterIndependently)
{
    NLS::Render::RHI::SamplerDesc desc{};
    desc.minFilter = NLS::Render::RHI::TextureFilter::Linear;
    desc.magFilter = NLS::Render::RHI::TextureFilter::Nearest;
    desc.mipFilter = NLS::Render::RHI::TextureMipFilter::Nearest;

    const auto nativeDesc = NLS::Render::RHI::DX12::BuildDX12SamplerDesc(desc);

    EXPECT_EQ(nativeDesc.Filter, D3D12_FILTER_MIN_LINEAR_MAG_MIP_POINT);
}
#endif
