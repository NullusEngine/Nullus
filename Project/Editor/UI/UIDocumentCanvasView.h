#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <imgui.h>

#include "Engine/UI/UICommon.h"
#include "UI/UIDocumentCanvasRenderer.h"
#include "UI/UIDocumentDesignerModel.h"

namespace NLS::Editor::UIDocumentTools
{
    inline constexpr const char* kUIDesignerPalettePayload = "NLS_UI_DESIGNER_ELEMENT";
    inline constexpr const char* kUIDesignerNodePayload = "NLS_UI_DESIGNER_NODE";

    // Document-space gesture math, independent from ImGui so it can be unit
    // tested without a frame.
    struct UIDocumentCanvasGeometry
    {
        enum class Handle : uint8_t
        {
            None,
            Left,
            Right,
            Top,
            Bottom,
            TopLeft,
            TopRight,
            BottomLeft,
            BottomRight
        };

        struct ResizeResult
        {
            NLS::Engine::UI::UIVector2 size;
            NLS::Engine::UI::UIVector2 offsetMin;
            NLS::Engine::UI::UIVector2 offsetMax;
            bool changed = false;
        };

        struct SnapResult
        {
            float deltaX = 0.0f;
            float deltaY = 0.0f;
            bool hasVerticalGuide = false;
            float verticalGuide = 0.0f;
            bool hasHorizontalGuide = false;
            float horizontalGuide = 0.0f;
        };

        // Resize semantics follow UIElement::Arrange:
        //   left = anchor + offsetMin - size * pivot
        // Non-stretched axes change `size` and keep the opposite edge fixed by
        // shifting offsetMin; stretched axes move their offset edge directly.
        [[nodiscard]] static ResizeResult Resize(
            const NLS::Engine::UI::UILayoutParams& layout,
            const NLS::Engine::UI::UIRect& rect,
            Handle handle,
            NLS::Engine::UI::UIVector2 delta,
            bool lockAspect,
            float gridSize = 0.0f);

        [[nodiscard]] static SnapResult Snap(
            const NLS::Engine::UI::UIRect& movingRect,
            const std::vector<NLS::Engine::UI::UIRect>& targets,
            float threshold);

        [[nodiscard]] static Handle HitTestHandle(
            const NLS::Engine::UI::UIRect& rect,
            NLS::Engine::UI::UIVector2 point,
            float halfSize);
    };

    class UIDocumentCanvasView final
    {
    public:
        struct Options
        {
            bool showGrid = true;
            bool snapToGrid = true;
            bool snapToGuides = true;
            float gridSize = 8.0f;
            bool wireframe = false;
            bool showHiddenElements = false;
            bool showRulers = true;
            bool allowEditing = true;
            std::vector<std::string> errorNodes;
            std::vector<std::string> warningNodes;
        };

        struct ViewState
        {
            float zoom = 1.0f;
            ImVec2 pan { 0.0f, 0.0f };
        };

        UIDocumentCanvasView();
        ~UIDocumentCanvasView();

        void Draw(UIDocumentDesignerModel& model, const Options& options);

        [[nodiscard]] ViewState& GetViewState() { return m_view; }
        [[nodiscard]] const ViewState& GetViewState() const { return m_view; }
        [[nodiscard]] UIDocumentCanvasRenderer& GetRenderer() { return m_renderer; }
        [[nodiscard]] const UIDocumentCanvasRenderer& GetRenderer() const { return m_renderer; }
        [[nodiscard]] bool IsHovered() const { return m_hovered; }
        [[nodiscard]] bool IsInteracting() const { return m_interaction != Interaction::None; }
        [[nodiscard]] bool HasActiveGesture() const { return m_gestureSessionOpen; }
        [[nodiscard]] float GetZoom() const { return m_view.zoom; }
        // Screen-space position of the canvas content origin from the last
        // Draw. Exposed for editor scripting and interaction tests.
        [[nodiscard]] ImVec2 GetCanvasOrigin() const { return m_canvasOrigin; }

        void FrameAll(const UIDocumentDesignerModel& model);
        void FrameNode(const UIDocumentDesignerModel& model, std::string_view nodeGuid);
        void RequestFrameSelection() { m_requestFrameSelection = true; }
        void RequestFrameAll() { m_requestFrameAll = true; }
        void SetZoom(const UIDocumentDesignerModel& model, float zoom);
        void ZoomIn(const UIDocumentDesignerModel& model);
        void ZoomOut(const UIDocumentDesignerModel& model);
        void ApplyDevicePreset(UIDocumentDesignerModel& model, int presetIndex);
        void CancelGesture(UIDocumentDesignerModel& model);

        // Shared command entry points for shortcuts and context menus.
        bool CopySelection(const UIDocumentDesignerModel& model);
        bool PasteClipboard(UIDocumentDesignerModel& model);
        bool DuplicateSelection(UIDocumentDesignerModel& model);
        bool DeleteSelection(UIDocumentDesignerModel& model);
        bool NudgeSelection(UIDocumentDesignerModel& model, float deltaX, float deltaY);
        static bool HasClipboardContent();

        static size_t GetDevicePresetCount();
        static const char* GetDevicePresetName(int presetIndex);
        static NLS::Engine::UI::UIVector2 GetDevicePresetSize(int presetIndex);

    private:
        enum class Interaction : uint8_t
        {
            None,
            PressPending,
            Move,
            Resize,
            Marquee,
            Pan
        };

        void HandleInput(UIDocumentDesignerModel& model, const Options& options);
        void BeginMove(UIDocumentDesignerModel& model);
        void UpdateMove(UIDocumentDesignerModel& model, const Options& options);
        void UpdateResize(UIDocumentDesignerModel& model, const Options& options);
        void FinishInteraction(UIDocumentDesignerModel& model, bool cancelled);
        void CollectSnapTargets(
            const UIDocumentDesignerModel& model,
            std::string_view movingNodeGuid,
            std::vector<NLS::Engine::UI::UIRect>& targets) const;
        void CollectElementsUnderMarquee(
            const UIDocumentDesignerModel& model,
            const NLS::Engine::UI::UIRect& marquee,
            const Options& options,
            std::vector<std::string>& selected) const;

        void DrawRulers() const;
        void DrawOverlays(const UIDocumentDesignerModel& model, const Options& options) const;
        void DrawContextMenu(UIDocumentDesignerModel& model);

        [[nodiscard]] ImVec2 DocumentToScreen(NLS::Engine::UI::UIVector2 point) const;
        [[nodiscard]] NLS::Engine::UI::UIVector2 ScreenToDocument(ImVec2 point) const;
        [[nodiscard]] bool CanManipulateParent(
            const UIDocumentDesignerModel& model,
            std::string_view nodeGuid) const;
        [[nodiscard]] bool IsSelected(
            const UIDocumentDesignerModel& model,
            std::string_view nodeGuid) const;

        UIDocumentCanvasRenderer m_renderer;
        ViewState m_view;
        Interaction m_interaction = Interaction::None;
        bool m_hovered = false;
        bool m_gestureSessionOpen = false;
        bool m_referenceInitialized = false;
        bool m_pressOnEmpty = false;

        ImVec2 m_canvasOrigin {};
        ImVec2 m_canvasSize {};
        ImVec2 m_gesturePressMouse {};
        ImVec2 m_panStart {};
        ImVec2 m_marqueeStart {};
        NLS::Engine::UI::UIVector2 m_mouseDocument {};

        std::string m_hoveredGuid;
        std::string m_gestureGuid;
        std::string m_contextNodeGuid;
        bool m_contextOnNode = false;
        UIDocumentCanvasGeometry::Handle m_gestureHandle = UIDocumentCanvasGeometry::Handle::None;
        NLS::Engine::UI::UIRect m_gestureStartRect {};
        NLS::Engine::UI::UILayoutParams m_gestureStartLayout {};
        NLS::Engine::UI::UIVector2 m_gestureStartOffsetMin {};
        NLS::Engine::UI::UIVector2 m_gestureStartOffsetMax {};
        UIDocumentCanvasGeometry::SnapResult m_activeSnap;
        std::vector<std::string> m_marqueeBaseSelection;
        bool m_marqueeAdd = false;
        bool m_marqueeToggle = false;

        bool m_requestFrameAll = false;
        bool m_requestFrameSelection = false;
    };
}
