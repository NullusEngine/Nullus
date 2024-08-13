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
	class NLS_CORE_API TextureManager : public AResourceManager<NLS::Render::Resources::Texture2D>
	{
	public:
		/**
		* Create the resource identified by the given path
		* @param p_path
		*/
		virtual NLS::Render::Resources::Texture2D* CreateResource(const std::string & p_path) override;

		/**
		* Destroy the given resource
		* @param p_resource
		*/
		virtual void DestroyResource(NLS::Render::Resources::Texture2D* p_resource) override;

		/**
		* Reload the given resource
		* @param p_resource
		* @param p_path
		*/
		virtual void ReloadResource(NLS::Render::Resources::Texture2D* p_resource, const std::string& p_path) override;

		static NLS::Render::Resources::TextureCube* CreateCubeMap(const std::vector<std::string>& filePaths);
	};
}