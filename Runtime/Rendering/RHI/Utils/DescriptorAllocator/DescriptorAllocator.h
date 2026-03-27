#pragma once

#include <cstdint>
#include <string>
#include <memory>

#include "RenderDef.h"
#include "Rendering/RHI/Core/RHIBinding.h"

namespace NLS::Render::RHI
{
    enum class NLS_RENDER_API DescriptorAllocationLifetime : uint8_t
    {
        TransientFrame,
        Persistent
    };

    struct NLS_RENDER_API DescriptorAllocation
    {
        uint64_t offset = 0;
        uint32_t count = 0;
        DescriptorAllocationLifetime lifetime = DescriptorAllocationLifetime::TransientFrame;
        uint64_t frameIndex = 0;
        std::string debugName;

        [[nodiscard]] bool IsValid() const
        {
            return count > 0;
        }
    };

    struct NLS_RENDER_API DescriptorAllocationRequest
    {
        uint32_t count = 1;
        DescriptorAllocationLifetime lifetime = DescriptorAllocationLifetime::TransientFrame;
        uint64_t frameIndex = 0;
        std::shared_ptr<RHIBindingLayout> layout;
        std::string debugName;
    };

    class NLS_RENDER_API DescriptorAllocator
    {
    public:
        virtual ~DescriptorAllocator() = default;
        virtual void BeginFrame(uint64_t frameIndex) = 0;
        virtual void EndFrame(uint64_t frameIndex) = 0;
        virtual DescriptorAllocation Allocate(const DescriptorAllocationRequest& request) = 0;
        virtual void Release(const DescriptorAllocation& allocation) = 0;
        virtual void Reset() = 0;
    };

    NLS_RENDER_API std::shared_ptr<DescriptorAllocator> CreateDefaultDescriptorAllocator(uint64_t transientCapacity = 65536);
}
