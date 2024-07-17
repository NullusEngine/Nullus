
#include "Core/EditorActions.h"

#include "ImGui/imgui.h"

#include "Core/CameraController.h"

#include <Components/LightComponent.h>
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

float GetActorFocusDist(Engine::GameObject& p_actor)
{
	float distance = 4.0f;

	if (p_actor.IsActive())
	{
		if (auto modelRenderer = p_actor.GetComponent<Engine::Components::MeshRenderer>())
		{
            const bool hasCustomBoundingSphere = modelRenderer->GetFrustumBehaviour() == Engine::Components::MeshRenderer::EFrustumBehaviour::CULL_CUSTOM;
			const bool hasModel = modelRenderer->GetModel();
			const auto boundingSphere = hasCustomBoundingSphere ? &modelRenderer->GetCustomBoundingSphere() : hasModel ? &modelRenderer->GetModel()->GetBoundingSphere() : nullptr;
			const auto& actorPosition = p_actor.GetTransform()->GetWorldPosition();
            const auto& actorScale = p_actor.GetTransform()->GetWorldScale();
			const auto scaleFactor = std::max(std::max(actorScale.x, actorScale.y), actorScale.z);

			distance = std::max(distance, boundingSphere ? (boundingSphere->radius + Maths::Vector3::Length(boundingSphere->position)) * scaleFactor * 2.0f : 10.0f);
		}

		for (auto child : p_actor.GetChildren())
			distance = std::max(distance, GetActorFocusDist(*child));
	}

	return distance;
}

void Editor::Core::CameraController::HandleInputs(float p_deltaTime)
{
	if (m_view.IsHovered())
	{
		UpdateMouseState();

		if (!NLS_SERVICE(NLS::UI::UIManager).IsAnyItemActive())
		{
			if (auto target = GetTargetActor())
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
		if (m_rightMousePressed || m_middleMousePressed || m_leftMousePressed)
		{
			auto pos = m_inputManager.GetMousePosition();

			bool wasFirstMouse = m_firstMouse;

			if (m_firstMouse)
			{
				m_lastMousePosX = pos.x;
				m_lastMousePosY = pos.y;
				m_firstMouse = false;
			}

			Maths::Vector2 mouseOffset
			{
                static_cast<float>(pos.x - m_lastMousePosX),
                static_cast<float>(m_lastMousePosY - pos.y)
			};

			m_lastMousePosX = pos.x;
            m_lastMousePosY = pos.y;

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
						if (auto target = GetTargetActor())
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

		if (m_view.IsHovered())
		{
			HandleCameraZoom();
		}

		HandleCameraFPSKeyboard(p_deltaTime);
	}
}

void Editor::Core::CameraController::MoveToTarget(Engine::GameObject& p_target)
{
	m_cameraDestinations.push({
		p_target.GetTransform()->GetWorldPosition() -
		m_camera.GetRotation() *
		Maths::Vector3::Forward *
		GetActorFocusDist(p_target),
		m_camera.GetRotation()
	});
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

void Editor::Core::CameraController::LockTargetActor(Engine::GameObject& p_actor)
{
	m_lockedActor = &p_actor;
}

void Editor::Core::CameraController::UnlockTargetActor()
{
	m_lockedActor = nullptr;
}

Engine::GameObject* Editor::Core::CameraController::GetTargetActor() const
{
	if (m_lockedActor)
	{
		return m_lockedActor;
	}
	else if (EDITOR_EXEC(IsAnyActorSelected()))
	{
		return EDITOR_EXEC(GetSelectedActor());
	}

	return nullptr;
}

void Editor::Core::CameraController::HandleCameraPanning(const Maths::Vector2& p_mouseOffset, bool p_firstMouset)
{
	m_window.SetCursorShape(Cursor::ECursorShape::HAND);

	auto mouseOffset = p_mouseOffset * m_cameraDragSpeed;

	m_camera.SetPosition(m_camera.GetPosition() + m_camera.transform->GetWorldRight() * mouseOffset.x);
	m_camera.SetPosition(m_camera.GetPosition() - m_camera.transform->GetWorldUp() * mouseOffset.y);
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
}

void Editor::Core::CameraController::HandleCameraZoom()
{
    m_camera.SetPosition(m_camera.GetPosition() + m_camera.transform->GetWorldForward() * NLS_SERVICE(UI::UIManager).GetMouseWheel());
}

void Editor::Core::CameraController::HandleCameraFPSMouse(const Maths::Vector2& p_mouseOffset, bool p_firstMouse)
{
	auto mouseOffset = p_mouseOffset * m_mouseSensitivity;

	if (p_firstMouse)
	{
		m_ypr = Maths::Quaternion::EulerAngles(m_camera.GetRotation());
		m_ypr = RemoveRoll(m_ypr);
	}

	m_ypr.y -= mouseOffset.x;
	m_ypr.x += -mouseOffset.y;
	m_ypr.x = std::max(std::min(m_ypr.x, 90.0f), -90.0f);

	m_camera.SetRotation(Maths::Quaternion(m_ypr));
}

void Editor::Core::CameraController::HandleCameraFPSKeyboard(float p_deltaTime)
{
	m_targetSpeed = Maths::Vector3(0.f, 0.f, 0.f);

	if (m_rightMousePressed)
	{
		bool run = m_inputManager.GetKeyState(Windowing::Inputs::EKey::KEY_LEFT_SHIFT) == Windowing::Inputs::EKeyState::KEY_DOWN;
		float velocity = m_cameraMoveSpeed * p_deltaTime * (run ? 2.0f : 1.0f);

		if (m_inputManager.GetKeyState(Windowing::Inputs::EKey::KEY_W) == Windowing::Inputs::EKeyState::KEY_DOWN)
			m_targetSpeed += m_camera.transform->GetWorldForward() * velocity;
		if (m_inputManager.GetKeyState(Windowing::Inputs::EKey::KEY_S) == Windowing::Inputs::EKeyState::KEY_DOWN)
			m_targetSpeed += m_camera.transform->GetWorldForward() * -velocity;
		if (m_inputManager.GetKeyState(Windowing::Inputs::EKey::KEY_A) == Windowing::Inputs::EKeyState::KEY_DOWN)
			m_targetSpeed += m_camera.transform->GetWorldRight() * velocity;
		if (m_inputManager.GetKeyState(Windowing::Inputs::EKey::KEY_D) == Windowing::Inputs::EKeyState::KEY_DOWN)
			m_targetSpeed += m_camera.transform->GetWorldRight() * -velocity;
		if (m_inputManager.GetKeyState(Windowing::Inputs::EKey::KEY_E) == Windowing::Inputs::EKeyState::KEY_DOWN)
			m_targetSpeed += {0.0f, velocity, 0.0f};
		if (m_inputManager.GetKeyState(Windowing::Inputs::EKey::KEY_Q) == Windowing::Inputs::EKeyState::KEY_DOWN)
			m_targetSpeed += {0.0f, -velocity, 0.0f};
	}

	m_currentMovementSpeed = Maths::Vector3::Lerp(m_currentMovementSpeed, m_targetSpeed, 10.0f * p_deltaTime);
	m_camera.SetPosition(m_camera.GetPosition() + m_currentMovementSpeed);
}

void Editor::Core::CameraController::UpdateMouseState()
{
	if (m_inputManager.IsMouseButtonPressed(Windowing::Inputs::EMouseButton::MOUSE_BUTTON_LEFT))
		m_leftMousePressed = true;

	if (m_inputManager.IsMouseButtonReleased(Windowing::Inputs::EMouseButton::MOUSE_BUTTON_LEFT))
	{
		m_leftMousePressed = false;
		m_firstMouse = true;
	}

	if (m_inputManager.IsMouseButtonPressed(Windowing::Inputs::EMouseButton::MOUSE_BUTTON_MIDDLE))
		m_middleMousePressed = true;

	if (m_inputManager.IsMouseButtonReleased(Windowing::Inputs::EMouseButton::MOUSE_BUTTON_MIDDLE))
	{
		m_middleMousePressed = false;
		m_firstMouse = true;
	}

	if (m_inputManager.IsMouseButtonPressed(Windowing::Inputs::EMouseButton::MOUSE_BUTTON_RIGHT))
	{
		m_rightMousePressed = true;
		m_window.SetCursorMode(Cursor::ECursorMode::DISABLED);
	}

	if (m_inputManager.IsMouseButtonReleased(Windowing::Inputs::EMouseButton::MOUSE_BUTTON_RIGHT))
	{
		m_rightMousePressed = false;
		m_firstMouse = true;
		m_window.SetCursorMode(Cursor::ECursorMode::NORMAL);
	}
}
