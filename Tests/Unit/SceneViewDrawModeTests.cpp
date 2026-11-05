#include <gtest/gtest.h>
#include "Core/SceneViewToolbarState.h"

namespace { using namespace NLS; using namespace NLS::Editor::Core; }

TEST(SceneViewDrawModeTests, CatalogKeepsOldPreferencesAndNewModesDistinct)
{
    using M = SceneViewDrawMode;
    const M oldModes[]{M::Shaded,M::Wireframe,M::ShadedWireframe,M::Albedo,M::Specular,M::Smoothness,M::Normal,M::RenderPaths,M::AlphaChannel};
    for (int i = 0; i < 9; ++i) EXPECT_EQ(DecodeSceneViewDrawMode(i), oldModes[i]);
    EXPECT_EQ(DecodeSceneViewDrawMode(9), M::Overdraw);
    EXPECT_EQ(DecodeSceneViewDrawMode(10), M::Mipmaps);
    EXPECT_EQ(DecodeSceneViewDrawMode(11), M::ValidateAlbedo);
    EXPECT_EQ(DecodeSceneViewDrawMode(12), M::ValidateMetalSpecular);
    EXPECT_EQ(DecodeSceneViewDrawMode(-1), M::Shaded);
    EXPECT_EQ(DecodeSceneViewDrawMode(13), M::Shaded);
    EXPECT_EQ(DecodeSceneViewDrawMode(999), M::Shaded);
    EXPECT_EQ(GetSceneViewDrawModes().size(), 15u);
    EXPECT_EQ(GetSceneViewDrawModeInfo(M::MeshDistanceField).persistedValue, -1);
    EXPECT_EQ(GetSceneViewDrawModeInfo(M::GlobalDistanceField).persistedValue, -1);
    for (const auto& entry : GetSceneViewDrawModes())
        if (entry.persistedValue >= 0) EXPECT_EQ(DecodeSceneViewDrawMode(entry.persistedValue), entry.mode);
}

TEST(SceneViewDrawModeTests, DiagnosticModesDoNotBecomeShadedWireframeOrChangeOtherViews)
{
    using namespace Engine::Rendering;
    SceneRenderOptions game, scene;
    scene.hiddenObjectIds = {4, 9};
    scene.previewLighting = true;
    ApplySceneViewDrawMode(SceneViewDrawMode::Overdraw, scene);
    EXPECT_EQ(scene.debugMode, SceneDisplayDebugMode::Overdraw);
    EXPECT_EQ(scene.geometryMode, SceneGeometryDrawMode::Shaded);
    EXPECT_FALSE(scene.postProcessing);
    EXPECT_EQ(scene.hiddenObjectIds, (std::vector<int32_t>{4,9}));
    EXPECT_TRUE(scene.previewLighting);
    EXPECT_EQ(game.debugMode, SceneDisplayDebugMode::None);
    EXPECT_TRUE(game.postProcessing);
    ApplySceneViewDrawMode(SceneViewDrawMode::Normal, scene);
    EXPECT_EQ(scene.debugMode, SceneDisplayDebugMode::Normal);
    EXPECT_EQ(scene.geometryMode, SceneGeometryDrawMode::Shaded);
    ApplySceneViewDrawMode(SceneViewDrawMode::Wireframe, scene);
    EXPECT_EQ(scene.debugMode, SceneDisplayDebugMode::None);
    EXPECT_EQ(scene.geometryMode, SceneGeometryDrawMode::Wireframe);
}


TEST(SceneViewDrawModeTests, EveryCatalogModeMapsToItsOwnRenderDiagnostic)
{
    using namespace Engine::Rendering;
    using M = SceneViewDrawMode;
    const std::pair<M,SceneDisplayDebugMode> modes[]{
        {M::Shaded,SceneDisplayDebugMode::None}, {M::Wireframe,SceneDisplayDebugMode::None},
        {M::ShadedWireframe,SceneDisplayDebugMode::None}, {M::Albedo,SceneDisplayDebugMode::Albedo},
        {M::Specular,SceneDisplayDebugMode::Specular}, {M::Smoothness,SceneDisplayDebugMode::Smoothness},
        {M::Normal,SceneDisplayDebugMode::Normal}, {M::RenderPaths,SceneDisplayDebugMode::RenderPaths},
        {M::AlphaChannel,SceneDisplayDebugMode::AlphaChannel}, {M::Overdraw,SceneDisplayDebugMode::Overdraw},
        {M::Mipmaps,SceneDisplayDebugMode::Mipmaps}, {M::ValidateAlbedo,SceneDisplayDebugMode::ValidateAlbedo},
        {M::ValidateMetalSpecular,SceneDisplayDebugMode::ValidateMetalSpecular}};
    for (const auto& [mode,debug] : modes)
    {
        SceneRenderOptions options;
        ApplySceneViewDrawMode(mode,options);
        EXPECT_EQ(options.debugMode,debug) << GetSceneViewDrawModeInfo(mode).label;
        EXPECT_EQ(options.geometryMode, mode == M::Wireframe ? SceneGeometryDrawMode::Wireframe :
            mode == M::ShadedWireframe ? SceneGeometryDrawMode::ShadedWireframe : SceneGeometryDrawMode::Shaded);
        EXPECT_EQ(options.postProcessing,debug == SceneDisplayDebugMode::None);
    }
}
