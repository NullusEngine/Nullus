#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "RenderDef.h"
#include "Rendering/UI/RHIImGuiFontAtlas.h"
#include "Rendering/UI/RHIImGuiTextureRegistry.h"
#include "Rendering/UI/UiDrawDataSnapshot.h"

namespace NLS::Render::RHI
{
    class RHICommandBuffer;
    class RHIBuffer;
    class RHIBindingLayout;
    class RHIDevice;
    class RHIGraphicsPipeline;
    class RHIPipelineLayout;
    class RHIShaderModule;
    class RHITextureView;
}

namespace NLS::Render::UI
{
    struct NLS_RENDER_API RHIImGuiOverlayDynamicBufferTelemetry
    {
        uint32_t allocationCount = 0u;
        uint32_t reallocationCount = 0u;
        uint64_t contentCacheHitCount = 0u;
        uint64_t contentCacheMissCount = 0u;
        uint64_t uploadedVertexBytes = 0u;
        uint64_t uploadedIndexBytes = 0u;
        uint64_t totalCpuCopyTimeNanoseconds = 0u;
    };

    struct NLS_RENDER_API RHIImGuiOverlayRecordingResult
    {
        bool success = false;
        bool recordedDraws = false;
        RHIImGuiOverlayDynamicBufferTelemetry dynamicBufferTelemetry;
        std::string message;
    };

    struct NLS_RENDER_API RHIImGuiOverlayPreparedResourceSnapshot
    {
        std::shared_ptr<RHI::RHIBuffer> vertexBuffer;
        std::shared_ptr<RHI::RHIBuffer> indexBuffer;
        std::shared_ptr<RHI::RHITextureView> fontAtlasTextureView;
        bool fontAtlasUploadTransitionRequired = false;
        std::vector<std::shared_ptr<RHI::RHITextureView>> registeredTextureViews;
    };

    class NLS_RENDER_API RHIImGuiOverlayRenderer
    {
    public:
        explicit RHIImGuiOverlayRenderer(RHIImGuiTextureRegistry* textureRegistry = nullptr);

        RHIImGuiOverlayRecordingResult PrepareFrameResources(
            RHI::RHIDevice& device,
            RHI::RHICommandBuffer& commandBuffer,
            const UiDrawDataSnapshot& snapshot,
            size_t frameResourceSlot);
        RHIImGuiOverlayRecordingResult RecordPrepared(
            RHI::RHICommandBuffer& commandBuffer,
            const UiDrawDataSnapshot& snapshot,
            size_t frameResourceSlot);
        [[nodiscard]] RHIImGuiOverlayPreparedResourceSnapshot GetPreparedResourceSnapshot(
            const UiDrawDataSnapshot& snapshot,
            size_t frameResourceSlot) const;

#if defined(NLS_ENABLE_TEST_HOOKS)
        [[nodiscard]] RHIImGuiFontAtlas& FontAtlas() { return m_fontAtlas; }
#endif
        [[nodiscard]] RHIImGuiTextureRegistry* TextureRegistry() const { return m_textureRegistry; }
        [[nodiscard]] RHIImGuiOverlayDynamicBufferTelemetry GetDynamicBufferTelemetry() const;
        void SetTextureRegistry(RHIImGuiTextureRegistry* textureRegistry);
        void InvalidateFontAtlas(uint64_t retireFrameId);
        void ReleaseRetiredResourcesUpTo(uint64_t completedFrameId);

    private:
        RHIImGuiOverlayRecordingResult RecordInternal(
            RHI::RHICommandBuffer& commandBuffer,
            const UiDrawDataSnapshot& snapshot,
            size_t frameResourceSlot = 0u,
            bool bindPreparedDynamicBuffers = false);
        bool BindPreparedDynamicBuffers(
            size_t frameResourceSlot,
            const UiDrawDataSnapshot& snapshot,
            RHI::RHICommandBuffer& commandBuffer,
            std::string& errorMessage);
        bool EnsureDynamicBuffers(
            size_t frameResourceSlot,
            RHI::RHIDevice& device,
            size_t vertexBytes,
            size_t indexBytes,
            std::string& errorMessage);
        bool EnsureGraphicsPipeline(RHI::RHIDevice& device, std::string& errorMessage);
        bool EnsureRegisteredTextureBindingSets(
            RHI::RHIDevice& device,
            const UiDrawDataSnapshot& snapshot,
            std::string& errorMessage);
        bool PrepareDrawCommands(
            const UiDrawDataSnapshot& snapshot,
            size_t frameResourceSlot,
            uint64_t contentSignature,
            std::string& errorMessage);
        bool UploadDynamicBuffers(
            size_t frameResourceSlot,
            const UiDrawDataSnapshot& snapshot,
            std::string& errorMessage);
        void RecordDynamicBufferUploadVisibilityBarrier(
            size_t frameResourceSlot,
            RHI::RHICommandBuffer& commandBuffer);

        RHIImGuiFontAtlas m_fontAtlas;
        RHIImGuiTextureRegistry* m_textureRegistry = nullptr;
        uint64_t m_pipelineDeviceCacheIdentity = 0u;
        std::shared_ptr<RHI::RHIBindingLayout> m_fontAtlasBindingLayout;
        std::shared_ptr<RHI::RHIPipelineLayout> m_pipelineLayout;
        std::shared_ptr<RHI::RHIShaderModule> m_vertexShaderModule;
        std::shared_ptr<RHI::RHIShaderModule> m_fragmentShaderModule;
        std::shared_ptr<RHI::RHIGraphicsPipeline> m_graphicsPipeline;
        struct DynamicBufferSet
        {
            struct PreparedDrawCommand
            {
                uint32_t elementCount = 0u;
                uint32_t firstIndex = 0u;
                int32_t vertexOffset = 0;
                int32_t scissorX = 0;
                int32_t scissorY = 0;
                uint32_t scissorWidth = 0u;
                uint32_t scissorHeight = 0u;
                UiTextureId textureId {};
                std::shared_ptr<RHI::RHIBindingSet> bindingSet;
            };

            std::shared_ptr<RHI::RHIBuffer> vertexBuffer;
            std::shared_ptr<RHI::RHIBuffer> indexBuffer;
            size_t vertexBufferCapacityBytes = 0u;
            size_t indexBufferCapacityBytes = 0u;
            uint64_t preparedFrameId = 0u;
            uint64_t preparedContentSignature = 0u;
            uint64_t deviceCacheIdentity = 0u;
            uint32_t preparedVertexCount = 0u;
            uint32_t preparedIndexCount = 0u;
            const UiDrawDataSnapshot* preparedSnapshotAddress = nullptr;
            bool hasPreparedSnapshot = false;
            bool fontAtlasUploadTransitionRequired = false;
            std::shared_ptr<RHI::RHITextureView> preparedFontAtlasTextureView;
            std::shared_ptr<RHI::RHIBindingSet> preparedFontAtlasBindingSet;
            std::vector<std::shared_ptr<RHI::RHITextureView>> preparedRegisteredTextureViews;
            std::vector<PreparedDrawCommand> preparedDrawCommands;
        };
        mutable std::mutex m_mutex;
        std::unordered_map<size_t, DynamicBufferSet> m_dynamicBuffersByFrameSlot;
        RHIImGuiOverlayDynamicBufferTelemetry m_dynamicBufferTelemetry;
        // UIOverlay passes are deliberately serial today; these scratch buffers are reused across prepares.
        std::vector<UiDrawVertex> m_cpuVertices;
        std::vector<uint32_t> m_cpuIndices;
    };
}
