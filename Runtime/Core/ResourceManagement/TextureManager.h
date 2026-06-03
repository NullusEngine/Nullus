#pragma once

#include "CoreDef.h"

#include <Rendering/Resources/Texture2D.h>
#include <Rendering/Resources/TextureCube.h>

#include "Core/ResourceManagement/AResourceManager.h"
#include <unordered_set>
#include <vector>

namespace NLS::Core::ResourceManagement
{
	/**
	* ResourceManager of textures
	*/
	class NLS_RESOURCE_MANAGEMENT_API TextureManager : public AResourceManager<Render::Resources::Texture2D>
	{
	public:
        using Texture2D = Render::Resources::Texture2D;
        using TextureCube = Render::Resources::TextureCube;

		const char* GetResourceTypeName() const override { return "Texture"; }

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

		Texture2D* RequestAsyncArtifact(const std::string& p_path, bool p_cancelableInterest = false);
		void CancelAsyncArtifact(const std::string& p_path);
		bool IsAsyncArtifactLoadPending(const std::string& p_path) const;
		bool IsAsyncArtifactLoadFailed(const std::string& p_path) const;
		void PumpAsyncLoads(size_t p_maxCompletions = 1u);
		void PumpAsyncLoadsForPaths(const std::unordered_set<std::string>& p_paths, size_t p_maxCompletions = 1u);

        static std::string ResolveResourcePath(const std::string& p_path);

		static TextureCube* CreateCubeMap(const std::vector<std::string>& filePaths);
	};
}
