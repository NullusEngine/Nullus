#pragma once

#include "Core/ResourceManagement/AResourceManager.h"
#include "CoreDef.h"
#include "Rendering/Resources/Mesh.h"

#include <unordered_set>

namespace NLS::Core::ResourceManagement
{
    class NLS_RESOURCE_MANAGEMENT_API MeshManager : public AResourceManager<Render::Resources::Mesh>
    {
    public:
        using Mesh = Render::Resources::Mesh;

        ~MeshManager();

        const char* GetResourceTypeName() const override { return "Mesh"; }

        Mesh* CreateResource(const std::string& path) override;
        void DestroyResource(Mesh* resource) override;
        void ReloadResource(Mesh* resource, const std::string& path) override;
        virtual Mesh* PrewarmArtifact(const std::string& path);
        virtual Mesh* RequestAsyncArtifact(const std::string& path, bool cancelableInterest = false);
        void CancelAsyncArtifact(const std::string& path);
        bool IsAsyncArtifactLoadPending(const std::string& path) const;
        bool IsAsyncArtifactLoadFailed(const std::string& path) const;
        bool IsAsyncArtifactLoadPendingExactPath(const std::string& path) const;
        bool IsAsyncArtifactLoadFailedExactPath(const std::string& path) const;
        void PumpAsyncLoads(size_t maxCompletions = 1u);
        void PumpAsyncLoadsForPaths(const std::unordered_set<std::string>& paths, size_t maxCompletions = 1u);
        void PumpAsyncLoadsForExactPaths(const std::unordered_set<std::string>& paths, size_t maxCompletions = 1u);
#if defined(NLS_ENABLE_TEST_HOOKS)
        static void ClearAsyncArtifactRequestStateForTesting();
        static bool WaitForAsyncArtifactWorkersForTesting(uint32_t timeoutMilliseconds = 5000u);
        static size_t GetMaxPendingAsyncArtifactRequestCountForTesting();
        static size_t GetPendingAsyncArtifactRequestCountForTesting();
        static size_t GetTotalAsyncArtifactRequestCountForTesting();
        static size_t GetFailedAsyncArtifactRequestCountForTesting();
        static void ResetArtifactResourcePathResolutionCountForTesting();
        static size_t GetArtifactResourcePathResolutionCountForTesting();
#endif
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
