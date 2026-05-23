
#include "Core/EditorActions.h"

#include "ImGui/imgui.h"

#include "Core/CameraController.h"
#include "Core/SceneCameraFocus.h"
#include "Shortcuts/EditorShortcutService.h"

#include <algorithm>

#include <Components/LightComponent.h>
#include <Components/MeshFilter.h>
#include <Components/TransformComponent.h>
#include "UI/UIManager.h"
#include "ServiceLocator.h"
using namespace NLS;

Editor::Core::CameraController::CameraController(
	Editor::Panels::AView& p_view,
	Render::Entities::Camera& p_camera
) :
	m_inputManager(*EDITOR_CONTEXT(inputManager)),
	m_window(*EDITOR_CONTEXT(window)),
	m_view(p_view),
	m_camera(p_camera)
{
	m_camera.SetFov(60.0f);
}

namespace
{
bool IsKeyDown(
    Windowing::Inputs::InputManager& p_inputManager,
    const Windowing::Inputs::EKey p_key)
{
    return p_inputManager.GetKeyState(p_key) == Windowing::Inputs::EKeyState::KEY_DOWN;
}

bool AreRequiredModifiersDown(
    Windowing::Inputs::InputManager& p_inputManager,
    const Editor::Shortcuts::ShortcutModifiers p_modifiers)
{
    using namespace Editor::Shortcuts;
    using Windowing::Inputs::EKey;

    return (!HasModifier(p_modifiers, EShortcutModifier::Ctrl) ||
            IsKeyDown(p_inputManager, EKey::KEY_LEFT_CONTROL) ||
            IsKeyDown(p_inputManager, EKey::KEY_RIGHT_CONTROL)) &&
        (!HasModifier(p_modifiers, EShortcutModifier::Shift) ||
            IsKeyDown(p_inputManager, EKey::KEY_LEFT_SHIFT) ||
            IsKeyDown(p_inputManager, EKey::KEY_RIGHT_SHIFT)) &&
        (!HasModifier(p_modifiers, EShortcutModifier::Alt) ||
            IsKeyDown(p_inputManager, EKey::KEY_LEFT_ALT) ||
            IsKeyDown(p_inputManager, EKey::KEY_RIGHT_ALT)) &&
        (!HasModifier(p_modifiers, EShortcutModifier::Super) ||
            IsKeyDown(p_inputManager, EKey::KEY_LEFT_SUPER) ||
            IsKeyDown(p_inputManager, EKey::KEY_RIGHT_SUPER));
}

bool IsFlyModeCommandDown(
    Windowing::Inputs::InputManager& p_inputManager,
    const std::string& p_commandId,
    const Windowing::Inputs::EKey p_fallbackKey)
{
    if (NLS::Core::ServiceLocator::Contains<Editor::Shortcuts::EditorShortcutService>())
    {
        const auto& shortcuts = NLS::Core::ServiceLocator::Get<Editor::Shortcuts::EditorShortcutService>();
        const auto binding = shortcuts.GetBinding(p_commandId);
        return binding.IsValid() &&
            IsKeyDown(p_inputManager, binding.primaryKey) &&
            AreRequiredModifiersDown(p_inputManager, binding.modifiers);
    }

    return IsKeyDown(p_inputManager, p_fallbackKey);
}
}

float GetActorFocusDist(Engine::GameObject& p_gameObject)
{
	float distance = 4.0f;

	if (p_gameObject.IsActive())
	{
		if (auto modelRenderer = p_gameObject.GetComponent<Engine::Components::MeshRenderer>())
		{
            const bool hasCustomBoundingSphere = modelRenderer->GetFrustumBehaviour() == Engine::Components::MeshRenderer::EFrustumBehaviour::CULL_CUSTOM;
			auto* meshFilter = p_gameObject.GetComponent<Engine::Components::MeshFilter>();
			const auto* mesh = meshFilter != nullptr ? meshFilter->ResolveMesh() : nullptr;
			const auto boundingSphere = hasCustomBoundingSphere ? &modelRenderer->GetCustomBoundingSphere() : mesh != nullptr ? &mesh->GetBoundingSphere() : nullptr;
			const auto& actorPosition = p_gameObject.GetTransform()->GetWorldPosition();
            const auto& actorScale = p_gameObject.GetTransform()->GetWorldScale();
			const auto scaleFactor = std::max(std::max(actorScale.x, actorScale.y), actorScale.z);

			distance = std::max(distance, boundingSphere ? (boundingSphere->radius + Maths::Vector3::Length(boundingSphere->position)) * scaleFactor * 2.0f : 10.0f);
		}

		for (auto child : p_gameObject.GetChildren())
			distance = std::max(distance, GetActorFocusDist(*child));
	}

	return distance;
}

void Editor::Core::CameraController::HandleInputs(float p_deltaTime)
{
    if (m_inputBlocked)
    {
        ResetMouseInteractionState();
        return;
    }

    if (!m_window.IsFocused())
    {
        ResetMouseInteractionState();
    }

    const Maths::Vector2 mousePosition = m_inputManager.GetMousePosition();
    const bool mouseOverView = m_view.IsMouseWithinView(mousePosition);
    const bool inputActive = mouseOverView || m_inputActive;
    const bool capturingMouse = m_leftMousePressed || m_middleMousePressed || m_rightMousePressed;
    const bool shouldProcessMouseState = inputActive || capturingMouse;

	if (shouldProcessMouseState)
	{
		UpdateMouseState();

		if (mouseOverView && !NLS_SERVICE(NLS::UI::UIManager).IsAnyItemActive())
		{
			if (auto target = GetTargetGameObject())
			{
                auto targetPos = target->GetTransform()->GetWorldPosition();

				float dist = GetActorFocusDist(*target);

				if (m_inputManager.IsKeyPressed(Windowing::Inputs::EKey::KEY_F))
				{
                    MoveToTarget(*target);
				}

				auto focusObjectFromAngle = [this, &targetPos, &dist]( const Maths::Vector3& offset)
				{
					auto camPos = targetPos + offset * dist;
					auto direction = Maths::Vector3::Normalize(targetPos - camPos);
					m_camera.SetRotation(Maths::Quaternion::LookAt(direction, abs(direction.y) == 1.0f ? Maths::Vector3::Right : Maths::Vector3::Up));
					m_cameraDestinations.push({ camPos, m_camera.GetRotation() });
                    if (m_focusState != nullptr)
                    {
                        *m_focusState = {
                            targetPos,
                            dist,
                            true
                        };
                    }
				};

				if (m_inputManager.IsKeyPressed(Windowing::Inputs::EKey::KEY_UP))			focusObjectFromAngle(Maths::Vector3::Up);
				if (m_inputManager.IsKeyPressed(Windowing::Inputs::EKey::KEY_DOWN))		focusObjectFromAngle(-Maths::Vector3::Up);
				if (m_inputManager.IsKeyPressed(Windowing::Inputs::EKey::KEY_RIGHT))		focusObjectFromAngle(Maths::Vector3::Right);
				if (m_inputManager.IsKeyPressed(Windowing::Inputs::EKey::KEY_LEFT))		focusObjectFromAngle(-Maths::Vector3::Right);
				if (m_inputManager.IsKeyPressed(Windowing::Inputs::EKey::KEY_PAGE_UP))	focusObjectFromAngle(Maths::Vector3::Forward);
				if (m_inputManager.IsKeyPressed(Windowing::Inputs::EKey::KEY_PAGE_DOWN))	focusObjectFromAngle(-Maths::Vector3::Forward);
			}
		}
	}

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
		if (m_rightMousePressed || m_middleMousePressed)
		{
            const Maths::Vector2 wrapCompensation = m_inputManager.PollInfiniteCursorWrap();
			auto pos = m_inputManager.GetMousePosition();

            if (!ConsumeSuppressedMouseDelta(pos))
            {
                bool wasFirstMouse = m_firstMouse;

                if (m_firstMouse)
                {
                    ResetLastMousePosition(pos);
                    m_firstMouse = false;
                }

                Maths::Vector2 mouseOffset
                {
                    static_cast<float>(pos.x - m_lastMousePosX - wrapCompensation.x),
                    static_cast<float>(m_lastMousePosY - pos.y + wrapCompensation.y)
                };
                const float maxMouseDeltaPerFrame = m_rightMousePressed
                    ? GetMaxLookMouseDeltaPerFrame()
                    : GetMaxPanOrbitMouseDeltaPerFrame();
                mouseOffset = ClampMouseDeltaForCameraControl(mouseOffset, maxMouseDeltaPerFrame);

                ResetLastMousePosition(pos);

                if (m_rightMousePressed)
                {
                    HandleCameraFPSMouse(mouseOffset, wasFirstMouse);
                }
                else
                {
                    if (m_middleMousePressed)
                    {
                        if (m_inputManager.GetKeyState(Windowing::Inputs::EKey::KEY_LEFT_ALT) == Windowing::Inputs::EKeyState::KEY_DOWN)
                        {
                            if (auto target = GetTargetGameObject())
                            {
                                HandleCameraOrbit(*target, mouseOffset, wasFirstMouse);
                            }
                        }
                        else
                        {
                            HandleCameraPanning(mouseOffset, wasFirstMouse);
                        }
                    }
                }
            }
		}

		if (inputActive)
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
    m_middleMousePressed = false;
    m_rightMousePressed = false;
    m_firstMouse = true;
    m_pendingMouseDeltaSuppressionFrames = 0u;
    if (m_forcedNoMouseCursorChange)
    {
        NLS_SERVICE(NLS::UI::UIManager).PopCustomCursorControl();
        m_forcedNoMouseCursorChange = false;
    }
    m_window.SetInfiniteCursorWrapEnabled(false);
    m_window.SetCursorMode(Cursor::ECursorMode::NORMAL);
    m_window.SetCursorShape(Cursor::ECursorShape::ARROW);
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
    return true;
}

void Editor::Core::CameraController::SetSpeed(float p_speed)
{
	m_cameraMoveSpeed = p_speed;
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
	return m_rightMousePressed;
}

bool Editor::Core::CameraController::IsMiddleMousePressed() const
{
    return m_middleMousePressed;
}

bool Editor::Core::CameraController::IsCameraControlActive() const
{
    return m_middleMousePressed || m_rightMousePressed;
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
    m_inputBlocked = p_inputBlocked;
    if (m_inputBlocked)
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
	else if (EDITOR_EXEC(IsAnyGameObjectSelected()))
	{
		return EDITOR_EXEC(GetSelectedGameObject());
	}

	return nullptr;
}

void Editor::Core::CameraController::HandleCameraPanning(const Maths::Vector2& p_mouseOffset, bool p_firstMouset)
{
    Maths::Vector3 panDelta;
    if (m_focusState != nullptr && m_focusState->hasFocus)
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
	Engine::GameObject& p_target,
	const Maths::Vector2& p_mouseOffset,
	bool p_firstMouse
)
{
	auto mouseOffset = p_mouseOffset * m_cameraOrbitSpeed;

	if (p_firstMouse)
	{
		m_ypr = Maths::Quaternion::EulerAngles(m_camera.GetRotation());
		m_ypr = RemoveRoll(m_ypr);
		m_orbitTarget = &p_target.GetTransform()->GetTransform();
		m_orbitStartOffset = -Maths::Vector3::Forward * Maths::Vector3::Distance(m_orbitTarget->GetWorldPosition(), m_camera.GetPosition());
	}

	m_ypr.y += -mouseOffset.x;
	m_ypr.x += -mouseOffset.y;
	m_ypr.x = std::max(std::min(m_ypr.x, 90.0f), -90.0f);

	auto& target = p_target.GetTransform()->GetTransform();
	Maths::Transform pivotTransform(target.GetWorldPosition());
	Maths::Transform cameraTransform(m_orbitStartOffset);
	cameraTransform.SetParent(pivotTransform);
	pivotTransform.RotateLocal(Maths::Quaternion(m_ypr));
	m_camera.SetPosition(cameraTransform.GetWorldPosition());
	m_camera.SetRotation(cameraTransform.GetWorldRotation());
    if (m_focusState != nullptr)
    {
        *m_focusState = {
            target.GetWorldPosition(),
            Maths::Vector3::Distance(target.GetWorldPosition(), m_camera.GetPosition()),
            true
        };
    }
}

void Editor::Core::CameraController::HandleCameraZoom()
{
    const float mouseWheel = m_inputManager.GetWheelMovement().y;
    if (mouseWheel == 0.0f)
        return;

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

	if (m_rightMousePressed)
	{
        using Windowing::Inputs::EKey;

		bool run = m_inputManager.GetKeyState(EKey::KEY_LEFT_SHIFT) == Windowing::Inputs::EKeyState::KEY_DOWN;
		float velocity = m_cameraMoveSpeed * p_deltaTime * (run ? 2.0f : 1.0f);

		if (IsFlyModeCommandDown(m_inputManager, "scene-view.fly-forward", EKey::KEY_W))
			m_targetSpeed += m_camera.transform->GetWorldForward() * velocity;
		if (IsFlyModeCommandDown(m_inputManager, "scene-view.fly-backward", EKey::KEY_S))
			m_targetSpeed += m_camera.transform->GetWorldForward() * -velocity;
		if (IsFlyModeCommandDown(m_inputManager, "scene-view.fly-left", EKey::KEY_A))
			m_targetSpeed += m_camera.transform->GetWorldRight() * velocity;
		if (IsFlyModeCommandDown(m_inputManager, "scene-view.fly-right", EKey::KEY_D))
			m_targetSpeed += m_camera.transform->GetWorldRight() * -velocity;
		if (IsFlyModeCommandDown(m_inputManager, "scene-view.fly-up", EKey::KEY_E))
			m_targetSpeed += {0.0f, velocity, 0.0f};
		if (IsFlyModeCommandDown(m_inputManager, "scene-view.fly-down", EKey::KEY_Q))
			m_targetSpeed += {0.0f, -velocity, 0.0f};
	}

	m_currentMovementSpeed = Maths::Vector3::Lerp(m_currentMovementSpeed, m_targetSpeed, 10.0f * p_deltaTime);
	m_camera.SetPosition(m_camera.GetPosition() + m_currentMovementSpeed);
    if (m_focusState != nullptr && Maths::Vector3::Length(m_currentMovementSpeed) > 0.0f)
        *m_focusState = UpdateSceneCameraFocusAfterPan(*m_focusState, m_currentMovementSpeed);
}

void Editor::Core::CameraController::UpdateMouseState()
{
    using Windowing::Inputs::EMouseButton;
    using Windowing::Inputs::EMouseButtonState;

    const bool hadCameraCursorInteraction = m_middleMousePressed || m_rightMousePressed;
    const bool leftDown = m_inputManager.GetMouseButtonState(EMouseButton::MOUSE_BUTTON_LEFT) == EMouseButtonState::MOUSE_DOWN;
    const bool middleDown = m_inputManager.GetMouseButtonState(EMouseButton::MOUSE_BUTTON_MIDDLE) == EMouseButtonState::MOUSE_DOWN;
    const bool rightDown = m_inputManager.GetMouseButtonState(EMouseButton::MOUSE_BUTTON_RIGHT) == EMouseButtonState::MOUSE_DOWN;

	if (m_inputManager.IsMouseButtonPressed(EMouseButton::MOUSE_BUTTON_LEFT))
    {
        m_leftMousePressed = true;
    }

	if (m_inputManager.IsMouseButtonReleased(EMouseButton::MOUSE_BUTTON_LEFT) || (m_leftMousePressed && !leftDown))
	{
		m_leftMousePressed = false;
		m_firstMouse = true;
	}

	if (m_inputManager.IsMouseButtonPressed(EMouseButton::MOUSE_BUTTON_MIDDLE))
    {
        m_middleMousePressed = true;
        m_window.SetInfiniteCursorWrapEnabled(true);
        if (!m_forcedNoMouseCursorChange)
        {
            NLS_SERVICE(NLS::UI::UIManager).PushCustomCursorControl();
            m_forcedNoMouseCursorChange = true;
        }
        SuppressMouseDeltaAfterCursorCapture();
    }

	if (m_inputManager.IsMouseButtonReleased(EMouseButton::MOUSE_BUTTON_MIDDLE) || (m_middleMousePressed && !middleDown))
	{
		m_middleMousePressed = false;
		m_firstMouse = true;
        m_pendingMouseDeltaSuppressionFrames = 0u;
        if (!m_rightMousePressed)
        {
		    m_window.SetCursorMode(Cursor::ECursorMode::NORMAL);
        }
        if (!m_rightMousePressed)
            m_window.SetInfiniteCursorWrapEnabled(false);
	}

	if (m_inputManager.IsMouseButtonPressed(EMouseButton::MOUSE_BUTTON_RIGHT))
	{
        m_rightMousePressed = true;
        m_window.SetInfiniteCursorWrapEnabled(true);
        if (!m_forcedNoMouseCursorChange)
        {
            NLS_SERVICE(NLS::UI::UIManager).PushCustomCursorControl();
            m_forcedNoMouseCursorChange = true;
        }
        SuppressMouseDeltaAfterCursorCapture();
    }

	if (m_inputManager.IsMouseButtonReleased(EMouseButton::MOUSE_BUTTON_RIGHT) || (m_rightMousePressed && !rightDown))
	{
		m_rightMousePressed = false;
		m_firstMouse = true;
        m_pendingMouseDeltaSuppressionFrames = 0u;
        if (!m_middleMousePressed)
        {
		    m_window.SetCursorMode(Cursor::ECursorMode::NORMAL);
        }
        if (!m_middleMousePressed)
            m_window.SetInfiniteCursorWrapEnabled(false);
	}

    if (m_rightMousePressed)
    {
        m_window.SetCursorMode(Cursor::ECursorMode::NORMAL);
        m_window.SetCursorShape(Cursor::ECursorShape::FPS_VIEW);
    }
    else if (m_middleMousePressed)
    {
        m_window.SetCursorMode(Cursor::ECursorMode::NORMAL);
        m_window.SetCursorShape(
            m_inputManager.GetKeyState(Windowing::Inputs::EKey::KEY_LEFT_ALT) == Windowing::Inputs::EKeyState::KEY_DOWN
                ? Cursor::ECursorShape::ORBIT_VIEW
                : Cursor::ECursorShape::PAN_VIEW);
    }
    else if (hadCameraCursorInteraction)
    {
        if (m_forcedNoMouseCursorChange)
        {
            NLS_SERVICE(NLS::UI::UIManager).PopCustomCursorControl();
            m_forcedNoMouseCursorChange = false;
        }
        m_window.SetCursorMode(Cursor::ECursorMode::NORMAL);
        m_window.SetCursorShape(Cursor::ECursorShape::ARROW);
    }
}
