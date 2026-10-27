#include <gtest/gtest.h>

#include <vector>

#include "UI/UIDocumentCanvasView.h"
#include "UI/UIDocumentDesignerModel.h"
#include "UIDocumentDesignerTestSupport.h"

namespace
{
    using NLS::Editor::UIDocumentTools::UIDocumentCanvasGeometry;
    using NLS::Engine::UI::UILayoutParams;
    using NLS::Engine::UI::UIRect;
    using NLS::Engine::UI::UIVector2;
    using Handle = UIDocumentCanvasGeometry::Handle;

    UILayoutParams Layout(const UIVector2 size, const UIVector2 pivot = { 0.0f, 0.0f })
    {
        UILayoutParams layout;
        layout.mode = NLS::Engine::UI::UILayoutMode::Absolute;
        layout.size = size;
        layout.pivot = pivot;
        layout.anchorMin = { 0.0f, 0.0f };
        layout.anchorMax = { 0.0f, 0.0f };
        return layout;
    }
}

TEST(UIDocumentCanvasGeometryTests, ResizeRightAndBottomKeepTopLeftFixed)
{
    const auto layout = Layout({ 100.0f, 50.0f });
    const UIRect rect { 10.0f, 20.0f, 100.0f, 50.0f };

    const auto result = UIDocumentCanvasGeometry::Resize(
        layout,
        rect,
        Handle::BottomRight,
        { 20.0f, 10.0f },
        false);
    EXPECT_TRUE(result.changed);
    EXPECT_FLOAT_EQ(result.size.x, 120.0f);
    EXPECT_FLOAT_EQ(result.size.y, 60.0f);
    EXPECT_FLOAT_EQ(result.offsetMin.x, 0.0f);
    EXPECT_FLOAT_EQ(result.offsetMin.y, 0.0f);
}

TEST(UIDocumentCanvasGeometryTests, ResizeLeftEdgeKeepsRightEdgeFixedForCenterPivot)
{
    UILayoutParams layout = Layout({ 100.0f, 50.0f }, { 0.5f, 0.5f });
    layout.offsetMin = { 20.0f, 10.0f };
    const UIRect rect { 20.0f, 10.0f, 100.0f, 50.0f };

    const auto result = UIDocumentCanvasGeometry::Resize(
        layout,
        rect,
        Handle::Left,
        { 10.0f, 0.0f },
        false);
    EXPECT_FLOAT_EQ(result.size.x, 90.0f);
    // right edge fixed: offsetMin += (old - new) * (1 - pivot)
    EXPECT_FLOAT_EQ(result.offsetMin.x, 25.0f);
    EXPECT_FLOAT_EQ(result.size.y, 50.0f);
}

TEST(UIDocumentCanvasGeometryTests, StretchedAxisMovesTheOffsetEdgeInsteadOfSize)
{
    UILayoutParams layout = Layout({ 0.0f, 50.0f });
    layout.anchorMax = { 1.0f, 0.0f };
    layout.offsetMin = { 0.0f, 0.0f };
    layout.offsetMax = { 0.0f, 0.0f };
    const UIRect rect { 0.0f, 0.0f, 100.0f, 50.0f };

    const auto result = UIDocumentCanvasGeometry::Resize(
        layout,
        rect,
        Handle::Left,
        { 12.0f, 0.0f },
        false);
    EXPECT_FLOAT_EQ(result.offsetMin.x, 12.0f);
    EXPECT_FLOAT_EQ(result.size.x, 0.0f);
    EXPECT_FLOAT_EQ(result.offsetMax.x, 0.0f);
}

TEST(UIDocumentCanvasGeometryTests, GridSnapRoundsTheResizedEdge)
{
    const auto layout = Layout({ 100.0f, 50.0f });
    const UIRect rect { 0.0f, 0.0f, 100.0f, 50.0f };

    const auto result = UIDocumentCanvasGeometry::Resize(
        layout,
        rect,
        Handle::Right,
        { 5.0f, 0.0f },
        false,
        8.0f);
    EXPECT_FLOAT_EQ(result.size.x, 104.0f);
}

TEST(UIDocumentCanvasGeometryTests, AspectLockScalesTheOtherAxisForCornerDrags)
{
    const auto layout = Layout({ 200.0f, 100.0f });
    const UIRect rect { 0.0f, 0.0f, 200.0f, 100.0f };

    const auto result = UIDocumentCanvasGeometry::Resize(
        layout,
        rect,
        Handle::BottomRight,
        { 40.0f, 0.0f },
        true);
    EXPECT_FLOAT_EQ(result.size.x, 240.0f);
    EXPECT_FLOAT_EQ(result.size.y, 120.0f);
}

TEST(UIDocumentCanvasGeometryTests, SnapPicksTheClosestEdgeAndReportsTheGuide)
{
    const std::vector<UIRect> targets { { 0.0f, 0.0f, 200.0f, 100.0f } };
    const auto snap = UIDocumentCanvasGeometry::Snap(
        { 3.0f, 7.0f, 10.0f, 10.0f },
        targets,
        10.0f);
    EXPECT_TRUE(snap.hasVerticalGuide);
    EXPECT_FLOAT_EQ(snap.deltaX, -3.0f);
    EXPECT_FLOAT_EQ(snap.verticalGuide, 0.0f);
    EXPECT_TRUE(snap.hasHorizontalGuide);
    EXPECT_FLOAT_EQ(snap.deltaY, -7.0f);
    EXPECT_FLOAT_EQ(snap.horizontalGuide, 0.0f);
}

TEST(UIDocumentCanvasGeometryTests, SnapUsesElementCenters)
{
    const std::vector<UIRect> targets { { 0.0f, 0.0f, 100.0f, 100.0f } };
    // Center (50, 50) already matches the target center; the nearest match has
    // zero delta and must still report the center guide.
    const auto snap = UIDocumentCanvasGeometry::Snap(
        { 45.0f, 45.0f, 10.0f, 10.0f },
        targets,
        8.0f);
    EXPECT_TRUE(snap.hasVerticalGuide);
    EXPECT_FLOAT_EQ(snap.deltaX, 0.0f);
    EXPECT_FLOAT_EQ(snap.verticalGuide, 50.0f);
    EXPECT_TRUE(snap.hasHorizontalGuide);
    EXPECT_FLOAT_EQ(snap.deltaY, 0.0f);
    EXPECT_FLOAT_EQ(snap.horizontalGuide, 50.0f);
}

TEST(UIDocumentCanvasGeometryTests, SnapIsDisabledOutsideThreshold)
{
    const std::vector<UIRect> targets { { 0.0f, 0.0f, 10.0f, 10.0f } };
    const auto snap = UIDocumentCanvasGeometry::Snap(
        { 500.0f, 500.0f, 10.0f, 10.0f },
        targets,
        8.0f);
    EXPECT_FALSE(snap.hasVerticalGuide);
    EXPECT_FALSE(snap.hasHorizontalGuide);
    EXPECT_FLOAT_EQ(snap.deltaX, 0.0f);
    EXPECT_FLOAT_EQ(snap.deltaY, 0.0f);
}

TEST(UIDocumentCanvasGeometryTests, HitTestHandleFindsCornersEdgesAndCenter)
{
    const UIRect rect { 0.0f, 0.0f, 100.0f, 50.0f };
    EXPECT_EQ(
        UIDocumentCanvasGeometry::HitTestHandle(rect, { 0.0f, 0.0f }, 3.0f),
        Handle::TopLeft);
    EXPECT_EQ(
        UIDocumentCanvasGeometry::HitTestHandle(rect, { 100.0f, 0.0f }, 3.0f),
        Handle::TopRight);
    EXPECT_EQ(
        UIDocumentCanvasGeometry::HitTestHandle(rect, { 0.0f, 50.0f }, 3.0f),
        Handle::BottomLeft);
    EXPECT_EQ(
        UIDocumentCanvasGeometry::HitTestHandle(rect, { 100.0f, 50.0f }, 3.0f),
        Handle::BottomRight);
    EXPECT_EQ(
        UIDocumentCanvasGeometry::HitTestHandle(rect, { 50.0f, 0.0f }, 3.0f),
        Handle::Top);
    EXPECT_EQ(
        UIDocumentCanvasGeometry::HitTestHandle(rect, { 100.0f, 25.0f }, 3.0f),
        Handle::Right);
    EXPECT_EQ(
        UIDocumentCanvasGeometry::HitTestHandle(rect, { 50.0f, 25.0f }, 3.0f),
        Handle::None);
}

using namespace NLS::Editor::UIToolsTest;

TEST(UIDocumentCanvas, DragMovesElementAndUndoRestores)
{
    using NLS::Editor::UIDocumentTools::UIDocumentCanvasView;
    using NLS::Editor::UIDocumentTools::UIDocumentDesignerModel;

    ScopedDocumentDirectory directory;
    ScopedUndoService undo;
    ScopedImGuiContext imgui;

    const auto path = directory.Write(
        "Assets/UI/Hud.ui",
        UIDocumentDesignerModel::CreateDocumentSource("Hud"));
    UIDocumentDesignerModel model;
    ASSERT_TRUE(model.Load(path));
    const auto rootGuid = model.GetRootGuid();
    ASSERT_TRUE(model.AddChildAt(rootGuid, "Panel", 100.0f, 100.0f));
    const auto childGuid = model.GetSelectedNodeGuid();
    ASSERT_TRUE(model.SetNodeLayoutSize(childGuid, 200.0f, 100.0f));

    UIDocumentCanvasView view;
    UIDocumentCanvasView::Options options;
    options.showGrid = false;
    options.snapToGrid = false;
    options.snapToGuides = false;
    options.showRulers = false;

    const auto draw = [&]()
    {
        ImGui::SetNextWindowPos({ 0.0f, 0.0f });
        ImGui::SetNextWindowSize({ 1000.0f, 800.0f });
        ImGui::Begin("UICanvasTestWindow", nullptr, ImGuiWindowFlags_NoDecoration);
        view.Draw(model, options);
        ImGui::End();
    };

    RenderFrame(draw);
    view.GetViewState().zoom = 1.0f;
    view.GetViewState().pan = { 40.0f, 40.0f };
    RenderFrame(draw);

    const ImVec2 origin = view.GetCanvasOrigin();
    const ImVec2 start {
        origin.x + view.GetViewState().pan.x + 150.0f,
        origin.y + view.GetViewState().pan.y + 150.0f
    };

    auto& io = ImGui::GetIO();
    io.AddMousePosEvent(start.x, start.y);
    RenderFrame(draw);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    RenderFrame(draw);
    io.AddMousePosEvent(start.x + 40.0f, start.y + 20.0f);
    RenderFrame(draw);
    io.AddMousePosEvent(start.x + 60.0f, start.y + 30.0f);
    RenderFrame(draw);

    const auto* offset = model.GetNodeProperty(childGuid, "layout.offsetMin");
    ASSERT_NE(offset, nullptr);
    EXPECT_FLOAT_EQ((*offset)[0].get<float>(), 160.0f);
    EXPECT_FLOAT_EQ((*offset)[1].get<float>(), 130.0f);

    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    RenderFrame(draw);

    ASSERT_TRUE(undo.Get().RequestUndo().Succeeded());
    offset = model.GetNodeProperty(childGuid, "layout.offsetMin");
    ASSERT_NE(offset, nullptr);
    EXPECT_FLOAT_EQ((*offset)[0].get<float>(), 100.0f);
    EXPECT_FLOAT_EQ((*offset)[1].get<float>(), 100.0f);
}
