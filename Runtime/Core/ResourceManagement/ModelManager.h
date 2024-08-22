
#pragma once

#include <Rendering/Resources/Loaders/ModelLoader.h>

#include "Core/ResourceManagement/AResourceManager.h"
#include "CoreDef.h"
namespace NLS::Core::ResourceManagement
{
	/**
	* ResourceManager of models
	*/
	class NLS_CORE_API ModelManager : public AResourceManager<NLS::Render::Resources::Model>
	{
	public:
		/**
		* Create the resource identified by the given path
		* @param p_path
		*/
		virtual NLS::Render::Resources::Model* CreateResource(const std::string & p_path) override;

		NLS::Render::Resources::Model* CreateResource(const std::string& name, const std::vector<NLS::Render::Resources::Mesh*>& meshes);

		/**
		* Destroy the given resource
		* @param p_resource
		*/
		virtual void DestroyResource(NLS::Render::Resources::Model* p_resource) override;

		/**
		* Reload the given resource
		* @param p_resource
		* @param p_path
		*/
		virtual void ReloadResource(NLS::Render::Resources::Model* p_resource, const std::string& p_path) override;
	};
}