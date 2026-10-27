#pragma once

#include "Rendering/Context/RhiCommandList.h"
#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/RHI/Utils/ResourceStateTracker/ResourceStateTracker.h"

namespace NLS::Render::Context
{
    // Records owned command arguments and plans barriers on the ordered producer.
    // The native command buffer is only used for backend state planning here.
    class NLS_RENDER_API RhiCommandListRecorder final : public RHI::RHICommandBuffer
    {
    public:
        using QueueReleaseFunction = std::function<RHI::RHICommandRecordingResult(const RHI::RHIBarrierDesc&)>;
        using PreviousStateResolver = std::function<void(RHI::RHIBarrierDesc&)>;

        RhiCommandListRecorder(
            RhiCommandList& list,
            RHI::RHICommandBuffer& nativeBuffer,
            std::shared_ptr<RHI::ResourceStateTracker> tracker,
            QueueReleaseFunction queueRelease = {},
            PreviousStateResolver previousStates = {});

        std::string_view GetDebugName() const override;
        void Begin() override;
        void End() override;
        void Reset() override;
        bool IsRecording() const override;
        RHI::NativeHandle GetNativeCommandBuffer() const override;
        void BeginGpuProfileScope(std::string_view name, std::string_view sourceFunction) override;
        void EndGpuProfileScope() override;
        void BeginRenderPass(const RHI::RHIRenderPassDesc& desc) override;
        void EndRenderPass() override;
        void SetViewport(const RHI::RHIViewport& viewport) override;
        void SetScissor(const RHI::RHIRect2D& rect) override;
        void BindGraphicsPipeline(const std::shared_ptr<RHI::RHIGraphicsPipeline>& pipeline) override;
        void BindComputePipeline(const std::shared_ptr<RHI::RHIComputePipeline>& pipeline) override;
        void BindBindingSet(uint32_t index, const std::shared_ptr<RHI::RHIBindingSet>& bindingSet) override;
        void PushConstants(RHI::ShaderStageMask stages, uint32_t offset, uint32_t size, const void* data) override;
        void BindVertexBuffer(uint32_t slot, const RHI::RHIVertexBufferView& view) override;
        void BindIndexBuffer(const RHI::RHIIndexBufferView& view) override;
        void Draw(uint32_t vertices, uint32_t instances, uint32_t firstVertex, uint32_t firstInstance) override;
        void DrawIndexed(uint32_t indices, uint32_t instances, uint32_t firstIndex,
            int32_t vertexOffset, uint32_t firstInstance) override;
        void Dispatch(uint32_t x, uint32_t y, uint32_t z) override;
        RHI::RHICommandRecordingResult DrawIndirect(const std::shared_ptr<RHI::RHIBuffer>& arguments, uint64_t offset = 0) override;
        RHI::RHICommandRecordingResult DrawIndexedIndirect(const std::shared_ptr<RHI::RHIBuffer>& arguments, uint64_t offset = 0) override;
        RHI::RHICommandRecordingResult DispatchIndirect(const std::shared_ptr<RHI::RHIBuffer>& arguments, uint64_t offset = 0) override;
        RHI::RHICommandRecordingResult WriteTimestamp(const std::shared_ptr<RHI::RHITimestampQuery>& query, uint32_t index) override;
        void CopyBuffer(const std::shared_ptr<RHI::RHIBuffer>& source,
            const std::shared_ptr<RHI::RHIBuffer>& destination, const RHI::RHIBufferCopyRegion& region) override;
        void CopyBufferToTexture(const RHI::RHIBufferToTextureCopyDesc& desc) override;
        void CopyTexture(const RHI::RHITextureCopyDesc& desc) override;
        RHI::RHIBarrierDesc FilterBarrierDesc(const RHI::RHIBarrierDesc& barrier) const override;
        RHI::RHICommandRecordingResult BarrierChecked(const RHI::RHIBarrierDesc& barrier) override;
        void Barrier(const RHI::RHIBarrierDesc& barrier) override;

    private:
        void Record(std::string name, RhiCommandList::RecordFunction body);
        RhiCommandList& m_list;
        RHI::RHICommandBuffer& m_nativeBuffer;
        std::shared_ptr<RHI::ResourceStateTracker> m_tracker;
        QueueReleaseFunction m_queueRelease;
        PreviousStateResolver m_previousStates;
        bool m_recording = false;
        bool m_canPrependQueueRelease = true;
    };
}
