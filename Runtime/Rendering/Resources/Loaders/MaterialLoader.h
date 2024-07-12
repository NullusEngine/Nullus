#pragma once

#include <string>
#include <vector>

#include "Rendering/Data/Material.h"

using namespace NLS::Rendering::Data;
namespace NLS::Rendering::Resources::Loaders
{
/**
 * Handle the Texture creation and destruction
 */
class NLS_RENDER_API MaterialLoader
{
public:
    /**
     * Disabled constructor
     */
    MaterialLoader() = delete;

    /**
     * Instantiate a material from a file
     * @param p_path
     */
    static Material* Create(const std::string& p_path);

    /**
     * Reload the material using the given file path
     * @param p_material
     * @param p_path
     */
    static void Reload(Material& p_material, const std::string& p_path);

    /**
     * Save the material to the given path
     * @param p_material
     * @param p_path
     */
    static void Save(Material& p_material, const std::string& p_path);

    /**
     * Destroy the given material
     * @param p_material
     */
    static bool Destroy(Material*& p_material);
};
} // namespace NLS::Rendering::Resources::Loaders