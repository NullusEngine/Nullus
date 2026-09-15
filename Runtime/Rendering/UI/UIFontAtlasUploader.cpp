#include "UIFontAtlasUploader.h"

#include "Rendering/Context/DriverAccess.h"
#include "Rendering/UI/UIFontAtlas.h"
#include "Rendering/UI/UIFontTextShaper.h"

#include <Debug/Logger.h>

#include <algorithm>
#include <string_view>
#include <utility>

namespace NLS::Render::UI
{
namespace
{
uint32_t PageStackOffset(const std::vector<UIFontAtlasPage>& pages, const uint32_t pageIndex)
{
    uint32_t offset = 0u;
    for (const auto& page : pages)
    {
        if (page.index == pageIndex)
            return offset;
        offset += page.height;
    }
    return offset;
}

uint32_t PageStackHeight(const std::vector<UIFontAtlasPage>& pages)
{
    uint32_t height = 0u;
    for (const auto& page : pages)
        height += page.height;
    return height;
}
}

UIFontAtlasImage BuildUIFontAtlasImage(const UIFontAtlas& atlas)
{
    UIFontAtlasImage image;
    const auto& pages = atlas.GetPages();
    if (pages.empty())
        return image;

    for (const auto& page : pages)
        image.width = (std::max)(image.width, page.width);
    image.height = PageStackHeight(pages);
    image.pageCount = static_cast<uint32_t>(pages.size());
    if (image.width == 0u || image.height == 0u)
        return image;

    image.rgba.assign(static_cast<size_t>(image.width) * image.height * 4u, 0u);
    for (const auto& page : pages)
    {
        const size_t expected = static_cast<size_t>(page.width) * page.height * 4u;
        if (page.pixels.size() != expected)
            continue;

        const uint32_t pageOffset = PageStackOffset(pages, page.index);
        for (uint32_t row = 0u; row < page.height; ++row)
        {
            const size_t sourceOffset = static_cast<size_t>(row) * page.width * 4u;
            const size_t destinationOffset =
                (static_cast<size_t>(pageOffset + row) * image.width) * 4u;
            std::copy(
                page.pixels.begin() + static_cast<std::ptrdiff_t>(sourceOffset),
                page.pixels.begin() + static_cast<std::ptrdiff_t>(sourceOffset + page.width * 4u),
                image.rgba.begin() + static_cast<std::ptrdiff_t>(destinationOffset));
        }
    }
    return image;
}

std::array<float, 2> MapUIFontAtlasPageUv(
    const UIFontAtlas& atlas,
    const uint32_t pageIndex,
    const float pageU,
    const float pageV)
{
    const auto& pages = atlas.GetPages();
    const uint32_t stackHeight = PageStackHeight(pages);
    if (stackHeight == 0u)
        return { pageU, pageV };

    uint32_t pageOffset = 0u;
    uint32_t pageHeight = 0u;
    for (const auto& page : pages)
    {
        if (page.index == pageIndex)
        {
            pageHeight = page.height;
            break;
        }
        pageOffset += page.height;
    }
    if (pageHeight == 0u)
        return { pageU, pageV };

    return {
        (std::clamp)(pageU, 0.0f, 1.0f),
        (static_cast<float>(pageOffset) +
            (std::clamp)(pageV, 0.0f, 1.0f) * static_cast<float>(pageHeight)) /
                static_cast<float>(stackHeight) };
}

namespace
{
// Printable ASCII plus the space, pre-warmed so the very first upload already
// carries usable glyphs instead of an empty page.
constexpr std::string_view kDefaultUiFontPrewarmText =
    " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~";
constexpr uint64_t kDefaultUiFontAssetId = 1u;
constexpr uint16_t kDefaultUiFontPixelSize = 16u;
}

bool UIFontAtlasUploader::Pump(NLS::Render::Context::Driver& driver, UIFontAtlas& atlas)
{
    if (!m_prewarmed && atlas.HasFontData())
    {
        (void)atlas.EnsureText(
            kDefaultUiFontAssetId,
            kDefaultUiFontPrewarmText,
            kDefaultUiFontPixelSize,
            1u,
            true);
        m_prewarmed = true;
    }

    if (m_requestId != 0u)
    {
        auto result = NLS::Render::Context::DriverUIAccess::ConsumeUiRgba8TextureUploadResult(
            driver,
            m_requestId);
        if (result.ready)
        {
            m_requestId = 0u;
            m_requestAgeFrames = 0u;
            if (result.success && result.texture != nullptr && result.textureView != nullptr)
            {
                const auto textureId = NLS::Render::Context::DriverUIAccess::RegisterUiTextureView(
                    driver,
                    result.textureView,
                    UiTextureSynchronizationScope::PreviousFrameOrStatic);
                if (textureId.IsValid())
                {
                    if (m_textureView != nullptr)
                    {
                        NLS::Render::Context::DriverUIAccess::ReleaseUiTextureView(
                            driver,
                            m_textureView);
                    }
                    m_texture = std::move(result.texture);
                    m_textureView = std::move(result.textureView);
                    m_textureId = textureId;
                    m_width = result.width;
                    m_height = result.height;
                    m_readyGeneration = m_requestedGeneration;
                    m_uploadedCoverage = std::move(m_pendingCoverage);
                    m_uploadedCoverageWidth = m_pendingCoverageWidth;
                    m_uploadedCoverageHeight = m_pendingCoverageHeight;
                    m_pendingCoverage.clear();
                    m_pendingCoverageWidth = 0u;
                    m_pendingCoverageHeight = 0u;
                    SetUiFontAtlasTextureId(textureId.value, textureId.generation);
                    NLS_LOG_INFO(
                        "[UIFontAtlas] uploaded glyph atlas: textureId=" +
                        std::to_string(textureId.value) + " size=" + std::to_string(m_width) +
                        "x" + std::to_string(m_height) + " contentGeneration=" +
                        std::to_string(m_readyGeneration));
                }
            }
            else
            {
                NLS_LOG_WARNING(
                    "[UIFontAtlas] glyph atlas upload failed for contentGeneration=" +
                    std::to_string(m_requestedGeneration) + ": " +
                    (result.diagnostic.empty() ? std::string("no diagnostic") : result.diagnostic));
                m_retryCooldownFrames = 60u;
            }
        }
        else
        {
            ++m_requestAgeFrames;
            // A dropped result must not pin the atlas forever.
            if (m_requestAgeFrames >= 120u)
            {
                NLS_LOG_WARNING(
                    "[UIFontAtlas] glyph atlas upload never completed; cancelling request " +
                    std::to_string(m_requestId) + " for contentGeneration=" +
                    std::to_string(m_requestedGeneration) + " and retrying");
                NLS::Render::Context::DriverUIAccess::CancelUiRgba8TextureUpload(driver, m_requestId);
                m_requestId = 0u;
                m_requestAgeFrames = 0u;
            }
        }
    }

    if (m_retryCooldownFrames > 0u)
    {
        --m_retryCooldownFrames;
    }
    else if (m_requestId == 0u)
    {
        const uint64_t generation = atlas.GetContentGeneration();
        if (generation != m_readyGeneration)
        {
            auto image = BuildUIFontAtlasImage(atlas);
            if (image.width != 0u && image.height != 0u && !image.rgba.empty())
            {
                // Keep the alpha channel of exactly this image. When the upload
                // completes, this becomes the ground truth of what the sampled
                // texture contains; diagnostics compare it against the geometry.
                m_pendingCoverageWidth = image.width;
                m_pendingCoverageHeight = image.height;
                m_pendingCoverage.resize(
                    static_cast<size_t>(image.width) * image.height);
                for (size_t pixel = 0u; pixel < m_pendingCoverage.size(); ++pixel)
                    m_pendingCoverage[pixel] = image.rgba[pixel * 4u + 3u];

                NLS::Render::Context::DriverUIAccess::Rgba8TextureUploadRequest request;
                request.width = image.width;
                request.height = image.height;
                request.rgbaPixels = std::move(image.rgba);
                request.debugName = "UiFontAtlas";
                m_requestedGeneration = generation;
                m_requestId = NLS::Render::Context::DriverUIAccess::RequestUiRgba8TextureUpload(
                    driver,
                    std::move(request));
                if (m_requestId == 0u)
                {
                    NLS_LOG_WARNING(
                        "[UIFontAtlas] glyph atlas upload request was rejected for "
                        "contentGeneration=" + std::to_string(generation) + " size=" +
                        std::to_string(image.width) + "x" + std::to_string(image.height));
                    m_retryCooldownFrames = 60u;
                }
            }
        }
    }

    return m_textureId.IsValid();
}

void UIFontAtlasUploader::Release(NLS::Render::Context::Driver& driver)
{
    if (m_requestId != 0u)
    {
        NLS::Render::Context::DriverUIAccess::CancelUiRgba8TextureUpload(driver, m_requestId);
        m_requestId = 0u;
    }
    if (m_textureView != nullptr)
    {
        NLS::Render::Context::DriverUIAccess::ReleaseUiTextureView(driver, m_textureView);
        m_textureView.reset();
    }
    m_texture.reset();
    m_textureId = {};
    m_readyGeneration = 0u;
    m_width = 0u;
    m_height = 0u;
    m_uploadedCoverage.clear();
    m_uploadedCoverageWidth = 0u;
    m_uploadedCoverageHeight = 0u;
    m_pendingCoverage.clear();
    m_pendingCoverageWidth = 0u;
    m_pendingCoverageHeight = 0u;
    ClearUiFontAtlasTextureId();
}

uint8_t UIFontAtlasUploader::SampleUploadedCoverage(const uint32_t x, const uint32_t y) const
{
    if (x >= m_uploadedCoverageWidth || y >= m_uploadedCoverageHeight)
        return 0u;
    const auto index = static_cast<size_t>(y) * m_uploadedCoverageWidth + x;
    return index < m_uploadedCoverage.size() ? m_uploadedCoverage[index] : 0u;
}
}
