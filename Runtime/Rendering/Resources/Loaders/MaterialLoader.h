#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "Rendering/Resources/Material.h"

namespace NLS::Render::Resources::Loaders
{
/**
 * Handle the Texture creation and destruction
 */
class NLS_RENDER_API MaterialLoader
{
public:
    struct LoadOptions
    {
        bool loadMissingTextures = true;
        bool loadMissingShaders = true;
        bool allowSourceAssetNativeContainer = false;
        std::filesystem::path artifactDatabasePath;
    };

    /**
     * Disabled constructor
     */
    MaterialLoader() = delete;

    /**
     * Instantiate a material from a file
     * @param p_path
     */
    static Material* Create(const std::string& p_path);
    static Material* Create(const std::string& p_path, const LoadOptions& options);
    static Material* CreateFromSerializedPayload(
        const std::string& p_path,
        const std::string& p_xml,
        const LoadOptions& options);
    static std::string ReadSerializedPayload(const std::string& p_path);

    /**
     * Reload the material using the given file path
     * @param p_material
     * @param p_path
     */
    static void Reload(Material& p_material, const std::string& p_path);
    static void Reload(Material& p_material, const std::string& p_path, const LoadOptions& options);

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
} // namespace NLS::Render::Resources::Loaders
