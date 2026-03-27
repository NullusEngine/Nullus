#pragma once

#include <glad/glad.h>

#include <cstdint>
#include <string>

namespace NLS::Render::Backend
{
	struct OpenGLShaderProgramAPI
	{
		static void DeleteProgram(uint32_t programId) { glDeleteProgram(programId); }
		static void UseProgram(uint32_t programId) { glUseProgram(programId); }
		static void SetUniformInt(int32_t location, int value) { glUniform1i(location, value); }
		static void SetUniformFloat(int32_t location, float value) { glUniform1f(location, value); }
		static void SetUniformVec2(int32_t location, float x, float y) { glUniform2f(location, x, y); }
		static void SetUniformVec3(int32_t location, float x, float y, float z) { glUniform3f(location, x, y, z); }
		static void SetUniformVec4(int32_t location, float x, float y, float z, float w) { glUniform4f(location, x, y, z, w); }
		static void SetUniformMat4(int32_t location, const float* values, bool transpose = true) { glUniformMatrix4fv(location, 1, transpose ? GL_TRUE : GL_FALSE, values); }
		static void GetUniformInt(uint32_t programId, int32_t location, int* value) { glGetUniformiv(programId, location, value); }
		static void GetUniformFloat(uint32_t programId, int32_t location, float* value) { glGetUniformfv(programId, location, value); }
		static void GetUniformFloatArray(uint32_t programId, int32_t location, float* values) { glGetUniformfv(programId, location, values); }
		static int32_t GetUniformLocation(uint32_t programId, const std::string& name) { return glGetUniformLocation(programId, name.c_str()); }
		static int32_t GetProgramResourceLocation(uint32_t programId, const std::string& name) { return glGetProgramResourceLocation(programId, GL_UNIFORM, name.c_str()); }
		static int32_t GetActiveUniformCount(uint32_t programId) { GLint count = 0; glGetProgramiv(programId, GL_ACTIVE_UNIFORMS, &count); return count; }
		static void GetActiveUniform(uint32_t programId, uint32_t uniformIndex, GLsizei bufferSize, GLsizei* actualLength, GLint* arraySize, GLenum* type, GLchar* nameData) { glGetActiveUniform(programId, uniformIndex, bufferSize, actualLength, arraySize, type, nameData); }
		static uint32_t CreateProgram() { return glCreateProgram(); }
		static void AttachShader(uint32_t programId, uint32_t shaderId) { glAttachShader(programId, shaderId); }
		static void LinkProgram(uint32_t programId) { glLinkProgram(programId); }
		static int32_t GetLinkStatus(uint32_t programId) { GLint status = 0; glGetProgramiv(programId, GL_LINK_STATUS, &status); return status; }
		static int32_t GetProgramInfoLogLength(uint32_t programId) { GLint length = 0; glGetProgramiv(programId, GL_INFO_LOG_LENGTH, &length); return length; }
		static void GetProgramInfoLog(uint32_t programId, GLsizei maxLength, GLsizei* length, char* infoLog) { glGetProgramInfoLog(programId, maxLength, length, infoLog); }
		static void ValidateProgram(uint32_t programId) { glValidateProgram(programId); }
		static uint32_t CreateShader(uint32_t shaderType) { return glCreateShader(shaderType); }
		static void SetShaderSource(uint32_t shaderId, const char* source) { glShaderSource(shaderId, 1, &source, nullptr); }
		static void CompileShader(uint32_t shaderId) { glCompileShader(shaderId); }
		static int32_t GetShaderCompileStatus(uint32_t shaderId) { GLint status = 0; glGetShaderiv(shaderId, GL_COMPILE_STATUS, &status); return status; }
		static int32_t GetShaderInfoLogLength(uint32_t shaderId) { GLint length = 0; glGetShaderiv(shaderId, GL_INFO_LOG_LENGTH, &length); return length; }
		static void GetShaderInfoLog(uint32_t shaderId, GLsizei maxLength, GLsizei* length, char* infoLog) { glGetShaderInfoLog(shaderId, maxLength, length, infoLog); }
		static void DeleteShader(uint32_t shaderId) { glDeleteShader(shaderId); }
	};
}
