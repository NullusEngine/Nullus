#pragma once

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

#include <imgui.h>

#include "Engine/UI/UIElement.h"
#include "UI/UIDocumentDesignerModel.h"

namespace NLS::Editor::UIDocumentTools
{
    // Caches the runtime instance of one designer document and converts its
    // retained-mode UIDrawSnapshot into ImGui draw-list geometry.
    //
    // The document JSON stays the single source of truth. This class only
    // mirrors it for layout, picking and WYSIWYG painting; a changed document
    // revision rebuilds the instance, an unchanged one reuses both the element
    // tree and the runtime snapshot cache.
    class UIDocumentCanvasRenderer final
    {
    public:
        struct RenderParameters
        {
            ImVec2 origin {};
            float scale = 1.0f;
            ImVec2 clipMin {};
            ImVec2 clipMax {};
            uint64_t frameId = 0u;
        };

        UIDocumentCanvasRenderer();
        ~UIDocumentCanvasRenderer();

        UIDocumentCanvasRenderer(const UIDocumentCanvasRenderer&) = delete;
        UIDocumentCanvasRenderer& operator=(const UIDocumentCanvasRenderer&) = delete;

        // Rebuilds the instance when the document revision or path changed.
        // Returns false while the document has validation errors.
        bool Sync(const UIDocumentDesignerModel& model);
        void Invalidate();

        [[nodiscard]] bool IsValid() const { return m_valid; }
        [[nodiscard]] const NLS::Engine::UI::UIDocumentInstance* GetInstance() const
        {
            return m_instance.get();
        }
        [[nodiscard]] const NLS::Engine::UI::UIDocument* GetDocument() const { return m_document.get(); }
        [[nodiscard]] NLS::Engine::UI::UIVector2 GetReferenceSize() const { return m_referenceSize; }
        [[nodiscard]] uint32_t GetLastDrawBatchCount() const { return m_lastBatchCount; }

        // Appends the document's geometry to the draw list. Solid-color batches
        // use ImGui's white pixel; textured batches use the packed runtime UI
        // texture id so glyph quads sample the same atlas as the Game View.
        bool Render(
            const UIDocumentDesignerModel& model,
            ImDrawList& drawList,
            const RenderParameters& parameters);

        [[nodiscard]] bool GetElementRect(
            std::string_view nodeGuid,
            NLS::Engine::UI::UIRect& rect) const;
        // Element rect inset by its padding; useful for anchors and drops.
        [[nodiscard]] bool GetElementContentRect(
            std::string_view nodeGuid,
            NLS::Engine::UI::UIRect& rect) const;
        [[nodiscard]] const NLS::Engine::UI::UIElement* FindElement(std::string_view nodeGuid) const;
        // Deepest visible element under a document-space point. Design-time
        // picking ignores the runtime interactive flag.
        [[nodiscard]] const NLS::Engine::UI::UIElement* HitTest(
            NLS::Engine::UI::UIVector2 point) const;

    private:
        static const NLS::Engine::UI::UIElement* HitTestRecursive(
            const NLS::Engine::UI::UIElement& element,
            NLS::Engine::UI::UIVector2 point);

        std::unique_ptr<NLS::Engine::UI::UIDocument> m_document;
        std::unique_ptr<NLS::Engine::UI::UIDocumentInstance> m_instance;
        uint64_t m_syncedRevision = (std::numeric_limits<uint64_t>::max)();
        std::string m_syncedPath;
        NLS::Engine::UI::UIVector2 m_referenceSize { 1920.0f, 1080.0f };
        bool m_valid = false;
        uint32_t m_lastBatchCount = 0u;
    };
}
