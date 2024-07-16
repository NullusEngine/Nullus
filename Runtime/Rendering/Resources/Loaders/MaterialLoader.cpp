#include "Rendering/Resources/Loaders/MaterialLoader.h"

NLS::Render::Resources::Material* NLS::Render::Resources::Loaders::MaterialLoader::Create(const std::string& p_path)
{
    return nullptr;
}

void NLS::Render::Resources::Loaders::MaterialLoader::Reload(Material& p_material, const std::string& p_path)
{
}

void NLS::Render::Resources::Loaders::MaterialLoader::Save(Material& p_material, const std::string& p_path)
{
}

bool NLS::Render::Resources::Loaders::MaterialLoader::Destroy(Material*& p_material)
{
    return false;
}
