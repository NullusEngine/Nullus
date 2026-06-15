#pragma once

#include <array>
#include <string>
#include <unordered_map>

#include <ResourceManagement/MeshManager.h>
#include <ResourceManagement/TextureManager.h>
#include <ResourceManagement/ShaderManager.h>

namespace NLS::Editor::Core
{
	/**
	* Handle the creation and storage of editor specific resources
	*/
	class EditorResources
	{
	public:
        struct IconFileOverride
        {
            const char* id;
            const char* fileName;
        };

		/**
		* Constructor
		* @param p_editorAssetsPath
		*/
		EditorResources(const std::string& p_editorAssetsPath, const std::string& p_projectAssetsPath = {});

		/**
		* Destructor
		*/
		~EditorResources();

		/**
		* Returns the file icon identified by the given string or nullptr on fail
		* @param p_filename
		*/
        NLS::Render::Resources::Texture2D* GetFileIcon(const std::string& p_filename);

		/**
		* Returns the texture identified by the given string or nullptr on fail
		* @param p_id
		*/
        NLS::Render::Resources::Texture2D* GetTexture(const std::string& p_id);

		/**
		* Returns the helper mesh identified by the given string or nullptr on fail
		* @param p_id
		*/
        NLS::Render::Resources::Mesh* GetMesh(const std::string& p_id);

		/**
		* Returns the shader identified by the given string or nullptr on fail
		* @param p_id
		*/
        NLS::Render::Resources::Shader* GetShader(const std::string& p_id);

		/**
		* Returns an already-loaded shader without trying to load it.
		* @param p_id
		*/
        NLS::Render::Resources::Shader* GetLoadedShader(const std::string& p_id) const;

        /**
         * Loads editor-owned shaders and helper meshes that are needed by the
         * default editor views before the main window is shown.
         */
        void PreloadStartupResources();

        static const std::array<IconFileOverride, 9>& EditorIconFileOverrides();

	private:
        NLS::Render::Resources::Mesh* LoadMesh(const std::string& p_id);
        NLS::Render::Resources::Shader* LoadShader(const std::string& p_id);

	private:
        std::string m_modelsFolder;
        std::string m_shadersFolder;
        std::string m_projectAssetsPath;
        std::unordered_map<std::string, NLS::Render::Resources::Texture2D*> m_textures;
        std::unordered_map<std::string, NLS::Render::Resources::Mesh*> m_meshes;
        std::unordered_map<std::string, NLS::Render::Resources::Shader*> m_shaders;
        std::unordered_map<std::string, std::string> m_meshPaths;
        std::unordered_map<std::string, std::string> m_shaderPaths;
	};
}
