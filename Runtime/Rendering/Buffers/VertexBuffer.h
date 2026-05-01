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
    VertexBuffer(T* p_data, size_t p_elements);

    /**
     * Create the vertex buffer from a vector.
     * @param p_data
     */
    VertexBuffer(std::vector<T>& p_data);

    /**
     * Destructor
     */
    ~VertexBuffer();

    const std::shared_ptr<RHI::RHIBuffer>& GetBufferHandle() const { return m_explicitBuffer; }
    const std::shared_ptr<RHI::RHIBuffer>& GetExplicitRHIBufferHandle() const { return GetBufferHandle(); }

private:
    std::shared_ptr<RHI::RHIBuffer> m_explicitBuffer;
};
} // namespace NLS::Render::Buffers

#include "Rendering/Buffers/VertexBuffer.inl"
