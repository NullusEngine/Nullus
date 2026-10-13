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

namespace NLS::Render::RHI
{
class RHIDevice;
}

namespace NLS::Render::Buffers
{
struct ExplicitUniformBufferSnapshot
{
    std::shared_ptr<RHI::RHIBuffer> buffer;
    uint64_t offset = 0u;
    bool createdBuffer = false;
};

// Pages are append-only and owned by the captured bindings, never by a reusable frame slot.
class NLS_RENDER_API ScopedUniformBufferSnapshotBatch
{
public:
    ScopedUniformBufferSnapshotBatch();
    ~ScopedUniformBufferSnapshotBatch();
    ScopedUniformBufferSnapshotBatch(const ScopedUniformBufferSnapshotBatch&) = delete;
    ScopedUniformBufferSnapshotBatch& operator=(const ScopedUniformBufferSnapshotBatch&) = delete;

private:
    friend class UniformBuffer;
    ExplicitUniformBufferSnapshot Allocate(
        const std::shared_ptr<RHI::RHIDevice>& device,
        const std::vector<uint8_t>& data,
        const std::string& debugName);

    ScopedUniformBufferSnapshotBatch* m_previous = nullptr;
    std::shared_ptr<RHI::RHIDevice> m_device;
    std::shared_ptr<RHI::RHIBuffer> m_page;
    uint64_t m_cursor = 0u;
    bool m_failed = false;
};

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
    ExplicitUniformBufferSnapshot CreateExplicitSnapshot(const std::string& debugName = {}) const;
    std::shared_ptr<RHI::RHIBuffer> CreateExplicitSnapshotBuffer(const std::string& debugName = {}) const;

    const std::shared_ptr<RHI::RHIBuffer>& GetBufferHandle() const { return m_explicitBuffer; }
    const std::shared_ptr<RHI::RHIBuffer>& GetExplicitRHIBufferHandle() const { return GetBufferHandle(); }
    /**
     * CPU-side copy of the buffer contents.
     *
     * Callers that copy the current value into their own ring have to read the
     * bytes the last SetSubData stored; there is no readback path from the GPU
     * buffer, and a snapshot buffer per write is what the ring exists to avoid.
     */
    const std::vector<uint8_t>& GetShadowData() const { return m_shadowData; }

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
