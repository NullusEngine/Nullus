#pragma once

#include <memory>
#include <vector>

#include "Rendering/RHI/Core/RHIResource.h"

namespace NLS::Render::Buffers
{
/**
 * Backend-neutral vertex-buffer wrapper over the formal RHI surface.
 */
template<class T>
class VertexBuffer
{
public:
    /**
     * Create the vertex buffer from a pointer to the first element and an element count.
     * @param p_data
     * @param p_elements
     */
    VertexBuffer(
        const T* p_data,
        size_t p_elements,
        RHI::MemoryUsage memoryUsage = RHI::MemoryUsage::GPUOnly);

    /**
     * Create the vertex buffer from a vector.
     * @param p_data
     */
    explicit VertexBuffer(
        const std::vector<T>& p_data,
        RHI::MemoryUsage memoryUsage = RHI::MemoryUsage::GPUOnly);

    /**
     * Destructor
     */
    ~VertexBuffer();

    const std::shared_ptr<RHI::RHIBuffer>& GetBufferHandle() const { return m_explicitBuffer; }
    const std::shared_ptr<RHI::RHIBuffer>& GetExplicitRHIBufferHandle() const { return GetBufferHandle(); }
    bool Update(
        const T* p_data,
        size_t p_elements,
        size_t p_destinationElementOffset = 0u);

private:
    std::shared_ptr<RHI::RHIBuffer> m_explicitBuffer;
};
} // namespace NLS::Render::Buffers

#include "Rendering/Buffers/VertexBuffer.inl"
