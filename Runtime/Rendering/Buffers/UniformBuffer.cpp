#include "Rendering/Buffers/UniformBuffer.h"
#include "Rendering/Backend/OpenGL/OpenGLTypeMappings.h"
#include "Rendering/Resources/Shader.h"
#include "Rendering/Backend/OpenGL/OpenGLAPI.h"

using OpenGLRHI = NLS::Render::Backend::OpenGLAPI;
namespace OpenGLBackend = NLS::Render::Backend;

namespace NLS::Render::Buffers
{
UniformBuffer::UniformBuffer(
    size_t p_size,
    uint32_t p_bindingPoint,
    uint32_t p_offset,
    Settings::EAccessSpecifier p_accessSpecifier)
    : m_bindingPoint(p_bindingPoint)
{
    OpenGLRHI rhi;
    m_bufferID = rhi.CreateBuffer();
    rhi.BindBuffer(GL_UNIFORM_BUFFER, m_bufferID);
    rhi.SetBufferData(GL_UNIFORM_BUFFER, p_size, nullptr, OpenGLBackend::ToOpenGLBufferUsage(p_accessSpecifier));
    rhi.BindBuffer(GL_UNIFORM_BUFFER, 0);
}

UniformBuffer::~UniformBuffer()
{
    OpenGLRHI{}.DestroyBuffer(m_bufferID);
}

void UniformBuffer::Bind(uint32_t p_bindingPoint)
{
    OpenGLRHI{}.BindBufferBase(GL_UNIFORM_BUFFER, p_bindingPoint, m_bufferID);
}

void UniformBuffer::Unbind()
{
    OpenGLRHI{}.BindBuffer(GL_UNIFORM_BUFFER, 0);
}

GLuint UniformBuffer::GetID() const
{
    return m_bufferID;
}

void UniformBuffer::BindBlockToShader(Resources::Shader& p_shader, uint32_t p_uniformBlockLocation, uint32_t p_bindingPoint)
{
    OpenGLRHI{}.SetUniformBlockBinding(p_shader.id, p_uniformBlockLocation, p_bindingPoint);
}

void UniformBuffer::BindBlockToShader(Resources::Shader& p_shader, const std::string& p_name, uint32_t p_bindingPoint)
{
    OpenGLRHI{}.SetUniformBlockBinding(p_shader.id, GetBlockLocation(p_shader, p_name), p_bindingPoint);
}

uint32_t UniformBuffer::GetBlockLocation(Resources::Shader& p_shader, const std::string& p_name)
{
    return OpenGLRHI{}.GetUniformBlockIndex(p_shader.id, p_name);
}

void UniformBuffer::_SetSubData(const void* p_data, uint32_t size, size_t p_offset)
{
    OpenGLRHI rhi;
    rhi.BindBuffer(GL_UNIFORM_BUFFER, m_bufferID);
    rhi.SetBufferSubData(GL_UNIFORM_BUFFER, p_offset, size, p_data);
    rhi.BindBuffer(GL_UNIFORM_BUFFER, 0);
}

void UniformBuffer::_SetSubData(const void* p_data, uint32_t size, std::reference_wrapper<size_t> p_offsetInOut)
{
    size_t dataSize = size;
    OpenGLRHI rhi;
    rhi.BindBuffer(GL_UNIFORM_BUFFER, m_bufferID);
    rhi.SetBufferSubData(GL_UNIFORM_BUFFER, p_offsetInOut.get(), dataSize, p_data);
    p_offsetInOut.get() += dataSize;
    rhi.BindBuffer(GL_UNIFORM_BUFFER, 0);
}
}
