#pragma once

#include "Core/ResourceManagement/AResourceManager.h"
#include "CoreDef.h"
#include "Rendering/Resources/Mesh.h"

namespace NLS::Core::ResourceManagement
{
    class NLS_RESOURCE_MANAGEMENT_API MeshManager : public AResourceManager<Render::Resources::Mesh>
    {
    public:
        using Mesh = Render::Resources::Mesh;

        const char* GetResourceTypeName() const override { return "Mesh"; }

        Mesh* CreateResource(const std::string& path) override;
        void DestroyResource(Mesh* resource) override;
        void ReloadResource(Mesh* resource, const std::string& path) override;
        Mesh* PrewarmArtifact(const std::string& path);
        ResourceHandle<Mesh> AcquireMeshHandle(
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
                    ResourceLifetimeResourceType::Mesh,
                    path,
                    estimatedBytes,
                    ownerKind });
        }

        size_t TrimUnusedMeshResources(
            ResourceLifetimeRegistry& registry,
            const ResourceLifetimeTrimOptions& options = {})
        {
            return TrimUnusedResources(
                registry,
                ResourceLifetimeResourceType::Mesh,
                options);
        }

        static std::string ResolveResourcePath(const std::string& path);
        static std::string ResolveArtifactResourcePath(const std::string& path);
        static const std::string& ProjectAssetsRoot();
    };
}
