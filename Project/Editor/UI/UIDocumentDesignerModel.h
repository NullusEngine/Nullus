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
            // Primary selection: the first entry of selectedNodeGuids. Kept as
            // its own member because existing callers and history snapshots
            // treat the selection as a single node.
            std::string selectedNodeGuid;
            std::vector<std::string> selectedNodeGuids;
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
            std::vector<std::string> beforeSelection;
            bool beforeLocalDirty = false;
            nlohmann::json afterDocument;
            std::vector<std::string> afterSelection;
            bool afterLocalDirty = false;
        };
    }

    // Owns the editable JSON source used by the Editor Designer. Runtime UI
    // always receives the validated UIDocument representation instead.
    //
    // The JSON document is the single source of truth for every edit. Views
    // (canvas, hierarchy, inspector) never mutate it directly: they issue
    // property/structure commands here so a gesture, a rename and a drag all
    // end up as one global undo command with the same semantics as the
    // existing setters.
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
        [[nodiscard]] const std::vector<std::string>& GetSelectedNodes() const
        {
            return m_state->selectedNodeGuids;
        }
        [[nodiscard]] const std::vector<UIDocumentDesignerDiagnostic>& GetDiagnostics() const
        {
            return m_state->diagnostics;
        }
        [[nodiscard]] bool HasErrors() const;
        [[nodiscard]] bool HasWarnings() const;
        [[nodiscard]] uint64_t GetDocumentRevision() const { return m_state->documentRevision; }
        [[nodiscard]] NLS::Editor::Undo::UndoScopeId GetUndoScope() const;
        [[nodiscard]] std::string GetRootGuid() const;

        // ---- Node queries -------------------------------------------------
        [[nodiscard]] const Json* FindNode(std::string_view nodeGuid) const;
        [[nodiscard]] Json* FindNode(std::string_view nodeGuid);
        [[nodiscard]] const Json* GetSelectedNode() const;
        [[nodiscard]] bool IsRootNode(std::string_view nodeGuid) const;
        [[nodiscard]] std::string GetNodeParentGuid(std::string_view nodeGuid) const;
        [[nodiscard]] bool TryGetNodeLocation(
            std::string_view nodeGuid,
            std::string& parentNodeGuid,
            size_t& index) const;
        [[nodiscard]] bool IsNodeInSubtree(
            std::string_view nodeGuid,
            std::string_view candidateDescendant) const;
        [[nodiscard]] size_t GetChildCount(std::string_view nodeGuid) const;
        // Deepest node that the JSON diagnostic path points at. Used to turn
        // "$.root.children[0].children[2].layout.size" back into a selection.
        [[nodiscard]] const Json* FindNodeByDiagnosticPath(std::string_view diagnosticPath) const;
        // Path of a node inside the document, matching the runtime
        // diagnostic format ($.root.children[0]...).
        [[nodiscard]] std::string BuildDiagnosticPath(std::string_view nodeGuid) const;

        // ---- Selection ----------------------------------------------------
        void SelectNode(std::string nodeGuid);
        void AddToSelection(std::string nodeGuid);
        void ToggleSelection(std::string nodeGuid);
        void SetSelection(std::vector<std::string> nodeGuids);
        void ClearSelection();

        // ---- Property editing ---------------------------------------------
        static bool IsSupportedPropertyPath(std::string_view path);
        [[nodiscard]] const Json* GetNodeProperty(std::string_view nodeGuid, std::string_view path) const;
        // Paths are validated against the runtime schema and values are
        // normalized (clamped, type-checked) before they reach the document.
        bool SetNodeProperty(
            std::string_view nodeGuid,
            std::string_view path,
            Json value,
            bool recordUndo = true,
            std::string label = {},
            std::string mergeKey = {},
            bool allowMerge = false);
        bool SetNodePropertyOnSelection(
            std::string_view path,
            Json value,
            bool recordUndo = true,
            std::string label = {},
            std::string mergeKey = {},
            bool allowMerge = false);
        bool SetNodeVector2(std::string_view nodeGuid, std::string_view path, float x, float y, bool recordUndo = true);
        bool SetNodeFloat(std::string_view nodeGuid, std::string_view path, float value, bool recordUndo = true);
        bool SetNodeUInt(std::string_view nodeGuid, std::string_view path, uint32_t value, bool recordUndo = true);
        bool SetNodeBool(std::string_view nodeGuid, std::string_view path, bool value, bool recordUndo = true);
        bool SetNodeColor(
            std::string_view nodeGuid,
            std::string_view path,
            float red,
            float green,
            float blue,
            float alpha,
            bool recordUndo = true);
        bool SetNodeString(std::string_view nodeGuid, std::string_view path, std::string value, bool recordUndo = true);
        bool SetNodeFont(
            std::string_view nodeGuid,
            uint64_t fontAssetId,
            uint16_t pixelSize,
            bool recordUndo = true);

        // ---- Legacy setters (kept: tests and editor scripts depend on them)
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

        // ---- Structure -----------------------------------------------------
        bool AddChildAt(
            std::string_view parentNodeGuid,
            std::string_view type,
            float localX,
            float localY);
        bool DuplicateNode(std::string_view nodeGuid, bool selectCopy = true);
        bool MoveNode(std::string_view nodeGuid, std::string_view parentNodeGuid, size_t index);
        bool NudgeNode(std::string_view nodeGuid, float deltaX, float deltaY);
        bool BringNodeForward(std::string_view nodeGuid);
        bool SendNodeBackward(std::string_view nodeGuid);
        bool BringNodeToFront(std::string_view nodeGuid);
        bool SendNodeToBack(std::string_view nodeGuid);
        bool SetNodeVisible(std::string_view nodeGuid, bool visible);
        // Canvas gesture writes: one change, one preview update.
        bool SetNodeOffsets(
            std::string_view nodeGuid,
            NLS::Engine::UI::UIVector2 offsetMin,
            NLS::Engine::UI::UIVector2 offsetMax,
            bool recordUndo = true);
        bool SetNodeSizeAndOffsets(
            std::string_view nodeGuid,
            NLS::Engine::UI::UIVector2 size,
            NLS::Engine::UI::UIVector2 offsetMin,
            NLS::Engine::UI::UIVector2 offsetMax,
            bool recordUndo = true);
        [[nodiscard]] std::string SerializeNode(std::string_view nodeGuid) const;
        bool PasteNode(
            std::string_view parentNodeGuid,
            size_t index,
            std::string_view serializedNode);

        // ---- Transactions --------------------------------------------------
        void BeginTransaction(std::string label = "Edit UI Document", std::string mergeKey = {});
        void EndTransaction();
        // Aborts the active gesture and restores its before snapshot.
        void CancelTransaction();
        [[nodiscard]] bool HasActiveTransaction() const { return m_activeTransaction.has_value(); }

        [[nodiscard]] bool BuildRuntimeDocument(
            NLS::Engine::UI::UIDocument& document,
            std::vector<NLS::Engine::UI::UIDocumentDiagnostic>* diagnostics = nullptr) const;

    private:
        struct DocumentSnapshot
        {
            Json document;
            std::vector<std::string> selection;
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
        static const Json* FindParentRecursive(const Json& node, std::string_view childNodeGuid);
        static bool TryGetNodeIndexInParent(
            const Json& parent,
            std::string_view childNodeGuid,
            size_t& index);
        static void AssignFreshIdentifiers(Json& node);
        static bool EnsureStableIdentifiers(Json& document);
        static bool IsSupportedElementType(std::string_view type);
        static bool IsSupportedLayoutMode(std::string_view mode);
        static bool NormalizePropertyValue(
            std::string_view path,
            const Json& value,
            Json& normalized,
            std::string* reason = nullptr);
        static bool WriteNodeProperty(Json& node, std::string_view path, const Json& value);
        static const Json* ReadNodeProperty(const Json& node, std::string_view path);
        static std::string DefaultPropertyLabel(std::string_view path);
        static bool WriteTextFileAtomically(const std::filesystem::path& path, const std::string& text);

        static void SyncPrimarySelection(Detail::UIDocumentDesignerState& state);

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
