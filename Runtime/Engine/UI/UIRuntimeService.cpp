#include "UIRuntimeService.h"

#include "Components/UICanvasComponent.h"
#include "Rendering/UI/UIDrawSnapshot.h"

#include <algorithm>

namespace NLS::Engine::UI
{
    void UIRuntimeService::RegisterCanvas(Components::UICanvasComponent& canvas)
    {
        if (std::find(m_canvases.begin(), m_canvases.end(), &canvas) == m_canvases.end())
            m_canvases.push_back(&canvas);
    }

    void UIRuntimeService::UnregisterCanvas(Components::UICanvasComponent& canvas)
    {
        m_canvases.erase(std::remove(m_canvases.begin(), m_canvases.end(), &canvas), m_canvases.end());
    }

    void UIRuntimeService::SyncCanvases(const std::vector<Components::UICanvasComponent*>& canvases)
    {
        m_canvases.clear();
        for (auto* canvas : canvases)
        {
            if (canvas)
                RegisterCanvas(*canvas);
        }
        std::stable_sort(
            m_canvases.begin(), m_canvases.end(),
            [](const auto* lhs, const auto* rhs)
            {
                if (lhs->GetSortLayer() != rhs->GetSortLayer())
                    return lhs->GetSortLayer() < rhs->GetSortLayer();
                if (lhs->GetSortOrder() != rhs->GetSortOrder())
                    return lhs->GetSortOrder() < rhs->GetSortOrder();
                return lhs < rhs;
            });
    }

    void UIRuntimeService::Tick(
        const float deltaSeconds,
        const UIVector2 viewportSize,
        const float dpiScale,
        const uint64_t frameId)
    {
        for (auto* canvas : m_canvases)
        {
            if (canvas)
                canvas->Tick(deltaSeconds, viewportSize, dpiScale, frameId);
        }
        m_latestSnapshot = BuildScreenSnapshot(
            static_cast<uint32_t>(std::max(0.0f, viewportSize.x)),
            static_cast<uint32_t>(std::max(0.0f, viewportSize.y)),
            dpiScale,
            frameId);
    }

    bool UIRuntimeService::DispatchInput(const UIInputEvent& event)
    {
        // Canvases are stored in painter order. Input is offered to the
        // top-most renderable canvas first and stops when it consumes input.
        for (auto iterator = m_canvases.rbegin(); iterator != m_canvases.rend(); ++iterator)
        {
            auto* canvas = *iterator;
            if (canvas == nullptr || !canvas->IsRenderable())
                continue;
            if (canvas->GetInputRouter().Dispatch(event))
                return true;
        }
        return false;
    }

    uint32_t UIRuntimeService::DispatchInputQueue(InputEventQueue& queue)
    {
        uint32_t consumedCount = 0u;
        for (auto event : queue.Drain())
        {
            if (event.consumed || DispatchInput(event))
            {
                event.consumed = true;
                ++consumedCount;
            }
        }
        return consumedCount;
    }

    std::shared_ptr<const NLS::Render::UI::UIDrawSnapshot> UIRuntimeService::BuildScreenSnapshot(
        const uint32_t viewportWidth,
        const uint32_t viewportHeight,
        const float dpiScale,
        const uint64_t frameId) const
    {
        auto result = std::make_shared<NLS::Render::UI::UIDrawSnapshot>();
        result->frameId = frameId;
        result->viewportWidth = viewportWidth;
        result->viewportHeight = viewportHeight;
        result->dpiScale = dpiScale > 0.0f ? dpiScale : 1.0f;
        for (const auto* canvas : m_canvases)
        {
            if (!canvas || !canvas->IsRenderable() || !canvas->GetInstance())
                continue;
            if (canvas->GetRenderMode() == Components::UICanvasComponent::RenderMode::World)
                continue;
            const auto snapshot = canvas->BuildDrawSnapshot(frameId, viewportWidth, viewportHeight, dpiScale);
            if (!snapshot)
                continue;

            const auto vertexOffset = static_cast<uint32_t>(result->vertices.size());
            const auto indexOffset = static_cast<uint32_t>(result->indices.size());
            const auto clipOffset = static_cast<uint32_t>(result->clips.size());
            result->vertices.insert(result->vertices.end(), snapshot->vertices.begin(), snapshot->vertices.end());
            for (const auto index : snapshot->indices)
                result->indices.push_back(index + vertexOffset);
            result->clips.insert(result->clips.end(), snapshot->clips.begin(), snapshot->clips.end());
            result->diagnostics.nodeCount += snapshot->diagnostics.nodeCount;
            result->diagnostics.visibleNodeCount += snapshot->diagnostics.visibleNodeCount;
            result->textRuns.reserve(result->textRuns.size() + snapshot->textRuns.size());
            for (auto textRun : snapshot->textRuns)
            {
                if (textRun.clipIndex < snapshot->clips.size())
                    textRun.clipIndex += clipOffset;
                else
                    textRun.clipIndex = NLS::Render::UI::UIClipIndexNone;
                textRun.sortKey +=
                    (static_cast<uint64_t>(static_cast<uint32_t>(canvas->GetSortLayer())) << 48u) |
                    (static_cast<uint64_t>(static_cast<uint32_t>(canvas->GetSortOrder())) << 32u);
                result->textRuns.push_back(std::move(textRun));
            }
            for (auto batch : snapshot->batches)
            {
                batch.firstVertex += vertexOffset;
                batch.firstIndex += indexOffset;
                if (batch.clipIndex < snapshot->clips.size())
                    batch.clipIndex += clipOffset;
                else
                    batch.clipIndex = NLS::Render::UI::UIClipIndexNone;
                batch.sortKey +=
                    (static_cast<uint64_t>(static_cast<uint32_t>(canvas->GetSortLayer())) << 48u) |
                    (static_cast<uint64_t>(static_cast<uint32_t>(canvas->GetSortOrder())) << 32u);
                result->batches.push_back(batch);
            }
        }
        result->diagnostics.batchCount = static_cast<uint32_t>(result->batches.size());
        result->diagnostics.clipCount = static_cast<uint32_t>(result->clips.size());
        result->diagnostics.uploadedVertexBytes = result->vertices.size() * sizeof(NLS::Render::UI::UIDrawVertex);
        result->diagnostics.uploadedIndexBytes = result->indices.size() * sizeof(uint32_t);
        result->diagnostics.textRunCount = static_cast<uint32_t>(result->textRuns.size());
        return result;
    }

    std::vector<UIWorldDrawSnapshot> UIRuntimeService::BuildWorldSnapshots(
        const uint32_t viewportWidth,
        const uint32_t viewportHeight,
        const float dpiScale,
        const uint64_t frameId) const
    {
        std::vector<UIWorldDrawSnapshot> result;
        for (const auto* canvas : m_canvases)
        {
            if (!canvas || !canvas->IsRenderable() || !canvas->GetInstance() ||
                canvas->GetRenderMode() != Components::UICanvasComponent::RenderMode::World)
                continue;

            UIWorldDrawSnapshot world;
            world.snapshot = canvas->BuildDrawSnapshot(
                frameId, viewportWidth, viewportHeight, dpiScale);
            if (!world.snapshot)
                continue;
            world.sortOrder = canvas->GetSortOrder();
            world.path = canvas->GetResolvedWorldRenderPath() ==
                Components::UICanvasComponent::WorldRenderPath::RenderTarget
                ? UIWorldSnapshotPath::RenderTarget
                : UIWorldSnapshotPath::Direct;
            world.depthTest = canvas->GetDepthTest();
            world.depthWrite = canvas->GetDepthWrite();
            world.alwaysOnTop = canvas->GetAlwaysOnTop();
            world.worldSize = canvas->GetWorldSize();
            world.pixelsPerUnit = canvas->GetPixelsPerUnit();
            result.push_back(std::move(world));
        }
        std::stable_sort(
            result.begin(), result.end(),
            [](const UIWorldDrawSnapshot& lhs, const UIWorldDrawSnapshot& rhs)
            {
                return lhs.sortOrder < rhs.sortOrder;
            });
        return result;
    }
}
