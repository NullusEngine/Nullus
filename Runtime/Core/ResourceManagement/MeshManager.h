#pragma once

#include "Core/ResourceManagement/AResourceManager.h"
#include "CoreDef.h"
#include "Rendering/Resources/Mesh.h"

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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
        virtual Mesh* RequestAsyncArtifactForPreview(const std::string& path, bool cancelableInterest = false);
        virtual Mesh* RequestAsyncPreparedArtifactForPreview(
            const std::string& path,
            std::shared_ptr<const std::vector<uint8_t>> payload,
            bool cancelableInterest = false);
        static AsyncArtifactRequestDiagnostics GetAsyncArtifactRequestDiagnostics();
        AsyncArtifactRequestDiagnostics GetAsyncArtifactRequestDiagnosticsForOwner() const;
        Mesh* FindRegisteredMeshByResolvedArtifactPath(const std::string& realPath) const;
        std::optional<std::string> FindRegisteredMeshPathByResolvedArtifactPath(
            const std::string& realPath) const;
        void CancelAsyncArtifact(const std::string& path);
        bool IsAsyncArtifactLoadPending(const std::string& path) const;
        bool IsAsyncArtifactLoadFailed(const std::string& path) const;
        bool IsAsyncArtifactLoadPendingExactPath(const std::string& path) const;
        bool IsAsyncArtifactLoadFailedExactPath(const std::string& path) const;
        void PumpAsyncLoads(size_t maxCompletions = 1u);
        void PumpAsyncLoadsForPaths(
            const std::unordered_set<std::string>& paths,
            size_t maxCompletions = 1u,
            const std::function<bool()>& shouldStop = {},
            bool allowReadyCompletionAfterStop = false);
        void PumpAsyncLoadsForExactPaths(
            const std::unordered_set<std::string>& paths,
            size_t maxCompletions = 1u,
            const std::function<bool()>& shouldStop = {},
            bool allowReadyCompletionAfterStop = false);
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

        ResourceHandle<Mesh> AcquireRegisteredMeshHandle(
            ResourceLifetimeRegistry& registry,
            const std::string& ownerToken,
            const std::string& path,
            ResourceLifetimeOwnerKind ownerKind = ResourceLifetimeOwnerKind::SceneInstance,
            size_t estimatedBytes = 0u)
        {
            return AcquireRegisteredResourceHandle(
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

    protected:
        void OnResourceRegistered(const std::string& path, Mesh* resource) override;
        void OnResourceUnregistered(const std::string& path, Mesh* resource) override;
        void OnResourceMoved(
            const std::string& previousPath,
            const std::string& newPath,
            Mesh* resource) override;
        void OnAllResourcesUnregistered() override;

    private:
        struct MeshPathIndexEntry
        {
            std::string registeredPath;
            Mesh* resource = nullptr;
        };

        void IndexMeshPath(const std::string& path, Mesh* resource);
        void RemoveMeshPathIndexEntry(const std::string& path, Mesh* resource);
        Mesh* RequestAsyncArtifactInternal(
            const std::string& path,
            bool cancelableInterest,
            bool previewPriority,
            std::shared_ptr<const std::vector<uint8_t>> preparedPayload = {});

        mutable std::mutex m_meshPathIndexMutex;
        std::unordered_map<std::string, MeshPathIndexEntry> m_meshPathIndex;
    };
}
