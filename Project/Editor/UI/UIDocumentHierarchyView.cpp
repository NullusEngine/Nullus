#include "UI/UIDocumentHierarchyView.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

#include "UI/UIDocumentCanvasView.h"

namespace NLS::Editor::UIDocumentTools
{
    namespace
    {
        constexpr const char* kHierarchyElementTypes[] = {
            "Panel", "Image", "NineSlice", "Text", "RichText", "Button", "Toggle",
            "Radio", "Slider", "ProgressBar", "TextField", "Dropdown", "ScrollView",
            "Scrollbar", "ListView"
        };

        std::string ToLower(std::string value)
        {
            std::transform(
                value.begin(),
                value.end(),
                value.begin(),
                [](const unsigned char character)
                {
                    return static_cast<char>(std::tolower(character));
                });
            return value;
        }

        bool ContainsCaseInsensitive(const std::string& haystack, const std::string& needle)
        {
            return ToLower(haystack).find(needle) != std::string::npos;
        }
    }

    UIDocumentHierarchyView::UIDocumentHierarchyView() = default;

    UIDocumentHierarchyView::~UIDocumentHierarchyView() = default;

    void UIDocumentHierarchyView::RequestRename(
        UIDocumentDesignerModel& model,
        const std::string_view nodeGuid)
    {
        if (nodeGuid.empty())
            return;
        const auto* node = model.FindNode(nodeGuid);
        if (node == nullptr)
            return;
        const auto name = node->value("name", node->value("type", std::string("Element")));
        std::fill(m_renameBuffer.begin(), m_renameBuffer.end(), '\0');
        name.copy(m_renameBuffer.data(), m_renameBuffer.size() - 1u);
        m_renameGuid = std::string(nodeGuid);
        m_renameFocus = true;
    }

    void UIDocumentHierarchyView::Draw(UIDocumentDesignerModel& model)
    {
        if (!model.IsLoaded())
        {
            ImGui::TextDisabled("No document open.");
            return;
        }

        // Selection changes coming from the canvas should reveal the row.
        const auto primary = model.GetSelectedNodeGuid();
        std::string selectionKey = primary;
        selectionKey += '|';
        selectionKey += std::to_string(model.GetSelectedNodes().size());
        if (selectionKey != m_lastSelectionKey)
        {
            m_lastSelectionKey = selectionKey;
            m_scrollToSelection = true;
        }

        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint(
            "##hierarchyFilter",
            "Search name or type",
            m_filterBuffer.data(),
            m_filterBuffer.size());

        const bool filtering = m_filterBuffer[0] != '\0';
        RebuildFlatList(model);

        ImGui::BeginChild("##hierarchyTree", ImVec2(0.0f, 0.0f), false);
        const auto root = model.GetDocument().find("root");
        if (root != model.GetDocument().end())
            DrawNode(model, *root, 0u, filtering);
        ImGui::EndChild();
    }

    void UIDocumentHierarchyView::RebuildFlatList(const UIDocumentDesignerModel& model)
    {
        m_flatOrder.clear();
        m_filteredNodes.clear();

        const auto root = model.GetDocument().find("root");
        if (root == model.GetDocument().end())
            return;

        const bool filtering = m_filterBuffer[0] != '\0';
        const std::string needle = ToLower(m_filterBuffer.data());

        const std::function<bool(const Json&)> collect =
            [&](const Json& node) -> bool
            {
                const auto guid = node.value("nodeGuid", std::string {});
                bool included = !filtering;
                if (filtering)
                {
                    const auto name = ToLower(node.value("name", std::string {}));
                    const auto type = ToLower(node.value("type", std::string {}));
                    included = name.find(needle) != std::string::npos ||
                        type.find(needle) != std::string::npos;
                }

                const auto children = node.find("children");
                if (children != node.end() && children->is_array())
                {
                    for (const auto& child : *children)
                    {
                        if (collect(child) && filtering)
                            included = true;
                    }
                }
                if (included && !guid.empty())
                    m_filteredNodes.insert(guid);
                return included;
            };
        collect(*root);

        const std::function<void(const Json&)> flatten =
            [&](const Json& node)
            {
                const auto guid = node.value("nodeGuid", std::string {});
                if (filtering && m_filteredNodes.find(guid) == m_filteredNodes.end())
                    return;
                if (!guid.empty())
                    m_flatOrder.push_back(guid);
                const auto children = node.find("children");
                if (children == node.end() || !children->is_array())
                    return;
                for (const auto& child : *children)
                    flatten(child);
            };
        flatten(*root);
    }

    bool UIDocumentHierarchyView::MatchesFilter(const Json& node) const
    {
        if (m_filterBuffer[0] == '\0')
            return true;
        const auto guid = node.value("nodeGuid", std::string {});
        return m_filteredNodes.find(guid) != m_filteredNodes.end();
    }

    void UIDocumentHierarchyView::DrawNode(
        UIDocumentDesignerModel& model,
        const Json& node,
        const size_t depth,
        const bool filtering)
    {
        const auto nodeGuid = node.value("nodeGuid", std::string {});
        if (nodeGuid.empty() || !MatchesFilter(node))
            return;

        const auto name = node.value("name", node.value("type", std::string("Element")));
        const auto type = node.value("type", std::string("Element"));
        const auto children = node.find("children");
        const bool hasChildren =
            children != node.end() && children->is_array() && !children->empty();
        const bool isRoot = model.IsRootNode(nodeGuid);
        const bool renaming = m_renameGuid == nodeGuid;

        ImGuiTreeNodeFlags flags =
            ImGuiTreeNodeFlags_SpanAvailWidth |
            ImGuiTreeNodeFlags_OpenOnArrow;
        if (!hasChildren)
            flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        if (nodeGuid == model.GetSelectedNodeGuid())
            flags |= ImGuiTreeNodeFlags_Selected;
        if (filtering)
            ImGui::SetNextItemOpen(true, ImGuiCond_Always);

        const bool open = ImGui::TreeNodeEx(
            nodeGuid.c_str(),
            flags,
            "%s",
            renaming ? "" : name.c_str());

        if (!renaming && ImGui::IsItemClicked(ImGuiMouseButton_Left))
        {
            const auto& io = ImGui::GetIO();
            if (io.KeyCtrl)
            {
                model.ToggleSelection(nodeGuid);
            }
            else if (io.KeyShift && !m_selectionAnchorGuid.empty())
            {
                const auto anchor = std::find(
                    m_flatOrder.begin(),
                    m_flatOrder.end(),
                    m_selectionAnchorGuid);
                const auto target = std::find(
                    m_flatOrder.begin(),
                    m_flatOrder.end(),
                    nodeGuid);
                if (anchor != m_flatOrder.end() && target != m_flatOrder.end())
                {
                    const auto begin = std::min(anchor, target);
                    const auto end = std::max(anchor, target);
                    std::vector<std::string> selection;
                    for (auto iterator = begin; iterator != end + 1; ++iterator)
                        selection.push_back(*iterator);
                    model.SetSelection(std::move(selection));
                }
                else
                {
                    model.SelectNode(nodeGuid);
                }
            }
            else
            {
                model.SelectNode(nodeGuid);
                m_selectionAnchorGuid = nodeGuid;
            }
            m_scrollToSelection = false;
        }

        if (!renaming && ImGui::IsItemHovered() &&
            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            RequestRename(model, nodeGuid);
        }

        if (!renaming)
            DrawDragDrop(model, nodeGuid, isRoot);

        if (!renaming)
            DrawContextMenu(model, nodeGuid);

        if (m_scrollToSelection && nodeGuid == model.GetSelectedNodeGuid())
        {
            ImGui::SetScrollHereY(0.5f);
            m_scrollToSelection = false;
        }

        if (renaming)
        {
            ImGui::SameLine();
            if (m_renameFocus)
            {
                ImGui::SetKeyboardFocusHere();
                m_renameFocus = false;
            }
            ImGui::SetNextItemWidth(-FLT_MIN - 28.0f);
            const bool entered = ImGui::InputText(
                "##rename",
                m_renameBuffer.data(),
                m_renameBuffer.size(),
                ImGuiInputTextFlags_EnterReturnsTrue |
                    ImGuiInputTextFlags_AutoSelectAll);
            if (entered)
            {
                CommitRename(model, false);
            }
            else if (ImGui::IsItemDeactivated())
            {
                CommitRename(model, ImGui::IsKeyPressed(ImGuiKey_Escape));
            }
        }

        // Type hint and visibility toggle.
        if (!isRoot)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", type.c_str());
            const float eyePosition = ImGui::GetWindowContentRegionMax().x - 22.0f;
            ImGui::SameLine(std::max(eyePosition, ImGui::GetCursorPosX() + 8.0f));
            bool visible = node.value("style", Json::object()).value("visible", true);
            ImGui::PushID("visible");
            if (ImGui::Checkbox("##toggle", &visible))
                model.SetNodeVisible(nodeGuid, visible);
            ImGui::PopID();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(visible ? "Hide element" : "Show element");
        }

        if (hasChildren && open)
        {
            for (const auto& child : *children)
                DrawNode(model, child, depth + 1u, filtering);
            ImGui::TreePop();
        }
    }

    void UIDocumentHierarchyView::DrawDragDrop(
        UIDocumentDesignerModel& model,
        const std::string& nodeGuid,
        const bool isRoot)
    {
        if (!isRoot && ImGui::BeginDragDropSource())
        {
            const auto* node = model.FindNode(nodeGuid);
            const auto name = node == nullptr
                ? nodeGuid
                : node->value("name", node->value("type", std::string("Element")));
            ImGui::SetDragDropPayload(
                kUIDesignerNodePayload,
                nodeGuid.data(),
                nodeGuid.size());
            ImGui::TextUnformatted(name.c_str());
            ImGui::EndDragDropSource();
        }

        const ImVec2 rowMin = ImGui::GetItemRectMin();
        const ImVec2 rowMax = ImGui::GetItemRectMax();
        if (!ImGui::BeginDragDropTarget())
            return;

        if (const ImGuiPayload* active = ImGui::GetDragDropPayload();
            active != nullptr && active->IsDataType(kUIDesignerNodePayload))
        {
            const float height = std::max(1.0f, rowMax.y - rowMin.y);
            const float t = (ImGui::GetIO().MousePos.y - rowMin.y) / height;
            auto* drawList = ImGui::GetWindowDrawList();
            if (t < 0.3f)
            {
                drawList->AddLine(
                    { rowMin.x, rowMin.y },
                    { rowMax.x, rowMin.y },
                    IM_COL32(76, 141, 255, 255),
                    2.0f);
            }
            else if (t > 0.7f)
            {
                drawList->AddLine(
                    { rowMin.x, rowMax.y },
                    { rowMax.x, rowMax.y },
                    IM_COL32(76, 141, 255, 255),
                    2.0f);
            }
            else
            {
                drawList->AddRect(
                    rowMin,
                    rowMax,
                    IM_COL32(76, 141, 255, 255),
                    0.0f,
                    0,
                    1.5f);
            }
        }

        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kUIDesignerNodePayload))
        {
            const std::string source(
                static_cast<const char*>(payload->Data),
                static_cast<size_t>(payload->DataSize));
            if (source != nodeGuid && !model.IsNodeInSubtree(source, nodeGuid))
            {
                std::string parentGuid;
                size_t index = 0u;
                if (model.TryGetNodeLocation(nodeGuid, parentGuid, index))
                {
                    const float height = std::max(1.0f, rowMax.y - rowMin.y);
                    const float t = (ImGui::GetIO().MousePos.y - rowMin.y) / height;
                    if (t < 0.3f)
                    {
                        model.MoveNode(source, parentGuid, index);
                    }
                    else if (t > 0.7f)
                    {
                        model.MoveNode(source, parentGuid, index + 1u);
                    }
                    else
                    {
                        model.MoveNode(source, nodeGuid, model.GetChildCount(nodeGuid));
                    }
                }
            }
        }
        ImGui::EndDragDropTarget();
    }

    void UIDocumentHierarchyView::DrawContextMenu(
        UIDocumentDesignerModel& model,
        const std::string& nodeGuid)
    {
        if (!ImGui::BeginPopupContextItem("##hierarchyContext"))
            return;

        const bool isRoot = model.IsRootNode(nodeGuid);
        if (ImGui::BeginMenu("Add Child"))
        {
            for (const auto* type : kHierarchyElementTypes)
            {
                if (ImGui::MenuItem(type))
                    model.AddChild(nodeGuid, type);
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Rename", "F2"))
            RequestRename(model, nodeGuid);
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, !isRoot))
            model.DuplicateNode(nodeGuid);
        if (ImGui::MenuItem("Delete", "Del", false, !isRoot))
            model.RemoveNode(nodeGuid);
        ImGui::Separator();
        if (ImGui::MenuItem("Bring Forward", nullptr, false, !isRoot))
            model.BringNodeForward(nodeGuid);
        if (ImGui::MenuItem("Send Backward", nullptr, false, !isRoot))
            model.SendNodeBackward(nodeGuid);
        if (ImGui::MenuItem("Bring to Front", nullptr, false, !isRoot))
            model.BringNodeToFront(nodeGuid);
        if (ImGui::MenuItem("Send to Back", nullptr, false, !isRoot))
            model.SendNodeToBack(nodeGuid);
        ImGui::Separator();
        const auto* node = model.FindNode(nodeGuid);
        const bool visible = node == nullptr ||
            node->value("style", Json::object()).value("visible", true);
        if (ImGui::MenuItem(visible ? "Hide" : "Show"))
            model.SetNodeVisible(nodeGuid, !visible);

        ImGui::EndPopup();
    }

    void UIDocumentHierarchyView::CommitRename(
        UIDocumentDesignerModel& model,
        const bool cancel)
    {
        if (m_renameGuid.empty())
            return;
        const auto guid = std::move(m_renameGuid);
        m_renameGuid.clear();
        m_renameFocus = false;
        if (!cancel && m_renameBuffer[0] != '\0')
            model.SetNodeName(guid, m_renameBuffer.data());
    }
}
