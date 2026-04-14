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
#endif
