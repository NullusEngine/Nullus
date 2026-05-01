#pragma once

#include <memory>
#include <vector>
#include <string>

#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/Settings/EAccessSpecifier.h"
#include "RenderDef.h"
namespace NLS::Render::Resources
{
}

namespace NLS::Render::Buffers
{
/**
 * Backend-neutral uniform-buffer wrapper over the formal RHI surface.
 */
class NLS_RENDER_API UniformBuffer
{
public:
    /**
     * Create a uniform buffer.
     * @param p_size Size in bytes.
     * @param p_bindingPoint Requested logical binding slot metadata.
     * @param p_offset Byte offset inside the logical binding range.
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

    const std::shared_ptr<RHI::RHIBuffer>& GetBufferHandle() const { return m_explicitBuffer; }
    const std::shared_ptr<RHI::RHIBuffer>& GetExplicitRHIBufferHandle() const { return GetBufferHandle(); }

private:
    void _SetSubData(const void* p_data, uint32_t size, size_t p_offset);
    void _SetSubData(const void* p_data, uint32_t size, std::reference_wrapper<size_t> p_offsetInOut);

private:
    size_t m_size = 0;
    std::vector<uint8_t> m_shadowData;
    std::shared_ptr<RHI::RHIBuffer> m_explicitBuffer;
};
} // namespace NLS::Render::Buffers

#include "Rendering/Buffers/UniformBuffer.inl"
