#include <gtest/gtest.h>

#include "Rendering/EditorPipelineStatePresets.h"

namespace
{
    class PresetTestRenderer final
    {
    public:
        NLS::Render::Data::PipelineState CreatePipelineState() const
        {
            return m_baseState;
        }

        NLS::Render::Data::PipelineState m_baseState{};
    };
}

TEST(EditorPipelineStatePresetsTests, OverlayPresetDisablesDepthWritingDepthTestAndCulling)
{
    NLS::Render::Data::PipelineState baseState;
    baseState.depthWriting = true;
    baseState.depthTest = true;
    baseState.culling = true;

    const auto overlayState = NLS::Editor::Rendering::CreateEditorOverlayPipelineState(baseState);

    EXPECT_FALSE(overlayState.depthWriting);
    EXPECT_FALSE(overlayState.depthTest);
    EXPECT_FALSE(overlayState.culling);
}

TEST(EditorPipelineStatePresetsTests, NoDepthPresetOnlyDisablesDepthTest)
{
    NLS::Render::Data::PipelineState baseState;
    baseState.depthWriting = true;
    baseState.depthTest = true;
    baseState.culling = true;

    const auto noDepthState = NLS::Editor::Rendering::CreateEditorNoDepthPipelineState(baseState);

    EXPECT_TRUE(noDepthState.depthWriting);
    EXPECT_FALSE(noDepthState.depthTest);
    EXPECT_TRUE(noDepthState.culling);
}

TEST(EditorPipelineStatePresetsTests, OutlineStencilPresetConfiguresStencilWriteAndBackfaceCulling)
{
    NLS::Render::Data::PipelineState baseState;
    const auto stencilState = NLS::Editor::Rendering::CreateEditorOutlineStencilPipelineState(baseState, 0xFFu, 3);

    EXPECT_TRUE(stencilState.stencilTest);
    EXPECT_EQ(stencilState.stencilWriteMask, 0xFFu);
    EXPECT_EQ(stencilState.stencilFuncRef, 3);
    EXPECT_EQ(stencilState.stencilFuncMask, 0xFFu);
    EXPECT_EQ(stencilState.stencilOpFail, NLS::Render::Settings::EOperation::REPLACE);
    EXPECT_EQ(stencilState.depthOpFail, NLS::Render::Settings::EOperation::REPLACE);
    EXPECT_EQ(stencilState.bothOpFail, NLS::Render::Settings::EOperation::REPLACE);
    EXPECT_EQ(stencilState.colorWriting.mask, 0x00);
    EXPECT_FALSE(stencilState.depthTest);
    EXPECT_TRUE(stencilState.culling);
    EXPECT_EQ(stencilState.cullFace, NLS::Render::Settings::ECullFace::BACK);
}

TEST(EditorPipelineStatePresetsTests, OutlineStrokePresetConfiguresLineRasterizationAndStencilCompare)
{
    NLS::Render::Data::PipelineState baseState;
    const auto outlineState = NLS::Editor::Rendering::CreateEditorOutlineStrokePipelineState(baseState, 5, 0xFFu, 3.0f);

    EXPECT_TRUE(outlineState.stencilTest);
    EXPECT_EQ(outlineState.stencilOpFail, NLS::Render::Settings::EOperation::KEEP);
    EXPECT_EQ(outlineState.depthOpFail, NLS::Render::Settings::EOperation::KEEP);
    EXPECT_EQ(outlineState.bothOpFail, NLS::Render::Settings::EOperation::REPLACE);
    EXPECT_EQ(outlineState.stencilFuncOp, NLS::Render::Settings::EComparaisonAlgorithm::NOTEQUAL);
    EXPECT_EQ(outlineState.stencilFuncRef, 5);
    EXPECT_EQ(outlineState.stencilFuncMask, 0xFFu);
    EXPECT_EQ(outlineState.rasterizationMode, NLS::Render::Settings::ERasterizationMode::LINE);
    EXPECT_FALSE(outlineState.depthTest);
}

TEST(EditorPipelineStatePresetsTests, UnculledPresetDisablesCullingOnly)
{
    NLS::Render::Data::PipelineState baseState;
    baseState.depthWriting = true;
    baseState.depthTest = true;
    baseState.culling = true;

    const auto unculledState = NLS::Editor::Rendering::CreateEditorUnculledPipelineState(baseState);

    EXPECT_TRUE(unculledState.depthWriting);
    EXPECT_TRUE(unculledState.depthTest);
    EXPECT_FALSE(unculledState.culling);
}

TEST(EditorPipelineStatePresetsTests, GridPresetDisablesDepthWritingAndCulling)
{
    NLS::Render::Data::PipelineState baseState;
    baseState.depthWriting = true;
    baseState.depthTest = true;
    baseState.culling = true;

    const auto gridState = NLS::Editor::Rendering::CreateEditorGridPipelineState(baseState);

    EXPECT_FALSE(gridState.depthWriting);
    EXPECT_TRUE(gridState.depthTest);
    EXPECT_FALSE(gridState.culling);
}

TEST(EditorPipelineStatePresetsTests, DebugLinePresetDisablesDepthWritingAndCullingWhileKeepingDepthTest)
{
    NLS::Render::Data::PipelineState baseState;
    baseState.depthWriting = true;
    baseState.depthTest = true;
    baseState.culling = true;

    const auto debugLineState = NLS::Editor::Rendering::CreateEditorDebugLinePipelineState(baseState);

    EXPECT_FALSE(debugLineState.depthWriting);
    EXPECT_TRUE(debugLineState.depthTest);
    EXPECT_FALSE(debugLineState.culling);
}

TEST(EditorPipelineStatePresetsTests, OverlayPresetCanBeCreatedDirectlyFromRendererBaseState)
{
    PresetTestRenderer renderer;
    renderer.m_baseState.depthWriting = true;
    renderer.m_baseState.depthTest = true;
    renderer.m_baseState.culling = true;

    const auto overlayState = NLS::Editor::Rendering::CreateEditorOverlayPipelineState(renderer);

    EXPECT_FALSE(overlayState.depthWriting);
    EXPECT_FALSE(overlayState.depthTest);
    EXPECT_FALSE(overlayState.culling);
}

TEST(EditorPipelineStatePresetsTests, OutlineStrokePresetCanBeCreatedDirectlyFromRendererBaseState)
{
    PresetTestRenderer renderer;
    renderer.m_baseState.depthWriting = true;
    renderer.m_baseState.depthTest = true;

    const auto outlineState = NLS::Editor::Rendering::CreateEditorOutlineStrokePipelineState(renderer, 7, 0xAAu, 2.5f);

    EXPECT_TRUE(outlineState.stencilTest);
    EXPECT_EQ(outlineState.stencilFuncRef, 7);
    EXPECT_EQ(outlineState.stencilFuncMask, 0xAAu);
    EXPECT_EQ(outlineState.rasterizationMode, NLS::Render::Settings::ERasterizationMode::LINE);
    EXPECT_FALSE(outlineState.depthTest);
}
