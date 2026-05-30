#include "Core/SceneViewCameraInteractionStateMachine.h"

namespace NLS::Editor::Core
{
static_assert(static_cast<int>(SceneViewCameraInteractionStateId::Count) == 5,
    "Update SceneViewCameraInteractionStateMachine transitions when adding camera interaction states.");

namespace
{
SceneViewCameraInteractionTransition MakeTransition(
    const SceneViewCameraInteractionStateId p_previousState,
    const SceneViewCameraInteractionStateId p_nextState)
{
    SceneViewCameraInteractionTransition transition;
    transition.previousState = p_previousState;
    transition.nextState = p_nextState;
    transition.stateChanged = p_previousState != p_nextState;
    return transition;
}

SceneViewCameraInteractionTransition MakeActiveEntryTransition(
    const SceneViewCameraInteractionStateId p_previousState,
    const SceneViewCameraInteractionStateId p_nextState,
    const NLS::Cursor::ECursorShape p_cursorShape)
{
    auto transition = MakeTransition(p_previousState, p_nextState);
    if (transition.stateChanged)
    {
        transition.shouldAcquireCursorControl = true;
        transition.shouldEnableInfiniteWrap = true;
        transition.shouldSuppressInitialMouseDelta = true;
        transition.cursorShape = p_cursorShape;
    }
    return transition;
}

SceneViewCameraInteractionTransition MakeNavigationStateSwitchTransition(
    const SceneViewCameraInteractionStateId p_previousState,
    const SceneViewCameraInteractionStateId p_nextState,
    const NLS::Cursor::ECursorShape p_cursorShape)
{
    auto transition = MakeTransition(p_previousState, p_nextState);
    if (transition.stateChanged)
        transition.cursorShape = p_cursorShape;
    return transition;
}

SceneViewCameraInteractionTransition MakeNavigationExitTransition(
    const SceneViewCameraInteractionStateId p_previousState,
    const SceneViewCameraInteractionStateId p_nextState)
{
    auto transition = MakeTransition(p_previousState, p_nextState);
    if (SceneViewCameraInteractionStateMachine::IsNavigationState(p_previousState) && transition.stateChanged)
    {
        transition.shouldReleaseCursorControl = true;
        transition.shouldDisableInfiniteWrap = true;
        transition.shouldResetTransientMouseState = true;
    }
    return transition;
}
}

SceneViewCameraInteractionTransition SceneViewCameraInteractionStateMachine::EvaluateTransition(
    const SceneViewCameraInteractionStateId p_currentState,
    const SceneViewCameraInteractionInputSnapshot& p_input)
{
    if (!p_input.windowFocused)
        return MakeNavigationExitTransition(p_currentState, SceneViewCameraInteractionStateId::Neutral);

    if (p_input.cameraInputBlocked || p_input.wantTextInput)
    {
        auto transition = MakeTransition(p_currentState, SceneViewCameraInteractionStateId::Blocked);
        if (p_currentState != SceneViewCameraInteractionStateId::Blocked)
        {
            transition.shouldReleaseCursorControl = SceneViewCameraInteractionStateMachine::IsNavigationState(p_currentState);
            transition.shouldDisableInfiniteWrap = SceneViewCameraInteractionStateMachine::IsNavigationState(p_currentState);
            transition.shouldResetTransientMouseState = true;
        }
        return transition;
    }

    switch (p_currentState)
    {
    case SceneViewCameraInteractionStateId::Blocked:
        return MakeTransition(p_currentState, SceneViewCameraInteractionStateId::Neutral);

    case SceneViewCameraInteractionStateId::Fly:
        if (!p_input.rightDown)
            return MakeNavigationExitTransition(p_currentState, SceneViewCameraInteractionStateId::Neutral);
        return MakeTransition(p_currentState, SceneViewCameraInteractionStateId::Fly);

    case SceneViewCameraInteractionStateId::Pan:
        if (!p_input.middleDown)
            return MakeNavigationExitTransition(p_currentState, SceneViewCameraInteractionStateId::Neutral);
        if (p_input.altDown)
            return MakeNavigationStateSwitchTransition(
                p_currentState,
                SceneViewCameraInteractionStateId::Orbit,
                NLS::Cursor::ECursorShape::ORBIT_VIEW);
        return MakeTransition(p_currentState, SceneViewCameraInteractionStateId::Pan);

    case SceneViewCameraInteractionStateId::Orbit:
        if (!p_input.middleDown)
            return MakeNavigationExitTransition(p_currentState, SceneViewCameraInteractionStateId::Neutral);
        if (!p_input.altDown)
            return MakeNavigationStateSwitchTransition(
                p_currentState,
                SceneViewCameraInteractionStateId::Pan,
                NLS::Cursor::ECursorShape::PAN_VIEW);
        return MakeTransition(p_currentState, SceneViewCameraInteractionStateId::Orbit);

    case SceneViewCameraInteractionStateId::Neutral:
        break;

    case SceneViewCameraInteractionStateId::Count:
        return MakeTransition(p_currentState, SceneViewCameraInteractionStateId::Neutral);
    }

    if (!p_input.sceneInputAllowed)
        return MakeTransition(p_currentState, SceneViewCameraInteractionStateId::Neutral);

    if (p_input.rightPressed)
    {
        return MakeActiveEntryTransition(
            p_currentState,
            SceneViewCameraInteractionStateId::Fly,
            NLS::Cursor::ECursorShape::FPS_VIEW);
    }

    if (p_input.middlePressed)
    {
        return MakeActiveEntryTransition(
            p_currentState,
            p_input.altDown ? SceneViewCameraInteractionStateId::Orbit : SceneViewCameraInteractionStateId::Pan,
            p_input.altDown ? NLS::Cursor::ECursorShape::ORBIT_VIEW : NLS::Cursor::ECursorShape::PAN_VIEW);
    }

    return MakeTransition(p_currentState, SceneViewCameraInteractionStateId::Neutral);
}

bool SceneViewCameraInteractionStateMachine::IsNavigationState(const SceneViewCameraInteractionStateId p_state)
{
    return p_state == SceneViewCameraInteractionStateId::Fly ||
        p_state == SceneViewCameraInteractionStateId::Pan ||
        p_state == SceneViewCameraInteractionStateId::Orbit;
}
} // namespace NLS::Editor::Core
