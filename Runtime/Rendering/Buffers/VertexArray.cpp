#include "Rendering/Buffers/VertexArray.h"

NLS::Render::Buffers::VertexArray::VertexArray()
{
	glGenVertexArrays(1, &m_bufferID);
	glBindVertexArray(m_bufferID);
}

NLS::Render::Buffers::VertexArray::~VertexArray()
{
	glDeleteVertexArrays(1, &m_bufferID);
}

void NLS::Render::Buffers::VertexArray::Bind() const
{
	glBindVertexArray(m_bufferID);
}

void NLS::Render::Buffers::VertexArray::Unbind() const
{
	glBindVertexArray(0);
}

GLint NLS::Render::Buffers::VertexArray::GetID() const
{
	return m_bufferID;
}
