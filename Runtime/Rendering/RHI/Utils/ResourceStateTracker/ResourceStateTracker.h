#pragma once

#include <optional>
#include <vector>

#include "RenderDef.h"
#include "Rendering/RHI/Core/RHICommand.h"

namespace NLS::Render::RHI
{
    struct NLS_RENDER_API TrackedBufferState
    {
        std::shared_ptr<RHIBuffer> buffer;
        ResourceState state = ResourceState::Unknown;
        PipelineStageMask stageMask = PipelineStageMask::None;
        AccessMask accessMask = AccessMask::None;
    };

    struct NLS_RENDER_API TrackedTextureState
    {
        std::shared_ptr<RHITexture> texture;
        RHISubresourceRange subresourceRange{};
        ResourceState state = ResourceState::Unknown;
        PipelineStageMask stageMask = PipelineStageMask::None;
        AccessMask accessMask = AccessMask::None;
    };

    struct NLS_RENDER_API ResourceStateTrackerStats
    {
        uint64_t currentFrameIndex = 0u;
        uint64_t trackedBufferCount = 0u;
        uint64_t trackedTextureCount = 0u;
        uint64_t transientBufferRegistrations = 0u;
        uint64_t transientTextureRegistrations = 0u;
        uint64_t retiredTransientBuffers = 0u;
        uint64_t retiredTransientTextures = 0u;
    };

    class NLS_RENDER_API ResourceStateTracker
    {
    public:
        virtual ~ResourceStateTracker() = default;
        virtual void BeginFrame(uint64_t frameIndex) = 0;
        virtual void Reset() = 0;
        virtual std::optional<TrackedBufferState> GetBufferState(const std::shared_ptr<RHIBuffer>& buffer) const = 0;
        virtual std::optional<TrackedTextureState> GetTextureState(
            const std::shared_ptr<RHITexture>& texture,
            const RHISubresourceRange& subresourceRange) const = 0;
        virtual RHIBarrierDesc BuildTransitionBarriers(
            const std::vector<RHIBufferBarrier>& bufferBarriers,
            const std::vector<RHITextureBarrier>& textureBarriers) const = 0;
        virtual void RegisterTransientBuffer(
            const std::shared_ptr<RHIBuffer>& buffer,
            uint64_t retireAfterFrameIndex) = 0;
        virtual void RegisterTransientTexture(
            const std::shared_ptr<RHITexture>& texture,
            const RHISubresourceRange& subresourceRange,
            uint64_t retireAfterFrameIndex) = 0;
        virtual void RetireTransientResources(uint64_t completedFrameIndex) = 0;
        virtual void Commit(const RHIBarrierDesc& barriers) = 0;
        virtual ResourceStateTrackerStats GetStats() const = 0;
    };

    NLS_RENDER_API std::shared_ptr<ResourceStateTracker> CreateDefaultResourceStateTracker();
}
