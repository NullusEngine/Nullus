#include <gtest/gtest.h>

#include "Core/CameraController.h"

namespace
{
using NLS::Editor::Core::CameraController;
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
