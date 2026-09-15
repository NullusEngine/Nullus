#pragma once

#include "RenderDef.h"
#include "Rendering/UI/UiDrawDataSnapshot.h"
#include "Rendering/UI/UIDrawSnapshot.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace NLS::Render::RHI
{
    class RHITexture;
    class RHITextureView;
}

namespace NLS::Render::Context
{
    class Driver;
}

namespace NLS::Render::UI
{
    class UIFontAtlas;

    // Atlas pages are stacked vertically into one RGBA8 image: page i occupies the
    // rows [offset(i), offset(i) + page.height). Glyph UVs are page-relative, so a
    // consumer that samples the stacked image must map them through
    // MapUIFontAtlasPageUv.
    struct NLS_RENDER_API UIFontAtlasImage
    {
        uint32_t width = 0u;
        uint32_t height = 0u;
        uint32_t pageCount = 0u;
        std::vector<uint8_t> rgba;
    };

    NLS_RENDER_API UIFontAtlasImage BuildUIFontAtlasImage(const UIFontAtlas& atlas);

    // Maps a page-relative UV (inside one atlas page) into the stacked image.
    NLS_RENDER_API std::array<float, 2> MapUIFontAtlasPageUv(
        const UIFontAtlas& atlas,
        uint32_t pageIndex,
        float pageU,
        float pageV);

    // Uploads changed atlas pages and registers the result as a UI texture, then
    // publishes it through SetUiFontAtlasTextureId so glyph quads sample real
    // pixels. The upload itself runs on the render thread, so Pump() reports ready
    // a frame or two after the atlas changed.
    class NLS_RENDER_API UIFontAtlasUploader final
    {
    public:
        // Takes a mutable atlas so the first pump can pre-warm the default glyph
        // set; otherwise a text-only document never rasterizes anything and the
        // atlas would stay empty while text waits for a texture to sample.
        bool Pump(NLS::Render::Context::Driver& driver, UIFontAtlas& atlas);
        void Release(NLS::Render::Context::Driver& driver);

        [[nodiscard]] UiTextureId GetTextureId() const { return m_textureId; }
        [[nodiscard]] uint32_t GetWidth() const { return m_width; }
        [[nodiscard]] uint32_t GetHeight() const { return m_height; }
        [[nodiscard]] uint64_t GetReadyGeneration() const { return m_readyGeneration; }
        [[nodiscard]] bool IsUploadPending() const { return m_requestId != 0u; }
        // Coverage of the image that the sampled texture actually holds. Diagnostics
        // use it to answer "does the texture contain ink where the geometry samples",
        // which is the difference between correct pixels and invisible glyphs.
        [[nodiscard]] bool HasUploadedCoverage() const { return m_uploadedCoverageWidth != 0u; }
        [[nodiscard]] uint32_t GetUploadedCoverageWidth() const { return m_uploadedCoverageWidth; }
        [[nodiscard]] uint32_t GetUploadedCoverageHeight() const { return m_uploadedCoverageHeight; }
        [[nodiscard]] uint8_t SampleUploadedCoverage(uint32_t x, uint32_t y) const;
        // Forces the next pump to upload the atlas again. Used when the sampled
        // texture is proven not to match the atlas the geometry was built from.
        void InvalidateUploadedGeneration()
        {
            m_readyGeneration = 0u;
            m_retryCooldownFrames = 0u;
        }

    private:
        bool m_prewarmed = false;
        uint64_t m_requestId = 0u;
        // Frames the in-flight request has been waiting for its result. A request
        // whose result never arrives would otherwise pin the atlas to stale pixels
        // forever, because no further upload is ever started.
        uint32_t m_requestAgeFrames = 0u;
        // Backs off after a rejected or failed upload so a broken path cannot
        // allocate a multi-megabyte atlas image every frame.
        uint32_t m_retryCooldownFrames = 0u;
        uint64_t m_requestedGeneration = 0u;
        uint64_t m_readyGeneration = 0u;
        uint32_t m_width = 0u;
        uint32_t m_height = 0u;
        UiTextureId m_textureId {};
        std::shared_ptr<RHI::RHITexture> m_texture;
        std::shared_ptr<RHI::RHITextureView> m_textureView;
        // Coverage of the last successfully uploaded image, and of the request that
        // is currently in flight (published only once that upload succeeds).
        std::vector<uint8_t> m_uploadedCoverage;
        uint32_t m_uploadedCoverageWidth = 0u;
        uint32_t m_uploadedCoverageHeight = 0u;
        std::vector<uint8_t> m_pendingCoverage;
        uint32_t m_pendingCoverageWidth = 0u;
        uint32_t m_pendingCoverageHeight = 0u;
    };
}
