#include "Rendering/Buffers/VertexArray.h"

Rendering::Buffers::VertexArray::VertexArray()
{
	glGenVertexArrays(1, &m_bufferID);
	glBindVertexArray(m_bufferID);
}

Rendering::Buffers::VertexArray::~VertexArray()
{
	glDeleteVertexArrays(1, &m_bufferID);
}

void Rendering::Buffers::VertexArray::Bind() const
{
	glBindVertexArray(m_bufferID);
}

void Rendering::Buffers::VertexArray::Unbind() const
{
	glBindVertexArray(0);
}

GLint Rendering::Buffers::VertexArray::GetID() const
{
	return m_bufferID;
}
