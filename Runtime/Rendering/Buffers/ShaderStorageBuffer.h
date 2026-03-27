#pragma once

#include <memory>
#include <vector>

#include "Rendering/Context/Driver.h"
#include "Rendering/RHI/IRHIResource.h"
#include "Rendering/Settings/EAccessSpecifier.h"

namespace NLS::Render::Resources
{
class Shader;
}

namespace NLS::Render::Buffers
{
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

    uint32_t GetID() const { return m_bufferID; }
    const RHI::IRHIBuffer* GetRHIBuffer() const { return m_bufferResource.get(); }

    /**
     * Send the block data
     */
    template<typename T>
    void SendBlocks(T* p_data, size_t p_size);

private:
    uint32_t m_bufferID = 0;
    std::shared_ptr<RHI::IRHIBuffer> m_bufferResource;
};
} // namespace NLS::Render::Buffers

#include "Rendering/Buffers/ShaderStorageBuffer.inl"
