#include "Rendering/UI/UIDrawSnapshotAdapter.h"

#include <algorithm>
#include <iterator>

namespace NLS::Render::UI
{
    namespace
    {
        UiTextureId AdaptTextureId(const uint64_t textureId)
        {
            if (textureId == 0u)
                return {};

            // UIDrawSnapshot uses a backend-neutral identity. The legacy
            // overlay registry additionally requires a non-zero generation;
            // generation one denotes a static/current retained-mode resource.
            return { textureId, 1u };
        }

        void CopyTextRuns(
            const UIDrawSnapshot& source,
            UiDrawDataSnapshot& target)
        {
            target.textRuns.reserve(source.textRuns.size());
            for (const auto& sourceRun : source.textRuns)
            {
                UiTextRunSnapshot targetRun;
                targetRun.text = sourceRun.text;
                targetRun.x = sourceRun.x;
                targetRun.y = sourceRun.y;
                targetRun.width = sourceRun.width;
                targetRun.height = sourceRun.height;
                targetRun.fontSize = sourceRun.fontSize;
                std::copy(
                    sourceRun.color.begin(),
                    sourceRun.color.end(),
                    std::begin(targetRun.color));
                targetRun.clipIndex = sourceRun.clipIndex;
                targetRun.fontAssetId = sourceRun.fontAssetId;
                targetRun.sortKey = sourceRun.sortKey;
                targetRun.fontPixelSize = sourceRun.fontPixelSize;
                target.textRuns.push_back(std::move(targetRun));
            }
            target.containsDeferredText = !target.textRuns.empty();
        }
    }

    std::shared_ptr<UiDrawDataSnapshot> AdaptUIDrawSnapshot(
        const UIDrawSnapshot& source)
    {
        auto target = std::make_shared<UiDrawDataSnapshot>();
        target->frameId = source.frameId;
        target->displayPos[0] = 0.0f;
        target->displayPos[1] = 0.0f;
        target->displaySize[0] = static_cast<float>(source.viewportWidth);
        target->displaySize[1] = static_cast<float>(source.viewportHeight);
        target->framebufferScale[0] = source.dpiScale > 0.0f ? source.dpiScale : 1.0f;
        target->framebufferScale[1] = target->framebufferScale[0];
        target->totalVertexCount = static_cast<uint32_t>(source.vertices.size());
        target->totalIndexCount = static_cast<uint32_t>(source.indices.size());
        target->drawListCount = source.vertices.empty() && source.indices.empty() ? 0u : 1u;
        target->copyDiagnostics.copiedVertexBytes =
            static_cast<uint64_t>(source.vertices.size() * sizeof(UiDrawVertex));
        target->copyDiagnostics.copiedIndexBytes =
            static_cast<uint64_t>(source.indices.size() * sizeof(uint32_t));
        target->copyDiagnostics.copiedCommandCount =
            static_cast<uint32_t>(source.batches.size());

        if (target->drawListCount != 0u)
        {
            UiDrawListSnapshot drawList;
            drawList.vertices.reserve(source.vertices.size());
            for (const auto& sourceVertex : source.vertices)
            {
                UiDrawVertex targetVertex;
                targetVertex.position[0] = sourceVertex.position[0];
                targetVertex.position[1] = sourceVertex.position[1];
                targetVertex.uv[0] = sourceVertex.uv[0];
                targetVertex.uv[1] = sourceVertex.uv[1];
                std::copy(
                    std::begin(sourceVertex.color),
                    std::end(sourceVertex.color),
                    std::begin(targetVertex.color));
                drawList.vertices.push_back(targetVertex);
            }
            drawList.indices = source.indices;
            drawList.commands.reserve(source.batches.size());
            for (const auto& sourceBatch : source.batches)
            {
                UiDrawCommandSnapshot command;
                command.elementCount = sourceBatch.indexCount;
                command.indexOffset = sourceBatch.firstIndex;
                command.vertexOffset = sourceBatch.firstVertex;
                if (sourceBatch.clipIndex < source.clips.size())
                {
                    const auto& clip = source.clips[sourceBatch.clipIndex];
                    command.clipRect[0] = clip.left;
                    command.clipRect[1] = clip.top;
                    command.clipRect[2] = clip.right;
                    command.clipRect[3] = clip.bottom;
                }
                else
                {
                    command.clipRect[0] = 0.0f;
                    command.clipRect[1] = 0.0f;
                    command.clipRect[2] = target->displaySize[0];
                    command.clipRect[3] = target->displaySize[1];
                }
                command.textureId = sourceBatch.textureId == 0u
                    ? UiTextureId {}
                    : UiTextureId {
                        sourceBatch.textureId,
                        sourceBatch.textureGeneration == 0u ? 1u : sourceBatch.textureGeneration };
                drawList.commands.push_back(command);
            }
            target->drawLists.push_back(std::move(drawList));
        }

        target->hasVisibleDraws = std::any_of(
            source.batches.begin(),
            source.batches.end(),
            [](const UIDrawBatch& batch) { return batch.indexCount != 0u; });
        CopyTextRuns(source, *target);
        target->contentSignature = ComputeUiDrawDataContentSignature(*target);
        return target;
    }

    std::shared_ptr<const UiDrawDataSnapshot> AdaptUIDrawSnapshot(
        const std::shared_ptr<const UIDrawSnapshot>& source)
    {
        if (!source)
            return nullptr;
        return AdaptUIDrawSnapshot(*source);
    }
}
