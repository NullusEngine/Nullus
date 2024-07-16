#include <glad/glad.h>

#include "Rendering/Buffers/ShaderStorageBuffer.h"

NLS::Render::Buffers::ShaderStorageBuffer::ShaderStorageBuffer(Settings::EAccessSpecifier p_accessSpecifier)
{
	glGenBuffers(1, &m_bufferID);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_bufferID);
	glBufferData(GL_SHADER_STORAGE_BUFFER, 0, nullptr, static_cast<GLenum>(p_accessSpecifier));
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

NLS::Render::Buffers::ShaderStorageBuffer::~ShaderStorageBuffer()
{
	glDeleteBuffers(1, &m_bufferID);
}

void NLS::Render::Buffers::ShaderStorageBuffer::Bind(uint32_t p_bindingPoint)
{
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, p_bindingPoint, m_bufferID);
}

void NLS::Render::Buffers::ShaderStorageBuffer::Unbind()
{
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}