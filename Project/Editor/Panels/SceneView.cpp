#include <UI/Plugins/DDTarget.h>
#include "ImGui/imgui.h"

#include "Rendering/DebugSceneRenderer.h"
#include "Rendering/PickingRenderPass.h"
#include "Rendering/SceneRendererFactory.h"
#include "Core/EditorActions.h"
#include "Panels/SceneView.h"
#include "Panels/GameView.h"
#include "Panels/SceneViewPickingPolicy.h"
#include "Panels/ViewFrameLifecycle.h"
#include "Settings/EditorSettings.h"
#include "Core/EditorInteractionBlocker.h"
#include "Core/SceneCameraFocus.h"
#include "ImGuizmo.h"
#include "Debug/Logger.h"
#include "Rendering/Settings/GraphicsBackendUtils.h"
#include <ServiceLocator.h>
#include <UI/UIManager.h>
#include <array>
#include <chrono>
#include <cmath>
#include <sstream>
using namespace NLS;

namespace
{
constexpr float kSceneViewGizmoCameraLength = 8.0f;
constexpr float kSceneViewDefaultFocusDistance = 15.0f;

ImGuizmo::OPERATION ToNativeImGuizmoOperation(Editor::Core::EGizmoOperation operation)
{
    switch (Editor::Core::ToImGuizmoOperation(operation))
    {
    case Editor::Core::SceneViewGizmoOperation::Rotate:
        return ImGuizmo::ROTATE;
    case Editor::Core::SceneViewGizmoOperation::Scale:
        return ImGuizmo::SCALE;
    case Editor::Core::SceneViewGizmoOperation::Translate:
    default:
        return ImGuizmo::TRANSLATE;
    }
}

ImGuizmo::MODE ToNativeImGuizmoMode(const Editor::Core::SceneViewGizmoSpace space)
{
    return space == Editor::Core::SceneViewGizmoSpace::Local
        ? ImGuizmo::LOCAL
        : ImGuizmo::WORLD;
}

Engine::GameObject* PickActorAtRenderCoordinate(
    Editor::Rendering::PickingRenderPass& pickingPass,
    const float x,
    const float y)
{
    auto pickingResult = pickingPass.PickAtRenderCoordinate(
        static_cast<uint32_t>(x),
        static_cast<uint32_t>(y));

    if (pickingResult.has_value())
    {
        if (const auto pickedActor = std::get_if<Engine::GameObject*>(&pickingResult.value()))
            return *pickedActor;
    }

    return nullptr;
}

Engine::GameObject* PickActorNearRenderCoordinate(
    Editor::Rendering::PickingRenderPass& pickingPass,
    const Maths::Vector2& mousePos,
    const float maxRenderX,
    const float maxRenderY)
{
    constexpr float kClickPickRadius = 2.0f;
    const std::array<Maths::Vector2, 9> offsets {{
        {0.0f, 0.0f},
        {-kClickPickRadius, 0.0f},
        {kClickPickRadius, 0.0f},
        {0.0f, -kClickPickRadius},
        {0.0f, kClickPickRadius},
        {-kClickPickRadius, -kClickPickRadius},
        {kClickPickRadius, -kClickPickRadius},
        {-kClickPickRadius, kClickPickRadius},
        {kClickPickRadius, kClickPickRadius},
    }};

    for (const auto& offset : offsets)
    {
        const float sampleX = std::clamp(mousePos.x + offset.x, 0.0f, maxRenderX);
        const float sampleY = std::clamp(mousePos.y + offset.y, 0.0f, maxRenderY);
        if (auto* pickedActor = PickActorAtRenderCoordinate(pickingPass, sampleX, sampleY))
            return pickedActor;
    }

    return nullptr;
}

bool ShouldLogScenePickingDiagnostics()
{
    const auto& diagnostics = NLS::Render::Settings::GetThreadDiagnosticsSettings();
    return diagnostics.dx12LogFrameFlow || diagnostics.editorLogScenePicking;
}

bool ShouldLogSceneCameraInputDiagnostics()
{
    const auto& diagnostics = NLS::Render::Settings::GetThreadDiagnosticsSettings();
    if (diagnostics.dx12LogFrameFlow || diagnostics.editorLogSceneCameraInput)
        return true;

    if (NLS::Core::ServiceLocator::Contains<NLS::Editor::Core::EditorActions>())
    {
        const auto& editorDiagnostics = EDITOR_EXEC(GetContext()).GetDiagnosticsSettings();
        return editorDiagnostics.dx12LogFrameFlow || editorDiagnostics.editorLogSceneCameraInput;
    }

    return false;
}

void LogScenePickingDiagnostics(const std::string& message)
{
    if (ShouldLogScenePickingDiagnostics())
        NLS_LOG_INFO("[SceneViewPicking] " + message);
}

void LogSceneCameraInputDiagnostics(const std::string& message)
{
    if (ShouldLogSceneCameraInputDiagnostics())
        NLS_LOG_INFO("[SceneViewCamera] " + message);
}
}
Editor::Panels::SceneView::SceneView(
    const std::string& p_title,
    bool p_opened,
    const UI::PanelWindowSettings& p_windowSettings)
    : AViewControllable(p_title, p_opened, p_windowSettings), m_sceneManager(EDITOR_CONTEXT(sceneManager))
{
    // Scene View should always render editor overlays (grid/gizmo/light billboards),
    // so use the editor renderer on every backend.
    m_renderer = std::make_unique<Editor::Rendering::DebugSceneRenderer>(*EDITOR_CONTEXT(driver));
    SetRequiresRetiredFrameConsumption(true);
    SetRequiresImmediateRetiredFrameReadback(ShouldSceneViewRequestImmediatePickingReadback());

    m_camera.SetFar(5000.0f);

    m_image->AddPlugin<UI::DDTarget<std::pair<std::string, UI::Widgets::Group*>>>("File").DataReceivedEvent += [this](auto p_data)
    {
        std::string path = p_data.first;

        switch (Utils::PathParser::GetFileType(path))
        {
            case Utils::PathParser::EFileType::SCENE:
                EDITOR_EXEC(LoadSceneFromDisk(path));
                break;
            case Utils::PathParser::EFileType::MODEL:
                EDITOR_EXEC(CreateActor(path, true));
                break;
        }
    };

    m_destroyedListener = Engine::GameObject::DestroyedEvent += [this](const Engine::GameObject& actor)
    {
        if (m_highlightedActor == &actor)
        {
            m_highlightedActor = nullptr;
        }
    };
}

Editor::Panels::SceneView::~SceneView()
{
    Engine::GameObject::DestroyedEvent -= m_destroyedListener;
}

void Editor::Panels::SceneView::Update(float p_deltaTime)
{
    using namespace Windowing::Inputs;
    const auto previousCameraPosition = m_camera.GetPosition();
    const auto previousCameraRotation = m_camera.GetRotation();
    const Maths::Vector2 mousePosition = EDITOR_CONTEXT(inputManager)->GetMousePosition();
    const bool shortcutsWindowOpen = Editor::Core::DoesShortcutSettingsWindowBlockSceneInput();
    const bool mouseOverSceneView = IsMouseWithinView(mousePosition) ||
        (m_image != nullptr && m_image->WasHoveredLastDraw());
    const bool sceneViewActive = !shortcutsWindowOpen && (IsFocused() || IsHovered() || mouseOverSceneView);
    const bool editingUiControlOutsideSceneView =
        NLS_SERVICE(UI::UIManager).IsAnyItemActive() && !mouseOverSceneView;
    m_cameraController.SetInputBlocked(shortcutsWindowOpen);
    if (shortcutsWindowOpen)
    {
        m_cameraController.ResetMouseInteractionState();
        m_gizmoInteraction = {};
        m_pendingClickPickRenderPos.reset();
        m_highlightedActor = nullptr;
        m_hasPickingSample = false;
    }
    EnsureCameraFocus();
    m_cameraController.SetFocusState(&m_cameraFocus);
    m_cameraController.SetInputActive(sceneViewActive && !editingUiControlOutsideSceneView);
    if (HasViewportImageBounds())
    {
        const auto imageMin = GetViewportImageMin();
        const auto imageMax = GetViewportImageMax();
        m_cameraController.SetViewportHeight(std::max(1.0f, imageMax.y - imageMin.y));
    }

    AViewControllable::Update(p_deltaTime);
    m_cameraMovedForPresentation = HasSceneViewCameraMotionForPresentation(
        previousCameraPosition,
        previousCameraRotation,
        m_camera.GetPosition(),
        m_camera.GetRotation());
    if (ShouldLogSceneCameraInputDiagnostics())
    {
        std::ostringstream stream;
        stream << "dt=" << p_deltaTime
            << " focused=" << IsFocused()
            << " hovered=" << IsHovered()
            << " mouseOver=" << mouseOverSceneView
            << " hasBounds=" << HasViewportImageBounds()
            << " active=" << sceneViewActive
            << " editingOutside=" << editingUiControlOutsideSceneView
            << " shortcutsBlocked=" << shortcutsWindowOpen
            << " cameraControl=" << m_cameraController.IsCameraControlActive()
            << " moved=" << m_cameraMovedForPresentation
            << " mouse=(" << mousePosition.x << "," << mousePosition.y << ")"
            << " pos=(" << m_camera.GetPosition().x << "," << m_camera.GetPosition().y << "," << m_camera.GetPosition().z << ")"
            << " rot=(" << m_camera.GetRotation().x << "," << m_camera.GetRotation().y << "," << m_camera.GetRotation().z << "," << m_camera.GetRotation().w << ")";
        LogSceneCameraInputDiagnostics(stream.str());
    }
}

Editor::Core::EGizmoOperation Editor::Panels::SceneView::GetCurrentGizmoOperation() const
{
    return m_currentOperation;
}

void Editor::Panels::SceneView::SetCurrentGizmoOperation(const Editor::Core::EGizmoOperation p_operation)
{
    m_currentOperation = p_operation;
}

Editor::Core::SceneViewGizmoPivot Editor::Panels::SceneView::GetCurrentGizmoPivot() const
{
    return m_currentPivot;
}

void Editor::Panels::SceneView::SetCurrentGizmoPivot(const Editor::Core::SceneViewGizmoPivot p_pivot)
{
    m_currentPivot = p_pivot;
}

void Editor::Panels::SceneView::ToggleCurrentGizmoPivot()
{
    m_currentPivot = Core::ToggleGizmoPivot(m_currentPivot);
}

Editor::Core::SceneViewGizmoSpace Editor::Panels::SceneView::GetCurrentGizmoSpace() const
{
    return m_currentSpace;
}

void Editor::Panels::SceneView::SetCurrentGizmoSpace(const Editor::Core::SceneViewGizmoSpace p_space)
{
    m_currentSpace = p_space;
}

void Editor::Panels::SceneView::ToggleCurrentGizmoSpace()
{
    m_currentSpace = Core::ToggleGizmoSpace(m_currentSpace);
}

void Editor::Panels::SceneView::InitFrame()
{
    AViewControllable::InitFrame();

    auto* debugRenderer = dynamic_cast<Editor::Rendering::DebugSceneRenderer*>(m_renderer.get());
    if (debugRenderer == nullptr)
        return;

    Engine::GameObject* selectedActor = nullptr;

    if (EDITOR_EXEC(IsAnyActorSelected()))
    {
        selectedActor = EDITOR_EXEC(GetSelectedActor());
    }

    m_requestPickingFrame = ShouldRequestPickingFrame();
    debugRenderer->AddDescriptor<Rendering::DebugSceneRenderer::DebugSceneDescriptor>({
        m_highlightedActor,
        selectedActor,
        m_requestPickingFrame});
}

Engine::SceneSystem::Scene* Editor::Panels::SceneView::GetScene()
{
    return m_sceneManager.GetCurrentScene();
}

Engine::Rendering::BaseSceneRenderer::SceneDescriptor Editor::Panels::SceneView::CreateSceneDescriptor()
{
    return AViewControllable::CreateSceneDescriptor();
}

bool Editor::Panels::SceneView::RequiresSynchronizedRetiredFramePresentation() const
{
    if (ShouldSceneViewSynchronizeRetiredFramePresentation())
        return true;

    using Windowing::Inputs::EMouseButton;

    const bool clickPickingRequested =
        m_requestPickingFrame &&
        EDITOR_CONTEXT(inputManager)->IsMouseButtonPressed(EMouseButton::MOUSE_BUTTON_LEFT) &&
        !m_cameraController.IsCameraControlActive();

    return m_cameraController.IsCameraControlActive() ||
        m_cameraMovedForPresentation ||
        m_gizmoInteraction.isUsing ||
        m_gizmoInteraction.isViewUsing ||
        clickPickingRequested ||
        m_pendingClickPickRenderPos.has_value();
}

void Editor::Panels::SceneView::DrawPreRenderViewportOverlay()
{
    if (ShouldApplySceneMutationFromViewportOverlay(ViewportOverlayLifecyclePhase::BeforeViewRender))
        DrawViewportOverlay();
}

void Editor::Panels::SceneView::OnAfterDrawWidgets()
{
    if (ShouldResolveViewportPicking(ViewportOverlayLifecyclePhase::AfterWidgetDraw))
        HandleActorPicking();
    EndViewportOverlayDrawListChannels();
}

void Editor::Panels::SceneView::DrawViewportOverlay()
{
    m_gizmoInteraction = {};
    if (Editor::Core::DoesShortcutSettingsWindowBlockSceneInput())
        return;

    const auto imageMin = HasViewportImageBounds()
        ? GetViewportImageMin()
        : GetCurrentViewportImageMin();
    const auto imageMax = HasViewportImageBounds()
        ? GetViewportImageMax()
        : GetCurrentViewportImageMax();
    const auto imageWidth = imageMax.x - imageMin.x;
    const auto imageHeight = imageMax.y - imageMin.y;
    if (imageWidth <= 0.0f || imageHeight <= 0.0f)
        return;

    const auto overlayMatrices = GetViewportOverlayCameraMatrices();
    auto viewMatrix = Core::ToImGuizmoMatrix(overlayMatrices.view);
    auto projectionMatrix = Core::ToImGuizmoMatrix(overlayMatrices.projection);
    const bool cameraControlActive = m_cameraController.IsCameraControlActive();
    EnsureCameraFocus();

    ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
    ImGuizmo::SetRect(imageMin.x, imageMin.y, imageWidth, imageHeight);

    const auto viewGizmoRect = Core::GetSceneViewViewGizmoRect(imageMin, imageMax);
    auto viewGizmoModelMatrix = Core::ToImGuizmoMatrix(Maths::Matrix4::Identity);
    ImGuizmo::ViewManipulate(
        viewMatrix.data(),
        projectionMatrix.data(),
        ToNativeImGuizmoOperation(m_currentOperation),
        ToNativeImGuizmoMode(m_currentSpace),
        viewGizmoModelMatrix.data(),
        kSceneViewGizmoCameraLength,
        ImVec2(viewGizmoRect.position.x, viewGizmoRect.position.y),
        ImVec2(viewGizmoRect.size.x, viewGizmoRect.size.y),
        IM_COL32(0, 0, 0, 0));

    m_gizmoInteraction.isViewHovered = ImGuizmo::IsViewManipulateHovered();
    m_gizmoInteraction.isViewUsing = ImGuizmo::IsUsingViewManipulate();
    if (Core::ShouldCancelViewGizmoCameraTransform(cameraControlActive, m_gizmoInteraction.isViewUsing))
    {
        ImGuizmo::CancelViewManipulate();
        m_gizmoInteraction.isViewUsing = false;
    }

    const ImGuiIO& io = ImGui::GetIO();
    const Maths::Vector2 mouseDelta {io.MouseDelta.x, io.MouseDelta.y};
    if (!cameraControlActive && Core::ShouldApplyViewGizmoCameraTransform(
        m_gizmoInteraction.isViewUsing,
        io.MouseDown[0],
        mouseDelta))
    {
        const auto cameraTransform = Core::GetCameraTransformFromViewMatrix(viewMatrix);
        float viewGizmoTargetDirection[3] {};
        const bool hasViewGizmoTargetDirection =
            ImGuizmo::GetViewManipulateTargetDirection(viewGizmoTargetDirection);
        const Maths::Vector3 targetForward {
            viewGizmoTargetDirection[0],
            viewGizmoTargetDirection[1],
            viewGizmoTargetDirection[2]
        };
        const Maths::Vector3 targetCameraForward =
            Core::GetCameraForwardFromImGuizmoViewTargetDirection(targetForward);
        const auto stableCameraTransform = Core::StabilizeViewGizmoCameraTransform(
            {m_camera.GetPosition(), m_camera.GetRotation()},
            cameraTransform,
            m_cameraFocus.focusDistance,
            m_cameraFocus.focusPoint,
            hasViewGizmoTargetDirection ? &targetCameraForward : nullptr);
        m_camera.SetPosition(stableCameraTransform.position);
        m_camera.SetRotation(stableCameraTransform.rotation);
        m_cameraFocus.focusDistance = Maths::Vector3::Distance(m_camera.GetPosition(), m_cameraFocus.focusPoint);
        m_cameraFocus.hasFocus = true;
        m_camera.CacheViewMatrix();
        m_cameraMovedForPresentation = true;
    }

    if (!EDITOR_EXEC(IsAnyActorSelected()))
        return;

    auto* selectedActor = EDITOR_EXEC(GetSelectedActor());
    if (selectedActor == nullptr || selectedActor->GetTransform() == nullptr)
        return;

    auto modelMatrix = Core::GetActorWorldGizmoMatrix(*selectedActor, m_currentPivot);

    using namespace Windowing::Inputs;
    const bool snapEnabled = Core::IsSnapModifierActive(
        EDITOR_CONTEXT(inputManager)->GetKeyState(EKey::KEY_LEFT_CONTROL),
        EDITOR_CONTEXT(inputManager)->GetKeyState(EKey::KEY_RIGHT_CONTROL));
    const float snapValue = Core::GetSnapValue(m_currentOperation);
    float snap[3] { snapValue, snapValue, snapValue };

    const bool manipulated = ImGuizmo::Manipulate(
        viewMatrix.data(),
        projectionMatrix.data(),
        ToNativeImGuizmoOperation(m_currentOperation),
        ToNativeImGuizmoMode(m_currentSpace),
        modelMatrix.data(),
        nullptr,
        snapEnabled ? snap : nullptr);

    m_gizmoInteraction.isHovered = !cameraControlActive && ImGuizmo::IsOver();
    m_gizmoInteraction.isUsing = !cameraControlActive && ImGuizmo::IsUsing();

    if (!cameraControlActive && manipulated)
    {
        Core::ApplyActorWorldGizmoMatrix(*selectedActor, modelMatrix, m_currentOperation, m_currentPivot);
        EDITOR_CONTEXT(sceneManager).MarkCurrentSceneDirty();
    }
}

void Editor::Panels::SceneView::EnsureCameraFocus()
{
    m_cameraFocus = Core::EnsureSceneCameraFocus(
        m_cameraFocus,
        m_camera.GetPosition(),
        m_camera.GetRotation() * Maths::Vector3::Forward,
        kSceneViewDefaultFocusDistance);
}

bool IsResizing()
{
    auto cursor = NLS_SERVICE(UI::UIManager).GetMouseCursor();

    return cursor == ImGuiMouseCursor_ResizeEW || cursor == ImGuiMouseCursor_ResizeNS || cursor == ImGuiMouseCursor_ResizeNWSE || cursor == ImGuiMouseCursor_ResizeNESW || cursor == ImGuiMouseCursor_ResizeAll;
}

bool Editor::Panels::SceneView::ShouldRequestPickingFrame() const
{
    using namespace Windowing::Inputs;
    if (Editor::Core::DoesShortcutSettingsWindowBlockSceneInput())
        return false;

    const bool pickingSuppressedByGizmo = Core::ShouldSuppressScenePicking(m_gizmoInteraction);
    if (pickingSuppressedByGizmo)
    {
        if (EDITOR_CONTEXT(inputManager)->IsMouseButtonPressed(EMouseButton::MOUSE_BUTTON_LEFT) ||
            m_pendingClickPickRenderPos.has_value())
        {
            std::ostringstream stream;
            stream << "request blocked by gizmo"
                << " leftPressed=" << EDITOR_CONTEXT(inputManager)->IsMouseButtonPressed(EMouseButton::MOUSE_BUTTON_LEFT)
                << " pendingClick=" << m_pendingClickPickRenderPos.has_value()
                << " hover=" << m_gizmoInteraction.isHovered
                << " using=" << m_gizmoInteraction.isUsing
                << " viewHover=" << m_gizmoInteraction.isViewHovered
                << " viewUsing=" << m_gizmoInteraction.isViewUsing;
            LogScenePickingDiagnostics(stream.str());
        }
        return false;
    }

    auto& inputManager = *EDITOR_CONTEXT(inputManager);
    const Maths::Vector2 screenMousePos = inputManager.GetMousePosition();
    const bool mouseOverView = IsMouseWithinView(screenMousePos);
    if (!mouseOverView || IsResizing())
    {
        if (inputManager.IsMouseButtonPressed(EMouseButton::MOUSE_BUTTON_LEFT) ||
            m_pendingClickPickRenderPos.has_value())
        {
            std::ostringstream stream;
            stream << "request blocked by bounds"
                << " mouseOverView=" << mouseOverView
                << " resizing=" << IsResizing()
                << " mouse=(" << screenMousePos.x << "," << screenMousePos.y << ")"
                << " hasBounds=" << HasViewportImageBounds();
            LogScenePickingDiagnostics(stream.str());
        }
        return false;
    }

    const auto localMousePos = GetLocalViewPosition(screenMousePos);
    if (!localMousePos.has_value())
        return false;

    auto mousePos = localMousePos.value();
    const auto [safeWidth, safeHeight] = GetSafeSize();
    const float maxRenderX = std::max(0.0f, static_cast<float>(safeWidth) - 1.0f);
    const float maxRenderY = std::max(0.0f, static_cast<float>(safeHeight) - 1.0f);

    mousePos.x = std::clamp(mousePos.x, 0.0f, maxRenderX);
    mousePos.y = NLS_SERVICE(UI::UIManager).UsesBottomLeftRenderTargetOrigin()
        ? std::clamp(static_cast<float>(safeHeight) - 1.0f - mousePos.y, 0.0f, maxRenderY)
        : std::clamp(mousePos.y, 0.0f, maxRenderY);

    const auto now = std::chrono::steady_clock::now();
    const bool mouseMoved =
        std::fabs(mousePos.x - m_lastPickingMousePos.x) > 0.5f ||
        std::fabs(mousePos.y - m_lastPickingMousePos.y) > 0.5f;
    constexpr int kHoverPickingIntervalMs = 120;
    const auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastPickingSampleTime).count();
    const bool sampleExpired =
        !m_hasPickingSample ||
        elapsedMs >= kHoverPickingIntervalMs;
    const bool leftClicked = inputManager.IsMouseButtonPressed(EMouseButton::MOUSE_BUTTON_LEFT);
    const bool cameraControlActive = m_cameraController.IsCameraControlActive();

    const bool request = ShouldRenderScenePickingFrame(
        mouseOverView,
        false,
        false,
        m_pendingClickPickRenderPos.has_value(),
        leftClicked,
        cameraControlActive,
        sampleExpired,
        mouseMoved,
        m_hasPickingSample);
    if (leftClicked || m_pendingClickPickRenderPos.has_value())
    {
        std::ostringstream stream;
        stream << "request"
            << " result=" << request
            << " leftClicked=" << leftClicked
            << " pendingClick=" << m_pendingClickPickRenderPos.has_value()
            << " cameraControl=" << cameraControlActive
            << " sampleExpired=" << sampleExpired
            << " mouseMoved=" << mouseMoved
            << " hasSample=" << m_hasPickingSample
            << " local=(" << mousePos.x << "," << mousePos.y << ")";
        LogScenePickingDiagnostics(stream.str());
    }
    return request;
}

void Editor::Panels::SceneView::HandleActorPicking()
{
    if (Editor::Core::DoesShortcutSettingsWindowBlockSceneInput())
    {
        m_highlightedActor = nullptr;
        m_hasPickingSample = false;
        return;
    }

    auto* debugRenderer = dynamic_cast<Editor::Rendering::DebugSceneRenderer*>(m_renderer.get());
    if (debugRenderer == nullptr)
    {
        m_highlightedActor = nullptr;
        m_hasPickingSample = false;
        return;
    }

    using namespace Windowing::Inputs;

    auto& inputManager = *EDITOR_CONTEXT(inputManager);
    const Maths::Vector2 screenMousePos = inputManager.GetMousePosition();
    const bool mouseOverView = IsMouseWithinView(screenMousePos);

    if (inputManager.IsMouseButtonReleased(EMouseButton::MOUSE_BUTTON_LEFT))
    {
        m_gizmoInteraction.isUsing = false;
    }

    if (Core::ShouldSuppressScenePicking(m_gizmoInteraction))
    {
        if (inputManager.IsMouseButtonPressed(EMouseButton::MOUSE_BUTTON_LEFT) ||
            m_pendingClickPickRenderPos.has_value())
        {
            std::ostringstream stream;
            stream << "handle blocked by gizmo"
                << " leftPressed=" << inputManager.IsMouseButtonPressed(EMouseButton::MOUSE_BUTTON_LEFT)
                << " leftReleased=" << inputManager.IsMouseButtonReleased(EMouseButton::MOUSE_BUTTON_LEFT)
                << " pendingClick=" << m_pendingClickPickRenderPos.has_value()
                << " hover=" << m_gizmoInteraction.isHovered
                << " using=" << m_gizmoInteraction.isUsing
                << " viewHover=" << m_gizmoInteraction.isViewHovered
                << " viewUsing=" << m_gizmoInteraction.isViewUsing;
            LogScenePickingDiagnostics(stream.str());
        }
        return;
    }

    if (mouseOverView && !IsResizing())
    {
        auto& actorPickingFeature = debugRenderer->GetPass<Rendering::PickingRenderPass>("Picking");
        if (!actorPickingFeature.SupportsPickingReadback())
        {
            m_highlightedActor = nullptr;
            m_hasPickingSample = false;
            return;
        }

        const auto localMousePos = GetLocalViewPosition(screenMousePos);
        if (!localMousePos.has_value())
        {
            m_highlightedActor = nullptr;
            m_hasPickingSample = false;
            return;
        }

        auto mousePos = localMousePos.value();
        const auto [safeWidth, safeHeight] = GetSafeSize();
        const float maxRenderX = std::max(0.0f, static_cast<float>(safeWidth) - 1.0f);
        const float maxRenderY = std::max(0.0f, static_cast<float>(safeHeight) - 1.0f);

        mousePos.x = std::clamp(mousePos.x, 0.0f, maxRenderX);
        mousePos.y = NLS_SERVICE(UI::UIManager).UsesBottomLeftRenderTargetOrigin()
            ? std::clamp(static_cast<float>(safeHeight) - 1.0f - mousePos.y, 0.0f, maxRenderY)
            : std::clamp(mousePos.y, 0.0f, maxRenderY);

        const auto now = std::chrono::steady_clock::now();
        const bool mouseMoved =
            std::fabs(mousePos.x - m_lastPickingMousePos.x) > 0.5f ||
            std::fabs(mousePos.y - m_lastPickingMousePos.y) > 0.5f;
        constexpr int kHoverPickingIntervalMs = 120;
        const auto elapsedMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastPickingSampleTime).count();
        const bool sampleExpired =
            !m_hasPickingSample ||
            elapsedMs >= kHoverPickingIntervalMs;
        const bool leftClicked = inputManager.IsMouseButtonPressed(EMouseButton::MOUSE_BUTTON_LEFT);
        const bool cameraControlActive = m_cameraController.IsCameraControlActive();
        const bool queuedClickPickThisFrame = leftClicked && !cameraControlActive;
        if (queuedClickPickThisFrame)
        {
            m_pendingClickPickRenderPos = mousePos;
            m_pendingClickMinReadablePickingFrameSerial =
                actorPickingFeature.GetSubmittedPickingFrameSerial();
            std::ostringstream stream;
            stream << "queued click"
                << " local=(" << mousePos.x << "," << mousePos.y << ")"
                << " minReadableSerial=" << m_pendingClickMinReadablePickingFrameSerial
                << " currentReadableSerial=" << actorPickingFeature.GetReadablePickingFrameSerial()
                << " requestThisFrame=" << m_requestPickingFrame;
            LogScenePickingDiagnostics(stream.str());
        }

        const bool shouldRepick = ShouldRenderScenePickingFrame(
            mouseOverView,
            false,
            false,
            m_pendingClickPickRenderPos.has_value(),
            leftClicked,
            cameraControlActive,
            sampleExpired,
            mouseMoved,
            m_hasPickingSample);
        if (shouldRepick)
        {
            m_highlightedActor = nullptr;

            const bool pickingFrameReadable = actorPickingFeature.HasReadablePickingFrame();
            const bool resolvePendingClickPick = ShouldResolvePendingSceneClickPick(
                m_pendingClickPickRenderPos.has_value(),
                queuedClickPickThisFrame,
                cameraControlActive,
                m_pendingClickMinReadablePickingFrameSerial,
                actorPickingFeature.GetReadablePickingFrameSerial());
            if (queuedClickPickThisFrame || m_pendingClickPickRenderPos.has_value())
            {
                std::ostringstream stream;
                stream << "repick"
                    << " shouldRepick=" << shouldRepick
                    << " readable=" << pickingFrameReadable
                    << " readableSerial=" << actorPickingFeature.GetReadablePickingFrameSerial()
                    << " minReadableSerial=" << m_pendingClickMinReadablePickingFrameSerial
                    << " resolvePending=" << resolvePendingClickPick
                    << " requestThisFrame=" << m_requestPickingFrame;
                LogScenePickingDiagnostics(stream.str());
            }
            if (!cameraControlActive && pickingFrameReadable &&
                (!queuedClickPickThisFrame || resolvePendingClickPick))
            {
                const bool resolveClickPick = resolvePendingClickPick;
                const auto samplePos = resolveClickPick
                    ? m_pendingClickPickRenderPos.value()
                    : mousePos;
                m_highlightedActor = resolveClickPick
                    ? PickActorNearRenderCoordinate(actorPickingFeature, samplePos, maxRenderX, maxRenderY)
                    : PickActorAtRenderCoordinate(actorPickingFeature, mousePos.x, mousePos.y);
                if (resolveClickPick)
                {
                    std::ostringstream stream;
                    stream << "resolved click"
                        << " hit=" << (m_highlightedActor != nullptr)
                        << " actor=" << (m_highlightedActor != nullptr ? m_highlightedActor->GetName() : std::string("<none>"));
                    LogScenePickingDiagnostics(stream.str());
                }
            }

            if (pickingFrameReadable)
            {
                m_lastPickingMousePos = mousePos;
                m_lastPickingSampleTime = now;
                m_hasPickingSample = true;
            }
        }
        else if (cameraControlActive)
        {
            m_highlightedActor = {};
        }

        const bool resolvePendingClickPick = ShouldResolvePendingSceneClickPick(
            m_pendingClickPickRenderPos.has_value(),
            queuedClickPickThisFrame,
            cameraControlActive,
            m_pendingClickMinReadablePickingFrameSerial,
            actorPickingFeature.GetReadablePickingFrameSerial());
        if (resolvePendingClickPick)
        {
            if (m_highlightedActor)
            {
                LogScenePickingDiagnostics("select actor=" + m_highlightedActor->GetName());
                EDITOR_EXEC(SelectActor(*m_highlightedActor));
                m_pendingClickPickRenderPos.reset();
            }
            else if (actorPickingFeature.HasReadablePickingFrame())
            {
                LogScenePickingDiagnostics("unselect actor from click");
                EDITOR_EXEC(UnselectActor());
                m_pendingClickPickRenderPos.reset();
            }
            m_pendingClickMinReadablePickingFrameSerial = 0u;
        }
    }
    else
    {
        if (inputManager.IsMouseButtonPressed(EMouseButton::MOUSE_BUTTON_LEFT) ||
            m_pendingClickPickRenderPos.has_value())
        {
            std::ostringstream stream;
            stream << "handle blocked by bounds"
                << " mouseOverView=" << mouseOverView
                << " resizing=" << IsResizing()
                << " mouse=(" << screenMousePos.x << "," << screenMousePos.y << ")"
                << " hasBounds=" << HasViewportImageBounds();
            LogScenePickingDiagnostics(stream.str());
        }
        m_highlightedActor = nullptr;
        m_hasPickingSample = false;
    }
}
