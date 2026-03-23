
#pragma once

#include <Rendering/Resources/Loaders/ModelLoader.h>

#include "Core/ResourceManagement/AResourceManager.h"
#include "CoreDef.h"
namespace NLS::Core::ResourceManagement
{
	/**
	* ResourceManager of models
	*/
	class NLS_CORE_API ModelManager : public AResourceManager<Render::Resources::Model>
	{
	public:
        using Model = Render::Resources::Model;
        using Mesh = Render::Resources::Mesh;

		/**
		* Create the resource identified by the given path
		* @param p_path
		*/
		virtual Model* CreateResource(const std::string & p_path) override;

		Model* CreateResource(const std::string& name, const std::vector<Mesh*>& meshes);

		/**
		* Destroy the given resource
		* @param p_resource
		*/
		virtual void DestroyResource(Model* p_resource) override;

		/**
		* Reload the given resource
		* @param p_resource
		* @param p_path
		*/
		virtual void ReloadResource(Model* p_resource, const std::string& p_path) override;
	};
}
