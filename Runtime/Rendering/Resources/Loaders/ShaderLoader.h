#pragma once

#include "Rendering/Resources/Shader.h"

#include <string>

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
		static Shader* Create(const std::string& p_filePath, const std::string& p_projectAssetsPath);
		static void SetDefaultProjectAssetsPath(const std::string& p_projectAssetsPath);

		static std::string GetCacheDatabasePath(
			const std::string& p_filePath,
			const std::string& p_projectAssetsPath = {});

		/**
		* Recompile a shader
		* @param p_shader
		* @param p_filePath
		*/
		static void	Recompile(Shader& p_shader, const std::string& p_filePath);
		static void	Recompile(Shader& p_shader, const std::string& p_filePath, const std::string& p_projectAssetsPath);

		/**
		* Destroy a shader
		* @param p_shader
		*/
		static bool Destroy(Shader*& p_shader);

	private:
		static const std::string& ResolveProjectAssetsPath(const std::string& p_projectAssetsPath);
		static void CopyRuntimeData(Shader& p_destination, const Shader& p_source);
		static Shader* CreateImportedShaderArtifact(const std::string& p_filePath);
		static Shader* CreateHLSLShaderAsset(
			const std::string& p_filePath,
			const std::string& p_projectAssetsPath = {});

		static std::string __FILE_TRACE;
	};
}
