#include <gtest/gtest.h>

#include "Core/GizmoOperation.h"
#include "Core/SceneViewImGuizmo.h"
#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "Core/ServiceLocator.h"
#include "GameObject.h"
#include "Math/Matrix4.h"
#include "Math/Quaternion.h"
#include "Panels/EditorTopBar.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Geometry/Vertex.h"
#include "Rendering/Resources/Mesh.h"
#include "Rendering/Settings/DriverSettings.h"
#include "Rendering/Settings/EGraphicsBackend.h"
#include "Settings/EditorSettings.h"

#include <memory>
#include <vector>

using namespace NLS;

namespace
{
constexpr float kTolerance = 0.0001f;

NLS::Render::Context::Driver& EnsureImGuizmoMeshTestDriver()
{
    static auto driver = std::make_unique<NLS::Render::Context::Driver>([]()
    {
        NLS::Render::Settings::DriverSettings settings;
        settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
        settings.enableExplicitRHI = false;
        return settings;
    }());
    NLS::Core::ServiceLocator::Provide(*driver);
    return *driver;
}

NLS::Render::Geometry::Vertex BoundsVertexAt(const float x, const float y, const float z)
{
    NLS::Render::Geometry::Vertex vertex {};
    vertex.position[0] = x;
    vertex.position[1] = y;
    vertex.position[2] = z;
    return vertex;
}

std::unique_ptr<NLS::Render::Resources::Mesh> CreateCenteredUnitBoundsMesh()
{
    EnsureImGuizmoMeshTestDriver();
    return std::make_unique<NLS::Render::Resources::Mesh>(
        std::vector<NLS::Render::Geometry::Vertex> {
            BoundsVertexAt(-1.0f, -1.0f, -1.0f),
            BoundsVertexAt(1.0f, 1.0f, 1.0f)
        },
        std::vector<uint32_t> {0u, 1u},
        0u);
}

void AttachCenteredRenderableBounds(Engine::GameObject& actor, NLS::Render::Resources::Mesh& mesh)
{
    auto* meshFilter = actor.AddComponent<Engine::Components::MeshFilter>();
    auto* meshRenderer = actor.AddComponent<Engine::Components::MeshRenderer>();
    ASSERT_NE(meshFilter, nullptr);
    ASSERT_NE(meshRenderer, nullptr);
    meshFilter->SetMesh(&mesh);
}
}

TEST(ImGuizmoTransformAdapterTests, MapsEditorOperationsToImGuizmoOperations)
{
    EXPECT_EQ(
        Editor::Core::ToImGuizmoOperation(Editor::Core::EGizmoOperation::TRANSLATE),
        Editor::Core::SceneViewGizmoOperation::Translate);
    EXPECT_EQ(
        Editor::Core::ToImGuizmoOperation(Editor::Core::EGizmoOperation::ROTATE),
        Editor::Core::SceneViewGizmoOperation::Rotate);
    EXPECT_EQ(
        Editor::Core::ToImGuizmoOperation(Editor::Core::EGizmoOperation::SCALE),
        Editor::Core::SceneViewGizmoOperation::Scale);
}

TEST(ImGuizmoTransformAdapterTests, OperationMappingKeepsDistinctModes)
{
    EXPECT_NE(
        Editor::Core::ToImGuizmoOperation(Editor::Core::EGizmoOperation::TRANSLATE),
        Editor::Core::ToImGuizmoOperation(Editor::Core::EGizmoOperation::ROTATE));
    EXPECT_NE(
        Editor::Core::ToImGuizmoOperation(Editor::Core::EGizmoOperation::ROTATE),
        Editor::Core::ToImGuizmoOperation(Editor::Core::EGizmoOperation::SCALE));
    EXPECT_NE(
        Editor::Core::ToImGuizmoOperation(Editor::Core::EGizmoOperation::TRANSLATE),
        Editor::Core::ToImGuizmoOperation(Editor::Core::EGizmoOperation::SCALE));
}

TEST(ImGuizmoTransformAdapterTests, TogglesPivotAndSpaceModes)
{
    EXPECT_EQ(
        Editor::Core::ToggleGizmoPivot(Editor::Core::SceneViewGizmoPivot::Pivot),
        Editor::Core::SceneViewGizmoPivot::Center);
    EXPECT_EQ(
        Editor::Core::ToggleGizmoPivot(Editor::Core::SceneViewGizmoPivot::Center),
        Editor::Core::SceneViewGizmoPivot::Pivot);
    EXPECT_EQ(
        Editor::Core::ToggleGizmoSpace(Editor::Core::SceneViewGizmoSpace::Global),
        Editor::Core::SceneViewGizmoSpace::Local);
    EXPECT_EQ(
        Editor::Core::ToggleGizmoSpace(Editor::Core::SceneViewGizmoSpace::Local),
        Editor::Core::SceneViewGizmoSpace::Global);
}

TEST(ImGuizmoTransformAdapterTests, MapsPivotModesToToolbarIcons)
{
    EXPECT_STREQ(
        Editor::Panels::GetToolbarPivotIconId(Editor::Core::SceneViewGizmoPivot::Pivot),
        "Toolbar_Pivot");
    EXPECT_STREQ(
        Editor::Panels::GetToolbarPivotIconId(Editor::Core::SceneViewGizmoPivot::Center),
        "Toolbar_Center");
}

TEST(ImGuizmoTransformAdapterTests, SelectsSnapValueForCurrentOperation)
{
    auto& sceneTools = Editor::Settings::EditorSettings::GetSceneToolSettingsObject();
    sceneTools.translationSnapUnit = 2.0f;
    sceneTools.rotationSnapUnit = 30.0f;
    sceneTools.scalingSnapUnit = 0.5f;

    EXPECT_NEAR(
        Editor::Core::GetSnapValue(Editor::Core::EGizmoOperation::TRANSLATE),
        2.0f,
        kTolerance);
    EXPECT_NEAR(
        Editor::Core::GetSnapValue(Editor::Core::EGizmoOperation::ROTATE),
        30.0f,
        kTolerance);
    EXPECT_NEAR(
        Editor::Core::GetSnapValue(Editor::Core::EGizmoOperation::SCALE),
        0.5f,
        kTolerance);
}

TEST(ImGuizmoTransformAdapterTests, CopiesMathMatrixToAndFromGizmoMatrix)
{
    const Maths::Matrix4 source(
        1.0f, 2.0f, 3.0f, 4.0f,
        5.0f, 6.0f, 7.0f, 8.0f,
        9.0f, 10.0f, 11.0f, 12.0f,
        13.0f, 14.0f, 15.0f, 16.0f);

    auto gizmoMatrix = Editor::Core::ToImGuizmoMatrix(source);
    const Maths::Matrix4 roundTrip = Editor::Core::FromImGuizmoMatrix(gizmoMatrix);

    for (int i = 0; i < 16; ++i)
        EXPECT_NEAR(roundTrip.data[i], source.data[i], kTolerance);
}

TEST(ImGuizmoTransformAdapterTests, ConvertsTranslationBetweenMatrixConventions)
{
    const Maths::Matrix4 source = Maths::Matrix4::Translation({4.0f, 5.0f, 6.0f});

    auto gizmoMatrix = Editor::Core::ToImGuizmoMatrix(source);

    EXPECT_NEAR(gizmoMatrix[12], 4.0f, kTolerance);
    EXPECT_NEAR(gizmoMatrix[13], 5.0f, kTolerance);
    EXPECT_NEAR(gizmoMatrix[14], 6.0f, kTolerance);

    gizmoMatrix[12] = 7.0f;
    gizmoMatrix[13] = 8.0f;
    gizmoMatrix[14] = 9.0f;

    const Maths::Matrix4 roundTrip = Editor::Core::FromImGuizmoMatrix(gizmoMatrix);
    EXPECT_NEAR(roundTrip.data[3], 7.0f, kTolerance);
    EXPECT_NEAR(roundTrip.data[7], 8.0f, kTolerance);
    EXPECT_NEAR(roundTrip.data[11], 9.0f, kTolerance);
}

TEST(ImGuizmoTransformAdapterTests, ExtractsCameraTransformFromViewMatrix)
{
    const Maths::Vector3 position {4.0f, 5.0f, 6.0f};
    const Maths::Quaternion rotation = Maths::Quaternion::LookAt(
        Maths::Vector3::Normalize({-1.0f, 0.25f, 0.5f}),
        Maths::Vector3::Up);
    const Maths::Vector3 forward = rotation * Maths::Vector3::Forward;
    const Maths::Vector3 up = rotation * Maths::Vector3::Up;
    const Maths::Matrix4 view = Maths::Matrix4::CreateView(
        position.x,
        position.y,
        position.z,
        position.x + forward.x,
        position.y + forward.y,
        position.z + forward.z,
        up.x,
        up.y,
        up.z);

    const auto transform = Editor::Core::GetCameraTransformFromViewMatrix(
        Editor::Core::ToImGuizmoMatrix(view));

    EXPECT_NEAR(transform.position.x, position.x, kTolerance);
    EXPECT_NEAR(transform.position.y, position.y, kTolerance);
    EXPECT_NEAR(transform.position.z, position.z, kTolerance);
    EXPECT_NEAR(
        std::fabs(Maths::Quaternion::DotProduct(transform.rotation, rotation)),
        1.0f,
        0.001f);
}

TEST(ImGuizmoTransformAdapterTests, ExtractsCameraTransformWhenLookingAtViewGizmoPoles)
{
    const Maths::Vector3 position {4.0f, 5.0f, 6.0f};
    const std::array<Maths::Vector3, 2> forwards {
        Maths::Vector3::Up,
        -Maths::Vector3::Up
    };

    for (const auto& forward : forwards)
    {
        const Maths::Quaternion rotation = Maths::Quaternion::LookAt(forward, Maths::Vector3::Forward);
        const Maths::Vector3 up = rotation * Maths::Vector3::Up;
        const Maths::Matrix4 view = Maths::Matrix4::CreateView(
            position.x,
            position.y,
            position.z,
            position.x + forward.x,
            position.y + forward.y,
            position.z + forward.z,
            up.x,
            up.y,
            up.z);

        const auto transform = Editor::Core::GetCameraTransformFromViewMatrix(
            Editor::Core::ToImGuizmoMatrix(view));

        const Maths::Vector3 extractedForward = transform.rotation * Maths::Vector3::Forward;
        const Maths::Vector3 extractedUp = transform.rotation * Maths::Vector3::Up;
        EXPECT_NEAR(Maths::Vector3::Dot(extractedForward, forward), 1.0f, 0.001f);
        EXPECT_NEAR(Maths::Vector3::Dot(extractedUp, up), 1.0f, 0.001f);
    }
}

TEST(ImGuizmoTransformAdapterTests, PreservesCameraRollWhenViewDirectionApproachesPole)
{
    const Maths::Vector3 position {4.0f, 5.0f, 6.0f};
    const Maths::Vector3 forward = Maths::Vector3::Normalize({0.0f, 1.0f, 0.001f});
    const Maths::Vector3 right = Maths::Vector3::Right;
    const Maths::Vector3 up = Maths::Vector3::Normalize(Maths::Vector3::Cross(forward, right));
    const Maths::Matrix4 view = Maths::Matrix4::CreateView(
        position.x,
        position.y,
        position.z,
        position.x + forward.x,
        position.y + forward.y,
        position.z + forward.z,
        up.x,
        up.y,
        up.z);
    const auto transform = Editor::Core::GetCameraTransformFromViewMatrix(
        Editor::Core::ToImGuizmoMatrix(view));

    EXPECT_NEAR(Maths::Vector3::Dot(transform.rotation * Maths::Vector3::Forward, forward), 1.0f, 0.001f);
    EXPECT_NEAR(Maths::Vector3::Dot(transform.rotation * Maths::Vector3::Right, right), 1.0f, 0.001f);
    EXPECT_NEAR(Maths::Vector3::Dot(transform.rotation * Maths::Vector3::Up, up), 1.0f, 0.001f);
}

TEST(ImGuizmoTransformAdapterTests, PlacesViewGizmoAtSceneViewTopRight)
{
    const auto rect = Editor::Core::GetSceneViewViewGizmoRect(
        {10.0f, 20.0f},
        {210.0f, 220.0f});

    EXPECT_NEAR(rect.size.x, 96.0f, kTolerance);
    EXPECT_NEAR(rect.size.y, 96.0f, kTolerance);
    EXPECT_NEAR(rect.position.x, 106.0f, kTolerance);
    EXPECT_NEAR(rect.position.y, 28.0f, kTolerance);
}

TEST(ImGuizmoTransformAdapterTests, AppliesViewGizmoCameraOnlyAfterClickReleaseInterpolation)
{
    EXPECT_FALSE(Editor::Core::ShouldApplyViewGizmoCameraTransform(false, false, {0.0f, 0.0f}));
    EXPECT_FALSE(Editor::Core::ShouldApplyViewGizmoCameraTransform(true, true, {0.0f, 0.0f}));
    EXPECT_FALSE(Editor::Core::ShouldApplyViewGizmoCameraTransform(true, true, {0.25f, 0.0f}));
    EXPECT_FALSE(Editor::Core::ShouldApplyViewGizmoCameraTransform(true, true, {0.0f, -0.25f}));
    EXPECT_TRUE(Editor::Core::ShouldApplyViewGizmoCameraTransform(true, false, {0.0f, 0.0f}));
}

TEST(ImGuizmoTransformAdapterTests, CancelsViewGizmoCameraWhenRightMouseCameraControlStarts)
{
    EXPECT_FALSE(Editor::Core::ShouldCancelViewGizmoCameraTransform(false, false));
    EXPECT_FALSE(Editor::Core::ShouldCancelViewGizmoCameraTransform(true, false));
    EXPECT_FALSE(Editor::Core::ShouldCancelViewGizmoCameraTransform(false, true));
    EXPECT_TRUE(Editor::Core::ShouldCancelViewGizmoCameraTransform(true, true));
}

TEST(ImGuizmoTransformAdapterTests, DetectsOppositeDirectionViewGizmoJump)
{
    const Maths::Quaternion current = Maths::Quaternion::LookAt(Maths::Vector3::Forward, Maths::Vector3::Up);
    const Maths::Quaternion opposite = Maths::Quaternion::LookAt(-Maths::Vector3::Forward, Maths::Vector3::Up);
    const Maths::Quaternion nearby = Maths::Quaternion::LookAt(
        Maths::Vector3::Normalize({0.1f, 0.0f, 1.0f}),
        Maths::Vector3::Up);

    EXPECT_TRUE(Editor::Core::IsViewGizmoOppositeDirectionJump(current, opposite));
    EXPECT_FALSE(Editor::Core::IsViewGizmoOppositeDirectionJump(current, current));
    EXPECT_FALSE(Editor::Core::IsViewGizmoOppositeDirectionJump(current, nearby));
}

TEST(ImGuizmoTransformAdapterTests, StabilizesOppositeViewGizmoClickByYawingAroundWorldUp)
{
    const Editor::Core::SceneViewCameraTransform current {
        {0.0f, 0.0f, 8.0f},
        Maths::Quaternion::LookAt(Maths::Vector3::Forward, Maths::Vector3::Up)
    };
    const Editor::Core::SceneViewCameraTransform candidate {
        {0.0f, 0.0f, -8.0f},
        Maths::Quaternion::LookAt(-Maths::Vector3::Forward, Maths::Vector3::Right)
    };

    const Maths::Vector3 orbitTarget = current.position + (current.rotation * Maths::Vector3::Forward) * 8.0f;
    const auto stabilized = Editor::Core::StabilizeViewGizmoCameraTransform(current, candidate, 8.0f, orbitTarget);

    EXPECT_NEAR(stabilized.position.y, current.position.y, kTolerance);
    EXPECT_NEAR(Maths::Vector3::Distance(stabilized.position, orbitTarget), 8.0f, 0.001f);
    EXPECT_LT(Maths::Vector3::Dot(stabilized.rotation * Maths::Vector3::Forward, Maths::Vector3::Forward), 1.0f);
    EXPECT_GT(Maths::Vector3::Dot(stabilized.rotation * Maths::Vector3::Forward, -Maths::Vector3::Forward), -0.999f);
    EXPECT_NEAR(Maths::Vector3::Dot(stabilized.rotation * Maths::Vector3::Up, Maths::Vector3::Up), 1.0f, 0.001f);
}

TEST(ImGuizmoTransformAdapterTests, StabilizedViewGizmoCameraOrbitsExplicitFocusPoint)
{
    const Editor::Core::SceneViewCameraTransform current {
        {10.0f, 0.0f, 0.0f},
        Maths::Quaternion::LookAt(Maths::Vector3::Right, Maths::Vector3::Up)
    };
    const Editor::Core::SceneViewCameraTransform candidate {
        {0.0f, 0.0f, 10.0f},
        Maths::Quaternion::LookAt(Maths::Vector3::Forward, Maths::Vector3::Up)
    };
    const Maths::Vector3 focus {30.0f, 0.0f, 0.0f};

    const auto stabilized = Editor::Core::StabilizeViewGizmoCameraTransform(current, candidate, 20.0f, focus);
    const Maths::Vector3 forward = stabilized.rotation * Maths::Vector3::Forward;

    EXPECT_NEAR(Maths::Vector3::Distance(stabilized.position, focus), 20.0f, 0.001f);
    EXPECT_NEAR(Maths::Vector3::Dot(Maths::Vector3::Normalize(focus - stabilized.position), forward), 1.0f, 0.001f);
}

TEST(ImGuizmoTransformAdapterTests, KeepsViewGizmoCameraUprightWhenCandidateCrossesPole)
{
    const Editor::Core::SceneViewCameraTransform current {
        {0.0f, 0.0f, 8.0f},
        Maths::Quaternion::LookAt(Maths::Vector3::Normalize({0.0f, 0.97f, 0.243f}), Maths::Vector3::Up)
    };
    const Editor::Core::SceneViewCameraTransform candidate {
        {0.0f, -7.99f, -0.3f},
        Maths::Quaternion::LookAt(Maths::Vector3::Normalize({0.0f, 0.999f, -0.045f}), Maths::Vector3::Right)
    };

    const Maths::Vector3 orbitTarget = current.position + (current.rotation * Maths::Vector3::Forward) * 8.0f;
    const auto stabilized = Editor::Core::StabilizeViewGizmoCameraTransform(current, candidate, 8.0f, orbitTarget);
    const Maths::Vector3 forward = stabilized.rotation * Maths::Vector3::Forward;
    const Maths::Vector3 horizontalForward = Maths::Vector3::Normalize({forward.x, 0.0f, forward.z});
    const Maths::Vector3 currentHorizontalForward = Maths::Vector3::Normalize({0.0f, 0.0f, 0.243f});
    const Maths::Vector3 candidateHorizontalForward = Maths::Vector3::Normalize({0.0f, 0.0f, -0.045f});

    EXPECT_GT(Maths::Vector3::Dot(stabilized.rotation * Maths::Vector3::Up, Maths::Vector3::Up), 0.0f);
    EXPECT_LE(forward.y, 0.986f);
    EXPECT_GT(Maths::Vector3::Dot(horizontalForward, currentHorizontalForward), 0.9f);
    EXPECT_LT(Maths::Vector3::Dot(horizontalForward, candidateHorizontalForward), -0.9f);
    EXPECT_GT(std::abs(horizontalForward.x), 0.01f);
}

TEST(ImGuizmoTransformAdapterTests, RoutesAroundWorldYBeforeOppositeTargetInterpolationReachesPole)
{
    const Editor::Core::SceneViewCameraTransform current {
        {0.0f, 0.0f, 8.0f},
        Maths::Quaternion::LookAt(Maths::Vector3::Forward, Maths::Vector3::Up)
    };
    const Editor::Core::SceneViewCameraTransform candidate {
        {0.0f, -4.0f, 6.928f},
        Maths::Quaternion::LookAt(Maths::Vector3::Normalize({0.0f, 0.5f, 0.866f}), Maths::Vector3::Up)
    };
    const Maths::Vector3 finalTarget = -Maths::Vector3::Forward;

    const auto stabilized = Editor::Core::StabilizeViewGizmoCameraTransform(
        current,
        candidate,
        8.0f,
        current.position + (current.rotation * Maths::Vector3::Forward) * 8.0f,
        &finalTarget);
    const Maths::Vector3 forward = stabilized.rotation * Maths::Vector3::Forward;

    EXPECT_NEAR(forward.y, 0.0f, 0.001f);
    EXPECT_GT(Maths::Vector3::Dot(Maths::Vector3::Normalize({forward.x, 0.0f, forward.z}), Maths::Vector3::Forward), 0.9f);
    EXPECT_GT(std::abs(forward.x), 0.01f);
}

TEST(ImGuizmoTransformAdapterTests, UsesViewGizmoTargetAsCameraForwardDirection)
{
    const Editor::Core::SceneViewCameraTransform current {
        {0.0f, 0.0f, 8.0f},
        Maths::Quaternion::LookAt(Maths::Vector3::Forward, Maths::Vector3::Up)
    };
    const Editor::Core::SceneViewCameraTransform candidate {
        {-1.0f, 0.0f, 7.0f},
        Maths::Quaternion::LookAt(Maths::Vector3::Normalize({0.1f, 0.0f, 0.995f}), Maths::Vector3::Up)
    };
    const Maths::Vector3 finalTargetForward = Maths::Vector3::Forward;

    const auto stabilized = Editor::Core::StabilizeViewGizmoCameraTransform(
        current,
        candidate,
        8.0f,
        current.position + (current.rotation * Maths::Vector3::Forward) * 8.0f,
        &finalTargetForward);
    const Maths::Vector3 forward = stabilized.rotation * Maths::Vector3::Forward;

    EXPECT_NEAR(forward.y, 0.0f, 0.001f);
    EXPECT_GT(Maths::Vector3::Dot(forward, Maths::Vector3::Forward), 0.99f);
    EXPECT_LT(std::abs(forward.x), 0.2f);
}

TEST(ImGuizmoTransformAdapterTests, StartsViewGizmoInterpolationOnReleaseFrameWhenCandidateHasNotMovedYet)
{
    const Editor::Core::SceneViewCameraTransform current {
        {0.0f, 0.0f, 8.0f},
        Maths::Quaternion::LookAt(Maths::Vector3::Forward, Maths::Vector3::Up)
    };
    const Editor::Core::SceneViewCameraTransform candidate = current;
    const Maths::Vector3 finalTargetForward = Maths::Vector3::Right;

    const auto stabilized = Editor::Core::StabilizeViewGizmoCameraTransform(
        current,
        candidate,
        8.0f,
        current.position + (current.rotation * Maths::Vector3::Forward) * 8.0f,
        &finalTargetForward);
    const Maths::Vector3 forward = stabilized.rotation * Maths::Vector3::Forward;

    EXPECT_GT(Maths::Vector3::Dot(forward, Maths::Vector3::Forward), 0.9f);
    EXPECT_GT(forward.x, 0.05f);
}

TEST(ImGuizmoTransformAdapterTests, ConvertsImGuizmoViewTargetDirectionToCameraForward)
{
    const Maths::Vector3 imGuizmoTargetDirection = Maths::Vector3::Forward;

    const Maths::Vector3 cameraForward =
        Editor::Core::GetCameraForwardFromImGuizmoViewTargetDirection(imGuizmoTargetDirection);

    EXPECT_NEAR(cameraForward.x, 0.0f, kTolerance);
    EXPECT_NEAR(cameraForward.y, 0.0f, kTolerance);
    EXPECT_NEAR(cameraForward.z, -1.0f, kTolerance);
}

TEST(ImGuizmoTransformAdapterTests, AllowsViewGizmoCameraToPitchUpAndDownBeforePoleLimit)
{
    const Editor::Core::SceneViewCameraTransform current {
        {0.0f, 0.0f, 8.0f},
        Maths::Quaternion::LookAt(Maths::Vector3::Forward, Maths::Vector3::Up)
    };
    const Editor::Core::SceneViewCameraTransform candidate {
        {0.0f, -4.0f, 6.928f},
        Maths::Quaternion::LookAt(Maths::Vector3::Normalize({0.0f, 0.5f, 0.866f}), Maths::Vector3::Up)
    };

    const auto stabilized = Editor::Core::StabilizeViewGizmoCameraTransform(
        current,
        candidate,
        8.0f,
        current.position + (current.rotation * Maths::Vector3::Forward) * 8.0f);
    const Maths::Vector3 forward = stabilized.rotation * Maths::Vector3::Forward;

    EXPECT_NEAR(forward.y, 0.5f, 0.001f);
    EXPECT_NEAR(Maths::Vector3::Dot(stabilized.rotation * Maths::Vector3::Up, Maths::Vector3::Up), 0.866f, 0.001f);
}

TEST(ImGuizmoTransformAdapterTests, ReadsAndAppliesGameObjectWorldTransform)
{
    Engine::GameObject actor("GizmoTarget");
    actor.GetTransform()->SetWorldPosition({1.0f, 2.0f, 3.0f});
    actor.GetTransform()->SetWorldRotation(Maths::Quaternion(Maths::Vector3(0.0f, 45.0f, 0.0f)));
    actor.GetTransform()->SetWorldScale({2.0f, 3.0f, 4.0f});

    const auto gizmoMatrix = Editor::Core::GetGameObjectWorldGizmoMatrix(actor);
    Editor::Core::ApplyGameObjectWorldGizmoMatrix(actor, gizmoMatrix);

    EXPECT_NEAR(actor.GetTransform()->GetWorldPosition().x, 1.0f, kTolerance);
    EXPECT_NEAR(actor.GetTransform()->GetWorldPosition().y, 2.0f, kTolerance);
    EXPECT_NEAR(actor.GetTransform()->GetWorldPosition().z, 3.0f, kTolerance);
    EXPECT_NEAR(actor.GetTransform()->GetWorldScale().x, 2.0f, kTolerance);
    EXPECT_NEAR(actor.GetTransform()->GetWorldScale().y, 3.0f, kTolerance);
    EXPECT_NEAR(actor.GetTransform()->GetWorldScale().z, 4.0f, kTolerance);
}

TEST(ImGuizmoTransformAdapterTests, CenterPivotFallsBackToGameObjectPivotWhenNoRenderableBoundsExist)
{
    Engine::GameObject actor("GizmoTarget");
    actor.GetTransform()->SetWorldPosition({1.0f, 2.0f, 3.0f});

    const auto pivotPosition = Editor::Core::GetGameObjectGizmoPivotPosition(
        actor,
        Editor::Core::SceneViewGizmoPivot::Pivot);
    const auto centerPosition = Editor::Core::GetGameObjectGizmoPivotPosition(
        actor,
        Editor::Core::SceneViewGizmoPivot::Center);
    const auto centerMatrix = Editor::Core::FromImGuizmoMatrix(Editor::Core::GetGameObjectWorldGizmoMatrix(
        actor,
        Editor::Core::SceneViewGizmoPivot::Center));

    EXPECT_NEAR(pivotPosition.x, 1.0f, kTolerance);
    EXPECT_NEAR(pivotPosition.y, 2.0f, kTolerance);
    EXPECT_NEAR(pivotPosition.z, 3.0f, kTolerance);
    EXPECT_NEAR(centerPosition.x, pivotPosition.x, kTolerance);
    EXPECT_NEAR(centerPosition.y, pivotPosition.y, kTolerance);
    EXPECT_NEAR(centerPosition.z, pivotPosition.z, kTolerance);
    EXPECT_NEAR(centerMatrix.data[3], pivotPosition.x, kTolerance);
    EXPECT_NEAR(centerMatrix.data[7], pivotPosition.y, kTolerance);
    EXPECT_NEAR(centerMatrix.data[11], pivotPosition.z, kTolerance);
}

TEST(ImGuizmoTransformAdapterTests, CenterPivotUsesChildRenderableBounds)
{
    Engine::GameObject parent("Parent");
    Engine::GameObject child("Child");
    parent.GetTransform()->SetWorldPosition({1.0f, 2.0f, 3.0f});
    child.GetTransform()->SetLocalPosition({10.0f, 0.0f, 0.0f});
    child.SetParent(parent);

    auto mesh = CreateCenteredUnitBoundsMesh();
    ASSERT_NE(mesh, nullptr);
    AttachCenteredRenderableBounds(child, *mesh);

    const auto centerPosition = Editor::Core::GetGameObjectGizmoPivotPosition(
        parent,
        Editor::Core::SceneViewGizmoPivot::Center);
    const auto centerMatrix = Editor::Core::FromImGuizmoMatrix(Editor::Core::GetGameObjectWorldGizmoMatrix(
        parent,
        Editor::Core::SceneViewGizmoPivot::Center));

    EXPECT_NEAR(centerPosition.x, 11.0f, kTolerance);
    EXPECT_NEAR(centerPosition.y, 2.0f, kTolerance);
    EXPECT_NEAR(centerPosition.z, 3.0f, kTolerance);
    EXPECT_NEAR(centerMatrix.data[3], centerPosition.x, kTolerance);
    EXPECT_NEAR(centerMatrix.data[7], centerPosition.y, kTolerance);
    EXPECT_NEAR(centerMatrix.data[11], centerPosition.z, kTolerance);

    child.DetachFromParent();
}

TEST(ImGuizmoTransformAdapterTests, CenterPivotTranslationAppliesDeltaToGameObjectPosition)
{
    Engine::GameObject actor("GizmoTarget");
    actor.GetTransform()->SetWorldPosition({1.0f, 2.0f, 3.0f});

    auto gizmoMatrix = Editor::Core::GetGameObjectWorldGizmoMatrix(
        actor,
        Editor::Core::SceneViewGizmoPivot::Center);
    gizmoMatrix[12] += 5.0f;
    gizmoMatrix[13] += 6.0f;
    gizmoMatrix[14] += 7.0f;

    Editor::Core::ApplyGameObjectWorldGizmoMatrix(
        actor,
        gizmoMatrix,
        Editor::Core::EGizmoOperation::TRANSLATE,
        Editor::Core::SceneViewGizmoPivot::Center);

    EXPECT_NEAR(actor.GetTransform()->GetWorldPosition().x, 6.0f, kTolerance);
    EXPECT_NEAR(actor.GetTransform()->GetWorldPosition().y, 8.0f, kTolerance);
    EXPECT_NEAR(actor.GetTransform()->GetWorldPosition().z, 10.0f, kTolerance);
}

TEST(ImGuizmoTransformAdapterTests, CenterPivotRotationKeepsRenderableCenterAtGizmoPosition)
{
    Engine::GameObject parent("Parent");
    Engine::GameObject child("Child");
    parent.GetTransform()->SetWorldPosition({1.0f, 2.0f, 3.0f});
    child.GetTransform()->SetLocalPosition({10.0f, 0.0f, 0.0f});
    child.SetParent(parent);

    auto mesh = CreateCenteredUnitBoundsMesh();
    ASSERT_NE(mesh, nullptr);
    AttachCenteredRenderableBounds(child, *mesh);

    auto gizmoMatrix = Editor::Core::GetGameObjectWorldGizmoMatrix(
        parent,
        Editor::Core::SceneViewGizmoPivot::Center);
    const auto centerBefore = Editor::Core::GetGameObjectGizmoPivotPosition(
        parent,
        Editor::Core::SceneViewGizmoPivot::Center);

    const Maths::Matrix4 rotatedCenterMatrix =
        Maths::Matrix4::Translation(centerBefore) *
        Maths::Quaternion::ToMatrix4(Maths::Quaternion(Maths::Vector3(0.0f, 90.0f, 0.0f)));
    gizmoMatrix = Editor::Core::ToImGuizmoMatrix(rotatedCenterMatrix);

    Editor::Core::ApplyGameObjectWorldGizmoMatrix(
        parent,
        gizmoMatrix,
        Editor::Core::EGizmoOperation::ROTATE,
        Editor::Core::SceneViewGizmoPivot::Center);

    const auto centerAfter = Editor::Core::GetGameObjectGizmoPivotPosition(
        parent,
        Editor::Core::SceneViewGizmoPivot::Center);

    EXPECT_NEAR(centerAfter.x, centerBefore.x, kTolerance);
    EXPECT_NEAR(centerAfter.y, centerBefore.y, kTolerance);
    EXPECT_NEAR(centerAfter.z, centerBefore.z, kTolerance);
    EXPECT_NEAR(parent.GetTransform()->GetWorldPosition().x, 11.0f, kTolerance);
    EXPECT_NEAR(parent.GetTransform()->GetWorldPosition().y, 2.0f, kTolerance);
    EXPECT_NEAR(parent.GetTransform()->GetWorldPosition().z, 13.0f, kTolerance);

    child.DetachFromParent();
}

TEST(ImGuizmoTransformAdapterTests, CenterPivotScaleKeepsRenderableCenterAtGizmoPosition)
{
    Engine::GameObject parent("Parent");
    Engine::GameObject child("Child");
    parent.GetTransform()->SetWorldPosition({1.0f, 2.0f, 3.0f});
    child.GetTransform()->SetLocalPosition({10.0f, 0.0f, 0.0f});
    child.SetParent(parent);

    auto mesh = CreateCenteredUnitBoundsMesh();
    ASSERT_NE(mesh, nullptr);
    AttachCenteredRenderableBounds(child, *mesh);

    const auto centerBefore = Editor::Core::GetGameObjectGizmoPivotPosition(
        parent,
        Editor::Core::SceneViewGizmoPivot::Center);
    const Maths::Matrix4 scaledCenterMatrix =
        Maths::Matrix4::Translation(centerBefore) *
        Maths::Matrix4::Scaling({2.0f, 2.0f, 2.0f});

    Editor::Core::ApplyGameObjectWorldGizmoMatrix(
        parent,
        Editor::Core::ToImGuizmoMatrix(scaledCenterMatrix),
        Editor::Core::EGizmoOperation::SCALE,
        Editor::Core::SceneViewGizmoPivot::Center);

    const auto centerAfter = Editor::Core::GetGameObjectGizmoPivotPosition(
        parent,
        Editor::Core::SceneViewGizmoPivot::Center);

    EXPECT_NEAR(centerAfter.x, centerBefore.x, kTolerance);
    EXPECT_NEAR(centerAfter.y, centerBefore.y, kTolerance);
    EXPECT_NEAR(centerAfter.z, centerBefore.z, kTolerance);
    EXPECT_NEAR(parent.GetTransform()->GetWorldPosition().x, -9.0f, kTolerance);
    EXPECT_NEAR(parent.GetTransform()->GetWorldPosition().y, 2.0f, kTolerance);
    EXPECT_NEAR(parent.GetTransform()->GetWorldPosition().z, 3.0f, kTolerance);
    EXPECT_NEAR(parent.GetTransform()->GetWorldScale().x, 2.0f, kTolerance);
    EXPECT_NEAR(parent.GetTransform()->GetWorldScale().y, 2.0f, kTolerance);
    EXPECT_NEAR(parent.GetTransform()->GetWorldScale().z, 2.0f, kTolerance);

    child.DetachFromParent();
}

TEST(ImGuizmoTransformAdapterTests, AppliesRotationFromGizmoMatrixWithoutChangingPositionOrScale)
{
    Engine::GameObject actor("GizmoTarget");

    const Maths::Vector3 expectedPosition {1.0f, 2.0f, 3.0f};
    const Maths::Vector3 expectedScale {2.0f, 3.0f, 4.0f};
    const Maths::Quaternion expectedRotation(Maths::Vector3(180.0f, 0.0f, 0.0f));
    actor.GetTransform()->SetWorldPosition(expectedPosition);
    actor.GetTransform()->SetWorldScale(expectedScale);

    const Maths::Matrix4 targetMatrix =
        Maths::Matrix4::Translation({10.0f, 20.0f, 30.0f}) *
        Maths::Quaternion::ToMatrix4(expectedRotation) *
        Maths::Matrix4::Scaling({8.0f, 9.0f, 10.0f});

    Editor::Core::ApplyGameObjectWorldGizmoMatrix(
        actor,
        Editor::Core::ToImGuizmoMatrix(targetMatrix),
        Editor::Core::EGizmoOperation::ROTATE);

    EXPECT_NEAR(actor.GetTransform()->GetWorldPosition().x, expectedPosition.x, kTolerance);
    EXPECT_NEAR(actor.GetTransform()->GetWorldPosition().y, expectedPosition.y, kTolerance);
    EXPECT_NEAR(actor.GetTransform()->GetWorldPosition().z, expectedPosition.z, kTolerance);
    EXPECT_NEAR(actor.GetTransform()->GetWorldScale().x, expectedScale.x, kTolerance);
    EXPECT_NEAR(actor.GetTransform()->GetWorldScale().y, expectedScale.y, kTolerance);
    EXPECT_NEAR(actor.GetTransform()->GetWorldScale().z, expectedScale.z, kTolerance);
    EXPECT_NEAR(
        std::fabs(Maths::Quaternion::DotProduct(actor.GetTransform()->GetWorldRotation(), expectedRotation)),
        1.0f,
        0.001f);
}

TEST(ImGuizmoTransformAdapterTests, RotationOperationDoesNotApplyGizmoMatrixScaleOrShear)
{
    Engine::GameObject actor("GizmoTarget");
    actor.GetTransform()->SetWorldPosition({1.0f, 2.0f, 3.0f});
    actor.GetTransform()->SetWorldScale({2.0f, 3.0f, 4.0f});

    const Maths::Quaternion expectedRotation(Maths::Vector3(0.0f, 45.0f, 0.0f));
    Maths::Matrix4 targetMatrix =
        Maths::Matrix4::Translation({10.0f, 20.0f, 30.0f}) *
        Maths::Quaternion::ToMatrix4(expectedRotation) *
        Maths::Matrix4::Scaling({8.0f, 9.0f, 10.0f});
    targetMatrix.data[1] += 0.25f;

    Editor::Core::ApplyGameObjectWorldGizmoMatrix(
        actor,
        Editor::Core::ToImGuizmoMatrix(targetMatrix),
        Editor::Core::EGizmoOperation::ROTATE);

    EXPECT_NEAR(actor.GetTransform()->GetWorldPosition().x, 1.0f, kTolerance);
    EXPECT_NEAR(actor.GetTransform()->GetWorldPosition().y, 2.0f, kTolerance);
    EXPECT_NEAR(actor.GetTransform()->GetWorldPosition().z, 3.0f, kTolerance);
    EXPECT_NEAR(actor.GetTransform()->GetWorldScale().x, 2.0f, kTolerance);
    EXPECT_NEAR(actor.GetTransform()->GetWorldScale().y, 3.0f, kTolerance);
    EXPECT_NEAR(actor.GetTransform()->GetWorldScale().z, 4.0f, kTolerance);
    EXPECT_NEAR(
        std::fabs(Maths::Quaternion::DotProduct(actor.GetTransform()->GetWorldRotation(), expectedRotation)),
        1.0f,
        0.001f);
}

TEST(ImGuizmoTransformAdapterTests, SuppressesScenePickingWhileGizmoIsHoveredOrUsingMouse)
{
    EXPECT_FALSE(Editor::Core::ShouldSuppressScenePicking({false, false}));
    EXPECT_TRUE(Editor::Core::ShouldSuppressScenePicking({true, false}));
    EXPECT_TRUE(Editor::Core::ShouldSuppressScenePicking({false, true}));
    EXPECT_TRUE(Editor::Core::ShouldSuppressScenePicking({true, true}));
    EXPECT_TRUE(Editor::Core::ShouldSuppressScenePicking({false, false, true, false}));
    EXPECT_TRUE(Editor::Core::ShouldSuppressScenePicking({false, false, false, true}));
}

TEST(ImGuizmoTransformAdapterTests, DetectsEitherControlKeyAsSnapModifier)
{
    using Windowing::Inputs::EKeyState;

    EXPECT_FALSE(Editor::Core::IsSnapModifierActive(EKeyState::KEY_UP, EKeyState::KEY_UP));
    EXPECT_TRUE(Editor::Core::IsSnapModifierActive(EKeyState::KEY_DOWN, EKeyState::KEY_UP));
    EXPECT_TRUE(Editor::Core::IsSnapModifierActive(EKeyState::KEY_UP, EKeyState::KEY_DOWN));
    EXPECT_TRUE(Editor::Core::IsSnapModifierActive(EKeyState::KEY_DOWN, EKeyState::KEY_DOWN));
}
