
#include "Core/ResourceManagement/TextureManager.h"
#include "Rendering/Settings/DriverSettings.h"

#include <Filesystem/IniFile.h>

std::tuple<NLS::Rendering::Settings::ETextureFilteringMode, NLS::Rendering::Settings::ETextureFilteringMode, bool> GetAssetMetadata(const std::string& p_path)
{
	auto metaFile = NLS::Filesystem::IniFile(p_path + ".meta");

	auto min = metaFile.GetOrDefault("MIN_FILTER", static_cast<int>(NLS::Rendering::Settings::ETextureFilteringMode::LINEAR_MIPMAP_LINEAR));
	auto mag = metaFile.GetOrDefault("MAG_FILTER", static_cast<int>(NLS::Rendering::Settings::ETextureFilteringMode::LINEAR));
	auto mipmap = metaFile.GetOrDefault("ENABLE_MIPMAPPING", true);

	return { static_cast<NLS::Rendering::Settings::ETextureFilteringMode>(min), static_cast<NLS::Rendering::Settings::ETextureFilteringMode>(mag), mipmap };
}

NLS::Rendering::Resources::Texture* NLS::Core::ResourceManagement::TextureManager::CreateResource(const std::string & p_path)
{
	std::string realPath = GetRealPath(p_path);

	auto [min, mag, mipmap] = GetAssetMetadata(realPath);

	NLS::Rendering::Resources::Texture* texture = NLS::Rendering::Resources::Loaders::TextureLoader::Create(realPath, min, mag, mipmap);
	if (texture)
		*reinterpret_cast<std::string*>(reinterpret_cast<char*>(texture) + offsetof(NLS::Rendering::Resources::Texture, path)) = p_path; // Force the resource path to fit the given path

	return texture;
}

void NLS::Core::ResourceManagement::TextureManager::DestroyResource(NLS::Rendering::Resources::Texture* p_resource)
{
	NLS::Rendering::Resources::Loaders::TextureLoader::Destroy(p_resource);
}

void NLS::Core::ResourceManagement::TextureManager::ReloadResource(NLS::Rendering::Resources::Texture* p_resource, const std::string& p_path)
{
	std::string realPath = GetRealPath(p_path);

	auto [min, mag, mipmap] = GetAssetMetadata(realPath);

	NLS::Rendering::Resources::Loaders::TextureLoader::Reload(*p_resource, realPath, min, mag, mipmap);
}
