#include "Panels/UIDocumentDesigner.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

#include <Debug/Logger.h>
#include <imgui.h>

#include "Core/EditorActions.h"
#include "Panels/AssetBrowser.h"

namespace NLS::Editor::Panels
{
    namespace
    {
        using Json = NLS::Editor::UIDocumentTools::UIDocumentDesignerModel::Json;

        constexpr const char* kElementTypes[] = {
            "Panel", "Image", "NineSlice", "Text", "RichText", "Button", "Toggle", "Radio",
            "Slider", "ProgressBar", "TextField", "Dropdown", "ScrollView", "Scrollbar", "ListView"
        };
        constexpr const char* kLayoutModes[] = {
            "Absolute", "Stack", "Flex", "Grid", "Overlay", "Scroll"
        };

        void CopyToBuffer(const std::string& value, char* buffer, const size_t size)
        {
            std::fill(buffer, buffer + size, '\0');
            value.copy(buffer, size - 1u);
        }

        std::array<float, 2u> ReadVector2(const Json& node, const char* section, const char* key)
        {
            const auto sectionIt = node.find(section);
            if (sectionIt == node.end() || !sectionIt->is_object())
                return {};
            const auto valueIt = sectionIt->find(key);
            if (valueIt == sectionIt->end() || !valueIt->is_array() || valueIt->size() < 2u)
                return {};
            return {
                (*valueIt)[0].is_number() ? (*valueIt)[0].get<float>() : 0.0f,
                (*valueIt)[1].is_number() ? (*valueIt)[1].get<float>() : 0.0f
            };
        }

        std::array<float, 4u> ReadColor(const Json& node)
        {
            const auto styleIt = node.find("style");
            if (styleIt == node.end() || !styleIt->is_object())
                return { 0.0f, 0.0f, 0.0f, 0.0f };
            const auto colorIt = styleIt->find("backgroundColor");
            if (colorIt == styleIt->end() || !colorIt->is_array() || colorIt->size() < 3u)
                return { 0.0f, 0.0f, 0.0f, 0.0f };
            return {
                (*colorIt)[0].is_number() ? (*colorIt)[0].get<float>() : 0.0f,
                (*colorIt)[1].is_number() ? (*colorIt)[1].get<float>() : 0.0f,
                (*colorIt)[2].is_number() ? (*colorIt)[2].get<float>() : 0.0f,
                colorIt->size() > 3u && (*colorIt)[3].is_number() ? (*colorIt)[3].get<float>() : 1.0f
            };
        }

        int LayoutModeIndex(const std::string& value)
        {
            for (int index = 0; index < static_cast<int>(std::size(kLayoutModes)); ++index)
            {
                if (value == kLayoutModes[index])
                    return index;
            }
            return 0;
        }

        ImU32 ToColor(const NLS::Engine::UI::UIColor color)
        {
            return ImGui::ColorConvertFloat4ToU32({ color.r, color.g, color.b, color.a });
        }

        ImVec2 Add(const ImVec2 left, const ImVec2 right)
        {
            return { left.x + right.x, left.y + right.y };
        }

        ImVec2 Subtract(const ImVec2 left, const ImVec2 right)
        {
            return { left.x - right.x, left.y - right.y };
        }
    }

    UIDocumentDesigner::UIDocumentDesigner(
        const std::string& title,
        const bool opened,
        const NLS::UI::PanelWindowSettings& windowSettings)
        : PanelWindow(title, opened, windowSettings)
    {
        minSize = { 760.0f, 480.0f };
    }

    bool UIDocumentDesigner::OpenDocument(const std::filesystem::path& path)
    {
        if (!m_model.Load(path))
        {
            NLS_LOG_ERROR("Failed to open UI document: " + path.string());
            return false;
        }

        RefreshEditBuffers();
        Open();
        Focus();
        return true;
    }

    void UIDocumentDesigner::OnBeforeDrawWidgets()
    {
        if (!m_model.IsLoaded())
        {
            ImGui::TextDisabled("Open a .ui asset from the Asset Browser.");
            return;
        }

        DrawToolbar();
        ImGui::Separator();

        const auto content = ImGui::GetContentRegionAvail();
        const float sideWidth = std::clamp(content.x * 0.20f, 190.0f, 280.0f);
        const float inspectorWidth = std::clamp(content.x * 0.25f, 240.0f, 360.0f);

        ImGui::BeginChild("##UiDesignerControls", { sideWidth, content.y }, true);
        DrawControls();
        ImGui::Separator();
        DrawHierarchy();
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild("##UiDesignerCanvas", { std::max(1.0f, content.x - sideWidth - inspectorWidth - 16.0f), content.y }, true);
        DrawCanvasPreview();
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild("##UiDesignerInspector", { inspectorWidth, content.y }, true);
        DrawInspector();
        ImGui::Separator();
        DrawDiagnostics();
        ImGui::EndChild();
    }

    void UIDocumentDesigner::DrawToolbar()
    {
        const bool canSave = m_model.IsDirty() && m_model.GetDiagnostics().empty();
        if (ImGui::Button("Save") && canSave)
        {
            if (m_model.Save())
                NotifyDocumentSaved();
        }
        ImGui::SameLine();
        if (!canSave)
            ImGui::BeginDisabled();
        ImGui::TextUnformatted(m_model.IsDirty() ? "Unsaved" : "Saved");
        if (!canSave)
            ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Reload"))
        {
            (void)m_model.Reload();
            RefreshEditBuffers();
        }
        ImGui::SameLine();
        if (!m_model.CanUndo())
            ImGui::BeginDisabled();
        if (ImGui::Button("Undo"))
        {
            (void)m_model.Undo();
            RefreshEditBuffers();
        }
        if (!m_model.CanUndo())
            ImGui::EndDisabled();
        ImGui::SameLine();
        if (!m_model.CanRedo())
            ImGui::BeginDisabled();
        if (ImGui::Button("Redo"))
        {
            (void)m_model.Redo();
            RefreshEditBuffers();
        }
        if (!m_model.CanRedo())
            ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("%s", m_model.GetPath().filename().string().c_str());
    }

    void UIDocumentDesigner::DrawControls()
    {
        ImGui::TextUnformatted("Controls");
        const auto parentGuid = m_model.GetSelectedNodeGuid().empty()
            ? m_model.GetDocument()["root"].value("nodeGuid", std::string {})
            : m_model.GetSelectedNodeGuid();
        for (const auto* type : kElementTypes)
        {
            if (ImGui::Selectable(type, false))
            {
                (void)m_model.AddChild(parentGuid, type);
                RefreshEditBuffers();
            }
        }
    }

    void UIDocumentDesigner::DrawHierarchy()
    {
        ImGui::TextUnformatted("Hierarchy");
        const auto root = m_model.GetDocument().find("root");
        if (root != m_model.GetDocument().end())
            DrawNodeTree(*root);
    }

    void UIDocumentDesigner::DrawNodeTree(const Json& node)
    {
        const auto nodeGuid = node.value("nodeGuid", std::string {});
        const auto name = node.value("name", node.value("type", std::string("Element")));
        const bool hasChildren = node.contains("children") && node["children"].is_array() && !node["children"].empty();
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow;
        if (!hasChildren)
            flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        if (nodeGuid == m_model.GetSelectedNodeGuid())
            flags |= ImGuiTreeNodeFlags_Selected;

        const bool open = ImGui::TreeNodeEx(nodeGuid.c_str(), flags, "%s", name.c_str());
        if (ImGui::IsItemClicked())
        {
            m_model.SelectNode(nodeGuid);
            RefreshEditBuffers();
        }
        if (hasChildren && open)
        {
            for (const auto& child : node["children"])
                DrawNodeTree(child);
            ImGui::TreePop();
        }
    }

    void UIDocumentDesigner::DrawCanvasPreview()
    {
        ImGui::TextUnformatted("Canvas");
        const auto available = ImGui::GetContentRegionAvail();
        const ImVec2 canvasSize { std::max(1.0f, available.x), std::max(1.0f, available.y - 22.0f) };
        const ImVec2 canvasOrigin = ImGui::GetCursorScreenPos();
        auto* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(canvasOrigin, Add(canvasOrigin, canvasSize), IM_COL32(26, 29, 35, 255));
        drawList->AddRect(canvasOrigin, Add(canvasOrigin, canvasSize), IM_COL32(70, 76, 88, 255));

        NLS::Engine::UI::UIDocument document;
        std::vector<NLS::Engine::UI::UIDocumentDiagnostic> diagnostics;
        if (!m_model.BuildRuntimeDocument(document, &diagnostics))
        {
            drawList->AddText(Add(canvasOrigin, ImVec2(12.0f, 12.0f)), IM_COL32(235, 100, 90, 255), "Document contains validation errors");
            ImGui::Dummy(canvasSize);
            return;
        }

        const auto rootDefinition = document.GetRootDefinition();
        const auto referenceSize = rootDefinition.layout.size;
        const float documentWidth = referenceSize.x > 0.0f ? referenceSize.x : 1920.0f;
        const float documentHeight = referenceSize.y > 0.0f ? referenceSize.y : 1080.0f;
        const float scale = std::max(0.05f, std::min(
            (canvasSize.x - 32.0f) / documentWidth,
            (canvasSize.y - 32.0f) / documentHeight));
        const ImVec2 previewSize { documentWidth * scale, documentHeight * scale };
        const ImVec2 previewOrigin {
            canvasOrigin.x + (canvasSize.x - previewSize.x) * 0.5f,
            canvasOrigin.y + (canvasSize.y - previewSize.y) * 0.5f
        };
        drawList->AddRectFilled(previewOrigin, Add(previewOrigin, previewSize), IM_COL32(48, 52, 63, 255));

        auto instance = document.Instantiate();
        instance->UpdateLayout({ documentWidth, documentHeight });
        if (const auto* root = instance->GetRoot())
            DrawRuntimeElement(*root, *drawList, previewOrigin, scale, m_model.GetSelectedNodeGuid());

        ImGui::SetCursorScreenPos(canvasOrigin);
        ImGui::InvisibleButton("##UiDesignerCanvasHitTarget", canvasSize);
        if (ImGui::IsItemClicked() && instance && instance->GetRoot())
        {
            const auto mouse = ImGui::GetIO().MousePos;
            const NLS::Engine::UI::UIVector2 point {
                (mouse.x - previewOrigin.x) / scale,
                (mouse.y - previewOrigin.y) / scale
            };
            if (const auto* hit = FindRuntimeElementAt(*instance->GetRoot(), point))
            {
                m_model.SelectNode(hit->GetNodeGuid().ToString());
                RefreshEditBuffers();
            }
        }
        ImGui::SetCursorScreenPos(Add(canvasOrigin, canvasSize));
    }

    void UIDocumentDesigner::DrawInspector()
    {
        ImGui::TextUnformatted("Inspector");
        const auto* node = m_model.GetSelectedNode();
        if (node == nullptr)
        {
            ImGui::TextDisabled("Select an element.");
            return;
        }

        const auto nodeGuid = node->value("nodeGuid", std::string {});
        if (m_buffersNodeGuid != nodeGuid)
            RefreshEditBuffers();

        ImGui::Text("Type: %s", node->value("type", std::string("Panel")).c_str());
        ImGui::TextDisabled("%s", nodeGuid.c_str());

        ImGui::TextUnformatted("Name");
        const bool nameChanged = ImGui::InputText("##UiElementName", m_nameBuffer.data(), m_nameBuffer.size());
        if (ImGui::IsItemActivated())
            m_model.BeginTransaction();
        if (nameChanged)
            m_model.SetNodeName(nodeGuid, m_nameBuffer.data(), false);
        if (ImGui::IsItemDeactivatedAfterEdit())
            m_model.EndTransaction();

        const auto type = node->value("type", std::string {});
        if (type == "Text" || type == "RichText" || type == "Button" || type == "TextField")
        {
            ImGui::TextUnformatted("Text");
            const bool textChanged = ImGui::InputTextMultiline(
                    "##UiElementText",
                    m_textBuffer.data(),
                    m_textBuffer.size(),
                    { -FLT_MIN, 72.0f });
            if (ImGui::IsItemActivated())
                m_model.BeginTransaction();
            if (textChanged)
                m_model.SetNodeText(nodeGuid, m_textBuffer.data(), false);
            if (ImGui::IsItemDeactivatedAfterEdit())
                m_model.EndTransaction();
        }

        ImGui::Separator();
        ImGui::TextUnformatted("Layout");
        const auto layout = node->contains("layout") && (*node)["layout"].is_object()
            ? (*node)["layout"] : Json::object();
        const auto layoutMode = layout.value("mode", std::string("Absolute"));
        int modeIndex = LayoutModeIndex(layoutMode);
        const bool modeChanged = ImGui::Combo("Mode", &modeIndex, kLayoutModes, static_cast<int>(std::size(kLayoutModes)));
        if (ImGui::IsItemActivated())
            m_model.BeginTransaction();
        if (modeChanged)
            m_model.SetNodeLayoutMode(nodeGuid, kLayoutModes[modeIndex], false);
        if (ImGui::IsItemDeactivatedAfterEdit())
            m_model.EndTransaction();

        auto size = ReadVector2(*node, "layout", "size");
        const bool sizeChanged = ImGui::DragFloat2("Size", size.data(), 1.0f, 0.0f, 8192.0f);
        if (ImGui::IsItemActivated())
            m_model.BeginTransaction();
        if (sizeChanged)
            m_model.SetNodeLayoutSize(nodeGuid, size[0], size[1], false);
        if (ImGui::IsItemDeactivatedAfterEdit())
            m_model.EndTransaction();

        float spacing = layout.value("spacing", 0.0f);
        const bool spacingChanged = ImGui::DragFloat("Spacing", &spacing, 0.25f, 0.0f, 512.0f);
        if (ImGui::IsItemActivated())
            m_model.BeginTransaction();
        if (spacingChanged)
            m_model.SetNodeSpacing(nodeGuid, spacing, false);
        if (ImGui::IsItemDeactivatedAfterEdit())
            m_model.EndTransaction();

        ImGui::Separator();
        ImGui::TextUnformatted("Style");
        auto color = ReadColor(*node);
        const bool colorChanged = ImGui::ColorEdit4("Background", color.data(), ImGuiColorEditFlags_AlphaBar);
        if (ImGui::IsItemActivated())
            m_model.BeginTransaction();
        if (colorChanged)
            m_model.SetNodeBackgroundColor(nodeGuid, color[0], color[1], color[2], color[3], false);
        if (ImGui::IsItemDeactivatedAfterEdit())
            m_model.EndTransaction();

        ImGui::Separator();
        const auto rootGuid = m_model.GetDocument()["root"].value("nodeGuid", std::string {});
        if (nodeGuid == rootGuid)
            ImGui::BeginDisabled();
        if (ImGui::Button("Delete Element"))
        {
            (void)m_model.RemoveNode(nodeGuid);
            RefreshEditBuffers();
        }
        if (nodeGuid == rootGuid)
            ImGui::EndDisabled();
    }

    void UIDocumentDesigner::DrawDiagnostics()
    {
        if (!ImGui::CollapsingHeader("Diagnostics", ImGuiTreeNodeFlags_DefaultOpen))
            return;
        const auto& diagnostics = m_model.GetDiagnostics();
        if (diagnostics.empty())
        {
            ImGui::TextDisabled("No validation errors.");
            return;
        }
        for (const auto& diagnostic : diagnostics)
        {
            const auto color = diagnostic.error
                ? IM_COL32(236, 104, 92, 255)
                : IM_COL32(224, 182, 90, 255);
            ImGui::TextColored(
                ImGui::ColorConvertU32ToFloat4(color),
                "%s: %s",
                diagnostic.path.c_str(),
                diagnostic.message.c_str());
        }
    }

    void UIDocumentDesigner::RefreshEditBuffers()
    {
        const auto* node = m_model.GetSelectedNode();
        if (node == nullptr)
        {
            m_buffersNodeGuid.clear();
            m_nameBuffer.fill('\0');
            m_textBuffer.fill('\0');
            return;
        }

        m_buffersNodeGuid = node->value("nodeGuid", std::string {});
        CopyToBuffer(node->value("name", std::string {}), m_nameBuffer.data(), m_nameBuffer.size());
        CopyToBuffer(node->value("text", std::string {}), m_textBuffer.data(), m_textBuffer.size());
    }

    void UIDocumentDesigner::NotifyDocumentSaved()
    {
        auto& assetBrowser = EDITOR_PANEL(NLS::Editor::Panels::AssetBrowser, "Asset Browser");
        assetBrowser.Refresh();
        NLS_LOG_INFO("Saved UI document: " + m_model.GetPath().string());
    }

    void UIDocumentDesigner::DrawRuntimeElement(
        const NLS::Engine::UI::UIElement& element,
        ImDrawList& drawList,
        const ImVec2 origin,
        const float scale,
        const std::string& selectedNodeGuid)
    {
        const auto rect = element.GetLayoutRect();
        const ImVec2 minimum { origin.x + rect.x * scale, origin.y + rect.y * scale };
        const ImVec2 maximum { minimum.x + rect.width * scale, minimum.y + rect.height * scale };
        const auto style = element.GetStyle();
        auto background = style.backgroundColor;
        if (background.a <= 0.0f && element.GetElementType() == NLS::Engine::UI::UIElementType::Button)
            background = { 0.18f, 0.34f, 0.62f, 1.0f };
        if (background.a > 0.0f)
            drawList.AddRectFilled(minimum, maximum, ToColor(background));
        drawList.AddRect(minimum, maximum, IM_COL32(132, 141, 158, 180));

        const auto type = element.GetElementType();
        if (type == NLS::Engine::UI::UIElementType::Text ||
            type == NLS::Engine::UI::UIElementType::RichText ||
            type == NLS::Engine::UI::UIElementType::Button ||
            type == NLS::Engine::UI::UIElementType::TextField)
        {
            const auto text = element.GetText();
            if (!text.empty())
                drawList.AddText(Add(minimum, ImVec2(6.0f, 5.0f)), ToColor(style.color), text.c_str());
        }
        if (element.GetNodeGuid().ToString() == selectedNodeGuid)
            drawList.AddRect(
                Subtract(minimum, ImVec2(2.0f, 2.0f)),
                Add(maximum, ImVec2(2.0f, 2.0f)),
                IM_COL32(90, 175, 255, 255),
                0.0f,
                0,
                2.0f);

        for (const auto& child : element.GetChildren())
        {
            if (child)
                DrawRuntimeElement(*child, drawList, origin, scale, selectedNodeGuid);
        }
    }

    const NLS::Engine::UI::UIElement* UIDocumentDesigner::FindRuntimeElementAt(
        const NLS::Engine::UI::UIElement& element,
        const NLS::Engine::UI::UIVector2 point)
    {
        const auto& children = element.GetChildren();
        for (auto it = children.rbegin(); it != children.rend(); ++it)
        {
            if (*it)
            {
                if (const auto* hit = FindRuntimeElementAt(**it, point))
                    return hit;
            }
        }
        return element.GetLayoutRect().Contains(point) ? &element : nullptr;
    }
}
