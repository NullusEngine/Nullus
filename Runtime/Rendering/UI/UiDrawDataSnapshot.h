#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "RenderDef.h"

struct ImDrawData;

namespace NLS::Render::UI
{
    enum class UiDrawCallbackKind : uint8_t
    {
        None = 0,
        ResetRenderState,
        Unsupported
    };

    enum class UiTextureSynchronizationScope : uint8_t
    {
        PreviousFrameOrStatic = 0,
        SameFrameProducer
    };

    struct NLS_RENDER_API UiTextureId
    {
        uint64_t value = 0u;
        uint32_t generation = 0u;

        [[nodiscard]] bool IsFontAtlas() const { return value == 0u && generation == 0u; }
        [[nodiscard]] bool IsValid() const { return value != 0u && generation != 0u; }
    };

    struct NLS_RENDER_API UiDrawVertex
    {
        float position[2] {};
        float uv[2] {};
        float color[4] {};
    };

    struct NLS_RENDER_API UiDrawCommandSnapshot
    {
        uint32_t elementCount = 0u;
        uint32_t indexOffset = 0u;
        uint32_t vertexOffset = 0u;
        float clipRect[4] {};
        UiTextureId textureId {};
        UiDrawCallbackKind callbackKind = UiDrawCallbackKind::None;
        bool hasUnsupportedTextureId = false;
    };

    struct NLS_RENDER_API UiDrawListSnapshot
    {
        std::vector<UiDrawVertex> vertices;
        std::vector<uint32_t> indices;
        std::vector<UiDrawCommandSnapshot> commands;
    };

    struct NLS_RENDER_API UiDrawDataCopyDiagnostics
    {
        uint64_t cpuTimeNanoseconds = 0u;
        uint64_t copiedVertexBytes = 0u;
        uint64_t copiedIndexBytes = 0u;
        uint32_t copiedCommandCount = 0u;
    };

    struct NLS_RENDER_API UiDrawDataSnapshot
    {
        uint64_t frameId = 0u;
        float displayPos[2] {};
        float displaySize[2] {};
        float framebufferScale[2] { 1.0f, 1.0f };
        uint32_t totalVertexCount = 0u;
        uint32_t totalIndexCount = 0u;
        uint32_t drawListCount = 0u;
        bool hasVisibleDraws = false;
        bool containsUnsupportedUserCallback = false;
        bool containsUnsupportedTextureId = false;
        UiDrawDataCopyDiagnostics copyDiagnostics;
        std::vector<UiDrawListSnapshot> drawLists;
    };

    NLS_RENDER_API uint64_t PackUiTextureIdForImGui(UiTextureId id);
    NLS_RENDER_API std::optional<UiTextureId> UnpackUiTextureIdFromImGui(uint64_t encodedTextureId);

    NLS_RENDER_API std::shared_ptr<UiDrawDataSnapshot> CaptureUiDrawDataSnapshot(
        const ImDrawData* drawData,
        uint64_t frameId);
}
