#pragma once

#include <memory>
#include <vector>

#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/RHI/Core/IRHIResource.h"

namespace NLS::Render::Buffers
{
/**
 * Wraps OpenGL EBO
 */
class NLS_RENDER_API IndexBuffer
{
public:
    /**
     * Create the EBO using a pointer to the first element and a size (number of elements)
     * @param p_data
     * @parma p_elements
     */
    IndexBuffer(unsigned int* p_data, size_t p_elements);

    /**
     * Create the EBO using a vector
     * @param p_data
     */
    IndexBuffer(std::vector<uint32_t>& p_data);

    /**
     * Destructor
     */
    ~IndexBuffer();

    /**
     * Bind the buffer
     */
    void Bind();

    /**
     * Unbind the buffer
     */
    void Unbind();

    /**
     * Returns the ID of the OpenGL EBO (always 0 for formal RHI)
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
