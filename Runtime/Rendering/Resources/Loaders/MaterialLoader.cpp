#include "Rendering/Resources/Loaders/MaterialLoader.h"

NLS::Rendering::Data::Material* NLS::Rendering::Resources::Loaders::MaterialLoader::Create(const std::string& p_path)
{
    return nullptr;
}

void NLS::Rendering::Resources::Loaders::MaterialLoader::Reload(Material& p_material, const std::string& p_path)
{
}

void NLS::Rendering::Resources::Loaders::MaterialLoader::Save(Material& p_material, const std::string& p_path)
{
}

bool NLS::Rendering::Resources::Loaders::MaterialLoader::Destroy(Material*& p_material)
{
    return false;
}
