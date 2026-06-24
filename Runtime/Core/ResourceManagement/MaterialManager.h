#pragma once

#include <Rendering/Resources/Loaders/MaterialLoader.h>

#include "Core/ResourceManagement/AResourceManager.h"
#include "CoreDef.h"
#include "Resources/Material.h"

#include <unordered_set>

namespace NLS::Core::ResourceManagement
{
	/**
	* ResourceManager of textures
	*/
class NLS_RESOURCE_MANAGEMENT_API MaterialManager : public AResourceManager<Render::Resources::Material>
	{
    public:
        using Material = Render::Resources::Material;

        ~MaterialManager();

			const char* GetResourceTypeName() const override { return "Material"; }

		/**
		* Create the resource identified by the given path
		* @param p_path
		*/
        virtual Material* CreateResource(const std::string& p_path) override;
        Material* CreateResource(const std::string& p_path, const NLS::Render::Resources::Loaders::MaterialLoader::LoadOptions& options);

        static std::string ResolveResourcePath(const std::string& path);

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

        virtual Material* PrewarmArtifact(const std::string& p_path);
        virtual Material* PrewarmArtifactWithDependencies(const std::string& p_path);
        virtual Material* LoadArtifactWithoutTextures(const std::string& p_path);
        virtual Material* RequestAsyncArtifact(const std::string& p_path, bool p_cancelableInterest = false);
        ResourceHandle<Material> AcquireMaterialHandle(
            ResourceLifetimeRegistry& registry,
            const std::string& ownerToken,
            const std::string& path,
            ResourceLifetimeOwnerKind ownerKind = ResourceLifetimeOwnerKind::SceneInstance,
            size_t estimatedBytes = 0u)
        {
            return AcquireResourceHandle(
                registry,
                ResourceLifetimeAcquireRequest {
                    ownerToken,
                    ResourceLifetimeResourceType::Material,
                    path,
                    estimatedBytes,
                    ownerKind });
        }

        size_t TrimUnusedMaterialResources(
            ResourceLifetimeRegistry& registry,
            const ResourceLifetimeTrimOptions& options = {})
        {
            return TrimUnusedResources(
                registry,
                ResourceLifetimeResourceType::Material,
                options);
        }

        void CancelAsyncArtifact(const std::string& p_path);
        bool IsAsyncArtifactLoadPending(const std::string& p_path) const;
        bool IsAsyncArtifactLoadFailed(const std::string& p_path) const;
        void PumpAsyncLoads(size_t p_maxCompletions = 1u);
        void PumpAsyncLoadsForPaths(const std::unordered_set<std::string>& p_paths, size_t p_maxCompletions = 1u);
#if defined(NLS_ENABLE_TEST_HOOKS)
        static void ClearAsyncArtifactRequestStateForTesting();
        static bool WaitForAsyncArtifactWorkersForTesting(uint32_t timeoutMilliseconds = 5000u);
        static size_t GetPendingAsyncArtifactRequestCountForTesting();
        static size_t GetTotalAsyncArtifactRequestCountForTesting();
        static size_t GetFailedAsyncArtifactRequestCountForTesting();
#endif
	};
}
