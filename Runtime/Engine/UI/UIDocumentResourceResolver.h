#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include "EngineDef.h"
#include "UIResourceReference.h"

namespace NLS::Engine::UI
{
    struct NLS_ENGINE_API UIResolvedTexture
    {
        uint64_t id = 0u;
        uint32_t generation = 0u;
        uint32_t width = 0u;
        uint32_t height = 0u;

        [[nodiscard]] bool IsValid() const { return id != 0u && generation != 0u; }
    };

    // Host service that turns durable authoring references into runtime texture
    // ids / font face ids. UIDocumentInstance resolves references on build and
    // refreshes them from Tick, so async loading never rebuilds the element tree.
    class NLS_ENGINE_API IUIDocumentResourceResolver
    {
    public:
        virtual ~IUIDocumentResourceResolver() = default;

        // Returns nullopt while the asset is still loading or unavailable.
        virtual std::optional<UIResolvedTexture> ResolveTexture(
            const UITextureReference& reference) = 0;

        // Returns a stable process-local font face id (2 and above); id 1 is the
        // default face. Nullopt means the font is not loaded yet.
        virtual std::optional<uint64_t> ResolveFont(
            const UIFontReference& reference) = 0;

        // Advances async work. Called once per document instance per frame; must
        // stay cheap when there is nothing pending.
        virtual void Pump() {}

        // Drops cached state after a reimport so the next Resolve reloads it.
        virtual void Invalidate(const UITextureReference& reference) { (void)reference; }
        virtual void Invalidate(const UIFontReference& reference) { (void)reference; }

        // Drops every cached texture/font entry backed by an asset. Used by the
        // editor after an import so live Game View canvases pick up new bytes.
        virtual void InvalidateAsset(const Core::Assets::AssetId& assetId) { (void)assetId; }
    };

    NLS_ENGINE_API void SetUIDocumentResourceResolver(
        std::shared_ptr<IUIDocumentResourceResolver> resolver);
    NLS_ENGINE_API std::shared_ptr<IUIDocumentResourceResolver>
        GetUIDocumentResourceResolver();
    NLS_ENGINE_API void ClearUIDocumentResourceResolver();

    // Shared resolver backed by RuntimeAssetDatabase, TextureManager and
    // DriverUIAccess. Resolution returns nullopt when those services are not
    // installed (for example in a headless unit test); it never throws.
    NLS_ENGINE_API std::shared_ptr<IUIDocumentResourceResolver>
        CreateRuntimeUIDocumentResourceResolver();

    // Returns the installed resolver, or lazily installs the runtime resolver
    // when a document actually contains resource references. Hosts that need a
    // custom resolver (editor preview, tests) call Set... before documents tick.
    NLS_ENGINE_API std::shared_ptr<IUIDocumentResourceResolver>
        EnsureUIDocumentResourceResolver();
}
