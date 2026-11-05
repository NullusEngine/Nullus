#include <gtest/gtest.h>

#include "Core/CameraController.h"
#include "Core/SceneViewCameraInteractionStateMachine.h"
#include "Panels/SceneViewSelectionCapture.h"
#include "ImGui/imgui.h"
#include "ImGui/imgui_internal.h"
#include <Platform/Windowing/Context/Device.h>
#include <UI/UIManager.h>
#include <ServiceLocator.h>
#include <Shortcuts/EditorShortcutService.h>
#include <cstdlib>

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
    bool showToolbar = false;

    void DrawImage()
    {
        if (showToolbar)
        {
            ImGui::BeginChild("Scene toolbar", {300.0f, 28.0f}, ImGuiChildFlags_None,
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            ImGui::InvisibleButton("Toolbar control", {120.0f, 22.0f});
            ImGui::EndChild();
        }
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

TEST(CameraControllerInputTests, LateUiCaptureUsesTheOriginalPressAndTheNextMotionRotates)
{
#if !defined(_WIN32) && !defined(__APPLE__)
    if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY")) GTEST_SKIP() << "A GLFW display is required";
#endif
    NLS::Context::Device device(NLS::Windowing::Settings::DeviceSettings{});
    NLS::Windowing::Settings::WindowSettings settings;
    settings.visible = false; settings.width = settings.height = 64;
    settings.clientAPI = NLS::Windowing::Settings::WindowClientAPI::NoAPI;
    NLS::Windowing::Window window(device, settings);
    NLS::Windowing::Inputs::InputManager input(window);
    auto* previousGui = ImGui::GetCurrentContext();
    struct GuiScope { ImGuiContext* previous; ~GuiScope() { ImGui::SetCurrentContext(previous); } } guiScope{previousGui};
    auto* previousUi = NLS::Core::ServiceLocator::Contains<NLS::UI::UIManager>() ?
        &NLS::Core::ServiceLocator::Get<NLS::UI::UIManager>() : nullptr;
    NLS::UI::UIManager ui(window.GetGlfwWindow(), NLS::Render::Settings::EGraphicsBackend::NONE);
    NLS::Core::ServiceLocator::Provide<NLS::UI::UIManager>(ui);
    struct ServiceScope
    {
        NLS::UI::UIManager* previous;
        ~ServiceScope()
        {
            if (previous) NLS::Core::ServiceLocator::Provide<NLS::UI::UIManager>(*previous);
            else NLS::Core::ServiceLocator::Remove<NLS::UI::UIManager>();
        }
    } serviceScope{previousUi};
    using NLS::Editor::Shortcuts::EditorShortcutService;
    auto* previousShortcuts = NLS::Core::ServiceLocator::Contains<EditorShortcutService>() ?
        &NLS::Core::ServiceLocator::Get<EditorShortcutService>() : nullptr;
    EditorShortcutService shortcuts;
    shortcuts.InitializeInput(input.GetInputSystem());
    std::vector<std::string> diagnostics;
    const auto defaults = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "App/Assets/Editor/Input/EditorShortcuts.inputactions";
    ASSERT_TRUE(shortcuts.LoadDefaults(defaults, diagnostics));
    NLS::Core::ServiceLocator::Provide<EditorShortcutService>(shortcuts);
    struct ShortcutScope
    {
        EditorShortcutService* previous;
        ~ShortcutScope()
        {
            if (previous) NLS::Core::ServiceLocator::Provide<EditorShortcutService>(*previous);
            else NLS::Core::ServiceLocator::Remove<EditorShortcutService>();
        }
    } shortcutScope{previousShortcuts};
    NLS::Maths::Transform cameraTransform;
    NLS::Render::Entities::Camera camera(&cameraTransform);
    camera.SetRotation(NLS::Maths::Quaternion({15.0f, 25.0f, 0.0f}));
    ASSERT_NE(camera.GetRotation().y, 0.0f);
    CameraController controller(camera, input, window);
    controller.SetViewportInputBounds({0, 0}, {64, 64});
    auto& system = input.GetInputSystem();
    NLS::Input::InputEvent event;
    event.window = input.GetPlatformBackend().FindWindowId(reinterpret_cast<uintptr_t>(window.GetGlfwWindow()));
    event.ingress = NLS::Input::IngressKind::Glfw;
    event.type = NLS::Input::InputEventType::FocusGained; ASSERT_TRUE(system.SubmitEvent(event));
    event.window = {}; event.ingress = NLS::Input::IngressKind::Virtual; event.device = system.GetMouse();
    event.type = NLS::Input::InputEventType::PointerMove; event.x = event.y = 20; ASSERT_TRUE(system.SubmitEvent(event));
    event.type = NLS::Input::InputEventType::ButtonDown; event.control = 1; ASSERT_TRUE(system.SubmitEvent(event));
    event.type = NLS::Input::InputEventType::Wheel; event.x = 0; event.y = 1; ASSERT_TRUE(system.SubmitEvent(event));
    system.FinishEventPump();
    shortcuts.PrepareInput({});
    controller.SetInputBlocked(true); controller.HandleInputs(.016f);
    EXPECT_FALSE(controller.IsRightMousePressed());
    const auto rotation = camera.GetRotation(); const auto position = camera.GetPosition();
    controller.SetInputBlocked(false); controller.RefreshPointerCaptureAfterUi();
    ASSERT_TRUE(controller.IsRightMousePressed());
    EXPECT_TRUE(input.GetPlatformBackend().OwnsPointerCapture(&controller));
    controller.RefreshPointerCaptureAfterUi();
    EXPECT_EQ(camera.GetPosition(), position); // No second movement/wheel update.
    EXPECT_EQ(camera.GetRotation().x, rotation.x);
    EXPECT_EQ(camera.GetRotation().y, rotation.y);
    EXPECT_EQ(camera.GetRotation().z, rotation.z);
    EXPECT_EQ(camera.GetRotation().w, rotation.w);
    input.ClearEvents();
    event.type = NLS::Input::InputEventType::PointerMove; event.x = 25; event.y = 20;
    ASSERT_TRUE(system.SubmitEvent(event)); system.FinishEventPump();
    shortcuts.PrepareInput({});
    controller.HandleInputs(.016f);
    EXPECT_NE(camera.GetRotation().y, rotation.y);
    event.type = NLS::Input::InputEventType::ButtonUp; event.control = 1;
    ASSERT_TRUE(system.SubmitEvent(event)); controller.RefreshPointerCaptureAfterUi();
    EXPECT_FALSE(controller.IsRightMousePressed());

    input.ClearEvents(); shortcuts.PrepareInput({});
    const auto binding = shortcuts.GetBindingSlots("camera-navigation.fly").front();
    std::string error;
    ASSERT_TRUE(shortcuts.SetBindingSlot("camera-navigation.fly", 0,
        {binding, NLS::Input::OverrideState::Override,
            {{{NLS::Input::DeviceKind::Mouse, NLS::Input::ControlKind::Button, 3}, {1}}}}, error)) << error;
    shortcuts.PrepareInput({}); input.ClearEvents();
    event.type = NLS::Input::InputEventType::ButtonDown; event.control = 1;
    ASSERT_TRUE(system.SubmitEvent(event)); shortcuts.PrepareInput({}); controller.HandleInputs(.016f);
    EXPECT_FALSE(controller.IsCameraControlActive()); // The old RMB has no fallback path.
    event.type = NLS::Input::InputEventType::ButtonUp; ASSERT_TRUE(system.SubmitEvent(event));
    shortcuts.PrepareInput({}); input.ClearEvents();
    event.type = NLS::Input::InputEventType::ButtonDown; event.control = 3;
    ASSERT_TRUE(system.SubmitEvent(event)); shortcuts.PrepareInput({}); controller.HandleInputs(.016f);
    ASSERT_TRUE(controller.IsCameraControlActive());
    const auto beforeMove = camera.GetPosition();
    event.device = system.GetKeyboard(); event.control = 87;
    ASSERT_TRUE(system.SubmitEvent(event)); controller.SetNavigationOptions(false, false, 1, 50);
    controller.HandleInputs(.016f);
    EXPECT_NE(camera.GetPosition(), beforeMove); // Rebound fly gate + W still moves.
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

TEST_F(ViewportMouseInputTests, FloatingEditorOwnsMouseWhileSceneIsDrawingFirst)
{
    constexpr auto flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
    auto& io = ImGui::GetIO();
    DrawFrames("Particle Library Editor");
    io.MousePos = {100.0f, 100.0f};
    io.MouseDown[0] = true;

    ImGui::NewFrame();
    ImGui::SetNextWindowPos({0.0f, 0.0f});
    ImGui::SetNextWindowSize({340.0f, 260.0f});
    ImGui::Begin((m_view->name + m_view->GetPanelID()).c_str(), nullptr, flags);
    m_view->DrawImage();
    EXPECT_TRUE(m_view->IsMouseWithinView({100.0f, 100.0f}));
    EXPECT_FALSE(m_view->IsMouseHoveringView({100.0f, 100.0f}));
    ImGui::End();

    ImGui::SetNextWindowPos({60.0f, 60.0f});
    ImGui::SetNextWindowSize({200.0f, 160.0f});
    ImGui::Begin("Particle Library Editor", nullptr, flags);
    ImGui::InvisibleButton("Editor control", {150.0f, 100.0f});
    EXPECT_TRUE(ImGui::IsItemActive());
    ImGui::End();
    ImGui::EndFrame();
}

TEST_F(ViewportMouseInputTests, DraggingFloatingEditorChildDoesNotStartSceneCapture)
{
    constexpr auto flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
    NLS::Editor::Panels::SceneViewSelectionCapture capture;
    auto& io = ImGui::GetIO();
    bool sceneStarted = false, editorActive = false, editorClicked = false;
    const auto drawFrame = [&]
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0.0f, 0.0f});
        ImGui::SetNextWindowSize({340.0f, 260.0f});
        ImGui::Begin((m_view->name + m_view->GetPanelID()).c_str(), nullptr, flags);
        m_view->DrawImage();
        if (m_view->IsMouseHoveringView({io.MousePos.x, io.MousePos.y}) && capture.CanBegin())
        { capture.Begin(); sceneStarted = true; }
        ImGui::End();
        ImGui::SetNextWindowPos({60.0f, 60.0f});
        ImGui::SetNextWindowSize({200.0f, 160.0f});
        ImGui::Begin("Particle Library Editor", nullptr, flags);
        ImGui::BeginChild("Graph", {180.0f, 140.0f});
        editorClicked = ImGui::InvisibleButton("Editor control", {150.0f, 100.0f});
        editorActive = ImGui::IsItemActive();
        ImGui::EndChild();
        ImGui::End();
        ImGui::EndFrame();
    };
    for (int frame = 0; frame < 3; ++frame) drawFrame();
    io.MousePos = {100.0f, 100.0f}; io.MouseDown[0] = true;
    drawFrame();
    EXPECT_FALSE(sceneStarted); EXPECT_TRUE(editorActive);
    io.MousePos = {150.0f, 120.0f};
    drawFrame();
    EXPECT_FALSE(sceneStarted); EXPECT_TRUE(editorActive);
    io.MouseDown[0] = false;
    drawFrame();
    EXPECT_FALSE(sceneStarted); EXPECT_TRUE(editorClicked);
    capture.Reset();
}

TEST_F(ViewportMouseInputTests, SceneSelectionCaptureSurvivesDraggingOutsideAndReleasesMouse)
{
    constexpr auto flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
    using NLS::Editor::Panels::SceneViewSelectionCapture;
    using NLS::Editor::Panels::SceneViewSelectionCaptureState;
    SceneViewSelectionCapture capture;
    auto& io = ImGui::GetIO();
    DrawFrames();
    io.MousePos = {100.0f, 100.0f};
    io.MouseDown[0] = true;

    const auto beginScene = [&]
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0.0f, 0.0f});
        ImGui::SetNextWindowSize({340.0f, 260.0f});
        ImGui::Begin((m_view->name + m_view->GetPanelID()).c_str(), nullptr, flags);
        m_view->DrawImage();
    };
    const auto endScene = [] { ImGui::End(); ImGui::EndFrame(); };

    beginScene();
    EXPECT_TRUE(m_view->IsMouseHoveringView({100.0f, 100.0f}));
    ASSERT_TRUE(capture.CanBegin());
    capture.Begin();
    EXPECT_EQ(capture.Poll(), SceneViewSelectionCaptureState::Held);
    endScene();

    io.MousePos = {500.0f, 350.0f};
    for (int frame = 0; frame < 3; ++frame)
    {
        beginScene();
        EXPECT_EQ(capture.Poll(), SceneViewSelectionCaptureState::Held);
        endScene();
    }
    io.MouseDown[0] = false;
    beginScene();
    EXPECT_EQ(capture.Poll(), SceneViewSelectionCaptureState::Released);
    capture.Reset();
    EXPECT_EQ(ImGui::GetActiveID(), 0u);
    EXPECT_EQ(ImGui::GetKeyOwner(ImGuiKey_MouseLeft), ImGuiKeyOwner_NoOwner);
    endScene();
}

TEST_F(ViewportMouseInputTests, SceneSelectionCaptureDoesNotClearAnotherOwnersMouse)
{
    using NLS::Editor::Panels::SceneViewSelectionCapture;
    using NLS::Editor::Panels::SceneViewSelectionCaptureState;
    SceneViewSelectionCapture capture;
    auto& io = ImGui::GetIO();
    DrawFrames();
    io.MousePos = {100.0f, 100.0f};
    io.MouseDown[0] = true;
    ImGui::NewFrame();
    ImGui::Begin((m_view->name + m_view->GetPanelID()).c_str());
    ASSERT_TRUE(capture.CanBegin());
    capture.Begin();
    const auto otherId = ImGui::GetID("Other control");
    ImGui::SetActiveID(otherId, ImGui::GetCurrentWindow());
    ImGui::SetKeyOwner(ImGuiKey_MouseLeft, otherId, ImGuiInputFlags_LockUntilRelease);
    EXPECT_EQ(capture.Poll(), SceneViewSelectionCaptureState::Lost);
    capture.Reset();
    EXPECT_EQ(ImGui::GetActiveID(), otherId);
    EXPECT_EQ(ImGui::GetKeyOwner(ImGuiKey_MouseLeft), otherId);
    ImGui::ClearActiveID();
    ImGui::SetKeyOwner(ImGuiKey_MouseLeft, ImGuiKeyOwner_NoOwner);
    ImGui::End(); ImGui::EndFrame();
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

TEST_F(ViewportMouseInputTests, SceneToolbarIsExcludedFromTheImageBeforeTheNextUiFrame)
{
    m_view->showToolbar = true;
    DrawFrames();
    EXPECT_FALSE(m_view->IsMouseWithinView({20.0f, 20.0f}));
    EXPECT_FALSE(m_view->IsMouseHoveringView({20.0f, 20.0f}));
    EXPECT_TRUE(m_view->IsMouseHoveringView({100.0f, 100.0f}));
}

TEST_F(ViewportMouseInputTests, DraggingAToolbarControlIntoTheImageKeepsToolbarOwnership)
{
    m_view->showToolbar = true;
    auto& io = ImGui::GetIO();
    io.MousePos = {20.0f, 20.0f};
    DrawFrames();
    io.MouseDown[0] = true;
    DrawFrames(nullptr, {}, 1);
    ASSERT_TRUE(ImGui::IsAnyItemActive());
    io.MousePos = {100.0f, 100.0f};
    DrawFrames(nullptr, {}, 1);
    ASSERT_TRUE(m_view->IsMouseWithinView({100.0f, 100.0f}));
    EXPECT_FALSE(m_view->IsMouseHoveringView({100.0f, 100.0f}));
    io.MouseDown[0] = false;
    DrawFrames(nullptr, {}, 1);
    EXPECT_TRUE(m_view->IsMouseHoveringView({100.0f, 100.0f}));
}
