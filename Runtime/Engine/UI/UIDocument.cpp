#include "UIDocument.h"

#include "UIDocumentResourceResolver.h"

#include "Rendering/UI/UIFontTextShaper.h"
#include "Rendering/UI/UIFontAtlas.h"

#include "Assets/NativeArtifactContainer.h"
#include "Rendering/UI/UIDrawSnapshot.h"

#include <Json/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <functional>
#include <sstream>
#include <valarray>
#include <unordered_set>

namespace NLS::Engine::UI
{
    namespace
    {
        using Json = nlohmann::json;

        void AddDiagnostic(
            std::vector<UIDocumentDiagnostic>* diagnostics,
            std::string path,
            std::string message)
        {
            if (diagnostics)
                diagnostics->push_back({ std::move(path), std::move(message), true });
        }

        bool ReadVector2(const Json& value, UIVector2& output)
        {
            if (!value.is_array() || value.size() < 2u || !value[0].is_number() || !value[1].is_number())
                return false;
            output = { value[0].get<float>(), value[1].get<float>() };
            return true;
        }

        bool ReadColor(const Json& value, UIColor& output)
        {
            if (!value.is_array() || value.size() < 3u)
                return false;
            if (!value[0].is_number() || !value[1].is_number() || !value[2].is_number())
                return false;
            output.r = value[0].get<float>();
            output.g = value[1].get<float>();
            output.b = value[2].get<float>();
            output.a = value.size() > 3u && value[3].is_number() ? value[3].get<float>() : 1.0f;
            return true;
        }

        template <typename Reference>
        bool ParseResourceReference(
            const Json& json,
            const std::string& path,
            std::optional<Reference>& output,
            std::vector<UIDocumentDiagnostic>* diagnostics)
        {
            if (!json.is_object())
            {
                AddDiagnostic(diagnostics, path, "reference must be an object");
                return false;
            }

            Reference reference;
            if (json.contains("assetId"))
            {
                if (!json["assetId"].is_string())
                {
                    AddDiagnostic(diagnostics, path + ".assetId", "assetId must be a GUID string");
                    return false;
                }
                const auto text = json["assetId"].get<std::string>();
                if (!text.empty() && !NLS::Guid::TryParse(text).has_value())
                {
                    AddDiagnostic(diagnostics, path + ".assetId", "assetId is not a valid GUID");
                    return false;
                }
                reference.SetAssetIdString(text);
            }
            if (json.contains("subAssetKey"))
            {
                if (!json["subAssetKey"].is_string())
                {
                    AddDiagnostic(diagnostics, path + ".subAssetKey", "subAssetKey must be a string");
                    return false;
                }
                reference.SetSubAssetKey(json["subAssetKey"].get<std::string>());
            }
            if (json.contains("path"))
            {
                if (!json["path"].is_string())
                {
                    AddDiagnostic(diagnostics, path + ".path", "path must be a string");
                    return false;
                }
                reference.SetResourcePath(json["path"].get<std::string>());
            }
            if (!reference.IsNull())
                output = std::move(reference);
            return true;
        }

        bool ParseNode(
            const Json& json,
            UIDocument::NodeDefinition& output,
            const std::string& path,
            const UINodeGuid& documentId,
            std::unordered_set<UINodeGuid>& seenGuids,
            std::vector<UIDocumentDiagnostic>* diagnostics,
            uint32_t depth)        {
            if (depth > 1024u)
            {
                AddDiagnostic(diagnostics, path, "maximum UI node depth exceeded");
                return false;
            }
            if (!json.is_object())
            {
                AddDiagnostic(diagnostics, path, "node must be an object");
                return false;
            }

            const auto typeValue = json.value("type", "Panel");
            output.type = ElementTypeFromString(typeValue);
            if (output.type == UIElementType::Unknown)
            {
                AddDiagnostic(diagnostics, path + ".type", "unknown UI element type: " + typeValue);
                return false;
            }
            output.name = json.value("name", std::string {});
            if (json.contains("className"))
            {
                if (!json["className"].is_string())
                {
                    AddDiagnostic(diagnostics, path + ".className", "className must be a string");
                    return false;
                }
                output.className = json["className"].get<std::string>();
            }
            if (json.contains("class"))
            {
                if (!json["class"].is_string())
                {
                    AddDiagnostic(diagnostics, path + ".class", "class must be a string");
                    return false;
                }
                if (output.className.empty())
                    output.className = json["class"].get<std::string>();
            }

            if (json.contains("nodeGuid"))
            {
                if (!json["nodeGuid"].is_string())
                {
                    AddDiagnostic(diagnostics, path + ".nodeGuid", "nodeGuid must be a GUID string");
                    return false;
                }
                const auto parsed = NLS::Guid::TryParse(json["nodeGuid"].get<std::string>());
                if (!parsed.has_value())
                {
                    AddDiagnostic(diagnostics, path + ".nodeGuid", "nodeGuid is not a valid GUID");
                    return false;
                }
                output.nodeGuid = *parsed;
            }
            else
            {
                output.nodeGuid = NLS::Guid::NewDeterministic(
                    "nui-node:" + documentId.ToString() + ":" + path);
            }
            if (!seenGuids.insert(output.nodeGuid).second)
            {
                AddDiagnostic(diagnostics, path + ".nodeGuid", "duplicate nodeGuid");
                return false;
            }

            if (json.contains("layout") && json["layout"].is_object())
            {
                const auto& layout = json["layout"];
                if (layout.contains("mode") && layout["mode"].is_string())
                    output.layout.mode = LayoutModeFromString(layout["mode"].get<std::string>());
                if (layout.contains("direction") && layout["direction"].is_string())
                    output.layout.direction = layout["direction"] == "Horizontal"
                        ? UIStackDirection::Horizontal : UIStackDirection::Vertical;
                ReadVector2(layout.value("anchorMin", Json {}), output.layout.anchorMin);
                ReadVector2(layout.value("anchorMax", Json {}), output.layout.anchorMax);
                ReadVector2(layout.value("pivot", Json {}), output.layout.pivot);
                ReadVector2(layout.value("offsetMin", Json {}), output.layout.offsetMin);
                ReadVector2(layout.value("offsetMax", Json {}), output.layout.offsetMax);
                ReadVector2(layout.value("size", Json {}), output.layout.size);
                ReadVector2(layout.value("minSize", Json {}), output.layout.minSize);
                ReadVector2(layout.value("maxSize", Json {}), output.layout.maxSize);
                ReadVector2(layout.value("margin", Json {}), output.layout.margin);
                ReadVector2(layout.value("padding", Json {}), output.layout.padding);
                if (layout.contains("aspectRatio") && layout["aspectRatio"].is_number())
                    output.layout.aspectRatio = std::max(0.0f, layout["aspectRatio"].get<float>());
                if (layout.contains("spacing") && layout["spacing"].is_number())
                    output.layout.spacing = std::max(0.0f, layout["spacing"].get<float>());
                if (layout.contains("flexGrow") && layout["flexGrow"].is_number())
                    output.layout.flexGrow = std::max(0.0f, layout["flexGrow"].get<float>());
                if (layout.contains("gridColumns") && layout["gridColumns"].is_number_unsigned())
                    output.layout.gridColumns = std::max(1u, layout["gridColumns"].get<uint32_t>());
                if (layout.contains("clipChildren") && layout["clipChildren"].is_boolean())
                    output.layout.clipChildren = layout["clipChildren"].get<bool>();
            }

            if (json.contains("style") && json["style"].is_object())
            {
                const auto& style = json["style"];
                ReadColor(style.value("backgroundColor", Json {}), output.style.backgroundColor);
                ReadColor(style.value("color", Json {}), output.style.color);
                if (style.contains("opacity") && style["opacity"].is_number())
                    output.style.opacity = std::clamp(style["opacity"].get<float>(), 0.0f, 1.0f);
                if (style.contains("visible") && style["visible"].is_boolean())
                    output.style.visible = style["visible"].get<bool>();
                if (style.contains("enabled") && style["enabled"].is_boolean())
                    output.style.enabled = style["enabled"].get<bool>();
                if (style.contains("interactive") && style["interactive"].is_boolean())
                {
                    output.style.interactive = style["interactive"].get<bool>();
                    output.style.interactiveSpecified = true;
                }
            }

            if (json.contains("properties") && json["properties"].is_object())
            {
                const auto& properties = json["properties"];
                if (properties.contains("text") && properties["text"].is_string())
                    output.text = properties["text"].get<std::string>();
            }
            if (json.contains("text") && json["text"].is_string())
                output.text = json["text"].get<std::string>();
            if (json.contains("password") && json["password"].is_boolean())
                output.passwordMode = json["password"].get<bool>();
            if (json.contains("renderTarget") && json["renderTarget"].is_boolean())
                output.requiresRenderTarget = json["renderTarget"].get<bool>();

            if (json.contains("textureId") && json["textureId"].is_number_unsigned())
            {
                output.textureId = json["textureId"].get<uint64_t>();
                output.textureGeneration = json.value("textureGeneration", 1u);
            }
            else if (json.contains("texture") && json["texture"].is_object())
            {
                const auto& texture = json["texture"];
                if (texture.contains("id") && texture["id"].is_number_unsigned())
                {
                    output.textureId = texture["id"].get<uint64_t>();
                    output.textureGeneration = texture.value("generation", 1u);
                }
            }
            if (json.contains("fontAssetId") && json["fontAssetId"].is_number_unsigned())
                output.fontAssetId = json["fontAssetId"].get<uint64_t>();
            if (json.contains("fontPixelSize") && json["fontPixelSize"].is_number_unsigned())
            {
                output.fontPixelSize = static_cast<uint16_t>(std::clamp<uint32_t>(
                    json["fontPixelSize"].get<uint32_t>(),
                    1u,
                    65535u));
            }

            if (json.contains("texture") &&
                !ParseResourceReference(
                    json["texture"],
                    path + ".texture",
                    output.texture,
                    diagnostics))
            {
                return false;
            }
            if (json.contains("font") &&
                !ParseResourceReference(
                    json["font"],
                    path + ".font",
                    output.font,
                    diagnostics))
            {
                return false;
            }

            if (json.contains("nineSlice"))
            {
                const auto& nineSlice = json["nineSlice"];
                if (!nineSlice.is_object())
                {
                    AddDiagnostic(diagnostics, path + ".nineSlice", "nineSlice must be an object");
                    return false;
                }
                const auto readInset = [&](const char* key, float& target)
                {
                    const auto iterator = nineSlice.find(key);
                    if (iterator == nineSlice.end())
                        return true;
                    if (!iterator->is_number())
                    {
                        AddDiagnostic(
                            diagnostics,
                            path + ".nineSlice." + key,
                            "nineSlice border must be a number");
                        return false;
                    }
                    target = std::max(0.0f, iterator->get<float>());
                    return true;
                };
                if (!readInset("left", output.nineSlice.left) ||
                    !readInset("top", output.nineSlice.top) ||
                    !readInset("right", output.nineSlice.right) ||
                    !readInset("bottom", output.nineSlice.bottom))
                {
                    return false;
                }
            }

            if (json.contains("children"))
            {
                if (!json["children"].is_array())
                {
                    AddDiagnostic(diagnostics, path + ".children", "children must be an array");
                    return false;
                }
                output.children.reserve(json["children"].size());
                for (size_t index = 0u; index < json["children"].size(); ++index)
                {
                    UIDocument::NodeDefinition child;
                    if (!ParseNode(
                        json["children"][index],
                        child,
                        path + ".children[" + std::to_string(index) + "]",
                        documentId,
                        seenGuids,
                        diagnostics,
                        depth + 1u))
                        return false;
                    output.children.push_back(std::move(child));
                }
            }
            return true;
        }

        std::string HashSource(std::string_view text)
        {
            // std::hash<std::string_view> is implementation-defined and may
            // change between processes or standard-library versions. Asset
            // freshness and artifact manifests need a stable source identity.
            constexpr uint64_t offsetBasis = 14695981039346656037ull;
            constexpr uint64_t prime = 1099511628211ull;
            uint64_t hash = offsetBasis;
            for (const unsigned char character : text)
            {
                hash ^= static_cast<uint64_t>(character);
                hash *= prime;
            }
            return std::to_string(hash);
        }
    }

    std::optional<UIDocument> UIDocument::LoadFromJson(
        const std::string_view jsonText,
        std::vector<UIDocumentDiagnostic>* diagnostics)
    {
        Json json;
        try
        {
            json = Json::parse(jsonText.begin(), jsonText.end());
        }
        catch (const std::exception& exception)
        {
            AddDiagnostic(diagnostics, "$", std::string("invalid JSON: ") + exception.what());
            return std::nullopt;
        }
        if (!json.is_object())
        {
            AddDiagnostic(diagnostics, "$", "document root must be an object");
            return std::nullopt;
        }

        UIDocument document;
        document.m_schemaVersion = json.value("version", 0u);
        if (document.m_schemaVersion != CurrentSchemaVersion)
        {
            AddDiagnostic(
                diagnostics,
                "$.version",
				"unsupported .ui schema version " + std::to_string(document.m_schemaVersion));
            return std::nullopt;
        }

        const auto documentIdValue = json.value("documentId", std::string {});
        if (!documentIdValue.empty())
        {
            const auto parsed = NLS::Guid::TryParse(documentIdValue);
            if (!parsed.has_value())
            {
                AddDiagnostic(diagnostics, "$.documentId", "documentId is not a valid GUID");
                return std::nullopt;
            }
            document.m_documentId = *parsed;
        }
        else
        {
            document.m_documentId = NLS::Guid::NewDeterministic("nui-document:" + std::string(jsonText));
        }
        document.m_sourceHash = HashSource(jsonText);

        if (json.contains("dependencies"))
        {
            if (!json["dependencies"].is_array())
            {
                AddDiagnostic(diagnostics, "$.dependencies", "dependencies must be an array");
                return std::nullopt;
            }
            std::unordered_set<std::string> dependencies;
            for (const auto& dependency : json["dependencies"])
            {
                if (!dependency.is_string())
                {
                    AddDiagnostic(diagnostics, "$.dependencies", "dependency must be a string");
                    return std::nullopt;
                }
                const auto value = dependency.get<std::string>();
                if (value == document.m_documentId.ToString() || !dependencies.insert(value).second)
                {
                    AddDiagnostic(diagnostics, "$.dependencies", "cyclic or duplicate document dependency");
                    return std::nullopt;
                }
                document.m_dependencies.push_back(value);
            }
        }

        if (!json.contains("root"))
        {
            AddDiagnostic(diagnostics, "$.root", "document must contain a root node");
            return std::nullopt;
        }
        std::unordered_set<UINodeGuid> seenGuids;
        if (!ParseNode(json["root"], document.m_root, "$.root", document.m_documentId, seenGuids, diagnostics, 0u))
            return std::nullopt;
        return document;
    }

    std::optional<UIDocument> UIDocument::LoadFromFile(
        const std::filesystem::path& path,
        std::vector<UIDocumentDiagnostic>* diagnostics)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
			AddDiagnostic(diagnostics, path.string(), "could not open .ui document");
            return std::nullopt;
        }
        std::ostringstream contents;
        contents << input.rdbuf();
        return LoadFromJson(contents.str(), diagnostics);
    }

    std::optional<UIDocument> UIDocument::LoadFromArtifactFile(
        const std::filesystem::path& path,
        std::vector<UIDocumentDiagnostic>* diagnostics)
    {
        const auto payload = NLS::Core::Assets::ReadNativeArtifactPayloadTextFromFile(
            path,
            NLS::Core::Assets::ArtifactType::UIDocument,
            CurrentSchemaVersion);
        if (!payload.has_value())
        {
            AddDiagnostic(diagnostics, path.string(), "could not read .ui artifact container");
            return std::nullopt;
        }
        return LoadFromJson(payload->payload, diagnostics);
    }

    void CollectUIResourceReferences(
        const UIDocument& document,
        std::vector<UITextureReference>& textures,
        std::vector<UIFontReference>& fonts)
    {
        const auto visit = [&](auto&& self, const UIDocument::NodeDefinition& node) -> void
        {
            if (node.texture.has_value() && !node.texture->IsNull())
                textures.push_back(*node.texture);
            if (node.font.has_value() && !node.font->IsNull())
                fonts.push_back(*node.font);
            for (const auto& child : node.children)
                self(self, child);
        };
        visit(visit, document.GetRootDefinition());
    }

    std::unique_ptr<UIDocumentInstance> UIDocument::Instantiate() const
    {
        std::vector<UIDocumentInstance::ResourceSlot> resourceSlots;
        auto root = UIDocumentInstance::BuildElementTree(m_root, &resourceSlots);
        auto instance = std::make_unique<UIDocumentInstance>(*this, std::move(root));
        instance->m_resourceSlots = std::move(resourceSlots);
        instance->RefreshResources();
        return instance;
    }

    UIDocumentInstance::UIDocumentInstance(
        const UIDocument& document,
        std::unique_ptr<UIElement> root)
        : m_documentId(document.GetDocumentId()), m_root(std::move(root))
    {
        m_requiresRenderTarget = ContainsRenderTargetRequirement(document.GetRootDefinition());
    }

    UIElement* UIDocumentInstance::GetElement(const UINodeGuid& nodeGuid)
    {
        return m_root ? m_root->FindByGuid(nodeGuid) : nullptr;
    }

    const UIElement* UIDocumentInstance::GetElement(const UINodeGuid& nodeGuid) const
    {
        return m_root ? m_root->FindByGuid(nodeGuid) : nullptr;
    }

    UIElement* UIDocumentInstance::GetElementByName(const std::string& name)
    {
        return m_root ? m_root->FindByName(name) : nullptr;
    }

    const UIElement* UIDocumentInstance::GetElementByName(const std::string& name) const
    {
        return m_root ? m_root->FindByName(name) : nullptr;
    }

    void UIDocumentInstance::UpdateLayout(const UIVector2 viewportSize, const float dpiScale)
    {
        const float normalizedDpiScale = dpiScale > 0.0f ? dpiScale : 1.0f;
        const bool viewportChanged = m_lastViewport != viewportSize ||
            std::abs(m_lastDpiScale - normalizedDpiScale) > 0.0001f;
        m_lastViewport = viewportSize;
        m_lastDpiScale = normalizedDpiScale;
        if (!m_root)
            return;

        // Measure/arrange is intentionally main-thread-only. A clean tree
        // with an unchanged viewport can be consumed as-is by the renderer.
        if (!viewportChanged && !m_root->IsLayoutDirty())
            return;
        m_root->Measure(viewportSize);
        m_root->Arrange({ 0.0f, 0.0f, viewportSize.x, viewportSize.y });
    }

    std::shared_ptr<NLS::Render::UI::UIDrawSnapshot> UIDocumentInstance::BuildDrawSnapshot(
        const uint64_t frameId,
        const uint32_t viewportWidth,
        const uint32_t viewportHeight,
        const float dpiScale) const
    {
        const float normalizedDpiScale = dpiScale > 0.0f ? dpiScale : 1.0f;
        const UIVector2 viewport {
            static_cast<float>(viewportWidth),
            static_cast<float>(viewportHeight) };
        const bool cacheMatches = m_cachedSnapshot != nullptr &&
            m_cachedViewport == viewport &&
            std::abs(m_cachedDpiScale - normalizedDpiScale) <= 0.0001f &&
            m_root != nullptr &&
            !m_root->IsLayoutDirty() &&
            !m_root->IsPaintDirty() &&
            m_cachedFontAtlasTextureId == NLS::Render::UI::GetUiFontAtlasTextureId() &&
            m_cachedFontAtlasTextureGeneration ==
                NLS::Render::UI::GetUiFontAtlasTextureGeneration() &&
            m_cachedFontAtlasContentGeneration ==
                NLS::Render::UI::GetSharedUIFontAtlas().GetContentGeneration();

        if (cacheMatches)
        {
            auto snapshot = std::make_shared<NLS::Render::UI::UIDrawSnapshot>(*m_cachedSnapshot);
            snapshot->frameId = frameId;
            snapshot->viewportWidth = viewportWidth;
            snapshot->viewportHeight = viewportHeight;
            snapshot->dpiScale = normalizedDpiScale;
            return snapshot;
        }

        auto snapshot = std::make_shared<NLS::Render::UI::UIDrawSnapshot>();
        snapshot->frameId = frameId;
        snapshot->viewportWidth = viewportWidth;
        snapshot->viewportHeight = viewportHeight;
        snapshot->dpiScale = normalizedDpiScale;
        if (m_root)
        {
            m_root->BuildDrawSnapshot(*snapshot);
            m_root->ClearDirtyFlags();
        }
        // Text runs stay in the snapshot for tooling, but the renderer consumes
        // vertices, so real glyph quads are appended here.
        NLS::Render::UI::AppendTextQuadGeometry(*snapshot, frameId);
        m_cachedSnapshot = snapshot;
        m_cachedViewport = viewport;
        m_cachedDpiScale = normalizedDpiScale;
        m_cachedFontAtlasTextureId = NLS::Render::UI::GetUiFontAtlasTextureId();
        m_cachedFontAtlasTextureGeneration =
            NLS::Render::UI::GetUiFontAtlasTextureGeneration();
        m_cachedFontAtlasContentGeneration =
            NLS::Render::UI::GetSharedUIFontAtlas().GetContentGeneration();
        return snapshot;
    }

    void UIDocumentInstance::Tick(const float deltaSeconds)
    {
        (void)deltaSeconds;
        RefreshResources();
    }

    void UIDocumentInstance::RefreshResources()
    {
        if (m_resourceSlots.empty())
            return;

        const auto resolver = EnsureUIDocumentResourceResolver();
        if (!resolver)
            return;

        resolver->Pump();
        for (auto& slot : m_resourceSlots)
        {
            auto* element = GetElement(slot.nodeGuid);
            if (element == nullptr)
                continue;

            if (slot.hasTexture)
            {
                const auto resolved = resolver->ResolveTexture(slot.texture);
                if (resolved.has_value() && resolved->IsValid() &&
                    (resolved->id != slot.textureId ||
                     resolved->generation != slot.textureGeneration))
                {
                    slot.textureId = resolved->id;
                    slot.textureGeneration = resolved->generation;
                    element->SetTexture(
                        slot.textureId,
                        slot.textureGeneration,
                        resolved->width,
                        resolved->height);
                }
            }

            if (slot.hasFont)
            {
                const auto fontId = resolver->ResolveFont(slot.font);
                if (fontId.has_value() && *fontId != 0u && *fontId != slot.fontId)
                {
                    slot.fontId = *fontId;
                    element->SetFont(slot.fontId, element->GetFontPixelSize());
                }
            }
        }
    }

    bool UIDocumentInstance::Reload(
        const UIDocument& document,
        std::vector<UIDocumentDiagnostic>* diagnostics)
    {
        std::vector<ResourceSlot> nextResourceSlots;
        auto nextRoot = BuildElementTree(
            document.GetRootDefinition(),
            &nextResourceSlots);
        if (!nextRoot)
            return false;
        if (m_root)
        {
            std::function<void(const UIElement&, UIElement&)> copyStates =
                [&copyStates](const UIElement& oldElement, UIElement& newElement)
                {
                    CopyCompatibleState(oldElement, newElement);
                    for (auto& child : newElement.GetChildren())
                    {
                        if (const auto* oldChild = oldElement.FindByGuid(child->GetNodeGuid()))
                            copyStates(*oldChild, *child);
                    }
                };
            copyStates(*m_root, *nextRoot);
        }
        m_documentId = document.GetDocumentId();
        m_root = std::move(nextRoot);
        m_resourceSlots = std::move(nextResourceSlots);
        m_requiresRenderTarget = ContainsRenderTargetRequirement(document.GetRootDefinition());
        m_cachedSnapshot.reset();
        ++m_generation;
        if (m_lastViewport.x > 0.0f && m_lastViewport.y > 0.0f)
            UpdateLayout(m_lastViewport, m_lastDpiScale);
        RefreshResources();
        (void)diagnostics;
        return true;
    }

    std::unique_ptr<UIElement> UIDocumentInstance::BuildElementTree(
        const UIDocument::NodeDefinition& definition,
        std::vector<ResourceSlot>* resourceSlots)
    {
        auto element = std::make_unique<UIElement>(definition.type, definition.name, definition.nodeGuid);
        element->SetClassName(definition.className);
        element->SetLayout(definition.layout);
        auto style = definition.style;
        if (!style.interactiveSpecified &&
            (definition.type == UIElementType::Button ||
             definition.type == UIElementType::Toggle ||
             definition.type == UIElementType::Radio ||
             definition.type == UIElementType::Slider ||
             definition.type == UIElementType::TextField ||
             definition.type == UIElementType::Dropdown ||
             definition.type == UIElementType::ListView))
        {
            style.interactive = true;
        }
        element->SetStyle(style);
        element->SetText(definition.text);
        element->SetPasswordMode(definition.passwordMode);
        element->SetTexture(definition.textureId, definition.textureGeneration);
        element->SetNineSlice(definition.nineSlice);
        element->SetFont(definition.fontAssetId, definition.fontPixelSize);
        if (resourceSlots != nullptr &&
            (definition.texture.has_value() || definition.font.has_value()))
        {
            ResourceSlot slot;
            slot.nodeGuid = definition.nodeGuid;
            if (definition.texture.has_value())
            {
                slot.hasTexture = true;
                slot.texture = *definition.texture;
            }
            if (definition.font.has_value())
            {
                slot.hasFont = true;
                slot.font = *definition.font;
            }
            resourceSlots->push_back(std::move(slot));
        }
        for (const auto& child : definition.children)
            element->AddChild(BuildElementTree(child, resourceSlots));
        return element;
    }

    bool UIDocumentInstance::ContainsRenderTargetRequirement(
        const UIDocument::NodeDefinition& definition)
    {
        if (definition.requiresRenderTarget)
            return true;
        for (const auto& child : definition.children)
            if (ContainsRenderTargetRequirement(child))
                return true;
        return false;
    }

    void UIDocumentInstance::CopyCompatibleState(const UIElement& oldElement, UIElement& newElement)
    {
        if (oldElement.GetElementType() != newElement.GetElementType())
            return;
        if (!oldElement.GetText().empty())
            newElement.SetText(oldElement.GetText());
        // Keep user-owned control state when a document is rebuilt.  These
        // values are intentionally keyed by NodeGuid by the caller, so a
        // source edit can replace unrelated nodes without losing input or
        // scroll position in compatible controls.
        newElement.SetValue(oldElement.GetValue());
        newElement.SetChecked(oldElement.IsChecked());
        newElement.SetSelected(oldElement.IsSelected());
        newElement.SetScrollOffset(oldElement.GetScrollOffset());
        newElement.SetTextSelection(
            oldElement.GetTextSelectionAnchor(),
            oldElement.GetTextCursor());
        const auto oldStyle = oldElement.GetStyle();
        auto newStyle = newElement.GetStyle();
        newStyle.visible = oldStyle.visible;
        newStyle.enabled = oldStyle.enabled;
        newStyle.interactive = oldStyle.interactive;
        newStyle.opacity = oldStyle.opacity;
        newElement.SetStyle(newStyle);
    }
}
