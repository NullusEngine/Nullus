#include <Debug/Logger.h>

#include "Rendering/Backend/OpenGL/OpenGLShaderProgramAPI.h"
#include "Rendering/Backend/OpenGL/OpenGLTypeMappings.h"
#include "Rendering/Resources/Shader.h"
#include "Rendering/Resources/Texture2D.h"
#include "Rendering/Resources/TextureCube.h"

namespace
{
	using OpenGLShaderProgramAPI = NLS::Render::Backend::OpenGLShaderProgramAPI;
    using ShaderResourceKind = NLS::Render::Resources::ShaderResourceKind;
    using UniformType = NLS::Render::Resources::UniformType;
    using NLS::Maths::Matrix4;
    using NLS::Maths::Vector2;
    using NLS::Maths::Vector3;
    using NLS::Maths::Vector4;

	ShaderResourceKind GetShaderResourceKind(UniformType type)
	{
		switch (type)
		{
		case UniformType::UNIFORM_SAMPLER_2D:
		case UniformType::UNIFORM_SAMPLER_CUBE:
			return ShaderResourceKind::SampledTexture;
		default:
			return ShaderResourceKind::Value;
		}
	}
}

namespace NLS::Render::Resources
{
Shader::Shader(const std::string p_path, uint32_t p_id) : path(p_path), id(p_id)
{
	QueryUniforms();
}

Shader::~Shader()
{
	OpenGLShaderProgramAPI::DeleteProgram(id);
}

void Shader::Bind() const
{
	OpenGLShaderProgramAPI::UseProgram(id);
}

void Shader::Unbind() const
{
	OpenGLShaderProgramAPI::UseProgram(0);
}

void Shader::SetUniformInt(const std::string& p_name, int p_value)
{
	OpenGLShaderProgramAPI::SetUniformInt(GetUniformLocation(p_name), p_value);
}

void Shader::SetUniformFloat(const std::string& p_name, float p_value)
{
	OpenGLShaderProgramAPI::SetUniformFloat(GetUniformLocation(p_name), p_value);
}

void Shader::SetUniformVec2(const std::string& p_name, const Maths::Vector2& p_vec2)
{
	OpenGLShaderProgramAPI::SetUniformVec2(GetUniformLocation(p_name), p_vec2.x, p_vec2.y);
}

void Shader::SetUniformVec3(const std::string& p_name, const Maths::Vector3& p_vec3)
{
	OpenGLShaderProgramAPI::SetUniformVec3(GetUniformLocation(p_name), p_vec3.x, p_vec3.y, p_vec3.z);
}

void Shader::SetUniformVec4(const std::string& p_name, const Maths::Vector4& p_vec4)
{
	OpenGLShaderProgramAPI::SetUniformVec4(GetUniformLocation(p_name), p_vec4.x, p_vec4.y, p_vec4.z, p_vec4.w);
}

void Shader::SetUniformMat4(const std::string& p_name, const Maths::Matrix4& p_mat4)
{
	OpenGLShaderProgramAPI::SetUniformMat4(GetUniformLocation(p_name), &p_mat4.data[0]);
}

int Shader::GetUniformInt(const std::string& p_name)
{
	int value = 0;
	OpenGLShaderProgramAPI::GetUniformInt(id, GetUniformLocation(p_name), &value);
	return value;
}

float Shader::GetUniformFloat(const std::string& p_name)
{
	float value = 0.0f;
	OpenGLShaderProgramAPI::GetUniformFloat(id, GetUniformLocation(p_name), &value);
	return value;
}

Maths::Vector2 Shader::GetUniformVec2(const std::string& p_name)
{
	GLfloat values[2]{};
	OpenGLShaderProgramAPI::GetUniformFloatArray(id, GetUniformLocation(p_name), values);
	return reinterpret_cast<Vector2&>(values);
}

Maths::Vector3 Shader::GetUniformVec3(const std::string& p_name)
{
	GLfloat values[3]{};
	OpenGLShaderProgramAPI::GetUniformFloatArray(id, GetUniformLocation(p_name), values);
	return reinterpret_cast<Vector3&>(values);
}

Maths::Vector4 Shader::GetUniformVec4(const std::string& p_name)
{
	GLfloat values[4]{};
	OpenGLShaderProgramAPI::GetUniformFloatArray(id, GetUniformLocation(p_name), values);
	return reinterpret_cast<Vector4&>(values);
}

Maths::Matrix4 Shader::GetUniformMat4(const std::string& p_name)
{
	GLfloat values[16]{};
	OpenGLShaderProgramAPI::GetUniformFloatArray(id, GetUniformLocation(p_name), values);
	return reinterpret_cast<Matrix4&>(values);
}

bool Shader::IsEngineUBOMember(const std::string& p_uniformName)
{
	return p_uniformName.rfind("ubo_", 0) == 0;
}

int32_t Shader::GetUniformLocation(const std::string& name)
{
	if (m_uniformLocationCache.find(name) != m_uniformLocationCache.end())
		return m_uniformLocationCache.at(name);

	int location = OpenGLShaderProgramAPI::GetUniformLocation(id, name);
	if (location == -1)
	{
		location = OpenGLShaderProgramAPI::GetProgramResourceLocation(id, name);
	}

	if (location == -1)
	{
		const auto isKnownUniform = std::any_of(uniforms.begin(), uniforms.end(), [&name](const UniformInfo& uniform)
		{
			return uniform.name == name;
		});

		if (!isKnownUniform)
			NLS_LOG_WARNING("Uniform: '" + name + "' doesn't exist");
	}

	m_uniformLocationCache[name] = location;
	return location;
}

void Shader::QueryUniforms()
{
	uniforms.clear();
	m_reflection.properties.clear();
	const auto numActiveUniforms = OpenGLShaderProgramAPI::GetActiveUniformCount(id);
	std::vector<GLchar> nameData(256);

	for (int unif = 0; unif < numActiveUniforms; ++unif)
	{
		GLint arraySize = 0;
		GLenum type = 0;
		GLsizei actualLength = 0;
		OpenGLShaderProgramAPI::GetActiveUniform(id, unif, static_cast<GLsizei>(nameData.size()), &actualLength, &arraySize, &type, &nameData[0]);
		std::string name(static_cast<char*>(nameData.data()), actualLength);

		if (IsEngineUBOMember(name))
			continue;

		int location = OpenGLShaderProgramAPI::GetUniformLocation(id, name);
		if (location == -1)
		{
			location = OpenGLShaderProgramAPI::GetProgramResourceLocation(id, name);
		}

		std::any defaultValue;

		const auto uniformType = NLS::Render::Backend::ToUniformType(type);

		switch (uniformType)
		{
		case UniformType::UNIFORM_BOOL:
		{
			int value = 0;
			if (location >= 0) OpenGLShaderProgramAPI::GetUniformInt(id, location, &value);
			defaultValue = std::make_any<bool>(value != 0);
			break;
		}
		case UniformType::UNIFORM_INT:
		{
			int value = 0;
			if (location >= 0) OpenGLShaderProgramAPI::GetUniformInt(id, location, &value);
			defaultValue = std::make_any<int>(value);
			break;
		}
		case UniformType::UNIFORM_FLOAT:
		{
			float value = 0.0f;
			if (location >= 0) OpenGLShaderProgramAPI::GetUniformFloat(id, location, &value);
			defaultValue = std::make_any<float>(value);
			break;
		}
		case UniformType::UNIFORM_FLOAT_VEC2:
		{
			GLfloat values[2]{};
			if (location >= 0) OpenGLShaderProgramAPI::GetUniformFloatArray(id, location, values);
			defaultValue = std::make_any<Maths::Vector2>(reinterpret_cast<Vector2&>(values));
			break;
		}
		case UniformType::UNIFORM_FLOAT_VEC3:
		{
			GLfloat values[3]{};
			if (location >= 0) OpenGLShaderProgramAPI::GetUniformFloatArray(id, location, values);
			defaultValue = std::make_any<Maths::Vector3>(reinterpret_cast<Vector3&>(values));
			break;
		}
		case UniformType::UNIFORM_FLOAT_VEC4:
		{
			GLfloat values[4]{};
			if (location >= 0) OpenGLShaderProgramAPI::GetUniformFloatArray(id, location, values);
			defaultValue = std::make_any<Maths::Vector4>(reinterpret_cast<Vector4&>(values));
			break;
		}
		case UniformType::UNIFORM_FLOAT_MAT4:
		{
			GLfloat values[16]{};
			if (location >= 0) OpenGLShaderProgramAPI::GetUniformFloatArray(id, location, values);
			defaultValue = std::make_any<Maths::Matrix4>(reinterpret_cast<Matrix4&>(values));
			break;
		}
		case UniformType::UNIFORM_SAMPLER_2D:
			defaultValue = std::make_any<Texture2D*>(nullptr);
			break;
		case UniformType::UNIFORM_SAMPLER_CUBE:
			defaultValue = std::make_any<TextureCube*>(nullptr);
			break;
		}

		if (defaultValue.has_value())
		{
			uniforms.push_back({
				uniformType,
				name,
				location,
				defaultValue
			});

			m_reflection.properties.push_back({
				name,
				uniformType,
				GetShaderResourceKind(uniformType),
				location,
				arraySize
			});
		}
	}
}

const UniformInfo* Shader::GetUniformInfo(const std::string& p_name) const
{
	auto found = std::find_if(uniforms.begin(), uniforms.end(), [&p_name](const UniformInfo& p_element)
	{
		return p_name == p_element.name;
	});

	if (found != uniforms.end())
		return &*found;
	else
		return nullptr;
}

const ShaderReflection& Shader::GetReflection() const
{
	return m_reflection;
}
}
