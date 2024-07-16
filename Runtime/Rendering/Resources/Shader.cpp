
#include <glad/glad.h>

#include <Debug/Logger.h>

#include "Rendering/Resources/Texture.h"
#include "Rendering/Resources/Shader.h"

NLS::Render::Resources::Shader::Shader(const std::string p_path, uint32_t p_id) : path(p_path), id(p_id)
{
	QueryUniforms();
}

NLS::Render::Resources::Shader::~Shader()
{
	glDeleteProgram(id);
}

void NLS::Render::Resources::Shader::Bind() const
{
	glUseProgram(id);
}

void NLS::Render::Resources::Shader::Unbind() const
{
	glUseProgram(0);
}

void NLS::Render::Resources::Shader::SetUniformInt(const std::string& p_name, int p_value)
{
	glUniform1i(GetUniformLocation(p_name), p_value);
}

void NLS::Render::Resources::Shader::SetUniformFloat(const std::string& p_name, float p_value)
{
	glUniform1f(GetUniformLocation(p_name), p_value);
}

void NLS::Render::Resources::Shader::SetUniformVec2(const std::string & p_name, const Maths::Vector2 & p_vec2)
{
	glUniform2f(GetUniformLocation(p_name), p_vec2.x, p_vec2.y);
}

void NLS::Render::Resources::Shader::SetUniformVec3(const std::string& p_name, const Maths::Vector3& p_vec3)
{
	glUniform3f(GetUniformLocation(p_name), p_vec3.x, p_vec3.y, p_vec3.z);
}

void NLS::Render::Resources::Shader::SetUniformVec4(const std::string& p_name, const Maths::Vector4& p_vec4)
{
	glUniform4f(GetUniformLocation(p_name), p_vec4.x, p_vec4.y, p_vec4.z, p_vec4.w);
}

void NLS::Render::Resources::Shader::SetUniformMat4(const std::string& p_name, const Maths::Matrix4& p_mat4)
{
	glUniformMatrix4fv(GetUniformLocation(p_name), 1, GL_TRUE, &p_mat4.data[0]);
}

int NLS::Render::Resources::Shader::GetUniformInt(const std::string& p_name)
{
	int value;
	glGetUniformiv(id, GetUniformLocation(p_name), &value);
	return value;
}

float NLS::Render::Resources::Shader::GetUniformFloat(const std::string& p_name)
{
	float value;
	glGetUniformfv(id, GetUniformLocation(p_name), &value);
	return value;
}

Maths::Vector2 NLS::Render::Resources::Shader::GetUniformVec2(const std::string& p_name)
{
	GLfloat values[2];
	glGetUniformfv(id, GetUniformLocation(p_name), values);
	return reinterpret_cast<Maths::Vector2&>(values);
}

Maths::Vector3 NLS::Render::Resources::Shader::GetUniformVec3(const std::string& p_name)
{
	GLfloat values[3];
	glGetUniformfv(id, GetUniformLocation(p_name), values);
	return reinterpret_cast<Maths::Vector3&>(values);
}

Maths::Vector4 NLS::Render::Resources::Shader::GetUniformVec4(const std::string& p_name)
{
	GLfloat values[4];
	glGetUniformfv(id, GetUniformLocation(p_name), values);
	return reinterpret_cast<Maths::Vector4&>(values);
}

Maths::Matrix4 NLS::Render::Resources::Shader::GetUniformMat4(const std::string& p_name)
{
	GLfloat values[16];
	glGetUniformfv(id, GetUniformLocation(p_name), values);
	return reinterpret_cast<Maths::Matrix4&>(values);
}

bool NLS::Render::Resources::Shader::IsEngineUBOMember(const std::string & p_uniformName)
{
	return p_uniformName.rfind("ubo_", 0) == 0;
}

uint32_t NLS::Render::Resources::Shader::GetUniformLocation(const std::string& name)
{
	if (m_uniformLocationCache.find(name) != m_uniformLocationCache.end())
		return m_uniformLocationCache.at(name);

	const int location = glGetUniformLocation(id, name.c_str());

	if (location == -1)
		NLS_LOG_WARNING("Uniform: '" + name + "' doesn't exist");

	m_uniformLocationCache[name] = location;

	return location;
}

void NLS::Render::Resources::Shader::QueryUniforms()
{
	GLint numActiveUniforms = 0;
	uniforms.clear();
	glGetProgramiv(id, GL_ACTIVE_UNIFORMS, &numActiveUniforms);
	std::vector<GLchar> nameData(256);
	for (int unif = 0; unif < numActiveUniforms; ++unif)
	{
		GLint arraySize = 0;
		GLenum type = 0;
		GLsizei actualLength = 0;
		glGetActiveUniform(id, unif, static_cast<GLsizei>(nameData.size()), &actualLength, &arraySize, &type, &nameData[0]);
		std::string name(static_cast<char*>(nameData.data()), actualLength);

		if (!IsEngineUBOMember(name))
		{
			std::any defaultValue;

			switch (static_cast<UniformType>(type))
			{
			case NLS::Render::Resources::UniformType::UNIFORM_BOOL:			defaultValue = std::make_any<bool>(GetUniformInt(name));					break;
			case NLS::Render::Resources::UniformType::UNIFORM_INT:			defaultValue = std::make_any<int>(GetUniformInt(name));						break;
			case NLS::Render::Resources::UniformType::UNIFORM_FLOAT:		defaultValue = std::make_any<float>(GetUniformFloat(name));					break;
			case NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC2:	defaultValue = std::make_any<Maths::Vector2>(GetUniformVec2(name));		break;
			case NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC3:	defaultValue = std::make_any<Maths::Vector3>(GetUniformVec3(name));		break;
			case NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC4:	defaultValue = std::make_any<Maths::Vector4>(GetUniformVec4(name));		break;
			case NLS::Render::Resources::UniformType::UNIFORM_SAMPLER_2D:	defaultValue = std::make_any<NLS::Render::Resources::Texture*>(nullptr);	break;
			}

			if (defaultValue.has_value())
			{
				uniforms.push_back
				({
					static_cast<UniformType>(type),
					name,
					GetUniformLocation(nameData.data()),
					defaultValue
				});
			}
		}
	}
}

const NLS::Render::Resources::UniformInfo* NLS::Render::Resources::Shader::GetUniformInfo(const std::string& p_name) const
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
