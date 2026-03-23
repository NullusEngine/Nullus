#pragma once

#include "CoreDef.h"

#include <Rendering/Resources/Texture2D.h>
#include <Rendering/Resources/TextureCube.h>

#include "Core/ResourceManagement/AResourceManager.h"
#include <vector>

namespace NLS::Core::ResourceManagement
{
	/**
	* ResourceManager of textures
	*/
	class NLS_CORE_API TextureManager : public AResourceManager<Render::Resources::Texture2D>
	{
	public:
        using Texture2D = Render::Resources::Texture2D;
        using TextureCube = Render::Resources::TextureCube;

		/**
		* Create the resource identified by the given path
		* @param p_path
		*/
		virtual Texture2D* CreateResource(const std::string & p_path) override;

		/**
		* Destroy the given resource
		* @param p_resource
		*/
		virtual void DestroyResource(Texture2D* p_resource) override;

		/**
		* Reload the given resource
		* @param p_resource
		* @param p_path
		*/
		virtual void ReloadResource(Texture2D* p_resource, const std::string& p_path) override;

		static TextureCube* CreateCubeMap(const std::vector<std::string>& filePaths);
	};
}
