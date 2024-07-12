
#include "Core/ResourceManagement/MaterialManager.h"
NLS::Rendering::Data::Material* NLS::Core::ResourceManagement::MaterialManager::CreateResource(const std::string& p_path)
{
    std::string realPath = GetRealPath(p_path);

    NLS::Rendering::Data::Material* material = NLS::Rendering::Resources::Loaders::MaterialLoader::Create(realPath);

    return material;
}

void NLS::Core::ResourceManagement::MaterialManager::DestroyResource(NLS::Rendering::Data::Material* p_resource)
{
    NLS::Rendering::Resources::Loaders::MaterialLoader::Destroy(p_resource);
}

void NLS::Core::ResourceManagement::MaterialManager::ReloadResource(NLS::Rendering::Data::Material* p_resource, const std::string& p_path)
{
    std::string realPath = GetRealPath(p_path);
    NLS::Rendering::Resources::Loaders::MaterialLoader::Reload(*p_resource, realPath);
}
