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
