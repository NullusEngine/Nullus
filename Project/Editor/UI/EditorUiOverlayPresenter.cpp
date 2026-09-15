#include "UI/EditorUiOverlayPresenter.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <Components/UICanvasComponent.h>
#include <Engine/UI/UIDocumentSourceResolver.h>
#include <Engine/UI/UIElement.h>
#include <Engine/SceneSystem/Scene.h>
#include <ImGui/imgui.h>
#include <Rendering/UI/UIDrawSnapshotAdapter.h>
#include <Rendering/UI/UIFontAtlas.h>
#include <Rendering/UI/UIFontTextShaper.h>
#include <Debug/Logger.h>

#include "Core/EditorActions.h"
#include "Panels/GameView.h"

namespace NLS::Editor::UiOverlay
{
namespace
{
// Rasterizes every glyph a document can currently draw. Doing this before the
// atlas is uploaded means the first texture already contains the document's
// glyphs, so rendering never depends on a follow-up upload landing.
void PrewarmElementGlyphs(
    const NLS::Engine::UI::UIElement& element,
    NLS::Render::UI::UIFontAtlas& atlas,
    const uint64_t frameId)
{
    if (!element.GetText().empty())
    {
        const auto fontAssetId = element.GetFontAssetId() != 0u ? element.GetFontAssetId() : 1u;
        const auto pixelSize = element.GetFontPixelSize() != 0u ? element.GetFontPixelSize() : 16u;
        (void)atlas.EnsureText(fontAssetId, element.GetText(), pixelSize, frameId, true);
    }
    for (const auto& child : element.GetChildren())
    {
        if (child != nullptr)
            PrewarmElementGlyphs(*child, atlas, frameId);
    }
}
}

void EditorUiOverlayPresenter::PumpFontAtlas(NLS::Render::Context::Driver& driver)
{
    auto& atlas = NLS::Render::UI::GetSharedUIFontAtlas();
    for (auto* canvas : m_uiRuntimeService.GetCanvases())
    {
        auto* instance = canvas != nullptr ? canvas->GetInstance() : nullptr;
        auto* root = instance != nullptr ? instance->GetRoot() : nullptr;
        if (root == nullptr)
            continue;
        PrewarmElementGlyphs(*root, atlas, m_frameId + 1u);
    }

    const bool ready = m_fontAtlasUploader.Pump(driver, atlas);
    if (!ready)
        return;

    // Document snapshots are cached by layout/paint dirtiness, so glyph geometry
    // is only correct for the atlas content it was built against. Rebuild after
    // the atlas becomes sampleable and whenever its content generation moves on
    // (new glyph, page eviction, font change); otherwise cached quads keep
    // sampling cells the uploaded texture does not contain yet.
    // Cached glyph quads embed both the atlas layout and the texture identity, so
    // a completed upload (new texture id) is just as invalidating as new glyphs.
    const auto atlasGeneration = NLS::Render::UI::GetSharedUIFontAtlas().GetContentGeneration();
    const auto textureId = NLS::Render::UI::GetUiFontAtlasTextureId();
    const auto textureGeneration = NLS::Render::UI::GetUiFontAtlasTextureGeneration();
    if (atlasGeneration != m_textAtlasGeneration ||
        textureId != m_textTextureId ||
        textureGeneration != m_textTextureGeneration)
    {
        m_textAtlasGeneration = atlasGeneration;
        m_textTextureId = textureId;
        m_textTextureGeneration = textureGeneration;
        InvalidateCanvasPaintCaches();
        NLS_LOG_INFO(
            "[UIOverlay] UI font atlas ready: textureId=" + std::to_string(textureId) +
            " textureGeneration=" + std::to_string(textureGeneration) +
            " contentGeneration=" + std::to_string(atlasGeneration));
    }

    // A texture that lags the atlas keeps sampling cells that do not exist yet,
    // which shows up as individual glyphs vanishing. Report it once it persists.
    if (m_fontAtlasUploader.GetReadyGeneration() != atlasGeneration)
    {
        ++m_atlasStaleFrames;
        if (m_atlasStaleFrames == 120u)
        {
            NLS_LOG_WARNING(
                "[UIOverlay] font atlas texture is stale: uploadedContentGeneration=" +
                std::to_string(m_fontAtlasUploader.GetReadyGeneration()) +
                " atlasContentGeneration=" + std::to_string(atlasGeneration) +
                " uploadPending=" +
                std::string(m_fontAtlasUploader.IsUploadPending() ? "yes" : "no"));
        }
    }
    else
    {
        m_atlasStaleFrames = 0u;
    }
}

void EditorUiOverlayPresenter::SyncSceneCanvases()
{
    std::vector<NLS::Engine::Components::UICanvasComponent*> canvases;
    if (auto* scene = EDITOR_CONTEXT(sceneManager).GetCurrentScene())
        canvases = scene->GetFastAccessComponents().uiCanvases;
    m_uiRuntimeService.SyncCanvases(canvases);
}

void EditorUiOverlayPresenter::ResolvePendingDocuments()
{
    bool warned = false;
    for (auto* canvas : m_uiRuntimeService.GetCanvases())
    {
        if (canvas == nullptr || canvas->GetDocument() != nullptr)
            continue;

        std::string owner;
        if (auto* gameObject = canvas->gameobject())
            owner = " on '" + gameObject->GetName() + "'";

        const auto& reference = canvas->GetDocumentReference();
        const auto& storedPath = reference.GetResourcePath();
        if (storedPath.empty())
        {
            warned = true;
            ReportDocumentWarning(
                "[UIOverlay] UICanvas" + owner +
                " has no UI document assigned; set the Document field in the Inspector to a .ui "
                "asset to render it in the Game View.");
            continue;
        }

        // The serialized reference stays untouched; only the live document is
        // restored from the resolved source path.
        const auto sourcePath = NLS::Engine::UI::ResolveUIDocumentSourcePath(reference);
        if (sourcePath.empty())
        {
            warned = true;
            ReportDocumentWarning(
                "[UIOverlay] UICanvas" + owner + " references UI document '" + storedPath +
                "', which could not be resolved to an existing source file under Assets.");
            continue;
        }

        std::vector<NLS::Engine::UI::UIDocumentDiagnostic> diagnostics;
        if (!canvas->LoadDocumentResolvedPath(sourcePath, &diagnostics))
        {
            warned = true;
            std::string message =
                "[UIOverlay] UI document '" + sourcePath + "'" + owner + " failed to load";
            if (!diagnostics.empty())
                message += ": " + diagnostics.front().path + " " + diagnostics.front().message;
            ReportDocumentWarning(message);
        }
    }

    if (!warned)
        m_lastDocumentWarning.clear();
}

void EditorUiOverlayPresenter::ReportDocumentWarning(const std::string& message)
{
    if (message == m_lastDocumentWarning)
        return;
    m_lastDocumentWarning = message;
    NLS_LOG_WARNING(message);
}

void EditorUiOverlayPresenter::ReportPresentationNotice(const std::string& message)
{
    if (message == m_lastPresentationNotice)
        return;
    m_lastPresentationNotice = message;
    NLS_LOG_INFO(message);
}

void EditorUiOverlayPresenter::ReportTextRunFonts(
    const NLS::Render::UI::UIDrawSnapshot& snapshot)
{
    std::string descriptor;
    for (const auto& run : snapshot.textRuns)
    {
        if (descriptor.empty() == false)
            descriptor += "; ";
        descriptor += "'" + run.text + "' pixelSize=" + std::to_string(run.fontPixelSize) +
            " fontAssetId=" + std::to_string(run.fontAssetId) +
            " clip=" + std::to_string(run.clipIndex);
    }
    if (descriptor.empty() || descriptor == m_lastTextRunFonts)
        return;

    m_lastTextRunFonts = descriptor;
    NLS_LOG_INFO(
        "[UIOverlay] text runs: " + descriptor +
        " | atlasTextureId=" + std::to_string(NLS::Render::UI::GetUiFontAtlasTextureId()) +
        " atlasContentGeneration=" +
        std::to_string(NLS::Render::UI::GetSharedUIFontAtlas().GetContentGeneration()));
}

void EditorUiOverlayPresenter::AuditTextRunAtlasInk(
    const NLS::Render::UI::UIDrawSnapshot& snapshot)
{
    if (snapshot.textRuns.empty())
        return;

    std::string signature;
    for (const auto& run : snapshot.textRuns)
    {
        signature += run.text;
        signature += '#';
        signature += std::to_string(run.fontPixelSize);
    }
    if (signature == m_lastTextInkAudit)
        return;
    m_lastTextInkAudit = signature;

    auto& atlas = NLS::Render::UI::GetSharedUIFontAtlas();
    const auto image = NLS::Render::UI::BuildUIFontAtlasImage(atlas);
    if (image.width == 0u || image.height == 0u)
    {
        NLS_LOG_WARNING("[UIOverlay] text ink audit: the font atlas image is empty");
        return;
    }

    std::string emptyCodepoints;
    uint32_t checked = 0u;
    for (const auto& run : snapshot.textRuns)
    {
        const auto pixelSize = run.fontPixelSize != 0u
            ? run.fontPixelSize
            : static_cast<uint16_t>(run.fontSize);
        const auto fontAssetId = run.fontAssetId != 0u ? run.fontAssetId : 1u;
        for (const char raw : run.text)
        {
            const auto codepoint = static_cast<uint32_t>(static_cast<unsigned char>(raw));
            if (codepoint == ' ' || codepoint == '\n' || codepoint == '\r')
                continue;

            const auto glyph = atlas.Find({ fontAssetId, codepoint, pixelSize });
            if (!glyph.has_value())
            {
                emptyCodepoints += " U+" + std::to_string(codepoint) + "(no-glyph)";
                continue;
            }

            const auto left = static_cast<int32_t>(glyph->uv[0] * static_cast<float>(image.width));
            const auto top = static_cast<int32_t>(glyph->uv[1] * static_cast<float>(image.height));
            const auto right = static_cast<int32_t>(glyph->uv[2] * static_cast<float>(image.width));
            const auto bottom = static_cast<int32_t>(glyph->uv[3] * static_cast<float>(image.height));
            uint32_t ink = 0u;
            for (int32_t y = top; y < bottom; ++y)
            {
                for (int32_t x = left; x < right; ++x)
                {
                    if (x < 0 || y < 0 || x >= static_cast<int32_t>(image.width) ||
                        y >= static_cast<int32_t>(image.height))
                        continue;
                    if (image.rgba[(static_cast<size_t>(y) * image.width +
                                    static_cast<uint32_t>(x)) * 4u + 3u] != 0u)
                        ++ink;
                }
            }
            ++checked;
            if (ink == 0u)
                emptyCodepoints += " U+" + std::to_string(codepoint) + "(" +
                    std::to_string(left) + "," + std::to_string(top) + ")";
        }
    }

    if (emptyCodepoints.empty())
    {
        NLS_LOG_INFO(
            "[UIOverlay] text ink audit: all " + std::to_string(checked) +
            " glyph(s) carry ink in the freshly built atlas image");
    }
    else
    {
        NLS_LOG_WARNING(
            "[UIOverlay] text ink audit: empty atlas cells for" + emptyCodepoints +
            " (checked " + std::to_string(checked) + ")");
    }
}

void EditorUiOverlayPresenter::AuditGlyphQuadCoverage(
    const NLS::Render::UI::UIDrawSnapshot& snapshot)
{
    if (!m_fontAtlasUploader.HasUploadedCoverage())
        return;

    const auto width = static_cast<float>(m_fontAtlasUploader.GetUploadedCoverageWidth());
    const auto height = static_cast<float>(m_fontAtlasUploader.GetUploadedCoverageHeight());
    if (width <= 0.0f || height <= 0.0f)
        return;

    const size_t quadCount = snapshot.vertices.size() / 4u;
    if (quadCount == 0u || quadCount == m_lastQuadCoverageQuads)
        return;
    m_lastQuadCoverageQuads = quadCount;

    size_t quadsWithInk = 0u;
    std::string emptyQuads;
    std::string quadDetails;
    for (size_t quad = 0u; quad + 4u <= snapshot.vertices.size(); quad += 4u)
    {
        const auto& first = snapshot.vertices[quad];
        const auto& opposite = snapshot.vertices[quad + 2u];
        // Quads are stored four vertices at a time (TL, TR, BR, BL). Taking the
        // min/max of the two ends keeps the rect correct regardless of winding.
        const auto left = static_cast<int32_t>((std::min)(first.uv[0], opposite.uv[0]) * width);
        const auto top = static_cast<int32_t>((std::min)(first.uv[1], opposite.uv[1]) * height);
        const auto right = static_cast<int32_t>((std::max)(first.uv[0], opposite.uv[0]) * width);
        const auto bottom = static_cast<int32_t>((std::max)(first.uv[1], opposite.uv[1]) * height);

        size_t ink = 0u;
        for (int32_t y = top; y < bottom; ++y)
        {
            for (int32_t x = left; x < right; ++x)
            {
                if (x < 0 || y < 0 || x >= static_cast<int32_t>(width) ||
                    y >= static_cast<int32_t>(height))
                    continue;
                if (m_fontAtlasUploader.SampleUploadedCoverage(
                        static_cast<uint32_t>(x), static_cast<uint32_t>(y)) != 0u)
                    ++ink;
            }
        }
        if (ink != 0u)
        {
            ++quadsWithInk;
        }
        else
        {
            emptyQuads += " [" + std::to_string(left) + "," + std::to_string(top) + ".." +
                std::to_string(right) + "," + std::to_string(bottom) + "]";
        }
        quadDetails += " q" + std::to_string(quad / 4u) + "=" + std::to_string(left) + "," +
            std::to_string(top) + ".." + std::to_string(right) + "," + std::to_string(bottom) +
            " ink=" + std::to_string(ink);
    }

    // Decisive check: does the baked geometry still describe the atlas the
    // texture holds? A stale snapshot keeps sampling the cells its glyphs used to
    // occupy, so compare each quad against the atlas' current glyph rect.
    std::string mismatchDetails;
    size_t expectedGlyphCount = 0u;
    {
        std::vector<std::array<float, 4>> expected;
        auto& atlas = NLS::Render::UI::GetSharedUIFontAtlas();
        for (const auto& run : snapshot.textRuns)
        {
            const auto pixelSize = run.fontPixelSize != 0u
                ? run.fontPixelSize
                : static_cast<uint16_t>(run.fontSize);
            const auto fontAssetId = run.fontAssetId != 0u ? run.fontAssetId : 1u;
            for (const char raw : run.text)
            {
                const auto codepoint = static_cast<uint32_t>(static_cast<unsigned char>(raw));
                if (codepoint == ' ' || codepoint == '\n' || codepoint == '\r' || codepoint == '\t')
                    continue;
                const auto glyph = atlas.Find({ fontAssetId, codepoint, pixelSize });
                if (!glyph.has_value())
                    continue;
                const auto topLeft = NLS::Render::UI::MapUIFontAtlasPageUv(
                    atlas, glyph->pageIndex, glyph->uv[0], glyph->uv[1]);
                const auto bottomRight = NLS::Render::UI::MapUIFontAtlasPageUv(
                    atlas, glyph->pageIndex, glyph->uv[2], glyph->uv[3]);
                expected.push_back({ topLeft[0], topLeft[1], bottomRight[0], bottomRight[1] });
            }
        }

        const size_t compared = (std::min)(expected.size(), snapshot.vertices.size() / 4u);
        expectedGlyphCount = expected.size();
        for (size_t quad = 0u; quad < compared; ++quad)
        {
            const auto& first = snapshot.vertices[quad * 4u];
            const auto& opposite = snapshot.vertices[quad * 4u + 2u];
            const auto bakedU = (std::min)(first.uv[0], opposite.uv[0]);
            const auto bakedV = (std::min)(first.uv[1], opposite.uv[1]);
            const bool matches =
                std::abs(bakedU - expected[quad][0]) <= 0.0005f &&
                std::abs(bakedV - expected[quad][1]) <= 0.0005f;
            if (matches)
                continue;
            mismatchDetails += " q" + std::to_string(quad) +
                " baked=" + std::to_string(bakedU) + "," + std::to_string(bakedV) +
                " atlas=" + std::to_string(expected[quad][0]) + "," +
                std::to_string(expected[quad][1]);
        }
    }

    std::string rawUvs;
    for (size_t index = 0u; index < (std::min)(snapshot.vertices.size(), size_t {8u}); ++index)
    {
        const auto& vertex = snapshot.vertices[index];
        rawUvs += " v" + std::to_string(index) + "=" + std::to_string(vertex.uv[0]) + "," +
            std::to_string(vertex.uv[1]);
    }
    NLS_LOG_INFO(
        "[UIOverlay] glyph quads: vertices=" + std::to_string(snapshot.vertices.size()) +
        " indices=" + std::to_string(snapshot.indices.size()) +
        " runs=" + std::to_string(snapshot.textRuns.size()) +
        " expectedGlyphs=" + std::to_string(expectedGlyphCount) + rawUvs);
    NLS_LOG_INFO("[UIOverlay] glyph quads rects:" + quadDetails);
    if (!mismatchDetails.empty())
    {
        NLS_LOG_WARNING(
            "[UIOverlay] glyph quads do not match the current atlas:" + mismatchDetails);
    }

    if (emptyQuads.empty())
    {
        m_quadCoverageRetries = 0u;
        NLS_LOG_INFO(
            "[UIOverlay] glyph sample audit: " + std::to_string(quadsWithInk) + "/" +
            std::to_string(quadCount) +
            " quad(s) sample ink in the uploaded texture");
    }
    else
    {
        NLS_LOG_WARNING(
            "[UIOverlay] glyph sample audit: " + std::to_string(quadsWithInk) + "/" +
            std::to_string(quadCount) +
            " quad(s) sample ink; empty at uploaded-texel rects" + emptyQuads);
        // The sampled texture provably does not contain what the geometry reads,
        // so its content is behind the atlas. Upload it again instead of leaving
        // the document with missing glyphs until some unrelated work happens to
        // refresh the texture.
        if (m_quadCoverageRetries < 3u)
        {
            ++m_quadCoverageRetries;
            m_fontAtlasUploader.InvalidateUploadedGeneration();
            m_lastQuadCoverageQuads = 0u;
            NLS_LOG_WARNING(
                "[UIOverlay] glyph sample audit: re-uploading the font atlas (attempt " +
                std::to_string(m_quadCoverageRetries) + " of 3)");
        }
    }
}

void EditorUiOverlayPresenter::InvalidateCanvasPaintCaches()
{
    for (auto* canvas : m_uiRuntimeService.GetCanvases())
    {
        if (canvas == nullptr)
            continue;
        auto* instance = canvas->GetInstance();
        if (instance == nullptr)
            continue;
        if (auto* root = instance->GetRoot())
            root->MarkPaintDirty();
    }
}

void EditorUiOverlayPresenter::AppendToOverlaySnapshot(
    NLS::Render::UI::UiDrawDataSnapshot& snapshot)
{
    SyncSceneCanvases();
    if (m_uiRuntimeService.GetCanvases().empty())
    {
        LogDiagnostics(0u, 0u, false, 0u, 0u);
        return;
    }

    ResolvePendingDocuments();

    auto& gameView = EDITOR_PANEL(NLS::Editor::Panels::GameView, "Game View");
    if (!gameView.IsOpened())
    {
        ReportPresentationNotice(
            "[UIOverlay] " + std::to_string(m_uiRuntimeService.GetCanvases().size()) +
            " canvas(es) are ready; open Window > Game View to present the in-game UI.");
        LogDiagnostics(m_uiRuntimeService.GetCanvases().size(), 0u, false, 0u, 0u);
        return;
    }
    if (!gameView.HasViewportImageBounds())
    {
        ReportPresentationNotice(
            "[UIOverlay] the Game View panel is open but has not produced a viewport image yet; the "
            "canvas is presented as soon as the panel renders its first frame.");
        LogDiagnostics(m_uiRuntimeService.GetCanvases().size(), 0u, false, 0u, 0u);
        return;
    }

    const auto min = gameView.GetViewportImageMin();
    const auto max = gameView.GetViewportImageMax();
    const auto viewWidth = static_cast<uint32_t>((std::max)(1.0f, std::round(max.x - min.x)));
    const auto viewHeight = static_cast<uint32_t>((std::max)(1.0f, std::round(max.y - min.y)));

    ++m_frameId;
    const float deltaSeconds = ImGui::GetIO().DeltaTime;
    m_uiRuntimeService.Tick(
        deltaSeconds,
        { static_cast<float>(viewWidth), static_cast<float>(viewHeight) },
        1.0f,
        m_frameId);

    auto retained = m_uiRuntimeService.BuildScreenSnapshot(viewWidth, viewHeight, 1.0f, m_frameId);
    if (retained == nullptr)
    {
        LogDiagnostics(m_uiRuntimeService.GetCanvases().size(), 0u, true, 0u, 0u);
        return;
    }

    ReportTextRunFonts(*retained);
    AuditTextRunAtlasInk(*retained);
    AuditGlyphQuadCoverage(*retained);

    auto adapted = NLS::Render::UI::AdaptUIDrawSnapshot(retained);
    if (adapted == nullptr || adapted->drawLists.empty() || !adapted->hasVisibleDraws)
    {
        LogDiagnostics(m_uiRuntimeService.GetCanvases().size(), 0u, true, 0u, 0u);
        return;
    }

    // Translate the view-local geometry into overlay space and clip it to the
    // view rect so a game UI never bleeds over the editor interface.
    NLS::Render::UI::UiDrawDataSnapshot translated = *adapted;
    // Counted while the lists are still owned here: pushing them into the
    // overlay snapshot moves them out and would report zero vertices.
    size_t appendedVertices = 0u;
    size_t appendedDrawLists = 0u;
    size_t appendedIndices = 0u;
    for (auto& drawList : translated.drawLists)
    {
        appendedVertices += drawList.vertices.size();
        appendedIndices += drawList.indices.size();
        ++appendedDrawLists;
        for (auto& vertex : drawList.vertices)
        {
            vertex.position[0] += min.x;
            vertex.position[1] += min.y;
        }
        for (auto& command : drawList.commands)
        {
            command.clipRect[0] = (std::clamp)(command.clipRect[0] + min.x, min.x, max.x);
            command.clipRect[1] = (std::clamp)(command.clipRect[1] + min.y, min.y, max.y);
            command.clipRect[2] = (std::clamp)(command.clipRect[2] + min.x, min.x, max.x);
            command.clipRect[3] = (std::clamp)(command.clipRect[3] + min.y, min.y, max.y);
        }
        snapshot.drawLists.push_back(std::move(drawList));
    }

    snapshot.drawListCount = static_cast<uint32_t>(snapshot.drawLists.size());
    snapshot.hasVisibleDraws = true;
    // The overlay renderer sizes its dynamic buffers from these totals, so the
    // appended geometry has to be added here. Leaving them at the ImGui-only
    // counts under-allocated the vertex/index buffers and the upload wrote past
    // the end, which showed up as missing or corrupted glyphs.
    snapshot.totalVertexCount += static_cast<uint32_t>(appendedVertices);
    snapshot.totalIndexCount += static_cast<uint32_t>(appendedIndices);
    // The overlay renderer sizes its dynamic buffers from these totals, so the
    // appended geometry has to be added here. Leaving them at the ImGui-only
    // counts made the upload write past the end of the vertex/index buffers,
    // which showed up as missing or corrupted glyphs.

    LogDiagnostics(
        m_uiRuntimeService.GetCanvases().size(),
        0u,
        true,
        appendedDrawLists,
        appendedVertices);
}

void EditorUiOverlayPresenter::LogDiagnostics(
    const size_t canvasCount,
    const size_t documentCount,
    const bool hasViewBounds,
    const size_t appendedDrawLists,
    const size_t appendedVertices)
{
    size_t loadedDocuments = documentCount;
    for (const auto* canvas : m_uiRuntimeService.GetCanvases())
    {
        if (canvas != nullptr && canvas->GetDocument() != nullptr)
            ++loadedDocuments;
    }

    const uint64_t signature =
        (static_cast<uint64_t>(canvasCount) << 32u) ^
        (static_cast<uint64_t>(loadedDocuments) << 16u) ^
        static_cast<uint64_t>(hasViewBounds ? 1u : 0u) ^
        (static_cast<uint64_t>(appendedDrawLists) << 8u) ^
        (static_cast<uint64_t>(appendedVertices) << 24u);
    if (signature == m_lastDiagnosticSignature)
        return;
    m_lastDiagnosticSignature = signature;

    NLS_LOG_INFO(
        "[UIOverlay] canvases=" + std::to_string(canvasCount) +
        " loadedDocuments=" + std::to_string(loadedDocuments) +
        " viewBounds=" + std::string(hasViewBounds ? "yes" : "no") +
        " fontAtlasTexture=" + std::string(
            NLS::Render::UI::HasUiFontAtlasTextureId() ? "yes" : "no") +
        " drawLists=" + std::to_string(appendedDrawLists) +
        " vertices=" + std::to_string(appendedVertices));
}
}
