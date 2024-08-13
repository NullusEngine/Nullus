
#include "Core/ResourceManagement/TextureManager.h"
#include "Rendering/Settings/DriverSettings.h"
#include <Rendering/Resources/Loaders/TextureLoader.h>

#include <Filesystem/IniFile.h>

std::tuple<NLS::Render::Settings::ETextureFilteringMode, NLS::Render::Settings::ETextureFilteringMode, bool> GetTextureMetadata(const std::string& p_path)
{
	auto metaFile = NLS::Filesystem::IniFile(p_path + ".meta");

	auto min = metaFile.GetOrDefault("MIN_FILTER", static_cast<int>(NLS::Render::Settings::ETextureFilteringMode::LINEAR_MIPMAP_LINEAR));
	auto mag = metaFile.GetOrDefault("MAG_FILTER", static_cast<int>(NLS::Render::Settings::ETextureFilteringMode::LINEAR));
	auto mipmap = metaFile.GetOrDefault("ENABLE_MIPMAPPING", true);

	return { static_cast<NLS::Render::Settings::ETextureFilteringMode>(min), static_cast<NLS::Render::Settings::ETextureFilteringMode>(mag), mipmap };
}

NLS::Render::Resources::Texture2D* NLS::Core::ResourceManagement::TextureManager::CreateResource(const std::string & p_path)
{
	std::string realPath = GetRealPath(p_path);

	auto [min, mag, mipmap] = GetTextureMetadata(realPath);

	NLS::Render::Resources::Texture2D* texture = NLS::Render::Resources::Loaders::TextureLoader::Create(realPath, min, mag, mipmap);
	if (texture)
		*reinterpret_cast<std::string*>(reinterpret_cast<char*>(texture) + offsetof(NLS::Render::Resources::Texture2D, path)) = p_path; // Force the resource path to fit the given path

	return texture;
}

void NLS::Core::ResourceManagement::TextureManager::DestroyResource(NLS::Render::Resources::Texture2D* p_resource)
{
	NLS::Render::Resources::Loaders::TextureLoader::Destroy(p_resource);
}

void NLS::Core::ResourceManagement::TextureManager::ReloadResource(NLS::Render::Resources::Texture2D* p_resource, const std::string& p_path)
{
	std::string realPath = GetRealPath(p_path);

	auto [min, mag, mipmap] = GetTextureMetadata(realPath);

	NLS::Render::Resources::Loaders::TextureLoader::Reload(p_resource, realPath, min, mag, mipmap);
}

NLS::Render::Resources::TextureCube* NLS::Core::ResourceManagement::TextureManager::CreateCubeMap(const std::vector<std::string>& filePaths)
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

	return NLS::Render::Resources::Loaders::TextureLoader::CreateCubeMap(realPaths);
}
