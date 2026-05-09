#include <gtest/gtest.h>

#include "Rendering/EditorPipelineStatePresets.h"
#include "Rendering/RHI/Core/RHIPipeline.h"
#include "Rendering/RHI/Core/RHIPipelineStateUtils.h"

TEST(RHIPipelineStateUtilsTests, MapsOutlineStencilPresetToExplicitStencilState)
{
    NLS::Render::Data::PipelineState baseState;
    const auto stencilState = NLS::Editor::Rendering::CreateEditorOutlineStencilPipelineState(
        baseState,
        0xABu,
        3);

    NLS::Render::RHI::RHIGraphicsPipelineDesc desc;
    NLS::Render::RHI::ApplyPipelineStateToGraphicsPipelineDesc(stencilState, desc);

    EXPECT_TRUE(desc.depthStencilState.stencilTest);
    EXPECT_EQ(desc.depthStencilState.stencilWriteMask, 0xABu);
    EXPECT_EQ(desc.depthStencilState.stencilReadMask, 0xABu);
    EXPECT_EQ(desc.depthStencilState.stencilReference, 3u);
    EXPECT_EQ(desc.depthStencilState.stencilCompare, NLS::Render::Settings::EComparaisonAlgorithm::ALWAYS);
    EXPECT_EQ(desc.depthStencilState.stencilFailOp, NLS::Render::Settings::EOperation::REPLACE);
    EXPECT_EQ(desc.depthStencilState.stencilDepthFailOp, NLS::Render::Settings::EOperation::REPLACE);
    EXPECT_EQ(desc.depthStencilState.stencilPassOp, NLS::Render::Settings::EOperation::REPLACE);
    EXPECT_FALSE(desc.rasterState.wireframe);
    EXPECT_FALSE(desc.depthStencilState.depthWrite);
}

TEST(RHIPipelineStateUtilsTests, MapsOutlineStrokePresetToWireframeAndStencilCompareState)
{
    NLS::Render::Data::PipelineState baseState;
    const auto outlineState = NLS::Editor::Rendering::CreateEditorOutlineStrokePipelineState(
        baseState,
        5,
        0xFFu,
        3.0f);

    NLS::Render::RHI::RHIGraphicsPipelineDesc desc;
    NLS::Render::RHI::ApplyPipelineStateToGraphicsPipelineDesc(outlineState, desc);

    EXPECT_TRUE(desc.rasterState.wireframe);
    EXPECT_TRUE(desc.depthStencilState.stencilTest);
    EXPECT_EQ(desc.depthStencilState.stencilReference, 5u);
    EXPECT_EQ(desc.depthStencilState.stencilReadMask, 0xFFu);
    EXPECT_EQ(desc.depthStencilState.stencilWriteMask, 0u);
    EXPECT_EQ(desc.depthStencilState.stencilCompare, NLS::Render::Settings::EComparaisonAlgorithm::NOTEQUAL);
    EXPECT_EQ(desc.depthStencilState.stencilFailOp, NLS::Render::Settings::EOperation::KEEP);
    EXPECT_EQ(desc.depthStencilState.stencilDepthFailOp, NLS::Render::Settings::EOperation::KEEP);
    EXPECT_EQ(desc.depthStencilState.stencilPassOp, NLS::Render::Settings::EOperation::REPLACE);
    EXPECT_EQ(desc.depthStencilState.stencilWriteMask, 0u);
    EXPECT_FALSE(desc.depthStencilState.depthWrite);
}

TEST(RHIPipelineStateUtilsTests, MapsOutlineShellPresetToFrontCulledSolidStencilState)
{
    NLS::Render::Data::PipelineState baseState;
    const auto outlineState = NLS::Editor::Rendering::CreateEditorOutlineShellPipelineState(
        baseState,
        7,
        0x3Fu);

    NLS::Render::RHI::RHIGraphicsPipelineDesc desc;
    NLS::Render::RHI::ApplyPipelineStateToGraphicsPipelineDesc(outlineState, desc);

    EXPECT_FALSE(desc.rasterState.wireframe);
    EXPECT_TRUE(desc.rasterState.cullEnabled);
    EXPECT_EQ(desc.rasterState.cullFace, NLS::Render::Settings::ECullFace::FRONT);
    EXPECT_TRUE(desc.depthStencilState.stencilTest);
    EXPECT_EQ(desc.depthStencilState.stencilReference, 7u);
    EXPECT_EQ(desc.depthStencilState.stencilReadMask, 0x3Fu);
    EXPECT_EQ(desc.depthStencilState.stencilWriteMask, 0u);
    EXPECT_EQ(desc.depthStencilState.stencilCompare, NLS::Render::Settings::EComparaisonAlgorithm::NOTEQUAL);
    EXPECT_FALSE(desc.depthStencilState.depthTest);
    EXPECT_FALSE(desc.depthStencilState.depthWrite);
}

TEST(RHIPipelineStateUtilsTests, MapsGeneralPipelineStateToCentralGraphicsPipelineDescFields)
{
    NLS::Render::Data::PipelineState pipelineState;
    pipelineState.culling = true;
    pipelineState.cullFace = NLS::Render::Settings::ECullFace::FRONT;
    pipelineState.rasterizationMode = NLS::Render::Settings::ERasterizationMode::LINE;
    pipelineState.colorWriting.mask = 0x00;
    pipelineState.depthTest = false;
    pipelineState.depthWriting = false;
    pipelineState.depthFunc = NLS::Render::Settings::EComparaisonAlgorithm::GREATER;
    pipelineState.stencilTest = true;
    pipelineState.stencilWriteMask = 0x3Cu;
    pipelineState.stencilFuncMask = 0x1Fu;
    pipelineState.stencilFuncRef = 9u;
    pipelineState.stencilFuncOp = NLS::Render::Settings::EComparaisonAlgorithm::NOTEQUAL;
    pipelineState.stencilOpFail = NLS::Render::Settings::EOperation::ZERO;
    pipelineState.depthOpFail = NLS::Render::Settings::EOperation::INCREMENT;
    pipelineState.bothOpFail = NLS::Render::Settings::EOperation::DECREMENT;

    NLS::Render::RHI::RHIGraphicsPipelineDesc desc;
    NLS::Render::RHI::ApplyPipelineStateToGraphicsPipelineDesc(pipelineState, desc);

    EXPECT_TRUE(desc.rasterState.cullEnabled);
    EXPECT_EQ(desc.rasterState.cullFace, NLS::Render::Settings::ECullFace::FRONT);
    EXPECT_TRUE(desc.rasterState.wireframe);
    EXPECT_FALSE(desc.blendState.colorWrite);
    EXPECT_FALSE(desc.depthStencilState.depthTest);
    EXPECT_FALSE(desc.depthStencilState.depthWrite);
    EXPECT_EQ(desc.depthStencilState.depthCompare, NLS::Render::Settings::EComparaisonAlgorithm::GREATER);
    EXPECT_TRUE(desc.depthStencilState.stencilTest);
    EXPECT_EQ(desc.depthStencilState.stencilWriteMask, 0x3Cu);
    EXPECT_EQ(desc.depthStencilState.stencilReadMask, 0x1Fu);
    EXPECT_EQ(desc.depthStencilState.stencilReference, 9u);
    EXPECT_EQ(desc.depthStencilState.stencilCompare, NLS::Render::Settings::EComparaisonAlgorithm::NOTEQUAL);
    EXPECT_EQ(desc.depthStencilState.stencilFailOp, NLS::Render::Settings::EOperation::ZERO);
    EXPECT_EQ(desc.depthStencilState.stencilDepthFailOp, NLS::Render::Settings::EOperation::INCREMENT);
    EXPECT_EQ(desc.depthStencilState.stencilPassOp, NLS::Render::Settings::EOperation::DECREMENT);
}

TEST(RHIPipelineStateUtilsTests, MapsLegacyPipelineStateToExpandedBlendAndMsaaState)
{
    NLS::Render::Data::PipelineState pipelineState;
    pipelineState.blending = true;
    pipelineState.sampleAlphaToCoverage = true;
    pipelineState.multisample = true;
    pipelineState.colorWriting.r = true;
    pipelineState.colorWriting.g = false;
    pipelineState.colorWriting.b = true;
    pipelineState.colorWriting.a = false;

    NLS::Render::RHI::RHIGraphicsPipelineDesc desc;
    desc.renderTargetLayout.colorFormats = {
        NLS::Render::RHI::TextureFormat::RGBA8,
        NLS::Render::RHI::TextureFormat::RGBA16F
    };
    desc.renderTargetLayout.sampleCount = 4u;

    NLS::Render::RHI::ApplyPipelineStateToGraphicsPipelineDesc(pipelineState, desc);

    EXPECT_TRUE(desc.blendState.enabled);
    EXPECT_TRUE(desc.blendState.alphaToCoverageEnable);
    EXPECT_FALSE(desc.blendState.independentBlendEnable);
    EXPECT_EQ(desc.blendState.renderTargets.size(), 2u);
    EXPECT_EQ(desc.blendState.renderTargets[0].colorWriteMask, NLS::Render::RHI::RHIColorWriteMask::Red | NLS::Render::RHI::RHIColorWriteMask::Blue);
    EXPECT_EQ(desc.blendState.renderTargets[1].colorWriteMask, NLS::Render::RHI::RHIColorWriteMask::Red | NLS::Render::RHI::RHIColorWriteMask::Blue);
    EXPECT_TRUE(desc.rasterState.multisampleEnable);
    EXPECT_EQ(desc.renderTargetLayout.sampleCount, 4u);
}
