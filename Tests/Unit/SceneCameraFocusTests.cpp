#include <gtest/gtest.h>

#include "Core/SceneCameraFocus.h"
#include "Math/Quaternion.h"

using namespace NLS;

namespace
{
constexpr float kTolerance = 0.0001f;
}

TEST(SceneCameraFocusTests, InitializesFocusFromCameraForward)
{
    const auto focus = Editor::Core::EnsureSceneCameraFocus(
        {},
        {1.0f, 2.0f, 3.0f},
        Maths::Vector3::Forward,
        20.0f);

    EXPECT_TRUE(focus.hasFocus);
    EXPECT_NEAR(focus.focusDistance, 20.0f, kTolerance);
    EXPECT_NEAR(focus.focusPoint.x, 1.0f, kTolerance);
    EXPECT_NEAR(focus.focusPoint.y, 2.0f, kTolerance);
    EXPECT_NEAR(focus.focusPoint.z, 23.0f, kTolerance);
}

TEST(SceneCameraFocusTests, PreservesExistingFocus)
{
    const Editor::Core::SceneCameraFocusState existing {
        {3.0f, 4.0f, 5.0f},
        12.0f,
        true
    };

    const auto focus = Editor::Core::EnsureSceneCameraFocus(
        existing,
        Maths::Vector3::Zero,
        Maths::Vector3::Forward,
        20.0f);

    EXPECT_TRUE(focus.hasFocus);
    EXPECT_NEAR(focus.focusDistance, 12.0f, kTolerance);
    EXPECT_NEAR(focus.focusPoint.x, 3.0f, kTolerance);
    EXPECT_NEAR(focus.focusPoint.y, 4.0f, kTolerance);
    EXPECT_NEAR(focus.focusPoint.z, 5.0f, kTolerance);
}

TEST(SceneCameraFocusTests, ProjectsFocusAfterCameraRotation)
{
    const Editor::Core::SceneCameraFocusState focus {
        {0.0f, 0.0f, 10.0f},
        10.0f,
        true
    };

    const auto updated = Editor::Core::UpdateSceneCameraFocusAfterRotation(
        focus,
        Maths::Vector3::Zero,
        Maths::Vector3::Right);

    EXPECT_NEAR(updated.focusPoint.x, 10.0f, kTolerance);
    EXPECT_NEAR(updated.focusPoint.y, 0.0f, kTolerance);
    EXPECT_NEAR(updated.focusPoint.z, 0.0f, kTolerance);
    EXPECT_NEAR(updated.focusDistance, 10.0f, kTolerance);
}

TEST(SceneCameraFocusTests, PanMovesCameraFocusByWorldDelta)
{
    const Editor::Core::SceneCameraFocusState focus {
        {0.0f, 0.0f, 10.0f},
        10.0f,
        true
    };

    const auto updated = Editor::Core::UpdateSceneCameraFocusAfterPan(
        focus,
        {2.0f, -3.0f, 4.0f});

    EXPECT_NEAR(updated.focusPoint.x, 2.0f, kTolerance);
    EXPECT_NEAR(updated.focusPoint.y, -3.0f, kTolerance);
    EXPECT_NEAR(updated.focusPoint.z, 14.0f, kTolerance);
    EXPECT_NEAR(updated.focusDistance, 10.0f, kTolerance);
}

TEST(SceneCameraFocusTests, ZoomKeepsFocusPointAndUpdatesDistance)
{
    const Editor::Core::SceneCameraFocusState focus {
        {0.0f, 0.0f, 10.0f},
        10.0f,
        true
    };

    const auto updated = Editor::Core::UpdateSceneCameraFocusAfterZoom(
        focus,
        {0.0f, 0.0f, 4.0f});

    EXPECT_NEAR(updated.focusPoint.z, 10.0f, kTolerance);
    EXPECT_NEAR(updated.focusDistance, 6.0f, kTolerance);
}

TEST(SceneCameraFocusTests, PanDeltaScalesWithFocusDistance)
{
    const Maths::Quaternion rotation = Maths::Quaternion::LookAt(Maths::Vector3::Forward, Maths::Vector3::Up);
    const Maths::Vector2 mouseDelta {10.0f, -5.0f};

    const auto nearDelta = Editor::Core::GetSceneCameraPanDelta(rotation, mouseDelta, 10.0f, 60.0f, 100.0f);
    const auto farDelta = Editor::Core::GetSceneCameraPanDelta(rotation, mouseDelta, 20.0f, 60.0f, 100.0f);

    EXPECT_NEAR(farDelta.x, nearDelta.x * 2.0f, 0.001f);
    EXPECT_NEAR(farDelta.y, nearDelta.y * 2.0f, 0.001f);
    EXPECT_NEAR(farDelta.z, nearDelta.z * 2.0f, 0.001f);
}

TEST(SceneCameraFocusTests, PanDeltaUsesCameraRightAndUp)
{
    const Maths::Quaternion rotation = Maths::Quaternion::LookAt(Maths::Vector3::Forward, Maths::Vector3::Up);

    const auto delta = Editor::Core::GetSceneCameraPanDelta(rotation, {10.0f, 5.0f}, 10.0f, 60.0f, 100.0f);

    EXPECT_GT(delta.x, 0.0f);
    EXPECT_LT(delta.y, 0.0f);
    EXPECT_NEAR(delta.z, 0.0f, kTolerance);
}

TEST(SceneCameraFocusTests, ZoomDeltaScalesWithFocusDistance)
{
    const auto nearDelta = Editor::Core::GetSceneCameraZoomDelta(Maths::Vector3::Forward, 1.0f, 10.0f);
    const auto farDelta = Editor::Core::GetSceneCameraZoomDelta(Maths::Vector3::Forward, 1.0f, 20.0f);

    EXPECT_NEAR(farDelta.z, nearDelta.z * 2.0f, 0.001f);
}

TEST(SceneCameraFocusTests, ZoomDeltaDoesNotCrossFocus)
{
    const auto delta = Editor::Core::GetSceneCameraZoomDelta(Maths::Vector3::Forward, 100.0f, 10.0f);

    EXPECT_LT(delta.z, 10.0f);
}
