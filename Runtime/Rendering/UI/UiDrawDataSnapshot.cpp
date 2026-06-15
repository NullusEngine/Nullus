#include "Rendering/UI/UiDrawDataSnapshot.h"

#include <cstdint>
#include <chrono>
#include <string>

#include "ImGui/imgui.h"
#include "Debug/Logger.h"

namespace NLS::Render::UI
{
    namespace
    {
        constexpr uint64_t kUiTextureIdImGuiMarkerMask = 0xffff000000000000ull;
        constexpr uint64_t kUiTextureIdImGuiMarker = 0x4e55000000000000ull;
        constexpr uint64_t kInvalidUiTextureIdImGuiMarker = 0x4e5f000000000000ull;
        constexpr uint64_t kUiTextureIdValueMask = 0xffffffffull;
        constexpr uint64_t kUiTextureIdGenerationMask = 0xffffull;

        std::optional<UiTextureId> ToUiTextureId(ImTextureID textureId)
        {
            const auto value = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(textureId));
            return UnpackUiTextureIdFromImGui(value);
        }

        void ExpandImGuiColor(const uint32_t packedColor, float (&outColor)[4])
        {
            outColor[0] = static_cast<float>(packedColor & 0xffu) / 255.0f;
            outColor[1] = static_cast<float>((packedColor >> 8u) & 0xffu) / 255.0f;
            outColor[2] = static_cast<float>((packedColor >> 16u) & 0xffu) / 255.0f;
            outColor[3] = static_cast<float>((packedColor >> 24u) & 0xffu) / 255.0f;
        }
    }

    uint64_t PackUiTextureIdForImGui(const UiTextureId id)
    {
        if (id.IsFontAtlas())
            return 0u;
        if (id.generation == 0u ||
            id.value == 0u ||
            id.value > kUiTextureIdValueMask ||
            id.generation > kUiTextureIdGenerationMask)
        {
            return kInvalidUiTextureIdImGuiMarker;
        }

        return kUiTextureIdImGuiMarker |
            (static_cast<uint64_t>(id.generation) << 32u) |
            (id.value & kUiTextureIdValueMask);
    }

    std::optional<UiTextureId> UnpackUiTextureIdFromImGui(const uint64_t encodedTextureId)
    {
        if (encodedTextureId == 0u)
            return UiTextureId {};
        if ((encodedTextureId & kUiTextureIdImGuiMarkerMask) != kUiTextureIdImGuiMarker)
            return std::nullopt;

        const auto value = encodedTextureId & kUiTextureIdValueMask;
        const auto generation = static_cast<uint32_t>(
            (encodedTextureId >> 32u) & kUiTextureIdGenerationMask);
        if (value == 0u || generation == 0u)
            return std::nullopt;

        return UiTextureId { value, generation };
    }

    std::shared_ptr<UiDrawDataSnapshot> CaptureUiDrawDataSnapshot(
        const ImDrawData* drawData,
        const uint64_t frameId)
    {
        const auto copyStart = std::chrono::steady_clock::now();
        auto snapshot = std::make_shared<UiDrawDataSnapshot>();
        snapshot->frameId = frameId;
        if (drawData == nullptr)
        {
            snapshot->copyDiagnostics.cpuTimeNanoseconds = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - copyStart).count());
            return snapshot;
        }

        snapshot->displayPos[0] = drawData->DisplayPos.x;
        snapshot->displayPos[1] = drawData->DisplayPos.y;
        snapshot->displaySize[0] = drawData->DisplaySize.x;
        snapshot->displaySize[1] = drawData->DisplaySize.y;
        snapshot->framebufferScale[0] = drawData->FramebufferScale.x;
        snapshot->framebufferScale[1] = drawData->FramebufferScale.y;
        snapshot->totalVertexCount = static_cast<uint32_t>(drawData->TotalVtxCount);
        snapshot->totalIndexCount = static_cast<uint32_t>(drawData->TotalIdxCount);
        snapshot->drawListCount = static_cast<uint32_t>(drawData->CmdListsCount);
        snapshot->drawLists.reserve(static_cast<size_t>(drawData->CmdListsCount));

        const bool framebufferIsRecordable =
            snapshot->displaySize[0] > 0.0f &&
            snapshot->displaySize[1] > 0.0f &&
            snapshot->framebufferScale[0] > 0.0f &&
            snapshot->framebufferScale[1] > 0.0f;

        for (int listIndex = 0; listIndex < drawData->CmdListsCount; ++listIndex)
        {
            const ImDrawList* sourceList = drawData->CmdLists[listIndex];
            if (sourceList == nullptr)
                continue;

            UiDrawListSnapshot copiedList;
            copiedList.vertices.reserve(static_cast<size_t>(sourceList->VtxBuffer.Size));
            for (int vertexIndex = 0; vertexIndex < sourceList->VtxBuffer.Size; ++vertexIndex)
            {
                const ImDrawVert& sourceVertex = sourceList->VtxBuffer[vertexIndex];
                UiDrawVertex copiedVertex;
                copiedVertex.position[0] = sourceVertex.pos.x;
                copiedVertex.position[1] = sourceVertex.pos.y;
                copiedVertex.uv[0] = sourceVertex.uv.x;
                copiedVertex.uv[1] = sourceVertex.uv.y;
                ExpandImGuiColor(sourceVertex.col, copiedVertex.color);
                copiedList.vertices.push_back(copiedVertex);
            }
            snapshot->copyDiagnostics.copiedVertexBytes +=
                static_cast<uint64_t>(copiedList.vertices.size() * sizeof(UiDrawVertex));

            copiedList.indices.reserve(static_cast<size_t>(sourceList->IdxBuffer.Size));
            for (int indexIndex = 0; indexIndex < sourceList->IdxBuffer.Size; ++indexIndex)
                copiedList.indices.push_back(static_cast<uint32_t>(sourceList->IdxBuffer[indexIndex]));
            snapshot->copyDiagnostics.copiedIndexBytes +=
                static_cast<uint64_t>(copiedList.indices.size() * sizeof(uint32_t));

            copiedList.commands.reserve(static_cast<size_t>(sourceList->CmdBuffer.Size));
            for (int commandIndex = 0; commandIndex < sourceList->CmdBuffer.Size; ++commandIndex)
            {
                const ImDrawCmd& sourceCommand = sourceList->CmdBuffer[commandIndex];
                UiDrawCommandSnapshot copiedCommand;
                copiedCommand.elementCount = sourceCommand.ElemCount;
                copiedCommand.indexOffset = sourceCommand.IdxOffset;
                copiedCommand.vertexOffset = sourceCommand.VtxOffset;
                copiedCommand.clipRect[0] = sourceCommand.ClipRect.x;
                copiedCommand.clipRect[1] = sourceCommand.ClipRect.y;
                copiedCommand.clipRect[2] = sourceCommand.ClipRect.z;
                copiedCommand.clipRect[3] = sourceCommand.ClipRect.w;
                const auto textureId = ToUiTextureId(sourceCommand.GetTexID());
                if (textureId.has_value())
                {
                    copiedCommand.textureId = *textureId;
                }
                else
                {
                    copiedCommand.hasUnsupportedTextureId = true;
                    snapshot->containsUnsupportedTextureId = true;
                }

                if (sourceCommand.UserCallback == nullptr)
                {
                    copiedCommand.callbackKind = UiDrawCallbackKind::None;
                }
                else if (sourceCommand.UserCallback == ImDrawCallback_ResetRenderState)
                {
                    copiedCommand.callbackKind = UiDrawCallbackKind::ResetRenderState;
                }
                else
                {
                    copiedCommand.callbackKind = UiDrawCallbackKind::Unsupported;
                    snapshot->containsUnsupportedUserCallback = true;
                    NLS_LOG_WARNING(
                        "CaptureUiDrawDataSnapshot: unsupported ImGui user callback captured in frame " +
                        std::to_string(frameId) +
                        " (draw list " + std::to_string(listIndex) +
                        ", command " + std::to_string(commandIndex) + ")");
                }

                if (copiedCommand.elementCount > 0u &&
                    framebufferIsRecordable &&
                    copiedCommand.callbackKind != UiDrawCallbackKind::Unsupported &&
                    !copiedCommand.hasUnsupportedTextureId)
                {
                    snapshot->hasVisibleDraws = true;
                }

                copiedList.commands.push_back(copiedCommand);
                ++snapshot->copyDiagnostics.copiedCommandCount;
            }

            snapshot->drawLists.push_back(std::move(copiedList));
        }

        snapshot->copyDiagnostics.cpuTimeNanoseconds = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - copyStart).count());
        return snapshot;
    }
}
