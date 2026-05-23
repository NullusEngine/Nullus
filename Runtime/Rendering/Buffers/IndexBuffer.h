#pragma once

#include <memory>
#include <vector>

#include "Rendering/RHI/Core/RHIResource.h"

namespace NLS::Render::Buffers
{
/**
 * Backend-neutral index-buffer wrapper over the formal RHI surface.
 */
class NLS_RENDER_API IndexBuffer
{
public:
    /**
     * Create the index buffer from a pointer to the first element and an element count.
     * @param p_data
     * @param p_elements
     */
    IndexBuffer(
        unsigned int* p_data,
        size_t p_elements,
        RHI::MemoryUsage memoryUsage = RHI::MemoryUsage::GPUOnly);

    /**
     * Create the index buffer from a vector.
     * @param p_data
     */
    explicit IndexBuffer(
        std::vector<uint32_t>& p_data,
        RHI::MemoryUsage memoryUsage = RHI::MemoryUsage::GPUOnly);

    /**
     * Destructor
     */
    ~IndexBuffer();

    const std::shared_ptr<RHI::RHIBuffer>& GetBufferHandle() const { return m_explicitBuffer; }
    const std::shared_ptr<RHI::RHIBuffer>& GetExplicitRHIBufferHandle() const { return GetBufferHandle(); }

private:
    std::shared_ptr<RHI::RHIBuffer> m_explicitBuffer;
};
} // namespace NLS::Render::Buffers
