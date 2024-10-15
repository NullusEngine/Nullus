#include <algorithm>
#include "Utils/PathParser.h"
#include <filesystem>
std::string NLS::Utils::PathParser::MakeWindowsStyle(const std::string & p_path)
{
	std::string result;
	result.resize(p_path.size());

	for (size_t i = 0; i < p_path.size(); ++i)
		result[i] = p_path[i] == '/' ? '\\' : p_path[i];

	return result;
}

std::string NLS::Utils::PathParser::MakeNonWindowsStyle(const std::string & p_path)
{
	std::string result;
	result.resize(p_path.size());

	for (size_t i = 0; i < p_path.size(); ++i)
		result[i] = p_path[i] == '\\' ? '/' : p_path[i];

	return result;
}

std::string NLS::Utils::PathParser::GetContainingFolder(const std::string & p_path)
{
	return std::filesystem::path(p_path).parent_path().string();
}

std::string NLS::Utils::PathParser::GetElementName(const std::string & p_path)
{
	return std::filesystem::path(p_path).filename().string();
}

std::string NLS::Utils::PathParser::GetExtension(const std::string & p_path)
{
	std::string result;

	for (auto it = p_path.rbegin(); it != p_path.rend() && *it != '.'; ++it)
		result += *it;

	std::reverse(result.begin(), result.end());

	return result;
}

std::string NLS::Utils::PathParser::FileTypeToString(EFileType p_fileType)
{
	switch (p_fileType)
	{
	case NLS::Utils::PathParser::EFileType::MODEL:		return "Model";
	case NLS::Utils::PathParser::EFileType::TEXTURE:	return "Texture";
	case NLS::Utils::PathParser::EFileType::SHADER:		return "Shader";
	case NLS::Utils::PathParser::EFileType::MATERIAL:	return "Material";
	case NLS::Utils::PathParser::EFileType::SOUND:		return "Sound";
	case NLS::Utils::PathParser::EFileType::SCENE:		return "Scene";
	case NLS::Utils::PathParser::EFileType::SCRIPT:		return "Script";
	case NLS::Utils::PathParser::EFileType::FONT:		return "Font";
	}

	return "Unknown";
}

NLS::Utils::PathParser::EFileType NLS::Utils::PathParser::GetFileType(const std::string & p_path)
{
	std::string ext = GetExtension(p_path);
	std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

	if (ext == "fbx" || ext == "obj" || ext == "gltf" || ext == "glb")			return EFileType::MODEL;
	else if (ext == "png" || ext == "jpeg" || ext == "jpg" || ext == "tga")		return EFileType::TEXTURE;
	else if (ext == "glsl")														return EFileType::SHADER;
	else if (ext == "ovmat")													return EFileType::MATERIAL;
	else if (ext == "wav" || ext == "mp3" || ext == "ogg")						return EFileType::SOUND;
	else if (ext == "scene")													return EFileType::SCENE;
	else if (ext == "lua")														return EFileType::SCRIPT;
	else if (ext == "ttf")														return EFileType::FONT;

	return EFileType::UNKNOWN;
}

std::string NLS::Utils::PathParser::Separator()
{
#if _WIN32
    return "\\";
#else
    return "/";
#endif
}
