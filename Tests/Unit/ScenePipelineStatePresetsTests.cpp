#include <gtest/gtest.h>

#include "Rendering/ScenePipelineStatePresets.h"
#include "Rendering/Resources/Material.h"

namespace
{
    class ScenePresetTestRenderer final
    {
    public:
        NLS::Render::Data::PipelineState CreatePipelineState() const
        {
            return m_baseState;
        }

        NLS::Render::Data::PipelineState m_baseState{};
    };
}

TEST(ScenePipelineStatePresetsTests, SkyboxPresetOnlyRelaxesDepthCompare)
{
    NLS::Render::Data::PipelineState baseState;
    baseState.depthTest = true;
    baseState.depthWriting = true;
    baseState.culling = true;
    baseState.depthFunc = NLS::Render::Settings::EComparaisonAlgorithm::LESS;

    const auto skyboxState = NLS::Engine::Rendering::CreateSceneSkyboxPipelineState(baseState);

    EXPECT_TRUE(skyboxState.depthTest);
    EXPECT_TRUE(skyboxState.depthWriting);
    EXPECT_TRUE(skyboxState.culling);
    EXPECT_EQ(skyboxState.depthFunc, NLS::Render::Settings::EComparaisonAlgorithm::LESS_EQUAL);
}

TEST(ScenePipelineStatePresetsTests, GBufferPresetCopiesMaterialDepthColorAndCullingState)
{
    NLS::Render::Data::PipelineState baseState;
    baseState.depthTest = false;
    baseState.depthWriting = false;
    baseState.colorWriting.mask = 0x00;
    baseState.culling = false;
    baseState.cullFace = NLS::Render::Settings::ECullFace::FRONT;

    NLS::Render::Resources::Material material;
    material.SetDepthTest(true);
    material.SetDepthWriting(true);
    material.SetColorWriting(true);
    material.SetBackfaceCulling(true);
    material.SetFrontfaceCulling(false);

    const auto gbufferState = NLS::Engine::Rendering::CreateSceneGBufferPipelineState(baseState, material);

    EXPECT_TRUE(gbufferState.depthTest);
    EXPECT_TRUE(gbufferState.depthWriting);
    EXPECT_EQ(gbufferState.colorWriting.mask, 0x0F);
    EXPECT_TRUE(gbufferState.culling);
    EXPECT_EQ(gbufferState.cullFace, NLS::Render::Settings::ECullFace::BACK);
}

TEST(ScenePipelineStatePresetsTests, FullscreenCompositePresetDisablesDepthAndCullingWhileKeepingColorWritesEnabled)
{
    NLS::Render::Data::PipelineState baseState;
    baseState.depthTest = true;
    baseState.depthWriting = true;
    baseState.culling = true;
    baseState.colorWriting.mask = 0x00;

    const auto compositeState = NLS::Engine::Rendering::CreateSceneFullscreenCompositePipelineState(baseState);

    EXPECT_FALSE(compositeState.depthTest);
    EXPECT_FALSE(compositeState.depthWriting);
    EXPECT_FALSE(compositeState.culling);
    EXPECT_EQ(compositeState.colorWriting.mask, 0x0F);
}

TEST(ScenePipelineStatePresetsTests, DefaultPresetCanBeCreatedDirectlyFromRendererBaseState)
{
    ScenePresetTestRenderer renderer;
    renderer.m_baseState.depthTest = true;
    renderer.m_baseState.depthWriting = false;
    renderer.m_baseState.culling = true;
    renderer.m_baseState.depthFunc = NLS::Render::Settings::EComparaisonAlgorithm::GREATER;

    const auto state = NLS::Engine::Rendering::CreateSceneDefaultPipelineState(renderer);

    EXPECT_TRUE(state.depthTest);
    EXPECT_FALSE(state.depthWriting);
    EXPECT_TRUE(state.culling);
    EXPECT_EQ(state.depthFunc, NLS::Render::Settings::EComparaisonAlgorithm::GREATER);
}

TEST(ScenePipelineStatePresetsTests, SkyboxPresetCanBeCreatedDirectlyFromRendererBaseState)
{
    ScenePresetTestRenderer renderer;
    renderer.m_baseState.depthTest = true;
    renderer.m_baseState.depthWriting = true;
    renderer.m_baseState.culling = true;
    renderer.m_baseState.depthFunc = NLS::Render::Settings::EComparaisonAlgorithm::LESS;

    const auto state = NLS::Engine::Rendering::CreateSceneSkyboxPipelineState(renderer);

    EXPECT_TRUE(state.depthTest);
    EXPECT_TRUE(state.depthWriting);
    EXPECT_TRUE(state.culling);
    EXPECT_EQ(state.depthFunc, NLS::Render::Settings::EComparaisonAlgorithm::LESS_EQUAL);
}

TEST(ScenePipelineStatePresetsTests, GBufferPresetCanBeCreatedDirectlyFromRendererBaseStateAndMaterialState)
{
    ScenePresetTestRenderer renderer;
    renderer.m_baseState.depthTest = false;
    renderer.m_baseState.depthWriting = false;
    renderer.m_baseState.colorWriting.mask = 0x00;
    renderer.m_baseState.culling = false;
    renderer.m_baseState.cullFace = NLS::Render::Settings::ECullFace::FRONT;

    NLS::Render::Resources::Material material;
    material.SetDepthTest(true);
    material.SetDepthWriting(true);
    material.SetColorWriting(true);
    material.SetBackfaceCulling(false);
    material.SetFrontfaceCulling(true);

    const auto state = NLS::Engine::Rendering::CreateSceneGBufferPipelineState(renderer, material);

    EXPECT_TRUE(state.depthTest);
    EXPECT_TRUE(state.depthWriting);
    EXPECT_EQ(state.colorWriting.mask, 0x0F);
    EXPECT_TRUE(state.culling);
    EXPECT_EQ(state.cullFace, NLS::Render::Settings::ECullFace::FRONT);
}

TEST(ScenePipelineStatePresetsTests, FullscreenCompositePresetCanBeCreatedDirectlyFromRendererBaseState)
{
    ScenePresetTestRenderer renderer;
    renderer.m_baseState.depthTest = true;
    renderer.m_baseState.depthWriting = true;
    renderer.m_baseState.culling = true;
    renderer.m_baseState.colorWriting.mask = 0x00;

    const auto state = NLS::Engine::Rendering::CreateSceneFullscreenCompositePipelineState(renderer);

    EXPECT_FALSE(state.depthTest);
    EXPECT_FALSE(state.depthWriting);
    EXPECT_FALSE(state.culling);
    EXPECT_EQ(state.colorWriting.mask, 0x0F);
}
