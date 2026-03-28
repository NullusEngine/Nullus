#pragma once

#include <memory>
#include <vector>
#include <string>

#include "Rendering/Context/Driver.h"
#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/RHI/IRHIResource.h"
#include "Rendering/Settings/EAccessSpecifier.h"
#include "RenderDef.h"
namespace NLS::Render::Resources
{
}

namespace NLS::Render::Buffers
{
/**
 * Wraps OpenGL UBO
 */
class NLS_RENDER_API UniformBuffer
{
public:
    /**
     * Create a UniformBuffer
     * @param p_size (Specify the size in bytes of the UBO data)
     * @param p_bindingPoint (Specify the binding point on which the uniform buffer should be binded)
     * @parma p_offset (The offset of the UBO, sizeof previouses UBO if the binding point is != 0)
     * @param p_accessSpecifier
     */
    UniformBuffer(
        size_t p_size,
        uint32_t p_bindingPoint = 0,
        uint32_t p_offset = 0,
        Settings::EAccessSpecifier p_accessSpecifier = Settings::EAccessSpecifier::DYNAMIC_DRAW);

    /**
     * Destructor of the UniformBuffer
     */
    ~UniformBuffer();

    /**
     * Bind the UBO
     * @param p_bindingPoint
     */
    void Bind(uint32_t p_bindingPoint);

    /**
     * Unbind the UBO
     */
    void Unbind();

    /**
     * Set the data in the UBO located at p_offset to p_data
     * @param p_data
     * @param p_offset
     */
    template<typename T>
    void SetSubData(const T& p_data, size_t p_offset);

    /**
     * Set the data in the UBO located at p_offset to p_data
     * @param p_data
     * @param p_offsetInOut (Will keep track of the current stride of the data layout)
     */
    template<typename T>
    void SetSubData(const T& p_data, std::reference_wrapper<size_t> p_offsetInOut);

    void SetRawData(const void* p_data, uint32_t size, size_t p_offset = 0);
    std::shared_ptr<RHI::RHIBuffer> CreateExplicitSnapshotBuffer(const std::string& debugName = {}) const;

    /**
     * Return the ID of the UBO
     */
    uint32_t GetID() const;
    const RHI::IRHIBuffer* GetRHIBuffer() const { return m_bufferResource.get(); }
    const std::shared_ptr<RHI::RHIBuffer>& GetBufferHandle() const { return m_explicitBuffer; }
    const std::shared_ptr<RHI::IRHIBuffer>& GetRHIBufferHandle() const { return m_bufferResource; }
    const std::shared_ptr<RHI::RHIBuffer>& GetExplicitRHIBufferHandle() const { return GetBufferHandle(); }

private:
    void _SetSubData(const void* p_data, uint32_t size, size_t p_offset);
    void _SetSubData(const void* p_data, uint32_t size, std::reference_wrapper<size_t> p_offsetInOut);

private:
    uint32_t m_bufferID = 0;
    uint32_t m_bindingPoint = 0;
    size_t m_size = 0;
    std::vector<uint8_t> m_shadowData;
    std::shared_ptr<RHI::IRHIBuffer> m_bufferResource;
    std::shared_ptr<RHI::RHIBuffer> m_explicitBuffer;
};
} // namespace NLS::Render::Buffers

#include "Rendering/Buffers/UniformBuffer.inl"
