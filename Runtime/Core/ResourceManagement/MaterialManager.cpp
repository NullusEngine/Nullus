#include "Core/ResourceManagement/MaterialManager.h"

namespace NLS::Core::ResourceManagement
{
using Material = Render::Resources::Material;
namespace MaterialLoader = Render::Resources::Loaders::MaterialLoader;

Material* MaterialManager::CreateResource(const std::string& path)
{
	std::string realPath = GetRealPath(path);

	Material* material = MaterialLoader::Create(realPath);
	if (material)
	{
		*reinterpret_cast<std::string*>(reinterpret_cast<char*>(material) + offsetof(Material, path)) = path;
	}

	return material;
}

void MaterialManager::DestroyResource(Material* resource)
{
	MaterialLoader::Destroy(resource);
}

void MaterialManager::ReloadResource(Material* resource, const std::string& path)
{
	std::string realPath = GetRealPath(path);
	MaterialLoader::Reload(*resource, realPath);
}
}
