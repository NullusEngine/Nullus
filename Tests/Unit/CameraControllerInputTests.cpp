#include <gtest/gtest.h>

#include "Core/CameraController.h"
#include "Core/SceneViewCameraInteractionStateMachine.h"

namespace
{
using NLS::Editor::Core::CameraController;
using NLS::Editor::Core::SceneViewCameraInteractionInputSnapshot;
using NLS::Editor::Core::SceneViewCameraInteractionStateId;
using NLS::Editor::Core::SceneViewCameraInteractionStateMachine;
using NLS::Maths::Vector2;
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
