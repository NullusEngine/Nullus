#include <gtest/gtest.h>

#include "Core/ResourceManagement/ResourceHandle.h"
#include "Core/ResourceManagement/AResourceManager.h"
#include "Core/ResourceManagement/MaterialManager.h"
#include "Core/ResourceManagement/MeshManager.h"
#include "Core/ResourceManagement/ResourceLifetimeRegistry.h"
#include "Core/ResourceManagement/TextureManager.h"
#include "Assets/ArtifactLoadTelemetry.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

using namespace NLS::Core::ResourceManagement;
using namespace NLS::Core::Assets;

namespace
{
class TestIntResourceManager final : public AResourceManager<int>
{
public:
    std::vector<std::string> destroyedPaths;

    ResourceHandle<int> AcquireIntHandle(
        ResourceLifetimeRegistry& registry,
        const std::string& ownerToken,
        const std::string& path)
    {
        return AcquireResourceHandle(
            registry,
            ResourceLifetimeAcquireRequest {
                ownerToken,
                ResourceLifetimeResourceType::Mesh,
                path,
                sizeof(int),
                ResourceLifetimeOwnerKind::SceneInstance });
    }

    size_t TrimMeshes(ResourceLifetimeRegistry& registry, const ResourceLifetimeTrimOptions& options = {})
    {
        return TrimUnusedResources(registry, ResourceLifetimeResourceType::Mesh, options);
    }

protected:
    int* CreateResource(const std::string& path) override
    {
        return new int(static_cast<int>(path.size()));
    }

    void DestroyResource(int* resource) override
    {
        delete resource;
    }

    void DestroyResourceForPath(const std::string& path, int* resource) override
    {
        destroyedPaths.push_back(path);
        DestroyResource(resource);
    }

    void ReloadResource(int* resource, const std::string& path) override
    {
        if (resource != nullptr)
            *resource = static_cast<int>(path.size());
    }
};
}

TEST(ResourceLifetimeRegistryTests, SharedOwnersPreventPrematureUnload)
{
    ResourceLifetimeRegistry registry;

    registry.Acquire({
        "scene:prefab-a",
        ResourceLifetimeResourceType::Mesh,
        "Library/Artifacts/model/body.nmesh",
        4096u,
        ResourceLifetimeOwnerKind::SceneInstance});
    registry.Acquire({
        "preview:hover-a",
        ResourceLifetimeResourceType::Mesh,
        "D:/Project/Library/Artifacts/model/body.nmesh",
        4096u,
        ResourceLifetimeOwnerKind::Preview});

    EXPECT_EQ(
        registry.GetActiveOwnerCount(
            ResourceLifetimeResourceType::Mesh,
            "Library/Artifacts/model/body.nmesh"),
        2u);

    registry.ReleaseOwner("preview:hover-a");

    EXPECT_TRUE(registry.HasActiveOwners(
        ResourceLifetimeResourceType::Mesh,
        "Library/Artifacts/model/body.nmesh"));
    EXPECT_TRUE(registry.CollectTrimCandidates({}).empty())
        << "Preview cancellation must not unload resources still owned by a committed scene instance.";

    registry.ReleaseOwner("scene:prefab-a");

    auto candidates = registry.CollectTrimCandidates({});
    ASSERT_EQ(candidates.size(), 1u);
    EXPECT_EQ(candidates.front().type, ResourceLifetimeResourceType::Mesh);
    EXPECT_EQ(candidates.front().normalizedPath, "Library/Artifacts/model/body.nmesh");
}

TEST(ResourceLifetimeRegistryTests, PreviewCancelCleanupDoesNotReleaseScenePrefabOwners)
{
    ResourceLifetimeRegistry registry;
    const auto meshPath = std::string("Library/Artifacts/model/shared-body.nmesh");
    const auto materialPath = std::string("Library/Artifacts/model/shared-body.nmat");
    const auto texturePath = std::string("Library/Artifacts/model/shared-albedo.ntex");

    registry.Acquire({
        "scene:prefab-instance",
        ResourceLifetimeResourceType::Mesh,
        meshPath,
        4096u,
        ResourceLifetimeOwnerKind::SceneInstance});
    registry.Acquire({
        "scene:prefab-instance",
        ResourceLifetimeResourceType::Material,
        materialPath,
        1024u,
        ResourceLifetimeOwnerKind::SceneInstance});
    registry.Acquire({
        "scene:prefab-instance",
        ResourceLifetimeResourceType::Texture,
        texturePath,
        8192u,
        ResourceLifetimeOwnerKind::SceneInstance});

    registry.Acquire({
        "preview:hover",
        ResourceLifetimeResourceType::Mesh,
        meshPath,
        4096u,
        ResourceLifetimeOwnerKind::Preview});
    registry.Acquire({
        "preview:hover",
        ResourceLifetimeResourceType::Material,
        materialPath,
        1024u,
        ResourceLifetimeOwnerKind::Preview});
    registry.Acquire({
        "preview:hover",
        ResourceLifetimeResourceType::Texture,
        texturePath,
        8192u,
        ResourceLifetimeOwnerKind::Preview});

    registry.ReleaseOwner("preview:hover");

    EXPECT_EQ(registry.GetActiveOwnerCount(ResourceLifetimeResourceType::Mesh, meshPath), 1u);
    EXPECT_EQ(registry.GetActiveOwnerCount(ResourceLifetimeResourceType::Material, materialPath), 1u);
    EXPECT_EQ(registry.GetActiveOwnerCount(ResourceLifetimeResourceType::Texture, texturePath), 1u);
    EXPECT_TRUE(registry.CollectTrimCandidates({}).empty())
        << "Cancelling a drag preview that shares renderer resources with a saved prefab must not make those resources trim-eligible.";

    registry.ReleaseOwner("scene:prefab-instance");
    EXPECT_EQ(registry.CollectTrimCandidates({}).size(), 3u);
}

TEST(ResourceLifetimeRegistryTests, TrimCandidatesUseDeterministicLruOrderAndBudget)
{
    ResourceLifetimeRegistry registry;

    registry.Acquire({
        "owner:old",
        ResourceLifetimeResourceType::Material,
        "Library/Artifacts/model/old.nmat",
        128u,
        ResourceLifetimeOwnerKind::Preview});
    registry.ReleaseOwner("owner:old");

    registry.Acquire({
        "owner:new",
        ResourceLifetimeResourceType::Texture,
        "Library/Artifacts/model/new.ntex",
        256u,
        ResourceLifetimeOwnerKind::Preview});
    registry.ReleaseOwner("owner:new");

    ResourceLifetimeTrimOptions options;
    options.maxCandidates = 1u;
    options.maxBytes = 1024u;

    auto candidates = registry.CollectTrimCandidates(options);
    ASSERT_EQ(candidates.size(), 1u);
    EXPECT_EQ(candidates.front().type, ResourceLifetimeResourceType::Material);
    EXPECT_EQ(candidates.front().normalizedPath, "Library/Artifacts/model/old.nmat");
}

TEST(ResourceLifetimeRegistryTests, ResourceHandleReleasesOwnerReferenceOnResetAndDestruction)
{
    ResourceLifetimeRegistry registry;
    int resource = 7;

    {
        ResourceHandle<int> handle(
            registry,
            ResourceLifetimeAcquireRequest {
                "preview:handle",
                ResourceLifetimeResourceType::Mesh,
                "Library/Artifacts/model/handle.nmesh",
                sizeof(resource),
                ResourceLifetimeOwnerKind::Preview },
            [&resource](const ResourceId& id) -> int*
            {
                return id.generation == 1u ? &resource : nullptr;
            });

        ASSERT_TRUE(handle);
        EXPECT_EQ(handle.Get(), &resource);
        EXPECT_TRUE(registry.HasActiveOwners(
            ResourceLifetimeResourceType::Mesh,
            "Library/Artifacts/model/handle.nmesh"));

        auto moved = std::move(handle);
        EXPECT_FALSE(handle);
        EXPECT_EQ(moved.Get(), &resource);
        moved.Reset();
        EXPECT_FALSE(registry.HasActiveOwners(
            ResourceLifetimeResourceType::Mesh,
            "Library/Artifacts/model/handle.nmesh"));
    }

    EXPECT_FALSE(registry.HasActiveOwners(
        ResourceLifetimeResourceType::Mesh,
        "Library/Artifacts/model/handle.nmesh"));
}

TEST(ResourceLifetimeRegistryTests, ResourceHandleCountsRepeatedSameOwnerAcquires)
{
    ResourceLifetimeRegistry registry;
    int resource = 11;

    ResourceLifetimeAcquireRequest request {
        "scene:shared-handle",
        ResourceLifetimeResourceType::Mesh,
        "Library/Artifacts/model/shared-handle.nmesh",
        sizeof(resource),
        ResourceLifetimeOwnerKind::SceneInstance };

    ResourceHandle<int> first(
        registry,
        request,
        [&resource](const ResourceId&) -> int*
        {
            return &resource;
        });
    ResourceHandle<int> second(
        registry,
        request,
        [&resource](const ResourceId&) -> int*
        {
            return &resource;
        });

    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(registry.GetActiveOwnerCount(
        ResourceLifetimeResourceType::Mesh,
        "Library/Artifacts/model/shared-handle.nmesh"), 2u);

    first.Reset();

    EXPECT_TRUE(registry.HasActiveOwners(
        ResourceLifetimeResourceType::Mesh,
        "Library/Artifacts/model/shared-handle.nmesh"))
        << "Resetting one handle must not release another live handle with the same owner token and resource.";
    EXPECT_EQ(registry.GetActiveOwnerCount(
        ResourceLifetimeResourceType::Mesh,
        "Library/Artifacts/model/shared-handle.nmesh"), 1u);

    second.Reset();
    EXPECT_FALSE(registry.HasActiveOwners(
        ResourceLifetimeResourceType::Mesh,
        "Library/Artifacts/model/shared-handle.nmesh"));
}

TEST(ResourceLifetimeRegistryTests, ResourceHandleReturnsNullAfterGenerationInvalidation)
{
    ResourceLifetimeRegistry registry;
    int first = 1;
    int second = 2;

    ResourceHandle<int> stale(
        registry,
        ResourceLifetimeAcquireRequest {
            "scene:stale",
            ResourceLifetimeResourceType::Texture,
            "Library/Artifacts/model/albedo.ntex",
            sizeof(first),
            ResourceLifetimeOwnerKind::SceneInstance },
        [&first](const ResourceId& id) -> int*
        {
            return id.generation == 1u ? &first : nullptr;
        });
    ASSERT_EQ(stale.Get(), &first);

    registry.InvalidateResource(
        ResourceLifetimeResourceType::Texture,
        "Library/Artifacts/model/albedo.ntex");

    EXPECT_EQ(stale.Get(), nullptr);

    ResourceHandle<int> fresh(
        registry,
        ResourceLifetimeAcquireRequest {
            "scene:fresh",
            ResourceLifetimeResourceType::Texture,
            "Library/Artifacts/model/albedo.ntex",
            sizeof(second),
            ResourceLifetimeOwnerKind::SceneInstance },
        [&second](const ResourceId& id) -> int*
        {
            return id.generation == 2u ? &second : nullptr;
        });

    EXPECT_EQ(fresh.Get(), &second);
}

TEST(ResourceLifetimeRegistryTests, StaleHandleResetDoesNotReleaseFreshGenerationOwner)
{
    ResourceLifetimeRegistry registry;
    int first = 1;
    int second = 2;

    ResourceLifetimeAcquireRequest request {
        "scene:generation-owner",
        ResourceLifetimeResourceType::Texture,
        "Library/Artifacts/model/generation.ntex",
        sizeof(first),
        ResourceLifetimeOwnerKind::SceneInstance };

    ResourceHandle<int> stale(
        registry,
        request,
        [&first](const ResourceId& id) -> int*
        {
            return id.generation == 1u ? &first : nullptr;
        });
    ASSERT_EQ(stale.Get(), &first);

    registry.InvalidateResource(
        ResourceLifetimeResourceType::Texture,
        "Library/Artifacts/model/generation.ntex");

    ResourceHandle<int> fresh(
        registry,
        request,
        [&second](const ResourceId& id) -> int*
        {
            return id.generation == 2u ? &second : nullptr;
        });
    ASSERT_EQ(fresh.Get(), &second);

    stale.Reset();

    EXPECT_EQ(fresh.Get(), &second);
    EXPECT_TRUE(registry.HasActiveOwners(
        ResourceLifetimeResourceType::Texture,
        "Library/Artifacts/model/generation.ntex"))
        << "A stale generation handle reset must not release the current generation owner reference.";
    EXPECT_EQ(registry.GetActiveOwnerCount(
        ResourceLifetimeResourceType::Texture,
        "Library/Artifacts/model/generation.ntex"), 1u);
}

TEST(ResourceLifetimeRegistryTests, ConcurrentAcquireReleaseKeepsRegistryConsistent)
{
    ResourceLifetimeRegistry registry;
    constexpr auto path = "Library/Artifacts/model/concurrent.nmesh";
    constexpr size_t threadCount = 8u;
    constexpr size_t iterations = 1000u;
    std::atomic<bool> failed { false };
    std::vector<std::thread> workers;

    for (size_t threadIndex = 0u; threadIndex < threadCount; ++threadIndex)
    {
        workers.emplace_back(
            [&registry, &failed, threadIndex]()
            {
                for (size_t iteration = 0u; iteration < iterations; ++iteration)
                {
                    const auto owner = "preview:thread-" + std::to_string(threadIndex) +
                        ":" + std::to_string(iteration);
                    const auto id = registry.Acquire({
                        owner,
                        ResourceLifetimeResourceType::Mesh,
                        path,
                        sizeof(int),
                        ResourceLifetimeOwnerKind::Preview });
                    if (id.normalizedPath != path)
                        failed = true;
                    registry.ReleaseOwner(owner);
                }
            });
    }

    for (auto& worker : workers)
        worker.join();

    EXPECT_FALSE(failed.load());
    EXPECT_FALSE(registry.HasActiveOwners(ResourceLifetimeResourceType::Mesh, path));
    const auto candidates = registry.CollectTrimCandidates({});
    ASSERT_EQ(candidates.size(), 1u);
    EXPECT_EQ(candidates.front().normalizedPath, path);
}

TEST(ResourceLifetimeRegistryTests, ReleaseOwnersByKindOnlyReleasesMatchingOwnerKind)
{
    ResourceLifetimeRegistry registry;

    registry.Acquire({
        "scene:instance-a",
        ResourceLifetimeResourceType::Mesh,
        "Library/Artifacts/model/shared.nmesh",
        1024u,
        ResourceLifetimeOwnerKind::SceneInstance });
    registry.Acquire({
        "preview:hover-a",
        ResourceLifetimeResourceType::Mesh,
        "D:/Project/Library/Artifacts/model/shared.nmesh",
        1024u,
        ResourceLifetimeOwnerKind::Preview });
    registry.Acquire({
        "scene:instance-b",
        ResourceLifetimeResourceType::Material,
        "Library/Artifacts/model/body.nmat",
        512u,
        ResourceLifetimeOwnerKind::SceneInstance });

    registry.ReleaseOwnersByKind(ResourceLifetimeOwnerKind::SceneInstance);

    EXPECT_EQ(registry.GetActiveOwnerCount(
        ResourceLifetimeResourceType::Mesh,
        "Library/Artifacts/model/shared.nmesh"), 1u)
        << "Scene unload should not release unrelated preview/inspector owners.";
    EXPECT_TRUE(registry.HasActiveOwners(
        ResourceLifetimeResourceType::Mesh,
        "Library/Artifacts/model/shared.nmesh"));
    EXPECT_FALSE(registry.HasActiveOwners(
        ResourceLifetimeResourceType::Material,
        "Library/Artifacts/model/body.nmat"));

    registry.ReleaseOwner("preview:hover-a");
    auto candidates = registry.CollectTrimCandidates({});
    ASSERT_EQ(candidates.size(), 2u);
}

TEST(ResourceLifetimeRegistryTests, ResourceLifetimeTelemetryRecordsAcquireReleaseTrimSkipAndEviction)
{
    ClearArtifactLoadTelemetry();

    ResourceLifetimeRegistry registry;
    TestIntResourceManager manager;

    auto sceneHandle = manager.AcquireIntHandle(
        registry,
        "scene:telemetry",
        "Library/Artifacts/model/telemetry.nmesh");
    ASSERT_TRUE(sceneHandle);

    auto telemetry = SnapshotArtifactLoadTelemetry();
    EXPECT_NE(std::find_if(
        telemetry.begin(),
        telemetry.end(),
        [](const ArtifactLoadTelemetryRecord& record)
        {
            return record.stage == ArtifactLoadTelemetryStage::LifetimeAcquire &&
                record.path == "Library/Artifacts/model/telemetry.nmesh" &&
                record.byteCount == sizeof(int);
        }), telemetry.end());

    EXPECT_EQ(manager.TrimMeshes(registry), 0u);
    telemetry = SnapshotArtifactLoadTelemetry();
    EXPECT_NE(std::find_if(
        telemetry.begin(),
        telemetry.end(),
        [](const ArtifactLoadTelemetryRecord& record)
        {
            return record.stage == ArtifactLoadTelemetryStage::LifetimeTrimSkip &&
                record.path == "Library/Artifacts/model/telemetry.nmesh" &&
                record.byteCount == sizeof(int);
        }), telemetry.end())
        << "Active scene owners should explain why trim skipped a registered resource.";

    sceneHandle.Reset();
    telemetry = SnapshotArtifactLoadTelemetry();
    EXPECT_NE(std::find_if(
        telemetry.begin(),
        telemetry.end(),
        [](const ArtifactLoadTelemetryRecord& record)
        {
            return record.stage == ArtifactLoadTelemetryStage::LifetimeRelease &&
                record.path == "Library/Artifacts/model/telemetry.nmesh";
        }), telemetry.end());

    EXPECT_EQ(manager.TrimMeshes(registry), 1u);
    telemetry = SnapshotArtifactLoadTelemetry();
    EXPECT_NE(std::find_if(
        telemetry.begin(),
        telemetry.end(),
        [](const ArtifactLoadTelemetryRecord& record)
        {
            return record.stage == ArtifactLoadTelemetryStage::Eviction &&
                record.path == "Library/Artifacts/model/telemetry.nmesh";
        }), telemetry.end())
        << "Last-owner release plus trim should emit eviction telemetry for resource cleanup investigations.";
}

TEST(ResourceLifetimeRegistryTests, ResourceManagerAcquireHandleAndTrimUnusedResources)
{
    ResourceLifetimeRegistry registry;
    TestIntResourceManager manager;

    auto sceneHandle = manager.AcquireIntHandle(
        registry,
        "scene:manager",
        "Library/Artifacts/model/managed.nmesh");
    ASSERT_TRUE(sceneHandle);
    EXPECT_TRUE(manager.IsResourceRegistered("Library/Artifacts/model/managed.nmesh"));
    EXPECT_EQ(manager.TrimMeshes(registry), 0u)
        << "Active scene owners must prevent manager trim from unloading resources.";

    sceneHandle.Reset();
    EXPECT_FALSE(registry.HasActiveOwners(
        ResourceLifetimeResourceType::Mesh,
        "Library/Artifacts/model/managed.nmesh"));

    EXPECT_EQ(manager.TrimMeshes(registry), 1u);
    EXPECT_FALSE(manager.IsResourceRegistered("Library/Artifacts/model/managed.nmesh"));
    ASSERT_EQ(manager.destroyedPaths.size(), 1u);
    EXPECT_EQ(manager.destroyedPaths.front(), "Library/Artifacts/model/managed.nmesh");
}

TEST(ResourceLifetimeRegistryTests, SceneSwitchRestoreReacquireKeepsPrefabResourcesOutOfTrimCandidates)
{
    ResourceLifetimeRegistry registry;

    constexpr auto meshPath = "Library/Artifacts/model/restored-body.nmesh";
    constexpr auto materialPath = "Library/Artifacts/model/restored-body.nmat";
    const auto previousSceneOwner = "scene-prefab:model:default:old-root";
    const auto restoredSceneOwner = "scene-prefab:model:default:new-root";

    registry.Acquire({
        previousSceneOwner,
        ResourceLifetimeResourceType::Mesh,
        meshPath,
        1024u,
        ResourceLifetimeOwnerKind::SceneInstance });
    registry.Acquire({
        previousSceneOwner,
        ResourceLifetimeResourceType::Material,
        materialPath,
        512u,
        ResourceLifetimeOwnerKind::SceneInstance });

    registry.ReleaseOwnersByKind(ResourceLifetimeOwnerKind::SceneInstance);
    EXPECT_EQ(registry.CollectTrimCandidates({}).size(), 2u)
        << "Scene unload should make old-scene prefab resources eligible for the next budgeted trim.";

    registry.Acquire({
        restoredSceneOwner,
        ResourceLifetimeResourceType::Mesh,
        meshPath,
        1024u,
        ResourceLifetimeOwnerKind::SceneInstance });
    registry.Acquire({
        restoredSceneOwner,
        ResourceLifetimeResourceType::Material,
        materialPath,
        512u,
        ResourceLifetimeOwnerKind::SceneInstance });

    EXPECT_TRUE(registry.CollectTrimCandidates({}).empty())
        << "Scene load fast-return restore must reacquire scene owners before delayed trim can evict visible prefab resources.";
    EXPECT_TRUE(registry.HasActiveOwners(ResourceLifetimeResourceType::Mesh, meshPath));
    EXPECT_TRUE(registry.HasActiveOwners(ResourceLifetimeResourceType::Material, materialPath));
}

TEST(ResourceLifetimeRegistryTests, ResourceManagerTrimTreatsAbsoluteLibraryPathAsRegisteredAlias)
{
    ResourceLifetimeRegistry registry;
    TestIntResourceManager manager;

    constexpr auto libraryPath = "Library/Artifacts/model/alias.nmesh";
    constexpr auto absolutePath = "D:/Project/Library/Artifacts/model/alias.nmesh";

    manager.RegisterResource(absolutePath, new int(42));
    auto handle = manager.AcquireIntHandle(
        registry,
        "scene:absolute-alias",
        absolutePath);
    ASSERT_TRUE(handle);
    handle.Reset();

    EXPECT_EQ(manager.TrimMeshes(registry), 1u)
        << "Lifetime-managed registry trim candidates are normalized to Library/... and must still unload manager entries registered by absolute artifact path.";
    EXPECT_FALSE(manager.IsResourceRegistered(absolutePath));
    ASSERT_EQ(manager.destroyedPaths.size(), 1u);
    EXPECT_EQ(manager.destroyedPaths.front(), absolutePath);
}

TEST(ResourceLifetimeRegistryTests, ResourceManagerTrimUnloadsAllRegisteredAliasesForNormalizedResource)
{
    ResourceLifetimeRegistry registry;
    TestIntResourceManager manager;

    constexpr auto libraryPath = "Library/Artifacts/model/multi-alias.nmesh";
    constexpr auto absolutePath = "D:/Project/Library/Artifacts/model/multi-alias.nmesh";

    manager.RegisterResource(libraryPath, new int(1));
    manager.RegisterResource(absolutePath, new int(2));
    auto handle = manager.AcquireIntHandle(
        registry,
        "scene:multi-alias",
        libraryPath);
    ASSERT_TRUE(handle);
    handle.Reset();

    EXPECT_EQ(manager.TrimMeshes(registry), 2u)
        << "Trimming a lifetime-managed normalized registry resource must unload every manager alias for that artifact.";
    EXPECT_FALSE(manager.IsResourceRegistered(libraryPath));
    EXPECT_FALSE(manager.IsResourceRegistered(absolutePath));
    ASSERT_EQ(manager.destroyedPaths.size(), 2u);
    EXPECT_NE(
        std::find(manager.destroyedPaths.begin(), manager.destroyedPaths.end(), libraryPath),
        manager.destroyedPaths.end());
    EXPECT_NE(
        std::find(manager.destroyedPaths.begin(), manager.destroyedPaths.end(), absolutePath),
        manager.destroyedPaths.end());
}

TEST(ResourceLifetimeRegistryTests, RegistryOnlyTrimCandidateDoesNotBlockLaterManagerResources)
{
    ResourceLifetimeRegistry registry;
    TestIntResourceManager manager;

    registry.Acquire({
        "scene:registry-only",
        ResourceLifetimeResourceType::Mesh,
        "Library/Artifacts/model/registry-only.nmesh",
        sizeof(int),
        ResourceLifetimeOwnerKind::SceneInstance });
    registry.ReleaseOwner("scene:registry-only");

    constexpr auto livePath = "Library/Artifacts/model/live-after-empty.nmesh";
    manager.RegisterResource(livePath, new int(5));
    registry.Acquire({
        "scene:live-after-empty",
        ResourceLifetimeResourceType::Mesh,
        livePath,
        sizeof(int),
        ResourceLifetimeOwnerKind::SceneInstance });
    registry.ReleaseOwner("scene:live-after-empty");

    ResourceLifetimeTrimOptions oneCandidate;
    oneCandidate.maxCandidates = 1u;

    EXPECT_EQ(manager.TrimMeshes(registry, oneCandidate), 1u)
        << "Registry-only candidates should count as trim progress so scheduled trim can continue.";
    EXPECT_TRUE(manager.IsResourceRegistered(livePath))
        << "The first budgeted slice should consume only the older registry-only candidate.";

    EXPECT_EQ(manager.TrimMeshes(registry, oneCandidate), 1u);
    EXPECT_FALSE(manager.IsResourceRegistered(livePath))
        << "The next budgeted slice must reach and unload real manager resources instead of being blocked forever.";
    EXPECT_FALSE(registry.HasActiveOwners(ResourceLifetimeResourceType::Mesh, livePath));
}

TEST(ResourceLifetimeRegistryTests, AcquireDuringPendingEvictionCancelsTrimAndKeepsResourceReachable)
{
    ResourceLifetimeRegistry registry;
    const auto id = registry.Acquire({
        "scene:evict",
        ResourceLifetimeResourceType::Mesh,
        "Library/Artifacts/model/evict.nmesh",
        sizeof(int),
        ResourceLifetimeOwnerKind::SceneInstance });
    registry.Release(id, "scene:evict");

    auto candidates = registry.CollectTrimCandidates({});
    ASSERT_EQ(candidates.size(), 1u);
    ASSERT_TRUE(registry.TryBeginEviction(candidates.front()));

    const auto reacquired = registry.Acquire({
        "preview:reacquired",
        ResourceLifetimeResourceType::Mesh,
        "Library/Artifacts/model/evict.nmesh",
        sizeof(int),
        ResourceLifetimeOwnerKind::Preview });

    EXPECT_FALSE(reacquired.normalizedPath.empty())
        << "A scene restore or drag preview owner that arrives before manager destruction must cancel pending eviction instead of becoming invisible.";
    EXPECT_TRUE(registry.HasActiveOwners(
        ResourceLifetimeResourceType::Mesh,
        "Library/Artifacts/model/evict.nmesh"));

    EXPECT_FALSE(registry.CompleteEviction(candidates.front().type, candidates.front().normalizedPath))
        << "The manager must restore its resource when eviction loses the race to a new active owner.";
    registry.Release(reacquired, "preview:reacquired");
    ASSERT_TRUE(registry.TryBeginEviction(candidates.front()));
    EXPECT_TRUE(registry.CompleteEviction(candidates.front().type, candidates.front().normalizedPath));
    const auto afterCompletedEviction = registry.Acquire({
        "preview:after-completed-evict",
        ResourceLifetimeResourceType::Mesh,
        "Library/Artifacts/model/evict.nmesh",
        sizeof(int),
        ResourceLifetimeOwnerKind::Preview });
    EXPECT_FALSE(afterCompletedEviction.normalizedPath.empty());
    EXPECT_TRUE(registry.IsGenerationCurrent(afterCompletedEviction));
}

TEST(ResourceLifetimeRegistryTests, ManagerTrimEvictsZeroOwnerRegisteredResourcesWithoutPermanentRawOwner)
{
    ResourceLifetimeRegistry registry;
    TestIntResourceManager manager;
    constexpr auto path = "Library/Artifacts/model/raw-bound.nmesh";
    manager.RegisterResource(path, new int(42));

    const auto id = registry.Acquire({
        "scene:raw-bound",
        ResourceLifetimeResourceType::Mesh,
        path,
        sizeof(int),
        ResourceLifetimeOwnerKind::SceneInstance });
    registry.Release(id, "scene:raw-bound");

    EXPECT_EQ(manager.TrimMeshes(registry, {}), 1u)
        << "Released preview/scene owners must not leave a permanent manager-raw lease that keeps canceled prefab resources alive forever.";
    EXPECT_FALSE(manager.IsResourceRegistered(path));
    EXPECT_FALSE(registry.HasActiveOwners(ResourceLifetimeResourceType::Mesh, path));
    ASSERT_EQ(manager.destroyedPaths.size(), 1u);
    EXPECT_EQ(manager.destroyedPaths.front(), path);

    const auto snapshot = registry.CreateDiagnosticSnapshot();
    EXPECT_EQ(snapshot.ownerCount, 0u)
        << "Trim must not manufacture manager-raw owners with no release path.";
}

TEST(ResourceLifetimeRegistryTests, RuntimeManagersExposeTypedHandleAndTrimApis)
{
    ResourceLifetimeRegistry registry;
    MeshManager meshManager;
    MaterialManager materialManager;
    TextureManager textureManager;

    auto meshHandle = meshManager.AcquireMeshHandle(
        registry,
        "scene:mesh",
        "Library/Artifacts/model/body.nmesh");
    auto materialHandle = materialManager.AcquireMaterialHandle(
        registry,
        "scene:material",
        "Library/Artifacts/model/body.nmat");
    auto textureHandle = textureManager.AcquireTextureHandle(
        registry,
        "scene:texture",
        "Library/Artifacts/model/albedo.ntex");

    EXPECT_FALSE(meshHandle)
        << "Typed handles must not create owner leases for missing mesh artifacts.";
    EXPECT_FALSE(materialHandle)
        << "Typed handles must not create owner leases for missing material artifacts.";
    EXPECT_FALSE(textureHandle)
        << "Typed handles must not create owner leases for missing texture artifacts.";
    EXPECT_EQ(registry.GetActiveOwnerCount(
        ResourceLifetimeResourceType::Mesh,
        "Library/Artifacts/model/body.nmesh"), 0u);
    EXPECT_EQ(registry.GetActiveOwnerCount(
        ResourceLifetimeResourceType::Material,
        "Library/Artifacts/model/body.nmat"), 0u);
    EXPECT_EQ(registry.GetActiveOwnerCount(
        ResourceLifetimeResourceType::Texture,
        "Library/Artifacts/model/albedo.ntex"), 0u);

    meshHandle.Reset();
    materialHandle.Reset();
    textureHandle.Reset();

    EXPECT_EQ(meshManager.TrimUnusedMeshResources(registry), 1u)
        << "Missing mesh handle attempts leave a registry-only trim candidate that should be consumed once.";
    EXPECT_EQ(materialManager.TrimUnusedMaterialResources(registry), 1u)
        << "Missing material handle attempts leave a registry-only trim candidate that should be consumed once.";
    EXPECT_EQ(textureManager.TrimUnusedTextureResources(registry), 1u)
        << "Missing texture handle attempts leave a registry-only trim candidate that should be consumed once.";

    EXPECT_EQ(meshManager.TrimUnusedMeshResources(registry), 0u);
    EXPECT_EQ(materialManager.TrimUnusedMaterialResources(registry), 0u);
    EXPECT_EQ(textureManager.TrimUnusedTextureResources(registry), 0u);
}

TEST(ResourceLifetimeRegistryTests, DiagnosticSnapshotReportsBaselineOwnerAndTrimState)
{
    ResourceLifetimeRegistry registry;

    registry.Acquire({
        "scene:prefab",
        ResourceLifetimeResourceType::Mesh,
        "Library/Artifacts/model/body.nmesh",
        128u,
        ResourceLifetimeOwnerKind::SceneInstance });
    registry.Acquire({
        "preview:hover",
        ResourceLifetimeResourceType::Material,
        "Library/Artifacts/model/body.nmat",
        64u,
        ResourceLifetimeOwnerKind::Preview });
    registry.ReleaseOwner("preview:hover");

    const auto snapshot = registry.CreateDiagnosticSnapshot();
    EXPECT_EQ(snapshot.resourceCount, 2u);
    EXPECT_EQ(snapshot.ownerCount, 1u);
    EXPECT_EQ(snapshot.activeLeaseCount, 1u);
    EXPECT_EQ(snapshot.zeroOwnerResourceCount, 1u);
    EXPECT_EQ(snapshot.trimCandidateCount, 1u);
    EXPECT_EQ(snapshot.totalEstimatedBytes, 192u);
    EXPECT_EQ(snapshot.activeEstimatedBytes, 128u);
    EXPECT_EQ(snapshot.zeroOwnerEstimatedBytes, 64u);

    const auto candidates = registry.CollectTrimCandidates({});
    ASSERT_EQ(candidates.size(), 1u);
    registry.CompleteEviction(candidates.front().type, candidates.front().normalizedPath);

    const auto postTrimSnapshot = registry.CreateDiagnosticSnapshot();
    EXPECT_EQ(postTrimSnapshot.resourceCount, 1u);
    EXPECT_EQ(postTrimSnapshot.ownerCount, 1u);
    EXPECT_EQ(postTrimSnapshot.activeLeaseCount, 1u);
    EXPECT_EQ(postTrimSnapshot.zeroOwnerResourceCount, 0u);
    EXPECT_EQ(postTrimSnapshot.trimCandidateCount, 0u);
    EXPECT_EQ(postTrimSnapshot.totalEstimatedBytes, 128u)
        << "Evicted tombstones must not look like still-resident zero-owner byte pressure.";
    EXPECT_EQ(postTrimSnapshot.activeEstimatedBytes, 128u);
    EXPECT_EQ(postTrimSnapshot.zeroOwnerEstimatedBytes, 0u);
}
