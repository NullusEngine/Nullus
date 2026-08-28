#pragma once

#include "RenderDef.h"

#include <array>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_map>
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
    };

    struct NLS_RENDER_API UIFontAtlasDiagnostics
    {
        uint64_t generatedGlyphs = 0u;
        uint64_t fallbackGlyphs = 0u;
        uint64_t evictedGlyphs = 0u;
        uint64_t pageAllocations = 0u;
        uint64_t missingFontRequests = 0u;
    };

    // Font backend contract. A production build can install a FreeType/
    // HarfBuzz rasterizer; the default allocator still supplies deterministic
    // metrics and a tofu glyph so missing data is observable, never silent.
    class NLS_RENDER_API UIFontAtlas final
    {
    public:
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

        void InvalidateFont(uint64_t fontAssetId, uint64_t retireFrame);
        void CollectRetiredPages(uint64_t completedFrame);
        void Clear();

    private:
        struct GlyphKeyHash
        {
            size_t operator()(UIFontGlyphKey key) const noexcept;
        };

        static bool DecodeUtf8(std::string_view text, size_t& offset, uint32_t& codepoint);
        bool AllocateGlyphPage(uint32_t cellWidth, uint32_t cellHeight, uint64_t frameId, UIFontGlyph& output);
        bool EvictOldestPage(uint64_t frameId);

        mutable std::mutex m_mutex;
        uint32_t m_pageWidth = 1024u;
        uint32_t m_pageHeight = 1024u;
        uint32_t m_maxPages = 8u;
        uint64_t m_completedFrame = 0u;
        std::vector<UIFontAtlasPage> m_pages;
        std::unordered_map<UIFontGlyphKey, UIFontGlyph, GlyphKeyHash> m_glyphs;
        UIFontAtlasDiagnostics m_diagnostics;
    };
}
