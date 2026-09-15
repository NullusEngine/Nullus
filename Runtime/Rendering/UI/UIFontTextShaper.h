#pragma once

#include "RenderDef.h"
#include "Rendering/UI/UIDrawSnapshot.h"

#include <cstdint>

namespace NLS::Render::UI
{
    class UIFontAtlas;

    // Converts the snapshot's text runs into textured glyph quads. Text runs stay
    // in the snapshot as metadata for tooling, but geometry is what the renderer
    // consumes. Missing glyph data still emits a tofu quad so text never silently
    // disappears. Returns the number of emitted quads.
    //
    // Glyph quads sample the registered UI font-atlas texture. Until a host
    // registers one there is nothing meaningful to sample, so no geometry is
    // emitted and text keeps its previous (invisible) behaviour instead of
    // drawing with unmatched atlas coordinates.
    NLS_RENDER_API void SetUiFontAtlasTextureId(uint64_t textureId, uint32_t generation);
    NLS_RENDER_API void ClearUiFontAtlasTextureId();
    [[nodiscard]] NLS_RENDER_API bool HasUiFontAtlasTextureId();
    // Identity of the texture glyph quads currently sample. Hosts rebuild cached
    // geometry when this or the atlas content generation changes.
    [[nodiscard]] NLS_RENDER_API uint64_t GetUiFontAtlasTextureId();
    [[nodiscard]] NLS_RENDER_API uint32_t GetUiFontAtlasTextureGeneration();

    NLS_RENDER_API uint32_t AppendTextQuadGeometry(
        UIDrawSnapshot& snapshot,
        UIFontAtlas& atlas,
        uint64_t frameId,
        bool fontAvailable = true);

    // Convenience overload using the process-wide atlas.
    NLS_RENDER_API uint32_t AppendTextQuadGeometry(
        UIDrawSnapshot& snapshot,
        uint64_t frameId);
}
