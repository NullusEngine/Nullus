#pragma once

#include "Rendering/Buffers/VertexBuffer.h"
#include "Rendering/Settings/EDataType.h"
namespace NLS::Render::Buffers
{
/**
 * Wraps OpenGL VAO
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
     * Register a VBO into the VAO
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

    /**
     * Bind the buffer
     */
    void Bind() const;

    /**
     * Unbind the buffer
     */
    void Unbind() const;

    /**
     * Return the VAO OpenGL ID
     */
    GLint GetID() const;

private:
    GLuint m_bufferID;
};
} // namespace NLS::Render::Buffers

#include "Rendering/Buffers/VertexArray.inl"