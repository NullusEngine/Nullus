#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <imgui.h>
#include <UI/Panels/PanelWindow.h>

#include "UI/UIDocumentCanvasView.h"
#include "UI/UIDocumentDesignerModel.h"
#include "UI/UIDocumentHierarchyView.h"
#include "UI/UIDocumentInspectorView.h"

namespace NLS::Editor::Panels
{
    class UIDocumentDesigner final : public NLS::UI::PanelWindow
    {
    public:
        UIDocumentDesigner(
            const std::string& title,
            bool opened,
            const NLS::UI::PanelWindowSettings& windowSettings);

        // Loads a document. When the current document has unsaved changes the
        // open request is deferred behind a Save / Discard / Cancel dialog.
        bool OpenDocument(const std::filesystem::path& path);
        [[nodiscard]] const std::filesystem::path& GetDocumentPath() const
        {
            return m_model.GetPath();
        }
        [[nodiscard]] bool HasDocument() const { return m_model.IsLoaded(); }
        // Lets the global shortcut dispatcher yield while this panel is focused.
        [[nodiscard]] bool OwnsShortcuts() const;

        // Editor command entry points shared by the toolbar, shortcuts and tests.
        bool ExecuteCommand(const std::string& id);
        [[nodiscard]] bool IsCommandEnabled(const std::string& id) const;

    protected:
        void OnBeforeDrawWidgets() override;

    private:
        enum class PendingAction
        {
            None,
            OpenPath,
            Reload
        };

        struct PendingRequest
        {
            PendingAction action = PendingAction::None;
            std::filesystem::path path;
        };

        void DrawToolbar();
        void DrawLeftColumn();
        void DrawPalette();
        void DrawDiagnostics();
        void DrawPendingDialog();
        void DrawExternalChangeBanner();
        void HandleShortcuts();
        void PollExternalChanges();
        void UpdateDiagnosticNodes();
        void SaveDocument();
        void ReloadDocument();
        bool LoadDocument(const std::filesystem::path& path);
        void ResolvePending(PendingAction action, const std::filesystem::path& path);
        [[nodiscard]] bool CanSave() const;

        UIDocumentTools::UIDocumentDesignerModel m_model;
        UIDocumentTools::UIDocumentCanvasView m_canvas;
        UIDocumentTools::UIDocumentHierarchyView m_hierarchy;
        UIDocumentTools::UIDocumentInspectorView m_inspector;

        float m_leftWidth = 250.0f;
        float m_rightWidth = 340.0f;
        std::array<char, 64> m_paletteFilter {};
        std::vector<std::string> m_errorNodes;
        std::vector<std::string> m_warningNodes;
        uint64_t m_lastDiagnosticRevision = 0u;

        bool m_shortcutFocus = false;
        bool m_openPendingDialog = false;
        bool m_pendingSaveFailed = false;
        PendingRequest m_pending;

        std::filesystem::file_time_type m_lastWriteTime {};
        bool m_hasLastWriteTime = false;
        bool m_externalChange = false;
        bool m_externalConflict = false;
        double m_nextExternalCheckTime = 0.0;
    };
}
