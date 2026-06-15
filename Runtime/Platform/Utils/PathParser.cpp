#include <algorithm>
#include "Utils/PathParser.h"
#include <cctype>
#include <filesystem>

namespace
{
std::string ToLower(std::string value)
{
	std::transform(
		value.begin(),
		value.end(),
		value.begin(),
		[](const unsigned char character)
		{
			return static_cast<char>(std::tolower(character));
		});
	return value;
}

bool EndsWith(const std::string& value, const std::string& suffix)
{
	return value.size() >= suffix.size() &&
		value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}
}
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
	case NLS::Utils::PathParser::EFileType::PREFAB:		return "Prefab";
	case NLS::Utils::PathParser::EFileType::SCRIPT:		return "Script";
	case NLS::Utils::PathParser::EFileType::FONT:		return "Font";
	case NLS::Utils::PathParser::EFileType::UNKNOWN:
	case NLS::Utils::PathParser::EFileType::COUNT:
		break;
	}

	return "Unknown";
}

NLS::Utils::PathParser::EFileType NLS::Utils::PathParser::GetFileType(const std::string & p_path)
{
	const auto loweredPath = ToLower(MakeNonWindowsStyle(p_path));
	if (EndsWith(loweredPath, ".objectgraph.json"))							return EFileType::SCENE;

	std::string ext = ToLower(GetExtension(p_path));

	if (ext == "fbx" || ext == "obj" || ext == "gltf" || ext == "glb")			return EFileType::MODEL;
	else if (ext == "png" || ext == "jpeg" || ext == "jpg" || ext == "tga" ||
		ext == "bmp" || ext == "dds")											return EFileType::TEXTURE;
	else if (ext == "glsl" || ext == "hlsl" || ext == "shader")					return EFileType::SHADER;
	else if (ext == "mat" || ext == "nmat")										return EFileType::MATERIAL;
	else if (ext == "wav" || ext == "mp3" || ext == "ogg")						return EFileType::SOUND;
	else if (ext == "scene" || ext == "nscene")									return EFileType::SCENE;
	else if (ext == "prefab")													return EFileType::PREFAB;
	else if (ext == "lua" || ext == "cs" || ext == "py")						return EFileType::SCRIPT;
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
