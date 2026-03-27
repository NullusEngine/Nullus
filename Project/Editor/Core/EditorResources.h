#pragma once

#include <string>
#include <unordered_map>

#include <ResourceManagement/TextureManager.h>
#include <ResourceManagement/ModelManager.h>
#include <ResourceManagement/ShaderManager.h>
#include <Rendering/Resources/Parsers/EModelParserFlags.h>

namespace NLS::Editor::Core
{
	/**
	* Handle the creation and storage of editor specific resources
	*/
	class EditorResources
	{
	public:
		/**
		* Constructor
		* @param p_editorAssetsPath
		*/
		EditorResources(const std::string& p_editorAssetsPath);

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
		* Returns the model identified by the given string or nullptr on fail
		* @param p_id
		*/
        NLS::Render::Resources::Model* GetModel(const std::string& p_id);

		/**
		* Returns the shader identified by the given string or nullptr on fail
		* @param p_id
		*/
        NLS::Render::Resources::Shader* GetShader(const std::string& p_id);

	private:
        NLS::Render::Resources::Model* LoadModel(const std::string& p_id);
        NLS::Render::Resources::Shader* LoadShader(const std::string& p_id);

	private:
        std::string m_modelsFolder;
        std::string m_shadersFolder;
        NLS::Render::Resources::Parsers::EModelParserFlags m_modelParserFlags =
            NLS::Render::Resources::Parsers::EModelParserFlags::NONE;
        std::unordered_map<std::string, NLS::Render::Resources::Texture2D*> m_textures;
        std::unordered_map<std::string, NLS::Render::Resources::Model*> m_models;
        std::unordered_map<std::string, NLS::Render::Resources::Shader*> m_shaders;
        std::unordered_map<std::string, std::string> m_modelPaths;
        std::unordered_map<std::string, std::string> m_shaderPaths;
	};
}
