#include "UI/UIDocumentCanvasRenderer.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include <Guid.h>
#include <Rendering/UI/UIFontTextShaper.h>
#include <Rendering/UI/UiDrawDataSnapshot.h>

namespace NLS::Editor::UIDocumentTools
{
    UIDocumentCanvasRenderer::UIDocumentCanvasRenderer() = default;

    UIDocumentCanvasRenderer::~UIDocumentCanvasRenderer() = default;

    void UIDocumentCanvasRenderer::Invalidate()
    {
        m_instance.reset();
        m_document.reset();
        m_syncedRevision = (std::numeric_limits<uint64_t>::max)();
        m_syncedPath.clear();
        m_referenceSize = { 1920.0f, 1080.0f };
        m_valid = false;
        m_lastBatchCount = 0u;
    }

    bool UIDocumentCanvasRenderer::Sync(const UIDocumentDesignerModel& model)
    {
        if (!model.IsLoaded())
        {
            Invalidate();
            return false;
        }

        const auto pathKey = model.GetPath().generic_string();
        if (m_valid && m_instance != nullptr && m_syncedRevision == model.GetDocumentRevision() &&
            m_syncedPath == pathKey)
        {
            return true;
        }

        m_syncedRevision = model.GetDocumentRevision();
        m_syncedPath = pathKey;
        m_valid = false;
        m_instance.reset();
        m_document.reset();

        NLS::Engine::UI::UIDocument document;
        std::vector<NLS::Engine::UI::UIDocumentDiagnostic> diagnostics;
        if (!model.BuildRuntimeDocument(document, &diagnostics))
            return false;

        m_document = std::make_unique<NLS::Engine::UI::UIDocument>(std::move(document));
        m_instance = m_document->Instantiate();
        if (m_instance == nullptr)
        {
            m_document.reset();
            return false;
        }

        const auto& root = m_document->GetRootDefinition();
        m_referenceSize = {
            root.layout.size.x > 0.0f ? root.layout.size.x : 1920.0f,
            root.layout.size.y > 0.0f ? root.layout.size.y : 1080.0f
        };
        m_instance->UpdateLayout(m_referenceSize);
        m_valid = true;
        return true;
    }

    bool UIDocumentCanvasRenderer::Render(
        const UIDocumentDesignerModel& model,
        ImDrawList& drawList,
        const RenderParameters& parameters)
    {
        m_lastBatchCount = 0u;
        if (!Sync(model) || m_instance == nullptr || m_instance->GetRoot() == nullptr)
            return false;

        // The designer uses the same resolver as Game View; async texture loads
        // and project font faces become visible without rebuilding the tree.
        m_instance->RefreshResources();
        m_instance->UpdateLayout(m_referenceSize);

        const auto viewportWidth = static_cast<uint32_t>(
            std::max(1.0f, std::ceil(m_referenceSize.x)));
        const auto viewportHeight = static_cast<uint32_t>(
            std::max(1.0f, std::ceil(m_referenceSize.y)));
        const auto snapshot = m_instance->BuildDrawSnapshot(
            parameters.frameId,
            viewportWidth,
            viewportHeight,
            1.0f);
        if (snapshot == nullptr)
            return false;

        const auto toScreen = [&parameters](const float x, const float y)
        {
            return ImVec2 {
                parameters.origin.x + x * parameters.scale,
                parameters.origin.y + y * parameters.scale
            };
        };

        const bool hasFontAtlas = NLS::Render::UI::GetUiFontAtlasTextureId() != 0u;
        const ImVec2 whiteUv = ImGui::GetFontTexUvWhitePixel();
        const ImTextureID fontTexture = ImGui::GetIO().Fonts->TexID;

        drawList.PushClipRect(parameters.clipMin, parameters.clipMax, true);
        for (const auto& batch : snapshot->batches)
        {
            if (batch.indexCount == 0u || batch.vertexCount == 0u ||
                batch.firstVertex + batch.vertexCount > snapshot->vertices.size() ||
                batch.firstIndex + batch.indexCount > snapshot->indices.size())
            {
                continue;
            }

            const bool hasClip = batch.clipIndex < snapshot->clips.size();
            if (hasClip)
            {
                const auto& clip = snapshot->clips[batch.clipIndex];
                drawList.PushClipRect(
                    toScreen(clip.left, clip.top),
                    toScreen(clip.right, clip.bottom),
                    true);
            }

            const bool textured = batch.textureId != 0u;
            if (textured)
            {
                const auto packed = NLS::Render::UI::PackUiTextureIdForImGui({
                    batch.textureId,
                    batch.textureGeneration == 0u ? 1u : batch.textureGeneration });
                drawList.PushTextureID(
                    reinterpret_cast<ImTextureID>(static_cast<uintptr_t>(packed)));
            }
            else
            {
                // Solid-color quads only need a white texel; the fonts atlas is
                // always resident and its white pixel is a stable ImGui UV.
                drawList.PushTextureID(fontTexture);
            }

            drawList.PrimReserve(
                static_cast<int>(batch.indexCount),
                static_cast<int>(batch.vertexCount));
            const unsigned int vertexBase = drawList._VtxCurrentIdx;
            for (uint32_t index = 0u; index < batch.vertexCount; ++index)
            {
                const auto& vertex = snapshot->vertices[batch.firstVertex + index];
                const ImVec2 position = toScreen(vertex.position[0], vertex.position[1]);
                const ImVec2 uv = textured ? ImVec2(vertex.uv[0], vertex.uv[1]) : whiteUv;
                drawList.PrimWriteVtx(
                    position,
                    uv,
                    ImGui::ColorConvertFloat4ToU32(ImVec4(
                        vertex.color[0],
                        vertex.color[1],
                        vertex.color[2],
                        vertex.color[3])));
            }
            for (uint32_t index = 0u; index < batch.indexCount; ++index)
            {
                const auto relative = snapshot->indices[batch.firstIndex + index];
                drawList.PrimWriteIdx(static_cast<ImDrawIdx>(vertexBase + relative));
            }

            drawList.PopTextureID();
            if (hasClip)
                drawList.PopClipRect();
        }
        drawList.PopClipRect();

        // The runtime only emits glyph quads once its atlas texture is
        // registered. Until then, keep text visible with the ImGui font so a
        // freshly opened document does not look empty.
        if (!hasFontAtlas)
        {
            for (const auto& run : snapshot->textRuns)
            {
                if (run.text.empty())
                    continue;
                const ImVec2 position = toScreen(run.x, run.y);
                const float fontSize = std::max(1.0f, run.fontSize * parameters.scale);
                drawList.AddText(
                    ImGui::GetFont(),
                    fontSize,
                    position,
                    ImGui::ColorConvertFloat4ToU32(ImVec4(
                        run.color[0],
                        run.color[1],
                        run.color[2],
                        run.color[3])),
                    run.text.c_str());
            }
        }

        m_lastBatchCount = static_cast<uint32_t>(snapshot->batches.size());
        return true;
    }

    bool UIDocumentCanvasRenderer::GetElementRect(
        const std::string_view nodeGuid,
        NLS::Engine::UI::UIRect& rect) const
    {
        if (m_instance == nullptr || nodeGuid.empty())
            return false;
        const auto parsed = NLS::Guid::TryParse(std::string(nodeGuid));
        if (!parsed.has_value())
            return false;
        const auto* element = m_instance->GetElement(*parsed);
        if (element == nullptr)
            return false;
        rect = element->GetLayoutRect();
        return true;
    }

    bool UIDocumentCanvasRenderer::GetElementContentRect(
        const std::string_view nodeGuid,
        NLS::Engine::UI::UIRect& rect) const
    {
        if (m_instance == nullptr || nodeGuid.empty())
            return false;
        const auto parsed = NLS::Guid::TryParse(std::string(nodeGuid));
        if (!parsed.has_value())
            return false;
        const auto* element = m_instance->GetElement(*parsed);
        if (element == nullptr)
            return false;
        rect = element->GetLayoutRect();
        const auto padding = element->GetLayout().padding;
        rect = rect.Inset(padding.x, padding.y, padding.x, padding.y);
        return true;
    }

    const NLS::Engine::UI::UIElement* UIDocumentCanvasRenderer::FindElement(
        const std::string_view nodeGuid) const
    {
        if (m_instance == nullptr || nodeGuid.empty())
            return nullptr;
        const auto parsed = NLS::Guid::TryParse(std::string(nodeGuid));
        return parsed.has_value() ? m_instance->GetElement(*parsed) : nullptr;
    }

    const NLS::Engine::UI::UIElement* UIDocumentCanvasRenderer::HitTest(
        const NLS::Engine::UI::UIVector2 point) const
    {
        if (m_instance == nullptr || m_instance->GetRoot() == nullptr)
            return nullptr;
        return HitTestRecursive(*m_instance->GetRoot(), point);
    }

    const NLS::Engine::UI::UIElement* UIDocumentCanvasRenderer::HitTestRecursive(
        const NLS::Engine::UI::UIElement& element,
        const NLS::Engine::UI::UIVector2 point)
    {
        if (!element.GetStyle().visible)
            return nullptr;
        const auto& children = element.GetChildren();
        for (auto iterator = children.rbegin(); iterator != children.rend(); ++iterator)
        {
            if (*iterator != nullptr)
            {
                if (const auto* hit = HitTestRecursive(**iterator, point))
                    return hit;
            }
        }
        return element.GetLayoutRect().Contains(point) ? &element : nullptr;
    }
}
