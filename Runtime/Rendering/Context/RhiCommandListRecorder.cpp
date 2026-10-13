#include "Rendering/Context/RhiCommandListRecorder.h"

#include <cstring>
#include <stdexcept>

namespace NLS::Render::Context
{
    namespace
    {
        void RequireSuccess(const RHI::RHICommandRecordingResult& result)
        {
            if (!result.Succeeded())
                throw std::runtime_error(result.message.empty() ? "Native RHI recording failed" : result.message);
        }
    }

    RhiCommandListRecorder::RhiCommandListRecorder(
        RhiCommandList& list, RHI::RHICommandBuffer& nativeBuffer,
        std::shared_ptr<RHI::ResourceStateTracker> tracker, QueueReleaseFunction queueRelease,
        PreviousStateResolver previousStates)
        : m_list(list), m_nativeBuffer(nativeBuffer), m_tracker(std::move(tracker)),
          m_queueRelease(std::move(queueRelease)), m_previousStates(std::move(previousStates))
    {
    }

    std::string_view RhiCommandListRecorder::GetDebugName() const { return m_list.GetDebugName(); }
    void RhiCommandListRecorder::Begin() { m_recording = m_list.IsRecording(); }
    void RhiCommandListRecorder::End() { m_recording = false; m_list.Close(); }
    void RhiCommandListRecorder::Reset() { throw std::logic_error("Deferred RHI lists cannot be reset"); }
    bool RhiCommandListRecorder::IsRecording() const { return m_recording; }
    RHI::NativeHandle RhiCommandListRecorder::GetNativeCommandBuffer() const { return {}; }

    void RhiCommandListRecorder::Record(std::string name, RhiCommandList::RecordFunction body)
    {
        if (!m_recording || !m_list.Record(std::move(name), std::move(body)))
            throw std::logic_error("Cannot record into a closed RHI command list");
    }

    void RhiCommandListRecorder::BeginGpuProfileScope(std::string_view name, std::string_view sourceFunction)
    {
        Record("BeginGpuProfileScope", [name = std::string(name), function = std::string(sourceFunction)](auto& buffer)
        {
            buffer.BeginGpuProfileScope(name, function);
        });
    }
    void RhiCommandListRecorder::EndGpuProfileScope()
    {
        Record("EndGpuProfileScope", [](auto& buffer) { buffer.EndGpuProfileScope(); });
    }
    void RhiCommandListRecorder::BeginRenderPass(const RHI::RHIRenderPassDesc& desc)
    {
        if (!desc.attachmentsRequireExternalStateTransitions)
            throw std::logic_error("Deferred render passes require ordered attachment transitions");
        Record("BeginRenderPass", [desc](auto& buffer) { buffer.BeginRenderPass(desc); });
        m_canPrependQueueRelease = false;
    }
    void RhiCommandListRecorder::EndRenderPass()
    {
        Record("EndRenderPass", [](auto& buffer) { buffer.EndRenderPass(); });
    }
    void RhiCommandListRecorder::SetViewport(const RHI::RHIViewport& viewport)
    {
        Record("SetViewport", [viewport](auto& buffer) { buffer.SetViewport(viewport); });
    }
    void RhiCommandListRecorder::SetScissor(const RHI::RHIRect2D& rect)
    {
        Record("SetScissor", [rect](auto& buffer) { buffer.SetScissor(rect); });
    }
    void RhiCommandListRecorder::BindGraphicsPipeline(const std::shared_ptr<RHI::RHIGraphicsPipeline>& pipeline)
    {
        Record("BindGraphicsPipeline", [pipeline](auto& buffer) { buffer.BindGraphicsPipeline(pipeline); });
    }
    void RhiCommandListRecorder::BindComputePipeline(const std::shared_ptr<RHI::RHIComputePipeline>& pipeline)
    {
        Record("BindComputePipeline", [pipeline](auto& buffer) { buffer.BindComputePipeline(pipeline); });
    }
    void RhiCommandListRecorder::BindBindingSet(uint32_t index, const std::shared_ptr<RHI::RHIBindingSet>& bindingSet)
    {
        Record("BindBindingSet", [index, bindingSet](auto& buffer) { buffer.BindBindingSet(index, bindingSet); });
    }
    void RhiCommandListRecorder::PushConstants(RHI::ShaderStageMask stages, uint32_t offset, uint32_t size, const void* data)
    {
        if (size != 0u && data == nullptr)
            throw std::invalid_argument("Push constants require owned input data");
        std::vector<std::byte> bytes(size);
        if (size != 0u)
            std::memcpy(bytes.data(), data, size);
        Record("PushConstants", [stages, offset, bytes = std::move(bytes)](auto& buffer)
        {
            buffer.PushConstants(stages, offset, static_cast<uint32_t>(bytes.size()), bytes.data());
        });
    }
    void RhiCommandListRecorder::BindVertexBuffer(uint32_t slot, const RHI::RHIVertexBufferView& view)
    {
        Record("BindVertexBuffer", [slot, view](auto& buffer) { buffer.BindVertexBuffer(slot, view); });
    }
    void RhiCommandListRecorder::BindIndexBuffer(const RHI::RHIIndexBufferView& view)
    {
        Record("BindIndexBuffer", [view](auto& buffer) { buffer.BindIndexBuffer(view); });
    }
    void RhiCommandListRecorder::Draw(uint32_t vertices, uint32_t instances, uint32_t firstVertex, uint32_t firstInstance)
    {
        Record("Draw", [vertices, instances, firstVertex, firstInstance](auto& buffer)
        {
            RequireSuccess(buffer.DrawChecked(vertices, instances, firstVertex, firstInstance));
        });
        m_canPrependQueueRelease = false;
    }
    void RhiCommandListRecorder::DrawIndexed(uint32_t indices, uint32_t instances, uint32_t firstIndex,
        int32_t vertexOffset, uint32_t firstInstance)
    {
        Record("DrawIndexed", [indices, instances, firstIndex, vertexOffset, firstInstance](auto& buffer)
        {
            RequireSuccess(buffer.DrawIndexedChecked(indices, instances, firstIndex, vertexOffset, firstInstance));
        });
        m_canPrependQueueRelease = false;
    }
    void RhiCommandListRecorder::Dispatch(uint32_t x, uint32_t y, uint32_t z)
    {
        Record("Dispatch", [x, y, z](auto& buffer) { buffer.Dispatch(x, y, z); });
        m_canPrependQueueRelease = false;
    }
    void RhiCommandListRecorder::CopyBuffer(const std::shared_ptr<RHI::RHIBuffer>& source,
        const std::shared_ptr<RHI::RHIBuffer>& destination, const RHI::RHIBufferCopyRegion& region)
    {
        Record("CopyBuffer", [source, destination, region](auto& buffer) { buffer.CopyBuffer(source, destination, region); });
        m_canPrependQueueRelease = false;
    }
    void RhiCommandListRecorder::CopyBufferToTexture(const RHI::RHIBufferToTextureCopyDesc& desc)
    {
        Record("CopyBufferToTexture", [desc](auto& buffer) { buffer.CopyBufferToTexture(desc); });
        m_canPrependQueueRelease = false;
    }
    void RhiCommandListRecorder::CopyTexture(const RHI::RHITextureCopyDesc& desc)
    {
        Record("CopyTexture", [desc](auto& buffer) { buffer.CopyTexture(desc); });
        m_canPrependQueueRelease = false;
    }
    RHI::RHIBarrierDesc RhiCommandListRecorder::FilterBarrierDesc(const RHI::RHIBarrierDesc& barrier) const
    {
        auto requested = barrier;
        if (m_previousStates)
            m_previousStates(requested);
        if (m_tracker != nullptr)
        {
            for (auto& transition : requested.bufferBarriers)
                if (m_tracker->GetBufferState(transition.buffer).has_value())
                    transition.before = RHI::ResourceState::Unknown;
            for (auto& transition : requested.textureBarriers)
                if (m_tracker->GetTextureState(transition.texture, transition.subresourceRange).has_value())
                    transition.before = RHI::ResourceState::Unknown;
        }
        auto resolved = m_tracker != nullptr
            ? m_tracker->BuildTransitionBarriers(requested.bufferBarriers, requested.textureBarriers)
            : requested;
        for (auto& transition : resolved.bufferBarriers)
            if (transition.buffer != nullptr && transition.before == RHI::ResourceState::Unknown)
                transition.before = transition.buffer->GetState();
        for (auto& transition : resolved.textureBarriers)
            if (transition.texture != nullptr && transition.before == RHI::ResourceState::Unknown)
                transition.before = transition.texture->GetState();
        return m_nativeBuffer.FilterBarrierDesc(resolved);
    }
    RHI::RHICommandRecordingResult RhiCommandListRecorder::BarrierChecked(const RHI::RHIBarrierDesc& barrier)
    {
        if (!m_recording)
            return { RHI::RHICommandRecordingStatusCode::InvalidArgument, "Deferred barrier requires a recording list" };
        auto resolved = FilterBarrierDesc(barrier);
        if (resolved.bufferBarriers.empty() && resolved.textureBarriers.empty())
            return {};
        const auto release = m_nativeBuffer.BuildQueueReleaseBarriers(resolved);
        if (!release.bufferBarriers.empty() || !release.textureBarriers.empty())
        {
            if (!m_canPrependQueueRelease)
                return { RHI::RHICommandRecordingStatusCode::InvalidArgument,
                    "Graphics queue release must precede GPU commands; split the RHI command list" };
            if (!m_queueRelease || m_tracker == nullptr)
                return { RHI::RHICommandRecordingStatusCode::InvalidArgument,
                    "RHI queue transition requires an ordered Graphics release list" };
            const auto result = m_queueRelease(release);
            if (!result.Succeeded())
                return result;
            resolved = FilterBarrierDesc(barrier);
        }
        resolved.statesAreExplicit = true;
        if (!m_list.RecordBarrier(resolved))
            return { RHI::RHICommandRecordingStatusCode::InvalidArgument, "Cannot record a closed RHI barrier" };
        if (m_tracker != nullptr)
            m_tracker->Commit(resolved);
        return {};
    }
    void RhiCommandListRecorder::Barrier(const RHI::RHIBarrierDesc& barrier)
    {
        RequireSuccess(BarrierChecked(barrier));
    }
}
