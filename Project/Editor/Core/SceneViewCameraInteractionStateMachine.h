#pragma once

#include <optional>

#include "Windowing/Cursor/ECursorShape.h"

namespace NLS::Editor::Core
{
enum class SceneViewCameraInteractionStateId
{
    Neutral,
    Blocked,
    Fly,
    Pan,
    Orbit,
    Count
};

struct SceneViewCameraInteractionInputSnapshot
{
    bool sceneInputAllowed = false;
    bool cameraInputBlocked = false;
    bool windowFocused = true;
    bool wantTextInput = false;
    bool altDown = false;
    bool middlePressed = false;
    bool rightPressed = false;
    bool middleDown = false;
    bool rightDown = false;
};

struct SceneViewCameraInteractionTransition
{
    SceneViewCameraInteractionStateId previousState = SceneViewCameraInteractionStateId::Neutral;
    SceneViewCameraInteractionStateId nextState = SceneViewCameraInteractionStateId::Neutral;
    bool stateChanged = false;
    bool shouldAcquireCursorControl = false;
    bool shouldReleaseCursorControl = false;
    bool shouldEnableInfiniteWrap = false;
    bool shouldDisableInfiniteWrap = false;
    bool shouldSuppressInitialMouseDelta = false;
    bool shouldResetTransientMouseState = false;
    std::optional<NLS::Cursor::ECursorShape> cursorShape;
};

class SceneViewCameraCursorShapeLease
{
public:
    void CaptureIfNeeded(NLS::Cursor::ECursorShape p_cursorShape)
    {
        if (!m_capturedCursorShape.has_value())
            m_capturedCursorShape = p_cursorShape;
    }

    std::optional<NLS::Cursor::ECursorShape> Release()
    {
        auto capturedCursorShape = m_capturedCursorShape;
        m_capturedCursorShape.reset();
        return capturedCursorShape;
    }

private:
    std::optional<NLS::Cursor::ECursorShape> m_capturedCursorShape;
};

class SceneViewCameraInteractionStateMachine
{
public:
    static SceneViewCameraInteractionTransition EvaluateTransition(
        SceneViewCameraInteractionStateId p_currentState,
        const SceneViewCameraInteractionInputSnapshot& p_input);

    static bool IsNavigationState(SceneViewCameraInteractionStateId p_state);
};
} // namespace NLS::Editor::Core
