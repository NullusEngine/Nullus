#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

#include <imgui.h>
#include <UI/Panels/PanelWindow.h>

#include "UI/UIDocumentDesignerModel.h"

namespace NLS::Editor::Panels
{
    class UIDocumentDesigner final : public NLS::UI::PanelWindow
    {
    public:
        UIDocumentDesigner(
            const std::string& title,
            bool opened,
            const NLS::UI::PanelWindowSettings& windowSettings);

        bool OpenDocument(const std::filesystem::path& path);
        [[nodiscard]] const std::filesystem::path& GetDocumentPath() const { return m_model.GetPath(); }

    protected:
        void OnBeforeDrawWidgets() override;

    private:
        void DrawToolbar();
        void DrawControls();
        void DrawHierarchy();
        void DrawNodeTree(const NLS::Editor::UIDocumentTools::UIDocumentDesignerModel::Json& node);
        void DrawCanvasPreview();
        void DrawInspector();
        void DrawDiagnostics();
        void RefreshEditBuffers();
        void NotifyDocumentSaved();

        static void DrawRuntimeElement(
            const NLS::Engine::UI::UIElement& element,
            ImDrawList& drawList,
            ImVec2 origin,
            float scale,
            const std::string& selectedNodeGuid);
        static const NLS::Engine::UI::UIElement* FindRuntimeElementAt(
            const NLS::Engine::UI::UIElement& element,
            NLS::Engine::UI::UIVector2 point);

        NLS::Editor::UIDocumentTools::UIDocumentDesignerModel m_model;
        std::array<char, 256u> m_nameBuffer {};
        std::array<char, 1024u> m_textBuffer {};
        std::string m_buffersNodeGuid;
        uint64_t m_buffersDocumentRevision = 0u;
    };
}
