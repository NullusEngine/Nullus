#include "UI/UIDocumentCanvasView.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <utility>

#include <Guid.h>

namespace NLS::Editor::UIDocumentTools
{
    namespace
    {
        using Json = UIDocumentDesignerModel::Json;
        using Vec2 = NLS::Engine::UI::UIVector2;
        using Rect = NLS::Engine::UI::UIRect;
        using Handle = UIDocumentCanvasGeometry::Handle;

        constexpr ImU32 kCanvasBackground = IM_COL32(11, 10, 14, 255);
        constexpr ImU32 kDeviceBackground = IM_COL32(20, 23, 29, 255);
        constexpr ImU32 kDeviceBorder = IM_COL32(58, 55, 66, 255);
        constexpr ImU32 kGridMinor = IM_COL32(255, 255, 255, 12);
        constexpr ImU32 kGridMajor = IM_COL32(255, 255, 255, 26);
        constexpr ImU32 kAccent = IM_COL32(76, 141, 255, 255);
        constexpr ImU32 kAccentSoft = IM_COL32(76, 141, 255, 120);
        constexpr ImU32 kHoverOutline = IM_COL32(160, 170, 190, 220);
        constexpr ImU32 kWireOutline = IM_COL32(130, 140, 160, 55);
        constexpr ImU32 kGuide = IM_COL32(224, 182, 90, 235);
        constexpr ImU32 kError = IM_COL32(236, 104, 92, 255);
        constexpr ImU32 kWarning = IM_COL32(224, 182, 90, 255);
        constexpr ImU32 kText = IM_COL32(204, 203, 210, 255);
        constexpr ImU32 kTextDim = IM_COL32(138, 135, 148, 255);
        constexpr ImU32 kBadgeBackground = IM_COL32(27, 35, 52, 235);
        constexpr ImU32 kBadgeBorder = IM_COL32(60, 84, 128, 255);
        constexpr float kHandleScreenSize = 7.0f;
        constexpr float kDragThresholdPixels = 3.0f;
        constexpr float kMinZoom = 0.05f;
        constexpr float kMaxZoom = 8.0f;

        struct DevicePreset
        {
            const char* name;
            float width;
            float height;
        };

        constexpr DevicePreset kDevicePresets[] = {
            { "1920 x 1080 (Landscape)", 1920.0f, 1080.0f },
            { "2560 x 1440 (Landscape)", 2560.0f, 1440.0f },
            { "1280 x 720 (Landscape)", 1280.0f, 720.0f },
            { "1920 x 1200 (Landscape)", 1920.0f, 1200.0f },
            { "1080 x 2340 (Portrait)", 1080.0f, 2340.0f },
            { "1080 x 1920 (Portrait)", 1080.0f, 1920.0f },
        };

        std::string g_clipboard;

        ImVec2 Add(const ImVec2 left, const ImVec2 right)
        {
            return { left.x + right.x, left.y + right.y };
        }

        ImVec2 Subtract(const ImVec2 left, const ImVec2 right)
        {
            return { left.x - right.x, left.y - right.y };
        }

        ImVec2 Multiply(const ImVec2 value, const float scale)
        {
            return { value.x * scale, value.y * scale };
        }

        bool Intersects(const Rect& left, const Rect& right)
        {
            return left.x < right.x + right.width &&
                left.x + left.width > right.x &&
                left.y < right.y + right.height &&
                left.y + left.height > right.y;
        }

        void AddDashedLine(
            ImDrawList& drawList,
            const ImVec2 start,
            const ImVec2 end,
            const ImU32 color,
            const float thickness = 1.0f)
        {
            constexpr float dash = 5.0f;
            constexpr float gap = 4.0f;
            const ImVec2 delta = Subtract(end, start);
            const float length = std::sqrt(delta.x * delta.x + delta.y * delta.y);
            if (length <= 0.001f)
                return;
            const ImVec2 direction { delta.x / length, delta.y / length };
            float travelled = 0.0f;
            while (travelled < length)
            {
                const float segmentEnd = std::min(length, travelled + dash);
                drawList.AddLine(
                    Add(start, Multiply(direction, travelled)),
                    Add(start, Multiply(direction, segmentEnd)),
                    color,
                    thickness);
                travelled += dash + gap;
            }
        }
    }

    UIDocumentCanvasView::UIDocumentCanvasView() = default;

    UIDocumentCanvasView::~UIDocumentCanvasView() = default;

    size_t UIDocumentCanvasView::GetDevicePresetCount()
    {
        return std::size(kDevicePresets);
    }

    const char* UIDocumentCanvasView::GetDevicePresetName(const int presetIndex)
    {
        return presetIndex >= 0 && presetIndex < static_cast<int>(std::size(kDevicePresets))
            ? kDevicePresets[presetIndex].name
            : "Custom";
    }

    Vec2 UIDocumentCanvasView::GetDevicePresetSize(const int presetIndex)
    {
        if (presetIndex < 0 || presetIndex >= static_cast<int>(std::size(kDevicePresets)))
            return { 1920.0f, 1080.0f };
        return { kDevicePresets[presetIndex].width, kDevicePresets[presetIndex].height };
    }

    // ---------------------------------------------------------------------
    // Geometry
    // ---------------------------------------------------------------------
    UIDocumentCanvasGeometry::ResizeResult UIDocumentCanvasGeometry::Resize(
        const NLS::Engine::UI::UILayoutParams& layout,
        const NLS::Engine::UI::UIRect& rect,
        const Handle handle,
        const NLS::Engine::UI::UIVector2 delta,
        const bool lockAspect,
        const float gridSize)
    {
        ResizeResult result;
        // Keep the stored values as the starting point. A stretched axis keeps
        // its (unused) stored size instead of materializing the runtime width.
        result.size = layout.size;
        result.offsetMin = layout.offsetMin;
        result.offsetMax = layout.offsetMax;

        const bool left = handle == Handle::Left || handle == Handle::TopLeft ||
            handle == Handle::BottomLeft;
        const bool right = handle == Handle::Right || handle == Handle::TopRight ||
            handle == Handle::BottomRight;
        const bool top = handle == Handle::Top || handle == Handle::TopLeft ||
            handle == Handle::TopRight;
        const bool bottom = handle == Handle::Bottom || handle == Handle::BottomLeft ||
            handle == Handle::BottomRight;
        if (!left && !right && !top && !bottom)
            return result;

        const bool stretchedX =
            std::abs(layout.anchorMax.x - layout.anchorMin.x) > 0.0001f;
        const bool stretchedY =
            std::abs(layout.anchorMax.y - layout.anchorMin.y) > 0.0001f;
        const float startWidth = layout.size.x > 0.0f ? layout.size.x : rect.width;
        const float startHeight = layout.size.y > 0.0f ? layout.size.y : rect.height;
        const float startOffsetMinX = result.offsetMin.x;
        const float startOffsetMinY = result.offsetMin.y;
        const auto snap = [gridSize](const float value)
        {
            return gridSize > 0.0001f ? std::round(value / gridSize) * gridSize : value;
        };

        float width = startWidth;
        float height = startHeight;
        if (!stretchedX && (left || right))
        {
            width = std::max(0.0f, startWidth + (right ? delta.x : 0.0f) - (left ? delta.x : 0.0f));
            width = std::max(0.0f, snap(width));
        }
        if (!stretchedY && (top || bottom))
        {
            height = std::max(0.0f, startHeight + (bottom ? delta.y : 0.0f) - (top ? delta.y : 0.0f));
            height = std::max(0.0f, snap(height));
        }

        // Shift keeps the ratio for corner drags on non-stretched axes.
        if (lockAspect && !stretchedX && !stretchedY && (left || right) && (top || bottom))
        {
            const float aspect = startHeight > 0.0001f ? startWidth / startHeight : 0.0f;
            if (aspect > 0.0001f)
            {
                const float relativeX = startWidth > 0.0001f
                    ? std::abs(width - startWidth) / startWidth
                    : 0.0f;
                const float relativeY = startHeight > 0.0001f
                    ? std::abs(height - startHeight) / startHeight
                    : 0.0f;
                if (relativeX >= relativeY)
                    height = width / aspect;
                else
                    width = height * aspect;
            }
        }

        if (stretchedX)
        {
            if (left)
                result.offsetMin.x += delta.x;
            if (right)
                result.offsetMax.x += delta.x;
            if (left)
                result.offsetMin.x = snap(result.offsetMin.x);
            if (right)
                result.offsetMax.x = snap(result.offsetMax.x);
        }
        else
        {
            result.size.x = width;
            if (right)
                result.offsetMin.x = startOffsetMinX + (width - startWidth) * layout.pivot.x;
            else if (left)
                result.offsetMin.x = startOffsetMinX + (startWidth - width) * (1.0f - layout.pivot.x);
        }

        if (stretchedY)
        {
            if (top)
                result.offsetMin.y += delta.y;
            if (bottom)
                result.offsetMax.y += delta.y;
            if (top)
                result.offsetMin.y = snap(result.offsetMin.y);
            if (bottom)
                result.offsetMax.y = snap(result.offsetMax.y);
        }
        else
        {
            result.size.y = height;
            if (bottom)
                result.offsetMin.y = startOffsetMinY + (height - startHeight) * layout.pivot.y;
            else if (top)
                result.offsetMin.y = startOffsetMinY + (startHeight - height) * (1.0f - layout.pivot.y);
        }

        const auto different = [](const float leftValue, const float rightValue)
        {
            return std::abs(leftValue - rightValue) > 0.0001f;
        };
        result.changed =
            different(result.size.x, layout.size.x) ||
            different(result.size.y, layout.size.y) ||
            different(result.offsetMin.x, layout.offsetMin.x) ||
            different(result.offsetMin.y, layout.offsetMin.y) ||
            different(result.offsetMax.x, layout.offsetMax.x) ||
            different(result.offsetMax.y, layout.offsetMax.y);
        return result;
    }

    UIDocumentCanvasGeometry::SnapResult UIDocumentCanvasGeometry::Snap(
        const NLS::Engine::UI::UIRect& movingRect,
        const std::vector<NLS::Engine::UI::UIRect>& targets,
        const float threshold)
    {
        SnapResult result;
        if (threshold <= 0.0f || targets.empty())
            return result;

        const auto consider = [threshold](
                                  const float moving,
                                  const float target,
                                  float& bestDelta,
                                  float& bestGuide,
                                  bool& found)
        {
            const float delta = target - moving;
            if (std::abs(delta) > threshold)
                return;
            if (!found || std::abs(delta) < std::abs(bestDelta))
            {
                bestDelta = delta;
                bestGuide = target;
                found = true;
            }
        };

        const float movingXs[3] = {
            movingRect.x,
            movingRect.x + movingRect.width * 0.5f,
            movingRect.x + movingRect.width
        };
        const float movingYs[3] = {
            movingRect.y,
            movingRect.y + movingRect.height * 0.5f,
            movingRect.y + movingRect.height
        };

        float bestX = 0.0f;
        float bestY = 0.0f;
        for (const auto& target : targets)
        {
            const float targetXs[3] = {
                target.x,
                target.x + target.width * 0.5f,
                target.x + target.width
            };
            const float targetYs[3] = {
                target.y,
                target.y + target.height * 0.5f,
                target.y + target.height
            };
            for (const float moving : movingXs)
            {
                for (const float candidate : targetXs)
                {
                    consider(moving, candidate, bestX, result.verticalGuide, result.hasVerticalGuide);
                }
            }
            for (const float moving : movingYs)
            {
                for (const float candidate : targetYs)
                {
                    consider(moving, candidate, bestY, result.horizontalGuide, result.hasHorizontalGuide);
                }
            }
        }

        result.deltaX = result.hasVerticalGuide ? bestX : 0.0f;
        result.deltaY = result.hasHorizontalGuide ? bestY : 0.0f;
        return result;
    }

    UIDocumentCanvasGeometry::Handle UIDocumentCanvasGeometry::HitTestHandle(
        const NLS::Engine::UI::UIRect& rect,
        const NLS::Engine::UI::UIVector2 point,
        const float halfSize)
    {
        const float left = rect.x;
        const float right = rect.x + rect.width;
        const float top = rect.y;
        const float bottom = rect.y + rect.height;
        const float centerX = (left + right) * 0.5f;
        const float centerY = (top + bottom) * 0.5f;
        const auto near = [point, halfSize](const float x, const float y)
        {
            return std::abs(point.x - x) <= halfSize && std::abs(point.y - y) <= halfSize;
        };

        if (near(left, top)) return Handle::TopLeft;
        if (near(right, top)) return Handle::TopRight;
        if (near(left, bottom)) return Handle::BottomLeft;
        if (near(right, bottom)) return Handle::BottomRight;
        if (near(left, centerY)) return Handle::Left;
        if (near(right, centerY)) return Handle::Right;
        if (near(centerX, top)) return Handle::Top;
        if (near(centerX, bottom)) return Handle::Bottom;
        return Handle::None;
    }

    // ---------------------------------------------------------------------
    // View
    // ---------------------------------------------------------------------
    void UIDocumentCanvasView::Draw(UIDocumentDesignerModel& model, const Options& options)
    {
        m_hovered = false;
        m_canvasOrigin = ImGui::GetCursorScreenPos();
        const ImVec2 available = ImGui::GetContentRegionAvail();
        m_canvasSize = {
            std::max(1.0f, available.x),
            std::max(1.0f, available.y)
        };
        const ImVec2 canvasMax {
            m_canvasOrigin.x + m_canvasSize.x,
            m_canvasOrigin.y + m_canvasSize.y
        };

        ImGui::InvisibleButton(
            "##UIDocumentCanvas",
            m_canvasSize,
            ImGuiButtonFlags_MouseButtonLeft |
                ImGuiButtonFlags_MouseButtonRight |
                ImGuiButtonFlags_MouseButtonMiddle);
        m_hovered = ImGui::IsItemHovered();
        const bool hovered = m_hovered;

        m_renderer.Sync(model);
        if (!m_referenceInitialized && m_canvasSize.x > 2.0f && m_canvasSize.y > 2.0f)
        {
            m_referenceInitialized = true;
            FrameAll(model);
        }

        // Wheel: Ctrl zooms around the cursor, plain wheel pans.
        if (hovered)
        {
            auto& io = ImGui::GetIO();
            if (io.MouseWheel != 0.0f)
            {
                if (io.KeyCtrl)
                {
                    const float previousZoom = m_view.zoom;
                    const auto documentPoint = ScreenToDocument(io.MousePos);
                    SetZoom(model, m_view.zoom * (io.MouseWheel > 0.0f ? 1.15f : 1.0f / 1.15f));
                    if (std::abs(previousZoom - m_view.zoom) > 0.0001f)
                    {
                        m_view.pan = {
                            io.MousePos.x - m_canvasOrigin.x - documentPoint.x * m_view.zoom,
                            io.MousePos.y - m_canvasOrigin.y - documentPoint.y * m_view.zoom
                        };
                    }
                }
                else if (io.KeyShift)
                {
                    m_view.pan.x += io.MouseWheel * 48.0f;
                }
                else
                {
                    m_view.pan.y += io.MouseWheel * 48.0f;
                }
            }
        }

        // Palette drops: created element lands where the asset was released.
        if (hovered && ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kUIDesignerPalettePayload))
            {
                const std::string type(
                    static_cast<const char*>(payload->Data),
                    static_cast<size_t>(payload->DataSize));
                const auto documentPoint = ScreenToDocument(ImGui::GetIO().MousePos);
                const auto* target = m_renderer.HitTest(documentPoint);
                const std::string parentGuid = target != nullptr
                    ? target->GetNodeGuid().ToString()
                    : model.GetRootGuid();
                Rect parentContent;
                float localX = documentPoint.x;
                float localY = documentPoint.y;
                if (m_renderer.GetElementContentRect(parentGuid, parentContent))
                {
                    localX -= parentContent.x;
                    localY -= parentContent.y;
                }
                if (model.AddChildAt(parentGuid, type, localX - 80.0f, localY - 16.0f))
                    RequestFrameSelection();
            }
            ImGui::EndDragDropTarget();
        }

        HandleInput(model, options);

        // Input may have changed the document; rebuild before painting.
        m_renderer.Sync(model);
        const auto referenceSize = m_renderer.GetReferenceSize();

        auto* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(m_canvasOrigin, canvasMax, kCanvasBackground);

        // Device frame and grid.
        const ImVec2 deviceMin = DocumentToScreen({ 0.0f, 0.0f });
        const ImVec2 deviceMax = DocumentToScreen(referenceSize);
        drawList->AddRectFilled(deviceMin, deviceMax, kDeviceBackground);
        if (options.showGrid)
        {
            const float step = options.gridSize > 0.0f ? options.gridSize : 8.0f;
            float screenStep = step * m_view.zoom;
            float documentStep = step;
            while (screenStep < 4.0f)
            {
                documentStep *= 2.0f;
                screenStep *= 2.0f;
            }
            drawList->PushClipRect(deviceMin, deviceMax, true);
            for (float x = 0.0f; x <= referenceSize.x; x += documentStep)
            {
                const ImVec2 start = DocumentToScreen({ x, 0.0f });
                const ImVec2 end = DocumentToScreen({ x, referenceSize.y });
                const bool major = std::fmod(x, documentStep * 10.0f) < 0.01f;
                drawList->AddLine(start, end, major ? kGridMajor : kGridMinor, 1.0f);
            }
            for (float y = 0.0f; y <= referenceSize.y; y += documentStep)
            {
                const ImVec2 start = DocumentToScreen({ 0.0f, y });
                const ImVec2 end = DocumentToScreen({ referenceSize.x, y });
                const bool major = std::fmod(y, documentStep * 10.0f) < 0.01f;
                drawList->AddLine(start, end, major ? kGridMajor : kGridMinor, 1.0f);
            }
            drawList->PopClipRect();
        }

        // Content clipped to the device viewport, like the Game View.
        const ImVec2 clipMin {
            std::max(deviceMin.x, m_canvasOrigin.x),
            std::max(deviceMin.y, m_canvasOrigin.y)
        };
        const ImVec2 clipMax {
            std::min(deviceMax.x, canvasMax.x),
            std::min(deviceMax.y, canvasMax.y)
        };
        if (clipMax.x > clipMin.x && clipMax.y > clipMin.y)
        {
            UIDocumentCanvasRenderer::RenderParameters parameters;
            parameters.origin = Add(m_canvasOrigin, m_view.pan);
            parameters.scale = m_view.zoom;
            parameters.clipMin = clipMin;
            parameters.clipMax = clipMax;
            parameters.frameId = ImGui::GetFrameCount();
            m_renderer.Render(model, *drawList, parameters);
        }

        drawList->AddRect(deviceMin, deviceMax, kDeviceBorder, 0.0f, 0, 1.0f);
        if (options.showRulers)
            DrawRulers();

        DrawOverlays(model, options);

        // Status line.
        {
            char status[160] {};
            snprintf(
                status,
                sizeof(status),
                "x %.0f  y %.0f   |   %.0f%%   |   %u batches",
                m_mouseDocument.x,
                m_mouseDocument.y,
                m_view.zoom * 100.0f,
                m_renderer.GetLastDrawBatchCount());
            drawList->AddText(
                ImVec2(m_canvasOrigin.x + 10.0f, canvasMax.y - 20.0f),
                kTextDim,
                status);
        }

        if (!m_renderer.IsValid())
        {
            drawList->AddText(
                ImVec2(m_canvasOrigin.x + 12.0f, m_canvasOrigin.y + 12.0f),
                kError,
                "The document has validation errors. Fix them in Diagnostics to resume preview.");
        }

        if (m_requestFrameAll)
        {
            m_requestFrameAll = false;
            FrameAll(model);
        }
        if (m_requestFrameSelection)
        {
            m_requestFrameSelection = false;
            if (!model.GetSelectedNodeGuid().empty())
                FrameNode(model, model.GetSelectedNodeGuid());
            else
                FrameAll(model);
        }

        DrawContextMenu(model);
    }

    void UIDocumentCanvasView::HandleInput(
        UIDocumentDesignerModel& model,
        const Options& options)
    {
        auto& io = ImGui::GetIO();
        m_mouseDocument = ScreenToDocument(io.MousePos);

        // Middle-drag / Space+left-drag pans the view.
        if (m_interaction == Interaction::None && m_hovered)
        {
            const bool spacePan =
                ImGui::IsKeyDown(ImGuiKey_Space) &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Left);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) || spacePan)
            {
                m_interaction = Interaction::Pan;
                m_gesturePressMouse = io.MousePos;
                m_panStart = m_view.pan;
            }
        }

        if (m_interaction == Interaction::Pan)
        {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Middle) ||
                ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                m_view.pan = Add(m_panStart, Subtract(io.MousePos, m_gesturePressMouse));
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }
            else
            {
                m_interaction = Interaction::None;
            }
            return;
        }

        if (!options.allowEditing)
            return;

        if (m_interaction == Interaction::None && m_hovered)
        {
            const auto* hit = m_renderer.HitTest(m_mouseDocument);
            if (hit != nullptr)
                m_hoveredGuid = hit->GetNodeGuid().ToString();
            else
                m_hoveredGuid.clear();

            // Hover cursor feedback over resize handles.
            if (model.GetSelectedNodes().size() == 1u)
            {
                const auto selected = model.GetSelectedNodeGuid();
                Rect rect;
                if (CanManipulateParent(model, selected) &&
                    m_renderer.GetElementRect(selected, rect))
                {
                    const auto handle = UIDocumentCanvasGeometry::HitTestHandle(
                        rect,
                        m_mouseDocument,
                        kHandleScreenSize / std::max(0.001f, m_view.zoom));
                    switch (handle)
                    {
                    case Handle::Left:
                    case Handle::Right:
                        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                        break;
                    case Handle::Top:
                    case Handle::Bottom:
                        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
                        break;
                    case Handle::TopLeft:
                    case Handle::BottomRight:
                        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
                        break;
                    case Handle::TopRight:
                    case Handle::BottomLeft:
                        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNESW);
                        break;
                    case Handle::None:
                        break;
                    }
                }
            }
        }

        if (m_interaction == Interaction::None && m_hovered &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            bool handled = false;

            // A press on a resize handle takes priority over element picking.
            if (model.GetSelectedNodes().size() == 1u)
            {
                const auto selected = model.GetSelectedNodeGuid();
                Rect rect;
                if (CanManipulateParent(model, selected) &&
                    m_renderer.GetElementRect(selected, rect))
                {
                    const auto handle = UIDocumentCanvasGeometry::HitTestHandle(
                        rect,
                        m_mouseDocument,
                        kHandleScreenSize / std::max(0.001f, m_view.zoom));
                    if (handle != Handle::None)
                    {
                        m_gestureGuid = selected;
                        m_gestureHandle = handle;
                        m_gestureStartRect = rect;
                        m_gesturePressMouse = io.MousePos;
                        if (const auto* element = m_renderer.FindElement(selected))
                            m_gestureStartLayout = element->GetLayout();
                        m_gestureStartOffsetMin = m_gestureStartLayout.offsetMin;
                        m_gestureStartOffsetMax = m_gestureStartLayout.offsetMax;
                        m_interaction = Interaction::Resize;
                        handled = true;
                    }
                }
            }

            if (!handled)
            {
                const auto* hit = m_renderer.HitTest(m_mouseDocument);
                if (hit != nullptr)
                {
                    const auto guid = hit->GetNodeGuid().ToString();
                    if (io.KeyCtrl)
                        model.ToggleSelection(guid);
                    else if (io.KeyShift)
                        model.AddToSelection(guid);
                    else if (!IsSelected(model, guid))
                        model.SelectNode(guid);

                    m_pressOnEmpty = false;
                    m_gestureGuid = guid;
                    m_gestureStartRect = hit->GetLayoutRect();
                    m_gestureStartLayout = hit->GetLayout();
                    m_gestureStartOffsetMin = m_gestureStartLayout.offsetMin;
                    m_gestureStartOffsetMax = m_gestureStartLayout.offsetMax;
                    m_gesturePressMouse = io.MousePos;
                    m_interaction = Interaction::PressPending;
                }
                else
                {
                    if (!io.KeyShift && !io.KeyCtrl)
                        model.ClearSelection();
                    m_pressOnEmpty = true;
                    m_gestureGuid.clear();
                    m_gesturePressMouse = io.MousePos;
                    m_marqueeStart = io.MousePos;
                    m_marqueeBaseSelection = model.GetSelectedNodes();
                    m_marqueeAdd = io.KeyShift;
                    m_marqueeToggle = io.KeyCtrl;
                    m_interaction = Interaction::PressPending;
                }
            }
        }

        if (m_interaction == Interaction::PressPending)
        {
            const ImVec2 delta = Subtract(io.MousePos, m_gesturePressMouse);
            const float distanceSquared = delta.x * delta.x + delta.y * delta.y;
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                m_interaction = Interaction::None;
                m_gestureGuid.clear();
            }
            else if (distanceSquared >= kDragThresholdPixels * kDragThresholdPixels)
            {
                if (m_pressOnEmpty)
                {
                    m_interaction = Interaction::Marquee;
                }
                else if (!m_gestureGuid.empty() &&
                         IsSelected(model, m_gestureGuid) &&
                         model.GetSelectedNodes().size() == 1u &&
                         CanManipulateParent(model, m_gestureGuid))
                {
                    BeginMove(model);
                }
                else
                {
                    m_interaction = Interaction::None;
                }
            }
        }

        switch (m_interaction)
        {
        case Interaction::Move:
            UpdateMove(model, options);
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
                FinishInteraction(model, false);
            break;
        case Interaction::Resize:
            UpdateResize(model, options);
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
                FinishInteraction(model, false);
            break;
        case Interaction::Marquee:
        {
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                const auto end = ScreenToDocument(io.MousePos);
                const auto start = ScreenToDocument(m_marqueeStart);
                Rect marquee {
                    std::min(start.x, end.x),
                    std::min(start.y, end.y),
                    std::abs(end.x - start.x),
                    std::abs(end.y - start.y)
                };
                std::vector<std::string> hits;
                CollectElementsUnderMarquee(model, marquee, options, hits);
                if (m_marqueeToggle)
                {
                    for (const auto& guid : hits)
                        model.ToggleSelection(guid);
                }
                else if (m_marqueeAdd)
                {
                    auto selection = m_marqueeBaseSelection;
                    for (const auto& guid : hits)
                    {
                        if (std::find(selection.begin(), selection.end(), guid) == selection.end())
                            selection.push_back(guid);
                    }
                    model.SetSelection(std::move(selection));
                }
                else
                {
                    model.SetSelection(std::move(hits));
                }
                m_interaction = Interaction::None;
            }
            break;
        }
        case Interaction::None:
        case Interaction::PressPending:
        case Interaction::Pan:
            break;
        }
    }

    void UIDocumentCanvasView::BeginMove(UIDocumentDesignerModel& model)
    {
        m_interaction = Interaction::Move;
        m_gestureSessionOpen = true;
        m_activeSnap = {};
        model.BeginTransaction("Move UI Element", "UIDocument.CanvasMove:" + m_gestureGuid);
    }

    void UIDocumentCanvasView::UpdateMove(
        UIDocumentDesignerModel& model,
        const Options& options)
    {
        if (m_gestureGuid.empty())
            return;
        auto& io = ImGui::GetIO();
        if (!m_gestureSessionOpen)
        {
            // Defensive: a move without an open session must not write outside
            // the model's undo tracking.
            m_gestureSessionOpen = true;
            model.BeginTransaction("Move UI Element", "UIDocument.CanvasMove:" + m_gestureGuid);
        }

        const ImVec2 screenDelta = Subtract(io.MousePos, m_gesturePressMouse);
        Vec2 delta { screenDelta.x / m_view.zoom, screenDelta.y / m_view.zoom };

        m_activeSnap = {};
        if (options.snapToGuides && !io.KeyAlt)
        {
            std::vector<Rect> targets;
            CollectSnapTargets(model, m_gestureGuid, targets);
            Rect moving = m_gestureStartRect;
            moving.x += delta.x;
            moving.y += delta.y;
            m_activeSnap = UIDocumentCanvasGeometry::Snap(
                moving,
                targets,
                6.0f / std::max(0.001f, m_view.zoom));
            delta.x += m_activeSnap.deltaX;
            delta.y += m_activeSnap.deltaY;
        }

        if (options.snapToGrid && !io.KeyAlt)
        {
            const float grid = std::max(1.0f, options.gridSize);
            if (!m_activeSnap.hasVerticalGuide)
            {
                const float snapped = std::round(
                    (m_gestureStartRect.x + delta.x) / grid) * grid;
                delta.x = snapped - m_gestureStartRect.x;
            }
            if (!m_activeSnap.hasHorizontalGuide)
            {
                const float snapped = std::round(
                    (m_gestureStartRect.y + delta.y) / grid) * grid;
                delta.y = snapped - m_gestureStartRect.y;
            }
        }

        const bool stretchedX =
            std::abs(m_gestureStartLayout.anchorMax.x - m_gestureStartLayout.anchorMin.x) > 0.0001f;
        const bool stretchedY =
            std::abs(m_gestureStartLayout.anchorMax.y - m_gestureStartLayout.anchorMin.y) > 0.0001f;

        Vec2 offsetMin {
            m_gestureStartOffsetMin.x + delta.x,
            m_gestureStartOffsetMin.y + delta.y
        };
        Vec2 offsetMax = m_gestureStartOffsetMax;
        if (stretchedX)
            offsetMax.x += delta.x;
        if (stretchedY)
            offsetMax.y += delta.y;

        model.SetNodeOffsets(m_gestureGuid, offsetMin, offsetMax, false);
    }

    void UIDocumentCanvasView::UpdateResize(
        UIDocumentDesignerModel& model,
        const Options& options)
    {
        if (m_gestureGuid.empty())
            return;
        auto& io = ImGui::GetIO();
        if (!m_gestureSessionOpen)
        {
            m_gestureSessionOpen = true;
            model.BeginTransaction("Resize UI Element", "UIDocument.CanvasResize:" + m_gestureGuid);
        }

        const ImVec2 screenDelta = Subtract(io.MousePos, m_gesturePressMouse);
        const Vec2 delta { screenDelta.x / m_view.zoom, screenDelta.y / m_view.zoom };
        const float grid = (options.snapToGrid && !io.KeyAlt)
            ? std::max(1.0f, options.gridSize)
            : 0.0f;
        const auto result = UIDocumentCanvasGeometry::Resize(
            m_gestureStartLayout,
            m_gestureStartRect,
            m_gestureHandle,
            delta,
            io.KeyShift,
            grid);
        if (!result.changed)
            return;
        model.SetNodeSizeAndOffsets(
            m_gestureGuid,
            result.size,
            result.offsetMin,
            result.offsetMax,
            false);
    }

    void UIDocumentCanvasView::FinishInteraction(
        UIDocumentDesignerModel& model,
        const bool cancelled)
    {
        if (m_gestureSessionOpen)
        {
            if (cancelled)
                model.CancelTransaction();
            else
                model.EndTransaction();
        }
        m_gestureSessionOpen = false;
        m_interaction = Interaction::None;
        m_gestureGuid.clear();
        m_gestureHandle = Handle::None;
        m_activeSnap = {};
    }

    void UIDocumentCanvasView::CancelGesture(UIDocumentDesignerModel& model)
    {
        if (m_interaction == Interaction::None)
            return;
        FinishInteraction(model, true);
        m_pressOnEmpty = false;
    }

    void UIDocumentCanvasView::CollectSnapTargets(
        const UIDocumentDesignerModel& model,
        const std::string_view movingNodeGuid,
        std::vector<Rect>& targets) const
    {
        const auto referenceSize = m_renderer.GetReferenceSize();
        targets.push_back({ 0.0f, 0.0f, referenceSize.x, referenceSize.y });

        const auto parentGuid = model.GetNodeParentGuid(movingNodeGuid);
        if (parentGuid.empty())
            return;

        Rect content;
        if (m_renderer.GetElementContentRect(parentGuid, content))
            targets.push_back(content);

        const auto* parent = model.FindNode(parentGuid);
        if (parent == nullptr)
            return;
        const auto children = parent->find("children");
        if (children == parent->end() || !children->is_array())
            return;
        for (const auto& child : *children)
        {
            const auto guid = child.value("nodeGuid", std::string {});
            if (guid == movingNodeGuid)
                continue;
            Rect rect;
            if (m_renderer.GetElementRect(guid, rect))
                targets.push_back(rect);
        }
    }

    void UIDocumentCanvasView::CollectElementsUnderMarquee(
        const UIDocumentDesignerModel& model,
        const NLS::Engine::UI::UIRect& marquee,
        const Options& options,
        std::vector<std::string>& selected) const
    {
        struct Candidate
        {
            const NLS::Engine::UI::UIElement* element = nullptr;
            std::string guid;
        };

        std::vector<Candidate> candidates;
        const auto* root = m_renderer.GetInstance() != nullptr
            ? m_renderer.GetInstance()->GetRoot()
            : nullptr;
        if (root == nullptr)
            return;

        const std::function<void(const NLS::Engine::UI::UIElement&)> visit =
            [&](const NLS::Engine::UI::UIElement& element)
            {
                if (!element.GetStyle().visible && !options.showHiddenElements)
                    return;
                for (const auto& child : element.GetChildren())
                {
                    if (child != nullptr)
                        visit(*child);
                }
                if (&element == root)
                    return;
                if (Intersects(element.GetLayoutRect(), marquee))
                    candidates.push_back({ &element, element.GetNodeGuid().ToString() });
            };
        visit(*root);

        // Keep the deepest match per branch so a marquee around a panel does not
        // also select every ancestor above it.
        for (const auto& candidate : candidates)
        {
            bool isAncestorOfOther = false;
            for (const auto& other : candidates)
            {
                if (other.element == candidate.element)
                    continue;
                for (const auto* parent = other.element->GetParent();
                     parent != nullptr;
                     parent = parent->GetParent())
                {
                    if (parent == candidate.element)
                    {
                        isAncestorOfOther = true;
                        break;
                    }
                }
                if (isAncestorOfOther)
                    break;
            }
            if (!isAncestorOfOther)
                selected.push_back(candidate.guid);
        }
    }

    // ---------------------------------------------------------------------
    // Drawing
    // ---------------------------------------------------------------------
    void UIDocumentCanvasView::DrawRulers() const
    {
        auto* drawList = ImGui::GetWindowDrawList();
        constexpr float thickness = 18.0f;
        const ImVec2 topMin = m_canvasOrigin;
        const ImVec2 topMax { m_canvasOrigin.x + m_canvasSize.x, m_canvasOrigin.y + thickness };
        const ImVec2 leftMin = m_canvasOrigin;
        const ImVec2 leftMax { m_canvasOrigin.x + 15.0f, m_canvasOrigin.y + m_canvasSize.y };

        drawList->AddRectFilled(topMin, topMax, IM_COL32(22, 20, 25, 240));
        drawList->AddRectFilled(leftMin, leftMax, IM_COL32(22, 20, 25, 240));
        drawList->AddLine(
            { topMin.x, topMax.y },
            { topMax.x, topMax.y },
            IM_COL32(58, 55, 66, 255));
        drawList->AddLine(
            { leftMax.x, leftMin.y },
            { leftMax.x, leftMax.y },
            IM_COL32(58, 55, 66, 255));

        float step = 100.0f;
        while (step * m_view.zoom < 40.0f)
            step *= 2.0f;

        drawList->PushClipRect(topMin, topMax, true);
        for (float x = 0.0f; x <= m_renderer.GetReferenceSize().x; x += step)
        {
            const float screenX = DocumentToScreen({ x, 0.0f }).x;
            if (screenX < topMin.x || screenX > topMax.x)
                continue;
            drawList->AddLine(
                { screenX, topMax.y - 5.0f },
                { screenX, topMax.y },
                IM_COL32(120, 118, 130, 255));
            if (std::fmod(x, step * 2.0f) < 0.01f)
            {
                char label[24] {};
                snprintf(label, sizeof(label), "%d", static_cast<int>(x));
                drawList->AddText(
                    { screenX + 2.0f, topMin.y + 2.0f },
                    kTextDim,
                    label);
            }
        }
        drawList->PopClipRect();

        drawList->PushClipRect(leftMin, leftMax, true);
        for (float y = 0.0f; y <= m_renderer.GetReferenceSize().y; y += step)
        {
            const float screenY = DocumentToScreen({ 0.0f, y }).y;
            if (screenY < leftMin.y || screenY > leftMax.y)
                continue;
            drawList->AddLine(
                { leftMax.x - 4.0f, screenY },
                { leftMax.x, screenY },
                IM_COL32(120, 118, 130, 255));
        }
        drawList->PopClipRect();
    }

    void UIDocumentCanvasView::DrawOverlays(
        const UIDocumentDesignerModel& model,
        const Options& options) const
    {
        auto* drawList = ImGui::GetWindowDrawList();
        const auto* root = m_renderer.GetInstance() != nullptr
            ? m_renderer.GetInstance()->GetRoot()
            : nullptr;
        if (root == nullptr)
            return;

        if (options.wireframe)
        {
            const std::function<void(const NLS::Engine::UI::UIElement&)> visit =
                [&](const NLS::Engine::UI::UIElement& element)
                {
                    if (!element.GetStyle().visible)
                        return;
                    const auto rect = element.GetLayoutRect();
                    drawList->AddRect(
                        DocumentToScreen({ rect.x, rect.y }),
                        DocumentToScreen({ rect.x + rect.width, rect.y + rect.height }),
                        kWireOutline);
                    for (const auto& child : element.GetChildren())
                    {
                        if (child != nullptr)
                            visit(*child);
                    }
                };
            visit(*root);
        }

        if (m_hoveredGuid != m_gestureGuid && !m_hoveredGuid.empty())
        {
            Rect rect;
            if (m_renderer.GetElementRect(m_hoveredGuid, rect))
            {
                const ImVec2 minimum = DocumentToScreen({ rect.x, rect.y });
                const ImVec2 maximum = DocumentToScreen({ rect.x + rect.width, rect.y + rect.height });
                drawList->AddRect(minimum, maximum, kHoverOutline);
                if (const auto* element = m_renderer.FindElement(m_hoveredGuid))
                {
                    drawList->AddText(
                        { minimum.x, minimum.y - 15.0f },
                        kHoverOutline,
                        element->GetName().c_str());
                }
            }
        }

        const auto& selection = model.GetSelectedNodes();
        for (const auto& guid : selection)
        {
            Rect rect;
            if (!m_renderer.GetElementRect(guid, rect))
                continue;
            const ImVec2 minimum = DocumentToScreen({ rect.x, rect.y });
            const ImVec2 maximum = DocumentToScreen({ rect.x + rect.width, rect.y + rect.height });
            const bool primary = guid == model.GetSelectedNodeGuid();
            drawList->AddRect(
                minimum,
                maximum,
                primary ? kAccent : kAccentSoft,
                0.0f,
                0,
                primary ? 2.0f : 1.0f);
        }

        if (selection.size() == 1u && !selection.front().empty())
        {
            const auto guid = selection.front();
            Rect rect;
            if (m_renderer.GetElementRect(guid, rect))
            {
                const ImVec2 minimum = DocumentToScreen({ rect.x, rect.y });
                const ImVec2 maximum = DocumentToScreen({ rect.x + rect.width, rect.y + rect.height });

                if (CanManipulateParent(model, guid))
                {
                    const ImVec2 center {
                        (minimum.x + maximum.x) * 0.5f,
                        (minimum.y + maximum.y) * 0.5f
                    };
                    const ImVec2 points[8] = {
                        { minimum.x, minimum.y },
                        { center.x, minimum.y },
                        { maximum.x, minimum.y },
                        { minimum.x, center.y },
                        { maximum.x, center.y },
                        { minimum.x, maximum.y },
                        { center.x, maximum.y },
                        { maximum.x, maximum.y }
                    };
                    for (const auto& point : points)
                    {
                        drawList->AddRectFilled(
                            { point.x - kHandleScreenSize * 0.5f, point.y - kHandleScreenSize * 0.5f },
                            { point.x + kHandleScreenSize * 0.5f, point.y + kHandleScreenSize * 0.5f },
                            IM_COL32(234, 241, 255, 255));
                        drawList->AddRect(
                            { point.x - kHandleScreenSize * 0.5f, point.y - kHandleScreenSize * 0.5f },
                            { point.x + kHandleScreenSize * 0.5f, point.y + kHandleScreenSize * 0.5f },
                            kAccent);
                    }

                    // Pivot marker.
                    if (const auto* element = m_renderer.FindElement(guid))
                    {
                        const auto layout = element->GetLayout();
                        const ImVec2 pivot = DocumentToScreen({
                            rect.x + rect.width * layout.pivot.x,
                            rect.y + rect.height * layout.pivot.y
                        });
                        drawList->AddCircleFilled(pivot, 3.0f, kAccent);
                        drawList->AddCircle(pivot, 5.0f, IM_COL32(234, 241, 255, 200));
                    }
                }

                char sizeLabel[64] {};
                snprintf(
                    sizeLabel,
                    sizeof(sizeLabel),
                    "%d x %d",
                    static_cast<int>(std::round(rect.width)),
                    static_cast<int>(std::round(rect.height)));
                const ImVec2 labelSize = ImGui::CalcTextSize(sizeLabel);
                const ImVec2 badgeMin { maximum.x + 6.0f, maximum.y + 4.0f };
                const ImVec2 badgeMax {
                    badgeMin.x + labelSize.x + 8.0f,
                    badgeMin.y + labelSize.y + 4.0f
                };
                drawList->AddRectFilled(badgeMin, badgeMax, kBadgeBackground, 3.0f);
                drawList->AddRect(badgeMin, badgeMax, kBadgeBorder, 3.0f);
                drawList->AddText(
                    { badgeMin.x + 4.0f, badgeMin.y + 2.0f },
                    IM_COL32(127, 178, 255, 255),
                    sizeLabel);
            }
        }
        else if (selection.size() > 1u)
        {
            char label[64] {};
            snprintf(label, sizeof(label), "%zu elements", selection.size());
            drawList->AddText(
                { m_canvasOrigin.x + 10.0f, m_canvasOrigin.y + 24.0f },
                IM_COL32(127, 178, 255, 255),
                label);
        }

        const auto drawDiagnostics = [&](const std::vector<std::string>& nodes, const ImU32 color)
        {
            for (const auto& guid : nodes)
            {
                Rect rect;
                if (!m_renderer.GetElementRect(guid, rect))
                    continue;
                const ImVec2 minimum = DocumentToScreen({ rect.x, rect.y });
                const ImVec2 maximum = DocumentToScreen({ rect.x + rect.width, rect.y + rect.height });
                drawList->AddRect(minimum, maximum, color, 0.0f, 0, 1.0f);
                drawList->AddCircleFilled(
                    { minimum.x + 4.0f, minimum.y + 4.0f },
                    3.0f,
                    color);
            }
        };
        drawDiagnostics(options.errorNodes, kError);
        drawDiagnostics(options.warningNodes, kWarning);

        if (m_activeSnap.hasVerticalGuide)
        {
            const ImVec2 top = DocumentToScreen({ m_activeSnap.verticalGuide, 0.0f });
            const ImVec2 bottom = DocumentToScreen({
                m_activeSnap.verticalGuide,
                m_renderer.GetReferenceSize().y
            });
            AddDashedLine(*drawList, top, bottom, kGuide);
            char label[32] {};
            snprintf(label, sizeof(label), "%d px", static_cast<int>(std::round(std::abs(m_activeSnap.deltaX))));
            drawList->AddText({ top.x + 4.0f, top.y + 26.0f }, kGuide, label);
        }
        if (m_activeSnap.hasHorizontalGuide)
        {
            const ImVec2 left = DocumentToScreen({ 0.0f, m_activeSnap.horizontalGuide });
            const ImVec2 right = DocumentToScreen({
                m_renderer.GetReferenceSize().x,
                m_activeSnap.horizontalGuide
            });
            AddDashedLine(*drawList, left, right, kGuide);
            char label[32] {};
            snprintf(label, sizeof(label), "%d px", static_cast<int>(std::round(std::abs(m_activeSnap.deltaY))));
            drawList->AddText({ left.x + 26.0f, left.y + 4.0f }, kGuide, label);
        }

        if (m_interaction == Interaction::Marquee)
        {
            const ImVec2 start {
                std::min(m_marqueeStart.x, ImGui::GetIO().MousePos.x),
                std::min(m_marqueeStart.y, ImGui::GetIO().MousePos.y)
            };
            const ImVec2 end {
                std::max(m_marqueeStart.x, ImGui::GetIO().MousePos.x),
                std::max(m_marqueeStart.y, ImGui::GetIO().MousePos.y)
            };
            drawList->AddRectFilled(start, end, IM_COL32(76, 141, 255, 40));
            drawList->AddRect(start, end, kAccent);
        }
    }

    void UIDocumentCanvasView::DrawContextMenu(UIDocumentDesignerModel& model)
    {
        if (m_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        {
            const auto* hit = m_renderer.HitTest(m_mouseDocument);
            m_contextOnNode = hit != nullptr;
            m_contextNodeGuid = hit != nullptr ? hit->GetNodeGuid().ToString() : std::string {};
            if (hit != nullptr && !IsSelected(model, m_contextNodeGuid))
                model.SelectNode(m_contextNodeGuid);
            ImGui::OpenPopup("##UIDocumentCanvasContext");
        }

        if (!ImGui::BeginPopup("##UIDocumentCanvasContext"))
            return;

        if (m_contextOnNode && !m_contextNodeGuid.empty())
        {
            if (ImGui::MenuItem("Duplicate", "Ctrl+D"))
            {
                model.DuplicateNode(m_contextNodeGuid);
                RequestFrameSelection();
            }
            if (ImGui::MenuItem("Delete", "Del"))
                model.RemoveNode(m_contextNodeGuid);
            ImGui::Separator();
            if (ImGui::MenuItem("Bring Forward"))
                model.BringNodeForward(m_contextNodeGuid);
            if (ImGui::MenuItem("Send Backward"))
                model.SendNodeBackward(m_contextNodeGuid);
            if (ImGui::MenuItem("Bring to Front"))
                model.BringNodeToFront(m_contextNodeGuid);
            if (ImGui::MenuItem("Send to Back"))
                model.SendNodeToBack(m_contextNodeGuid);
            ImGui::Separator();
            const auto* node = model.FindNode(m_contextNodeGuid);
            const bool visible = node == nullptr ||
                node->value("style", Json::object()).value("visible", true);
            if (ImGui::MenuItem(visible ? "Hide" : "Show"))
                model.SetNodeVisible(m_contextNodeGuid, !visible);
            ImGui::Separator();
            if (ImGui::MenuItem("Copy"))
                CopySelection(model);
        }
        else
        {
            if (ImGui::MenuItem("Select All"))
            {
                std::vector<std::string> all;
                const auto* root = m_renderer.GetInstance() != nullptr
                    ? m_renderer.GetInstance()->GetRoot()
                    : nullptr;
                if (root != nullptr)
                {
                    const std::function<void(const NLS::Engine::UI::UIElement&)> visit =
                        [&](const NLS::Engine::UI::UIElement& element)
                        {
                            for (const auto& child : element.GetChildren())
                            {
                                if (child == nullptr)
                                    continue;
                                all.push_back(child->GetNodeGuid().ToString());
                                visit(*child);
                            }
                        };
                    visit(*root);
                }
                model.SetSelection(std::move(all));
            }
            if (ImGui::MenuItem("Paste", "Ctrl+V", false, HasClipboardContent()))
                PasteClipboard(model);
            ImGui::Separator();
            if (ImGui::MenuItem("Frame All", "F"))
                FrameAll(model);
        }
        ImGui::EndPopup();
    }

    // ---------------------------------------------------------------------
    // View helpers
    // ---------------------------------------------------------------------
    ImVec2 UIDocumentCanvasView::DocumentToScreen(const Vec2 point) const
    {
        return {
            m_canvasOrigin.x + m_view.pan.x + point.x * m_view.zoom,
            m_canvasOrigin.y + m_view.pan.y + point.y * m_view.zoom
        };
    }

    Vec2 UIDocumentCanvasView::ScreenToDocument(const ImVec2 point) const
    {
        return {
            (point.x - m_canvasOrigin.x - m_view.pan.x) / std::max(0.0001f, m_view.zoom),
            (point.y - m_canvasOrigin.y - m_view.pan.y) / std::max(0.0001f, m_view.zoom)
        };
    }

    bool UIDocumentCanvasView::CanManipulateParent(
        const UIDocumentDesignerModel& model,
        const std::string_view nodeGuid) const
    {
        const auto parentGuid = model.GetNodeParentGuid(nodeGuid);
        if (parentGuid.empty())
            return false;
        const auto* parent = model.FindNode(parentGuid);
        if (parent == nullptr)
            return false;
        const auto mode = parent->value("layout", Json::object()).value("mode", std::string("Absolute"));
        return mode == "Absolute" || mode == "Overlay" || mode == "Scroll";
    }

    bool UIDocumentCanvasView::IsSelected(
        const UIDocumentDesignerModel& model,
        const std::string_view nodeGuid) const
    {
        const auto& selection = model.GetSelectedNodes();
        return std::find(selection.begin(), selection.end(), nodeGuid) != selection.end();
    }

    void UIDocumentCanvasView::FrameAll(const UIDocumentDesignerModel& model)
    {
        const auto referenceSize = m_renderer.GetReferenceSize();
        const float margin = 72.0f;
        const float width = std::max(1.0f, referenceSize.x);
        const float height = std::max(1.0f, referenceSize.y);
        m_view.zoom = std::clamp(
            std::min(
                (m_canvasSize.x - margin) / width,
                (m_canvasSize.y - margin) / height),
            kMinZoom,
            kMaxZoom);
        m_view.pan = {
            (m_canvasSize.x - width * m_view.zoom) * 0.5f,
            (m_canvasSize.y - height * m_view.zoom) * 0.5f
        };
        (void)model;
    }

    void UIDocumentCanvasView::FrameNode(
        const UIDocumentDesignerModel& model,
        const std::string_view nodeGuid)
    {
        Rect rect;
        if (!m_renderer.GetElementRect(nodeGuid, rect))
        {
            FrameAll(model);
            return;
        }
        const float margin = 96.0f;
        m_view.zoom = std::clamp(
            std::min(
                (m_canvasSize.x - margin) / std::max(1.0f, rect.width),
                (m_canvasSize.y - margin) / std::max(1.0f, rect.height)),
            kMinZoom,
            kMaxZoom);
        m_view.pan = {
            m_canvasSize.x * 0.5f - (rect.x + rect.width * 0.5f) * m_view.zoom,
            m_canvasSize.y * 0.5f - (rect.y + rect.height * 0.5f) * m_view.zoom
        };
    }

    void UIDocumentCanvasView::SetZoom(
        const UIDocumentDesignerModel& model,
        const float zoom)
    {
        const ImVec2 center {
            m_canvasSize.x * 0.5f,
            m_canvasSize.y * 0.5f
        };
        const Vec2 documentCenter {
            (center.x - m_view.pan.x) / std::max(0.0001f, m_view.zoom),
            (center.y - m_view.pan.y) / std::max(0.0001f, m_view.zoom)
        };
        m_view.zoom = std::clamp(zoom, kMinZoom, kMaxZoom);
        m_view.pan = {
            center.x - documentCenter.x * m_view.zoom,
            center.y - documentCenter.y * m_view.zoom
        };
        (void)model;
    }

    void UIDocumentCanvasView::ZoomIn(const UIDocumentDesignerModel& model)
    {
        SetZoom(model, m_view.zoom * 1.15f);
    }

    void UIDocumentCanvasView::ZoomOut(const UIDocumentDesignerModel& model)
    {
        SetZoom(model, m_view.zoom / 1.15f);
    }

    void UIDocumentCanvasView::ApplyDevicePreset(
        UIDocumentDesignerModel& model,
        const int presetIndex)
    {
        if (presetIndex < 0 || presetIndex >= static_cast<int>(GetDevicePresetCount()))
            return;
        const auto size = GetDevicePresetSize(presetIndex);
        model.SetNodeProperty(
            model.GetRootGuid(),
            "layout.size",
            Json::array({ size.x, size.y }),
            true,
            "Change UI Reference Resolution");
        FrameAll(model);
    }

    // ---------------------------------------------------------------------
    // Shared commands
    // ---------------------------------------------------------------------
    bool UIDocumentCanvasView::HasClipboardContent()
    {
        return !g_clipboard.empty();
    }

    bool UIDocumentCanvasView::CopySelection(const UIDocumentDesignerModel& model)
    {
        const auto& selection = model.GetSelectedNodes();
        if (selection.empty())
            return false;
        g_clipboard = model.SerializeNode(selection.front());
        return !g_clipboard.empty();
    }

    bool UIDocumentCanvasView::PasteClipboard(UIDocumentDesignerModel& model)
    {
        if (g_clipboard.empty())
            return false;

        const auto primary = model.GetSelectedNodeGuid();
        std::string parentGuid;
        size_t index = 0u;
        if (primary.empty())
        {
            parentGuid = model.GetRootGuid();
            index = model.GetChildCount(parentGuid);
        }
        else if (!model.TryGetNodeLocation(primary, parentGuid, index))
        {
            parentGuid = primary;
            index = model.GetChildCount(primary);
        }
        else
        {
            ++index;
        }

        if (!model.PasteNode(parentGuid, index, g_clipboard))
            return false;
        RequestFrameSelection();
        return true;
    }

    bool UIDocumentCanvasView::DuplicateSelection(UIDocumentDesignerModel& model)
    {
        const auto selection = model.GetSelectedNodes();
        if (selection.empty())
            return false;
        if (selection.size() == 1u)
        {
            const bool duplicated = model.DuplicateNode(selection.front());
            if (duplicated)
                RequestFrameSelection();
            return duplicated;
        }

        model.BeginTransaction("Duplicate UI Elements");
        bool changed = false;
        for (const auto& guid : selection)
        {
            if (model.IsRootNode(guid))
                continue;
            changed |= model.DuplicateNode(guid, false);
        }
        model.EndTransaction();
        return changed;
    }

    bool UIDocumentCanvasView::DeleteSelection(UIDocumentDesignerModel& model)
    {
        const auto selection = model.GetSelectedNodes();
        if (selection.empty())
            return false;

        model.BeginTransaction("Delete UI Elements");
        bool changed = false;
        for (const auto& guid : selection)
        {
            if (model.IsRootNode(guid))
                continue;
            changed |= model.RemoveNode(guid);
        }
        model.EndTransaction();
        return changed;
    }

    bool UIDocumentCanvasView::NudgeSelection(
        UIDocumentDesignerModel& model,
        const float deltaX,
        const float deltaY)
    {
        const auto selection = model.GetSelectedNodes();
        if (selection.empty())
            return false;

        model.BeginTransaction("Nudge UI Elements");
        bool changed = false;
        for (const auto& guid : selection)
        {
            if (model.IsRootNode(guid))
                continue;
            changed |= model.NudgeNode(guid, deltaX, deltaY);
        }
        model.EndTransaction();
        return changed;
    }
}
