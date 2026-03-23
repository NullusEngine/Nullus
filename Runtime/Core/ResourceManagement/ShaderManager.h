#pragma once

#include <Rendering/Resources/Loaders/ShaderLoader.h>

#include "Core/ResourceManagement/AResourceManager.h"
#include "CoreDef.h"
namespace NLS::Core::ResourceManagement
{
	/**
	* ResourceManager of shaders
	*/
	class NLS_CORE_API ShaderManager : public AResourceManager<Render::Resources::Shader>
	{
	public:
        using Shader = Render::Resources::Shader;

		/**
		* Create the resource identified by the given path
		* @param p_path
		*/
		virtual Shader* CreateResource(const std::string & p_path) override;

		/**
		* Destroy the given resource
		* @param p_resource
		*/
		virtual void DestroyResource(Shader* p_resource) override;

		/**
		* Reload the given resource
		* @param p_resource
		* @param p_path
		*/
		virtual void ReloadResource(Shader* p_resource, const std::string& p_path) override;
	};
}
