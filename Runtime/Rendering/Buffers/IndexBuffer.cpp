#include <glad/glad.h>
#include "Rendering/Buffers/IndexBuffer.h"

NLS::Render::Buffers::IndexBuffer::IndexBuffer(unsigned int* p_data, size_t p_elements)
{
	glGenBuffers(1, &m_bufferID);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_bufferID);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, p_elements * sizeof(unsigned int), p_data, GL_STATIC_DRAW);
}

NLS::Render::Buffers::IndexBuffer::IndexBuffer(std::vector<uint32_t>& p_data) : IndexBuffer(p_data.data(), p_data.size())
{
}

NLS::Render::Buffers::IndexBuffer::~IndexBuffer()
{
	glDeleteBuffers(1, &m_bufferID);
}

void NLS::Render::Buffers::IndexBuffer::Bind()
{
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_bufferID);
}

void NLS::Render::Buffers::IndexBuffer::Unbind()
{
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

uint32_t NLS::Render::Buffers::IndexBuffer::GetID()
{
	return m_bufferID;
}
