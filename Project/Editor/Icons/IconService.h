#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <Math/Vector2.h>
#include <Rendering/UI/UiDrawDataSnapshot.h>

#include "Icons/EditorIconCatalog.h"

namespace NLS::Render::Resources
{
    class Texture2D;
}

namespace NLS::Render::RHI
{
    class RHITextureView;
}

namespace NLS::Editor::Icons
{
    /**
    * A bitmap icon ready to draw. The texture id is the engine-native,
    * generation-carrying UI texture id; panels never cache it across frames.
    */
    struct ResolvedIcon
    {
        Render::UI::UiTextureId texture;
        Maths::Vector2 uvMin { 0.0f, 1.0f };
        Maths::Vector2 uvMax { 1.0f, 0.0f };
        float pixelSize = 0.0f;
        bool tintable = false;
        bool valid = false;
        std::uint32_t registryGeneration = 0;
    };

    struct GlyphIcon
    {
        char utf8[5] {};
        float pixelSize = 0.0f;
        bool valid = false;
    };

    enum class IconResolveStatus
    {
        Resolved,
        Fallback,
        Missing
    };

    struct IconLoadResult
    {
        ResolvedIcon icon;
        std::string resolvedId;
        IconResolveStatus status = IconResolveStatus::Missing;
    };

    struct IconDiagnostics
    {
        std::size_t loadedTextureCount = 0;
        std::size_t estimatedTextureBytes = 0;
        std::size_t resolutionCount = 0;
        std::size_t fallbackCount = 0;
        std::size_t cacheHitCount = 0;
        std::size_t unusedDefinitionCount = 0;
    };

    struct IconAtlasFrame
    {
        std::string_view pageResourceId;
        std::uint32_t pageSize = 0;
        float u0 = 0.0f;
        float v0 = 0.0f;
        float u1 = 1.0f;
        float v1 = 1.0f;
    };

    struct IconAtlasEntry
    {
        std::string_view iconId;
        std::uint32_t pixels = 0;
        IconAtlasFrame frame;
    };

    /**
    * Texture pair produced by the loader. The service never dereferences either
    * member; the bridge callbacks own the real types. This keeps the service
    * testable without a GPU device.
    */
    struct IconTexture
    {
        Render::Resources::Texture2D* texture = nullptr;
        std::shared_ptr<Render::RHI::RHITextureView> view;

        explicit operator bool() const { return texture != nullptr; }
    };

    struct IconServiceBindings
    {
        // Resource id -> development path. Usually delegates to EditorResourceCatalog.
        std::function<std::optional<std::filesystem::path>(std::string_view)> resolvePath;
        std::function<bool(const std::string&)> fileExists;
        std::function<std::optional<IconTexture>(const std::string&)> loadTexture;
        std::function<void(Render::Resources::Texture2D*)> destroyTexture;
        std::function<Render::UI::UiTextureId(
            const std::shared_ptr<Render::RHI::RHITextureView>&)> registerTextureView;
        std::function<void(
            const std::shared_ptr<Render::RHI::RHITextureView>&)> releaseTextureView;
        // Optional: available pixel tiers for a file (P3 multi-tier assets).
        std::function<std::vector<std::uint32_t>(const std::string&)> availableTiers;
        // Optional atlas lookup. Returning a frame switches the resolution to
        // the page texture + UV; nullptr keeps the standalone tier file.
        std::function<const IconAtlasFrame*(std::string_view, std::uint32_t)> findAtlasFrame;
        std::function<std::optional<std::filesystem::path>(std::string_view)> resolveAtlasPagePath;
        std::function<void(std::string_view)> logWarning;
    };

    /**
    * Resolves semantic ids to drawable icons with lazy loading, per-file
    * deduplication, DPI tier selection, a default-icon fallback chain and
    * deferred GPU retirement. No ImGui dependency.
    */
    class IconService
    {
    public:
        IconService(IconRegistry& p_registry, IconServiceBindings p_bindings);
        ~IconService();

        IconService(const IconService&) = delete;
        IconService& operator=(const IconService&) = delete;
        IconService(IconService&&) = delete;
        IconService& operator=(IconService&&) = delete;

        IconLoadResult Resolve(
            IconId p_id,
            std::uint32_t p_targetPixels,
            bool p_countUsage = true);
        GlyphIcon ResolveGlyph(
            IconId p_id,
            float p_logicalPixels,
            bool p_countUsage = true);

        std::size_t Preload(std::initializer_list<IconId> p_ids, std::uint32_t p_targetPixels);

        // Loads at most p_maxPerFrame not-yet-resolved icons. Returns true while
        // more definitions remain (call once per frame for a smooth background
        // fill instead of first-use hitches).
        bool PreloadIdleStep(std::uint32_t p_maxPerFrame, std::uint32_t p_targetPixels);

        // Usage telemetry for deprecation decisions. Counts requested and
        // resolved ids; glyph resolutions count too.
        std::uint32_t GetUsageCount(std::string_view p_id) const;
        void ResetUsageCounts();
        IconDiagnostics GetDiagnostics() const;

        // Hot reload / plugin unload: entries are retired, not destroyed inline.
        void InvalidateModule(std::string_view p_module);
        void SetFrameId(std::uint64_t p_frameId);
        void TickRetirement();
        void OnDeviceRecreated();
        void Clear();

        std::size_t LoadedTextureCount() const { return m_textures.size(); }
        std::size_t ResolutionCount() const { return m_resolutionCount; }
        std::size_t FallbackCount() const { return m_fallbackCount; }
        std::size_t CacheHitCount() const { return m_cacheHitCount; }

    private:
        struct CacheEntry
        {
            ResolvedIcon icon;
            std::string path;
            std::string module;
        };

        struct RetiredTexture
        {
            IconTexture texture;
            std::uint64_t releaseFrame = 0;
        };

        IconLoadResult ResolveInternal(std::string_view p_id, std::uint32_t p_pixels);
        IconLoadResult Fallback(std::uint32_t p_pixels, IconResolveStatus p_status);
        void ReportOnce(const std::string& p_key, const std::string& p_message);
        void RetireOrphanTextures();

        IconRegistry* m_registry = nullptr;
        IconServiceBindings m_bindings;
        std::unordered_map<std::string, CacheEntry> m_entries;
        std::unordered_map<std::string, IconTexture> m_textures;
        std::unordered_map<std::string, std::size_t> m_textureBytesByPath;
        std::unordered_map<std::string, std::uint32_t> m_usageCounts;
        std::vector<RetiredTexture> m_retired;
        std::unordered_set<std::string> m_reported;
        std::size_t m_idlePreloadIndex = 0;
        std::size_t m_estimatedTextureBytes = 0;
        std::uint64_t m_frameId = 0;
        std::size_t m_resolutionCount = 0;
        std::size_t m_fallbackCount = 0;
        std::size_t m_cacheHitCount = 0;
    };
}
