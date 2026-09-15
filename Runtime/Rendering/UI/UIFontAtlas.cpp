#include "UIFontAtlas.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>

#define STB_TRUETYPE_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb/stb_truetype.h>

namespace NLS::Render::UI
{
    struct UIFontAtlas::FontRasterizerState
    {
        stbtt_fontinfo info {};
        bool ready = false;
    };

    namespace
    {
        // Writes a white RGBA glyph cell. Coverage arrives in `alpha`; `tofu`
        // paints the whole cell so an unbacked font is unmistakably visible.
        void BlitGlyphCell(
            UIFontAtlasPage& page,
            const uint32_t cellX,
            const uint32_t cellY,
            const uint32_t cellWidth,
            const uint32_t cellHeight,
            const uint8_t* alpha,
            const uint32_t alphaWidth,
            const uint32_t alphaHeight,
            const bool tofu)
        {
            const size_t expected = static_cast<size_t>(page.width) * page.height * 4u;
            if (page.pixels.size() != expected)
                return;

            for (uint32_t y = 0u; y < cellHeight; ++y)
            {
                const uint32_t pageY = cellY + y;
                if (pageY >= page.height)
                    break;
                for (uint32_t x = 0u; x < cellWidth; ++x)
                {
                    const uint32_t pageX = cellX + x;
                    if (pageX >= page.width)
                        break;

                    uint8_t coverage = 0u;
                    if (tofu)
                    {
                        const bool border = x == 0u || y == 0u ||
                            x + 1u == cellWidth || y + 1u == cellHeight;
                        coverage = border ? 255u : 40u;
                    }
                    else if (alpha != nullptr && x < alphaWidth && y < alphaHeight)
                    {
                        coverage = alpha[static_cast<size_t>(y) * alphaWidth + x];
                    }
                    if (coverage == 0u)
                        continue;

                    const size_t offset = (static_cast<size_t>(pageY) * page.width + pageX) * 4u;
                    page.pixels[offset + 0u] = 255u;
                    page.pixels[offset + 1u] = 255u;
                    page.pixels[offset + 2u] = 255u;
                    page.pixels[offset + 3u] = coverage;
                }
            }
        }
    }

    UIFontAtlas::UIFontAtlas() = default;
    UIFontAtlas::~UIFontAtlas() = default;

    bool UIFontAtlas::SetFontData(const uint8_t* data, const size_t byteCount)
    {
        std::lock_guard lock(m_mutex);
        m_fontState.reset();
        m_fontData.clear();
        if (data == nullptr || byteCount == 0u)
            return false;

        auto state = std::make_unique<FontRasterizerState>();
        m_fontData.assign(data, data + byteCount);
        if (stbtt_InitFont(&state->info, m_fontData.data(), 0) == 0)
        {
            m_fontData.clear();
            return false;
        }

        state->ready = true;
        m_fontState = std::move(state);
        // Glyph cells are rasterized from the new font, so drop cached pages.
        m_pages.clear();
        m_glyphs.clear();
        ++m_generation;
        return true;
    }

    bool UIFontAtlas::LoadFontFromFile(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
            return false;
        const std::vector<uint8_t> bytes {
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>() };
        return SetFontData(bytes.data(), bytes.size());
    }

    bool UIFontAtlas::HasFontData() const
    {
        std::lock_guard lock(m_mutex);
        return m_fontState != nullptr && m_fontState->ready;
    }

    namespace
    {
        std::mutex& SharedAtlasMutex()
        {
            static std::mutex mutex;
            return mutex;
        }

        std::filesystem::path& SharedAtlasFontPathStorage()
        {
            static std::filesystem::path path;
            return path;
        }

        void EnsureSharedAtlasFontLoaded(UIFontAtlas& atlas)
        {
            if (atlas.HasFontData())
                return;

            // Callers may point at an explicit font; otherwise fall back to the
            // engine font relative to the install root and the current directory.
            const std::filesystem::path configured = SharedAtlasFontPathStorage();
            const std::filesystem::path relative = "Assets/Engine/Fonts/Ruda-Bold.ttf";
            std::filesystem::path candidates[3];
            if (!configured.empty())
                candidates[0] = configured;
            candidates[1] = relative;
            auto root = std::filesystem::current_path();
            for (int depth = 0; depth < 6 && !root.empty(); ++depth)
            {
                const auto candidate = root / relative;
                if (std::filesystem::is_regular_file(candidate))
                {
                    candidates[2] = candidate;
                    break;
                }
                root = root.parent_path();
            }

            for (const auto& candidate : candidates)
            {
                if (!candidate.empty() && std::filesystem::is_regular_file(candidate) &&
                    atlas.LoadFontFromFile(candidate))
                {
                    return;
                }
            }
        }
    }

    UIFontAtlas& GetSharedUIFontAtlas()
    {
        static UIFontAtlas atlas;
        std::lock_guard lock(SharedAtlasMutex());
        EnsureSharedAtlasFontLoaded(atlas);
        return atlas;
    }

    void SetSharedUIFontAtlasFontPath(std::filesystem::path fontPath)
    {
        std::lock_guard lock(SharedAtlasMutex());
        SharedAtlasFontPathStorage() = std::move(fontPath);
    }

    size_t UIFontAtlas::GlyphKeyHash::operator()(const UIFontGlyphKey key) const noexcept
    {
        size_t hash = static_cast<size_t>(key.fontAssetId ^ (key.fontAssetId >> 32u));
        hash ^= static_cast<size_t>(key.codepoint) * static_cast<size_t>(0x9e3779b9u);
        hash ^= static_cast<size_t>(key.pixelSize) * static_cast<size_t>(0x85ebca6bu);
        return hash;
    }

    void UIFontAtlas::Configure(
        const uint32_t pageWidth,
        const uint32_t pageHeight,
        const uint32_t maxPages)
    {
        std::lock_guard lock(m_mutex);
        m_pageWidth = std::max(64u, pageWidth);
        m_pageHeight = std::max(64u, pageHeight);
        m_maxPages = std::max(1u, maxPages);
        Clear();
    }

    bool UIFontAtlas::EnsureGlyph(
        const UIFontGlyphKey key,
        const uint64_t frameId,
        UIFontGlyph& output,
        const bool fontAvailable)
    {
        if (key.fontAssetId == 0u || key.codepoint == 0u || key.pixelSize == 0u)
            return false;

        std::lock_guard lock(m_mutex);
        if (const auto existing = m_glyphs.find(key); existing != m_glyphs.end())
        {
            existing->second.lastUsedFrame = frameId;
            output = existing->second;
            return true;
        }

        const uint32_t cell = std::clamp<uint32_t>(
            static_cast<uint32_t>(std::ceil(static_cast<float>(key.pixelSize) * 1.5f)),
            8u,
            std::min(m_pageWidth, m_pageHeight));
        if (!AllocateGlyphPage(cell, cell, frameId, output))
            return false;

        output.key = key;
        output.lastUsedFrame = frameId;

        UIFontAtlasPage* page = nullptr;
        for (auto& candidate : m_pages)
        {
            if (candidate.index == output.pageIndex)
            {
                page = &candidate;
                break;
            }
        }

        const int glyphIndex = fontAvailable && m_fontState != nullptr && m_fontState->ready
            ? stbtt_FindGlyphIndex(&m_fontState->info, static_cast<int>(key.codepoint))
            : 0;
        if (glyphIndex != 0 && page != nullptr)
        {
            const float scale = stbtt_ScaleForPixelHeight(
                &m_fontState->info,
                static_cast<float>(key.pixelSize));
            int advance = 0;
            int leftSideBearing = 0;
            stbtt_GetGlyphHMetrics(&m_fontState->info, glyphIndex, &advance, &leftSideBearing);
            int x0 = 0;
            int y0 = 0;
            int x1 = 0;
            int y1 = 0;
            stbtt_GetGlyphBitmapBox(&m_fontState->info, glyphIndex, scale, scale, &x0, &y0, &x1, &y1);
            const auto bitmapWidth = static_cast<uint32_t>((std::max)(0, x1 - x0));
            const auto bitmapHeight = static_cast<uint32_t>((std::max)(0, y1 - y0));
            std::vector<uint8_t> bitmap(static_cast<size_t>(bitmapWidth) * bitmapHeight, 0u);
            if (bitmapWidth != 0u && bitmapHeight != 0u)
            {
                stbtt_MakeGlyphBitmap(
                    &m_fontState->info,
                    bitmap.data(),
                    static_cast<int>(bitmapWidth),
                    static_cast<int>(bitmapHeight),
                    static_cast<int>(bitmapWidth),
                    scale,
                    scale,
                    glyphIndex);
            }

            const auto cellX = static_cast<uint32_t>(output.uv[0] * static_cast<float>(page->width));
            const auto cellY = static_cast<uint32_t>(output.uv[1] * static_cast<float>(page->height));
            BlitGlyphCell(*page, cellX, cellY, cell, cell, bitmap.data(), bitmapWidth, bitmapHeight, false);

            // The cell is a padded slot, but the bitmap only fills its top-left
            // corner. The glyph rect must describe that bitmap: sampling the whole
            // cell squeezed every glyph into a fraction of its quad and rendered
            // text as unreadable smudges.
            output.uv[2] = static_cast<float>(cellX + bitmapWidth) / static_cast<float>(page->width);
            output.uv[3] = static_cast<float>(cellY + bitmapHeight) / static_cast<float>(page->height);

            output.advance = static_cast<float>(advance) * scale;
            output.bearingX = static_cast<float>(x0);
            output.bearingY = static_cast<float>(-y0);
            output.width = static_cast<float>(bitmapWidth);
            output.height = static_cast<float>(bitmapHeight);
            output.fallback = false;
            ++m_diagnostics.generatedGlyphs;
        }
        else
        {
            // A missing glyph paints its whole cell, so the quad covers that cell
            // instead of an arbitrary fraction of it.
            output.advance = static_cast<float>(key.pixelSize) * 0.75f;
            output.bearingY = static_cast<float>(key.pixelSize);
            output.width = static_cast<float>(cell);
            output.height = static_cast<float>(cell);
            output.fallback = true;
            if (page != nullptr)
            {
                const auto cellX = static_cast<uint32_t>(output.uv[0] * static_cast<float>(page->width));
                const auto cellY = static_cast<uint32_t>(output.uv[1] * static_cast<float>(page->height));
                BlitGlyphCell(*page, cellX, cellY, cell, cell, nullptr, 0u, 0u, true);
            }
            ++m_diagnostics.fallbackGlyphs;
            ++m_diagnostics.missingFontRequests;
        }
        m_glyphs.emplace(key, output);
        ++m_generation;
        return true;
    }

    uint32_t UIFontAtlas::EnsureText(
        const uint64_t fontAssetId,
        const std::string_view utf8,
        const uint16_t pixelSize,
        const uint64_t frameId,
        const bool fontAvailable)
    {
        uint32_t count = 0u;
        size_t offset = 0u;
        while (offset < utf8.size())
        {
            uint32_t codepoint = 0xfffdu;
            if (!DecodeUtf8(utf8, offset, codepoint))
                codepoint = 0xfffdu;
            UIFontGlyph glyph;
            if (EnsureGlyph({ fontAssetId, codepoint, pixelSize }, frameId, glyph, fontAvailable))
                ++count;
        }
        return count;
    }

    std::optional<UIFontGlyph> UIFontAtlas::Find(const UIFontGlyphKey key) const
    {
        std::lock_guard lock(m_mutex);
        const auto iterator = m_glyphs.find(key);
        return iterator == m_glyphs.end()
            ? std::nullopt
            : std::optional<UIFontGlyph>(iterator->second);
    }

    void UIFontAtlas::InvalidateFont(const uint64_t fontAssetId, const uint64_t retireFrame)
    {
        std::lock_guard lock(m_mutex);
        for (auto iterator = m_glyphs.begin(); iterator != m_glyphs.end();)
        {
            if (iterator->first.fontAssetId == fontAssetId)
                iterator = m_glyphs.erase(iterator);
            else
                ++iterator;
        }
        for (auto& page : m_pages)
            page.retireFrame = std::max(page.retireFrame, retireFrame);
    }

    void UIFontAtlas::CollectRetiredPages(const uint64_t completedFrame)
    {
        std::lock_guard lock(m_mutex);
        m_completedFrame = std::max(m_completedFrame, completedFrame);
        m_pages.erase(
            std::remove_if(
                m_pages.begin(), m_pages.end(),
                [completedFrame](const UIFontAtlasPage& page)
                {
                    return page.retireFrame != 0u && page.retireFrame <= completedFrame;
                }),
            m_pages.end());
    }

    void UIFontAtlas::Clear()
    {
        m_pages.clear();
        m_glyphs.clear();
        m_diagnostics = {};
        ++m_generation;
    }

    bool UIFontAtlas::DecodeUtf8(
        const std::string_view text,
        size_t& offset,
        uint32_t& codepoint)
    {
        if (offset >= text.size())
            return false;
        const auto byte = static_cast<uint8_t>(text[offset++]);
        if (byte < 0x80u)
        {
            codepoint = byte;
            return true;
        }

        uint32_t continuationCount = 0u;
        uint32_t result = 0u;
        uint32_t minimum = 0u;
        if ((byte & 0xe0u) == 0xc0u)
        {
            continuationCount = 1u;
            result = byte & 0x1fu;
            minimum = 0x80u;
        }
        else if ((byte & 0xf0u) == 0xe0u)
        {
            continuationCount = 2u;
            result = byte & 0x0fu;
            minimum = 0x800u;
        }
        else if ((byte & 0xf8u) == 0xf0u)
        {
            continuationCount = 3u;
            result = byte & 0x07u;
            minimum = 0x10000u;
        }
        else
        {
            codepoint = 0xfffdu;
            return false;
        }

        if (offset + continuationCount > text.size())
        {
            codepoint = 0xfffdu;
            offset = text.size();
            return false;
        }
        for (uint32_t index = 0u; index < continuationCount; ++index)
        {
            const auto next = static_cast<uint8_t>(text[offset++]);
            if ((next & 0xc0u) != 0x80u)
            {
                codepoint = 0xfffdu;
                return false;
            }
            result = (result << 6u) | (next & 0x3fu);
        }
        if (result < minimum || result > 0x10ffffu ||
            (result >= 0xd800u && result <= 0xdfffu))
        {
            codepoint = 0xfffdu;
            return false;
        }
        codepoint = result;
        return true;
    }

    bool UIFontAtlas::AllocateGlyphPage(
        const uint32_t cellWidth,
        const uint32_t cellHeight,
        const uint64_t frameId,
        UIFontGlyph& output)
    {
        auto allocateInPage = [&](UIFontAtlasPage& page) -> bool
        {
            if (page.cursorX + cellWidth > page.width)
            {
                page.cursorX = 0u;
                page.cursorY += page.rowHeight;
                page.rowHeight = 0u;
            }
            if (page.cursorY + cellHeight > page.height)
                return false;
            const float width = static_cast<float>(page.width);
            const float height = static_cast<float>(page.height);
            output.pageIndex = page.index;
            output.uv = {
                static_cast<float>(page.cursorX) / width,
                static_cast<float>(page.cursorY) / height,
                static_cast<float>(page.cursorX + cellWidth) / width,
                static_cast<float>(page.cursorY + cellHeight) / height };
            page.cursorX += cellWidth;
            page.rowHeight = std::max(page.rowHeight, cellHeight);
            page.dirty = true;
            return true;
        };

        for (auto& page : m_pages)
            if (page.retireFrame == 0u && allocateInPage(page))
                return true;

        if (m_pages.size() >= m_maxPages && !EvictOldestPage(frameId))
            return false;

        UIFontAtlasPage page;
        page.index = m_pages.empty() ? 0u : m_pages.back().index + 1u;
        page.width = m_pageWidth;
        page.height = m_pageHeight;
        page.pixels.assign(static_cast<size_t>(page.width) * page.height * 4u, 0u);
        ++m_diagnostics.pageAllocations;
        m_pages.push_back(page);
        return allocateInPage(m_pages.back());
    }

    bool UIFontAtlas::EvictOldestPage(const uint64_t frameId)
    {
        if (m_pages.empty())
            return false;
        auto oldest = m_pages.end();
        uint64_t oldestFrame = std::numeric_limits<uint64_t>::max();
        for (auto iterator = m_pages.begin(); iterator != m_pages.end(); ++iterator)
        {
            uint64_t newestUse = 0u;
            for (const auto& [key, glyph] : m_glyphs)
                if (glyph.pageIndex == iterator->index)
                    newestUse = std::max(newestUse, glyph.lastUsedFrame);
            if (newestUse <= m_completedFrame && newestUse < oldestFrame)
            {
                oldestFrame = newestUse;
                oldest = iterator;
            }
        }
        if (oldest == m_pages.end())
            return false;
        const auto pageIndex = oldest->index;
        for (auto iterator = m_glyphs.begin(); iterator != m_glyphs.end();)
        {
            if (iterator->second.pageIndex == pageIndex)
            {
                iterator = m_glyphs.erase(iterator);
                ++m_diagnostics.evictedGlyphs;
            }
            else
                ++iterator;
        }
        oldest->cursorX = 0u;
        oldest->cursorY = 0u;
        oldest->rowHeight = 0u;
        oldest->retireFrame = 0u;
        oldest->dirty = true;
        std::fill(oldest->pixels.begin(), oldest->pixels.end(), 0u);
        ++m_generation;
        (void)frameId;
        return true;
    }
}
