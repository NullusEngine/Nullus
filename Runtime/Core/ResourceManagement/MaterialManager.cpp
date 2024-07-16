
#include "Core/ResourceManagement/MaterialManager.h"
NLS::Render::Resources::Material* NLS::Core::ResourceManagement::MaterialManager::CreateResource(const std::string& p_path)
{
    std::string realPath = GetRealPath(p_path);

    NLS::Render::Resources::Material* material = NLS::Render::Resources::Loaders::MaterialLoader::Create(realPath);

    return material;
}

void NLS::Core::ResourceManagement::MaterialManager::DestroyResource(NLS::Render::Resources::Material* p_resource)
{
    NLS::Render::Resources::Loaders::MaterialLoader::Destroy(p_resource);
}

void NLS::Core::ResourceManagement::MaterialManager::ReloadResource(NLS::Render::Resources::Material* p_resource, const std::string& p_path)
{
    std::string realPath = GetRealPath(p_path);
    NLS::Render::Resources::Loaders::MaterialLoader::Reload(*p_resource, realPath);
}
