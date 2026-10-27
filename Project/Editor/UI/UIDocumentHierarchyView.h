#pragma once

#include <array>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "UI/UIDocumentDesignerModel.h"

namespace NLS::Editor::UIDocumentTools
{
    // Tree view with selection sync, inline rename, drag reorder/reparent,
    // context menus and visibility toggles. All structural changes are issued
    // through the model so they share the document's undo history.
    class UIDocumentHierarchyView final
    {
    public:
        UIDocumentHierarchyView();
        ~UIDocumentHierarchyView();

        void Draw(UIDocumentDesignerModel& model);
        void RequestRename(UIDocumentDesignerModel& model, std::string_view nodeGuid);
        void RequestScrollToSelection() { m_scrollToSelection = true; }
        [[nodiscard]] bool IsRenaming() const { return !m_renameGuid.empty(); }
        [[nodiscard]] std::string GetFilter() const { return m_filterBuffer.data(); }

    private:
        using Json = UIDocumentDesignerModel::Json;

        void RebuildFlatList(const UIDocumentDesignerModel& model);
        void DrawNode(
            UIDocumentDesignerModel& model,
            const Json& node,
            size_t depth,
            bool filtering);
        bool MatchesFilter(const Json& node) const;
        void CommitRename(UIDocumentDesignerModel& model, bool cancel);
        void DrawContextMenu(UIDocumentDesignerModel& model, const std::string& nodeGuid);
        void DrawDragDrop(
            UIDocumentDesignerModel& model,
            const std::string& nodeGuid,
            bool isRoot);

        std::array<char, 128> m_filterBuffer {};
        std::vector<std::string> m_flatOrder;
        std::unordered_set<std::string> m_filteredNodes;
        std::string m_renameGuid;
        std::array<char, 256> m_renameBuffer {};
        bool m_renameFocus = false;
        bool m_scrollToSelection = false;
        std::string m_lastSelectionKey;
        std::string m_selectionAnchorGuid;
    };
}
