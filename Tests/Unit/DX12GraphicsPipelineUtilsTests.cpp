#include <gtest/gtest.h>

#if defined(_WIN32)
#include "Rendering/RHI/Backends/DX12/DX12GraphicsPipelineUtils.h"

TEST(DX12GraphicsPipelineUtilsTests, BuildsOwnedInputLayoutFromVertexAttributesAndBindings)
{
    NLS::Render::RHI::RHIGraphicsPipelineDesc desc;
    desc.vertexBuffers = {
        { 0u, 56u, false },
        { 1u, 16u, true }
    };
    desc.vertexAttributes = {
        { 0u, 0u, 0u, 12u },
        { 1u, 0u, 12u, 8u },
        { 2u, 1u, 0u, 16u },
        { 4u, 1u, 16u, 12u }
    };

    const auto inputLayout = NLS::Render::RHI::DX12::BuildDX12OwnedInputLayout(desc);

    ASSERT_EQ(inputLayout.elements.size(), 4u);
    ASSERT_EQ(inputLayout.semanticNames.size(), inputLayout.elements.size());

    EXPECT_STREQ(inputLayout.elements[0].SemanticName, "POSITION");
    EXPECT_EQ(inputLayout.elements[0].Format, DXGI_FORMAT_R32G32B32_FLOAT);
    EXPECT_EQ(inputLayout.elements[0].AlignedByteOffset, 0u);
    EXPECT_EQ(inputLayout.elements[0].InputSlot, 0u);
    EXPECT_EQ(inputLayout.elements[0].InputSlotClass, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA);

    EXPECT_STREQ(inputLayout.elements[1].SemanticName, "TEXCOORD");
    EXPECT_EQ(inputLayout.elements[1].SemanticIndex, 0u);
    EXPECT_EQ(inputLayout.elements[1].Format, DXGI_FORMAT_R32G32_FLOAT);
    EXPECT_EQ(inputLayout.elements[1].AlignedByteOffset, 12u);

    EXPECT_STREQ(inputLayout.elements[2].SemanticName, "NORMAL");
    EXPECT_EQ(inputLayout.elements[2].SemanticIndex, 0u);
    EXPECT_EQ(inputLayout.elements[2].Format, DXGI_FORMAT_R32G32B32A32_FLOAT);
    EXPECT_EQ(inputLayout.elements[2].InputSlot, 1u);
    EXPECT_EQ(inputLayout.elements[2].InputSlotClass, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA);
    EXPECT_EQ(inputLayout.elements[2].InstanceDataStepRate, 1u);

    EXPECT_STREQ(inputLayout.elements[3].SemanticName, "TEXCOORD");
    EXPECT_EQ(inputLayout.elements[3].SemanticIndex, 2u);
    EXPECT_EQ(inputLayout.elements[3].Format, DXGI_FORMAT_R32G32B32_FLOAT);
}

TEST(DX12GraphicsPipelineUtilsTests, ReturnsEmptyInputLayoutWhenNoVertexAttributesAreProvided)
{
    NLS::Render::RHI::RHIGraphicsPipelineDesc desc;
    desc.vertexBuffers = {
        { 0u, 32u, false }
    };

    const auto inputLayout = NLS::Render::RHI::DX12::BuildDX12OwnedInputLayout(desc);

    EXPECT_TRUE(inputLayout.semanticNames.empty());
    EXPECT_TRUE(inputLayout.elements.empty());
}

TEST(DX12GraphicsPipelineUtilsTests, BuildsExpandedBlendStateForIndependentRenderTargets)
{
    NLS::Render::RHI::RHIGraphicsPipelineDesc desc;
    desc.blendState.alphaToCoverageEnable = true;
    desc.blendState.independentBlendEnable = true;
    desc.blendState.renderTargets.resize(2u);
    desc.blendState.renderTargets[0].blendEnable = true;
    desc.blendState.renderTargets[0].srcColor = NLS::Render::RHI::RHIBlendFactor::SrcAlpha;
    desc.blendState.renderTargets[0].dstColor = NLS::Render::RHI::RHIBlendFactor::InvSrcAlpha;
    desc.blendState.renderTargets[0].colorOp = NLS::Render::RHI::RHIBlendOp::Add;
    desc.blendState.renderTargets[0].srcAlpha = NLS::Render::RHI::RHIBlendFactor::One;
    desc.blendState.renderTargets[0].dstAlpha = NLS::Render::RHI::RHIBlendFactor::Zero;
    desc.blendState.renderTargets[0].alphaOp = NLS::Render::RHI::RHIBlendOp::Max;
    desc.blendState.renderTargets[0].colorWriteMask =
        NLS::Render::RHI::RHIColorWriteMask::Red | NLS::Render::RHI::RHIColorWriteMask::Alpha;
    desc.blendState.renderTargets[1].blendEnable = false;
    desc.blendState.renderTargets[1].colorWriteMask = NLS::Render::RHI::RHIColorWriteMask::Green;

    const auto blendState = NLS::Render::RHI::DX12::BuildDX12BlendState(desc);

    EXPECT_TRUE(blendState.AlphaToCoverageEnable);
    EXPECT_TRUE(blendState.IndependentBlendEnable);
    EXPECT_TRUE(blendState.RenderTarget[0].BlendEnable);
    EXPECT_EQ(blendState.RenderTarget[0].SrcBlend, D3D12_BLEND_SRC_ALPHA);
    EXPECT_EQ(blendState.RenderTarget[0].DestBlend, D3D12_BLEND_INV_SRC_ALPHA);
    EXPECT_EQ(blendState.RenderTarget[0].BlendOpAlpha, D3D12_BLEND_OP_MAX);
    EXPECT_EQ(blendState.RenderTarget[0].RenderTargetWriteMask, D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_ALPHA);
    EXPECT_FALSE(blendState.RenderTarget[1].BlendEnable);
    EXPECT_EQ(blendState.RenderTarget[1].RenderTargetWriteMask, D3D12_COLOR_WRITE_ENABLE_GREEN);
}

TEST(DX12GraphicsPipelineUtilsTests, BuildsRasterizerStateWithMsaaFlag)
{
    NLS::Render::RHI::RHIGraphicsPipelineDesc desc;
    desc.rasterState.multisampleEnable = true;

    const auto rasterizerState = NLS::Render::RHI::DX12::BuildDX12RasterizerState(desc);

    EXPECT_TRUE(rasterizerState.MultisampleEnable);
}
#endif
