#include <gtest/gtest.h>

#include "Core/SceneViewCameraInteractionStateMachine.h"

namespace
{
using NLS::Cursor::ECursorShape;
using NLS::Editor::Core::SceneViewCameraCursorShapeLease;
using NLS::Editor::Core::SceneViewCameraInteractionInputSnapshot;
using NLS::Editor::Core::SceneViewCameraInteractionStateId;
using NLS::Editor::Core::SceneViewCameraInteractionStateMachine;
}

TEST(SceneViewCameraInteractionStateMachineTests, NeutralTransitionsToFlyOnRightMousePress)
{
    SceneViewCameraInteractionInputSnapshot input;
    input.sceneInputAllowed = true;
    input.windowFocused = true;
    input.rightPressed = true;
    input.rightDown = true;

    const auto transition = SceneViewCameraInteractionStateMachine::EvaluateTransition(
        SceneViewCameraInteractionStateId::Neutral,
        input);

    EXPECT_EQ(transition.previousState, SceneViewCameraInteractionStateId::Neutral);
    EXPECT_EQ(transition.nextState, SceneViewCameraInteractionStateId::Fly);
    EXPECT_TRUE(transition.stateChanged);
    EXPECT_TRUE(transition.shouldAcquireCursorControl);
    EXPECT_TRUE(transition.shouldEnableInfiniteWrap);
    EXPECT_TRUE(transition.shouldSuppressInitialMouseDelta);
    ASSERT_TRUE(transition.cursorShape.has_value());
    EXPECT_EQ(*transition.cursorShape, ECursorShape::FPS_VIEW);
}

TEST(SceneViewCameraInteractionStateMachineTests, NeutralTransitionsToPanOrOrbitFromMiddleMousePress)
{
    SceneViewCameraInteractionInputSnapshot panInput;
    panInput.sceneInputAllowed = true;
    panInput.windowFocused = true;
    panInput.middlePressed = true;
    panInput.middleDown = true;
    const auto panTransition = SceneViewCameraInteractionStateMachine::EvaluateTransition(
        SceneViewCameraInteractionStateId::Neutral,
        panInput);
    EXPECT_EQ(panTransition.nextState, SceneViewCameraInteractionStateId::Pan);
    ASSERT_TRUE(panTransition.cursorShape.has_value());
    EXPECT_EQ(*panTransition.cursorShape, ECursorShape::PAN_VIEW);

    SceneViewCameraInteractionInputSnapshot orbitInput;
    orbitInput.sceneInputAllowed = true;
    orbitInput.windowFocused = true;
    orbitInput.altDown = true;
    orbitInput.middlePressed = true;
    orbitInput.middleDown = true;
    const auto orbitTransition = SceneViewCameraInteractionStateMachine::EvaluateTransition(
        SceneViewCameraInteractionStateId::Neutral,
        orbitInput);
    EXPECT_EQ(orbitTransition.nextState, SceneViewCameraInteractionStateId::Orbit);
    ASSERT_TRUE(orbitTransition.cursorShape.has_value());
    EXPECT_EQ(*orbitTransition.cursorShape, ECursorShape::ORBIT_VIEW);
}

TEST(SceneViewCameraInteractionStateMachineTests, ActiveNavigationStateDoesNotReapplyCursorWhileUnchanged)
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
    EXPECT_FALSE(transition.shouldReleaseCursorControl);
    EXPECT_FALSE(transition.cursorShape.has_value());
}

TEST(SceneViewCameraInteractionStateMachineTests, NavigationStateTransitionsToBlockedWhenTextInputStarts)
{
    SceneViewCameraInteractionInputSnapshot input;
    input.sceneInputAllowed = true;
    input.windowFocused = true;
    input.wantTextInput = true;
    input.middleDown = true;

    const auto transition = SceneViewCameraInteractionStateMachine::EvaluateTransition(
        SceneViewCameraInteractionStateId::Pan,
        input);

    EXPECT_EQ(transition.nextState, SceneViewCameraInteractionStateId::Blocked);
    EXPECT_TRUE(transition.stateChanged);
    EXPECT_TRUE(transition.shouldReleaseCursorControl);
    EXPECT_TRUE(transition.shouldDisableInfiniteWrap);
    EXPECT_TRUE(transition.shouldResetTransientMouseState);
    EXPECT_FALSE(transition.cursorShape.has_value());
}

TEST(SceneViewCameraInteractionStateMachineTests, MiddleMouseNavigationSwitchesBetweenPanAndOrbitWithoutReacquire)
{
    SceneViewCameraInteractionInputSnapshot orbitInput;
    orbitInput.windowFocused = true;
    orbitInput.altDown = true;
    orbitInput.middleDown = true;

    const auto orbitTransition = SceneViewCameraInteractionStateMachine::EvaluateTransition(
        SceneViewCameraInteractionStateId::Pan,
        orbitInput);

    EXPECT_EQ(orbitTransition.nextState, SceneViewCameraInteractionStateId::Orbit);
    EXPECT_TRUE(orbitTransition.stateChanged);
    EXPECT_FALSE(orbitTransition.shouldAcquireCursorControl);
    EXPECT_FALSE(orbitTransition.shouldReleaseCursorControl);
    ASSERT_TRUE(orbitTransition.cursorShape.has_value());
    EXPECT_EQ(*orbitTransition.cursorShape, ECursorShape::ORBIT_VIEW);

    SceneViewCameraInteractionInputSnapshot panInput;
    panInput.windowFocused = true;
    panInput.middleDown = true;

    const auto panTransition = SceneViewCameraInteractionStateMachine::EvaluateTransition(
        SceneViewCameraInteractionStateId::Orbit,
        panInput);

    EXPECT_EQ(panTransition.nextState, SceneViewCameraInteractionStateId::Pan);
    EXPECT_TRUE(panTransition.stateChanged);
    EXPECT_FALSE(panTransition.shouldAcquireCursorControl);
    EXPECT_FALSE(panTransition.shouldReleaseCursorControl);
    ASSERT_TRUE(panTransition.cursorShape.has_value());
    EXPECT_EQ(*panTransition.cursorShape, ECursorShape::PAN_VIEW);
}

TEST(SceneViewCameraInteractionStateMachineTests, NavigationStateReleasesToNeutralWhenButtonOwnershipEnds)
{
    SceneViewCameraInteractionInputSnapshot input;
    input.windowFocused = true;

    const auto transition = SceneViewCameraInteractionStateMachine::EvaluateTransition(
        SceneViewCameraInteractionStateId::Pan,
        input);

    EXPECT_EQ(transition.nextState, SceneViewCameraInteractionStateId::Neutral);
    EXPECT_TRUE(transition.stateChanged);
    EXPECT_TRUE(transition.shouldReleaseCursorControl);
    EXPECT_TRUE(transition.shouldDisableInfiniteWrap);
    EXPECT_TRUE(transition.shouldResetTransientMouseState);
}

TEST(SceneViewCameraInteractionStateMachineTests, CursorShapeLeaseRestoresShapeCapturedBeforeCameraNavigation)
{
    SceneViewCameraCursorShapeLease lease;

    EXPECT_FALSE(lease.Release().has_value());

    lease.CaptureIfNeeded(ECursorShape::IBEAM);
    lease.CaptureIfNeeded(ECursorShape::FPS_VIEW);

    const auto restoredShape = lease.Release();

    ASSERT_TRUE(restoredShape.has_value());
    EXPECT_EQ(*restoredShape, ECursorShape::IBEAM);
    EXPECT_FALSE(lease.Release().has_value());
}
