#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <Json/json.hpp>

#include "Engine/UI/UIDocument.h"

namespace NLS::Editor::UIDocumentTools
{
    struct UIDocumentDesignerDiagnostic
    {
        std::string path;
        std::string message;
        bool error = true;
    };

    // Owns the editable JSON source used by the Editor Designer. Runtime UI
    // always receives the validated UIDocument representation instead.
    class UIDocumentDesignerModel final
    {
    public:
        using Json = nlohmann::json;

        // A newly created document deliberately has only its required root.
        // UI content is added through the Designer's control library.
        static std::string CreateDocumentSource(std::string_view documentName);

        bool Load(const std::filesystem::path& path);
        bool Save();
        bool Reload();

        [[nodiscard]] bool IsLoaded() const { return !m_path.empty(); }
        [[nodiscard]] bool IsDirty() const { return m_dirty; }
        [[nodiscard]] const std::filesystem::path& GetPath() const { return m_path; }
        [[nodiscard]] const Json& GetDocument() const { return m_document; }
        [[nodiscard]] const std::string& GetSelectedNodeGuid() const { return m_selectedNodeGuid; }
        [[nodiscard]] const std::vector<UIDocumentDesignerDiagnostic>& GetDiagnostics() const
        {
            return m_diagnostics;
        }

        [[nodiscard]] const Json* FindNode(std::string_view nodeGuid) const;
        [[nodiscard]] Json* FindNode(std::string_view nodeGuid);
        [[nodiscard]] const Json* GetSelectedNode() const;
        void SelectNode(std::string nodeGuid);

        bool AddChild(std::string_view parentNodeGuid, std::string_view type);
        bool RemoveNode(std::string_view nodeGuid);
        bool SetNodeName(std::string_view nodeGuid, std::string value, bool recordUndo = true);
        bool SetNodeText(std::string_view nodeGuid, std::string value, bool recordUndo = true);
        bool SetNodeLayoutSize(std::string_view nodeGuid, float width, float height, bool recordUndo = true);
        bool SetNodeLayoutMode(std::string_view nodeGuid, std::string_view mode, bool recordUndo = true);
        bool SetNodeSpacing(std::string_view nodeGuid, float spacing, bool recordUndo = true);
        bool SetNodeBackgroundColor(
            std::string_view nodeGuid,
            float red,
            float green,
            float blue,
            float alpha,
            bool recordUndo = true);

        void BeginTransaction();
        void EndTransaction();
        [[nodiscard]] bool CanUndo() const { return !m_undo.empty(); }
        [[nodiscard]] bool CanRedo() const { return !m_redo.empty(); }
        bool Undo();
        bool Redo();

        [[nodiscard]] bool BuildRuntimeDocument(
            NLS::Engine::UI::UIDocument& document,
            std::vector<NLS::Engine::UI::UIDocumentDiagnostic>* diagnostics = nullptr) const;

    private:
        static Json CreateNode(std::string_view type, std::string_view name);
        static Json CreateEmptyDocument(std::string_view documentName);
        static Json* FindNodeRecursive(Json& node, std::string_view nodeGuid);
        static const Json* FindNodeRecursive(const Json& node, std::string_view nodeGuid);
        static Json* FindParentRecursive(Json& node, std::string_view childNodeGuid);
        static bool EnsureStableIdentifiers(Json& document);
        static bool IsSupportedElementType(std::string_view type);
        static bool IsSupportedLayoutMode(std::string_view mode);
        static bool WriteTextFileAtomically(const std::filesystem::path& path, const std::string& text);

        void RecordUndo();
        void MarkChanged(bool recordUndo);
        void Revalidate();
        void Restore(Json document);

        std::filesystem::path m_path;
        Json m_document;
        std::string m_selectedNodeGuid;
        std::vector<UIDocumentDesignerDiagnostic> m_diagnostics;
        std::vector<Json> m_undo;
        std::vector<Json> m_redo;
        Json m_transactionBefore;
        bool m_transactionActive = false;
        bool m_dirty = false;
    };
}
