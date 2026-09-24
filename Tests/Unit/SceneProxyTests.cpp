#include <gtest/gtest.h>

#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "Components/TransformComponent.h"
#include "Rendering/SceneProxySourceRegistry.h"
#include "Rendering/PersistentObjectDataTable.h"
#include "Rendering/RenderScene.h"
#include "Rendering/SceneRenderStore.h"
#include "SceneSystem/Scene.h"

using namespace NLS::Engine::Rendering;

namespace
{
    SceneProxyHandle MakeHandle(
        const uint64_t sceneId,
        const uint32_t index,
        const uint32_t generation = 1u)
    {
        SceneProxyHandle handle;
        handle.sceneId = sceneId;
        handle.index = index;
        handle.generation = generation;
        return handle;
    }

}

TEST(SceneRenderStoreTests, AppliesAddTransformPayloadAndRemoveCommands)
{
    constexpr uint64_t kSceneId = 17u;
    SceneRenderStore store(kSceneId);
    const auto handle = MakeHandle(kSceneId, 0u);

    SceneProxyUpdateBatch addBatch;
    addBatch.sceneId = kSceneId;
    SceneProxyCommand addCommand;
    addCommand.kind = SceneProxyCommandKind::Add;
    addCommand.handle = handle;
    addCommand.payload.kind = SceneProxyKind::Primitive;
    addCommand.payload.ownerInstanceId = 42u;
    addCommand.transform.worldPosition = {1.0f, 2.0f, 3.0f};
    addBatch.commands.push_back(std::move(addCommand));

    const auto addStats = store.Apply(addBatch);
    EXPECT_EQ(addStats.addedProxyCount, 1u);
    EXPECT_EQ(store.GetProxyCount(), 1u);

    const SceneProxyPayload* payload = nullptr;
    const SceneProxyTransformState* transform = nullptr;
    ASSERT_TRUE(store.TryGetProxy(handle, payload, transform));
    ASSERT_NE(payload, nullptr);
    ASSERT_NE(transform, nullptr);
    EXPECT_EQ(payload->ownerInstanceId, 42u);
    EXPECT_FLOAT_EQ(transform->worldPosition.x, 1.0f);

    SceneProxyUpdateBatch transformBatch;
    transformBatch.sceneId = kSceneId;
    SceneProxyCommand transformCommand;
    transformCommand.kind = SceneProxyCommandKind::UpdateTransform;
    transformCommand.handle = handle;
    transformCommand.transform.worldPosition = {4.0f, 5.0f, 6.0f};
    transformBatch.commands.push_back(std::move(transformCommand));
    const auto transformStats = store.Apply(transformBatch);
    EXPECT_EQ(transformStats.updatedTransformCount, 1u);
    ASSERT_TRUE(store.TryGetProxy(handle, payload, transform));
    EXPECT_FLOAT_EQ(transform->worldPosition.x, 4.0f);

    SceneProxyUpdateBatch payloadBatch;
    payloadBatch.sceneId = kSceneId;
    SceneProxyCommand payloadCommand;
    payloadCommand.kind = SceneProxyCommandKind::UpdatePayload;
    payloadCommand.handle = handle;
    payloadCommand.payload.kind = SceneProxyKind::Primitive;
    payloadCommand.payload.ownerInstanceId = 42u;
    payloadCommand.payload.layer = 3u;
    payloadBatch.commands.push_back(std::move(payloadCommand));
    const auto payloadStats = store.Apply(payloadBatch);
    EXPECT_EQ(payloadStats.updatedPayloadCount, 1u);
    ASSERT_TRUE(store.TryGetProxy(handle, payload, transform));
    EXPECT_EQ(payload->layer, 3u);

    SceneProxyUpdateBatch removeBatch;
    removeBatch.sceneId = kSceneId;
    SceneProxyCommand removeCommand;
    removeCommand.kind = SceneProxyCommandKind::Remove;
    removeCommand.handle = handle;
    removeBatch.commands.push_back(std::move(removeCommand));
    const auto removeStats = store.Apply(removeBatch);
    EXPECT_EQ(removeStats.removedProxyCount, 1u);
    EXPECT_EQ(store.GetProxyCount(), 0u);
    EXPECT_FALSE(store.TryGetProxy(handle, payload, transform));
}

TEST(SceneRenderStoreTests, RejectsStaleGenerationCommands)
{
    constexpr uint64_t kSceneId = 19u;
    SceneRenderStore store(kSceneId);
    const auto handle = MakeHandle(kSceneId, 3u);

    SceneProxyUpdateBatch addBatch;
    addBatch.sceneId = kSceneId;
    addBatch.commands.push_back({ SceneProxyCommandKind::Add, handle });
    ASSERT_EQ(store.Apply(addBatch).addedProxyCount, 1u);

    SceneProxyUpdateBatch staleBatch;
    staleBatch.sceneId = kSceneId;
    staleBatch.commands.push_back({ SceneProxyCommandKind::Remove, MakeHandle(kSceneId, 3u, 99u) });
    const auto staleStats = store.Apply(staleBatch);
    EXPECT_EQ(staleStats.staleCommandCount, 1u);
    EXPECT_EQ(store.GetProxyCount(), 1u);
}

TEST(PersistentObjectDataTableTests, KeepsStableSlotsAndBuildsNonContiguousIndirection)
{
    NLS::Engine::Rendering::PersistentObjectDataTable table;
    const auto first = MakeHandle(7u, 10u);
    const auto second = MakeHandle(7u, 20u);

    const uint32_t firstSlot = table.Acquire(first, NLS::Maths::Matrix4::Identity, 1u);
    const uint32_t secondSlot = table.Acquire(second, NLS::Maths::Matrix4::Identity, 1u);
    EXPECT_NE(firstSlot, secondSlot);

    const auto indirection = table.BuildInstanceIndirection({ second, first });
    ASSERT_EQ(indirection.size(), 2u);
    EXPECT_EQ(indirection[0], secondSlot);
    EXPECT_EQ(indirection[1], firstSlot);

    auto transformed = NLS::Maths::Matrix4::Identity;
    transformed.data[12] = 5.0f;
    EXPECT_TRUE(table.Update(first, transformed, 2u));
    const auto frame = table.BuildFrame();
    ASSERT_GT(frame.objectData.size(), firstSlot);
    EXPECT_FLOAT_EQ(frame.objectData[firstSlot].data[12], 5.0f);
    ASSERT_EQ(frame.dirtyRanges.size(), 1u);
    EXPECT_LE(frame.dirtyRanges[0].begin, firstSlot);
    EXPECT_GT(frame.dirtyRanges[0].end, firstSlot);

    table.Release(second);
    const auto third = MakeHandle(7u, 30u);
    EXPECT_EQ(table.Acquire(third, NLS::Maths::Matrix4::Identity, 1u), secondSlot);
}

TEST(SceneRenderStoreTests, DelaysRemovedProxyResourceReleaseUntilRetireWindowCloses)
{
    constexpr uint64_t kSceneId = 23u;
    NLS::Core::ResourceManagement::ResourceLifetimeRegistry registry;
    SceneRenderStore store(kSceneId);
    const auto handle = MakeHandle(kSceneId, 0u);

    using MeshHandle = SceneProxyResourceBinding::MeshHandle;
    using MaterialHandle = SceneProxyResourceBinding::MaterialHandle;

    NLS::Core::ResourceManagement::ResourceLifetimeAcquireRequest meshRequest;
    meshRequest.ownerToken = "SceneRenderStoreTests/RetireWindow";
    meshRequest.type = NLS::Core::ResourceManagement::ResourceLifetimeResourceType::Mesh;
    meshRequest.path = "Tests/RetireWindow.mesh";
    NLS::Core::ResourceManagement::ResourceLifetimeAcquireRequest materialRequest = meshRequest;
    materialRequest.type = NLS::Core::ResourceManagement::ResourceLifetimeResourceType::Material;
    materialRequest.path = "Tests/RetireWindow.material";

    SceneProxyResourceBinding resources;
    resources.meshHandle = std::make_shared<MeshHandle>(
        registry,
        meshRequest,
        [](const NLS::Core::ResourceManagement::ResourceId&) -> NLS::Render::Resources::Mesh* { return nullptr; });
    resources.materialHandle = std::make_shared<MaterialHandle>(
        registry,
        materialRequest,
        [](const NLS::Core::ResourceManagement::ResourceId&) -> NLS::Render::Resources::Material* { return nullptr; });

    const auto meshPath = NLS::Core::ResourceManagement::ResourceLifetimeRegistry::NormalizeResourcePath(meshRequest.path);
    const auto materialPath =
        NLS::Core::ResourceManagement::ResourceLifetimeRegistry::NormalizeResourcePath(materialRequest.path);

    SceneProxyUpdateBatch addBatch;
    addBatch.sceneId = kSceneId;
    addBatch.frameId = 5u;
    SceneProxyCommand addCommand;
    addCommand.kind = SceneProxyCommandKind::Add;
    addCommand.handle = handle;
    addCommand.payload.kind = SceneProxyKind::Primitive;
    addCommand.payload.resources = resources;
    addBatch.commands.push_back(std::move(addCommand));
    resources = {};
    ASSERT_EQ(store.Apply(addBatch).addedProxyCount, 1u);
    EXPECT_EQ(
        registry.GetActiveOwnerCount(NLS::Core::ResourceManagement::ResourceLifetimeResourceType::Mesh, meshPath),
        1u);

    SceneProxyUpdateBatch removeBatch;
    removeBatch.sceneId = kSceneId;
    removeBatch.frameId = 6u;
    SceneProxyCommand removeCommand;
    removeCommand.kind = SceneProxyCommandKind::Remove;
    removeCommand.handle = handle;
    removeBatch.commands.push_back(std::move(removeCommand));
    ASSERT_EQ(store.Apply(removeBatch).removedProxyCount, 1u);
    EXPECT_EQ(store.GetProxyCount(), 0u);
    EXPECT_EQ(store.GetPendingResourceRetireCount(), 1u);

    // Drop the publisher-side copies so only the store retire queue keeps the
    // resources alive from this point on.
    addBatch = {};
    removeBatch = {};

    // Delayed render commands may still reference the removed proxy resources.
    EXPECT_EQ(
        registry.GetActiveOwnerCount(NLS::Core::ResourceManagement::ResourceLifetimeResourceType::Mesh, meshPath),
        1u);
    EXPECT_EQ(
        registry.GetActiveOwnerCount(
            NLS::Core::ResourceManagement::ResourceLifetimeResourceType::Material,
            materialPath),
        1u);

    store.RetireResourcesThrough(9u);
    EXPECT_EQ(store.GetPendingResourceRetireCount(), 1u);
    EXPECT_EQ(
        registry.GetActiveOwnerCount(NLS::Core::ResourceManagement::ResourceLifetimeResourceType::Mesh, meshPath),
        1u);

    store.RetireResourcesThrough(10u);
    EXPECT_EQ(store.GetPendingResourceRetireCount(), 0u);
    EXPECT_EQ(
        registry.GetActiveOwnerCount(NLS::Core::ResourceManagement::ResourceLifetimeResourceType::Mesh, meshPath),
        0u);
    EXPECT_EQ(
        registry.GetActiveOwnerCount(
            NLS::Core::ResourceManagement::ResourceLifetimeResourceType::Material,
            materialPath),
        0u);
}

TEST(SceneRenderStoreTests, TransformCommandsUpdatePersistentObjectDataSlots)
{
    constexpr uint64_t kSceneId = 21u;
    SceneRenderStore store(kSceneId);
    const auto handle = MakeHandle(kSceneId, 0u);

    SceneProxyUpdateBatch addBatch;
    addBatch.sceneId = kSceneId;
    addBatch.commands.push_back({ SceneProxyCommandKind::Add, handle });
    ASSERT_EQ(store.Apply(addBatch).addedProxyCount, 1u);

    uint32_t slot = ~0u;
    ASSERT_TRUE(store.GetObjectDataTable().TryGetSlot(handle, slot));

    SceneProxyUpdateBatch transformBatch;
    transformBatch.sceneId = kSceneId;
    SceneProxyCommand command;
    command.kind = SceneProxyCommandKind::UpdateTransform;
    command.handle = handle;
    command.transform.worldMatrix.data[12] = 9.0f;
    transformBatch.commands.push_back(command);
    ASSERT_EQ(store.Apply(transformBatch).updatedTransformCount, 1u);

    const auto frame = store.GetObjectDataTable().BuildFrame();
    ASSERT_GT(frame.objectData.size(), slot);
    EXPECT_FLOAT_EQ(frame.objectData[slot].data[12], 9.0f);
}

TEST(SceneRenderStoreTests, ChangeJournalReportsDeltasAndInvalidatesExpiredReaders)
{
    constexpr uint64_t kSceneId = 29u;
    SceneRenderStore store(kSceneId, 2u);
    const auto firstHandle = MakeHandle(kSceneId, 0u);
    const auto secondHandle = MakeHandle(kSceneId, 1u);

    SceneProxyUpdateBatch addFirst;
    addFirst.sceneId = kSceneId;
    addFirst.commands.push_back({ SceneProxyCommandKind::Add, firstHandle });
    ASSERT_EQ(store.Apply(addFirst).addedProxyCount, 1u);

    const auto firstChanges = store.GetChangesSince(0u);
    ASSERT_FALSE(firstChanges.requiresFullResync);
    ASSERT_EQ(firstChanges.changedHandles.size(), 1u);
    EXPECT_EQ(firstChanges.changedHandles.front(), firstHandle);
    const uint64_t firstSerial = firstChanges.changeSerial;

    SceneProxyUpdateBatch transformFirst;
    transformFirst.sceneId = kSceneId;
    transformFirst.commands.push_back({ SceneProxyCommandKind::UpdateTransform, firstHandle });
    ASSERT_EQ(store.Apply(transformFirst).updatedTransformCount, 1u);

    const auto transformChanges = store.GetChangesSince(firstSerial);
    ASSERT_FALSE(transformChanges.requiresFullResync);
    ASSERT_EQ(transformChanges.changedHandles.size(), 1u);
    EXPECT_EQ(transformChanges.changedHandles.front(), firstHandle);
    const uint64_t transformSerial = transformChanges.changeSerial;

    SceneProxyUpdateBatch addSecond;
    addSecond.sceneId = kSceneId;
    addSecond.commands.push_back({ SceneProxyCommandKind::Add, secondHandle });
    ASSERT_EQ(store.Apply(addSecond).addedProxyCount, 1u);

    EXPECT_TRUE(store.GetChangesSince(0u).requiresFullResync);
    const auto retainedChanges = store.GetChangesSince(firstSerial);
    ASSERT_FALSE(retainedChanges.requiresFullResync);
    ASSERT_EQ(retainedChanges.changedHandles.size(), 2u);
    EXPECT_EQ(retainedChanges.changedHandles[0], firstHandle);
    EXPECT_EQ(retainedChanges.changedHandles[1], secondHandle);

    SceneProxyUpdateBatch removeFirst;
    removeFirst.sceneId = kSceneId;
    removeFirst.commands.push_back({ SceneProxyCommandKind::Remove, firstHandle });
    ASSERT_EQ(store.Apply(removeFirst).removedProxyCount, 1u);

    EXPECT_TRUE(store.GetChangesSince(firstSerial).requiresFullResync);
    const auto latestChanges = store.GetChangesSince(transformSerial);
    ASSERT_FALSE(latestChanges.requiresFullResync);
    ASSERT_EQ(latestChanges.changedHandles.size(), 1u);
    ASSERT_EQ(latestChanges.removedHandles.size(), 1u);
    EXPECT_EQ(latestChanges.changedHandles.front(), secondHandle);
    EXPECT_EQ(latestChanges.removedHandles.front(), firstHandle);

    const uint64_t serialBeforeClear = store.GetChangeSerial();
    store.Clear();
    EXPECT_TRUE(store.GetChangesSince(serialBeforeClear).requiresFullResync);
}

TEST(SceneProxySourceRegistryTests, EmitsIncrementalAddTransformAndRemoveCommands)
{
    NLS::Engine::SceneSystem::Scene scene;
    auto& gameObject = scene.CreateGameObject("ProxySource");
    auto* meshFilter = gameObject.AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* meshRenderer = gameObject.AddComponent<NLS::Engine::Components::MeshRenderer>();
    meshFilter->SetMesh(nullptr);
    ASSERT_NE(meshRenderer, nullptr);

    SceneProxySourceRegistry registry;
    SceneProxyExtractionOptions options;
    const auto added = registry.Collect(scene, options, 1u);
    ASSERT_EQ(added.commands.size(), 1u);
    EXPECT_EQ(added.commands.front().kind, SceneProxyCommandKind::Add);
    EXPECT_EQ(registry.GetTrackedPrimitiveProxyCount(), 1u);
    EXPECT_EQ(registry.GetCollectionScanCountForTesting(), 1u);

    const auto stable = registry.Collect(scene, options, 2u);
    EXPECT_TRUE(stable.commands.empty());
    EXPECT_EQ(registry.GetCollectionScanCountForTesting(), 1u);

    gameObject.GetTransform()->SetLocalPosition({2.0f, 0.0f, 0.0f});
    const auto transformed = registry.Collect(scene, options, 3u);
    ASSERT_EQ(transformed.commands.size(), 1u);
    EXPECT_EQ(transformed.commands.front().kind, SceneProxyCommandKind::UpdateTransform);
    EXPECT_EQ(registry.GetCollectionScanCountForTesting(), 2u);

    meshRenderer->SetFrustumBehaviour(NLS::Engine::Components::MeshRenderer::EFrustumBehaviour::DISABLED);
    const auto payloadChanged = registry.Collect(scene, options, 4u);
    ASSERT_EQ(payloadChanged.commands.size(), 1u);
    EXPECT_EQ(payloadChanged.commands.front().kind, SceneProxyCommandKind::UpdatePayload);

    scene.DestroyGameObject(gameObject);
    const auto removed = registry.Collect(scene, options, 5u);
    ASSERT_EQ(removed.commands.size(), 1u);
    EXPECT_EQ(removed.commands.front().kind, SceneProxyCommandKind::Remove);
    EXPECT_EQ(registry.GetTrackedPrimitiveProxyCount(), 0u);
}

TEST(SceneProxySourceRegistryTests, ParentMotionAdvancesChildWorldTransformRevision)
{
    NLS::Engine::SceneSystem::Scene scene;
    auto& parent = scene.CreateGameObject("ProxyParent");
    auto& child = scene.CreateGameObject("ProxyChild");
    child.SetParent(parent);
    auto* meshFilter = child.AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* meshRenderer = child.AddComponent<NLS::Engine::Components::MeshRenderer>();
    meshFilter->SetMesh(nullptr);
    ASSERT_NE(meshRenderer, nullptr);

    SceneProxySourceRegistry registry;
    SceneProxyExtractionOptions options;
    const auto added = registry.Collect(scene, options, 1u);
    ASSERT_EQ(added.commands.size(), 1u);
    ASSERT_EQ(added.commands.front().kind, SceneProxyCommandKind::Add);
    const auto initialTransform = added.commands.front().transform;

    parent.GetTransform()->TranslateLocal({3.0f, 0.0f, 0.0f});
    const auto moved = registry.Collect(scene, options, 2u);
    ASSERT_EQ(moved.commands.size(), 1u);
    ASSERT_EQ(moved.commands.front().kind, SceneProxyCommandKind::UpdateTransform);
    const auto& movedTransform = moved.commands.front().transform;

    EXPECT_FALSE(NLS::Maths::Matrix4::AreEquals(
        initialTransform.worldMatrix,
        movedTransform.worldMatrix));
    EXPECT_TRUE(NLS::Maths::Matrix4::AreEquals(
        child.GetTransform()->GetWorldMatrix(),
        movedTransform.worldMatrix));
    EXPECT_GT(movedTransform.transformRevision, initialTransform.transformRevision);

    const auto stable = registry.Collect(scene, options, 3u);
    EXPECT_TRUE(stable.commands.empty());
}

TEST(SceneProxySourceRegistryTests, PrimitiveHierarchyMetadataEmitsPayloadUpdates)
{
    NLS::Engine::SceneSystem::Scene scene;
    auto& firstParent = scene.CreateGameObject("FirstProxyParent");
    auto& secondParent = scene.CreateGameObject("SecondProxyParent");
    auto& child = scene.CreateGameObject("MetadataProxyChild");
    child.SetParent(firstParent);
    auto* meshFilter = child.AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* meshRenderer = child.AddComponent<NLS::Engine::Components::MeshRenderer>();
    meshFilter->SetMesh(nullptr);
    ASSERT_NE(meshRenderer, nullptr);

    SceneProxySourceRegistry registry;
    SceneProxyExtractionOptions options;
    ASSERT_EQ(registry.Collect(scene, options, 1u).commands.size(), 1u);

    child.SetSourceObjectKey("Source/Child");
    const auto sourceChanged = registry.Collect(scene, options, 2u);
    ASSERT_EQ(sourceChanged.commands.size(), 1u);
    EXPECT_EQ(sourceChanged.commands.front().kind, SceneProxyCommandKind::UpdatePayload);
    EXPECT_EQ(sourceChanged.commands.front().payload.sourceObjectKey, "Source/Child");

    firstParent.SetLargeSceneHLODMetadata("{\"cluster\":1}");
    const auto hlodChanged = registry.Collect(scene, options, 3u);
    ASSERT_EQ(hlodChanged.commands.size(), 1u);
    EXPECT_EQ(hlodChanged.commands.front().kind, SceneProxyCommandKind::UpdatePayload);
    EXPECT_EQ(
        hlodChanged.commands.front().payload.hlodScopeRootInstanceId,
        static_cast<uint64_t>(firstParent.GetInstanceID()));
    EXPECT_EQ(hlodChanged.commands.front().payload.hlodScopeMetadata, "{\"cluster\":1}");

    child.SetParent(secondParent);
    const auto parentChanged = registry.Collect(scene, options, 4u);
    ASSERT_EQ(parentChanged.commands.size(), 1u);
    EXPECT_EQ(parentChanged.commands.front().kind, SceneProxyCommandKind::UpdatePayload);
    EXPECT_EQ(
        parentChanged.commands.front().payload.parentInstanceId,
        static_cast<uint64_t>(secondParent.GetInstanceID()));
    EXPECT_EQ(parentChanged.commands.front().payload.hlodScopeRootInstanceId, 0u);
    EXPECT_TRUE(parentChanged.commands.front().payload.hlodScopeMetadata.empty());
}

TEST(SceneProxySourceRegistryTests, ViewRenderScenesShareOneStorePerScene)
{
    NLS::Engine::SceneSystem::Scene scene;
    auto& gameObject = scene.CreateGameObject("SharedStoreProxy");
    auto* meshFilter = gameObject.AddComponent<NLS::Engine::Components::MeshFilter>();
    gameObject.AddComponent<NLS::Engine::Components::MeshRenderer>();
    meshFilter->SetMesh(nullptr);

    NLS::Engine::Rendering::RenderScene first;
    NLS::Engine::Rendering::RenderScene second;
    first.Synchronize(scene);
    second.Synchronize(scene);

    EXPECT_EQ(&first.GetProxyStore(), &second.GetProxyStore());
    EXPECT_EQ(first.GetProxyStore().GetProxyCount(), 1u);
    EXPECT_EQ(second.GetProxyStore().GetProxyCount(), 1u);
}

TEST(SceneProxySourceRegistryTests, SharedRenderScenesConsumeStoreChangesIndependently)
{
    NLS::Engine::SceneSystem::Scene scene;
    auto& gameObject = scene.CreateGameObject("SharedStoreIncrementalProxy");
    auto* meshFilter = gameObject.AddComponent<NLS::Engine::Components::MeshFilter>();
    gameObject.AddComponent<NLS::Engine::Components::MeshRenderer>();
    meshFilter->SetMesh(nullptr);

    NLS::Engine::Rendering::RenderScene first;
    NLS::Engine::Rendering::RenderScene second;
    ASSERT_EQ(first.Synchronize(scene).syncTouchedPrimitiveCount, 1u);
    ASSERT_EQ(second.Synchronize(scene).syncTouchedPrimitiveCount, 1u);
    const auto initialSnapshot = first.CreatePrimitiveSnapshotForTesting();
    ASSERT_EQ(initialSnapshot.primitiveRecords.size(), 1u);

    gameObject.GetTransform()->SetLocalPosition({4.0f, 0.0f, 0.0f});
    const auto firstChanged = first.Synchronize(scene);
    const auto secondChanged = second.Synchronize(scene);

    EXPECT_EQ(firstChanged.syncTouchedPrimitiveCount, 1u);
    EXPECT_EQ(secondChanged.syncTouchedPrimitiveCount, 1u);
    EXPECT_EQ(firstChanged.syncFullSweepCount, 0u);
    EXPECT_EQ(secondChanged.syncFullSweepCount, 0u);

    const auto firstSnapshot = first.CreatePrimitiveSnapshotForTesting();
    const auto secondSnapshot = second.CreatePrimitiveSnapshotForTesting();
    ASSERT_EQ(firstSnapshot.primitiveRecords.size(), 1u);
    ASSERT_EQ(secondSnapshot.primitiveRecords.size(), 1u);
    EXPECT_FALSE(NLS::Maths::Matrix4::AreEquals(
        initialSnapshot.primitiveRecords.front().worldMatrix,
        firstSnapshot.primitiveRecords.front().worldMatrix));
    EXPECT_TRUE(NLS::Maths::Matrix4::AreEquals(
        firstSnapshot.primitiveRecords.front().worldMatrix,
        secondSnapshot.primitiveRecords.front().worldMatrix));
}
