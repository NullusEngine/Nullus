#include "UI/UIDocumentDesignerModel.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include <Guid.h>

namespace NLS::Editor::UIDocumentTools
{
    namespace
    {
        constexpr size_t kMaxUndoEntries = 128u;

        std::string DefaultNodeName(const std::string_view type)
        {
            if (type == "Panel") return "Panel";
            if (type == "Image") return "Image";
            if (type == "NineSlice") return "Nine Slice";
            if (type == "Text") return "Text";
            if (type == "RichText") return "Rich Text";
            if (type == "Button") return "Button";
            if (type == "Toggle") return "Toggle";
            if (type == "Radio") return "Radio";
            if (type == "Slider") return "Slider";
            if (type == "ProgressBar") return "Progress Bar";
            if (type == "TextField") return "Text Field";
            if (type == "Dropdown") return "Dropdown";
            if (type == "ScrollView") return "Scroll View";
            if (type == "Scrollbar") return "Scrollbar";
            if (type == "ListView") return "List View";
            return "Element";
        }

        nlohmann::json Color(float red, float green, float blue, float alpha)
        {
            return nlohmann::json::array({ red, green, blue, alpha });
        }
    }

    std::string UIDocumentDesignerModel::CreateDocumentSource(const std::string_view documentName)
    {
        return CreateEmptyDocument(documentName).dump(2) + "\n";
    }

    bool UIDocumentDesignerModel::Load(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            m_diagnostics = {{ path.string(), "could not open UI document", true }};
            return false;
        }

        std::ostringstream source;
        source << input.rdbuf();
        try
        {
            auto document = Json::parse(source.str());
            if (!document.is_object())
            {
                m_diagnostics = {{ "$", "document root must be an object", true }};
                return false;
            }

            m_path = path;
            m_document = std::move(document);
            m_undo.clear();
            m_redo.clear();
            m_transactionBefore = Json {};
            m_transactionActive = false;
            m_dirty = EnsureStableIdentifiers(m_document);
            m_selectedNodeGuid = m_document["root"].value("nodeGuid", std::string {});
            Revalidate();
            return std::none_of(
                m_diagnostics.begin(),
                m_diagnostics.end(),
                [](const UIDocumentDesignerDiagnostic& diagnostic) { return diagnostic.error; });
        }
        catch (const std::exception& exception)
        {
            m_diagnostics = {{ "$", std::string("invalid JSON: ") + exception.what(), true }};
            return false;
        }
    }

    bool UIDocumentDesignerModel::Save()
    {
        if (!IsLoaded())
            return false;

        Revalidate();
        if (std::any_of(
                m_diagnostics.begin(),
                m_diagnostics.end(),
                [](const UIDocumentDesignerDiagnostic& diagnostic) { return diagnostic.error; }))
            return false;

        if (!WriteTextFileAtomically(m_path, m_document.dump(2) + "\n"))
        {
            m_diagnostics.push_back({ m_path.string(), "could not save UI document", true });
            return false;
        }

        m_dirty = false;
        return true;
    }

    bool UIDocumentDesignerModel::Reload()
    {
        if (!IsLoaded())
            return false;
        return Load(m_path);
    }

    const UIDocumentDesignerModel::Json* UIDocumentDesignerModel::FindNode(
        const std::string_view nodeGuid) const
    {
        const auto root = m_document.find("root");
        return root == m_document.end() ? nullptr : FindNodeRecursive(*root, nodeGuid);
    }

    UIDocumentDesignerModel::Json* UIDocumentDesignerModel::FindNode(const std::string_view nodeGuid)
    {
        const auto root = m_document.find("root");
        return root == m_document.end() ? nullptr : FindNodeRecursive(*root, nodeGuid);
    }

    const UIDocumentDesignerModel::Json* UIDocumentDesignerModel::GetSelectedNode() const
    {
        return FindNode(m_selectedNodeGuid);
    }

    void UIDocumentDesignerModel::SelectNode(std::string nodeGuid)
    {
        if (FindNode(nodeGuid) != nullptr)
            m_selectedNodeGuid = std::move(nodeGuid);
    }

    bool UIDocumentDesignerModel::AddChild(const std::string_view parentNodeGuid, const std::string_view type)
    {
        if (!IsSupportedElementType(type))
            return false;
        auto* parent = FindNode(parentNodeGuid);
        if (parent == nullptr)
            return false;

        RecordUndo();
        auto node = CreateNode(type, DefaultNodeName(type));
        const auto nodeGuid = node.value("nodeGuid", std::string {});
        (*parent)["children"].push_back(std::move(node));
        m_selectedNodeGuid = nodeGuid;
        m_dirty = true;
        Revalidate();
        return true;
    }

    bool UIDocumentDesignerModel::RemoveNode(const std::string_view nodeGuid)
    {
        const auto root = m_document.find("root");
        if (root == m_document.end() || root->value("nodeGuid", std::string {}) == nodeGuid)
            return false;

        auto* parent = FindParentRecursive(*root, nodeGuid);
        if (parent == nullptr || !parent->contains("children") || !(*parent)["children"].is_array())
            return false;

        RecordUndo();
        auto& children = (*parent)["children"];
        children.erase(
            std::remove_if(
                children.begin(),
                children.end(),
                [nodeGuid](const Json& child)
                {
                    return child.value("nodeGuid", std::string {}) == nodeGuid;
                }),
            children.end());
        m_selectedNodeGuid = parent->value("nodeGuid", std::string {});
        m_dirty = true;
        Revalidate();
        return true;
    }

    bool UIDocumentDesignerModel::SetNodeName(
        const std::string_view nodeGuid,
        std::string value,
        const bool recordUndo)
    {
        auto* node = FindNode(nodeGuid);
        if (node == nullptr || node->value("name", std::string {}) == value)
            return false;
        MarkChanged(recordUndo);
        (*node)["name"] = std::move(value);
        Revalidate();
        return true;
    }

    bool UIDocumentDesignerModel::SetNodeText(
        const std::string_view nodeGuid,
        std::string value,
        const bool recordUndo)
    {
        auto* node = FindNode(nodeGuid);
        if (node == nullptr || node->value("text", std::string {}) == value)
            return false;
        MarkChanged(recordUndo);
        (*node)["text"] = std::move(value);
        Revalidate();
        return true;
    }

    bool UIDocumentDesignerModel::SetNodeLayoutSize(
        const std::string_view nodeGuid,
        const float width,
        const float height,
        const bool recordUndo)
    {
        auto* node = FindNode(nodeGuid);
        if (node == nullptr)
            return false;

        const auto value = Json::array({ std::max(0.0f, width), std::max(0.0f, height) });
        if ((*node)["layout"].value("size", Json::array()) == value)
            return false;
        MarkChanged(recordUndo);
        (*node)["layout"]["size"] = value;
        Revalidate();
        return true;
    }

    bool UIDocumentDesignerModel::SetNodeLayoutMode(
        const std::string_view nodeGuid,
        const std::string_view mode,
        const bool recordUndo)
    {
        if (!IsSupportedLayoutMode(mode))
            return false;
        auto* node = FindNode(nodeGuid);
        if (node == nullptr || (*node)["layout"].value("mode", std::string("Absolute")) == mode)
            return false;
        MarkChanged(recordUndo);
        (*node)["layout"]["mode"] = std::string(mode);
        Revalidate();
        return true;
    }

    bool UIDocumentDesignerModel::SetNodeSpacing(
        const std::string_view nodeGuid,
        const float spacing,
        const bool recordUndo)
    {
        auto* node = FindNode(nodeGuid);
        if (node == nullptr)
            return false;
        const auto value = std::max(0.0f, spacing);
        if (std::abs((*node)["layout"].value("spacing", 0.0f) - value) <= 0.0001f)
            return false;
        MarkChanged(recordUndo);
        (*node)["layout"]["spacing"] = value;
        Revalidate();
        return true;
    }

    bool UIDocumentDesignerModel::SetNodeBackgroundColor(
        const std::string_view nodeGuid,
        const float red,
        const float green,
        const float blue,
        const float alpha,
        const bool recordUndo)
    {
        auto* node = FindNode(nodeGuid);
        if (node == nullptr)
            return false;
        const auto value = Color(
            std::clamp(red, 0.0f, 1.0f),
            std::clamp(green, 0.0f, 1.0f),
            std::clamp(blue, 0.0f, 1.0f),
            std::clamp(alpha, 0.0f, 1.0f));
        if ((*node)["style"].value("backgroundColor", Json::array()) == value)
            return false;
        MarkChanged(recordUndo);
        (*node)["style"]["backgroundColor"] = value;
        Revalidate();
        return true;
    }

    void UIDocumentDesignerModel::BeginTransaction()
    {
        if (!m_transactionActive && IsLoaded())
        {
            m_transactionBefore = m_document;
            m_transactionActive = true;
        }
    }

    void UIDocumentDesignerModel::EndTransaction()
    {
        if (!m_transactionActive)
            return;

        if (m_document != m_transactionBefore)
        {
            m_undo.push_back(std::move(m_transactionBefore));
            if (m_undo.size() > kMaxUndoEntries)
                m_undo.erase(m_undo.begin());
            m_redo.clear();
            m_dirty = true;
        }
        m_transactionBefore = Json {};
        m_transactionActive = false;
    }

    bool UIDocumentDesignerModel::Undo()
    {
        if (m_undo.empty() || m_transactionActive)
            return false;
        m_redo.push_back(m_document);
        auto restored = std::move(m_undo.back());
        m_undo.pop_back();
        Restore(std::move(restored));
        return true;
    }

    bool UIDocumentDesignerModel::Redo()
    {
        if (m_redo.empty() || m_transactionActive)
            return false;
        m_undo.push_back(m_document);
        auto restored = std::move(m_redo.back());
        m_redo.pop_back();
        Restore(std::move(restored));
        return true;
    }

    bool UIDocumentDesignerModel::BuildRuntimeDocument(
        NLS::Engine::UI::UIDocument& document,
        std::vector<NLS::Engine::UI::UIDocumentDiagnostic>* diagnostics) const
    {
        auto parsed = NLS::Engine::UI::UIDocument::LoadFromJson(m_document.dump(), diagnostics);
        if (!parsed.has_value())
            return false;
        document = std::move(*parsed);
        return true;
    }

    UIDocumentDesignerModel::Json UIDocumentDesignerModel::CreateNode(
        const std::string_view type,
        const std::string_view name)
    {
        Json node {
            { "nodeGuid", NLS::Guid::New().ToString() },
            { "type", std::string(type) },
            { "name", std::string(name) },
            { "layout", {
                { "mode", "Absolute" },
                { "size", Json::array({ 160.0f, 32.0f }) }
            } }
        };
        if (type == "Text" || type == "RichText" || type == "Button" || type == "TextField")
            node["text"] = DefaultNodeName(type);
        if (type == "Panel")
            node["style"] = {{ "backgroundColor", Color(0.12f, 0.13f, 0.16f, 1.0f) }};
        return node;
    }

    UIDocumentDesignerModel::Json UIDocumentDesignerModel::CreateEmptyDocument(
        const std::string_view documentName)
    {
        auto root = CreateNode("Panel", documentName.empty() ? "Root" : documentName);
        root["layout"] = {
            { "mode", "Overlay" },
            { "size", Json::array({ 1920.0f, 1080.0f }) }
        };
        root["style"] = {{ "backgroundColor", Color(0.0f, 0.0f, 0.0f, 0.0f) }};

        return {
            { "version", 1u },
            { "documentId", NLS::Guid::New().ToString() },
            { "root", std::move(root) }
        };
    }

    UIDocumentDesignerModel::Json* UIDocumentDesignerModel::FindNodeRecursive(
        Json& node,
        const std::string_view nodeGuid)
    {
        if (node.value("nodeGuid", std::string {}) == nodeGuid)
            return &node;
        const auto children = node.find("children");
        if (children == node.end() || !children->is_array())
            return nullptr;
        for (auto& child : *children)
        {
            if (auto* found = FindNodeRecursive(child, nodeGuid))
                return found;
        }
        return nullptr;
    }

    const UIDocumentDesignerModel::Json* UIDocumentDesignerModel::FindNodeRecursive(
        const Json& node,
        const std::string_view nodeGuid)
    {
        if (node.value("nodeGuid", std::string {}) == nodeGuid)
            return &node;
        const auto children = node.find("children");
        if (children == node.end() || !children->is_array())
            return nullptr;
        for (const auto& child : *children)
        {
            if (const auto* found = FindNodeRecursive(child, nodeGuid))
                return found;
        }
        return nullptr;
    }

    UIDocumentDesignerModel::Json* UIDocumentDesignerModel::FindParentRecursive(
        Json& node,
        const std::string_view childNodeGuid)
    {
        const auto children = node.find("children");
        if (children == node.end() || !children->is_array())
            return nullptr;
        for (auto& child : *children)
        {
            if (child.value("nodeGuid", std::string {}) == childNodeGuid)
                return &node;
            if (auto* found = FindParentRecursive(child, childNodeGuid))
                return found;
        }
        return nullptr;
    }

    bool UIDocumentDesignerModel::EnsureStableIdentifiers(Json& document)
    {
        bool changed = false;
        if (!document.contains("version"))
        {
            document["version"] = 1u;
            changed = true;
        }
        if (!document.contains("documentId") || !document["documentId"].is_string() ||
            !NLS::Guid::TryParse(document.value("documentId", std::string {})).has_value())
        {
            document["documentId"] = NLS::Guid::New().ToString();
            changed = true;
        }
        if (!document.contains("root") || !document["root"].is_object())
        {
            document["root"] = CreateNode("Panel", "Root");
            changed = true;
        }

        const auto ensureNode = [&changed](auto&& self, Json& node) -> void
        {
            if (!node.contains("nodeGuid") || !node["nodeGuid"].is_string() ||
                !NLS::Guid::TryParse(node.value("nodeGuid", std::string {})).has_value())
            {
                node["nodeGuid"] = NLS::Guid::New().ToString();
                changed = true;
            }
            if (!node.contains("type") || !node["type"].is_string())
            {
                node["type"] = "Panel";
                changed = true;
            }
            if (node.contains("children") && node["children"].is_array())
            {
                for (auto& child : node["children"])
                    self(self, child);
            }
        };
        ensureNode(ensureNode, document["root"]);
        return changed;
    }

    bool UIDocumentDesignerModel::IsSupportedElementType(const std::string_view type)
    {
        return type == "Panel" || type == "Image" || type == "NineSlice" || type == "Text" ||
            type == "RichText" || type == "Button" || type == "Toggle" || type == "Radio" ||
            type == "Slider" || type == "ProgressBar" || type == "TextField" ||
            type == "Dropdown" || type == "ScrollView" || type == "Scrollbar" || type == "ListView";
    }

    bool UIDocumentDesignerModel::IsSupportedLayoutMode(const std::string_view mode)
    {
        return mode == "Absolute" || mode == "Stack" || mode == "Flex" || mode == "Grid" ||
            mode == "Overlay" || mode == "Scroll";
    }

    bool UIDocumentDesignerModel::WriteTextFileAtomically(
        const std::filesystem::path& path,
        const std::string& text)
    {
        std::error_code error;
        if (!path.parent_path().empty())
        {
            std::filesystem::create_directories(path.parent_path(), error);
            if (error)
                return false;
        }

        const auto temporaryPath = path.string() + ".tmp";
        {
            std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
            if (!output)
                return false;
            output << text;
            if (!output.good())
                return false;
        }

        std::filesystem::rename(temporaryPath, path, error);
        if (!error)
            return true;
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(temporaryPath, path, error);
        if (!error)
            return true;
        std::filesystem::remove(temporaryPath, error);
        return false;
    }

    void UIDocumentDesignerModel::RecordUndo()
    {
        if (m_transactionActive)
            return;
        m_undo.push_back(m_document);
        if (m_undo.size() > kMaxUndoEntries)
            m_undo.erase(m_undo.begin());
        m_redo.clear();
    }

    void UIDocumentDesignerModel::MarkChanged(const bool recordUndo)
    {
        if (recordUndo)
            RecordUndo();
        m_dirty = true;
    }

    void UIDocumentDesignerModel::Revalidate()
    {
        m_diagnostics.clear();
        std::vector<NLS::Engine::UI::UIDocumentDiagnostic> runtimeDiagnostics;
        (void)NLS::Engine::UI::UIDocument::LoadFromJson(m_document.dump(), &runtimeDiagnostics);
        for (const auto& diagnostic : runtimeDiagnostics)
            m_diagnostics.push_back({ diagnostic.path, diagnostic.message, diagnostic.error });
    }

    void UIDocumentDesignerModel::Restore(Json document)
    {
        m_document = std::move(document);
        EnsureStableIdentifiers(m_document);
        if (FindNode(m_selectedNodeGuid) == nullptr)
            m_selectedNodeGuid = m_document["root"].value("nodeGuid", std::string {});
        m_dirty = true;
        Revalidate();
    }
}
