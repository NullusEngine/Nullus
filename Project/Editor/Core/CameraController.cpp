
#include "Core/EditorActions.h"

#include "ImGui/imgui.h"

#include "Core/CameraController.h"
#include "Core/SceneCameraFocus.h"
#include "Core/EditorGuiInput.h"
#include "Shortcuts/EditorShortcutService.h"
#include "Panels/SceneView.h"

#include <algorithm>

#include <Components/LightComponent.h>
#include <Components/MeshFilter.h>
#include <Components/TransformComponent.h>
#include <Rendering/Geometry/BoundingSphereUtils.h>
#include "UI/UIManager.h"
#include "ServiceLocator.h"
using namespace NLS;
namespace { Editor::Core::CameraController* g_keyboardCamera = nullptr; uint64_t g_keyboardCameraFrame = 0; }

Editor::Core::CameraController::CameraController(
	Editor::Panels::AView& p_view,
	Render::Entities::Camera& p_camera
) : CameraController(p_camera,*EDITOR_CONTEXT(inputManager),*EDITOR_CONTEXT(window))
{
    m_view = &p_view;
    m_camera.SetFov(60.0f);
}

Editor::Core::CameraController::CameraController(Render::Entities::Camera& camera,
    Windowing::Inputs::InputManager& input, Windowing::Window& window)
    : m_inputManager(input),m_window(window),m_camera(camera)
{}

Editor::Core::CameraController::~CameraController()
{
    if (g_keyboardCamera == this) g_keyboardCamera = nullptr;
    if (IsCameraControlActive() || m_forcedNoMouseCursorChange) ResetMouseInteractionState();
}

void Editor::Core::CameraController::SetViewportInputBounds(const Maths::Vector2& minimum,const Maths::Vector2& maximum,bool available)
{
    m_viewportMin = minimum; m_viewportMax = maximum;
    m_hasViewportBounds = available && maximum.x > minimum.x && maximum.y > minimum.y;
    SetViewportHeight(maximum.y-minimum.y);
}

void Editor::Core::CameraController::ClearViewportInputBounds()
{
    m_hasViewportBounds = false; m_inputActive = false;
    m_leftMousePressed = false; m_firstMouse = true;
    m_currentMovementSpeed = Maths::Vector3::Zero; m_targetSpeed = Maths::Vector3::Zero;
    if (IsCameraControlActive() || m_forcedNoMouseCursorChange) ResetMouseInteractionState();
}

float GetActorFocusDist(Engine::GameObject& p_gameObject)
{
	float distance = 4.0f;

	if (p_gameObject.IsActive())
	{
		if (p_gameObject.GetComponent<Engine::Components::MeshRenderer>() != nullptr)
		{
			auto* meshFilter = p_gameObject.GetComponent<Engine::Components::MeshFilter>();
			const auto* mesh = meshFilter != nullptr ? meshFilter->ResolveMesh() : nullptr;
			const auto& actorPosition = p_gameObject.GetTransform()->GetWorldPosition();

            if (mesh != nullptr)
            {
                const auto worldBounds = Render::Geometry::TransformBounds(
                    mesh->GetBounds(),
                    p_gameObject.GetTransform()->GetWorldMatrix());
                const auto halfExtents = worldBounds.size * 0.5f;
                const float boundsRadius = Maths::Vector3::Length(halfExtents);
                const float pivotOffset = Maths::Vector3::Distance(worldBounds.center, actorPosition);
                distance = std::max(distance, (boundsRadius + pivotOffset) * 2.0f);
            }
            else
            {
                distance = std::max(distance, 10.0f);
            }
		}

		for (auto child : p_gameObject.GetChildren())
			distance = std::max(distance, GetActorFocusDist(*child));
	}

	return distance;
}

Editor::Core::CameraController* Editor::Core::CameraController::GetKeyboardInputTarget()
{
    return g_keyboardCamera && g_keyboardCameraFrame == g_keyboardCamera->m_inputManager.GetInputSystem().GetFrameIndex() ? g_keyboardCamera : nullptr;
}
bool Editor::Core::CameraController::IsSceneViewCamera() const { return dynamic_cast<Panels::SceneView*>(m_view) != nullptr; }
bool Editor::Core::CameraController::NavigationOwnsInput()
{
    auto* camera = g_keyboardCamera;
    if (!camera || camera->m_inputBlocked || !camera->m_inputManager.HasFocusedWindow()) return false;
    if (camera->IsCameraControlActive()) return true;
    const auto fly = Shortcuts::GetEditorInputState(Shortcuts::EditorInputAction::CameraFly);
    return fly.pressed && fly.actuated &&
        camera->m_view && camera->m_view->IsMouseHoveringView(camera->m_inputManager.GetMousePosition());
}
bool Editor::Core::CameraController::FocusInputTarget(const Maths::Vector3& angle)
{
    auto* target = GetTargetGameObject();
    if (m_inputBlocked || (!target && (!m_focusState || !m_focusState->hasFocus))) return false;
    const auto point = target ? target->GetTransform()->GetWorldPosition() : m_focusState->focusPoint;
    const auto distance = target ? GetActorFocusDist(*target) : m_focusState->focusDistance;
    if (angle == Maths::Vector3::Zero) { MoveToPoint(point, distance); return true; }
    if (m_planarNavigation) return false;
    const auto position = point + angle * distance;
    const auto direction = Maths::Vector3::Normalize(point - position);
    m_camera.SetRotation(Maths::Quaternion::LookAt(direction, std::abs(direction.y) == 1.0f ? Maths::Vector3::Right : Maths::Vector3::Up));
    m_cameraDestinations.push({position, m_camera.GetRotation()});
    if (m_focusState) *m_focusState = {point, distance, true};
    return true;
}

bool Editor::Core::CameraController::UpdatePointerState(bool& viewportInputAvailable, bool& mouseOverView)
{
    viewportInputAvailable = mouseOverView = false;
    if (m_inputBlocked)
    {
        ResetMouseInteractionState();
        return false;
    }

    if (!m_inputManager.HasFocusedWindow())
    {
        ResetMouseInteractionState();
        return false;
    }

    const Maths::Vector2 mousePosition = m_inputManager.GetMousePosition();
    if (IsCameraControlActive() && !m_inputManager.GetPlatformBackend().OwnsPointerCapture(this)) ResetNavigationInteractionState();
    viewportInputAvailable = m_view ? m_view->HasViewportImageInputBounds() : m_hasViewportBounds;
    mouseOverView = viewportInputAvailable && (m_view ? m_view->IsMouseHoveringView(mousePosition) :
        mousePosition.x >= m_viewportMin.x && mousePosition.x < m_viewportMax.x &&
        mousePosition.y >= m_viewportMin.y && mousePosition.y < m_viewportMax.y);
    const bool inputActive = mouseOverView && (!m_view || m_inputActive);
    if (inputActive && !ImGui::IsAnyItemActive())
    { g_keyboardCamera = this; g_keyboardCameraFrame = m_inputManager.GetInputSystem().GetFrameIndex(); }
    const bool capturingMouse = m_leftMousePressed || IsCameraControlActive();
    const bool shouldProcessMouseState = inputActive || capturingMouse;

    if (shouldProcessMouseState) UpdateMouseState(inputActive);
    return true;
}

void Editor::Core::CameraController::RefreshPointerCaptureAfterUi()
{
    const bool wasNavigating = IsCameraControlActive();
    bool viewportInputAvailable = false, mouseOverView = false;
    if (UpdatePointerState(viewportInputAvailable, mouseOverView) && !wasNavigating && IsCameraControlActive())
        ConsumeSuppressedMouseDelta(m_inputManager.GetMousePosition());
}

void Editor::Core::CameraController::HandleInputs(float p_deltaTime)
{
    bool viewportInputAvailable = false, mouseOverView = false;
    if (!UpdatePointerState(viewportInputAvailable, mouseOverView)) return;

	if (!m_cameraDestinations.empty())
	{
		m_currentMovementSpeed = 0.0f;

		while (m_cameraDestinations.size() != 1)
			m_cameraDestinations.pop();

		auto& [destPos, destRotation] = m_cameraDestinations.front();

		float t = m_focusLerpCoefficient * p_deltaTime;

		if (Maths::Vector3::Distance(m_camera.GetPosition(), destPos) <= 0.03f)
		{
			m_camera.SetPosition(destPos);
			m_camera.SetRotation(destRotation);
			m_cameraDestinations.pop();
		}
		else
		{
			m_camera.SetPosition(Maths::Vector3::Lerp(m_camera.GetPosition(), destPos, t));
			m_camera.SetRotation(Maths::Quaternion::Lerp(m_camera.GetRotation(), destRotation, t));
		}
	} 
	else
	{
        if (IsCameraControlActive())
        {
            m_inputManager.PollInfiniteCursorWrap();
			auto pos = m_inputManager.GetMousePosition();

            if (!ConsumeSuppressedMouseDelta(pos))
            {
                bool wasFirstMouse = m_firstMouse;

                if (m_firstMouse)
                {
                    ResetLastMousePosition(pos);
                    m_firstMouse = false;
                }

                Maths::Vector2 mouseOffset = Shortcuts::GetEditorInputState(Shortcuts::EditorInputAction::CameraLook).value.AsVector2();
                const float maxMouseDeltaPerFrame = m_interactionState == SceneViewCameraInteractionStateId::Fly
                    ? GetMaxLookMouseDeltaPerFrame()
                    : GetMaxPanOrbitMouseDeltaPerFrame();
                mouseOffset = ClampMouseDeltaForCameraControl(mouseOffset, maxMouseDeltaPerFrame);

                ResetLastMousePosition(pos);

                if (m_interactionState == SceneViewCameraInteractionStateId::Fly)
                {
                    HandleCameraFPSMouse(mouseOffset, wasFirstMouse);
                }
                else if (m_interactionState == SceneViewCameraInteractionStateId::Orbit)
                {
                    if (auto target = GetTargetGameObject())
                    {
                        HandleCameraOrbit(target->GetTransform()->GetWorldPosition(), mouseOffset, wasFirstMouse);
                    }
                    else if (m_focusState && m_focusState->hasFocus)
                        HandleCameraOrbit(m_focusState->focusPoint,mouseOffset,wasFirstMouse);
                }
                else if (m_interactionState == SceneViewCameraInteractionStateId::Pan)
                {
                    HandleCameraPanning(mouseOffset, wasFirstMouse);
                }
            }
		}

		if (ShouldHandleWheelZoom(viewportInputAvailable, mouseOverView))
		{
			HandleCameraZoom();
		}

		HandleCameraFPSKeyboard(p_deltaTime);
	}
}

void Editor::Core::CameraController::MoveToTarget(Engine::GameObject& p_target)
{
    const Maths::Vector3 focusPoint = p_target.GetTransform()->GetWorldPosition();
    const float focusDistance = GetActorFocusDist(p_target);
    MoveToPoint(focusPoint,focusDistance);
}

void Editor::Core::CameraController::MoveToPoint(const Maths::Vector3& focusPoint,const float focusDistance)
{
    if (m_camera.GetProjectionMode() == Render::Settings::EProjectionMode::ORTHOGRAPHIC)
        m_camera.SetSize(std::max(0.01f, focusDistance * 0.5f));
	m_cameraDestinations.push({
		focusPoint -
		m_camera.GetRotation() *
		Maths::Vector3::Forward *
		focusDistance,
		m_camera.GetRotation()
	});
    if (m_focusState != nullptr)
    {
        *m_focusState = {
            focusPoint,
            focusDistance,
            true
        };
    }
}

void Editor::Core::CameraController::ResetMouseInteractionState()
{
    m_leftMousePressed = false;
    ResetNavigationInteractionState();
}

void Editor::Core::CameraController::ResetNavigationInteractionState()
{
    const bool ownedCursorControl = IsCameraControlActive() || m_forcedNoMouseCursorChange;
    m_firstMouse = true;
    m_pendingMouseDeltaSuppressionFrames = 0u;
    if (m_forcedNoMouseCursorChange)
    {
        NLS_SERVICE(NLS::UI::UIManager).PopCustomCursorControl();
        m_forcedNoMouseCursorChange = false;
    }
    if (ownedCursorControl)
    {
        m_inputManager.SetPointerCapture(false, this);
        m_window.SetCursorMode(Cursor::ECursorMode::NORMAL);
    }
    if (const auto restoredCursorShape = m_cursorShapeLease.Release())
        m_window.SetCursorShape(*restoredCursorShape);
    m_interactionState = SceneViewCameraInteractionStateId::Neutral;
}

void Editor::Core::CameraController::ResetLastMousePosition(const Maths::Vector2& p_mousePosition)
{
    m_lastMousePosX = p_mousePosition.x;
    m_lastMousePosY = p_mousePosition.y;
}

void Editor::Core::CameraController::SuppressMouseDeltaAfterCursorCapture()
{
    m_pendingMouseDeltaSuppressionFrames = GetLookCaptureSuppressionFrameCount();
    m_firstMouse = true;
    ResetLastMousePosition(m_inputManager.GetMousePosition());
}

bool Editor::Core::CameraController::ConsumeSuppressedMouseDelta(const Maths::Vector2& p_mousePosition)
{
    if (m_pendingMouseDeltaSuppressionFrames == 0u)
        return false;

    ResetLastMousePosition(p_mousePosition);
    --m_pendingMouseDeltaSuppressionFrames;
    if (m_interactionState == SceneViewCameraInteractionStateId::Fly)
    {
        // Seed the look orientation during the suppressed capture frame, so
        // the next movement is not discarded as another "first" sample.
        HandleCameraFPSMouse({}, true);
        m_firstMouse = false;
    }
    return true;
}

void Editor::Core::CameraController::SetSpeed(float p_speed)
{
	m_cameraMoveSpeed = std::clamp(p_speed, m_minCameraSpeed, m_maxCameraSpeed);
}

float Editor::Core::CameraController::GetSpeed() const
{
	return m_cameraMoveSpeed;
}

void Editor::Core::CameraController::SetPosition(const Maths::Vector3 & p_position)
{
	m_camera.SetPosition(p_position);
}

void Editor::Core::CameraController::SetRotation(const Maths::Quaternion & p_rotation)
{
	m_camera.SetRotation(p_rotation);
}

const Maths::Vector3& Editor::Core::CameraController::GetPosition() const
{
	return m_camera.GetPosition();
}

const Maths::Quaternion& Editor::Core::CameraController::GetRotation() const
{
	return m_camera.GetRotation();
}

bool Editor::Core::CameraController::IsRightMousePressed() const
{
	return m_interactionState == SceneViewCameraInteractionStateId::Fly;
}

bool Editor::Core::CameraController::IsMiddleMousePressed() const
{
    return m_interactionState == SceneViewCameraInteractionStateId::Pan ||
        m_interactionState == SceneViewCameraInteractionStateId::Orbit;
}

bool Editor::Core::CameraController::IsCameraControlActive() const
{
    return SceneViewCameraInteractionStateMachine::IsNavigationState(m_interactionState);
}

void Editor::Core::CameraController::SetFocusState(SceneCameraFocusState* p_focusState)
{
    m_focusState = p_focusState;
}

void Editor::Core::CameraController::SetViewportHeight(const float p_viewportHeight)
{
    m_viewportHeight = std::max(1.0f, p_viewportHeight);
}

void Editor::Core::CameraController::SetInputActive(const bool p_inputActive)
{
    m_inputActive = p_inputActive;
}

void Editor::Core::CameraController::SetInputBlocked(const bool p_inputBlocked)
{
    const bool shouldResetMouseInteraction =
        ShouldResetMouseInteractionForInputBlockChange(m_inputBlocked, p_inputBlocked);
    m_inputBlocked = p_inputBlocked;
    if (shouldResetMouseInteraction)
        ResetMouseInteractionState();
}

bool Editor::Core::CameraController::IsInputBlocked() const
{
    return m_inputBlocked;
}

void Editor::Core::CameraController::LockTargetGameObject(Engine::GameObject& p_gameObject)
{
	m_lockedGameObject = &p_gameObject;
}

void Editor::Core::CameraController::UnLockTargetGameObject()
{
	m_lockedGameObject = nullptr;
}

Engine::GameObject* Editor::Core::CameraController::GetTargetGameObject() const
{
	if (m_lockedGameObject)
	{
		return m_lockedGameObject;
	}
    else if (m_view && EDITOR_EXEC(IsAnyGameObjectSelected()))
	{
		return EDITOR_EXEC(GetSelectedGameObject());
	}

	return nullptr;
}

void Editor::Core::CameraController::HandleCameraPanning(const Maths::Vector2& p_mouseOffset, bool p_firstMouset)
{
    Maths::Vector3 panDelta;
    if (m_camera.GetProjectionMode() == Render::Settings::EProjectionMode::ORTHOGRAPHIC)
    {
        const float unitsPerPixel = 2.0f * m_camera.GetSize() / std::max(1.0f, m_viewportHeight);
        panDelta = m_camera.transform->GetWorldRight() * (p_mouseOffset.x * unitsPerPixel) -
            m_camera.transform->GetWorldUp() * (p_mouseOffset.y * unitsPerPixel);
    }
    else if (m_focusState != nullptr && m_focusState->hasFocus)
    {
        panDelta = GetSceneCameraPanDelta(
            m_camera.GetRotation(),
            p_mouseOffset,
            m_focusState->focusDistance,
            m_camera.GetFov(),
            m_viewportHeight);
    }
    else
    {
	    auto mouseOffset = p_mouseOffset * m_cameraDragSpeed;
        panDelta = m_camera.transform->GetWorldRight() * mouseOffset.x -
            m_camera.transform->GetWorldUp() * mouseOffset.y;
    }

	m_camera.SetPosition(m_camera.GetPosition() + panDelta);
    if (m_focusState != nullptr)
        *m_focusState = UpdateSceneCameraFocusAfterPan(*m_focusState, panDelta);
}

Maths::Vector3 RemoveRoll(const Maths::Vector3& p_ypr)
{
	Maths::Vector3 result = p_ypr;

	if (result.z >= 179.0f || result.z <= -179.0f)
	{
		result.x += result.z;
		result.y = 180.0f - result.y;
		result.z = 0.0f;
	}

	if (result.x > 180.0f) result.x -= 360.0f;
	if (result.x < -180.0f) result.x += 360.0f;

	return result;
}

void Editor::Core::CameraController::HandleCameraOrbit(
    const Maths::Vector3& target,
	const Maths::Vector2& p_mouseOffset,
	bool p_firstMouse
)
{
	auto mouseOffset = p_mouseOffset * m_cameraOrbitSpeed;

	if (p_firstMouse)
	{
		m_ypr = Maths::Quaternion::EulerAngles(m_camera.GetRotation());
		m_ypr = RemoveRoll(m_ypr);
        m_orbitStartOffset = -Maths::Vector3::Forward * Maths::Vector3::Distance(target, m_camera.GetPosition());
	}

	m_ypr.y += -mouseOffset.x;
	m_ypr.x += -mouseOffset.y;
	m_ypr.x = std::max(std::min(m_ypr.x, 90.0f), -90.0f);

    Maths::Transform pivotTransform(target);
	Maths::Transform cameraTransform(m_orbitStartOffset);
	cameraTransform.SetParent(pivotTransform);
	pivotTransform.RotateLocal(Maths::Quaternion(m_ypr));
	m_camera.SetPosition(cameraTransform.GetWorldPosition());
	m_camera.SetRotation(cameraTransform.GetWorldRotation());
    if (m_focusState != nullptr)
    {
        *m_focusState = {
            target,
            Maths::Vector3::Distance(target, m_camera.GetPosition()),
            true
        };
    }
}

void Editor::Core::CameraController::HandleCameraZoom()
{
    const float mouseWheel = Shortcuts::GetEditorInputState(Shortcuts::EditorInputAction::CameraZoom).value.vector.y;
    if (mouseWheel == 0.0f)
        return;

    if (m_camera.GetProjectionMode() == Render::Settings::EProjectionMode::ORTHOGRAPHIC)
    {
        m_camera.SetSize(std::clamp(m_camera.GetSize() * std::exp(-mouseWheel * 0.15f), 0.01f, 100000.0f));
        return;
    }

    const float focusDistance =
        (m_focusState != nullptr && m_focusState->hasFocus)
            ? m_focusState->focusDistance
            : m_focusDistance;
    const Maths::Vector3 zoomDelta = GetSceneCameraZoomDelta(
        m_camera.transform->GetWorldForward(),
        mouseWheel,
        focusDistance);
    m_camera.SetPosition(m_camera.GetPosition() + zoomDelta);
    if (m_focusState != nullptr)
        *m_focusState = UpdateSceneCameraFocusAfterZoom(*m_focusState, m_camera.GetPosition());
}

void Editor::Core::CameraController::HandleCameraFPSMouse(const Maths::Vector2& p_mouseOffset, bool p_firstMouse)
{
	auto mouseOffset = p_mouseOffset * m_mouseSensitivity;

	if (p_firstMouse)
	{
		m_ypr = Maths::Quaternion::EulerAngles(m_camera.GetRotation());
		m_ypr = RemoveRoll(m_ypr);
        return;
	}

	m_ypr.y -= mouseOffset.x;
	m_ypr.x += -mouseOffset.y;
	m_ypr.x = std::max(std::min(m_ypr.x, 90.0f), -90.0f);

	m_camera.SetRotation(Maths::Quaternion(m_ypr));
    if (m_focusState != nullptr)
    {
        *m_focusState = UpdateSceneCameraFocusAfterRotation(
            *m_focusState,
            m_camera.GetPosition(),
            m_camera.GetRotation() * Maths::Vector3::Forward);
    }
}

void Editor::Core::CameraController::HandleCameraFPSKeyboard(float p_deltaTime)
{
	m_targetSpeed = Maths::Vector3(0.f, 0.f, 0.f);

    if (!m_planarNavigation && m_interactionState == SceneViewCameraInteractionStateId::Fly)
	{
            const bool run = Shortcuts::GetEditorInputState(Shortcuts::EditorInputAction::CameraBoost).actuated;
		float velocity = m_cameraMoveSpeed * p_deltaTime * (run ? 2.0f : 1.0f);

        if (NLS::Core::ServiceLocator::Contains<Editor::Shortcuts::EditorShortcutService>())
        {
            const auto move = NLS_SERVICE(Editor::Shortcuts::EditorShortcutService).GetNavigationValue();
            m_targetSpeed = (m_camera.transform->GetWorldForward() * move.z -
                m_camera.transform->GetWorldRight() * move.x + Maths::Vector3(0, move.y, 0)) * velocity;
        }
	}

	if (Maths::Vector3::Length(m_targetSpeed) > 0.0f)
    {
        m_flyMovementDuration += std::max(0.0f, p_deltaTime);
        if (m_cameraAcceleration) m_targetSpeed *= std::min(8.0f, 1.0f + m_flyMovementDuration * 0.5f);
    }
    else m_flyMovementDuration = 0.0f;
    m_currentMovementSpeed = m_cameraEasing ?
        Maths::Vector3::Lerp(m_currentMovementSpeed, m_targetSpeed, std::clamp(10.0f * p_deltaTime, 0.0f, 1.0f)) : m_targetSpeed;
	m_camera.SetPosition(m_camera.GetPosition() + m_currentMovementSpeed);
    if (m_focusState != nullptr && Maths::Vector3::Length(m_currentMovementSpeed) > 0.0f)
        *m_focusState = UpdateSceneCameraFocusAfterPan(*m_focusState, m_currentMovementSpeed);
}

void Editor::Core::CameraController::ApplyInteractionTransition(const SceneViewCameraInteractionTransition& p_transition)
{
    if (p_transition.shouldAcquireCursorControl && !m_forcedNoMouseCursorChange && !m_inputManager.SetPointerCapture(true, this)) return;
    if (p_transition.shouldReleaseCursorControl && m_forcedNoMouseCursorChange)
    {
        NLS_SERVICE(NLS::UI::UIManager).PopCustomCursorControl();
        m_forcedNoMouseCursorChange = false;
    }

    if (p_transition.shouldDisableInfiniteWrap)
        m_inputManager.SetPointerCapture(false, this);

    if (p_transition.shouldResetTransientMouseState)
    {
        m_firstMouse = true;
        m_pendingMouseDeltaSuppressionFrames = 0u;
    }

    if (p_transition.shouldAcquireCursorControl && !m_forcedNoMouseCursorChange)
    {
        m_cursorShapeLease.CaptureIfNeeded(m_window.GetCursorShape());
        NLS_SERVICE(NLS::UI::UIManager).PushCustomCursorControl();
        m_forcedNoMouseCursorChange = true;
    }

    if (p_transition.shouldEnableInfiniteWrap)
    {
        m_window.SetCursorMode(Cursor::ECursorMode::NORMAL);
        m_inputManager.SetPointerCapture(true, this);
    }

    if (p_transition.cursorShape.has_value())
    {
        m_window.SetCursorMode(Cursor::ECursorMode::NORMAL);
        m_window.SetCursorShape(*p_transition.cursorShape);
    }

    if (p_transition.shouldReleaseCursorControl)
    {
        if (const auto restoredCursorShape = m_cursorShapeLease.Release())
            m_window.SetCursorShape(*restoredCursorShape);
    }

    if (p_transition.shouldSuppressInitialMouseDelta)
        SuppressMouseDeltaAfterCursorCapture();

    m_interactionState = p_transition.nextState;
}

void Editor::Core::CameraController::UpdateMouseState(const bool p_sceneInputAllowed)
{
    using Windowing::Inputs::EMouseButton;
    using Windowing::Inputs::EMouseButtonState;

    const bool leftDown = m_inputManager.GetMouseButtonState(EMouseButton::MOUSE_BUTTON_LEFT) == EMouseButtonState::MOUSE_DOWN;

	if (m_inputManager.IsMouseButtonPressed(EMouseButton::MOUSE_BUTTON_LEFT))
    {
        m_leftMousePressed = true;
    }

	if (m_inputManager.IsMouseButtonReleased(EMouseButton::MOUSE_BUTTON_LEFT) || (m_leftMousePressed && !leftDown))
	{
		m_leftMousePressed = false;
		m_firstMouse = true;
	}

    SceneViewCameraInteractionInputSnapshot inputSnapshot;
    inputSnapshot.sceneInputAllowed = p_sceneInputAllowed;
    inputSnapshot.cameraInputBlocked = m_inputBlocked;
    inputSnapshot.windowFocused = m_inputManager.HasFocusedWindow();
    inputSnapshot.wantTextInput = IsEditorTextInputActive();
    const auto fly = Shortcuts::GetEditorInputState(Shortcuts::EditorInputAction::CameraFly);
    const auto pan = Shortcuts::GetEditorInputState(Shortcuts::EditorInputAction::CameraPan);
    const auto orbit = Shortcuts::GetEditorInputState(Shortcuts::EditorInputAction::CameraOrbit);
    inputSnapshot.altDown = orbit.actuated;
    inputSnapshot.middlePressed = (pan.pressed && pan.actuated) || (orbit.pressed && orbit.actuated);
    inputSnapshot.rightPressed = fly.pressed && fly.actuated;
    inputSnapshot.middleDown = pan.actuated || orbit.actuated;
    inputSnapshot.rightDown = fly.actuated;
    if (m_planarNavigation)
    {
        inputSnapshot.altDown = false;
        inputSnapshot.middlePressed |= inputSnapshot.rightPressed;
        inputSnapshot.middleDown |= inputSnapshot.rightDown;
        inputSnapshot.rightPressed = false;
        inputSnapshot.rightDown = false;
    }
    const auto transition = SceneViewCameraInteractionStateMachine::EvaluateTransition(m_interactionState, inputSnapshot);
    ApplyInteractionTransition(transition);
}

void Editor::Core::CameraController::SetPlanarNavigation(const bool enabled)
{
    if (m_planarNavigation == enabled) return;
    ResetMouseInteractionState();
    m_planarNavigation = enabled;
    m_cameraDestinations = {};
    m_targetSpeed = Maths::Vector3::Zero;
    m_currentMovementSpeed = Maths::Vector3::Zero;
}
