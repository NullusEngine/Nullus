#include <sstream>
#include <fstream>

#include <glad/glad.h>

#include <Debug/Logger.h>

#include "Rendering/Backend/OpenGL/OpenGLShaderProgramAPI.h"
#include "Rendering/Resources/Loaders/ShaderLoader.h"

namespace
{
	using OpenGLShaderProgramAPI = NLS::Render::Backend::OpenGLShaderProgramAPI;
}

namespace NLS::Render::Resources::Loaders
{
std::string ShaderLoader::__FILE_TRACE;

Shader* ShaderLoader::Create(const std::string& p_filePath)
{
	__FILE_TRACE = p_filePath;

	std::pair<std::string, std::string> source = ParseShader(p_filePath);
	uint32_t programID = CreateProgram(source.first, source.second);

	if (programID)
		return new Shader(p_filePath, programID);

	return nullptr;
}

Shader* ShaderLoader::CreateFromSource(const std::string& p_vertexShader, const std::string& p_fragmentShader)
{
	uint32_t programID = CreateProgram(p_vertexShader, p_fragmentShader);

	if (programID)
		return new Shader("", programID);

	return nullptr;
}

void ShaderLoader::Recompile(Shader& p_shader, const std::string& p_filePath)
{
	__FILE_TRACE = p_filePath;

	std::pair<std::string, std::string> source = ParseShader(p_filePath);
	uint32_t newProgram = CreateProgram(source.first, source.second);

	if (newProgram)
	{
		auto& shaderID = const_cast<std::uint32_t&>(p_shader.id);
		OpenGLShaderProgramAPI::DeleteProgram(shaderID);
		shaderID = newProgram;
		p_shader.QueryUniforms();
		NLS_LOG_INFO("[COMPILE] \"" + __FILE_TRACE + "\": Success!");
	}
	else
	{
		NLS_LOG_ERROR("[COMPILE] \"" + __FILE_TRACE + "\": Failed! Previous shader version keept");
	}
}

bool ShaderLoader::Destroy(Shader*& p_shader)
{
	if (p_shader)
	{
		delete p_shader;
		p_shader = nullptr;
		return true;
	}

	return false;
}

std::pair<std::string, std::string> ShaderLoader::ParseShader(const std::string& p_filePath)
{
	std::ifstream stream(p_filePath);

	enum class ShaderType { NONE = -1, VERTEX = 0, FRAGMENT = 1 };

	std::string line;
	std::stringstream ss[2];
	ShaderType type = ShaderType::NONE;

	while (std::getline(stream, line))
	{
		if (line.find("#shader") != std::string::npos)
		{
			if (line.find("vertex") != std::string::npos)			type = ShaderType::VERTEX;
			else if (line.find("fragment") != std::string::npos)	type = ShaderType::FRAGMENT;
		}
		else if (type != ShaderType::NONE)
		{
			ss[static_cast<int>(type)] << line << '\n';
		}
	}

	return {
		ss[static_cast<int>(ShaderType::VERTEX)].str(),
		ss[static_cast<int>(ShaderType::FRAGMENT)].str()
	};
}

uint32_t ShaderLoader::CreateProgram(const std::string& p_vertexShader, const std::string& p_fragmentShader)
{
	const uint32_t program = OpenGLShaderProgramAPI::CreateProgram();
	const uint32_t vs = CompileShader(GL_VERTEX_SHADER, p_vertexShader);
	const uint32_t fs = CompileShader(GL_FRAGMENT_SHADER, p_fragmentShader);

	if (vs == 0 || fs == 0)
		return 0;

	OpenGLShaderProgramAPI::AttachShader(program, vs);
	OpenGLShaderProgramAPI::AttachShader(program, fs);
	OpenGLShaderProgramAPI::LinkProgram(program);

	if (OpenGLShaderProgramAPI::GetLinkStatus(program) == GL_FALSE)
	{
		GLsizei maxLength = static_cast<GLsizei>(OpenGLShaderProgramAPI::GetProgramInfoLogLength(program));
		std::string errorLog(maxLength, ' ');
		OpenGLShaderProgramAPI::GetProgramInfoLog(program, maxLength, &maxLength, errorLog.data());
		NLS_LOG_ERROR("[LINK] \"" + __FILE_TRACE + "\":\n" + errorLog);
		OpenGLShaderProgramAPI::DeleteProgram(program);
		return 0;
	}

	OpenGLShaderProgramAPI::ValidateProgram(program);
	OpenGLShaderProgramAPI::DeleteShader(vs);
	OpenGLShaderProgramAPI::DeleteShader(fs);
	return program;
}

uint32_t ShaderLoader::CompileShader(uint32_t p_type, const std::string& p_source)
{
	const uint32_t id = OpenGLShaderProgramAPI::CreateShader(p_type);
	const char* src = p_source.c_str();

	OpenGLShaderProgramAPI::SetShaderSource(id, src);
	OpenGLShaderProgramAPI::CompileShader(id);

	if (OpenGLShaderProgramAPI::GetShaderCompileStatus(id) == GL_FALSE)
	{
		GLsizei maxLength = static_cast<GLsizei>(OpenGLShaderProgramAPI::GetShaderInfoLogLength(id));
		std::string errorLog(maxLength, ' ');
		OpenGLShaderProgramAPI::GetShaderInfoLog(id, maxLength, &maxLength, errorLog.data());

		std::string shaderTypeString = p_type == GL_VERTEX_SHADER ? "VERTEX SHADER" : "FRAGMENT SHADER";
		std::string errorHeader = "[" + shaderTypeString + "] \"";
		NLS_LOG_ERROR(errorHeader + __FILE_TRACE + "\":\n" + errorLog);

		OpenGLShaderProgramAPI::DeleteShader(id);
		return 0;
	}

	return id;
}
}
