#pragma once

#include <glad/glad.h>

#include <cstdint>

namespace NLS::Render::Backend
{
	struct OpenGLVertexInputAPI
	{
		static uint32_t CreateVertexArray()
		{
			GLuint id = 0;
			glGenVertexArrays(1, &id);
			return id;
		}

		static void DestroyVertexArray(uint32_t vertexArrayId)
		{
			GLuint id = vertexArrayId;
			glDeleteVertexArrays(1, &id);
		}

		static void BindVertexArray(uint32_t vertexArrayId)
		{
			glBindVertexArray(vertexArrayId);
		}

		static void EnableVertexAttribute(uint32_t attribute)
		{
			glEnableVertexAttribArray(attribute);
		}

		static void SetVertexAttributePointer(uint32_t attribute, int32_t count, uint32_t type, bool normalized, uint32_t stride, intptr_t offset)
		{
			glVertexAttribPointer(attribute, count, type, normalized ? GL_TRUE : GL_FALSE, stride, reinterpret_cast<const void*>(offset));
		}
	};
}
