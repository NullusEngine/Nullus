#pragma once

#include <memory>
#include <vector>

#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/RHI/Core/IRHIResource.h"
#include "Rendering/Settings/EAccessSpecifier.h"

namespace NLS::Render::Resources
{
class Shader;
}

namespace NLS::Render::Buffers
{
class ShaderStorageBuffer;

/**
 * Wraps OpenGL SSBO
 */
class NLS_RENDER_API ShaderStorageBuffer
{
public:
    /**
     * Create a SSBO with the given access specifier hint
     */
    ShaderStorageBuffer(Settings::EAccessSpecifier p_accessSpecifier);

    /**
     * Destroy the SSBO
     */
    ~ShaderStorageBuffer();

    /**
     * Bind the SSBO to the given binding point
     * @param p_bindingPoint
     */
    void Bind(uint32_t p_bindingPoint);

    /**
     * Unbind the SSBO from the currently binding point
     */
    void Unbind();

    uint32_t GetID() const { return 0; } // Formal RHI has no legacy buffer ID
    const RHI::IRHIBuffer* GetRHIBuffer() const { return nullptr; }
    const std::shared_ptr<RHI::RHIBuffer>& GetBufferHandle() const { return m_explicitBuffer; }
    const std::shared_ptr<RHI::IRHIBuffer>& GetRHIBufferHandle() const { return nullptr; }
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
