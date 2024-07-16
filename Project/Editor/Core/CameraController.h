#pragma once

#include <queue>

#include <Windowing/Inputs/InputManager.h>
#include <Windowing/Window.h>
#include <Rendering/Entities/Camera.h>

#include "Panels/Hierarchy.h"
#include "Panels/AView.h"

namespace NLS::Editor::Core
{
/**
 * A simple camera controller used to navigate into views
 */
class CameraController
{
public:
    /**
     * Constructor
     * @param p_view
     * @param p_camera
     */
    CameraController(
        Editor::Panels::AView& p_view,
        Render::Entities::Camera& p_camera);

    /**
     * Handle mouse and keyboard inputs
     * @parma p_deltaTime
     */
    void HandleInputs(float p_deltaTime);

    /**
     * Asks the camera to move to the target actor
     * @param p_target
     */
    void MoveToTarget(Engine::GameObject& p_target);

    /**
     * Defines the speed of the camera
     * @param p_speed
     */
    void SetSpeed(float p_speed);

    /**
     * Returns the camera speed
     */
    float GetSpeed() const;

    /**
     * Defines the position of the camera
     * @param p_position
     */
    void SetPosition(const Maths::Vector3& p_position);

    /**
     * Defines the rotation of the camera
     * @param p_rotation
     */
    void SetRotation(const Maths::Quaternion& p_rotation);

    /**
     * Returns the position of the camera
     */
    const Maths::Vector3& GetPosition() const;

    /**
     * Returns the position of the camera
     */
    const Maths::Quaternion& GetRotation() const;

    /**
     * Returns true if the right mouse click is being pressed
     */
    bool IsRightMousePressed() const;

    /**
     * Lock the target actor to the given actor.
     * @note Usefull to force orbital camera or camera focus to target a specific actor
     * @param p_actor
     */
    void LockTargetActor(Engine::GameObject& p_actor);

    /**
     * Removes any locked actor
     */
    void UnlockTargetActor();

private:
    Engine::GameObject* GetTargetActor() const;
    void HandleCameraPanning(const Maths::Vector2& p_mouseOffset, bool p_firstMouse);
    void HandleCameraOrbit(Engine::GameObject& p_target, const Maths::Vector2& p_mouseOffset, bool p_firstMouse);
    void HandleCameraFPSMouse(const Maths::Vector2& p_mouseOffset, bool p_firstMouse);

    void HandleCameraZoom();
    void HandleCameraFPSKeyboard(float p_deltaTime);
    void UpdateMouseState();

private:
    Windowing::Inputs::InputManager& m_inputManager;
    Windowing::Window& m_window;
    Editor::Panels::AView& m_view;
    Render::Entities::Camera& m_camera;

    std::queue<std::tuple<Maths::Vector3, Maths::Quaternion>> m_cameraDestinations;

    bool m_leftMousePressed = false;
    bool m_middleMousePressed = false;
    bool m_rightMousePressed = false;

    Maths::Vector3 m_targetSpeed;
    Maths::Vector3 m_currentMovementSpeed;

    Maths::Transform* m_orbitTarget = nullptr;
    Maths::Vector3 m_orbitStartOffset;
    bool m_firstMouse = true;
    double m_lastMousePosX = 0.0;
    double m_lastMousePosY = 0.0;
    Maths::Vector3 m_ypr;
    float m_mouseSensitivity = 0.12f;
    float m_cameraDragSpeed = 0.03f;
    float m_cameraOrbitSpeed = 0.5f;
    float m_cameraMoveSpeed = 15.0f;
    float m_focusDistance = 15.0f;
    float m_focusLerpCoefficient = 8.0f;

    Engine::GameObject* m_lockedActor = nullptr;
};
} // namespace NLS::Editor::Core