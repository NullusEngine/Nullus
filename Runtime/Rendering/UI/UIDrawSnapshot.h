#pragma once

#include "RenderDef.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace NLS::Render::UI
{
    // A batch/text run with this value does not use a clip rectangle.  Zero is
    // a valid clip index, so keeping a dedicated sentinel is required when
    // snapshots from multiple canvases are merged.
    inline constexpr uint32_t UIClipIndexNone = (std::numeric_limits<uint32_t>::max)();

    struct UIDrawVertex
    {
        float position[2] {};
        float uv[2] {};
        float color[4] { 1.0f, 1.0f, 1.0f, 1.0f };
    };

    struct UIClipRect
    {
        float left = 0.0f;
        float top = 0.0f;
        float right = 0.0f;
        float bottom = 0.0f;
    };

    struct UIDrawBatch
    {
        uint32_t firstVertex = 0u;
        uint32_t vertexCount = 0u;
        uint32_t firstIndex = 0u;
        uint32_t indexCount = 0u;
        uint32_t clipIndex = UIClipIndexNone;
        uint64_t textureId = 0u;
        uint64_t sortKey = 0u;
        uint32_t textureGeneration = 0u;
    };

    struct UIDrawSnapshotDiagnostics
    {
        uint32_t nodeCount = 0u;
        uint32_t visibleNodeCount = 0u;
        uint32_t batchCount = 0u;
        uint32_t clipCount = 0u;
        uint64_t uploadedVertexBytes = 0u;
        uint64_t uploadedIndexBytes = 0u;
        uint32_t textRunCount = 0u;
    };

    struct UIDrawTextRun
    {
        std::string text;
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        float fontSize = 16.0f;
        std::array<float, 4> color { 1.0f, 1.0f, 1.0f, 1.0f };
        uint32_t clipIndex = 0u;
        uint64_t fontAssetId = 0u;
        uint64_t sortKey = 0u;
        uint16_t fontPixelSize = 16u;
    };

    // Render-thread input. The producer owns a complete snapshot for a frame;
    // consumers never observe the mutable UI tree.
    struct UIDrawSnapshot
    {
        uint64_t frameId = 0u;
        uint32_t viewportWidth = 0u;
        uint32_t viewportHeight = 0u;
        float dpiScale = 1.0f;
        std::array<float, 16> canvasMatrix {
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f };
        std::vector<UIDrawVertex> vertices;
        std::vector<uint32_t> indices;
        std::vector<UIClipRect> clips;
        std::vector<UIDrawBatch> batches;
        std::vector<UIDrawTextRun> textRuns;
        UIDrawSnapshotDiagnostics diagnostics;

        void ResetGeometry()
        {
            vertices.clear();
            indices.clear();
            clips.clear();
            batches.clear();
            textRuns.clear();
            diagnostics = {};
        }

        uint32_t AddClip(const UIClipRect& clip)
        {
            if (!clips.empty())
            {
                const auto& previous = clips.back();
                if (previous.left == clip.left && previous.top == clip.top &&
                    previous.right == clip.right && previous.bottom == clip.bottom)
                    return static_cast<uint32_t>(clips.size() - 1u);
            }
            clips.push_back(clip);
            diagnostics.clipCount = static_cast<uint32_t>(clips.size());
            return static_cast<uint32_t>(clips.size() - 1u);
        }

        void AddQuad(
            float left,
            float top,
            float right,
            float bottom,
            const std::array<float, 4>& color,
            uint32_t clipIndex = UIClipIndexNone,
            uint64_t textureId = 0u,
            uint64_t sortKey = 0u,
            uint32_t textureGeneration = 0u)
        {
            AddTexturedQuad(
                left,
                top,
                right,
                bottom,
                { 0.0f, 0.0f },
                { 1.0f, 1.0f },
                color,
                clipIndex,
                textureId,
                sortKey,
                textureGeneration);
        }

        // Textured variant. Glyphs supply atlas sub-rect UVs, so a plain 0..1
        // mapping is not enough.
        void AddTexturedQuad(
            float left,
            float top,
            float right,
            float bottom,
            const std::array<float, 2>& topLeftUv,
            const std::array<float, 2>& bottomRightUv,
            const std::array<float, 4>& color,
            uint32_t clipIndex = UIClipIndexNone,
            uint64_t textureId = 0u,
            uint64_t sortKey = 0u,
            uint32_t textureGeneration = 0u)
        {
            const auto firstVertex = static_cast<uint32_t>(vertices.size());
            const auto firstIndex = static_cast<uint32_t>(indices.size());
            const UIDrawVertex quadVertices[] = {
                { { left, top }, { topLeftUv[0], topLeftUv[1] }, { color[0], color[1], color[2], color[3] } },
                { { right, top }, { bottomRightUv[0], topLeftUv[1] }, { color[0], color[1], color[2], color[3] } },
                { { right, bottom }, { bottomRightUv[0], bottomRightUv[1] }, { color[0], color[1], color[2], color[3] } },
                { { left, bottom }, { topLeftUv[0], bottomRightUv[1] }, { color[0], color[1], color[2], color[3] } }
            };
            vertices.insert(vertices.end(), std::begin(quadVertices), std::end(quadVertices));
            // Indices address vertices relative to this batch's base. Consumers
            // bind the batch base as the draw call's vertex offset, so baking
            // firstVertex into the indices as well shifted every quad by its own
            // offset: quad N sampled quad 2N, which made alternating glyphs
            // ("Score: 0" -> "Soe0") disappear.
            indices.insert(indices.end(), {
                0u, 1u, 2u,
                0u, 2u, 3u });
            batches.push_back({
                firstVertex, 4u, firstIndex, 6u, clipIndex, textureId,
                sortKey,
                textureId == 0u ? 0u : (textureGeneration == 0u ? 1u : textureGeneration) });
            diagnostics.batchCount = static_cast<uint32_t>(batches.size());
            diagnostics.uploadedVertexBytes =
                static_cast<uint64_t>(vertices.size() * sizeof(UIDrawVertex));
            diagnostics.uploadedIndexBytes =
                static_cast<uint64_t>(indices.size() * sizeof(uint32_t));
        }
    };

    using UIDrawSnapshotPtr = std::shared_ptr<const UIDrawSnapshot>;

    // Small render hand-off used by the game and editor bridges. Backends can
    // consume Latest() from their render thread without touching the UI tree.
    class RHIUIRenderer
    {
    public:
        void Submit(std::shared_ptr<const UIDrawSnapshot> snapshot)
        {
            std::lock_guard lock(m_mutex);
            m_latest = std::move(snapshot);
        }

        [[nodiscard]] UIDrawSnapshotPtr Latest() const
        {
            std::lock_guard lock(m_mutex);
            return m_latest;
        }

        void Clear()
        {
            std::lock_guard lock(m_mutex);
            m_latest.reset();
        }

    private:
        mutable std::mutex m_mutex;
        UIDrawSnapshotPtr m_latest;
    };
}
