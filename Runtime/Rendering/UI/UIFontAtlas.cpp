#include "UIFontAtlas.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace NLS::Render::UI
{
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
        output.advance = static_cast<float>(key.pixelSize) * (fontAvailable ? 0.6f : 0.75f);
        output.bearingY = static_cast<float>(key.pixelSize);
        output.fallback = !fontAvailable;
        output.lastUsedFrame = frameId;
        if (output.fallback)
        {
            ++m_diagnostics.fallbackGlyphs;
            ++m_diagnostics.missingFontRequests;
        }
        else
        {
            ++m_diagnostics.generatedGlyphs;
        }
        m_glyphs.emplace(key, output);
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
        (void)frameId;
        return true;
    }
}
