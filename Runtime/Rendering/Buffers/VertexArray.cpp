#include "Rendering/Buffers/VertexArray.h"

NLS::Rendering::Buffers::VertexArray::VertexArray()
{
	glGenVertexArrays(1, &m_bufferID);
	glBindVertexArray(m_bufferID);
}

NLS::Rendering::Buffers::VertexArray::~VertexArray()
{
	glDeleteVertexArrays(1, &m_bufferID);
}

void NLS::Rendering::Buffers::VertexArray::Bind() const
{
	glBindVertexArray(m_bufferID);
}

void NLS::Rendering::Buffers::VertexArray::Unbind() const
{
	glBindVertexArray(0);
}

GLint NLS::Rendering::Buffers::VertexArray::GetID() const
{
	return m_bufferID;
}
