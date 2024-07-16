
#include "Core/ResourceManagement/TextureManager.h"
#include "Rendering/Settings/DriverSettings.h"

#include <Filesystem/IniFile.h>

std::tuple<NLS::Render::Settings::ETextureFilteringMode, NLS::Render::Settings::ETextureFilteringMode, bool> GetTextureMetadata(const std::string& p_path)
{
	auto metaFile = NLS::Filesystem::IniFile(p_path + ".meta");

	auto min = metaFile.GetOrDefault("MIN_FILTER", static_cast<int>(NLS::Render::Settings::ETextureFilteringMode::LINEAR_MIPMAP_LINEAR));
	auto mag = metaFile.GetOrDefault("MAG_FILTER", static_cast<int>(NLS::Render::Settings::ETextureFilteringMode::LINEAR));
	auto mipmap = metaFile.GetOrDefault("ENABLE_MIPMAPPING", true);

	return { static_cast<NLS::Render::Settings::ETextureFilteringMode>(min), static_cast<NLS::Render::Settings::ETextureFilteringMode>(mag), mipmap };
}

NLS::Render::Resources::Texture* NLS::Core::ResourceManagement::TextureManager::CreateResource(const std::string & p_path)
{
	std::string realPath = GetRealPath(p_path);

	auto [min, mag, mipmap] = GetTextureMetadata(realPath);

	NLS::Render::Resources::Texture* texture = NLS::Render::Resources::Loaders::TextureLoader::Create(realPath, min, mag, mipmap);
	if (texture)
		*reinterpret_cast<std::string*>(reinterpret_cast<char*>(texture) + offsetof(NLS::Render::Resources::Texture, path)) = p_path; // Force the resource path to fit the given path

	return texture;
}

void NLS::Core::ResourceManagement::TextureManager::DestroyResource(NLS::Render::Resources::Texture* p_resource)
{
	NLS::Render::Resources::Loaders::TextureLoader::Destroy(p_resource);
}

void NLS::Core::ResourceManagement::TextureManager::ReloadResource(NLS::Render::Resources::Texture* p_resource, const std::string& p_path)
{
	std::string realPath = GetRealPath(p_path);

	auto [min, mag, mipmap] = GetTextureMetadata(realPath);

	NLS::Render::Resources::Loaders::TextureLoader::Reload(*p_resource, realPath, min, mag, mipmap);
}
