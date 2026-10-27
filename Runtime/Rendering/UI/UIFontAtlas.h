#pragma once

#include "RenderDef.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace NLS::Render::UI
{
    struct NLS_RENDER_API UIFontGlyphKey
    {
        uint64_t fontAssetId = 0u;
        uint32_t codepoint = 0u;
        uint16_t pixelSize = 16u;

        friend constexpr bool operator==(UIFontGlyphKey, UIFontGlyphKey) = default;
    };

    struct NLS_RENDER_API UIFontGlyph
    {
        UIFontGlyphKey key;
        uint32_t pageIndex = 0u;
        std::array<float, 4> uv { 0.0f, 0.0f, 1.0f, 1.0f };
        float advance = 0.0f;
        float bearingX = 0.0f;
        float bearingY = 0.0f;
        // Rasterized bitmap size in pixels. Producers need it to place the glyph
        // quad; tofu supplies a proportional box instead of nothing.
        float width = 0.0f;
        float height = 0.0f;
        bool fallback = false;
        uint64_t lastUsedFrame = 0u;
    };

    struct NLS_RENDER_API UIFontAtlasPage
    {
        uint32_t index = 0u;
        uint32_t width = 1024u;
        uint32_t height = 1024u;
        uint32_t generation = 1u;
        uint32_t cursorX = 0u;
        uint32_t cursorY = 0u;
        uint32_t rowHeight = 0u;
        bool dirty = false;
        uint64_t retireFrame = 0u;
        // RGBA8 page pixels. White RGB with the glyph coverage in alpha, which is
        // what the UI overlay samples for text and icons.
        std::vector<uint8_t> pixels;
    };

    struct NLS_RENDER_API UIFontAtlasDiagnostics
    {
        uint64_t generatedGlyphs = 0u;
        uint64_t fallbackGlyphs = 0u;
        uint64_t evictedGlyphs = 0u;
        uint64_t pageAllocations = 0u;
        uint64_t missingFontRequests = 0u;
        // Glyphs whose requested face was unavailable and were rasterized with
        // the default face instead. Distinct from tofu fallbackGlyphs.
        uint64_t fallbackFaceGlyphs = 0u;
    };

    // Font backend contract. A production build can install a FreeType/
    // HarfBuzz rasterizer; the default allocator still supplies deterministic
    // metrics and a tofu glyph so missing data is observable, never silent.
    class NLS_RENDER_API UIFontAtlas final
    {
    public:
        UIFontAtlas();
        ~UIFontAtlas();

        // TrueType bytes used to rasterize real glyph bitmaps. Without font data
        // the atlas keeps deterministic metrics and a visible tofu cell so missing
        // data stays observable instead of silently rendering nothing.
        bool SetFontData(const uint8_t* data, size_t byteCount);
        bool LoadFontFromFile(const std::filesystem::path& path);
        [[nodiscard]] bool HasFontData() const;

        // Per-font-asset faces. Face id 1 is the process default used when a
        // text run does not request a project font. SetFaceData overrides a
        // face immediately; EnsureFace asks the installed provider for bytes
        // on demand. A failed provider request is cached until InvalidateFont,
        // Clear or ClearFaceFailures so a missing font does not hit the disk
        // for every glyph.
        bool SetFaceData(uint64_t fontAssetId, const uint8_t* data, size_t byteCount);
        bool EnsureFace(uint64_t fontAssetId);
        [[nodiscard]] bool HasFace(uint64_t fontAssetId) const;
        [[nodiscard]] uint32_t GetFaceCount() const;
        void SetMaxFaces(uint32_t maxFaces);
        void ClearFaceFailures();

        void Configure(uint32_t pageWidth, uint32_t pageHeight, uint32_t maxPages);
        void SetCompletedFrame(uint64_t completedFrame) { m_completedFrame = completedFrame; }

        bool EnsureGlyph(
            UIFontGlyphKey key,
            uint64_t frameId,
            UIFontGlyph& output,
            bool fontAvailable = true);
        uint32_t EnsureText(
            uint64_t fontAssetId,
            std::string_view utf8,
            uint16_t pixelSize,
            uint64_t frameId,
            bool fontAvailable = true);

        [[nodiscard]] std::optional<UIFontGlyph> Find(UIFontGlyphKey key) const;
        [[nodiscard]] const std::vector<UIFontAtlasPage>& GetPages() const { return m_pages; }
        [[nodiscard]] const UIFontAtlasDiagnostics& GetDiagnostics() const { return m_diagnostics; }
        // Bumped whenever page pixels change, so uploaders can skip unchanged
        // frames without hashing the whole atlas.
        [[nodiscard]] uint64_t GetContentGeneration() const { return m_generation; }

        void InvalidateFont(uint64_t fontAssetId, uint64_t retireFrame);
        void CollectRetiredPages(uint64_t completedFrame);
        void Clear();

    private:
        struct FontRasterizerState;
        struct FontFace;

        struct GlyphKeyHash
        {
            size_t operator()(UIFontGlyphKey key) const noexcept;
        };

        static constexpr uint64_t DefaultFaceId = 1u;

        static bool DecodeUtf8(std::string_view text, size_t& offset, uint32_t& codepoint);
        bool AllocateGlyphPage(uint32_t cellWidth, uint32_t cellHeight, uint64_t frameId, UIFontGlyph& output);
        bool EvictOldestPage(uint64_t frameId);
        [[nodiscard]] FontFace* FindFaceLocked(uint64_t fontAssetId);
        [[nodiscard]] const FontFace* FindFaceLocked(uint64_t fontAssetId) const;
        [[nodiscard]] bool FindFaceReadyLocked(uint64_t fontAssetId) const;
        void RemoveFaceGlyphsLocked(uint64_t fontAssetId);
        void EvictFacesLocked(uint64_t keepFaceId);

        mutable std::mutex m_mutex;
        uint32_t m_pageWidth = 1024u;
        uint32_t m_pageHeight = 1024u;
        uint32_t m_maxPages = 8u;
        uint64_t m_completedFrame = 0u;
        std::vector<UIFontAtlasPage> m_pages;
        std::unordered_map<UIFontGlyphKey, UIFontGlyph, GlyphKeyHash> m_glyphs;
        UIFontAtlasDiagnostics m_diagnostics;
        std::unordered_map<uint64_t, std::unique_ptr<FontFace>> m_faces;
        std::unordered_set<uint64_t> m_failedFaces;
        uint32_t m_maxFontFaces = 16u;
        uint64_t m_generation = 1u;
    };

    // Host-installed face source. The provider receives a stable font asset id
    // (allocated by IUIDocumentResourceResolver) and fills bytes with the TTF
    // payload. Returning false keeps the face unavailable and the atlas uses
    // the default face (or tofu) until the face is invalidated.
    using UIFontFaceProvider = std::function<bool(uint64_t fontAssetId, std::vector<uint8_t>& bytes)>;
    NLS_RENDER_API void SetUIFontFaceProvider(UIFontFaceProvider provider);
    NLS_RENDER_API void ClearUIFontFaceProvider();

    // Process-wide atlas for producers that do not own one (the document paint
    // path, for example). The font is loaded lazily from the engine font asset.
    NLS_RENDER_API UIFontAtlas& GetSharedUIFontAtlas();
    NLS_RENDER_API void SetSharedUIFontAtlasFontPath(std::filesystem::path fontPath);
}
