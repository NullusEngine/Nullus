#pragma once

#include <Engine/UI/UIRuntimeService.h>
#include <Rendering/UI/UIFontAtlasUploader.h>
#include <Rendering/UI/UiDrawDataSnapshot.h>

#include <cstdint>
#include <string>

namespace NLS::Render::Context
{
class Driver;
}

// Deliberately not NLS::Editor::UI: that namespace would shadow the unqualified
// UI:: lookups used across the editor headers.
namespace NLS::Editor::UiOverlay
{
// Presents the retained in-game UI of the edited scene inside the Game View.
//
// The editor renders its own ImGui interface through the swapchain UI overlay
// pass. Rather than adding a second rendering path, this presenter adapts the
// runtime draw snapshot, translates it into the game view's screen rect, clips
// it to that rect and appends it to the published overlay snapshot so it draws
// above the ImGui frame.
class EditorUiOverlayPresenter final
{
public:
    // Uploads engine font atlas pages when they changed. Called once per editor
    // frame by the editor update, never from the ImGui publish path.
    void PumpFontAtlas(NLS::Render::Context::Driver& driver);

    // Appends the current scene's screen-space UI into the overlay snapshot.
    void AppendToOverlaySnapshot(NLS::Render::UI::UiDrawDataSnapshot& snapshot);

private:
    void SyncSceneCanvases();
    void ResolvePendingDocuments();
    // Document problems are warnings; a closed or not-yet-rendered Game View is
    // a legitimate state and is reported at info level. Each category keeps its
    // own last message so one cannot reset the other's one-shot state.
    void ReportDocumentWarning(const std::string& message);
    void ReportPresentationNotice(const std::string& message);
    // Logs the font identity of the shaped text once per document state. Written
    // as a diagnostic because a run that does not match the prewarmed atlas is
    // the difference between readable text and glyphs that sample empty cells.
    void ReportTextRunFonts(const NLS::Render::UI::UIDrawSnapshot& snapshot);
    // Verifies that every shaped codepoint has ink in the atlas image that gets
    // uploaded. An empty cell explains glyphs that disappear on the GPU.
    void AuditTextRunAtlasInk(const NLS::Render::UI::UIDrawSnapshot& snapshot);
    // Authoritative check: samples the coverage of the image the GPU texture was
    // actually built from, at the exact UVs the emitted quads use.
    void AuditGlyphQuadCoverage(const NLS::Render::UI::UIDrawSnapshot& snapshot);
    void InvalidateCanvasPaintCaches();
    void LogDiagnostics(
        size_t canvasCount,
        size_t documentCount,
        bool hasViewBounds,
        size_t appendedDrawLists,
        size_t appendedVertices);

    NLS::Engine::UI::UIRuntimeService m_uiRuntimeService;
    NLS::Render::UI::UIFontAtlasUploader m_fontAtlasUploader;
    uint64_t m_frameId = 0u;
    // Atlas content generation the cached document geometry was built against.
    uint64_t m_textAtlasGeneration = 0u;
    uint64_t m_textTextureId = 0u;
    uint32_t m_textTextureGeneration = 0u;
    std::string m_lastTextRunFonts;
    std::string m_lastTextInkAudit;
    size_t m_lastQuadCoverageQuads = 0u;
    // Bounded self-heal: a texture proven not to match the sampled geometry is
    // re-uploaded a few times, then the diagnostic is left to report the state.
    uint32_t m_quadCoverageRetries = 0u;
    uint32_t m_atlasStaleFrames = 0u;
    // One-shot state so a stalled presentation is diagnosable from the editor log
    // without spamming every frame.
    uint64_t m_lastDiagnosticSignature = 0u;
    std::string m_lastDocumentWarning;
    std::string m_lastPresentationNotice;
};
}
