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

    struct UIFontAtlas::FontFace
    {
        // stbtt_fontinfo keeps pointers into bytes, so the buffer must never be
        // copied or reallocated after initialization.
        std::vector<uint8_t> bytes;
        std::unique_ptr<FontRasterizerState> rasterizer;
        uint64_t lastUsedFrame = 0u;
    };

    namespace
    {
        bool LooksLikeTrueType(const uint8_t* data, const size_t byteCount)
        {
            if (data == nullptr || byteCount < 12u)
                return false;
            const uint32_t tag =
                (static_cast<uint32_t>(data[0]) << 24u) |
                (static_cast<uint32_t>(data[1]) << 16u) |
                (static_cast<uint32_t>(data[2]) << 8u) |
                static_cast<uint32_t>(data[3]);
            // TrueType, OpenType/CFF, 'true' and 'typ1'. stb_truetype can read
            // the first four formats; collections ('ttcf') need an explicit
            // face offset and are rejected rather than parsed as face zero.
            return tag == 0x00010000u || tag == 0x4f54544fu ||
                tag == 0x74727565u || tag == 0x74797031u;
        }

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

    namespace
    {
        std::mutex& FaceProviderMutex()
        {
            static std::mutex mutex;
            return mutex;
        }

        UIFontFaceProvider& FaceProviderStorage()
        {
            static UIFontFaceProvider provider;
            return provider;
        }

        UIFontFaceProvider CopyFaceProvider()
        {
            std::lock_guard lock(FaceProviderMutex());
            return FaceProviderStorage();
        }
    }

    void SetUIFontFaceProvider(UIFontFaceProvider provider)
    {
        std::lock_guard lock(FaceProviderMutex());
        FaceProviderStorage() = std::move(provider);
    }

    void ClearUIFontFaceProvider()
    {
        std::lock_guard lock(FaceProviderMutex());
        FaceProviderStorage() = nullptr;
    }

    bool UIFontAtlas::SetFontData(const uint8_t* data, const size_t byteCount)
    {
        return SetFaceData(DefaultFaceId, data, byteCount);
    }

    bool UIFontAtlas::SetFaceData(
        uint64_t fontAssetId,
        const uint8_t* data,
        const size_t byteCount)
    {
        if (fontAssetId == 0u)
            fontAssetId = DefaultFaceId;
        if (!LooksLikeTrueType(data, byteCount))
            return false;

        auto face = std::make_unique<FontFace>();
        face->bytes.assign(data, data + byteCount);

        auto rasterizer = std::make_unique<FontRasterizerState>();
        if (stbtt_InitFont(&rasterizer->info, face->bytes.data(), 0) == 0)
            return false;
        rasterizer->ready = true;
        face->rasterizer = std::move(rasterizer);

        std::lock_guard lock(m_mutex);
        m_faces[fontAssetId] = std::move(face);
        m_failedFaces.erase(fontAssetId);
        if (fontAssetId == DefaultFaceId)
        {
            // The default face backs text runs that did not request a project
            // font; replacing it invalidates every cached page (all pages were
            // rasterized against the previous default). Project faces stay
            // loaded and are re-rasterized on demand.
            m_pages.clear();
            m_glyphs.clear();
        }
        else
        {
            RemoveFaceGlyphsLocked(fontAssetId);
        }
        EvictFacesLocked(fontAssetId);
        ++m_generation;
        return true;
    }

    bool UIFontAtlas::EnsureFace(const uint64_t fontAssetId)
    {
        const auto faceId = fontAssetId == 0u ? DefaultFaceId : fontAssetId;
        {
            std::lock_guard lock(m_mutex);
            if (FindFaceReadyLocked(faceId))
                return true;
            if (m_failedFaces.find(faceId) != m_failedFaces.end())
                return false;
        }

        const auto provider = CopyFaceProvider();
        if (!provider)
            return false;

        std::vector<uint8_t> bytes;
        if (!provider(faceId, bytes) || bytes.empty())
        {
            std::lock_guard lock(m_mutex);
            m_failedFaces.insert(faceId);
            return false;
        }
        return SetFaceData(faceId, bytes.data(), bytes.size());
    }

    bool UIFontAtlas::HasFace(const uint64_t fontAssetId) const
    {
        const auto faceId = fontAssetId == 0u ? DefaultFaceId : fontAssetId;
        std::lock_guard lock(m_mutex);
        return FindFaceReadyLocked(faceId);
    }

    uint32_t UIFontAtlas::GetFaceCount() const
    {
        std::lock_guard lock(m_mutex);
        return static_cast<uint32_t>(m_faces.size());
    }

    void UIFontAtlas::SetMaxFaces(const uint32_t maxFaces)
    {
        std::lock_guard lock(m_mutex);
        // The default face is always resident, so a useful cap is at least two.
        m_maxFontFaces = std::max(2u, maxFaces);
        EvictFacesLocked(DefaultFaceId);
    }

    void UIFontAtlas::ClearFaceFailures()
    {
        std::lock_guard lock(m_mutex);
        m_failedFaces.clear();
    }

    UIFontAtlas::FontFace* UIFontAtlas::FindFaceLocked(const uint64_t fontAssetId)
    {
        const auto iterator = m_faces.find(fontAssetId);
        return iterator == m_faces.end() ? nullptr : iterator->second.get();
    }

    const UIFontAtlas::FontFace* UIFontAtlas::FindFaceLocked(const uint64_t fontAssetId) const
    {
        const auto iterator = m_faces.find(fontAssetId);
        return iterator == m_faces.end() ? nullptr : iterator->second.get();
    }

    bool UIFontAtlas::FindFaceReadyLocked(const uint64_t fontAssetId) const
    {
        const auto* face = FindFaceLocked(fontAssetId);
        return face != nullptr && face->rasterizer != nullptr && face->rasterizer->ready;
    }

    void UIFontAtlas::RemoveFaceGlyphsLocked(const uint64_t fontAssetId)
    {
        for (auto iterator = m_glyphs.begin(); iterator != m_glyphs.end();)
        {
            if (iterator->first.fontAssetId == fontAssetId)
                iterator = m_glyphs.erase(iterator);
            else
                ++iterator;
        }
    }

    void UIFontAtlas::EvictFacesLocked(const uint64_t keepFaceId)
    {
        while (m_faces.size() > m_maxFontFaces)
        {
            auto victim = m_faces.end();
            for (auto iterator = m_faces.begin(); iterator != m_faces.end(); ++iterator)
            {
                if (iterator->first == DefaultFaceId || iterator->first == keepFaceId)
                    continue;
                if (victim == m_faces.end() ||
                    iterator->second->lastUsedFrame < victim->second->lastUsedFrame)
                {
                    victim = iterator;
                }
            }
            if (victim == m_faces.end())
                break;
            const auto faceId = victim->first;
            RemoveFaceGlyphsLocked(faceId);
            m_faces.erase(victim);
            ++m_generation;
        }
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
        return HasFace(DefaultFaceId);
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

        std::unique_lock lock(m_mutex);
        if (const auto existing = m_glyphs.find(key); existing != m_glyphs.end())
        {
            existing->second.lastUsedFrame = frameId;
            output = existing->second;
            return true;
        }

        auto findFace = [&](const uint64_t fontAssetId) -> FontFace*
        {
            return FindFaceLocked(fontAssetId == 0u ? DefaultFaceId : fontAssetId);
        };

        FontFace* requestedFace = fontAvailable ? findFace(key.fontAssetId) : nullptr;
        if (requestedFace == nullptr && fontAvailable && key.fontAssetId != DefaultFaceId)
        {
            // Provider I/O may touch the file system/asset database, so it must
            // not run while holding the atlas lock.
            lock.unlock();
            EnsureFace(key.fontAssetId);
            lock.lock();

            // Another thread may have generated the same glyph while the lock
            // was released; honor its work instead of rasterizing twice.
            if (const auto existing = m_glyphs.find(key); existing != m_glyphs.end())
            {
                existing->second.lastUsedFrame = frameId;
                output = existing->second;
                return true;
            }
            requestedFace = findFace(key.fontAssetId);
        }

        FontFace* face = nullptr;
        bool usingFallbackFace = false;
        if (fontAvailable)
        {
            if (requestedFace != nullptr && requestedFace->rasterizer != nullptr &&
                requestedFace->rasterizer->ready)
            {
                face = requestedFace;
            }
            else if (key.fontAssetId != DefaultFaceId)
            {
                // A missing project face keeps text legible with the default
                // face and reports the fallback instead of painting tofu for an
                // otherwise supported codepoint.
                auto* defaultFace = findFace(DefaultFaceId);
                if (defaultFace != nullptr && defaultFace->rasterizer != nullptr &&
                    defaultFace->rasterizer->ready)
                {
                    face = defaultFace;
                    usingFallbackFace = true;
                }
            }
        }
        if (face != nullptr)
            face->lastUsedFrame = frameId;

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

        const int glyphIndex = face != nullptr
            ? stbtt_FindGlyphIndex(&face->rasterizer->info, static_cast<int>(key.codepoint))
            : 0;
        if (glyphIndex != 0 && page != nullptr)
        {
            stbtt_fontinfo& info = face->rasterizer->info;
            const float scale = stbtt_ScaleForPixelHeight(
                &info,
                static_cast<float>(key.pixelSize));
            int advance = 0;
            int leftSideBearing = 0;
            stbtt_GetGlyphHMetrics(&info, glyphIndex, &advance, &leftSideBearing);
            int x0 = 0;
            int y0 = 0;
            int x1 = 0;
            int y1 = 0;
            stbtt_GetGlyphBitmapBox(&info, glyphIndex, scale, scale, &x0, &y0, &x1, &y1);
            const auto bitmapWidth = static_cast<uint32_t>((std::max)(0, x1 - x0));
            const auto bitmapHeight = static_cast<uint32_t>((std::max)(0, y1 - y0));
            std::vector<uint8_t> bitmap(static_cast<size_t>(bitmapWidth) * bitmapHeight, 0u);
            if (bitmapWidth != 0u && bitmapHeight != 0u)
            {
                stbtt_MakeGlyphBitmap(
                    &info,
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
            if (usingFallbackFace)
                ++m_diagnostics.fallbackFaceGlyphs;
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
        RemoveFaceGlyphsLocked(fontAssetId);
        m_failedFaces.erase(fontAssetId);
        if (fontAssetId != DefaultFaceId)
            m_faces.erase(fontAssetId);
        for (auto& page : m_pages)
            page.retireFrame = std::max(page.retireFrame, retireFrame);
        ++m_generation;
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
        m_failedFaces.clear();
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
