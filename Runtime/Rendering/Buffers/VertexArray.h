#pragma once

#include <cstdint>

#include "Rendering/Buffers/VertexBuffer.h"
#include "Rendering/Settings/EDataType.h"
namespace NLS::Render::Buffers
{
/**
 * Backend-neutral vertex-input layout wrapper over the formal RHI surface.
 */
class NLS_RENDER_API VertexArray
{
public:
    /**
     * Create the vertex array
     */
    VertexArray();

    /**
     * Destroy the vertex array
     */
    ~VertexArray();

    /**
     * Register a vertex buffer attribute layout.
     * @param p_attribute
     * @param p_vertexBuffer
     * @param p_type
     * @param p_count
     * @param p_stride
     * @param p_offset
     */
    template<class T>
    void BindAttribute(
        uint32_t p_attribute,
        VertexBuffer<T>& p_vertexBuffer,
        Settings::EDataType p_type,
        uint64_t p_count,
        uint64_t p_stride,
        intptr_t p_offset) const;
};
} // namespace NLS::Render::Buffers

#include "Rendering/Buffers/VertexArray.inl"
