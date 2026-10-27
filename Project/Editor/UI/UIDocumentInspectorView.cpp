#include "UI/UIDocumentInspectorView.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <string>
#include <vector>

#include <imgui.h>

namespace NLS::Editor::UIDocumentTools
{
    namespace
    {
        using Json = UIDocumentDesignerModel::Json;
        using Vec2 = NLS::Engine::UI::UIVector2;

        constexpr float kLabelColumn = 140.0f;

        void RowLabel(const char* label, const bool mixed)
        {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            if (mixed)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("—");
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "Selected elements have different values. Editing applies to all.");
                }
            }
            ImGui::SameLine(kLabelColumn);
            ImGui::SetNextItemWidth(-FLT_MIN);
        }

        bool IsStretched(
            const UIDocumentRectEdit& rect,
            const bool horizontal)
        {
            return horizontal
                ? std::abs(rect.anchorMax.x - rect.anchorMin.x) > 0.0001f
                : std::abs(rect.anchorMax.y - rect.anchorMin.y) > 0.0001f;
        }

        template <typename T, typename Read>
        std::vector<T> ReadSelectedValues(
            const UIDocumentDesignerModel& model,
            Read&& read)
        {
            std::vector<T> values;
            values.reserve(model.GetSelectedNodes().size());
            for (const auto& guid : model.GetSelectedNodes())
            {
                if (const auto* node = model.FindNode(guid))
                    values.push_back(read(*node));
            }
            return values;
        }
    }

    UIDocumentInspectorView::UIDocumentInspectorView()
        : m_lifetime(std::make_shared<int>(0))
    {
    }

    UIDocumentInspectorView::~UIDocumentInspectorView() = default;

    void UIDocumentInspectorView::Draw(
        UIDocumentDesignerModel& model,
        const UIDocumentCanvasRenderer* renderer)
    {
        m_activeModel = &model;
        if (!model.IsLoaded())
        {
            (void)Inspector::FinishReflectedEdits(m_properties, *this);
            ImGui::TextDisabled("Open a .ui asset from the Asset Browser.");
            return;
        }

        EnsureUIDocumentInspectorReflection();
        m_properties.SetLifetime(m_lifetime);

        // Reloading the same path keeps node GUIDs but replaces the undo scope;
        // drop any in-flight edit so later writes target the live scope.
        const auto scopeGeneration = model.GetUndoScope().generation;
        if (m_scopeGeneration != scopeGeneration)
        {
            FinishEdit(model, false);
            m_scopeGeneration = scopeGeneration;
            m_cachedSelectionKey.clear();
        }

        const auto selectionKey = SelectionKey(model);
        if (selectionKey != m_cachedSelectionKey)
        {
            // Keep an in-flight edit in history instead of silently dropping it
            // when the selection changes underneath the inspector.
            FinishEdit(model, false);
            m_cachedSelectionKey = selectionKey;
        }

        const auto* node = model.GetSelectedNode();
        if (node == nullptr || model.GetSelectedNodes().empty())
        {
            (void)Inspector::FinishReflectedEdits(m_properties, *this);
            ImGui::TextDisabled("Select an element in the Hierarchy or on the Canvas.");
            return;
        }

        const auto type = node->value("type", std::string("Panel"));
        const auto& selection = model.GetSelectedNodes();
        ImGui::Text("%s", type.c_str());
        if (selection.size() > 1u)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("(%zu elements selected)", selection.size());
            ImGui::TextDisabled(
                "Edits apply to the whole selection; mixed fields show —.");
        }

        ImGui::PushID("UIDocumentInspectorSections");
        DrawElementSection(model);
        DrawRectSection(model, renderer);
        DrawLayoutSection(model);
        DrawStyleSection(model);
        DrawContentSection(model);
        DrawActions(model);
        ImGui::PopID();

        (void)Inspector::FinishReflectedEdits(m_properties, *this);
        m_activeModel = nullptr;
    }

    void UIDocumentInspectorView::DrawElementSection(UIDocumentDesignerModel& model)
    {
        if (!ImGui::CollapsingHeader("Element", ImGuiTreeNodeFlags_DefaultOpen))
            return;

        auto values = ReadSelectedValues<UIDocumentElementEdit>(
            model,
            [](const Json& node) { return ReadElementEdit(node); });
        if (values.empty())
            return;

        const auto id = std::string("ElementFields:") + SelectionKey(model);
        DrawReflectedSection<UIDocumentElementEdit>(
            model,
            id.c_str(),
            "",
            values,
            {},
            [&model](const UIDocumentElementEdit& value, const std::vector<std::string>& paths)
            {
                ApplyElementEdit(model, value, paths, false);
            });

        const auto primary = model.GetSelectedNodeGuid();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("GUID");
        ImGui::SameLine(kLabelColumn);
        ImGui::TextDisabled("%s", primary.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy"))
            ImGui::SetClipboardText(primary.c_str());
    }

    void UIDocumentInspectorView::DrawRectSection(
        UIDocumentDesignerModel& model,
        const UIDocumentCanvasRenderer* renderer)
    {
        if (!ImGui::CollapsingHeader("Rect & Anchors", ImGuiTreeNodeFlags_DefaultOpen))
            return;

        const auto* node = model.GetSelectedNode();
        if (node == nullptr)
            return;

        auto values = ReadSelectedValues<UIDocumentRectEdit>(
            model,
            [](const Json& selected) { return ReadRectEdit(selected); });
        if (values.empty())
            return;

        const auto value = values.front();
        const bool stretchedX = IsStretched(value, true);
        const bool stretchedY = IsStretched(value, false);

        DrawAnchorPreset(model, renderer);

        Inspector::ReflectedPropertyOptions options;
        if (stretchedX || stretchedY)
        {
            options.disabled = { "size" };
            ImGui::TextDisabled(
                "Size on stretched axes is derived from the anchors and offsets.");
        }

        const auto id = std::string("RectFields:") + SelectionKey(model);
        DrawReflectedSection<UIDocumentRectEdit>(
            model,
            id.c_str(),
            "",
            values,
            options,
            [&model](const UIDocumentRectEdit& edited, const std::vector<std::string>& paths)
            {
                ApplyRectEdit(model, edited, paths, false);
            });

        DrawPositionRow(model, renderer);

        if (model.IsRootNode(model.GetSelectedNodeGuid()))
        {
            ImGui::Separator();
            ImGui::TextDisabled("Root size is the document reference resolution.");
        }
    }

    void UIDocumentInspectorView::DrawLayoutSection(UIDocumentDesignerModel& model)
    {
        if (!ImGui::CollapsingHeader("Layout", ImGuiTreeNodeFlags_DefaultOpen))
            return;

        auto values = ReadSelectedValues<UIDocumentLayoutEdit>(
            model,
            [](const Json& selected) { return ReadLayoutEdit(selected); });
        if (values.empty())
            return;

        const auto value = values.front();
        const bool stacked =
            value.mode == UIDocumentLayoutModeEdit::Stack ||
            value.mode == UIDocumentLayoutModeEdit::Flex ||
            value.mode == UIDocumentLayoutModeEdit::Scroll;
        const bool flex =
            value.mode == UIDocumentLayoutModeEdit::Stack ||
            value.mode == UIDocumentLayoutModeEdit::Flex;
        const bool grid = value.mode == UIDocumentLayoutModeEdit::Grid;

        Inspector::ReflectedPropertyOptions options;
        if (!stacked)
            options.hidden.push_back("direction");
        if (!flex)
            options.hidden.push_back("align");
        if (!stacked && !grid)
            options.hidden.push_back("spacing");
        if (!grid)
            options.hidden.push_back("gridColumns");

        // flexGrow is consumed by the parent container.
        const auto parentGuid = model.GetNodeParentGuid(model.GetSelectedNodeGuid());
        const auto* parent = model.FindNode(parentGuid);
        const auto parentMode = parent == nullptr
            ? std::string("Absolute")
            : parent->value("layout", Json::object()).value("mode", std::string("Absolute"));
        if (parentMode != "Stack" && parentMode != "Flex")
            options.hidden.push_back("flexGrow");

        const auto id = std::string("LayoutFields:") + SelectionKey(model);
        DrawReflectedSection<UIDocumentLayoutEdit>(
            model,
            id.c_str(),
            "",
            values,
            options,
            [&model](const UIDocumentLayoutEdit& edited, const std::vector<std::string>& paths)
            {
                ApplyLayoutEdit(model, edited, paths, false);
            });
    }

    void UIDocumentInspectorView::DrawStyleSection(UIDocumentDesignerModel& model)
    {
        if (!ImGui::CollapsingHeader("Style", ImGuiTreeNodeFlags_DefaultOpen))
            return;

        auto values = ReadSelectedValues<UIDocumentStyleEdit>(
            model,
            [](const Json& selected) { return ReadStyleEdit(selected); });
        if (values.empty())
            return;

        const auto id = std::string("StyleFields:") + SelectionKey(model);
        DrawReflectedSection<UIDocumentStyleEdit>(
            model,
            id.c_str(),
            "",
            values,
            {},
            [&model](const UIDocumentStyleEdit& value, const std::vector<std::string>& paths)
            {
                ApplyStyleEdit(model, value, paths, false);
            });
    }

    void UIDocumentInspectorView::DrawContentSection(UIDocumentDesignerModel& model)
    {
        if (!ImGui::CollapsingHeader("Content", ImGuiTreeNodeFlags_DefaultOpen))
            return;

        const auto* node = model.GetSelectedNode();
        if (node == nullptr)
            return;

        auto values = ReadSelectedValues<UIDocumentContentEdit>(
            model,
            [](const Json& selected) { return ReadContentEdit(selected); });
        if (values.empty())
            return;

        const auto type = node->value("type", std::string("Panel"));
        const bool textLike = IsTextLikeElement(type);
        const bool imageLike = type == "Image" || type == "NineSlice";

        Inspector::ReflectedPropertyOptions options;
        if (!textLike)
        {
            options.hidden = {
                "text", "password", "font", "fontAssetId", "fontPixelSize" };
        }
        else
        {
            // The durable font reference replaces the legacy numeric id in the
            // normal editing flow; the old id stays visible under Advanced.
            options.hidden.push_back("fontAssetId");
            if (type != "TextField")
                options.hidden.push_back("password");
        }
        if (!imageLike)
            options.hidden.push_back("texture");
        if (type != "NineSlice")
            options.hidden.push_back("nineSlice");

        const auto id = std::string("ContentFields:") + SelectionKey(model);
        DrawReflectedSection<UIDocumentContentEdit>(
            model,
            id.c_str(),
            "",
            values,
            options,
            [&model](const UIDocumentContentEdit& value, const std::vector<std::string>& paths)
            {
                ApplyContentEdit(model, value, paths, false);
            });

        ImGui::Separator();
        if (ImGui::TreeNode("Advanced (Legacy Runtime IDs)"))
        {
            const auto textureId = node->value("textureId", 0ull);
            const auto textureGeneration = node->value("textureGeneration", 0u);
            const auto fontAssetId = node->value("fontAssetId", 0ull);
            ImGui::TextDisabled(
                "textureId: %llu (generation %u)",
                textureId,
                textureGeneration);
            ImGui::TextDisabled("fontAssetId: %llu", fontAssetId);
            ImGui::TextDisabled(
                "Texture/Font references above take precedence at runtime.");
            ImGui::TreePop();
        }
    }

    void UIDocumentInspectorView::DrawActions(UIDocumentDesignerModel& model)
    {
        if (!ImGui::CollapsingHeader("Actions", ImGuiTreeNodeFlags_DefaultOpen))
            return;

        const auto primary = model.GetSelectedNodeGuid();
        if (ImGui::Button("Duplicate"))
            model.DuplicateNode(primary);
        ImGui::SameLine();
        if (model.IsRootNode(primary))
            ImGui::BeginDisabled();
        if (ImGui::Button("Delete"))
            model.RemoveNode(primary);
        if (model.IsRootNode(primary))
            ImGui::EndDisabled();
        ImGui::SameLine();
        const auto* node = model.FindNode(primary);
        const bool visible = node == nullptr ||
            node->value("style", Json::object()).value("visible", true);
        if (ImGui::Button(visible ? "Hide" : "Show"))
            model.SetNodeVisible(primary, !visible);
    }

    void UIDocumentInspectorView::DrawAnchorPreset(
        UIDocumentDesignerModel& model,
        const UIDocumentCanvasRenderer* renderer)
    {
        const auto primary = model.GetSelectedNodeGuid();
        const auto* node = model.GetSelectedNode();
        if (node == nullptr)
            return;

        const auto value = ReadRectEdit(*node);
        const auto pivot = value.pivot;

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Anchor");
        if (IsMixed(model, "layout.anchorMin") || IsMixed(model, "layout.anchorMax"))
        {
            ImGui::SameLine();
            ImGui::TextDisabled("—");
        }
        ImGui::SameLine(kLabelColumn);
        ImGui::BeginGroup();
        for (int row = 0; row < 3; ++row)
        {
            for (int column = 0; column < 3; ++column)
            {
                ImGui::PushID(row * 3 + column);
                const bool selectedAnchor =
                    std::abs(value.anchorMin.x - column * 0.5f) <= 0.0001f &&
                    std::abs(value.anchorMin.y - row * 0.5f) <= 0.0001f &&
                    std::abs(value.anchorMax.x - column * 0.5f) <= 0.0001f &&
                    std::abs(value.anchorMax.y - row * 0.5f) <= 0.0001f;
                if (selectedAnchor)
                {
                    ImGui::PushStyleColor(
                        ImGuiCol_Button,
                        ImVec4(0.30f, 0.45f, 0.80f, 1.0f));
                }
                if (ImGui::Button("##anchor", ImVec2(20.0f, 18.0f)))
                {
                    const float anchorX = column * 0.5f;
                    const float anchorY = row * 0.5f;
                    NLS::Engine::UI::UIRect rect {};
                    NLS::Engine::UI::UIRect parentContent {};
                    const bool hasRect =
                        renderer != nullptr &&
                        renderer->GetElementRect(primary, rect) &&
                        renderer->GetElementContentRect(
                            model.GetNodeParentGuid(primary),
                            parentContent);

                    BeginEdit(model, "Set UI Anchor");
                    model.SetNodePropertyOnSelection(
                        "layout.anchorMin",
                        Json::array({ anchorX, anchorY }),
                        false);
                    model.SetNodePropertyOnSelection(
                        "layout.anchorMax",
                        Json::array({ anchorX, anchorY }),
                        false);
                    if (hasRect)
                    {
                        const Vec2 size { rect.width, rect.height };
                        model.SetNodePropertyOnSelection(
                            "layout.size",
                            Json::array({ size.x, size.y }),
                            false);
                        model.SetNodePropertyOnSelection(
                            "layout.offsetMin",
                            Json::array({
                                rect.x - parentContent.x -
                                    parentContent.width * anchorX + size.x * pivot.x,
                                rect.y - parentContent.y -
                                    parentContent.height * anchorY + size.y * pivot.y
                            }),
                            false);
                    }
                    FinishEdit(model, false);
                }
                if (selectedAnchor)
                    ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "Anchor %.1f / %.1f\nRe-anchors without moving the current rect.",
                        column * 0.5f,
                        row * 0.5f);
                }
                ImGui::PopID();
                if (column < 2)
                    ImGui::SameLine();
            }
        }
        ImGui::EndGroup();
        ImGui::SameLine();
        ImGui::TextDisabled("stretch: edit Min/Max");
    }

    void UIDocumentInspectorView::DrawPositionRow(
        UIDocumentDesignerModel& model,
        const UIDocumentCanvasRenderer* renderer)
    {
        if (renderer == nullptr)
            return;

        const auto primary = model.GetSelectedNodeGuid();
        NLS::Engine::UI::UIRect rect {};
        if (!renderer->GetElementRect(primary, rect))
            return;

        Maths::Vector2 position(rect.x, rect.y);
        Inspector::ReflectedPropertyOptions options;
        options.metadata.emplace();
        options.metadata->step = 1.0f;
        options.metadata->min = -100000.0f;
        options.metadata->max = 100000.0f;
        const auto edit = m_properties.Draw("LivePosition:" + SelectionKey(model), "Position", position, options);
        if (edit.canceled)
        {
            (void)Inspector::ApplyReflectedEdit(*this, edit, "Move UI Element");
            return;
        }
        if (edit.changed && (!IsPropertyEditOpen() ? BeginPropertyEdit("Move UI Element") : true))
        {
            const auto* node = model.GetSelectedNode();
            if (node != nullptr)
            {
                const auto rectValue = ReadRectEdit(*node);
                const Vec2 delta{position.x - rect.x, position.y - rect.y};
                Vec2 offsetMin { rectValue.offsetMin.x, rectValue.offsetMin.y };
                Vec2 offsetMax { rectValue.offsetMax.x, rectValue.offsetMax.y };
                offsetMin.x += delta.x;
                offsetMin.y += delta.y;
                if (IsStretched(rectValue, true))
                    offsetMax.x += delta.x;
                if (IsStretched(rectValue, false))
                    offsetMax.y += delta.y;
                model.SetNodeOffsets(primary, offsetMin, offsetMax, false);
            }
        }
        (void)Inspector::ApplyReflectedEdit(*this, edit, "Move UI Element");
    }

    bool UIDocumentInspectorView::BeginPropertyEdit(const std::string_view label)
    {
        if (m_activeModel == nullptr)
            return false;
        if (m_editOpen)
            return true;
        m_activeModel->BeginTransaction(std::string(label));
        m_editOpen = m_activeModel->HasActiveTransaction();
        return m_editOpen;
    }

    bool UIDocumentInspectorView::PreviewPropertyEdit()
    {
        // Model writes are applied directly under the open transaction; there is
        // no separate candidate to publish.
        return m_editOpen;
    }

    bool UIDocumentInspectorView::CommitPropertyEdit()
    {
        if (m_activeModel == nullptr || !m_editOpen)
            return false;
        m_activeModel->EndTransaction();
        m_editOpen = false;
        return true;
    }

    bool UIDocumentInspectorView::CancelPropertyEdit()
    {
        if (m_activeModel == nullptr || !m_editOpen)
            return false;
        m_activeModel->CancelTransaction();
        m_editOpen = false;
        return true;
    }

    bool UIDocumentInspectorView::IsPropertyEditOpen() const
    {
        return m_editOpen;
    }

    void UIDocumentInspectorView::BeginEdit(
        UIDocumentDesignerModel& model,
        const char* label)
    {
        m_activeModel = &model;
        (void)BeginPropertyEdit(label);
    }

    void UIDocumentInspectorView::FinishEdit(
        UIDocumentDesignerModel& model,
        const bool cancelled)
    {
        m_activeModel = &model;
        if (cancelled)
            (void)CancelPropertyEdit();
        else
            (void)CommitPropertyEdit();
    }

    std::string UIDocumentInspectorView::SelectionKey(
        const UIDocumentDesignerModel& model) const
    {
        std::string key;
        for (const auto& guid : model.GetSelectedNodes())
        {
            key += guid;
            key += '|';
        }
        return key;
    }

    bool UIDocumentInspectorView::IsMixed(
        const UIDocumentDesignerModel& model,
        const char* path) const
    {
        const auto& selection = model.GetSelectedNodes();
        if (selection.size() <= 1u)
            return false;
        const auto* first = model.GetNodeProperty(selection.front(), path);
        for (size_t index = 1u; index < selection.size(); ++index)
        {
            const auto* value = model.GetNodeProperty(selection[index], path);
            if ((first == nullptr) != (value == nullptr))
                return true;
            if (first != nullptr && *first != *value)
                return true;
        }
        return false;
    }
}
