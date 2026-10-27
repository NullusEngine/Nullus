#pragma once

#include "Object/Object.h"
#include "EngineDef.h"
#include "Rendering/UI/UIDrawSnapshot.h"
#include "UIElement.h"
#include "UISignal.h"
#include "UIResourceReference.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace NLS::Engine::UI
{
    struct UIDocumentDiagnostic
    {
        std::string path;
        std::string message;
        bool error = true;
    };

    class UIDocumentInstance;

    class NLS_ENGINE_API UIDocument final
    {
    public:
        static constexpr uint32_t CurrentSchemaVersion = 1u;

        struct NodeDefinition;

        UIDocument() = default;
        UIDocument(UIDocument&&) noexcept = default;
        UIDocument& operator=(UIDocument&&) noexcept = default;
        UIDocument(const UIDocument&) = delete;
        UIDocument& operator=(const UIDocument&) = delete;

        static std::optional<UIDocument> LoadFromJson(
            std::string_view jsonText,
            std::vector<UIDocumentDiagnostic>* diagnostics = nullptr);
        static std::optional<UIDocument> LoadFromFile(
            const std::filesystem::path& path,
            std::vector<UIDocumentDiagnostic>* diagnostics = nullptr);
        static std::optional<UIDocument> LoadFromArtifactFile(
            const std::filesystem::path& path,
            std::vector<UIDocumentDiagnostic>* diagnostics = nullptr);

        [[nodiscard]] uint32_t GetSchemaVersion() const { return m_schemaVersion; }
        [[nodiscard]] const UINodeGuid& GetDocumentId() const { return m_documentId; }
        [[nodiscard]] const std::string& GetSourceHash() const { return m_sourceHash; }
        [[nodiscard]] const std::vector<std::string>& GetDependencies() const { return m_dependencies; }
        [[nodiscard]] const UIDocument::NodeDefinition& GetRootDefinition() const { return m_root; }

        std::unique_ptr<UIDocumentInstance> Instantiate() const;

        struct NodeDefinition
        {
            UINodeGuid nodeGuid = UINodeGuid::Empty();
            UIElementType type = UIElementType::Unknown;
            std::string name;
            std::string className;
            UILayoutParams layout;
            UIStyle style;
            std::string text;
            bool passwordMode = false;
            bool requiresRenderTarget = false;
            uint64_t textureId = 0u;
            uint32_t textureGeneration = 0u;
            // Authoring references. Runtime textureId/fontAssetId are resolved
            // by the host through IUIDocumentResourceResolver; legacy documents
            // may still carry raw ids without references.
            std::optional<UITextureReference> texture;
            std::optional<UIFontReference> font;
            UIInsets nineSlice;
            uint64_t fontAssetId = 0u;
            uint16_t fontPixelSize = 16u;
            std::vector<NodeDefinition> children;
        };

    private:
        uint32_t m_schemaVersion = CurrentSchemaVersion;
        UINodeGuid m_documentId = UINodeGuid::Empty();
        std::string m_sourceHash;
        std::vector<std::string> m_dependencies;
        NodeDefinition m_root;
    };

    // Recursively collects authoring texture/font references for the importer's
    // dependency scan. References are returned in document order.
    NLS_ENGINE_API void CollectUIResourceReferences(
        const UIDocument& document,
        std::vector<UITextureReference>& textures,
        std::vector<UIFontReference>& fonts);

    class NLS_ENGINE_API UIDocumentInstance final : public NLS::Object
    {
    public:
        UIDocumentInstance(const UIDocument& document, std::unique_ptr<UIElement> root);
        ~UIDocumentInstance() override = default;

        [[nodiscard]] const UINodeGuid& GetDocumentId() const { return m_documentId; }
        [[nodiscard]] uint32_t GetGeneration() const { return m_generation; }
        [[nodiscard]] UIElement* GetRoot() { return m_root.get(); }
        [[nodiscard]] const UIElement* GetRoot() const { return m_root.get(); }
        UIElement* GetElement(const UINodeGuid& nodeGuid);
        const UIElement* GetElement(const UINodeGuid& nodeGuid) const;
        UIElement* GetElementByName(const std::string& name);
        const UIElement* GetElementByName(const std::string& name) const;

        void UpdateLayout(UIVector2 viewportSize, float dpiScale = 1.0f);
        [[nodiscard]] std::shared_ptr<NLS::Render::UI::UIDrawSnapshot> BuildDrawSnapshot(
            uint64_t frameId,
            uint32_t viewportWidth,
            uint32_t viewportHeight,
            float dpiScale = 1.0f) const;
        void Tick(float deltaSeconds);
        // Resolves authoring texture/font references against the installed
        // IUIDocumentResourceResolver and updates elements when async loads
        // complete. Safe to call every frame; no-op for documents without
        // resource references.
        void RefreshResources();
        [[nodiscard]] UISignalBus& Signals() { return m_signals; }
        [[nodiscard]] const UISignalBus& Signals() const { return m_signals; }
        [[nodiscard]] bool RequiresRenderTarget() const { return m_requiresRenderTarget; }

        // Rebuilds the tree while retaining compatible per-node state by
        // NodeGuid. Script handles are intentionally invalidated by the
        // generation increment, matching native-object hot reload semantics.
        bool Reload(const UIDocument& document, std::vector<UIDocumentDiagnostic>* diagnostics = nullptr);

    private:
        friend class UIDocument;

        struct ResourceSlot
        {
            UINodeGuid nodeGuid;
            UITextureReference texture;
            UIFontReference font;
            bool hasTexture = false;
            bool hasFont = false;
            uint64_t textureId = 0u;
            uint32_t textureGeneration = 0u;
            uint64_t fontId = 0u;
        };

        static std::unique_ptr<UIElement> BuildElementTree(
            const UIDocument::NodeDefinition& definition,
            std::vector<ResourceSlot>* resourceSlots);
        static void CopyCompatibleState(const UIElement& oldElement, UIElement& newElement);
        static bool ContainsRenderTargetRequirement(const UIDocument::NodeDefinition& definition);

        UINodeGuid m_documentId = UINodeGuid::Empty();
        uint32_t m_generation = 1u;
        std::unique_ptr<UIElement> m_root;
        std::vector<ResourceSlot> m_resourceSlots;
        UIVector2 m_lastViewport;
        float m_lastDpiScale = 1.0f;
        // The render thread receives a value snapshot. Keeping the last
        // immutable geometry here lets a static document skip layout and
        // geometry rebuilds while still producing a frame-scoped hand-off.
        mutable std::shared_ptr<NLS::Render::UI::UIDrawSnapshot> m_cachedSnapshot;
        mutable UIVector2 m_cachedViewport;
        mutable float m_cachedDpiScale = 1.0f;
        // Text geometry is resolved against the live font atlas, so the cache is
        // only valid for the atlas layout and texture identity it was built with.
        mutable uint64_t m_cachedFontAtlasTextureId = 0u;
        mutable uint32_t m_cachedFontAtlasTextureGeneration = 0u;
        mutable uint64_t m_cachedFontAtlasContentGeneration = 0u;
        bool m_requiresRenderTarget = false;
        UISignalBus m_signals;
    };
}
