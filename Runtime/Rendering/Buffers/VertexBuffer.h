#pragma once

#include <memory>
#include <vector>

#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/RHI/Core/IRHIResource.h"

namespace NLS::Render::Buffers
{
/**
 * Wraps OpenGL VBO
 */
template<class T>
class VertexBuffer
{
public:
    /**
     * Create the VBO using a pointer to the first element and a size (number of elements)
     * @param p_data
     * @parma p_elements
     */
    VertexBuffer(T* p_data, size_t p_elements);

    /**
     * Create the EBO using a vector
     * @param p_data
     */
    VertexBuffer(std::vector<T>& p_data);

    /**
     * Destructor
     */
    ~VertexBuffer();

    /**
     * Bind the buffer
     */
    void Bind();

    /**
     * Bind the buffer
     */
    void Unbind();

    /**
     * Returnd the ID of the VBO (always 0 for formal RHI)
     */
    uint32_t GetID();
    const RHI::IRHIBuffer* GetRHIBuffer() const { return nullptr; }
    const std::shared_ptr<RHI::RHIBuffer>& GetBufferHandle() const { return m_explicitBuffer; }
    const std::shared_ptr<RHI::IRHIBuffer>& GetRHIBufferHandle() const { return nullptr; }
    const std::shared_ptr<RHI::RHIBuffer>& GetExplicitRHIBufferHandle() const { return GetBufferHandle(); }

private:
    std::shared_ptr<RHI::RHIBuffer> m_explicitBuffer;
};
} // namespace NLS::Render::Buffers

#include "Rendering/Buffers/VertexBuffer.inl"
