#pragma once

#include <Rendering/Resources/Loaders/TextureLoader.h>

#include "Core/ResourceManagement/AResourceManager.h"

namespace NLS::Core::ResourceManagement
{
	/**
	* ResourceManager of textures
	*/
	class TextureManager : public AResourceManager<NLS::Rendering::Resources::Texture>
	{
	public:
		/**
		* Create the resource identified by the given path
		* @param p_path
		*/
		virtual NLS::Rendering::Resources::Texture* CreateResource(const std::string & p_path) override;

		/**
		* Destroy the given resource
		* @param p_resource
		*/
		virtual void DestroyResource(NLS::Rendering::Resources::Texture* p_resource) override;

		/**
		* Reload the given resource
		* @param p_resource
		* @param p_path
		*/
		virtual void ReloadResource(NLS::Rendering::Resources::Texture* p_resource, const std::string& p_path) override;
	};
}