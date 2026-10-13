#include <gtest/gtest.h>

#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "Components/LightComponent.h"
#include "Components/TransformComponent.h"
#include "Rendering/SceneProxySourceRegistry.h"
#include "Rendering/PersistentObjectDataTable.h"
#include "Rendering/RenderScene.h"
#include "Rendering/SceneRenderStore.h"
#include "Rendering/SceneRenderStoreRegistry.h"
#include "SceneSystem/Scene.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Settings/DriverSettings.h"
#include "Rendering/Context/RhiCommandListExecutor.h"
#include "Tests/RenderingThreadTestHelpers.h"

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

using namespace NLS::Engine::Rendering;

namespace
{
    class ProxyRetirementFence final : public NLS::Render::RHI::RHIFence
    {
    public:
        std::string_view GetDebugName() const override { return "ProxyRetirementFence"; }
        bool IsSignaled() const override { return signaled.load(); }
        bool Wait(uint64_t) override { return IsSignaled(); }
        void Reset() override { signaled.store(false); }
        std::atomic_bool signaled { false };
    };

    class ProxyRetirementExecutorScope final
    {
    public:
        ProxyRetirementExecutorScope()
        {
            executor.ResetForTesting();
            NLS::Render::Context::RhiExecutorTargets targets;
            targets.signalGpuFences = [this](auto, auto& fences)
            {
                ++markerCount;
                fences.push_back(fence);
                return NLS::Render::RHI::RHIQueueOperationResult {
                    NLS::Render::RHI::RHIQueueOperationStatusCode::Success, {}, true, true
                };
            };
            executor.Initialize(NLS::Render::Context::RhiThreadMode::None, std::move(targets));
            (void)executor.RunOnSubmitLane("EarlierSceneFrame", []
                { return NLS::Render::RHI::RHIQueueOperationResult {}; });
        }
        ~ProxyRetirementExecutorScope() { executor.ResetForTesting(); }

        NLS::Render::Context::RhiCommandListExecutor& executor =
            NLS::Render::Context::RhiCommandListExecutor::Get();
        std::shared_ptr<ProxyRetirementFence> fence = std::make_shared<ProxyRetirementFence>();
        size_t markerCount = 0u;
    };

    SceneProxyResourceBinding MakeTrackedProxyResources(
        NLS::Core::ResourceManagement::ResourceLifetimeRegistry& registry)
    {
        NLS::Core::ResourceManagement::ResourceLifetimeAcquireRequest request;
        request.ownerToken = "SceneRenderStoreTests/GpuRetirement";
        request.type = NLS::Core::ResourceManagement::ResourceLifetimeResourceType::Mesh;
        request.path = "Tests/GpuRetirement.mesh";
        SceneProxyResourceBinding resources;
        resources.meshHandle = std::make_shared<SceneProxyResourceBinding::MeshHandle>(registry, request,
            [](const auto&) -> NLS::Render::Resources::Mesh* { return nullptr; });
        return resources;
    }

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

    /** Builds one Add command whose payload and transform encode its slot index. */
    SceneProxyUpdateBatch MakeSingleAddBatch(
        const uint64_t sceneId,
        const uint32_t index,
        const uint64_t frameId = 0u)
    {
        SceneProxyUpdateBatch batch;
        batch.sceneId = sceneId;
        batch.frameId = frameId;
        SceneProxyCommand command;
        command.kind = SceneProxyCommandKind::Add;
        command.handle = MakeHandle(sceneId, index);
        command.payload.kind = SceneProxyKind::Primitive;
        // Both fields carry the slot index so a torn read across the two members
        // is detectable by the readers below.
        command.payload.ownerInstanceId = index;
        command.transform.worldPosition = {
            static_cast<float>(index),
            static_cast<float>(index),
            static_cast<float>(index)
        };
        batch.commands.push_back(std::move(command));
        return batch;
    }

    /**
     * Builds one UpdatePayload command. Apply reassigns the whole slot payload,
     * which is exactly the mutation a concurrently-held payload pointer must be
     * protected from.
     */
    SceneProxyUpdateBatch MakePayloadUpdateBatch(
        const uint64_t sceneId,
        const uint32_t index,
        const uint64_t ownerInstanceId)
    {
        SceneProxyUpdateBatch batch;
        batch.sceneId = sceneId;
        SceneProxyCommand command;
        command.kind = SceneProxyCommandKind::UpdatePayload;
        command.handle = MakeHandle(sceneId, index);
        command.payload.kind = SceneProxyKind::Primitive;
        command.payload.ownerInstanceId = ownerInstanceId;
        command.transform.worldPosition = {
            static_cast<float>(index),
            static_cast<float>(index),
            static_cast<float>(index)
        };
        batch.commands.push_back(std::move(command));
        return batch;
    }

}

class SceneRenderStoreOwnershipTests : public ::testing::Test,
    private NLS::Render::Context::IRenderingThreadTick
{
protected:
    void SetUp() override
    {
        NLS::Base::TaskGraph::TaskGraphConfig config;
        config.workerCount = 1u;
        graph = std::make_unique<NLS::Base::TaskGraph::TaskGraph>(config);
        if (NLS::Render::Context::RenderingThreadSystem::HasActiveRenderThread())
        {
            ASSERT_TRUE(graph->Start());
            gameThread = std::make_unique<NLS::Render::Context::ScopedGameThread>(graph.get());
            return;
        }
        NLS::Render::Context::RenderingThreadInitParams params;
        params.rhiThreadMode = NLS::Render::Context::RhiThreadMode::None;
        params.taskGraph = graph.get();
        params.tick = this;
        ASSERT_TRUE(threads.InitRenderingThreads(params));
    }

    void TearDown() override
    {
        threads.ShutdownRenderingThreads();
        gameThread.reset();
        graph->Shutdown();
    }

    bool TickRenderThread() override
    {
        return NLS::Render::Context::RenderCommandDispatcher::Dispatch() != 0u;
    }

    bool TickRhiLane() override { return false; }

    std::unique_ptr<NLS::Base::TaskGraph::TaskGraph> graph;
    std::unique_ptr<NLS::Render::Context::ScopedGameThread> gameThread;
    NLS::Render::Context::RenderingThreadSystem threads;
};

TEST_F(SceneRenderStoreOwnershipTests, GameThreadCannotReadOrMutateProxyContents)
{
    constexpr uint64_t sceneId = 31u;
    SceneRenderStore store(sceneId);
    NLS::Tests::RunOnRenderThread([&] { (void)store.Apply(MakeSingleAddBatch(sceneId, 0u)); });
    EXPECT_TRUE(NLS::Render::Context::RenderingThreadSystem::IsInGameThread());
    EXPECT_THROW(store.Apply(MakeSingleAddBatch(sceneId, 1u)), std::logic_error);
    EXPECT_THROW(store.Clear(), std::logic_error);
    EXPECT_THROW(store.RetireResources(), std::logic_error);
    EXPECT_THROW((void)store.Read(), std::logic_error);
    EXPECT_THROW((void)store.GetObjectDataTable(), std::logic_error);
    EXPECT_THROW((void)store.GetChangesSince(0u), std::logic_error);
    EXPECT_THROW((void)store.GetPendingResourceRetireCount(), std::logic_error);
    EXPECT_THROW(store.ForEachProxy({}), std::logic_error);
    EXPECT_THROW(store.ForEachLight({}), std::logic_error);
    EXPECT_THROW(store.ForEachSkyBox({}), std::logic_error);
    const SceneProxyPayload* payload = nullptr;
    const SceneProxyTransformState* transform = nullptr;
    EXPECT_THROW((void)store.TryGetProxy(MakeHandle(sceneId, 0u), payload, transform), std::logic_error);
    EXPECT_EQ(store.GetProxyCount(), 1u);
    EXPECT_EQ(store.GetChangeSerial(), 1u);
}

TEST_F(SceneRenderStoreOwnershipTests, WorkerCannotAccessStoreOrBorrowedReadView)
{
    constexpr uint64_t sceneId = 32u;
    SceneRenderStore store(sceneId);
    NLS::Tests::RunOnRenderThread([&]
    {
        (void)store.Apply(MakeSingleAddBatch(sceneId, 0u));
        const auto view = store.Read();
        const auto worker = graph->Submit([&]
        {
            EXPECT_TRUE(graph->IsWorkerThread());
            EXPECT_FALSE(NLS::Render::Context::RenderingThreadSystem::IsInRenderingThread());
            EXPECT_THROW(store.Apply(MakePayloadUpdateBatch(sceneId, 0u, 99u)), std::logic_error);
            EXPECT_THROW((void)store.Read(), std::logic_error);
            EXPECT_THROW((void)view.GetProxyCount(), std::logic_error);
            EXPECT_THROW((void)view.GetChangeSerial(), std::logic_error);
            EXPECT_THROW((void)view.GetChangesSince(0u), std::logic_error);
            EXPECT_THROW((void)view.GetObjectDataTable(), std::logic_error);
            const SceneProxyPayload* payload = nullptr;
            const SceneProxyTransformState* transform = nullptr;
            EXPECT_THROW((void)view.TryGetProxy(MakeHandle(sceneId, 0u), payload, transform), std::logic_error);
            EXPECT_THROW(view.ForEachProxy({}), std::logic_error);
            EXPECT_EQ(store.GetProxyCount(), 1u);
        });
        EXPECT_EQ(graph->Wait(worker), NLS::Base::TaskGraph::TaskStatus::Succeeded);
        EXPECT_EQ(view.GetProxyCount(), 1u);
    });
}

TEST_F(SceneRenderStoreOwnershipTests, MainPipeAppliesAddUpdateRemoveInFifoOrder)
{
    using namespace NLS::Render::Context;
    constexpr uint64_t sceneId = 33u;
    SceneRenderStore store(sceneId);
    std::vector<uint64_t> observed;
    EnqueueRenderCommand<struct StoreAddTestCommand>([&]
    {
        (void)store.Apply(MakeSingleAddBatch(sceneId, 0u));
        observed.push_back(store.GetChangeSerial());
    });
    EnqueueRenderCommand<struct StoreUpdateTestCommand>([&]
    {
        (void)store.Apply(MakePayloadUpdateBatch(sceneId, 0u, 99u));
        const auto view = store.Read();
        const SceneProxyPayload* payload = nullptr;
        const SceneProxyTransformState* transform = nullptr;
        ASSERT_TRUE(view.TryGetProxy(MakeHandle(sceneId, 0u), payload, transform));
        EXPECT_EQ(payload->ownerInstanceId, 99u);
        observed.push_back(view.GetChangeSerial());
    });
    RenderCommandEvent complete;
    EnqueueRenderCommand<struct StoreRemoveTestCommand>([&]
    {
        SceneProxyUpdateBatch remove;
        remove.sceneId = sceneId;
        remove.commands.push_back({ SceneProxyCommandKind::Remove, MakeHandle(sceneId, 0u) });
        EXPECT_EQ(store.Apply(remove).removedProxyCount, 1u);
        observed.push_back(store.GetChangeSerial());
    }, complete);
    complete.Wait();
    ASSERT_TRUE(complete.Succeeded()) << complete.GetDiagnostic();
    EXPECT_EQ(observed, (std::vector<uint64_t> { 1u, 2u, 3u }));
    EXPECT_EQ(store.GetProxyCount(), 0u);
}

TEST_F(SceneRenderStoreOwnershipTests, ReadViewsRejectReentrantMutationAndPermitNestedReads)
{
    constexpr uint64_t sceneId = 34u;
    SceneRenderStore store(sceneId);
    NLS::Tests::RunOnRenderThread([&]
    {
        (void)store.Apply(MakeSingleAddBatch(sceneId, 0u));
        {
            const auto view = store.Read();
            const auto nested = store.Read();
            EXPECT_EQ(nested.GetProxyCount(), 1u);
            EXPECT_THROW(store.Apply(MakePayloadUpdateBatch(sceneId, 0u, 99u)), std::logic_error);
            EXPECT_THROW(store.Clear(), std::logic_error);
            EXPECT_THROW(store.RetireResources(), std::logic_error);
        }
        store.ForEachProxy([&](const auto&, const auto& payload, const auto&)
        {
            EXPECT_EQ(payload.ownerInstanceId, 0u);
            EXPECT_THROW(store.Clear(), std::logic_error);
            EXPECT_EQ(store.Read().GetProxyCount(), 1u);
        });
        EXPECT_EQ(store.Apply(MakePayloadUpdateBatch(sceneId, 0u, 99u)).updatedPayloadCount, 1u);
        store.Clear();
        EXPECT_EQ(store.GetProxyCount(), 0u);
    });
}

TEST_F(SceneRenderStoreOwnershipTests, PublishedMetadataIsReadableDuringRenderMutation)
{
    using namespace NLS::Render::Context;
    constexpr uint64_t sceneId = 35u;
    constexpr uint32_t proxyCount = 256u;
    SceneRenderStore store(sceneId);
    std::promise<void> started;
    std::promise<void> observed;
    auto observerReady = observed.get_future();
    RenderCommandEvent complete;
    EnqueueRenderCommand<struct StoreMetadataTestCommand>([&]
    {
        (void)store.Apply(MakeSingleAddBatch(sceneId, 0u));
        started.set_value();
        observerReady.wait();
        for (uint32_t index = 1u; index < proxyCount; ++index)
            (void)store.Apply(MakeSingleAddBatch(sceneId, index));
    }, complete);
    started.get_future().wait();
    EXPECT_EQ(store.GetProxyCount(), 1u);
    EXPECT_EQ(store.GetChangeSerial(), 1u);
    observed.set_value();
    size_t lastCount = 1u;
    uint64_t lastSerial = 1u;
    do
    {
        const auto count = store.GetProxyCount();
        const auto serial = store.GetChangeSerial();
        EXPECT_GE(count, lastCount);
        EXPECT_GE(serial, lastSerial);
        lastCount = count;
        lastSerial = serial;
        std::this_thread::yield();
    } while (!complete.IsComplete());
    ASSERT_TRUE(complete.Succeeded()) << complete.GetDiagnostic();
    EXPECT_EQ(store.GetProxyCount(), proxyCount);
    EXPECT_EQ(store.GetChangeSerial(), proxyCount);
}

TEST_F(SceneRenderStoreOwnershipTests, RegistryDestructionIsOrderedAndTransfersGpuOwners)
{
    using namespace NLS::Render::Context;
    NLS::Core::ResourceManagement::ResourceLifetimeRegistry registry;
    ProxyRetirementExecutorScope retirement;
    int sceneIdentity = 0;
    auto entry = SceneRenderStoreRegistry::Acquire(36u, &sceneIdentity);
    auto batch = MakeSingleAddBatch(36u, 0u);
    batch.commands.front().payload.resources = MakeTrackedProxyResources(registry);
    std::weak_ptr<SceneProxyResourceBinding::MeshHandle> resource =
        batch.commands.front().payload.resources.meshHandle;
    NLS::Tests::RunOnRenderThread([&] { (void)entry.store->Apply(batch); });
    batch = {};
    std::promise<void> blocked;
    std::promise<void> resume;
    auto resumeFuture = resume.get_future();
    EnqueueRenderCommand<struct BlockStoreReleaseTestCommand>([&]
    {
        blocked.set_value();
        resumeFuture.wait();
    });
    blocked.get_future().wait();
    entry.store.reset();
    SceneRenderStoreRegistry::Release(&sceneIdentity);
    EXPECT_EQ(retirement.markerCount, 0u);
    EXPECT_FALSE(resource.expired());
    RenderCommandEvent complete;
    EnqueueRenderCommand<struct AfterStoreReleaseTestCommand>([] {}, complete);
    resume.set_value();
    complete.Wait();
    ASSERT_TRUE(complete.Succeeded()) << complete.GetDiagnostic();
    EXPECT_EQ(retirement.markerCount, 1u);
    EXPECT_FALSE(resource.expired());
    retirement.fence->signaled.store(true);
    EXPECT_EQ(retirement.executor.ReleaseRetiredBatches(), 1u);
    EXPECT_TRUE(resource.expired());
}

TEST_F(SceneRenderStoreOwnershipTests, SceneDestructionRetiresLeasesDespiteCachedStoreOwners)
{
    using namespace NLS::Render::Context;
    NLS::Core::ResourceManagement::ResourceLifetimeRegistry registry;
    ProxyRetirementExecutorScope retirement;
    auto scene = std::make_unique<NLS::Engine::SceneSystem::Scene>();
    const auto sceneId = static_cast<uint64_t>(scene->GetInstanceID());
    auto entry = SceneRenderStoreRegistry::Acquire(sceneId, scene.get());
    auto batch = MakeSingleAddBatch(sceneId, 0u);
    batch.commands.front().payload.resources = MakeTrackedProxyResources(registry);
    NLS::Core::ResourceManagement::ResourceLifetimeAcquireRequest materialRequest;
    materialRequest.ownerToken = "SceneRenderStoreTests/CachedMaterial";
    materialRequest.type = NLS::Core::ResourceManagement::ResourceLifetimeResourceType::Material;
    materialRequest.path = "Tests/CachedMaterial.mat";
    batch.commands.front().payload.resources.materialHandle =
        std::make_shared<SceneProxyResourceBinding::MaterialHandle>(registry, materialRequest,
            [](const auto&) -> NLS::Render::Resources::Material* { return nullptr; });
    std::weak_ptr<void> mesh = batch.commands.front().payload.resources.meshHandle;
    std::weak_ptr<void> material = batch.commands.front().payload.resources.materialHandle;
    NLS::Tests::RunOnRenderThread([&] { (void)entry.store->Apply(batch); });
    batch = {};

    scene.reset();
    NLS::Tests::RunOnRenderThread([] {});
    EXPECT_EQ(entry.store->GetProxyCount(), 0u);
    EXPECT_EQ(retirement.markerCount, 1u);
    EXPECT_FALSE(mesh.expired());
    EXPECT_FALSE(material.expired());
    retirement.fence->signaled.store(true);
    EXPECT_EQ(retirement.executor.ReleaseRetiredBatches(), 1u);
    EXPECT_TRUE(mesh.expired());
    EXPECT_TRUE(material.expired());
    EXPECT_EQ(registry.CreateDiagnosticSnapshot().activeLeaseCount, 0u);
}

TEST_F(SceneRenderStoreOwnershipTests, ShutdownReleasesRegistryLeasesBeforeCachedStoresAreDestroyed)
{
    NLS::Core::ResourceManagement::ResourceLifetimeRegistry registry;
    ProxyRetirementExecutorScope retirement;
    int firstIdentity = 0;
    int secondIdentity = 0;
    auto first = SceneRenderStoreRegistry::Acquire(37u, &firstIdentity);
    auto second = SceneRenderStoreRegistry::Acquire(38u, &secondIdentity);
    auto batch = MakeSingleAddBatch(37u, 0u);
    batch.commands.front().payload.resources = MakeTrackedProxyResources(registry);
    std::weak_ptr<void> resource = batch.commands.front().payload.resources.meshHandle;
    NLS::Tests::RunOnRenderThread([&] { (void)first.store->Apply(batch); });
    batch.sceneId = 38u;
    batch.commands.front().handle = MakeHandle(38u, 0u);
    NLS::Tests::RunOnRenderThread([&] { (void)second.store->Apply(batch); });
    batch = {};

    SceneRenderStoreRegistry::ReleaseAll();
    NLS::Tests::RunOnRenderThread([] {});
    EXPECT_EQ(first.store->GetProxyCount(), 0u);
    EXPECT_EQ(second.store->GetProxyCount(), 0u);
    EXPECT_FALSE(resource.expired());
    retirement.fence->signaled.store(true);
    EXPECT_EQ(retirement.executor.ReleaseRetiredBatches(), 2u);
    EXPECT_TRUE(resource.expired());
    EXPECT_EQ(registry.CreateDiagnosticSnapshot().activeLeaseCount, 0u);
    // Idempotent teardown must also work after the render lane has joined.
    threads.ShutdownRenderingThreads();
    SceneRenderStoreRegistry::ReleaseAll();
}

TEST(SceneRenderStoreShutdownTests, DriverShutdownJoinsAllRegistryReleaseCommandsBeforeGpuFlush)
{
    using namespace NLS::Render::Context;
    NLS::Core::ResourceManagement::ResourceLifetimeRegistry registry;
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    Driver driver(settings);
    std::vector<int> identities(32u);
    std::vector<SceneRenderStoreEntry> entries;
    for (size_t index = 0u; index < identities.size(); ++index)
    {
        const auto sceneId = 200u + index;
        entries.push_back(SceneRenderStoreRegistry::Acquire(sceneId, &identities[index]));
        auto batch = MakeSingleAddBatch(sceneId, 0u);
        batch.commands.front().payload.resources = MakeTrackedProxyResources(registry);
        NLS::Tests::RunOnRenderThread([&] { (void)entries.back().store->Apply(batch); });
    }
    EXPECT_EQ(registry.CreateDiagnosticSnapshot().activeLeaseCount, identities.size());
    SceneRenderStoreRegistry::ReleaseAll();
    driver.ShutdownThreadedRendering();
    EXPECT_EQ(registry.CreateDiagnosticSnapshot().activeLeaseCount, 0u);
    for (const auto& entry : entries)
        EXPECT_EQ(entry.store->GetProxyCount(), 0u);
}

NLS_RENDER_THREAD_TEST(SceneRenderStoreTests, AppliesAddTransformPayloadAndRemoveCommands)
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

NLS_RENDER_THREAD_TEST(SceneRenderStoreTests, RejectsStaleGenerationCommands)
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

NLS_RENDER_THREAD_TEST(SceneRenderStoreTests, DelaysRemovedProxyResourceReleaseUntilGpuCompletes)
{
    constexpr uint64_t kSceneId = 23u;
    NLS::Core::ResourceManagement::ResourceLifetimeRegistry registry;
    ProxyRetirementExecutorScope retirement;
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

    // CPU frame advancement cannot prove that earlier GPU work retired.
    SceneProxyUpdateBatch laterBatch;
    laterBatch.sceneId = kSceneId;
    laterBatch.frameId = 10000u;
    (void)store.Apply(laterBatch);
    store.RetireResources();
    EXPECT_EQ(retirement.executor.ReleaseRetiredBatches(), 0u);
    EXPECT_EQ(store.GetPendingResourceRetireCount(), 1u);
    EXPECT_EQ(
        registry.GetActiveOwnerCount(NLS::Core::ResourceManagement::ResourceLifetimeResourceType::Mesh, meshPath),
        1u);

    retirement.fence->signaled.store(true);
    EXPECT_EQ(retirement.executor.ReleaseRetiredBatches(), 1u);
    store.RetireResources();
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

NLS_RENDER_THREAD_TEST(SceneRenderStoreTests, StoreDestructionTransfersLiveBindingsToGpuRetirement)
{
    NLS::Core::ResourceManagement::ResourceLifetimeRegistry registry;
    ProxyRetirementExecutorScope retirement;
    auto store = std::make_unique<SceneRenderStore>(23u);
    auto batch = MakeSingleAddBatch(23u, 0u);
    batch.commands.front().payload.resources = MakeTrackedProxyResources(registry);
    std::weak_ptr<SceneProxyResourceBinding::MeshHandle> resource =
        batch.commands.front().payload.resources.meshHandle;
    ASSERT_EQ(store->Apply(batch).addedProxyCount, 1u);
    batch = {};
    store.reset();
    EXPECT_FALSE(resource.expired());
    EXPECT_EQ(retirement.markerCount, 1u);
    retirement.fence->signaled.store(true);
    EXPECT_EQ(retirement.executor.ReleaseRetiredBatches(), 1u);
    EXPECT_TRUE(resource.expired());
}

NLS_RENDER_THREAD_TEST(SceneRenderStoreTests, ClearAndPayloadReplacementBatchResourceRetirement)
{
    for (const auto operation : { SceneProxyCommandKind::Remove, SceneProxyCommandKind::UpdatePayload,
        SceneProxyCommandKind::Recreate, SceneProxyCommandKind::Add })
    {
        SCOPED_TRACE(static_cast<int>(operation));
        NLS::Core::ResourceManagement::ResourceLifetimeRegistry registry;
        ProxyRetirementExecutorScope retirement;
        SceneRenderStore store(23u);
        auto batch = MakeSingleAddBatch(23u, 0u);
        batch.commands.front().payload.resources = MakeTrackedProxyResources(registry);
        std::weak_ptr<SceneProxyResourceBinding::MeshHandle> resource =
            batch.commands.front().payload.resources.meshHandle;
        ASSERT_EQ(store.Apply(batch).addedProxyCount, 1u);
        batch.commands.front().kind = operation;
        batch.commands.front().payload.resources = {};
        (void)store.Apply(batch);
        batch = {};
        EXPECT_FALSE(resource.expired());
        EXPECT_EQ(retirement.markerCount, 1u);
        EXPECT_EQ(store.GetPendingResourceRetireCount(), 1u);
        retirement.fence->signaled.store(true);
        EXPECT_EQ(retirement.executor.ReleaseRetiredBatches(), 1u);
        EXPECT_TRUE(resource.expired());
        EXPECT_EQ(store.GetPendingResourceRetireCount(), 0u);
    }
}

NLS_RENDER_THREAD_TEST(SceneRenderStoreTests, FailedGpuEventPreservesBindingsAfterStoreDestruction)
{
    NLS::Core::ResourceManagement::ResourceLifetimeRegistry registry;
    ProxyRetirementExecutorScope retirement;
    auto store = std::make_unique<SceneRenderStore>(23u);
    auto batch = MakeSingleAddBatch(23u, 0u);
    batch.commands.front().payload.resources = MakeTrackedProxyResources(registry);
    std::weak_ptr<SceneProxyResourceBinding::MeshHandle> resource =
        batch.commands.front().payload.resources.meshHandle;
    ASSERT_EQ(store->Apply(batch).addedProxyCount, 1u);
    batch = {};
    store->Clear();
    retirement.executor.NotifyDeviceLost("Injected scene retirement failure");
    retirement.fence->signaled.store(true);
    EXPECT_EQ(retirement.executor.ReleaseRetiredBatches(), 0u);
    EXPECT_EQ(store->GetPendingResourceRetireCount(), 1u);
    store.reset();
    EXPECT_FALSE(resource.expired());
    EXPECT_EQ(retirement.executor.GetQuarantinedResourceReleaseCount(), 1u);
    retirement.executor.ResetForTesting();
    EXPECT_TRUE(resource.expired());
}

NLS_RENDER_THREAD_TEST(SceneRenderStoreTests, TransformCommandsUpdatePersistentObjectDataSlots)
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

NLS_RENDER_THREAD_TEST(SceneRenderStoreTests, ChangeJournalReportsDeltasAndInvalidatesExpiredReaders)
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

TEST(SceneProxySourceRegistryTests, LightCommandsPreserveWorldTransformAndSizeAcrossUpdates)
{
    using namespace NLS::Maths;
    NLS::Engine::SceneSystem::Scene scene;
    auto& parent = scene.CreateGameObject("LightParent");
    auto& child = scene.CreateGameObject("LightChild");
    child.SetParent(parent);
    child.GetTransform()->SetLocalPosition({2.0f, 1.0f, 3.0f});
    child.GetTransform()->SetLocalRotation(Quaternion(Vector3 {0.0f, 90.0f, 0.0f}));
    child.GetTransform()->SetLocalScale({2.0f, 3.0f, 4.0f});
    auto* light = child.AddComponent<NLS::Engine::Components::LightComponent>();
    ASSERT_NE(light, nullptr);
    light->SetLightType(NLS::Render::Settings::ELightType::DIRECTIONAL);
    light->SetSize({4.0f, 5.0f, 6.0f});

    SceneProxySourceRegistry registry;
    SceneRenderStore store(static_cast<uint64_t>(scene.GetInstanceID()));
    SceneProxyExtractionOptions options;
    uint64_t frameId = 0u;
    uint64_t previousRevision = 0u;
    const auto collectAndApply = [&](const SceneProxyCommandKind expectedKind)
    {
        const auto batch = registry.Collect(scene, options, ++frameId);
        ASSERT_EQ(batch.commands.size(), 1u);
        const auto& command = batch.commands.front();
        EXPECT_EQ(command.kind, expectedKind);
        const auto expectedMatrix = child.GetTransform()->GetWorldMatrix();
        const auto expectedPosition = child.GetTransform()->GetWorldPosition();
        const auto expectedRotation = child.GetTransform()->GetWorldRotation();
        const auto expectedScale = child.GetTransform()->GetWorldScale();
        const auto expectedForward = child.GetTransform()->GetTransform().GetWorldForward();
        const auto expectedSize = light->GetSize();
        EXPECT_TRUE(Matrix4::AreEquals(command.transform.worldMatrix, expectedMatrix));
        for (size_t component = 0u; component < 4u; ++component)
            EXPECT_FLOAT_EQ(command.transform.worldRotation.array[component], expectedRotation.array[component]);
        EXPECT_EQ(command.transform.worldScale, expectedScale);
        EXPECT_GE(command.transform.transformRevision, previousRevision);
        previousRevision = command.transform.transformRevision;
        NLS::Tests::RunOnRenderThread([&]
        {
            const auto stats = store.Apply(batch);
            EXPECT_EQ(stats.staleCommandCount, 0u);
            size_t lightCount = 0u;
            store.ForEachLight([&](const auto& renderLight)
            {
                ++lightCount;
                ASSERT_NE(renderLight.transform, nullptr);
                for (size_t element = 0u; element < 16u; ++element)
                    EXPECT_NEAR(renderLight.transform->GetWorldMatrix().data[element], expectedMatrix.data[element], 1e-5f);
                EXPECT_EQ(renderLight.transform->GetWorldPosition(), expectedPosition);
                for (size_t component = 0u; component < 4u; ++component)
                    EXPECT_FLOAT_EQ(renderLight.transform->GetWorldRotation().array[component], expectedRotation.array[component]);
                EXPECT_EQ(renderLight.transform->GetWorldScale(), expectedScale);
                const auto forward = renderLight.transform->GetWorldForward();
                EXPECT_NEAR(forward.x, expectedForward.x, 1e-5f);
                EXPECT_NEAR(forward.y, expectedForward.y, 1e-5f);
                EXPECT_NEAR(forward.z, expectedForward.z, 1e-5f);
                EXPECT_EQ(renderLight.size, expectedSize);
            });
            EXPECT_EQ(lightCount, 1u);
        });
    };

    collectAndApply(SceneProxyCommandKind::Add);
    child.GetTransform()->SetLocalRotation(Quaternion(Vector3 {30.0f, 45.0f, 0.0f}));
    collectAndApply(SceneProxyCommandKind::UpdateTransform);
    parent.GetTransform()->SetLocalRotation(Quaternion(Vector3 {0.0f, 45.0f, 0.0f}));
    parent.GetTransform()->SetLocalScale({2.0f, 2.0f, 2.0f});
    collectAndApply(SceneProxyCommandKind::UpdateTransform);
    light->SetSize({7.0f, 8.0f, 9.0f});
    collectAndApply(SceneProxyCommandKind::UpdatePayload);
    child.GetTransform()->SetLocalRotation(Quaternion(Vector3 {45.0f, 0.0f, 0.0f}));
    light->SetIntensity(3.0f);
    collectAndApply(SceneProxyCommandKind::UpdatePayload);
    EXPECT_TRUE(registry.Collect(scene, options, ++frameId).Empty());
}

NLS_RENDER_THREAD_TEST(SceneRenderStoreTests, RecreatedLightPreservesDirectionAndShape)
{
    SceneRenderStore store(23u);
    auto batch = MakeSingleAddBatch(23u, 0u);
    auto& command = batch.commands.front();
    command.payload.kind = SceneProxyKind::Light;
    command.payload.ownerAlive = true;
    command.payload.ownerActive = true;
    command.payload.light = SceneProxyLightData {};
    ASSERT_EQ(store.Apply(batch).addedProxyCount, 1u);
    command.kind = SceneProxyCommandKind::Recreate;
    command.payload.light->type = NLS::Render::Settings::ELightType::SPOT;
    command.payload.light->size = {7.0f, 8.0f, 9.0f};
    command.payload.light->cutoff = 20.0f;
    command.payload.light->outerCutoff = 25.0f;
    command.transform.worldPosition = {3.0f, 4.0f, 5.0f};
    command.transform.worldRotation = NLS::Maths::Quaternion(NLS::Maths::Vector3 {90.0f, 0.0f, 0.0f});
    command.transform.worldScale = {2.0f, 3.0f, 4.0f};
    ASSERT_EQ(store.Apply(batch).updatedPayloadCount, 1u);
    size_t lightCount = 0u;
    store.ForEachLight([&](const auto& light)
    {
        ++lightCount;
        EXPECT_EQ(light.type, NLS::Render::Settings::ELightType::SPOT);
        EXPECT_EQ(light.size, command.payload.light->size);
        EXPECT_EQ(light.cutoff, 20.0f);
        EXPECT_EQ(light.outerCutoff, 25.0f);
        ASSERT_NE(light.transform, nullptr);
        EXPECT_EQ(light.transform->GetWorldPosition(), command.transform.worldPosition);
        EXPECT_EQ(light.transform->GetWorldScale(), command.transform.worldScale);
        const auto forward = light.transform->GetWorldForward();
        EXPECT_NEAR(forward.x, 0.0f, 1e-5f);
        EXPECT_NEAR(forward.y, -1.0f, 1e-5f);
        EXPECT_NEAR(forward.z, 0.0f, 1e-5f);
    });
    EXPECT_EQ(lightCount, 1u);
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
    NLS::Tests::SynchronizeSceneForTest(first, scene);
    NLS::Tests::SynchronizeSceneForTest(second, scene);

    // Same underlying store: the handles must alias, which is the property this
    // test pins (two render scenes for one Scene share its proxy store).
    const auto firstStore = first.GetProxyStoreHandle();
    const auto secondStore = second.GetProxyStoreHandle();
    ASSERT_NE(firstStore, nullptr);
    ASSERT_NE(secondStore, nullptr);
    EXPECT_EQ(firstStore.get(), secondStore.get());
    EXPECT_EQ(firstStore->GetProxyCount(), 1u);
    EXPECT_EQ(secondStore->GetProxyCount(), 1u);
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
    ASSERT_EQ(NLS::Tests::SynchronizeSceneForTest(first, scene).syncTouchedPrimitiveCount, 1u);
    ASSERT_EQ(NLS::Tests::SynchronizeSceneForTest(second, scene).syncTouchedPrimitiveCount, 1u);
    const auto initialSnapshot = first.CreatePrimitiveSnapshotForTesting();
    ASSERT_EQ(initialSnapshot.primitiveRecords.size(), 1u);

    gameObject.GetTransform()->SetLocalPosition({4.0f, 0.0f, 0.0f});
    const auto firstChanged = NLS::Tests::SynchronizeSceneForTest(first, scene);
    const auto secondChanged = NLS::Tests::SynchronizeSceneForTest(second, scene);

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

namespace
{
    /** One MeshRenderer-only game object: the minimum a sync can turn into a primitive. */
    NLS::Engine::GameObject& AddSingleMeshRenderer(
        NLS::Engine::SceneSystem::Scene& scene,
        const char* name)
    {
        auto& gameObject = scene.CreateGameObject(name);
        auto* meshFilter = gameObject.AddComponent<NLS::Engine::Components::MeshFilter>();
        gameObject.AddComponent<NLS::Engine::Components::MeshRenderer>();
        meshFilter->SetMesh(nullptr);
        return gameObject;
    }
}

TEST(RenderSceneHandoffTests, OutstandingSynchronizationsAreBoundedAndReleasedBySerial)
{
    NLS::Engine::SceneSystem::Scene scene;
    AddSingleMeshRenderer(scene, "DepthProxy");

    RenderScene renderScene;
    // Default depth one is the alternating hand-off shape: the second capture is
    // refused, exactly as it was before the outstanding set became a queue.
    EXPECT_EQ(renderScene.GetMaxOutstandingSynchronizations(), 1u);
    auto first = renderScene.BeginSynchronize(scene);
    ASSERT_TRUE(first.IsValid());
    EXPECT_EQ(renderScene.GetOutstandingSynchronizationCount(), 1u);
    EXPECT_FALSE(renderScene.BeginSynchronize(scene).IsValid());

    ASSERT_TRUE(NLS::Tests::CompleteSceneSynchronizationForTest(renderScene, std::move(first)).addedPrimitiveCount == 1u);
    EXPECT_EQ(renderScene.GetOutstandingSynchronizationCount(), 0u);

    // Raising the limit lets the capture run ahead, which is what a render-thread
    // consumer needs. Each capture must still be resolvable independently.
    renderScene.SetMaxOutstandingSynchronizations(3u);
    ASSERT_EQ(renderScene.GetMaxOutstandingSynchronizations(), 3u);

    auto ahead1 = renderScene.BeginSynchronize(scene);
    auto ahead2 = renderScene.BeginSynchronize(scene);
    auto ahead3 = renderScene.BeginSynchronize(scene);
    ASSERT_TRUE(ahead1.IsValid());
    ASSERT_TRUE(ahead2.IsValid());
    ASSERT_TRUE(ahead3.IsValid());
    EXPECT_EQ(renderScene.GetOutstandingSynchronizationCount(), 3u);

    // At the limit the next capture is refused rather than queued without bound.
    EXPECT_FALSE(renderScene.BeginSynchronize(scene).IsValid());
    EXPECT_EQ(renderScene.GetOutstandingSynchronizationCount(), 3u);

    // Resolving out of order releases exactly its own claim: the middle one first,
    // which must leave the other two slots occupied.
    (void)NLS::Tests::CompleteSceneSynchronizationForTest(renderScene, std::move(ahead2));
    EXPECT_EQ(renderScene.GetOutstandingSynchronizationCount(), 2u);
    EXPECT_TRUE(renderScene.BeginSynchronize(scene).IsValid());
    EXPECT_EQ(renderScene.GetOutstandingSynchronizationCount(), 3u);

    (void)NLS::Tests::CompleteSceneSynchronizationForTest(renderScene, std::move(ahead1));
    (void)NLS::Tests::CompleteSceneSynchronizationForTest(renderScene, std::move(ahead3));
    EXPECT_EQ(renderScene.GetOutstandingSynchronizationCount(), 1u);

    // A limit of zero would refuse every capture, so it clamps to one.
    renderScene.SetMaxOutstandingSynchronizations(0u);
    EXPECT_EQ(renderScene.GetMaxOutstandingSynchronizations(), 1u);
}

TEST(RenderSceneHandoffTests, CrossThreadCaptureMatchesSingleThreadedSynchronize)
{
    // Two independent scenes so the shared store registry cannot couple them.
    NLS::Engine::SceneSystem::Scene referenceScene;
    NLS::Engine::SceneSystem::Scene handoffScene;
    AddSingleMeshRenderer(referenceScene, "ReferenceProxy");
    AddSingleMeshRenderer(handoffScene, "HandoffProxy");

    RenderScene referenceRenderer;
    const auto referenceStats = NLS::Tests::SynchronizeSceneForTest(referenceRenderer, referenceScene);

    RenderScene handoffRenderer;
    RenderSceneSyncRequest request;
    // Capture on a different thread than the one that completes it: this is the
    // game-thread -> render-thread shape the split exists for.
    std::thread captureThread([&handoffRenderer, &handoffScene, &request]
    {
        request = handoffRenderer.BeginSynchronize(handoffScene);
    });
    captureThread.join();

    ASSERT_TRUE(request.IsValid());
    EXPECT_FALSE(request.canReuseProxySynchronization);
    EXPECT_TRUE(request.sourceNeedsCollection)
        << "A first synchronization has no revision history, so it must collect.";

    const auto handoffStats = NLS::Tests::CompleteSceneSynchronizationForTest(handoffRenderer, std::move(request));

    EXPECT_EQ(handoffStats.syncTouchedPrimitiveCount, referenceStats.syncTouchedPrimitiveCount);
    EXPECT_EQ(handoffStats.addedPrimitiveCount, referenceStats.addedPrimitiveCount);
    EXPECT_EQ(handoffStats.syncFullSweepCount, referenceStats.syncFullSweepCount);
    EXPECT_EQ(handoffStats.removedPrimitiveCount, referenceStats.removedPrimitiveCount);
    EXPECT_EQ(
        handoffRenderer.CreatePrimitiveSnapshotForTesting().primitiveRecords.size(),
        referenceRenderer.CreatePrimitiveSnapshotForTesting().primitiveRecords.size());
}

TEST(RenderSceneHandoffTests, CompleteSynchronizeConsumesTheCapturedBatch)
{
    NLS::Engine::SceneSystem::Scene scene;
    AddSingleMeshRenderer(scene, "BatchProxy");

    RenderScene renderScene;
    auto request = renderScene.BeginSynchronize(scene);
    ASSERT_TRUE(request.IsValid());
    ASSERT_TRUE(request.sourceNeedsCollection);

    // The batch is handed over, not re-collected: completing must not need the
    // scene, so the request is the only input besides the options.
    const auto stats = NLS::Tests::CompleteSceneSynchronizationForTest(renderScene, std::move(request));
    EXPECT_EQ(stats.addedPrimitiveCount, 1u);
    EXPECT_EQ(renderScene.GetPrimitiveCount(), 1u);
}

TEST(RenderSceneHandoffTests, PendingSceneSwitchKeepsEachCapturedStore)
{
    NLS::Engine::SceneSystem::Scene firstScene;
    NLS::Engine::SceneSystem::Scene secondScene;
    AddSingleMeshRenderer(firstScene, "FirstProxy").GetTransform()->SetWorldPosition({ 2.0f, 0.0f, 0.0f });
    AddSingleMeshRenderer(secondScene, "SecondProxy").GetTransform()->SetWorldPosition({ -3.0f, 0.0f, 0.0f });

    RenderScene renderScene;
    renderScene.SetMaxOutstandingSynchronizations(2u);
    NLS::Tests::SynchronizeSceneForTest(renderScene, firstScene);
    const auto firstStore = renderScene.GetProxyStoreHandle();
    NLS::Tests::SynchronizeSceneForTest(renderScene, secondScene);
    const auto secondStore = renderScene.GetProxyStoreHandle();
    NLS::Tests::SynchronizeSceneForTest(renderScene, firstScene);

    auto previousFrame = renderScene.BeginSynchronize(firstScene);
    auto nextFrame = renderScene.BeginSynchronize(secondScene);
    ASSERT_TRUE(previousFrame.IsValid());
    ASSERT_TRUE(nextFrame.IsValid());
    EXPECT_EQ(renderScene.GetProxyStoreHandle(), firstStore)
        << "Capture must not switch the store that the render thread is consuming.";

    NLS::Tests::CompleteSceneSynchronizationForTest(renderScene, std::move(previousFrame));
    EXPECT_EQ(renderScene.GetProxyStoreHandle(), firstStore);
    EXPECT_EQ(renderScene.GetPrimitiveCount(), 1u);
    NLS::Tests::CompleteSceneSynchronizationForTest(renderScene, std::move(nextFrame));
    EXPECT_EQ(renderScene.GetProxyStoreHandle(), secondStore);
    EXPECT_EQ(renderScene.GetPrimitiveCount(), 1u);
    EXPECT_EQ(firstStore->GetProxyCount(), 1u);
    EXPECT_EQ(secondStore->GetProxyCount(), 1u);

    const auto snapshot = renderScene.CreatePrimitiveSnapshotForTesting();
    ASSERT_EQ(snapshot.primitiveRecords.size(), 1u);
    EXPECT_TRUE(NLS::Maths::Matrix4::AreEquals(
        snapshot.primitiveRecords.front().worldMatrix,
        secondScene.GetGameObjects().front()->GetTransform()->GetWorldMatrix()));
}

TEST(RenderSceneHandoffTests, RevisionFastPathSurvivesTheHandoff)
{
    NLS::Engine::SceneSystem::Scene scene;
    AddSingleMeshRenderer(scene, "FastPathProxy");

    RenderSceneSyncOptions options;
    options.trustSceneRenderContentRevision = true;

    RenderScene renderScene;
    const auto first = NLS::Tests::CompleteSceneSynchronizationForTest(renderScene,
        renderScene.BeginSynchronize(scene, options),
        options);
    EXPECT_FALSE(first.usedSceneRenderContentRevisionFastPath)
        << "The first synchronization has no revision history to trust.";

    // Nothing mutated the scene, so the second hand-off must skip collection.
    auto secondRequest = renderScene.BeginSynchronize(scene, options);
    EXPECT_TRUE(secondRequest.canReuseProxySynchronization);
    EXPECT_FALSE(secondRequest.sourceNeedsCollection)
        << "The revision fast path must still be decided during capture.";

    const auto second = NLS::Tests::CompleteSceneSynchronizationForTest(renderScene, std::move(secondRequest), options);
    EXPECT_TRUE(second.usedSceneRenderContentRevisionFastPath);
    EXPECT_FALSE(second.detectedRenderContentChange);
    EXPECT_EQ(second.reusedPrimitiveCount, 1u);
}

TEST(RenderSceneHandoffTests, CompletingAnInvalidRequestIsANoOp)
{
    RenderScene renderScene;
    const RenderSceneSyncRequest invalid;
    ASSERT_FALSE(invalid.IsValid());

    const auto stats = NLS::Tests::CompleteSceneSynchronizationForTest(renderScene, invalid);
    EXPECT_EQ(stats.syncTouchedPrimitiveCount, 0u);
    EXPECT_EQ(stats.usedSceneRenderContentRevisionFastPath, false);
    EXPECT_EQ(renderScene.GetPrimitiveCount(), 0u);
}

TEST(RenderSceneHandoffTests, RefusesASecondCaptureUntilTheOutstandingOneResolves)
{
    NLS::Engine::SceneSystem::Scene scene;
    AddSingleMeshRenderer(scene, "AlternationProxy");

    RenderScene renderScene;
    auto first = renderScene.BeginSynchronize(scene);
    ASSERT_TRUE(first.IsValid());
    const uint64_t firstSerial = first.captureSerial;

    // The resolve mutates the primitive index and the pending declared-texture set
    // that this capture reads, so a second capture must be refused.
    const auto refused = renderScene.BeginSynchronize(scene);
    EXPECT_FALSE(refused.IsValid());

    // Resolving the outstanding capture must restore captures, so a refused one
    // cannot wedge the scene.
    const auto firstStats = NLS::Tests::CompleteSceneSynchronizationForTest(renderScene, std::move(first));
    EXPECT_EQ(firstStats.addedPrimitiveCount, 1u);

    const auto second = renderScene.BeginSynchronize(scene);
    EXPECT_TRUE(second.IsValid()) << "a refused capture must not wedge future captures";
    EXPECT_NE(second.captureSerial, 0u);
    EXPECT_NE(second.captureSerial, firstSerial);
    (void)NLS::Tests::CompleteSceneSynchronizationForTest(renderScene, std::move(second));
}

TEST(RenderSceneHandoffTests, AnInvalidRequestDoesNotResolveAnOutstandingCapture)
{
    NLS::Engine::SceneSystem::Scene scene;
    AddSingleMeshRenderer(scene, "OutstandingProxy");

    RenderScene renderScene;
    auto outstanding = renderScene.BeginSynchronize(scene);
    ASSERT_TRUE(outstanding.IsValid());

    // Completing a request that never claimed a capture must leave the outstanding
    // claim alone, otherwise it would silently re-open the race window.
    const RenderSceneSyncRequest invalid;
    (void)NLS::Tests::CompleteSceneSynchronizationForTest(renderScene, invalid);
    EXPECT_FALSE(renderScene.BeginSynchronize(scene).IsValid())
        << "the original capture is still outstanding";

    (void)NLS::Tests::CompleteSceneSynchronizationForTest(renderScene, std::move(outstanding));
    EXPECT_TRUE(renderScene.BeginSynchronize(scene).IsValid());
}

TEST(RenderSceneHandoffTests, AnUnresolvedCaptureKeepsTheSceneRefusingCaptures)
{
    // Pins the constraint documented on RenderSceneSyncRequest: a capture must be
    // resolved explicitly. It is never resolved implicitly, so a caller that
    // forgets gets a loud refusal instead of a silent race. The intended drop
    // resolution is ApplyDroppedSynchronizeCapture, covered separately.
    NLS::Engine::SceneSystem::Scene scene;
    AddSingleMeshRenderer(scene, "UnresolvedProxy");

    RenderScene renderScene;
    auto unresolved = renderScene.BeginSynchronize(scene);
    ASSERT_TRUE(unresolved.IsValid());

    EXPECT_FALSE(renderScene.BeginSynchronize(scene).IsValid())
        << "an unresolved capture keeps the scene refusing further captures";
    EXPECT_EQ(renderScene.GetPrimitiveCount(), 0u)
        << "nothing may be applied from a capture that was never resolved";
}

TEST(RenderSceneHandoffTests, CombinedSynchronizeRecoversAfterARefusedCapture)
{
    NLS::Engine::SceneSystem::Scene scene;
    AddSingleMeshRenderer(scene, "CombinedProxy");

    RenderScene renderScene;
    auto outstanding = renderScene.BeginSynchronize(scene);
    ASSERT_TRUE(outstanding.IsValid());

    // The combined path must resolve nothing rather than race the outstanding
    // capture, and must recover once that capture resolves.
    EXPECT_EQ(NLS::Tests::SynchronizeSceneForTest(renderScene, scene).addedPrimitiveCount, 0u);
    EXPECT_EQ(renderScene.GetPrimitiveCount(), 0u);

    (void)NLS::Tests::CompleteSceneSynchronizationForTest(renderScene, std::move(outstanding));
    EXPECT_EQ(renderScene.GetPrimitiveCount(), 1u);

    (void)NLS::Tests::SynchronizeSceneForTest(renderScene, scene);
    EXPECT_EQ(renderScene.GetPrimitiveCount(), 1u)
        << "the combined path must stay consistent once captures recover";
}

TEST(RenderSceneHandoffTests, DroppedCaptureAppliesItsUpdatesInsteadOfLosingThem)
{
    NLS::Engine::SceneSystem::Scene scene;
    auto& gameObject = AddSingleMeshRenderer(scene, "DroppedUpdateProxy");

    RenderScene renderScene;
    ASSERT_EQ(NLS::Tests::SynchronizeSceneForTest(renderScene, scene).addedPrimitiveCount, 1u);
    const auto beforeDrop = renderScene.CreatePrimitiveSnapshotForTesting();
    ASSERT_EQ(beforeDrop.primitiveRecords.size(), 1u);

    // Move the object so the next capture carries a transform update.
    gameObject.GetTransform()->SetWorldPosition({ 5.0f, 0.0f, 0.0f });
    const auto dropped = renderScene.BeginSynchronize(scene);
    ASSERT_TRUE(dropped.IsValid());

    // Models a frame dropped between capture and resolution. Collection already
    // advanced the shared registry cursor, so these updates exist nowhere else.
    const auto droppedStats = NLS::Tests::ApplyDroppedSceneCaptureForTest(renderScene, dropped);
    EXPECT_EQ(droppedStats.updatedTransformCount, 1u)
        << "the dropped update must reach the store rather than be discarded";

    // The drop applies only; the primitive rebuild is intentionally skipped
    // because this frame is never gathered or drawn.
    const auto afterDrop = renderScene.CreatePrimitiveSnapshotForTesting();
    ASSERT_EQ(afterDrop.primitiveRecords.size(), 1u);
    EXPECT_TRUE(NLS::Maths::Matrix4::AreEquals(
        beforeDrop.primitiveRecords.front().worldMatrix,
        afterDrop.primitiveRecords.front().worldMatrix))
        << "a dropped frame must not pay for the primitive rebuild";

    // Captures resume immediately after the drop.
    auto next = renderScene.BeginSynchronize(scene);
    ASSERT_TRUE(next.IsValid()) << "a dropped capture must not wedge the scene";
    (void)NLS::Tests::CompleteSceneSynchronizationForTest(renderScene, std::move(next));

    // The render-side primitive converges on the update on the next rendered
    // frame, so nothing was permanently lost.
    const auto converged = renderScene.CreatePrimitiveSnapshotForTesting();
    ASSERT_EQ(converged.primitiveRecords.size(), 1u);
    EXPECT_FALSE(NLS::Maths::Matrix4::AreEquals(
        beforeDrop.primitiveRecords.front().worldMatrix,
        converged.primitiveRecords.front().worldMatrix))
        << "the next rendered frame must pick up the dropped update";
}

TEST(RenderSceneHandoffTests, DroppedCaptureWithoutCommandsStillReleasesTheClaim)
{
    NLS::Engine::SceneSystem::Scene scene;
    AddSingleMeshRenderer(scene, "IdleDropProxy");

    RenderScene renderScene;
    ASSERT_EQ(NLS::Tests::SynchronizeSceneForTest(renderScene, scene).addedPrimitiveCount, 1u);

    // Nothing changed, so this capture carries no commands; a drop of it must
    // still release the claim rather than wedge the scene.
    const auto dropped = renderScene.BeginSynchronize(scene);
    ASSERT_TRUE(dropped.IsValid());
    const auto droppedStats = NLS::Tests::ApplyDroppedSceneCaptureForTest(renderScene, dropped);
    EXPECT_EQ(droppedStats.addedProxyCount, 0u);
    EXPECT_EQ(droppedStats.updatedTransformCount, 0u);

    EXPECT_TRUE(renderScene.BeginSynchronize(scene).IsValid());
}

TEST(RenderSceneHandoffTests, DroppingAnInvalidRequestIsANoOp)
{
    RenderScene renderScene;
    const RenderSceneSyncRequest invalid;
    ASSERT_FALSE(invalid.IsValid());

    const auto stats = NLS::Tests::ApplyDroppedSceneCaptureForTest(renderScene, invalid);
    EXPECT_EQ(stats.addedProxyCount, 0u);
    EXPECT_EQ(renderScene.GetPrimitiveCount(), 0u);
}
