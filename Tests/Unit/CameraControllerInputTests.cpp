#include <gtest/gtest.h>

#include "Core/CameraController.h"
#include "Core/SceneViewCameraInteractionStateMachine.h"
#include "ImGui/imgui.h"

namespace
{
using NLS::Editor::Core::CameraController;
using NLS::Editor::Core::SceneViewCameraInteractionInputSnapshot;
using NLS::Editor::Core::SceneViewCameraInteractionStateId;
using NLS::Editor::Core::SceneViewCameraInteractionStateMachine;
using NLS::Maths::Vector2;

class InputProbeView final : public NLS::Editor::Panels::AView
{
public:
    InputProbeView() : AView("Scene Input Probe", true, {}) {}

    NLS::Render::Entities::Camera* GetCamera() override { return nullptr; }
    NLS::Engine::SceneSystem::Scene* GetScene() override { return nullptr; }

    void DrawImage()
    {
        m_image->size = {300.0f, 220.0f};
        m_image->Draw();
        MarkViewportImageInputBoundsForLastDraw();
    }
};

class ViewportMouseInputTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = {800.0f, 600.0f};
        io.MousePos = {20.0f, 20.0f};
        io.Fonts->AddFontDefault();
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        m_view = std::make_unique<InputProbeView>();
    }

    void TearDown() override
    {
        m_view.reset();
        ImGui::DestroyContext();
    }

    void DrawFrames(const char* editor = nullptr, const ImVec2 editorPosition = {60.0f, 60.0f}, int count = 3)
    {
        constexpr auto flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
        for (int frame = 0; frame < count; ++frame)
        {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos({0.0f, 0.0f});
            ImGui::SetNextWindowSize({340.0f, 260.0f});
            ImGui::Begin((m_view->name + m_view->GetPanelID()).c_str(), nullptr, flags);
            m_view->DrawImage();
            ImGui::End();
            if (editor != nullptr)
            {
                ImGui::SetNextWindowPos(editorPosition);
                ImGui::SetNextWindowSize({200.0f, 160.0f});
                ImGui::Begin(editor, nullptr, flags);
                ImGui::InvisibleButton("Editor control", {150.0f, 100.0f});
                ImGui::End();
            }
            ImGui::EndFrame();
        }
    }

    std::unique_ptr<InputProbeView> m_view;
};
}

TEST(CameraControllerInputTests, LookCaptureSuppressesOnlyOneFrame)
{
    EXPECT_EQ(CameraController::GetLookCaptureSuppressionFrameCount(), 1u);
}

TEST(CameraControllerInputTests, LookClampAllowsLargerMouseDeltasThanPanOrbit)
{
    const Vector2 rawDelta{48.0f, -48.0f};

    const Vector2 lookDelta =
        CameraController::ClampMouseDeltaForCameraControl(rawDelta, CameraController::GetMaxLookMouseDeltaPerFrame());
    const Vector2 panOrbitDelta =
        CameraController::ClampMouseDeltaForCameraControl(rawDelta, CameraController::GetMaxPanOrbitMouseDeltaPerFrame());

    EXPECT_FLOAT_EQ(lookDelta.x, 48.0f);
    EXPECT_FLOAT_EQ(lookDelta.y, -48.0f);
    EXPECT_FLOAT_EQ(panOrbitDelta.x, 16.0f);
    EXPECT_FLOAT_EQ(panOrbitDelta.y, -16.0f);
}

TEST(CameraControllerInputTests, FlyStateKeepsCursorOwnershipStableWithoutRepeatedEntryActions)
{
    SceneViewCameraInteractionInputSnapshot input;
    input.windowFocused = true;
    input.rightDown = true;

    const auto transition = SceneViewCameraInteractionStateMachine::EvaluateTransition(
        SceneViewCameraInteractionStateId::Fly,
        input);

    EXPECT_EQ(transition.nextState, SceneViewCameraInteractionStateId::Fly);
    EXPECT_FALSE(transition.stateChanged);
    EXPECT_FALSE(transition.shouldAcquireCursorControl);
    EXPECT_FALSE(transition.shouldSuppressInitialMouseDelta);
}

TEST(CameraControllerInputTests, InputBlockResetOnlyRunsOnBlockedEntry)
{
    EXPECT_TRUE(CameraController::ShouldResetMouseInteractionForInputBlockChange(false, true));
    EXPECT_FALSE(CameraController::ShouldResetMouseInteractionForInputBlockChange(true, true));
    EXPECT_FALSE(CameraController::ShouldResetMouseInteractionForInputBlockChange(true, false));
    EXPECT_FALSE(CameraController::ShouldResetMouseInteractionForInputBlockChange(false, false));
}

TEST(CameraControllerInputTests, WheelZoomRequiresFreshViewportInputBoundsAndMouseOverView)
{
    EXPECT_TRUE(CameraController::ShouldHandleWheelZoom(true, true));
    EXPECT_FALSE(CameraController::ShouldHandleWheelZoom(true, false));
    EXPECT_FALSE(CameraController::ShouldHandleWheelZoom(false, true));
    EXPECT_FALSE(CameraController::ShouldHandleWheelZoom(false, false));
}

TEST_F(ViewportMouseInputTests, VisibleViewportAcceptsMouseInputOnlyInsideItsImage)
{
    DrawFrames();
    ASSERT_TRUE(m_view->HasViewportImageInputBounds());
    EXPECT_TRUE(m_view->IsMouseHoveringView({100.0f, 100.0f}));
    EXPECT_FALSE(m_view->IsMouseHoveringView({380.0f, 100.0f}));
    m_view->SetOpened(false);
    EXPECT_FALSE(m_view->IsMouseHoveringView({100.0f, 100.0f}));
}

TEST_F(ViewportMouseInputTests, FloatingEditorsBlockSceneInputBeforeTheNextUiFrame)
{
    for (const auto* editor : {"VFX Editor", "UI Designer"})
    {
        SCOPED_TRACE(editor);
        DrawFrames(editor);
        // The previous UI frame hovered the exposed Scene area at (20,20).
        // The input update already has a new position over the floating editor.
        ASSERT_TRUE(m_view->IsMouseWithinView({100.0f, 100.0f}));
        EXPECT_FALSE(m_view->IsMouseHoveringView({100.0f, 100.0f}));
        EXPECT_TRUE(m_view->IsMouseHoveringView({20.0f, 20.0f}));
    }
}

TEST_F(ViewportMouseInputTests, DraggingAnEditorControlAcrossTheSceneDoesNotTransferMouseInput)
{
    auto& io = ImGui::GetIO();
    io.MousePos = {440.0f, 40.0f};
    DrawFrames("UI Designer", {400.0f, 0.0f});
    io.MouseDown[0] = true;
    DrawFrames("UI Designer", {400.0f, 0.0f}, 1);
    ASSERT_TRUE(ImGui::IsAnyItemActive());

    io.MousePos = {100.0f, 100.0f};
    DrawFrames("UI Designer", {400.0f, 0.0f}, 1);
    ASSERT_TRUE(m_view->IsMouseWithinView({100.0f, 100.0f}));
    EXPECT_FALSE(m_view->IsMouseHoveringView({100.0f, 100.0f}));

    io.MouseDown[0] = false;
    DrawFrames("UI Designer", {400.0f, 0.0f}, 1);
    EXPECT_TRUE(m_view->IsMouseHoveringView({100.0f, 100.0f}));
}
