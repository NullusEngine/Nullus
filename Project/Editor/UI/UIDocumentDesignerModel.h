#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <Json/json.hpp>

#include "Engine/UI/UIDocument.h"
#include "Undo/EditorUndoService.h"

namespace NLS::Editor::UIDocumentTools
{
    struct UIDocumentDesignerDiagnostic
    {
        std::string path;
        std::string message;
        bool error = true;
    };

    namespace Detail
    {
        // Kept independently of the panel/model object so global history
        // commands can weakly resolve a live document without retaining a
        // raw model pointer across frames.
        struct UIDocumentDesignerState
        {
            std::filesystem::path path;
            std::string canonicalPath;
            nlohmann::json document;
            std::string selectedNodeGuid;
            std::vector<UIDocumentDesignerDiagnostic> diagnostics;
            uint64_t generation = 0u;
            uint64_t documentRevision = 0u;
            bool localDirty = false;
            bool requiresSave = false;
        };

        struct UIDocumentDesignerEditState
        {
            std::weak_ptr<UIDocumentDesignerState> target;
            NLS::Editor::Undo::UndoScopeId scope;
            nlohmann::json beforeDocument;
            std::string beforeSelection;
            bool beforeLocalDirty = false;
            nlohmann::json afterDocument;
            std::string afterSelection;
            bool afterLocalDirty = false;
        };
    }

    // Owns the editable JSON source used by the Editor Designer. Runtime UI
    // always receives the validated UIDocument representation instead.
    class UIDocumentDesignerModel final
    {
    public:
        using Json = nlohmann::json;

        UIDocumentDesignerModel();
        ~UIDocumentDesignerModel();

        UIDocumentDesignerModel(const UIDocumentDesignerModel&) = delete;
        UIDocumentDesignerModel& operator=(const UIDocumentDesignerModel&) = delete;
        UIDocumentDesignerModel(UIDocumentDesignerModel&&) = delete;
        UIDocumentDesignerModel& operator=(UIDocumentDesignerModel&&) = delete;

        // A newly created document deliberately has only its required root.
        // UI content is added through the Designer's control library.
        static std::string CreateDocumentSource(std::string_view documentName);

        bool Load(const std::filesystem::path& path);
        bool Save();
        bool Reload();

        [[nodiscard]] bool IsLoaded() const { return !m_state->path.empty(); }
        [[nodiscard]] bool IsDirty() const;
        [[nodiscard]] const std::filesystem::path& GetPath() const { return m_state->path; }
        [[nodiscard]] const Json& GetDocument() const { return m_state->document; }
        [[nodiscard]] const std::string& GetSelectedNodeGuid() const { return m_state->selectedNodeGuid; }
        [[nodiscard]] const std::vector<UIDocumentDesignerDiagnostic>& GetDiagnostics() const
        {
            return m_state->diagnostics;
        }
        [[nodiscard]] uint64_t GetDocumentRevision() const { return m_state->documentRevision; }
        [[nodiscard]] NLS::Editor::Undo::UndoScopeId GetUndoScope() const;

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

        void BeginTransaction(std::string label = "Edit UI Document");
        void EndTransaction();

        [[nodiscard]] bool BuildRuntimeDocument(
            NLS::Engine::UI::UIDocument& document,
            std::vector<NLS::Engine::UI::UIDocumentDiagnostic>* diagnostics = nullptr) const;

    private:
        struct DocumentSnapshot
        {
            Json document;
            std::string selectedNodeGuid;
            bool localDirty = false;
            uint64_t documentRevision = 0u;
        };

        struct ActiveTransaction
        {
            std::shared_ptr<Detail::UIDocumentDesignerEditState> edit;
            std::optional<NLS::Editor::Undo::UndoEditSession> session;
        };

        static Json CreateNode(std::string_view type, std::string_view name);
        static Json CreateEmptyDocument(std::string_view documentName);
        static Json* FindNodeRecursive(Json& node, std::string_view nodeGuid);
        static const Json* FindNodeRecursive(const Json& node, std::string_view nodeGuid);
        static Json* FindParentRecursive(Json& node, std::string_view childNodeGuid);
        static bool EnsureStableIdentifiers(Json& document);
        static bool IsSupportedElementType(std::string_view type);
        static bool IsSupportedLayoutMode(std::string_view mode);
        static bool WriteTextFileAtomically(const std::filesystem::path& path, const std::string& text);

        [[nodiscard]] DocumentSnapshot CaptureSnapshot() const;
        [[nodiscard]] bool FinishChange(
            DocumentSnapshot before,
            std::chrono::steady_clock::time_point mergeStartedAt,
            bool recordUndo,
            std::string label,
            std::string affected,
            std::string mergeKey = {},
            bool allowMerge = false);
        bool CommitSnapshotChange(
            DocumentSnapshot before,
            DocumentSnapshot after,
            std::string label,
            std::string affected,
            std::string mergeKey,
            bool allowMerge,
            std::chrono::steady_clock::time_point mergeStartedAt);
        [[nodiscard]] bool UpdateActiveEdit();
        void CancelActiveTransaction();
        void RestoreSnapshot(const DocumentSnapshot& snapshot);
        void Revalidate();

        std::shared_ptr<Detail::UIDocumentDesignerState> m_state;
        std::optional<ActiveTransaction> m_activeTransaction;
    };
}
