#include <glad/glad.h>

#include "Rendering/Buffers/UniformBuffer.h"
#include "Rendering/Resources/Shader.h"

NLS::Render::Buffers::UniformBuffer::UniformBuffer(
    size_t p_size,
    uint32_t p_bindingPoint,
    uint32_t p_offset,
    Settings::EAccessSpecifier p_accessSpecifier)
    : m_bindingPoint(p_bindingPoint)
{
    glGenBuffers(1, &m_bufferID);
    glBindBuffer(GL_UNIFORM_BUFFER, m_bufferID);
    glBufferData(GL_UNIFORM_BUFFER, p_size, nullptr, static_cast<GLint>(p_accessSpecifier));
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
}

NLS::Render::Buffers::UniformBuffer::~UniformBuffer()
{
    glDeleteBuffers(1, &m_bufferID);
}

void NLS::Render::Buffers::UniformBuffer::Bind(uint32_t p_bindingPoint)
{
    glBindBufferBase(GL_UNIFORM_BUFFER, p_bindingPoint, m_bufferID);
}

void NLS::Render::Buffers::UniformBuffer::Unbind()
{
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
}

GLuint NLS::Render::Buffers::UniformBuffer::GetID() const
{
    return m_bufferID;
}

void NLS::Render::Buffers::UniformBuffer::BindBlockToShader(NLS::Render::Resources::Shader& p_shader, uint32_t p_uniformBlockLocation, uint32_t p_bindingPoint)
{
    glUniformBlockBinding(p_shader.id, p_uniformBlockLocation, p_bindingPoint);
}

void NLS::Render::Buffers::UniformBuffer::BindBlockToShader(NLS::Render::Resources::Shader& p_shader, const std::string& p_name, uint32_t p_bindingPoint)
{
    glUniformBlockBinding(p_shader.id, GetBlockLocation(p_shader, p_name), p_bindingPoint);
}

uint32_t NLS::Render::Buffers::UniformBuffer::GetBlockLocation(NLS::Render::Resources::Shader& p_shader, const std::string& p_name)
{
    return glGetUniformBlockIndex(p_shader.id, p_name.c_str());
}

void NLS::Render::Buffers::UniformBuffer::_SetSubData(const void* p_data, uint32_t size, size_t p_offset)
{
    glBindBuffer(GL_UNIFORM_BUFFER, m_bufferID);
    glBufferSubData(GL_UNIFORM_BUFFER, p_offset, size, p_data);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
}

void NLS::Render::Buffers::UniformBuffer::_SetSubData(const void* p_data, uint32_t size, std::reference_wrapper<size_t> p_offsetInOut)
{
    glBindBuffer(GL_UNIFORM_BUFFER, m_bufferID);
    size_t dataSize = size;
    glBufferSubData(GL_UNIFORM_BUFFER, p_offsetInOut.get(), dataSize, p_data);
    p_offsetInOut.get() += dataSize;
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
}
