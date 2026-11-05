#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <Reflection/ReflectionDatabase.h>
#include <Math/Transform.h>
#include <Math/Vector4.h>
#include <Rendering/ShaderCompiler/ShaderCompiler.h>
#include <Rendering/FrameGraph/ExternalResourceBridge.h>
#include "Core/SceneViewToolbarState.h"
#include "Core/SceneVisibilityState.h"
#include "Rendering/SceneGizmoIcons.h"
#include "Engine/Rendering/SceneVisibilityPipeline.h"
#include "Engine/Rendering/SceneSpatialIndex.h"
#include "Rendering/EditorGridRenderer.h"
#include "Engine/Rendering/SceneRenderOptions.h"
#include "Engine/Rendering/SceneLightingProvider.h"
#include "Settings/EditorSettings.h"
#include "Settings/EditorSettingsPersistence.h"

namespace
{
using namespace NLS;
using namespace NLS::Editor::Core;
using Settings = NLS::Editor::Settings::EditorSettings;
constexpr float kTolerance = 0.0001f;

TEST(SceneViewToolbarTests, FrozenFramesRetainDisplayOptionsWithoutChangingOtherViews)
{
    Maths::Transform transform;
    Render::Entities::Camera camera(&transform);
    Render::Data::FrameDescriptor scene;
    scene.camera = &camera;
    scene.renderWidth = 640; scene.renderHeight = 360;
    scene.applyToneMapping = false;
    scene.outputViewport = Maths::Vector4(10, 20, 320, 180);
    scene.gameTime = 1.5; scene.playSessionId = 3; scene.simulationFrame = 9;
    const auto frozen = Render::FrameGraph::FreezeFrameDescriptor(scene);
    EXPECT_FALSE(frozen.descriptor.applyToneMapping);
    EXPECT_EQ(frozen.descriptor.outputViewport, scene.outputViewport);
    EXPECT_EQ(frozen.descriptor.gameTime, scene.gameTime);
    EXPECT_EQ(frozen.descriptor.playSessionId, scene.playSessionId);
    EXPECT_EQ(frozen.descriptor.simulationFrame, scene.simulationFrame);
    EXPECT_EQ(frozen.descriptor.outputBuffer, nullptr);
    scene.applyToneMapping = true;
    EXPECT_FALSE(frozen.descriptor.applyToneMapping);
    EXPECT_TRUE(Render::Data::FrameDescriptor{}.applyToneMapping);
}

void ExpectVectorNear(const Maths::Vector3& actual, const Maths::Vector3& expected)
{
    EXPECT_NEAR(actual.x, expected.x, kTolerance);
    EXPECT_NEAR(actual.y, expected.y, kTolerance);
    EXPECT_NEAR(actual.z, expected.z, kTolerance);
}

class SceneViewToolbarSettingsTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        scene = Settings::GetSceneViewSettingsObject();
        debug = Settings::GetDebugDrawSettingsObject();
        meta::ReflectionDatabase::Instance();
        Settings::RegisterSettingObjects(registry);
        path = std::filesystem::temp_directory_path() / "NullusSceneViewToolbarTests" / "settings.json";
        std::filesystem::create_directories(path.parent_path());
        std::filesystem::remove(path);
    }
    void TearDown() override
    {
        Settings::GetSceneViewSettingsObject() = scene;
        Settings::GetDebugDrawSettingsObject() = debug;
        std::filesystem::remove(path);
    }
    Editor::Settings::EditorSceneViewSettingsObject scene;
    Editor::Settings::EditorDebugDrawSettingsObject debug;
    Editor::Settings::EditorSettingsRegistry registry;
    std::filesystem::path path;
};
}

TEST(SceneViewToolbarTests, TwoDHasUprightWorldAxesAndRestoresTheThreeDCamera)
{
    Maths::Transform transform;
    Render::Entities::Camera camera(&transform);
    camera.SetPosition({-10.0f, 3.0f, 10.0f});
    camera.SetRotation(Maths::Quaternion({15.0f, 135.0f, 0.0f}));
    camera.SetFov(60.0f);
    camera.SetFar(5000.0f);
    camera.SetSize(7.0f);
    SceneCameraFocusState focus{{2.0f, 4.0f, 1.0f}, 15.0f, true};
    const auto saved = CaptureSceneViewCamera(camera, focus);
    EnterSceneView2D(camera, focus);
    EXPECT_EQ(camera.GetProjectionMode(), Render::Settings::EProjectionMode::ORTHOGRAPHIC);
    ExpectVectorNear(camera.GetRotation() * Maths::Vector3::Forward, -Maths::Vector3::Forward);
    camera.CacheMatrices(800u, 600u);
    const auto viewProjection = camera.GetProjectionMatrix() * camera.GetViewMatrix();
    const auto project = [&](const Maths::Vector3& point)
    { return viewProjection * Maths::Vector4(point.x, point.y, point.z, 1.0f); };
    const auto center = project(focus.focusPoint);
    const auto right = project(focus.focusPoint + Maths::Vector3::Right);
    const auto up = project(focus.focusPoint + Maths::Vector3::Up);
    EXPECT_GT(right.x / right.w, center.x / center.w);
    EXPECT_GT(up.y / up.w, center.y / center.w);
    camera.SetPosition({50.0f, 20.0f, 30.0f});
    camera.SetSize(40.0f);
    RestoreSceneViewCamera(camera, focus, saved);
    ExpectVectorNear(camera.GetPosition(), saved.position);
    ExpectVectorNear(camera.GetRotation() * Maths::Vector3::Forward, saved.rotation * Maths::Vector3::Forward);
    EXPECT_EQ(camera.GetProjectionMode(), saved.projection);
    EXPECT_FLOAT_EQ(camera.GetSize(), saved.orthographicSize);
    ExpectVectorNear(focus.focusPoint, saved.focus.focusPoint);
    EXPECT_FLOAT_EQ(focus.focusDistance, saved.focus.focusDistance);
}

TEST(SceneViewToolbarTests, OrthographicThreeDCameraSurvivesTwoDRoundTrip)
{
    Maths::Transform transform;
    Render::Entities::Camera camera(&transform);
    camera.SetPosition({5.0f, 6.0f, 7.0f});
    camera.SetRotation(Maths::Quaternion({35.0f, 70.0f, 0.0f}));
    camera.SetProjectionMode(Render::Settings::EProjectionMode::ORTHOGRAPHIC);
    camera.SetSize(12.0f);
    SceneCameraFocusState focus{};
    const auto saved = CaptureSceneViewCamera(camera, focus);
    EnterSceneView2D(camera, focus);
    RestoreSceneViewCamera(camera, focus, saved);
    EXPECT_EQ(camera.GetProjectionMode(), Render::Settings::EProjectionMode::ORTHOGRAPHIC);
    EXPECT_FLOAT_EQ(camera.GetSize(), 12.0f);
    EXPECT_FALSE(focus.hasFocus);
    ExpectVectorNear(camera.GetPosition(), saved.position);
}

TEST(SceneViewToolbarTests, OrthographicPlacementRaysAreParallelAndFollowScreenPosition)
{
    Maths::Transform transform;
    Render::Entities::Camera camera(&transform);
    camera.SetPosition({0.0f, 0.0f, 10.0f});
    camera.SetRotation(Maths::Quaternion::LookAt(-Maths::Vector3::Forward, Maths::Vector3::Up));
    camera.SetProjectionMode(Render::Settings::EProjectionMode::ORTHOGRAPHIC);
    camera.SetSize(5.0f);
    const auto center = BuildSceneViewPlacementRay(camera, {400.0f, 300.0f}, {800.0f, 600.0f});
    const auto corner = BuildSceneViewPlacementRay(camera, {800.0f, 0.0f}, {800.0f, 600.0f});
    ExpectVectorNear(center.direction, corner.direction);
    ExpectVectorNear(center.direction, -Maths::Vector3::Forward);
    EXPECT_GT(corner.origin.x, center.origin.x);
    EXPECT_GT(corner.origin.y, center.origin.y);
    const auto placement = corner.origin + corner.direction * (-corner.origin.z / corner.direction.z);
    EXPECT_NEAR(placement.z, 0.0f, kTolerance);
    EXPECT_NEAR(placement.x, 5.0f * 800.0f / 600.0f, kTolerance);
}

TEST(SceneViewToolbarTests, PerspectivePlacementRaysKeepTheCameraOrigin)
{
    Maths::Transform transform;
    Render::Entities::Camera camera(&transform);
    camera.SetPosition({2.0f, 3.0f, 4.0f});
    const auto center = BuildSceneViewPlacementRay(camera, {400.0f, 300.0f}, {800.0f, 600.0f});
    const auto corner = BuildSceneViewPlacementRay(camera, {0.0f, 0.0f}, {800.0f, 600.0f});
    ExpectVectorNear(center.origin, camera.GetPosition());
    ExpectVectorNear(corner.origin, camera.GetPosition());
    EXPECT_LT(Maths::Vector3::Dot(center.direction, corner.direction), 0.999f);
}

TEST(SceneViewToolbarTests, OverflowReservesItsButtonAndKeepsWholeGroups)
{
    constexpr std::array<float, 3> widths{100.0f, 80.0f, 90.0f};
    EXPECT_EQ(GetVisibleSceneToolbarGroupCount(widths, 300.0f, 24.0f, 4.0f), 3u);
    EXPECT_EQ(GetVisibleSceneToolbarGroupCount(widths, 205.0f, 24.0f, 4.0f), 1u);
    EXPECT_EQ(GetVisibleSceneToolbarGroupCount(widths, 212.0f, 24.0f, 4.0f), 2u);
    EXPECT_EQ(GetVisibleSceneToolbarGroupCount(widths, 60.0f, 24.0f, 4.0f), 0u);
    constexpr std::array<float, 3> scaled{200.0f, 160.0f, 180.0f};
    EXPECT_EQ(GetVisibleSceneToolbarGroupCount(scaled, 424.0f, 48.0f, 8.0f), 2u);
}

TEST(SceneViewToolbarTests, TwoDGridIsInTheXYPlaneAndOtherPreviewsKeepXZ)
{
    const auto xy = Editor::Rendering::BuildEditorGridModelMatrix({3.0f, 4.0f, 8.0f}, true, 10.0f);
    const auto xz = Editor::Rendering::BuildEditorGridModelMatrix({3.0f, 4.0f, 8.0f}, false, 10.0f);
    const auto point = Maths::Vector4(0.0f, 0.0f, 1.0f, 1.0f);
    const auto onXY = xy * point;
    const auto onXZ = xz * point;
    EXPECT_NEAR(onXY.z, 0.0f, kTolerance);
    EXPECT_NEAR(onXY.y, 14.0f, kTolerance);
    EXPECT_NEAR(onXZ.y, 0.0f, kTolerance);
    EXPECT_NEAR(onXZ.z, 18.0f, kTolerance);
}

TEST(SceneViewToolbarTests, WireframeOverridesAreLocalAndOverlayPreservesDepth)
{
    using namespace Engine::Rendering;
    Render::Resources::MaterialPipelineStateOverrides game, scene;
    SceneRenderOptions options;
    options.geometryMode = SceneGeometryDrawMode::Wireframe;
    ApplySceneGeometryOverrides(options, scene);
    EXPECT_EQ(scene.wireframe, true);
    EXPECT_FALSE(game.wireframe.has_value());
    ApplySceneGeometryOverrides({}, game);
    EXPECT_FALSE(game.wireframe.has_value());
    const auto overlay = BuildSceneWireframeOverlayOverrides();
    EXPECT_EQ(overlay.wireframe, true);
    EXPECT_EQ(overlay.depthTest, true);
    EXPECT_EQ(overlay.depthWrite, false);
    EXPECT_EQ(overlay.depthCompare, Render::Settings::EComparaisonAlgorithm::LESS_EQUAL);
    EXPECT_FALSE(overlay.colorWrite.has_value());
}

TEST_F(SceneViewToolbarSettingsTests, NewDiagnosticModesSurvivePreferencesRoundTrip)
{
    auto& value = Settings::GetSceneViewSettingsObject();
    for (int mode = 9; mode <= 12; ++mode)
    {
        value.drawMode = mode;
        ASSERT_TRUE(Editor::Settings::EditorSettingsPersistence::Save(path, registry));
        value.drawMode = 0;
        ASSERT_TRUE(Editor::Settings::EditorSettingsPersistence::Load(path, registry));
        EXPECT_EQ(value.drawMode, mode);
    }
}

TEST(SceneViewToolbarTests, PreviewLightingOwnsTwoLightsWithoutSceneObjects)
{
    Engine::Rendering::SceneLightingProvider provider;
    {
        Maths::Transform transform;
        Render::Entities::Camera camera(&transform);
        camera.SetPosition({3.0f, 4.0f, 5.0f});
        camera.SetRotation(Maths::Quaternion({20.0f, 30.0f, 0.0f}));
        provider.CollectPreview(camera);
    }
    const auto lights = provider.ReadPublishedLightingDescriptor();
    ASSERT_NE(lights, nullptr);
    ASSERT_EQ(lights->lights.size(), 2u);
    const auto& directional = lights->lights[0].get();
    EXPECT_EQ(directional.type, Render::Settings::ELightType::DIRECTIONAL);
    EXPECT_FLOAT_EQ(directional.intensity, 0.75f);
    ExpectVectorNear(directional.transform->GetWorldPosition(), {3.0f, 4.0f, 5.0f});
    EXPECT_EQ(lights->lights[1].get().type, Render::Settings::ELightType::AMBIENT_SPHERE);
    EXPECT_FLOAT_EQ(lights->lights[1].get().intensity, 0.1f);
}

TEST_F(SceneViewToolbarSettingsTests, RegistersAndPersistsIndependentScenePreferences)
{
    ASSERT_NE(registry.Find("editor.scene-view"), nullptr);
    auto& value = Settings::GetSceneViewSettingsObject();
    value.drawMode = 6;
    value.showGrid = false;
    value.snapEnabled = true;
    value.sceneLighting = false;
    value.showParticles = false;
    value.showSkybox = false;
    value.showSelectionOutline = false;
    value.cameraFov = 75.0f;
    value.effectsEnabled = false; value.postProcessing = false; value.alwaysRefresh = false;
    value.icons3D = false; value.showLightIcons = false; value.showSelectionWire = true;
    value.cameraDynamicClipping = true; value.cameraAcceleration = true; value.cameraEasing = false;
    value.cameraMinSpeed = 0.1f; value.cameraMaxSpeed = 70.0f; value.sceneVisibility = false;
    ASSERT_TRUE(Editor::Settings::EditorSettingsPersistence::Save(path, registry));
    value = {};
    ASSERT_TRUE(Editor::Settings::EditorSettingsPersistence::Load(path, registry));
    EXPECT_EQ(value.drawMode, 6);
    EXPECT_FALSE(value.showGrid);
    EXPECT_TRUE(value.snapEnabled);
    EXPECT_FALSE(value.sceneLighting);
    EXPECT_FALSE(value.showParticles);
    EXPECT_FALSE(value.showSkybox);
    EXPECT_FALSE(value.showSelectionOutline);
    EXPECT_FLOAT_EQ(value.cameraFov, 75.0f);
    EXPECT_FALSE(value.effectsEnabled); EXPECT_FALSE(value.postProcessing); EXPECT_FALSE(value.alwaysRefresh);
    EXPECT_FALSE(value.icons3D); EXPECT_FALSE(value.showLightIcons); EXPECT_TRUE(value.showSelectionWire);
    EXPECT_TRUE(value.cameraDynamicClipping); EXPECT_TRUE(value.cameraAcceleration); EXPECT_FALSE(value.cameraEasing);
    EXPECT_FLOAT_EQ(value.cameraMinSpeed, 0.1f); EXPECT_FLOAT_EQ(value.cameraMaxSpeed, 70.0f); EXPECT_FALSE(value.sceneVisibility);
}

TEST_F(SceneViewToolbarSettingsTests, MigratesLegacyGizmosOnceAndPreservesNewPreferences)
{
    {
        std::ofstream file(path);
        file << R"({"version":2,"objects":{"editor.debug-draw":{"fields":{"debugDrawEnabled":true,"debugDrawGrid":false,"debugDrawCamera":false,"debugDrawLighting":true,"debugDrawBounds":true,"lightBillboardScale":1.25}}}})";
    }
    ASSERT_TRUE(Editor::Settings::EditorSettingsPersistence::Load(path, registry));
    auto& value = Settings::GetSceneViewSettingsObject();
    EXPECT_FALSE(value.showGrid);
    EXPECT_FALSE(value.showCameras);
    EXPECT_FALSE(value.showCameraIcons);
    EXPECT_TRUE(value.showBounds);
    EXPECT_FLOAT_EQ(value.lightBillboardScale, 1.25f);
    value.showGrid = true;
    value.showCameras = true;
    ASSERT_TRUE(Editor::Settings::EditorSettingsPersistence::Save(path, registry));
    value.showGrid = false;
    ASSERT_TRUE(Editor::Settings::EditorSettingsPersistence::Load(path, registry));
    EXPECT_TRUE(value.showGrid);
    EXPECT_TRUE(value.showCameras);
    EXPECT_FALSE(Settings::GetDebugDrawSettingsObject().debugDrawGrid);
}

TEST_F(SceneViewToolbarSettingsTests, GizmosMasterDoesNotHideGridAndKeepsPreviewDefaults)
{
    const auto previous = Settings::GetDebugDrawSettingsObject();
    auto& value = Settings::GetSceneViewSettingsObject();
    value.showGizmos = false;
    value.showGrid = true;
    const auto effective = Editor::Settings::BuildSceneViewDebugDrawSettings(value);
    EXPECT_TRUE(effective.debugDrawGrid);
    EXPECT_FALSE(effective.debugDrawCamera);
    EXPECT_FALSE(effective.debugDrawLighting);
    EXPECT_EQ(Settings::GetDebugDrawSettingsObject().debugDrawGrid, previous.debugDrawGrid);
    EXPECT_EQ(Settings::GetDebugDrawSettingsObject().debugDrawCamera, previous.debugDrawCamera);
}

TEST(SceneViewWireframeShaderTests, CompilesRigidAndSkinnedDx12Variants)
{
#if !defined(_WIN32)
    GTEST_SKIP() << "DXIL compilation is validated on Windows.";
#endif
    namespace sc = NLS::Render::ShaderCompiler;
    sc::ShaderCompiler compiler;
    const auto root = std::filesystem::temp_directory_path() / "NullusSceneWireframeShaderTests";
    for (const bool skinned : {false, true})
    {
        SCOPED_TRACE(skinned ? "skinned" : "rigid");
        for (const bool pixel : {false, true})
        {
            sc::ShaderCompilationInput input;
            input.assetPath = "App/Assets/Engine/Shaders/SceneWireframe.hlsl";
            input.stage = pixel ? sc::ShaderStage::Pixel : sc::ShaderStage::Vertex;
            input.options.targetPlatform = sc::ShaderTargetPlatform::DXIL;
            input.options.entryPoint = pixel ? "PSMain" : "VSMain";
            input.options.targetProfile = pixel ? "ps_6_0" : "vs_6_0";
            input.options.artifactDirectory = root.string();
            if (skinned) input.options.macros.push_back({"NLS_SKINNED", "1"});
            const auto result = compiler.Compile(input);
            ASSERT_EQ(result.status, sc::ShaderCompilationStatus::Succeeded) << result.diagnostics;
            EXPECT_FALSE(result.bytecode.empty());
        }
    }
}

TEST(SceneViewVisibilityTests, MasterSwitchPreservesHiddenObjectsAndIsolatedSelection)
{
    SceneVisibilityState state;
    const std::array<int32_t, 4> objects{5, 2, 4, 3};
    state.SetHidden(std::array<int32_t, 2>{2, 3}, true);
    EXPECT_EQ(state.CaptureHiddenIds(objects, true), (std::vector<int32_t>{2, 3}));
    EXPECT_TRUE(state.CaptureHiddenIds(objects, false).empty());
    EXPECT_EQ(state.CaptureHiddenIds(objects, true), (std::vector<int32_t>{2, 3}));
    state.Isolate(std::array<int32_t, 1>{4});
    EXPECT_EQ(state.CaptureHiddenIds(objects, true), (std::vector<int32_t>{2, 3, 5}));
    state.ExitIsolation();
    EXPECT_EQ(state.CaptureHiddenIds(objects, true), (std::vector<int32_t>{2, 3}));
    state.ShowAll();
    EXPECT_FALSE(state.HasHiddenObjects());
}

TEST(SceneViewVisibilityTests, VisibilitySnapshotIsOwnedAndSceneChangesClearSessionOverrides)
{
    SceneVisibilityState state;
    int sceneA, sceneB;
    state.BindScene(&sceneA);
    state.SetHidden(std::array<int32_t, 1>{7}, true);
    const auto revision = state.Revision();
    const auto frozen = state.CaptureHiddenIds(std::array<int32_t, 1>{7}, true);
    state.BindScene(&sceneA);
    EXPECT_EQ(state.Revision(), revision);
    state.BindScene(&sceneB);
    EXPECT_FALSE(state.HasHiddenObjects());
    EXPECT_EQ(frozen, std::vector<int32_t>{7});
}

TEST(SceneViewVisibilityTests, RenderPipelineExcludesHiddenObjectsWithoutMutatingSharedSnapshots)
{
    using namespace Engine::Rendering;
    ScenePrimitiveSnapshot snapshot;
    snapshot.sceneId = 77; snapshot.snapshotSerial = 1;
    for (uint32_t i = 0; i < 3; ++i)
    {
        ScenePrimitiveSnapshotRecord record;
        record.handle = {77, i, 1}; record.commandOffsetBegin = i; record.commandOffsetEnd = i + 1;
        record.mesh = reinterpret_cast<Render::Resources::Mesh*>(uintptr_t{1});
        record.modelBounds.size = {1,1,1};
        record.occupied = record.ownerAlive = record.ownerActive = record.hasMeshBinding = record.hasValidMaterial = true;
        snapshot.primitiveRecords.push_back(record); snapshot.denseIndexToHandle.push_back(record.handle);
        snapshot.handleToDenseIndex.emplace_back(record.handle, i);
    }
    SceneSpatialIndex index;
    SceneVisibilityPipelineOptions sceneOptions;
    sceneOptions.hiddenPrimitiveIndices = {1};
    const auto scene = SceneVisibilityPipeline::Evaluate(sceneOptions, snapshot, index);
    EXPECT_EQ(scene.visiblePrimitiveCount, 2u);
    const auto game = SceneVisibilityPipeline::Evaluate({}, snapshot, index);
    EXPECT_EQ(game.visiblePrimitiveCount, 3u);
    EXPECT_TRUE(snapshot.primitiveRecords[1].ownerActive);
}

TEST(SceneViewGizmoTests, TwoDComponentIconsKeepTheirPixelSizeAcrossDistanceAndProjection)
{
    Maths::Transform transform; Render::Entities::Camera camera(&transform);
    camera.SetFov(60);
    using Editor::Rendering::ComputeSceneIconPresentation;
    const auto nearIcon = ComputeSceneIconPresentation(camera, {0,0,5}, 600, 2, 0.5f, false, false);
    const auto farIcon = ComputeSceneIconPresentation(camera, {0,0,50}, 600, 2, 0.5f, false, false);
    EXPECT_NEAR(farIcon.scale / nearIcon.scale, 10.0f, kTolerance);
    camera.SetProjectionMode(Render::Settings::EProjectionMode::ORTHOGRAPHIC); camera.SetSize(8);
    const auto orthoA = ComputeSceneIconPresentation(camera, {0,0,5}, 600, 2, 0.5f, false, false);
    const auto orthoB = ComputeSceneIconPresentation(camera, {0,0,50}, 600, 2, 0.5f, false, false);
    EXPECT_FLOAT_EQ(orthoA.scale, orthoB.scale);
    EXPECT_NEAR(orthoA.scale * 2 / (16.0f / 600), 32.0f, kTolerance);
}

TEST(SceneViewGizmoTests, ThreeDIconsShrinkWithDistanceAndFadeOnlyWhenRequested)
{
    Maths::Transform transform; Render::Entities::Camera camera(&transform);
    using Editor::Rendering::ComputeSceneIconPresentation;
    const auto visible = ComputeSceneIconPresentation(camera, {0,0,1000}, 600, 2, 0.5f, true, false);
    const auto faded = ComputeSceneIconPresentation(camera, {0,0,1000}, 600, 2, 0.5f, true, true);
    EXPECT_FLOAT_EQ(visible.scale, 0.05f); EXPECT_FLOAT_EQ(visible.opacity, 1.0f);
    EXPECT_FLOAT_EQ(faded.opacity, 0.0f);
}

TEST(SceneViewGizmoTests, TwoDLightBillboardCancelsShaderDistanceScaling)
{
    Maths::Transform transform;
    Render::Entities::Camera camera(&transform);
    camera.SetFov(60);
    for (const auto& position : {Maths::Vector3{0, 0, 5}, Maths::Vector3{8, 0, 50}})
    {
        const auto icon = Editor::Rendering::ComputeSceneIconPresentation(camera, position, 600, 2, 0.5f, false, false);
        const auto shaderScale = Editor::Rendering::ComputeLightBillboardShaderScale(camera, position, icon, false);
        EXPECT_NEAR(shaderScale * Maths::Vector3::Distance(position, camera.GetPosition()), icon.scale, kTolerance);
        EXPECT_FLOAT_EQ(Editor::Rendering::ComputeLightBillboardShaderScale(camera, position, icon, true), icon.scale);
    }
}

TEST_F(SceneViewToolbarSettingsTests, ComponentGizmosAndIconsHaveIndependentSettings)
{
    auto value = scene;
    value.showGizmos = true; value.showCameras = false; value.showCameraIcons = true;
    value.showLights = true; value.showLightIcons = false;
    const auto effective = Editor::Settings::BuildSceneViewDebugDrawSettings(value);
    EXPECT_FALSE(effective.debugDrawCamera); EXPECT_TRUE(effective.debugDrawCameraIcons);
    EXPECT_TRUE(effective.debugDrawLighting); EXPECT_FALSE(effective.debugDrawLightIcons);
}

TEST(SceneViewVisibilityTests, ReloadAtTheSameAddressDoesNotInheritIsolation)
{
    SceneVisibilityState state;
    int address;
    state.BindScene(&address, 1);
    state.Isolate(std::array<int32_t, 1>{7});
    state.BindScene(&address, 2);
    EXPECT_FALSE(state.IsIsolating());
    EXPECT_FALSE(state.HasHiddenObjects());
}
