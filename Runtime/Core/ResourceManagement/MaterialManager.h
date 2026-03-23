#pragma once

#include <Rendering/Resources/Loaders/MaterialLoader.h>

#include "Core/ResourceManagement/AResourceManager.h"
#include "CoreDef.h"
#include "Resources/Material.h"
namespace NLS::Core::ResourceManagement
{
	/**
	* ResourceManager of textures
	*/
class NLS_CORE_API MaterialManager : public AResourceManager<Render::Resources::Material>
	{
	public:
        using Material = Render::Resources::Material;

		/**
		* Create the resource identified by the given path
		* @param p_path
		*/
        virtual Material* CreateResource(const std::string& p_path) override;

		/**
		* Destroy the given resource
		* @param p_resource
		*/
        virtual void DestroyResource(Material* p_resource) override;

		/**
		* Reload the given resource
		* @param p_resource
		* @param p_path
		*/
        virtual void ReloadResource(Material* p_resource, const std::string& p_path) override;
	};
}
