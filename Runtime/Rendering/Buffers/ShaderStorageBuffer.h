#pragma once

#include <memory>

#include "Rendering/RHI/Core/RHIResource.h"

namespace NLS::Render::Buffers
{
/**
 * Backend-neutral shader-storage buffer wrapper over the formal RHI surface.
 */
class NLS_RENDER_API ShaderStorageBuffer
{
public:
    /**
     * Create an empty shader-storage buffer handle.
     */
    ShaderStorageBuffer();

    /**
     * Destroy the shader-storage buffer.
     */
    ~ShaderStorageBuffer();

    const std::shared_ptr<RHI::RHIBuffer>& GetBufferHandle() const { return m_explicitBuffer; }
    const std::shared_ptr<RHI::RHIBuffer>& GetExplicitRHIBufferHandle() const { return GetBufferHandle(); }

    /**
     * Send the block data
     */
    template<typename T>
    void SendBlocks(T* p_data, size_t p_size);

private:
    std::shared_ptr<RHI::RHIBuffer> m_explicitBuffer;
    size_t m_currentSize = 0;
};
} // namespace NLS::Render::Buffers

#include "Rendering/Buffers/ShaderStorageBuffer.inl"
