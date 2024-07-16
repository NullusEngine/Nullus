#pragma once

#include <Rendering/Resources/Loaders/ShaderLoader.h>

#include "Core/ResourceManagement/AResourceManager.h"
#include "CoreDef.h"
namespace NLS::Core::ResourceManagement
{
	/**
	* ResourceManager of shaders
	*/
	class NLS_CORE_API ShaderManager : public AResourceManager<NLS::Render::Resources::Shader>
	{
	public:
		/**
		* Create the resource identified by the given path
		* @param p_path
		*/
		virtual NLS::Render::Resources::Shader* CreateResource(const std::string & p_path) override;

		/**
		* Destroy the given resource
		* @param p_resource
		*/
		virtual void DestroyResource(NLS::Render::Resources::Shader* p_resource) override;

		/**
		* Reload the given resource
		* @param p_resource
		* @param p_path
		*/
		virtual void ReloadResource(NLS::Render::Resources::Shader* p_resource, const std::string& p_path) override;
	};
}