#pragma once

#include <Rendering/Resources/Loaders/MaterialLoader.h>

#include <mutex>
#include <unordered_map>

#include "Core/ResourceManagement/AResourceManager.h"
#include "CoreDef.h"
#include "Resources/Material.h"
namespace NLS::Core::ResourceManagement
{
	/**
	* ResourceManager of textures
	*/
class NLS_RESOURCE_MANAGEMENT_API MaterialManager : public AResourceManager<Render::Resources::Material>
	{
	public:
        using Material = Render::Resources::Material;

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
        virtual Material* FindRegisteredMaterialByEquivalentArtifactPath(const std::string& p_path);
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
        void ClearShaderReferences(const NLS::Render::Resources::Shader* p_shader);

    protected:
        void OnResourceRegistered(const std::string& p_path, Material* p_resource) override;
        void OnResourceUnregistered(const std::string& p_path, Material* p_resource) override;
        void OnResourceMoved(const std::string& p_previousPath, const std::string& p_newPath, Material* p_resource) override;
        void OnAllResourcesUnregistered() override;

    private:
        void IndexMaterialPath(const std::string& p_path, Material* p_resource);
        void RemoveMaterialPathIndexEntries(const std::string& p_path, Material* p_resource);
        void RebuildMaterialPathIndex();

        mutable std::mutex m_materialPathIndexMutex;
        std::unordered_map<std::string, Material*> m_materialPathIndex;
	};
}
