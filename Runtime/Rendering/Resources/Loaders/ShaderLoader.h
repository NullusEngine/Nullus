#pragma once

#include "Rendering/Resources/Shader.h"

namespace NLS::Render::Resources::Loaders
{
	/**
	* Handle the Shader creation and destruction
	*/
	class NLS_RENDER_API ShaderLoader
	{
	public:
		/**
		* Disabled constructor
		*/
		ShaderLoader() = delete;

		/**
		* Create a shader
		* @param p_filePath
		*/
		static Shader* Create(const std::string& p_filePath);

		/**
		* Recompile a shader
		* @param p_shader
		* @param p_filePath
		*/
		static void	Recompile(Shader& p_shader, const std::string& p_filePath);

		/**
		* Destroy a shader
		* @param p_shader
		*/
		static bool Destroy(Shader*& p_shader);

	private:
		static Shader* CreateHLSLShaderAsset(const std::string& p_filePath);

		static std::string __FILE_TRACE;
	};
}
