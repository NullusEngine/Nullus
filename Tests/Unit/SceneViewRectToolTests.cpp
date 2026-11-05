#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <memory>
#include <vector>
#include "Core/SceneViewRectTool.h"
#include "Core/ServiceLocator.h"
#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "Components/TransformComponent.h"
#include "GameObject.h"
#include "Math/Vector4.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Geometry/Vertex.h"
#include "Rendering/Resources/Mesh.h"
#include "Rendering/Settings/DriverSettings.h"
#include "ImGui/imgui.h"

namespace
{
using namespace NLS;
using namespace NLS::Editor::Core;
constexpr float kPi = 3.14159265358979323846f;
constexpr float kTolerance = 0.0002f;

RectToolView View()
{
    RectToolView view;
    view.viewProjection = Maths::Matrix4::Scaling({0.1f, 0.1f, 0.01f});
    view.cameraPosition = {0, 0, 10};
    view.min = {0, 0}; view.max = {800, 600}; view.orthographic = true;
    return view;
}
RectToolView ViewFrom(const Maths::Vector3& forward, const bool perspective = false)
{
    auto view = View();
    view.cameraForward = Maths::Vector3::Normalize(forward);
    view.cameraPosition = view.cameraForward * -10.0f;
    const auto referenceUp = std::abs(view.cameraForward.y) > 0.99f ? Maths::Vector3::Forward : Maths::Vector3::Up;
    const auto right = Maths::Vector3::Normalize(Maths::Vector3::Cross(view.cameraForward, referenceUp));
    const auto up = Maths::Vector3::Normalize(Maths::Vector3::Cross(right, view.cameraForward));
    const auto camera = view.cameraPosition;
    const auto look = camera + view.cameraForward;
    const auto viewMatrix = Maths::Matrix4::CreateView(camera.x, camera.y, camera.z, look.x, look.y, look.z, up.x, up.y, up.z);
    view.viewProjection = (perspective ? Maths::Matrix4::CreatePerspective(60, 4.0f / 3.0f, 0.1f, 100.0f) :
        Maths::Matrix4::CreateOrthographic(5, 4.0f / 3.0f, 0.1f, 100.0f)) * viewMatrix;
    view.orthographic = !perspective;
    return view;
}
Maths::Vector3 Point(const Maths::Matrix4& matrix, const Maths::Vector3& point)
{
    const auto p = matrix * Maths::Vector4(point.x, point.y, point.z, 1);
    return {p.x, p.y, p.z};
}
Maths::Matrix4 World(const RectToolTarget& target, const RectToolTransform& transform)
{
    return target.parentWorld * Maths::Matrix4::Translation(transform.localPosition) *
        Maths::Quaternion::ToMatrix4(transform.localRotation) * Maths::Matrix4::Scaling(transform.localScale);
}
Maths::Vector2 Screen(const RectToolView& view, const Maths::Vector3& point)
{
    const auto p = view.viewProjection * Maths::Vector4(point.x, point.y, point.z, 1);
    return {view.min.x + (p.x / p.w + 1) * 0.5f * (view.max.x - view.min.x),
        view.min.y + (1 - p.y / p.w) * 0.5f * (view.max.y - view.min.y)};
}
void ExpectNear(const Maths::Vector3& actual, const Maths::Vector3& expected)
{
    EXPECT_NEAR(actual.x, expected.x, kTolerance);
    EXPECT_NEAR(actual.y, expected.y, kTolerance);
    EXPECT_NEAR(actual.z, expected.z, kTolerance);
}
std::optional<RectToolTransform> Rotate(RectToolDrag& drag, const float degrees, const RectToolSnap& snap = {})
{
    const float radians = degrees * kPi / 180.0f;
    const auto& frame = drag.frame;
    return UpdateRectToolDrag(drag, Screen(frame.view, frame.pivotWorld +
        frame.rotationBasisU * (2 * std::cos(radians)) + frame.rotationBasisV * (2 * std::sin(radians))), snap);
}
std::optional<RectToolDrag> RotationDrag(const RectToolFrame& frame, const float degrees = 0)
{
    const float radians = degrees * kPi / 180.0f;
    return BeginRectToolDrag(frame, RectToolHandle::RotateTopRight, Screen(frame.view, frame.pivotWorld +
        frame.rotationBasisU * (2 * std::cos(radians)) + frame.rotationBasisV * (2 * std::sin(radians))));
}
}

TEST(SceneViewRectToolTests, EmptyGameObjectUsesCenteredUnitRectangle)
{
    Engine::GameObject object("Rect empty");
    object.GetTransform()->SetLocalPosition({1, 2, 3});
    const auto target = CaptureRectToolTarget(object);
    ExpectNear(target.boundsMin, {-0.5f, -0.5f, -0.5f});
    ExpectNear(target.boundsMax, {0.5f, 0.5f, 0.5f});
    ExpectNear(target.transform.localPosition, {1, 2, 3});
    EXPECT_TRUE(BuildRectToolFrame(target, View(), SceneViewGizmoPivot::Pivot));
}

TEST(SceneViewRectToolTests, ChildMeshBoundsAreCollectedInRootLocalSpace)
{
    static auto driver = std::make_unique<Render::Context::Driver>([]
    {
        Render::Settings::DriverSettings settings;
        settings.graphicsBackend = Render::Settings::EGraphicsBackend::NONE;
        settings.enableExplicitRHI = false;
        return settings;
    }());
    driver->ShutdownThreadedRendering();
    NLS::Core::ServiceLocator::Provide(*driver);
    std::vector<Render::Geometry::Vertex> vertices(2);
    vertices[0].position[0] = vertices[0].position[1] = vertices[0].position[2] = -1;
    vertices[1].position[0] = vertices[1].position[1] = vertices[1].position[2] = 1;
    Render::Resources::Mesh mesh(vertices, std::vector<uint32_t> {0, 1}, 0);
    Engine::GameObject root("Rect root"), child("Rect child");
    root.GetTransform()->SetLocalTransform({7, 8, 9}, Maths::Quaternion(Maths::Vector3 {20, 30, 40}), {-2, 3, 4});
    child.SetParent(root);
    child.GetTransform()->SetLocalPosition({3, 0, 0});
    child.AddComponent<Engine::Components::MeshRenderer>();
    child.AddComponent<Engine::Components::MeshFilter>()->SetMesh(&mesh);
    const auto target = CaptureRectToolTarget(root);
    ExpectNear(target.boundsMin, {2, -1, -1});
    ExpectNear(target.boundsMax, {4, 1, 1});
    child.DetachFromParent();
}

TEST(SceneViewRectToolTests, ChoosesCameraFacingObjectPlaneAndLocksItDuringDrag)
{
    for (const bool perspective : {false, true})
        for (const auto& [forward, expected] : std::array {
            std::pair {Maths::Vector3 {0, 0, -1}, RectToolPlane::XY},
            std::pair {Maths::Vector3 {0, -1, 0}, RectToolPlane::XZ},
            std::pair {Maths::Vector3 {-1, 0, 0}, RectToolPlane::YZ}})
        {
            const auto frame = BuildRectToolFrame({}, ViewFrom(forward, perspective), SceneViewGizmoPivot::Pivot);
            ASSERT_TRUE(frame);
            EXPECT_EQ(frame->plane, expected);
        }
    const auto otherView = ViewFrom({-3, -1, -1});
    const auto automatic = BuildRectToolFrame({}, otherView, SceneViewGizmoPivot::Pivot);
    const auto locked = BuildRectToolFrame({}, otherView, SceneViewGizmoPivot::Pivot, RectToolPlane::XY);
    ASSERT_TRUE(automatic); ASSERT_TRUE(locked);
    EXPECT_EQ(automatic->plane, RectToolPlane::YZ);
    EXPECT_EQ(locked->plane, RectToolPlane::XY);
}

TEST(SceneViewRectToolTests, HitPriorityKeepsCornersAndEdgesAheadOfTheInterior)
{
    const auto frame = BuildRectToolFrame({}, View(), SceneViewGizmoPivot::Pivot);
    ASSERT_TRUE(frame);
    EXPECT_EQ(HitTestRectTool(*frame, frame->corners[0]), RectToolHandle::BottomLeft);
    EXPECT_EQ(HitTestRectTool(*frame, (frame->corners[1] + frame->corners[2]) * 0.5f), RectToolHandle::Right);
    EXPECT_EQ(HitTestRectTool(*frame, {400, 300}), RectToolHandle::Move);
    EXPECT_EQ(HitTestRectTool(*frame, {100, 100}), RectToolHandle::None);
    EXPECT_EQ(HitTestRectTool(*frame, {-1, 300}), RectToolHandle::None);
}

TEST(SceneViewRectToolTests, DistantOrthographicProjectionDoesNotHideAVisibleRectangle)
{
    auto view = ViewFrom({0, 0, -1});
    const auto camera = view.cameraPosition, look = camera + view.cameraForward;
    view.viewProjection = Maths::Matrix4::CreateOrthographic(20, 4.0f / 3.0f, 0.1f, 5000) *
        Maths::Matrix4::CreateView(camera.x, camera.y, camera.z, look.x, look.y, look.z, 0, 1, 0);
    view.is2D = true;
    const auto frame = BuildRectToolFrame({}, view, SceneViewGizmoPivot::Pivot);
    ASSERT_TRUE(frame);
    auto drag = BeginRectToolDrag(*frame, RectToolHandle::Move, {400, 300});
    ASSERT_TRUE(drag);
    const auto result = UpdateRectToolDrag(*drag, {415, 300}, {});
    ASSERT_TRUE(result);
    ExpectNear(result->localPosition, {1, 0, 0});
}

TEST(SceneViewRectToolTests, CentimeterParentScaleStillAllowsAVisibleImportedMeshToBeEdited)
{
    RectToolTarget target;
    target.parentWorld = Maths::Matrix4::Scaling({0.01f, 0.01f, 0.01f});
    target.boundsMin = {-50, -50, -50}; target.boundsMax = {50, 50, 50};
    const auto frame = BuildRectToolFrame(target, View(), SceneViewGizmoPivot::Pivot);
    ASSERT_TRUE(frame);
    auto drag = BeginRectToolDrag(*frame, RectToolHandle::Move, {400, 300});
    ASSERT_TRUE(drag);
    const auto result = UpdateRectToolDrag(*drag, {440, 300}, {});
    ASSERT_TRUE(result);
    ExpectNear(Point(World(target, *result), Maths::Vector3::Zero), {1, 0, 0});
}

TEST(SceneViewRectToolTests, RotationZoneIsOutsideCornersAndScalesWithDpi)
{
    const auto frame = BuildRectToolFrame({}, View(), SceneViewGizmoPivot::Pivot);
    ASSERT_TRUE(frame);
    const auto outside = frame->corners[0] + Maths::Vector2 {-8, 8};
    EXPECT_EQ(HitTestRectTool(*frame, outside, 1), RectToolHandle::RotateBottomLeft);
    EXPECT_EQ(HitTestRectTool(*frame, outside, 2), RectToolHandle::BottomLeft);
    EXPECT_EQ(HitTestRectTool(*frame, frame->corners[0] + Maths::Vector2 {-25, 25}), RectToolHandle::None);
}

TEST(SceneViewRectToolTests, MovementUsesStartSnapshotAndContinuesOutsideViewport)
{
    const auto frame = BuildRectToolFrame({}, View(), SceneViewGizmoPivot::Pivot);
    ASSERT_TRUE(frame);
    auto drag = BeginRectToolDrag(*frame, RectToolHandle::Move, {400, 300});
    ASSERT_TRUE(drag);
    auto first = UpdateRectToolDrag(*drag, {440, 270}, {});
    auto second = UpdateRectToolDrag(*drag, {480, 240}, {});
    auto outside = UpdateRectToolDrag(*drag, {1000, 300}, {});
    ASSERT_TRUE(first); ASSERT_TRUE(second); ASSERT_TRUE(outside);
    ExpectNear(first->localPosition, {1, 1, 0});
    ExpectNear(second->localPosition, {2, 2, 0});
    ExpectNear(outside->localPosition, {15, 0, 0});
    ExpectNear(second->localScale, {1, 1, 1});
}

TEST(SceneViewRectToolTests, EveryEdgeScalesOneAxisAndFixesItsOppositeEdge)
{
    const auto frame = BuildRectToolFrame({}, View(), SceneViewGizmoPivot::Pivot);
    ASSERT_TRUE(frame);
    const std::array handles {RectToolHandle::Right, RectToolHandle::Left, RectToolHandle::Top, RectToolHandle::Bottom};
    for (int i = 0; i < 4; ++i)
    {
        SCOPED_TRACE(i);
        const int axis = i < 2 ? 0 : 1;
        const float sign = i % 2 == 0 ? 1.0f : -1.0f;
        Maths::Vector3 start {0, 0, 0.5f}, end;
        start[axis] = 0.5f * sign;
        end = start; end[axis] += 0.5f * sign;
        auto drag = BeginRectToolDrag(*frame, handles[i], Screen(frame->view, start));
        ASSERT_TRUE(drag);
        const auto result = UpdateRectToolDrag(*drag, Screen(frame->view, end), {});
        ASSERT_TRUE(result);
        EXPECT_NEAR(result->localScale[axis], 1.5f, kTolerance);
        EXPECT_FLOAT_EQ(result->localScale[1 - axis], 1);
        EXPECT_FLOAT_EQ(result->localScale.z, 1);
        auto anchor = start; anchor[axis] *= -1;
        ExpectNear(Point(World(frame->target, *result), anchor), anchor);
    }
}

TEST(SceneViewRectToolTests, CornerScalesBothAxesWithTheOppositeCornerFixed)
{
    const auto frame = BuildRectToolFrame({}, View(), SceneViewGizmoPivot::Pivot);
    ASSERT_TRUE(frame);
    auto drag = BeginRectToolDrag(*frame, RectToolHandle::TopRight, frame->corners[2]);
    ASSERT_TRUE(drag);
    const auto result = UpdateRectToolDrag(*drag, Screen(frame->view, {1.5f, 2.5f, 0.5f}), {});
    ASSERT_TRUE(result);
    ExpectNear(result->localScale, {2, 3, 1});
    ExpectNear(Point(World(frame->target, *result), {-0.5f, -0.5f, 0.5f}), {-0.5f, -0.5f, 0.5f});
}

TEST(SceneViewRectToolTests, SignedLocalScaleAndNonuniformRotatedParentKeepTheAnchorFixed)
{
    RectToolTarget target;
    target.parentWorld = Maths::Matrix4::Translation({1, 2, 0}) *
        Maths::Quaternion::ToMatrix4(Maths::Quaternion(Maths::Vector3 {0, 0, 35})) * Maths::Matrix4::Scaling({-2, 0.75f, 1});
    target.transform.localRotation = Maths::Quaternion(Maths::Vector3 {0, 0, 25});
    target.transform.localScale = {-2, 3, -1};
    const auto frame = BuildRectToolFrame(target, View(), SceneViewGizmoPivot::Pivot);
    ASSERT_TRUE(frame);
    const auto start = (frame->corners[1] + frame->corners[2]) * 0.5f;
    const auto end = Screen(frame->view, Point(frame->world, {1, 0, frame->localCorners[0].z}));
    auto drag = BeginRectToolDrag(*frame, RectToolHandle::Right, start);
    ASSERT_TRUE(drag);
    const auto result = UpdateRectToolDrag(*drag, end, {});
    ASSERT_TRUE(result);
    ExpectNear(result->localScale, {-3, 3, -1});
    ExpectNear(Point(World(target, *result), {-0.5f, 0, 0}), Point(frame->world, {-0.5f, 0, 0}));
    EXPECT_NEAR(std::abs(Maths::Quaternion::DotProduct(result->localRotation, target.transform.localRotation)), 1, kTolerance);
}

TEST(SceneViewRectToolTests, ScalingCannotCrossZeroOrChangeTheOriginalSign)
{
    RectToolTarget target; target.transform.localScale = {-1, 1, 1};
    const auto frame = BuildRectToolFrame(target, View(), SceneViewGizmoPivot::Pivot);
    ASSERT_TRUE(frame);
    auto drag = BeginRectToolDrag(*frame, RectToolHandle::Right, (frame->corners[1] + frame->corners[2]) * 0.5f);
    ASSERT_TRUE(drag);
    const auto result = UpdateRectToolDrag(*drag, Screen(frame->view, {5, 0, 0.5f}), {});
    ASSERT_TRUE(result);
    EXPECT_LT(result->localScale.x, 0);
    EXPECT_GT(result->localScale.x, -0.001f);
    EXPECT_FLOAT_EQ(result->localScale.y, 1);
}

TEST(SceneViewRectToolTests, RotationUsesPivotOrCenterWithoutChangingScale)
{
    for (const auto pivot : {SceneViewGizmoPivot::Pivot, SceneViewGizmoPivot::Center})
    {
        RectToolTarget target; target.boundsMin = {1, 2, -0.5f}; target.boundsMax = {3, 4, 0.5f};
        const auto frame = BuildRectToolFrame(target, View(), pivot);
        ASSERT_TRUE(frame);
        auto drag = RotationDrag(*frame);
        ASSERT_TRUE(drag);
        const auto result = Rotate(*drag, 90);
        ASSERT_TRUE(result);
        ExpectNear(result->localRotation * Maths::Vector3::Right, Maths::Vector3::Up);
        ExpectNear(result->localScale, target.transform.localScale);
        const auto anchor = pivot == SceneViewGizmoPivot::Center ? Maths::Vector3 {2, 3, 0} : Maths::Vector3::Zero;
        ExpectNear(Point(World(target, *result), anchor), Point(frame->world, anchor));
    }
}

TEST(SceneViewRectToolTests, RotationUnwrapsAcrossTheAngleBoundary)
{
    const auto frame = BuildRectToolFrame({}, View(), SceneViewGizmoPivot::Pivot);
    ASSERT_TRUE(frame);
    auto drag = RotationDrag(*frame, 179);
    ASSERT_TRUE(drag);
    const auto result = Rotate(*drag, -179);
    ASSERT_TRUE(result);
    EXPECT_NEAR(drag->accumulatedAngle * 180 / kPi, 2, 0.001f);
    ExpectNear(result->localRotation * Maths::Vector3::Right, {std::cos(2 * kPi / 180), std::sin(2 * kPi / 180), 0});
}

TEST(SceneViewRectToolTests, EachSuboperationUsesItsOwnSnapUnit)
{
    const auto frame = BuildRectToolFrame({}, View(), SceneViewGizmoPivot::Pivot);
    ASSERT_TRUE(frame);
    const RectToolSnap snap {true, 1, 15, 0.5f};
    auto move = BeginRectToolDrag(*frame, RectToolHandle::Move, {400, 300});
    ASSERT_TRUE(move);
    const auto moved = UpdateRectToolDrag(*move, Screen(frame->view, {0.6f, 0.4f, 0}), snap);
    ASSERT_TRUE(moved); ExpectNear(moved->localPosition, {1, 0, 0});
    auto scale = BeginRectToolDrag(*frame, RectToolHandle::Right, (frame->corners[1] + frame->corners[2]) * 0.5f);
    ASSERT_TRUE(scale);
    const auto scaled = UpdateRectToolDrag(*scale, Screen(frame->view, {0.76f, 0, 0.5f}), snap);
    ASSERT_TRUE(scaled); ExpectNear(scaled->localScale, {1.5f, 1, 1});
    auto rotate = RotationDrag(*frame);
    ASSERT_TRUE(rotate);
    const auto rotated = Rotate(*rotate, 23, snap);
    ASSERT_TRUE(rotated);
    ExpectNear(rotated->localRotation * Maths::Vector3::Right, {std::cos(kPi / 6), 0.5f, 0});
}

TEST(SceneViewRectToolTests, TwoDLocksWorldDepthAndLocalZScaleEvenWithATiltedParent)
{
    auto view = View(); view.is2D = true;
    RectToolTarget target;
    target.parentRotation = Maths::Quaternion(Maths::Vector3 {12, 20, 30});
    target.parentWorld = Maths::Matrix4::Translation({0, 0, 4}) *
        Maths::Quaternion::ToMatrix4(target.parentRotation) * Maths::Matrix4::Scaling({2, 3, 1});
    target.transform = {{0.2f, 0.3f, 1}, Maths::Quaternion(Maths::Vector3 {10, 10, 25}), {1, 1, -2}};
    const auto frame = BuildRectToolFrame(target, view, SceneViewGizmoPivot::Center);
    ASSERT_TRUE(frame); EXPECT_EQ(frame->plane, RectToolPlane::XY);
    const auto before = Point(frame->world, Maths::Vector3::Zero);
    auto move = BeginRectToolDrag(*frame, RectToolHandle::Move, Screen(view, frame->origin));
    auto scale = BeginRectToolDrag(*frame, RectToolHandle::Right, (frame->corners[1] + frame->corners[2]) * 0.5f);
    auto rotate = RotationDrag(*frame);
    ASSERT_TRUE(move); ASSERT_TRUE(scale); ASSERT_TRUE(rotate);
    const auto moved = UpdateRectToolDrag(*move, Screen(view, frame->origin) + Maths::Vector2 {35, -40}, {});
    const auto scaled = UpdateRectToolDrag(*scale, (frame->corners[1] + frame->corners[2]) * 0.5f + Maths::Vector2 {40, -10}, {});
    const auto rotated = Rotate(*rotate, 90);
    ASSERT_TRUE(moved); ASSERT_TRUE(scaled); ASSERT_TRUE(rotated);
    for (const auto& result : {*moved, *scaled, *rotated})
    {
        EXPECT_NEAR(Point(World(target, result), Maths::Vector3::Zero).z, before.z, kTolerance);
        EXPECT_FLOAT_EQ(result.localScale.z, -2);
    }
    const auto expected = Maths::Quaternion::Inverse(target.parentRotation) *
        Maths::Quaternion(Maths::Vector3 {0, 0, 90}) * target.parentRotation * target.transform.localRotation;
    EXPECT_NEAR(std::abs(Maths::Quaternion::DotProduct(rotated->localRotation, expected)), 1, kTolerance);
}

TEST(SceneViewRectToolTests, ClickingWithoutMovementProducesAnExactlyUnchangedTransform)
{
    RectToolTarget target; target.transform.localRotation = Maths::Quaternion(Maths::Vector3 {0, 0, 37});
    const auto frame = BuildRectToolFrame(target, View(), SceneViewGizmoPivot::Center);
    ASSERT_TRUE(frame);
    for (const auto handle : {RectToolHandle::Move, RectToolHandle::TopRight, RectToolHandle::RotateTopRight})
    {
        const auto mouse = frame->corners[2];
        auto drag = BeginRectToolDrag(*frame, handle, mouse);
        ASSERT_TRUE(drag);
        const auto result = UpdateRectToolDrag(*drag, mouse, {});
        ASSERT_TRUE(result);
        EXPECT_EQ(*result, target.transform);
    }
}

TEST(SceneViewRectToolTests, RejectsSingularParentsDegenerateProjectionAndNonfiniteInput)
{
    RectToolTarget target; target.parentWorld = Maths::Matrix4::Scaling({0, 1, 1});
    EXPECT_FALSE(BuildRectToolFrame(target, View(), SceneViewGizmoPivot::Pivot));
    target = {}; target.transform.localScale = {0, 1, 1};
    EXPECT_FALSE(BuildRectToolFrame(target, View(), SceneViewGizmoPivot::Pivot));
    target = {}; target.boundsMax = target.boundsMin;
    EXPECT_FALSE(BuildRectToolFrame(target, View(), SceneViewGizmoPivot::Pivot));
    auto view = View(); view.max = view.min;
    EXPECT_FALSE(BuildRectToolFrame({}, view, SceneViewGizmoPivot::Pivot));
    const auto frame = BuildRectToolFrame({}, View(), SceneViewGizmoPivot::Pivot);
    ASSERT_TRUE(frame);
    EXPECT_FALSE(BeginRectToolDrag(*frame, RectToolHandle::Move, {std::numeric_limits<float>::quiet_NaN(), 0}));
    target = {}; target.transform.localPosition = {0, 0, 20};
    EXPECT_FALSE(BuildRectToolFrame(target, ViewFrom({0, 0, -1}, true), SceneViewGizmoPivot::Pivot));
}

TEST(SceneViewRectToolTests, OverlayDrawsHandlesAndSuppliesTheRotationCursorWithoutAGpu)
{
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr; io.DisplaySize = {800, 600};
    io.Fonts->AddFontDefault();
    unsigned char* pixels = nullptr; int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    ImGui::NewFrame(); ImGui::Begin("Rect overlay test");
    const auto frame = BuildRectToolFrame({}, View(), SceneViewGizmoPivot::Pivot);
    ASSERT_TRUE(frame);
    auto* list = ImGui::GetWindowDrawList();
    const auto before = list->VtxBuffer.Size;
    DrawRectToolOverlay(*list, *frame, RectToolHandle::Right, RectToolHandle::None, 2);
    EXPECT_GT(list->VtxBuffer.Size, before);
    EXPECT_EQ(ImGui::GetMouseCursor(), ImGuiMouseCursor_ResizeEW);
    DrawRectToolOverlay(*list, *frame, RectToolHandle::RotateTopRight, RectToolHandle::None, 1);
    EXPECT_EQ(ImGui::GetMouseCursor(), ImGuiMouseCursor_None);
    EXPECT_GT(ImGui::GetForegroundDrawList()->VtxBuffer.Size, 0);
    ImGui::End(); ImGui::EndFrame(); ImGui::DestroyContext();
}
