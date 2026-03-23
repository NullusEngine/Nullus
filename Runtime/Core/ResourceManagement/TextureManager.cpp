#include "Core/ResourceManagement/TextureManager.h"
#include "Rendering/Settings/DriverSettings.h"

#include <Rendering/Resources/Loaders/TextureLoader.h>

#include <Filesystem/IniFile.h>

namespace
{
using ETextureFilteringMode = NLS::Render::Settings::ETextureFilteringMode;
using Texture2D = NLS::Render::Resources::Texture2D;
using TextureCube = NLS::Render::Resources::TextureCube;
using TextureLoader = NLS::Render::Resources::Loaders::TextureLoader;

ETextureFilteringMode ParseTextureFilteringMode(int value)
{
	switch (value)
	{
	case 0x2600: return ETextureFilteringMode::NEAREST;
	case 0x2601: return ETextureFilteringMode::LINEAR;
	case 0x2700: return ETextureFilteringMode::NEAREST_MIPMAP_NEAREST;
	case 0x2701: return ETextureFilteringMode::LINEAR_MIPMAP_NEAREST;
	case 0x2702: return ETextureFilteringMode::NEAREST_MIPMAP_LINEAR;
	case 0x2703: return ETextureFilteringMode::LINEAR_MIPMAP_LINEAR;
	default: return static_cast<ETextureFilteringMode>(value);
	}
}

std::tuple<ETextureFilteringMode, ETextureFilteringMode, bool> GetTextureMetadata(const std::string& path)
{
	auto metaFile = NLS::Filesystem::IniFile(path + ".meta");

	auto min = metaFile.GetOrDefault("MIN_FILTER", static_cast<int>(ETextureFilteringMode::LINEAR_MIPMAP_LINEAR));
	auto mag = metaFile.GetOrDefault("MAG_FILTER", static_cast<int>(ETextureFilteringMode::LINEAR));
	auto mipmap = metaFile.GetOrDefault("ENABLE_MIPMAPPING", true);

	return { ParseTextureFilteringMode(min), ParseTextureFilteringMode(mag), mipmap };
}
}

namespace NLS::Core::ResourceManagement
{
Texture2D* TextureManager::CreateResource(const std::string& path)
{
	std::string realPath = GetRealPath(path);

	auto [min, mag, mipmap] = GetTextureMetadata(realPath);

	Texture2D* texture = TextureLoader::Create(realPath, min, mag, mipmap);
	if (texture)
	{
		texture->path = path;
	}

	return texture;
}

void TextureManager::DestroyResource(Texture2D* resource)
{
	TextureLoader::Destroy(resource);
}

void TextureManager::ReloadResource(Texture2D* resource, const std::string& path)
{
	std::string realPath = GetRealPath(path);

	auto [min, mag, mipmap] = GetTextureMetadata(realPath);

	TextureLoader::Reload(resource, realPath, min, mag, mipmap);
}

TextureCube* TextureManager::CreateCubeMap(const std::vector<std::string>& filePaths)
{
	if (filePaths.size() != 6)
	{
		return nullptr;
	}

	std::vector<std::string> realPaths =
	{
		GetRealPath(filePaths[0]),
		GetRealPath(filePaths[1]),
		GetRealPath(filePaths[2]),
		GetRealPath(filePaths[3]),
		GetRealPath(filePaths[4]),
		GetRealPath(filePaths[5])
	};

	return TextureLoader::CreateCubeMap(realPaths);
}
}
