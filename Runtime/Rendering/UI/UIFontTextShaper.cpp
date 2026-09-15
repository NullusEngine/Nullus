#include "UIFontTextShaper.h"

#include "Rendering/UI/UIFontAtlas.h"
#include "Rendering/UI/UIFontAtlasUploader.h"

#include <algorithm>
#include <vector>

namespace NLS::Render::UI
{
namespace
{
// A zero font asset id means "engine default font". The atlas keys glyphs by a
// non-zero id, so default text maps onto this stable slot.
constexpr uint64_t kDefaultUiFontAssetId = 1u;

struct UiFontAtlasTextureBinding
{
    uint64_t textureId = 0u;
    uint32_t generation = 0u;
};

UiFontAtlasTextureBinding& FontAtlasTextureBinding()
{
    static UiFontAtlasTextureBinding binding;
    return binding;
}

bool DecodeNextCodepoint(const std::string_view text, size_t& offset, uint32_t& codepoint)
{
    if (offset >= text.size())
        return false;

    const auto lead = static_cast<uint8_t>(text[offset++]);
    if (lead < 0x80u)
    {
        codepoint = lead;
        return true;
    }

    uint32_t continuationCount = 0u;
    uint32_t value = 0u;
    if ((lead & 0xe0u) == 0xc0u)
    {
        continuationCount = 1u;
        value = lead & 0x1fu;
    }
    else if ((lead & 0xf0u) == 0xe0u)
    {
        continuationCount = 2u;
        value = lead & 0x0fu;
    }
    else if ((lead & 0xf8u) == 0xf0u)
    {
        continuationCount = 3u;
        value = lead & 0x07u;
    }
    else
    {
        codepoint = 0xfffdu;
        return false;
    }

    if (offset + continuationCount > text.size())
    {
        offset = text.size();
        codepoint = 0xfffdu;
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
        value = (value << 6u) | (next & 0x3fu);
    }

    codepoint = value;
    return true;
}
}

uint32_t AppendTextQuadGeometry(
    UIDrawSnapshot& snapshot,
    UIFontAtlas& atlas,
    const uint64_t frameId,
    const bool fontAvailable)
{
    if (snapshot.textRuns.empty())
        return 0u;

    const auto binding = FontAtlasTextureBinding();
    // Glyphs are still rasterized while no atlas texture is registered: the
    // atlas must fill its pages so the uploader has something to publish. Only
    // the quads wait for a texture to sample.
    const bool emitGeometry = binding.textureId != 0u;

    uint32_t quadCount = 0u;
    for (const auto& run : snapshot.textRuns)
    {
        if (run.text.empty())
            continue;

        const uint16_t pixelSize = run.fontPixelSize != 0u
            ? run.fontPixelSize
            : static_cast<uint16_t>((std::max)(1.0f, run.fontSize));
        const uint64_t fontAssetId = run.fontAssetId != 0u
            ? run.fontAssetId
            : kDefaultUiFontAssetId;
        const float lineHeight = static_cast<float>(pixelSize) * 1.35f;
        float penX = run.x;
        float baseline = run.y + static_cast<float>(pixelSize);

        size_t offset = 0u;
        while (offset < run.text.size())
        {
            uint32_t codepoint = 0xfffdu;
            if (!DecodeNextCodepoint(run.text, offset, codepoint))
                codepoint = 0xfffdu;

            if (codepoint == 0x0au)
            {
                penX = run.x;
                baseline += lineHeight;
                continue;
            }
            if (codepoint == 0x0du)
                continue;

            UIFontGlyph glyph;
            if (!atlas.EnsureGlyph({ fontAssetId, codepoint, pixelSize }, frameId, glyph, fontAvailable))
                continue;

            // Overflowing glyphs continue on the next line rather than bleeding
            // past the run rect. Full word wrapping belongs to the text layout
            // pass; this keeps a single line inside its element.
            if (run.width > 0.0f && penX + glyph.advance > run.x + run.width && penX > run.x)
            {
                penX = run.x;
                baseline += lineHeight;
            }

            if (emitGeometry && glyph.width > 0.0f && glyph.height > 0.0f)
            {
                const float left = penX + glyph.bearingX;
                const float top = baseline - glyph.bearingY;
                // Glyph UVs are page-relative; the sampled texture is the stacked
                // atlas image, so map them into its space.
                const auto topLeft = MapUIFontAtlasPageUv(atlas, glyph.pageIndex, glyph.uv[0], glyph.uv[1]);
                const auto bottomRight = MapUIFontAtlasPageUv(atlas, glyph.pageIndex, glyph.uv[2], glyph.uv[3]);
                snapshot.AddTexturedQuad(
                    left,
                    top,
                    left + glyph.width,
                    top + glyph.height,
                    topLeft,
                    bottomRight,
                    run.color,
                    run.clipIndex,
                    binding.textureId,
                    run.sortKey);
                snapshot.batches.back().textureGeneration = binding.generation;
                ++quadCount;
            }

            penX += glyph.advance;
        }
    }

    return quadCount;
}

uint32_t AppendTextQuadGeometry(UIDrawSnapshot& snapshot, const uint64_t frameId)
{
    return AppendTextQuadGeometry(snapshot, GetSharedUIFontAtlas(), frameId, true);
}

void SetUiFontAtlasTextureId(const uint64_t textureId, const uint32_t generation)
{
    auto& binding = FontAtlasTextureBinding();
    binding.textureId = textureId;
    binding.generation = textureId == 0u ? 0u : (generation == 0u ? 1u : generation);
}

void ClearUiFontAtlasTextureId()
{
    SetUiFontAtlasTextureId(0u, 0u);
}

bool HasUiFontAtlasTextureId()
{
    return FontAtlasTextureBinding().textureId != 0u;
}

uint64_t GetUiFontAtlasTextureId()
{
    return FontAtlasTextureBinding().textureId;
}

uint32_t GetUiFontAtlasTextureGeneration()
{
    return FontAtlasTextureBinding().generation;
}
}
