#pragma once

#include <Rendering/Resources/Loaders/MaterialLoader.h>

#include "Core/ResourceManagement/AResourceManager.h"
#include "CoreDef.h"
#include "Data/Material.h"
namespace NLS::Core::ResourceManagement
{
	/**
	* ResourceManager of textures
	*/
class NLS_CORE_API MaterialManager : public AResourceManager<NLS::Rendering::Data::Material>
	{
	public:
		/**
		* Create the resource identified by the given path
		* @param p_path
		*/
        virtual NLS::Rendering::Data::Material* CreateResource(const std::string& p_path) override;

		/**
		* Destroy the given resource
		* @param p_resource
		*/
        virtual void DestroyResource(NLS::Rendering::Data::Material* p_resource) override;

		/**
		* Reload the given resource
		* @param p_resource
		* @param p_path
		*/
        virtual void ReloadResource(NLS::Rendering::Data::Material* p_resource, const std::string& p_path) override;
	};
}