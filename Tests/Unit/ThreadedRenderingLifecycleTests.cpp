#include <gtest/gtest.h>

#include <algorithm>
#include <condition_variable>
#include <chrono>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "Core/ServiceLocator.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Context/SwapchainResizePolicy.h"
#include "Rendering/Context/ThreadedRenderingLifecycle.h"
#include "Rendering/Core/CompositeRenderer.h"
#include "Rendering/Data/FrameDescriptor.h"
#include "Rendering/Entities/Camera.h"
#include "Rendering/DeferredSceneRenderer.h"
#include "Rendering/ForwardSceneRenderer.h"
#include "Rendering/RHI/BindingPointMap.h"
#include "Rendering/RHI/Core/RHICommand.h"
#include "Rendering/RHI/Core/RHIBinding.h"
#include "Rendering/RHI/Core/RHIPipeline.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/RHI/Core/RHIMesh.h"
#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/RHI/Core/RHISwapchain.h"
#include "Rendering/RHI/Utils/DescriptorAllocator/DescriptorAllocator.h"
#include "Rendering/RHI/Utils/PipelineCache/PipelineCache.h"
#include "Rendering/RHI/Utils/ResourceStateTracker/ResourceStateTracker.h"
#include "Rendering/RHI/Utils/UploadContext/UploadContext.h"
#include "Rendering/Settings/DriverSettings.h"
#include "Rendering/BaseSceneRenderer.h"
#include "Components/LightComponent.h"
#include "Components/MeshRenderer.h"
#include "SceneSystem/Scene.h"

namespace
{
    class SnapshotPublishingRenderer : public NLS::Render::Core::CompositeRenderer
    {
    public:
        explicit SnapshotPublishingRenderer(NLS::Render::Context::Driver& driver)
            : CompositeRenderer(driver)
        {
        }

        std::optional<NLS::Render::Context::FrameSnapshot> CaptureFrameSnapshot(
            const NLS::Render::Data::FrameDescriptor& frameDescriptor) const
        {
            return BuildFrameSnapshot(frameDescriptor);
        }
    };

    class VisibleSnapshotPublishingRenderer final : public SnapshotPublishingRenderer
    {
    public:
        using SnapshotPublishingRenderer::SnapshotPublishingRenderer;

    protected:
        std::optional<NLS::Render::Context::FrameSnapshot> BuildFrameSnapshot(
            const NLS::Render::Data::FrameDescriptor& frameDescriptor) const override
        {
            auto snapshot = SnapshotPublishingRenderer::BuildFrameSnapshot(frameDescriptor);
            if (snapshot.has_value())
                snapshot->visibleOpaqueDrawCount = 1u;
            return snapshot;
        }
    };

    struct DescriptorRequiredForPreparedBuilder
    {
        uint64_t sceneActorCount = 0u;
    };

    class DescriptorDependentPreparedBuilderRenderer final : public NLS::Render::Core::CompositeRenderer
    {
    public:
        explicit DescriptorDependentPreparedBuilderRenderer(NLS::Render::Context::Driver& driver)
            : CompositeRenderer(driver)
        {
        }

    protected:
        std::optional<NLS::Render::Context::FrameSnapshot> BuildFrameSnapshot(
            const NLS::Render::Data::FrameDescriptor& frameDescriptor) const override
        {
            auto snapshot = CompositeRenderer::BuildFrameSnapshot(frameDescriptor);
            if (snapshot.has_value())
                snapshot->visibleOpaqueDrawCount = 1u;
            return snapshot;
        }

        NLS::Render::Context::PreparedRenderSceneBuilder BuildPreparedRenderSceneBuilder(
            const NLS::Render::Context::FrameSnapshot& snapshot) const override
        {
            const uint64_t sceneActorCount = HasDescriptor<DescriptorRequiredForPreparedBuilder>()
                ? GetDescriptor<DescriptorRequiredForPreparedBuilder>().sceneActorCount
                : 0u;

            return [snapshot, sceneActorCount]()
            {
                NLS::Render::Context::RenderScenePackage package;
                package.frameId = snapshot.frameId;
                package.hasVisibleDraws = true;
                package.visibleDrawCount = 1u;
                package.frameDataReady = true;
                package.objectDataReady = true;
                package.sceneActorCount = sceneActorCount;
                return package;
            };
        }
    };

    class PreparedDrawSnapshotRenderer final : public SnapshotPublishingRenderer
    {
    public:
        PreparedDrawSnapshotRenderer(
            NLS::Render::Context::Driver& driver,
            std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline> pipeline,
            std::shared_ptr<NLS::Render::RHI::RHIBindingSet> materialBindingSet,
            std::shared_ptr<NLS::Render::RHI::RHIMesh> mesh)
            : SnapshotPublishingRenderer(driver)
            , m_pipeline(std::move(pipeline))
            , m_materialBindingSet(std::move(materialBindingSet))
            , m_mesh(std::move(mesh))
        {
        }

    protected:
        std::optional<NLS::Render::Context::FrameSnapshot> BuildFrameSnapshot(
            const NLS::Render::Data::FrameDescriptor& frameDescriptor) const override
        {
            auto snapshot = SnapshotPublishingRenderer::BuildFrameSnapshot(frameDescriptor);
            if (snapshot.has_value())
                snapshot->visibleOpaqueDrawCount = 1u;
            return snapshot;
        }

        bool PrepareRecordedDraw(
            PipelineState,
            const NLS::Render::Entities::Drawable&,
            PreparedRecordedDraw& outDraw) const override
        {
            outDraw.commandBuffer = GetActiveExplicitCommandBuffer();
            outDraw.pipeline = m_pipeline;
            outDraw.materialBindingSet = m_materialBindingSet;
            outDraw.mesh = m_mesh;
            outDraw.instanceCount = 2u;
            return m_pipeline != nullptr && m_materialBindingSet != nullptr && m_mesh != nullptr;
        }

    private:
        std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline> m_pipeline;
        std::shared_ptr<NLS::Render::RHI::RHIBindingSet> m_materialBindingSet;
        std::shared_ptr<NLS::Render::RHI::RHIMesh> m_mesh;
    };

    class SceneSnapshotRenderer final : public NLS::Engine::Rendering::BaseSceneRenderer
    {
    public:
        explicit SceneSnapshotRenderer(NLS::Render::Context::Driver& driver)
            : BaseSceneRenderer(driver)
        {
        }

        std::optional<NLS::Render::Context::FrameSnapshot> CaptureFrameSnapshot(
            const NLS::Render::Data::FrameDescriptor& frameDescriptor) const
        {
            return BuildFrameSnapshot(frameDescriptor);
        }

        NLS::Render::Context::RenderScenePackage CaptureRenderScenePackage(
            const NLS::Render::Context::FrameSnapshot& snapshot) const
        {
            return BuildRenderScenePackage(snapshot);
        }
    };

    class TestAdapter final : public NLS::Render::RHI::RHIAdapter
    {
    public:
        std::string_view GetDebugName() const override { return "TestAdapter"; }
        NLS::Render::RHI::NativeBackendType GetBackendType() const override { return NLS::Render::RHI::NativeBackendType::None; }
        std::string_view GetVendor() const override { return "TestVendor"; }
        std::string_view GetHardware() const override { return "TestHardware"; }
    };

    class TestFence final : public NLS::Render::RHI::RHIFence
    {
    public:
        std::string_view GetDebugName() const override { return "TestFence"; }
        bool IsSignaled() const override { return signaled; }
        void Reset() override
        {
            signaled = false;
            ++resetCalls;
        }
        bool Wait(uint64_t = 0) override
        {
            ++waitCalls;
            signaled = true;
            return true;
        }

        mutable size_t waitCalls = 0u;
        mutable size_t resetCalls = 0u;
        bool signaled = false;
    };

    class TestSemaphore final : public NLS::Render::RHI::RHISemaphore
    {
    public:
        std::string_view GetDebugName() const override { return "TestSemaphore"; }
        bool IsSignaled() const override { return signaled; }
        void Reset() override
        {
            signaled = false;
            ++resetCalls;
        }

        void* GetNativeSemaphoreHandle() override { return this; }

        mutable size_t resetCalls = 0u;
        bool signaled = false;
    };

    class TestCommandBuffer final : public NLS::Render::RHI::RHICommandBuffer
    {
    public:
        std::string_view GetDebugName() const override { return "TestCommandBuffer"; }
        void Begin() override
        {
            ++beginCalls;
            recording = true;
            events.emplace_back("Begin");
        }
        void End() override
        {
            ++endCalls;
            recording = false;
            events.emplace_back("End");
        }
        void Reset() override
        {
            ++resetCalls;
            recording = false;
        }
        bool IsRecording() const override { return recording; }
        void* GetNativeCommandBuffer() const override { return nullptr; }
        void SetScissor(const NLS::Render::RHI::RHIRect2D&) override {}
        void BindComputePipeline(const std::shared_ptr<NLS::Render::RHI::RHIComputePipeline>&) override {}
        void BindBindingSet(uint32_t setIndex, const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& bindingSet) override
        {
            ++bindBindingSetCalls;
            boundSetIndices.push_back(setIndex);
            boundBindingSets.push_back(bindingSet);
            events.emplace_back("BindBindingSet");
        }
        void PushConstants(NLS::Render::RHI::ShaderStageMask, uint32_t, uint32_t, const void*) override {}
        void BindVertexBuffer(uint32_t, const NLS::Render::RHI::RHIVertexBufferView&) override
        {
            ++bindVertexBufferCalls;
        }
        void BindIndexBuffer(const NLS::Render::RHI::RHIIndexBufferView&) override {}
        void Draw(uint32_t, uint32_t, uint32_t, uint32_t) override
        {
            ++drawCalls;
            events.emplace_back("Draw");
        }
        void DrawIndexed(uint32_t, uint32_t, uint32_t, int32_t, uint32_t) override
        {
            ++drawIndexedCalls;
            events.emplace_back("DrawIndexed");
        }
        void Dispatch(uint32_t, uint32_t, uint32_t) override
        {
            ++dispatchCalls;
            events.emplace_back("Dispatch");
        }
        void CopyBuffer(
            const std::shared_ptr<NLS::Render::RHI::RHIBuffer>&,
            const std::shared_ptr<NLS::Render::RHI::RHIBuffer>&,
            const NLS::Render::RHI::RHIBufferCopyRegion&) override {}
        void CopyBufferToTexture(const NLS::Render::RHI::RHIBufferToTextureCopyDesc&) override {}
        void CopyTexture(const NLS::Render::RHI::RHITextureCopyDesc&) override {}
        void Barrier(const NLS::Render::RHI::RHIBarrierDesc& barrierDesc) override
        {
            ++barrierCalls;
            barrierHistory.push_back(barrierDesc);
            events.emplace_back("Barrier");
        }

        void BeginRenderPass(const NLS::Render::RHI::RHIRenderPassDesc& desc) override
        {
            ++beginRenderPassCalls;
            lastRenderPassDesc = desc;
            events.emplace_back("BeginRenderPass");
        }
        void EndRenderPass() override
        {
            ++endRenderPassCalls;
            events.emplace_back("EndRenderPass");
        }
        void SetViewport(const NLS::Render::RHI::RHIViewport& viewport) override
        {
            ++setViewportCalls;
            lastViewport = viewport;
            events.emplace_back("SetViewport");
        }

        void BindGraphicsPipeline(const std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline>& pipeline) override
        {
            ++bindGraphicsPipelineCalls;
            lastGraphicsPipeline = pipeline;
            events.emplace_back("BindGraphicsPipeline");
        }

        size_t beginCalls = 0u;
        size_t endCalls = 0u;
        size_t resetCalls = 0u;
        size_t beginRenderPassCalls = 0u;
        size_t endRenderPassCalls = 0u;
        size_t setViewportCalls = 0u;
        size_t bindGraphicsPipelineCalls = 0u;
        size_t bindBindingSetCalls = 0u;
        size_t bindVertexBufferCalls = 0u;
        size_t drawCalls = 0u;
        size_t drawIndexedCalls = 0u;
        size_t dispatchCalls = 0u;
        size_t barrierCalls = 0u;
        bool recording = false;
        NLS::Render::RHI::RHIRenderPassDesc lastRenderPassDesc {};
        NLS::Render::RHI::RHIViewport lastViewport {};
        std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline> lastGraphicsPipeline;
        std::vector<uint32_t> boundSetIndices;
        std::vector<std::shared_ptr<NLS::Render::RHI::RHIBindingSet>> boundBindingSets;
        std::vector<NLS::Render::RHI::RHIBarrierDesc> barrierHistory;
        std::vector<std::string> events;
    };

    class TestBuffer final : public NLS::Render::RHI::RHIBuffer
    {
    public:
        explicit TestBuffer(size_t size)
        {
            m_desc.size = size;
        }

        explicit TestBuffer(NLS::Render::RHI::RHIBufferDesc desc)
            : m_desc(std::move(desc))
        {
        }

        std::string_view GetDebugName() const override { return "TestBuffer"; }
        const NLS::Render::RHI::RHIBufferDesc& GetDesc() const override { return m_desc; }
        NLS::Render::RHI::ResourceState GetState() const override { return NLS::Render::RHI::ResourceState::Unknown; }
        uint64_t GetGPUAddress() const override { return 0u; }

    private:
        NLS::Render::RHI::RHIBufferDesc m_desc {};
    };

    class TestBindingSet final : public NLS::Render::RHI::RHIBindingSet
    {
    public:
        explicit TestBindingSet(std::string debugName)
        {
            m_desc.debugName = std::move(debugName);
        }

        std::string_view GetDebugName() const override { return m_desc.debugName; }
        const NLS::Render::RHI::RHIBindingSetDesc& GetDesc() const override { return m_desc; }

    private:
        NLS::Render::RHI::RHIBindingSetDesc m_desc {};
    };

    struct ShutdownOrderProbe
    {
        bool descriptorAllocatorAlive = false;
        bool bindingSetDestroyed = false;
        bool bindingSetDestroyedBeforeDescriptorAllocator = false;
        bool descriptorAllocatorDestroyed = false;
    };

    class ShutdownOrderBindingSet final : public NLS::Render::RHI::RHIBindingSet
    {
    public:
        explicit ShutdownOrderBindingSet(std::shared_ptr<ShutdownOrderProbe> probe)
            : m_probe(std::move(probe))
        {
            m_desc.debugName = "ShutdownOrderBindingSet";
        }

        ~ShutdownOrderBindingSet() override
        {
            if (m_probe != nullptr)
            {
                m_probe->bindingSetDestroyed = true;
                m_probe->bindingSetDestroyedBeforeDescriptorAllocator =
                    m_probe->descriptorAllocatorAlive;
            }
        }

        std::string_view GetDebugName() const override { return m_desc.debugName; }
        const NLS::Render::RHI::RHIBindingSetDesc& GetDesc() const override { return m_desc; }

    private:
        std::shared_ptr<ShutdownOrderProbe> m_probe;
        NLS::Render::RHI::RHIBindingSetDesc m_desc {};
    };

    class TestGraphicsPipeline final : public NLS::Render::RHI::RHIGraphicsPipeline
    {
    public:
        explicit TestGraphicsPipeline(std::string debugName)
        {
            m_desc.debugName = std::move(debugName);
        }

        std::string_view GetDebugName() const override { return m_desc.debugName; }
        const NLS::Render::RHI::RHIGraphicsPipelineDesc& GetDesc() const override { return m_desc; }

    private:
        NLS::Render::RHI::RHIGraphicsPipelineDesc m_desc {};
    };

    class TestComputePipeline final : public NLS::Render::RHI::RHIComputePipeline
    {
    public:
        explicit TestComputePipeline(std::string debugName)
        {
            m_desc.debugName = std::move(debugName);
        }

        std::string_view GetDebugName() const override { return m_desc.debugName; }
        const NLS::Render::RHI::RHIComputePipelineDesc& GetDesc() const override { return m_desc; }

    private:
        NLS::Render::RHI::RHIComputePipelineDesc m_desc {};
    };

    class TestMesh final : public NLS::Render::RHI::RHIMesh
    {
    public:
        TestMesh()
            : m_vertexBuffer(std::make_shared<TestBuffer>(256u))
            , m_indexBuffer(std::make_shared<TestBuffer>(128u))
        {
        }

        std::shared_ptr<NLS::Render::RHI::RHIBuffer> GetVertexBuffer() const override { return m_vertexBuffer; }
        std::shared_ptr<NLS::Render::RHI::RHIBuffer> GetIndexBuffer() const override { return m_indexBuffer; }
        uint32_t GetVertexCount() const override { return 3u; }
        uint32_t GetIndexCount() const override { return 3u; }
        NLS::Render::Settings::EPrimitiveMode GetPrimitiveMode() const override
        {
            return NLS::Render::Settings::EPrimitiveMode::TRIANGLES;
        }
        uint32_t GetVertexStride() const override { return 32u; }
        NLS::Render::RHI::IndexType GetIndexType() const override { return NLS::Render::RHI::IndexType::UInt32; }

    private:
        std::shared_ptr<TestBuffer> m_vertexBuffer;
        std::shared_ptr<TestBuffer> m_indexBuffer;
    };

    class ThreadedDrawCaptureProvider final : public NLS::Render::Core::FrameObjectBindingProvider
    {
    public:
        ThreadedDrawCaptureProvider(
            NLS::Render::Core::CompositeRenderer& renderer,
            std::shared_ptr<NLS::Render::RHI::RHIBindingSet> frameBindingSet,
            std::shared_ptr<NLS::Render::RHI::RHIBindingSet> objectBindingSet)
            : FrameObjectBindingProvider(renderer)
            , m_frameBindingSet(std::move(frameBindingSet))
            , m_objectBindingSet(std::move(objectBindingSet))
        {
        }

        uint64_t captureCalls = 0u;

    protected:
        bool OnCapturePreparedBindingSets(
            PipelineState&,
            const NLS::Render::Entities::Drawable&,
            PreparedBindingSets& outBindings) override
        {
            ++captureCalls;
            outBindings.frameBindingSet = m_frameBindingSet;
            outBindings.objectBindingSet = m_objectBindingSet;
            return true;
        }

    private:
        std::shared_ptr<NLS::Render::RHI::RHIBindingSet> m_frameBindingSet;
        std::shared_ptr<NLS::Render::RHI::RHIBindingSet> m_objectBindingSet;
    };

    class TestCommandPool final : public NLS::Render::RHI::RHICommandPool
    {
    public:
        std::string_view GetDebugName() const override { return "TestCommandPool"; }
        NLS::Render::RHI::QueueType GetQueueType() const override { return queueType; }
        std::shared_ptr<NLS::Render::RHI::RHICommandBuffer> CreateCommandBuffer(std::string = {}) override { return commandBuffer; }
        void Reset() override { ++resetCalls; }

        NLS::Render::RHI::QueueType queueType = NLS::Render::RHI::QueueType::Graphics;
        std::shared_ptr<NLS::Render::RHI::RHICommandBuffer> commandBuffer;
        size_t resetCalls = 0u;
    };

    class TestDescriptorAllocator final : public NLS::Render::RHI::DescriptorAllocator
    {
    public:
        explicit TestDescriptorAllocator(uint64_t transientCapacity = 64u)
            : m_transientCapacity(transientCapacity)
        {
            m_stats.transientCapacity = transientCapacity;
        }

        void BeginFrame(uint64_t frameIndex) override
        {
            ++beginFrameCalls;
            lastBeginFrameIndex = frameIndex;
            m_stats.currentFrameIndex = frameIndex;
        }

        void EndFrame(uint64_t frameIndex) override
        {
            ++endFrameCalls;
            lastEndFrameIndex = frameIndex;
        }

        NLS::Render::RHI::DescriptorAllocation Allocate(const NLS::Render::RHI::DescriptorAllocationRequest& request) override
        {
            if (request.count == 0u)
                return {};

            NLS::Render::RHI::DescriptorAllocation allocation;
            allocation.count = request.count;
            allocation.lifetime = request.lifetime;
            allocation.frameIndex = request.frameIndex;
            allocation.debugName = request.debugName;

            if (request.lifetime == NLS::Render::RHI::DescriptorAllocationLifetime::TransientFrame)
            {
                if (m_transientUsed + request.count > m_transientCapacity)
                {
                    ++m_stats.allocationFailures;
                    return {};
                }

                allocation.offset = m_transientUsed;
                m_transientUsed += request.count;
                m_stats.transientUsed = m_transientUsed;
                m_stats.transientPeak = std::max<uint64_t>(m_stats.transientPeak, m_transientUsed);
                return allocation;
            }

            allocation.offset = m_persistentUsed;
            m_persistentUsed += request.count;
            m_stats.persistentUsed = m_persistentUsed;
            m_stats.persistentPeak = std::max<uint64_t>(m_stats.persistentPeak, m_persistentUsed);
            return allocation;
        }

        void Release(const NLS::Render::RHI::DescriptorAllocation& allocation) override
        {
            if (!allocation.IsValid() ||
                allocation.lifetime != NLS::Render::RHI::DescriptorAllocationLifetime::Persistent)
            {
                return;
            }

            m_persistentUsed = m_persistentUsed >= allocation.count
                ? m_persistentUsed - allocation.count
                : 0u;
            m_stats.persistentUsed = m_persistentUsed;
            m_stats.persistentReleased += allocation.count;
        }

        void Reset() override
        {
            ++resetCalls;
            m_transientUsed = 0u;
            m_persistentUsed = 0u;
            m_stats = {};
            m_stats.transientCapacity = m_transientCapacity;
        }

        NLS::Render::RHI::DescriptorAllocatorStats GetStats() const override { return m_stats; }

        size_t beginFrameCalls = 0u;
        size_t endFrameCalls = 0u;
        size_t resetCalls = 0u;
        uint64_t lastBeginFrameIndex = 0u;
        uint64_t lastEndFrameIndex = 0u;

    private:
        uint64_t m_transientCapacity = 0u;
        uint64_t m_transientUsed = 0u;
        uint64_t m_persistentUsed = 0u;
        NLS::Render::RHI::DescriptorAllocatorStats m_stats{};
    };

    class ShutdownOrderDescriptorAllocator final : public NLS::Render::RHI::DescriptorAllocator
    {
    public:
        explicit ShutdownOrderDescriptorAllocator(std::shared_ptr<ShutdownOrderProbe> probe)
            : m_probe(std::move(probe))
        {
            if (m_probe != nullptr)
                m_probe->descriptorAllocatorAlive = true;
        }

        ~ShutdownOrderDescriptorAllocator() override
        {
            if (m_probe != nullptr)
            {
                m_probe->descriptorAllocatorAlive = false;
                m_probe->descriptorAllocatorDestroyed = true;
            }
        }

        void BeginFrame(uint64_t frameIndex) override { m_stats.currentFrameIndex = frameIndex; }
        void EndFrame(uint64_t) override {}

        NLS::Render::RHI::DescriptorAllocation Allocate(
            const NLS::Render::RHI::DescriptorAllocationRequest& request) override
        {
            NLS::Render::RHI::DescriptorAllocation allocation;
            allocation.offset = m_stats.persistentUsed + m_stats.transientUsed;
            allocation.count = request.count;
            allocation.lifetime = request.lifetime;
            allocation.frameIndex = request.frameIndex;
            allocation.debugName = request.debugName;
            return allocation;
        }

        void Release(const NLS::Render::RHI::DescriptorAllocation&) override {}
        void Reset() override {}
        NLS::Render::RHI::DescriptorAllocatorStats GetStats() const override { return m_stats; }

    private:
        std::shared_ptr<ShutdownOrderProbe> m_probe;
        NLS::Render::RHI::DescriptorAllocatorStats m_stats {};
    };

    class TestUploadContext final : public NLS::Render::RHI::UploadContext
    {
    public:
        void BeginFrame(uint64_t frameIndex) override
        {
            ++beginFrameCalls;
            lastBeginFrameIndex = frameIndex;
        }

        void EndFrame(uint64_t completedFrameIndex) override
        {
            ++endFrameCalls;
            lastEndFrameIndex = completedFrameIndex;
        }

        NLS::Render::RHI::UploadAllocation Allocate(size_t, size_t, std::string) override
        {
            return {};
        }

        bool UploadBuffer(NLS::Render::RHI::RHICommandBuffer&, const NLS::Render::RHI::UploadBufferRequest&) override
        {
            return true;
        }

        bool UploadTexture(NLS::Render::RHI::RHICommandBuffer&, const NLS::Render::RHI::UploadTextureRequest&) override
        {
            return true;
        }

        void CollectGarbage(uint64_t) override {}

        size_t beginFrameCalls = 0u;
        size_t endFrameCalls = 0u;
        uint64_t lastBeginFrameIndex = 0u;
        uint64_t lastEndFrameIndex = 0u;
    };

    class BlockingBeginFrameDescriptorAllocator final : public NLS::Render::RHI::DescriptorAllocator
    {
    public:
        void BeginFrame(uint64_t frameIndex) override
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            ++beginFrameCalls;
            lastBeginFrameIndex = frameIndex;
            ++m_activeBeginFrameCalls;
            maxConcurrentBeginFrameCalls = std::max(maxConcurrentBeginFrameCalls, m_activeBeginFrameCalls);
            if (beginFrameCalls == 1u)
            {
                m_firstBeginFrameEntered = true;
                m_cv.notify_all();
                m_cv.wait(lock, [this]()
                {
                    return m_releaseFirstBeginFrame;
                });
            }
            --m_activeBeginFrameCalls;
            m_cv.notify_all();
        }

        void EndFrame(uint64_t frameIndex) override
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++endFrameCalls;
            lastEndFrameIndex = frameIndex;
        }

        NLS::Render::RHI::DescriptorAllocation Allocate(
            const NLS::Render::RHI::DescriptorAllocationRequest& request) override
        {
            NLS::Render::RHI::DescriptorAllocation allocation;
            allocation.offset = nextOffset++;
            allocation.count = request.count;
            allocation.lifetime = request.lifetime;
            allocation.frameIndex = request.frameIndex;
            allocation.debugName = request.debugName;
            return allocation;
        }

        void Release(const NLS::Render::RHI::DescriptorAllocation&) override {}
        void Reset() override {}
        NLS::Render::RHI::DescriptorAllocatorStats GetStats() const override { return {}; }

        bool WaitForFirstBeginFrame(std::chrono::milliseconds timeout)
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            return m_cv.wait_for(lock, timeout, [this]()
            {
                return m_firstBeginFrameEntered;
            });
        }

        bool WaitForConcurrentBeginFrame(std::chrono::milliseconds timeout)
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            return m_cv.wait_for(lock, timeout, [this]()
            {
                return maxConcurrentBeginFrameCalls > 1u;
            });
        }

        void ReleaseFirstBeginFrame()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_releaseFirstBeginFrame = true;
            m_cv.notify_all();
        }

        size_t GetMaxConcurrentBeginFrameCalls() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return maxConcurrentBeginFrameCalls;
        }

        size_t beginFrameCalls = 0u;
        size_t endFrameCalls = 0u;
        uint64_t lastBeginFrameIndex = 0u;
        uint64_t lastEndFrameIndex = 0u;
        uint64_t nextOffset = 1u;

    private:
        mutable std::mutex m_mutex;
        std::condition_variable m_cv;
        bool m_firstBeginFrameEntered = false;
        bool m_releaseFirstBeginFrame = false;
        size_t m_activeBeginFrameCalls = 0u;
        size_t maxConcurrentBeginFrameCalls = 0u;
    };

    class TestTexture final : public NLS::Render::RHI::RHITexture
    {
    public:
        explicit TestTexture(NLS::Render::RHI::RHITextureDesc desc)
            : m_desc(std::move(desc))
        {
        }

        std::string_view GetDebugName() const override { return m_desc.debugName; }
        const NLS::Render::RHI::RHITextureDesc& GetDesc() const override { return m_desc; }
        NLS::Render::RHI::ResourceState GetState() const override { return NLS::Render::RHI::ResourceState::Unknown; }

    private:
        NLS::Render::RHI::RHITextureDesc m_desc {};
    };

    class TestTextureView final : public NLS::Render::RHI::RHITextureView
    {
    public:
        TestTextureView() = default;

        TestTextureView(
            std::shared_ptr<NLS::Render::RHI::RHITexture> texture,
            NLS::Render::RHI::RHITextureViewDesc desc)
            : m_desc(std::move(desc))
            , m_texture(std::move(texture))
        {
        }

        std::string_view GetDebugName() const override { return "TestTextureView"; }
        const NLS::Render::RHI::RHITextureViewDesc& GetDesc() const override { return m_desc; }
        const std::shared_ptr<NLS::Render::RHI::RHITexture>& GetTexture() const override { return m_texture; }

    private:
        NLS::Render::RHI::RHITextureViewDesc m_desc{};
        std::shared_ptr<NLS::Render::RHI::RHITexture> m_texture;
    };

    class TestSwapchain final : public NLS::Render::RHI::RHISwapchain
    {
    public:
        TestSwapchain()
            : backbufferView(std::make_shared<TestTextureView>())
        {
        }

        std::string_view GetDebugName() const override { return "TestSwapchain"; }
        const NLS::Render::RHI::SwapchainDesc& GetDesc() const override { return desc; }
        uint32_t GetImageCount() const override { return 2u; }
        std::optional<NLS::Render::RHI::RHIAcquiredImage> AcquireNextImage(
            const std::shared_ptr<NLS::Render::RHI::RHISemaphore>&,
            const std::shared_ptr<NLS::Render::RHI::RHIFence>&) override
        {
            ++acquireCalls;
            return NLS::Render::RHI::RHIAcquiredImage{ 1u, backbufferView, false };
        }
        std::shared_ptr<NLS::Render::RHI::RHITextureView> GetBackbufferView(uint32_t index) override
        {
            lastBackbufferIndex = index;
            return backbufferView;
        }
        bool Resize(uint32_t width, uint32_t height) override
        {
            resizeWidth = width;
            resizeHeight = height;
            return true;
        }

        NLS::Render::RHI::SwapchainDesc desc{};
        std::shared_ptr<NLS::Render::RHI::RHITextureView> backbufferView;
        size_t acquireCalls = 0u;
        uint32_t lastBackbufferIndex = 0u;
        uint32_t resizeWidth = 0u;
        uint32_t resizeHeight = 0u;
    };

    class TestQueue final : public NLS::Render::RHI::RHIQueue
    {
    public:
        explicit TestQueue(NLS::Render::RHI::QueueType queueType = NLS::Render::RHI::QueueType::Graphics)
            : m_queueType(queueType)
        {
        }

        std::string_view GetDebugName() const override { return "TestQueue"; }
        NLS::Render::RHI::QueueType GetType() const override { return m_queueType; }
        void Submit(const NLS::Render::RHI::RHISubmitDesc& submitDesc) override
        {
            ++submitCalls;
            lastSubmitDesc = submitDesc;
            submitHistory.push_back(submitDesc);
            for (const auto& semaphore : submitDesc.waitSemaphores)
            {
                auto testSemaphore = std::dynamic_pointer_cast<TestSemaphore>(semaphore);
                if (testSemaphore != nullptr)
                    testSemaphore->signaled = false;
            }
            for (const auto& semaphore : submitDesc.signalSemaphores)
            {
                auto testSemaphore = std::dynamic_pointer_cast<TestSemaphore>(semaphore);
                if (testSemaphore != nullptr)
                    testSemaphore->signaled = true;
            }
        }
        void Present(const NLS::Render::RHI::RHIPresentDesc& presentDesc) override
        {
            ++presentCalls;
            lastPresentDesc = presentDesc;
        }

    private:
        NLS::Render::RHI::QueueType m_queueType = NLS::Render::RHI::QueueType::Graphics;

    public:
        size_t submitCalls = 0u;
        size_t presentCalls = 0u;
        NLS::Render::RHI::RHISubmitDesc lastSubmitDesc {};
        NLS::Render::RHI::RHIPresentDesc lastPresentDesc {};
        std::vector<NLS::Render::RHI::RHISubmitDesc> submitHistory;
    };

    class TestExplicitDevice final : public NLS::Render::RHI::RHIDevice
    {
    public:
        TestExplicitDevice()
            : m_adapter(std::make_shared<TestAdapter>())
            , m_queue(std::make_shared<TestQueue>(NLS::Render::RHI::QueueType::Graphics))
            , m_computeQueue(std::make_shared<TestQueue>(NLS::Render::RHI::QueueType::Compute))
        {
            m_nativeDeviceInfo.backend = NLS::Render::RHI::NativeBackendType::DX12;
            m_capabilities.backendReady = true;
            m_capabilities.supportsGraphics = true;
            m_capabilities.supportsCompute = true;
            m_capabilities.supportsSwapchain = true;
            m_capabilities.supportsCurrentSceneRenderer = true;
            m_capabilities.supportsOffscreenFramebuffers = true;
            m_capabilities.supportsMultiRenderTargets = true;
            m_capabilities.supportsExplicitBarriers = true;
            m_capabilities.supportsCentralizedDescriptorManagement = true;
            m_capabilities.supportsPipelineStateCache = true;
        }

        NLS::Render::RHI::RHIDeviceCapabilities& MutableCapabilities() { return m_capabilities; }
        void SetNativeBackendType(const NLS::Render::RHI::NativeBackendType backend) { m_nativeDeviceInfo.backend = backend; }
        void SetComputeQueue(std::shared_ptr<TestQueue> computeQueue)
        {
            m_computeQueue = std::move(computeQueue);
            m_hasDedicatedComputeQueueOverride = true;
        }
        const std::vector<std::shared_ptr<TestCommandPool>>& GetCreatedCommandPools() const { return m_createdCommandPools; }

        std::string_view GetDebugName() const override { return "TestExplicitDevice"; }
        const std::shared_ptr<NLS::Render::RHI::RHIAdapter>& GetAdapter() const override { return m_adapter; }
        const NLS::Render::RHI::RHIDeviceCapabilities& GetCapabilities() const override { return m_capabilities; }
        NLS::Render::RHI::NativeRenderDeviceInfo GetNativeDeviceInfo() const override { return m_nativeDeviceInfo; }
        bool IsBackendReady() const override { return true; }
        std::shared_ptr<NLS::Render::RHI::RHIQueue> GetQueue(NLS::Render::RHI::QueueType queueType) override
        {
            if (queueType == NLS::Render::RHI::QueueType::Compute)
            {
                if (m_hasDedicatedComputeQueueOverride)
                    return m_computeQueue;
                return m_computeQueue;
            }
            return m_queue;
        }
        std::shared_ptr<TestQueue> GetTestQueue() const { return m_queue; }
        std::shared_ptr<TestQueue> GetComputeTestQueue() const { return m_computeQueue; }
        std::shared_ptr<NLS::Render::RHI::RHISwapchain> CreateSwapchain(const NLS::Render::RHI::SwapchainDesc&) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHIBuffer> CreateBuffer(const NLS::Render::RHI::RHIBufferDesc&, const void* = nullptr) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHITexture> CreateTexture(const NLS::Render::RHI::RHITextureDesc& desc, const void* = nullptr) override
        {
            return std::make_shared<TestTexture>(desc);
        }
        std::shared_ptr<NLS::Render::RHI::RHITextureView> CreateTextureView(
            const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture,
            const NLS::Render::RHI::RHITextureViewDesc& desc) override
        {
            return std::make_shared<TestTextureView>(texture, desc);
        }
        std::shared_ptr<NLS::Render::RHI::RHISampler> CreateSampler(const NLS::Render::RHI::SamplerDesc&, std::string = {}) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHIBindingLayout> CreateBindingLayout(const NLS::Render::RHI::RHIBindingLayoutDesc&) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHIBindingSet> CreateBindingSet(const NLS::Render::RHI::RHIBindingSetDesc& desc) override
        {
            return std::make_shared<TestBindingSet>(
                desc.debugName.empty()
                    ? "TestBindingSet"
                    : desc.debugName);
        }
        std::shared_ptr<NLS::Render::RHI::RHIPipelineLayout> CreatePipelineLayout(const NLS::Render::RHI::RHIPipelineLayoutDesc&) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHIShaderModule> CreateShaderModule(const NLS::Render::RHI::RHIShaderModuleDesc&) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline> CreateGraphicsPipeline(const NLS::Render::RHI::RHIGraphicsPipelineDesc&) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHIComputePipeline> CreateComputePipeline(const NLS::Render::RHI::RHIComputePipelineDesc&) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHICommandPool> CreateCommandPool(NLS::Render::RHI::QueueType queueType, std::string = {}) override
        {
            auto pool = std::make_shared<TestCommandPool>();
            pool->commandBuffer = std::make_shared<TestCommandBuffer>();
            pool->queueType = queueType;
            m_createdCommandPools.push_back(pool);
            return pool;
        }
        std::shared_ptr<NLS::Render::RHI::RHIFence> CreateFence(std::string = {}) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHISemaphore> CreateSemaphore(std::string = {}) override
        {
            return std::make_shared<TestSemaphore>();
        }
        void ReadPixels(
            const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture,
            uint32_t,
            uint32_t,
            uint32_t,
            uint32_t,
            NLS::Render::Settings::EPixelDataFormat,
            NLS::Render::Settings::EPixelDataType,
            void*) override
        {
            lastReadPixelsTexture = texture;
        }

        std::shared_ptr<NLS::Render::RHI::RHITexture> lastReadPixelsTexture;

    private:
        std::shared_ptr<NLS::Render::RHI::RHIAdapter> m_adapter;
        NLS::Render::RHI::RHIDeviceCapabilities m_capabilities {};
        NLS::Render::RHI::NativeRenderDeviceInfo m_nativeDeviceInfo {};
        std::shared_ptr<TestQueue> m_queue;
        std::shared_ptr<TestQueue> m_computeQueue;
        bool m_hasDedicatedComputeQueueOverride = false;
        std::vector<std::shared_ptr<TestCommandPool>> m_createdCommandPools;
    };

    std::vector<NLS::Render::Context::InFlightFrameSlot> WaitForRetiredCopiedSlots(
        const NLS::Render::Context::ThreadedRenderingLifecycle& lifecycle)
    {
        for (int attempt = 0; attempt < 100; ++attempt)
        {
            auto copiedSlots = lifecycle.CopySlots();
            for (const auto& slot : copiedSlots)
            {
                if (slot.stage == NLS::Render::Context::ThreadedFrameStage::Retired)
                    return copiedSlots;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }

        return lifecycle.CopySlots();
    }

    std::shared_ptr<TestCommandBuffer> GetSubmittedTestCommandBuffer(const std::shared_ptr<TestQueue>& queue, size_t index = 0u)
    {
        if (queue == nullptr || queue->lastSubmitDesc.commandBuffers.size() <= index)
            return nullptr;

        return std::dynamic_pointer_cast<TestCommandBuffer>(queue->lastSubmitDesc.commandBuffers[index]);
    }

    std::shared_ptr<TestCommandBuffer> FindSubmittedTestCommandBuffer(
        const std::shared_ptr<TestQueue>& queue,
        const std::function<bool(const TestCommandBuffer&)>& predicate)
    {
        if (queue == nullptr)
            return nullptr;

        for (const auto& commandBuffer : queue->lastSubmitDesc.commandBuffers)
        {
            auto testCommandBuffer = std::dynamic_pointer_cast<TestCommandBuffer>(commandBuffer);
            if (testCommandBuffer != nullptr && predicate(*testCommandBuffer))
                return testCommandBuffer;
        }

        return nullptr;
    }
}

TEST(ThreadedRenderingLifecycleTests, PublishesSnapshotIntoOpenSlotAndTracksInFlightDepth)
{
    NLS::Render::Context::ThreadedRenderingLifecycle lifecycle(2u);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 1u;
    snapshot.renderWidth = 128u;
    snapshot.renderHeight = 64u;

    size_t publishedSlot = 99u;
    EXPECT_TRUE(lifecycle.TryPublishFrameSnapshot(snapshot, &publishedSlot));
    EXPECT_EQ(publishedSlot, 0u);
    EXPECT_EQ(lifecycle.GetInFlightDepth(), 1u);
    EXPECT_FALSE(lifecycle.IsBackPressured());

    const auto* slot = lifecycle.PeekSlot(0u);
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(slot->stage, NLS::Render::Context::ThreadedFrameStage::Published);
    ASSERT_TRUE(slot->snapshot.has_value());
    EXPECT_EQ(slot->snapshot->frameId, 1u);
}

TEST(ThreadedRenderingLifecycleTests, CopiesSlotStateForDiagnosticsWithoutExposingInternalPointer)
{
    NLS::Render::Context::ThreadedRenderingLifecycle lifecycle(1u);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 7u;
    snapshot.renderWidth = 320u;
    snapshot.renderHeight = 180u;

    ASSERT_TRUE(lifecycle.TryPublishFrameSnapshot(snapshot));

    const auto copiedSlot = lifecycle.CopySlot(0u);
    ASSERT_TRUE(copiedSlot.has_value());
    EXPECT_EQ(copiedSlot->slotIndex, 0u);
    EXPECT_EQ(copiedSlot->stage, NLS::Render::Context::ThreadedFrameStage::Published);
    ASSERT_TRUE(copiedSlot->snapshot.has_value());
    EXPECT_EQ(copiedSlot->snapshot->frameId, 7u);
    EXPECT_EQ(copiedSlot->snapshot->renderWidth, 320u);
    EXPECT_EQ(copiedSlot->snapshot->renderHeight, 180u);

    NLS::Render::Context::RenderScenePackage package;
    package.frameId = 7u;
    ASSERT_TRUE(lifecycle.TryBeginRenderScene(0u));
    ASSERT_TRUE(lifecycle.CompleteRenderScene(0u, package));

    EXPECT_EQ(copiedSlot->stage, NLS::Render::Context::ThreadedFrameStage::Published);
    EXPECT_FALSE(copiedSlot->renderScenePackage.has_value());

    const auto updatedSlot = lifecycle.CopySlot(0u);
    ASSERT_TRUE(updatedSlot.has_value());
    EXPECT_EQ(updatedSlot->stage, NLS::Render::Context::ThreadedFrameStage::RenderReady);
    ASSERT_TRUE(updatedSlot->renderScenePackage.has_value());
    EXPECT_EQ(updatedSlot->renderScenePackage->frameId, 7u);
}

TEST(ThreadedRenderingLifecycleTests, CopiesAllSlotsForDiagnostics)
{
    NLS::Render::Context::ThreadedRenderingLifecycle lifecycle(2u);

    NLS::Render::Context::FrameSnapshot firstSnapshot;
    firstSnapshot.frameId = 11u;
    NLS::Render::Context::FrameSnapshot secondSnapshot;
    secondSnapshot.frameId = 12u;

    ASSERT_TRUE(lifecycle.TryPublishFrameSnapshot(firstSnapshot));
    ASSERT_TRUE(lifecycle.TryPublishFrameSnapshot(secondSnapshot));

    const auto copiedSlots = lifecycle.CopySlots();
    ASSERT_EQ(copiedSlots.size(), 2u);
    ASSERT_TRUE(copiedSlots[0].snapshot.has_value());
    ASSERT_TRUE(copiedSlots[1].snapshot.has_value());
    EXPECT_EQ(copiedSlots[0].snapshot->frameId, 11u);
    EXPECT_EQ(copiedSlots[1].snapshot->frameId, 12u);

    NLS::Render::Context::RenderScenePackage package;
    package.frameId = 11u;
    ASSERT_TRUE(lifecycle.TryBeginRenderScene(0u));
    ASSERT_TRUE(lifecycle.CompleteRenderScene(0u, package));

    EXPECT_EQ(copiedSlots[0].stage, NLS::Render::Context::ThreadedFrameStage::Published);
    EXPECT_FALSE(copiedSlots[0].renderScenePackage.has_value());
}

TEST(ThreadedRenderingLifecycleTests, GameThreadPublicationProducesImmutableRenderFrameInputArtifact)
{
    NLS::Render::Context::ThreadedRenderingLifecycle lifecycle(1u);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 601u;
    snapshot.sceneRevision = 17u;
    snapshot.renderWidth = 1920u;
    snapshot.renderHeight = 1080u;
    snapshot.targetsSwapchain = true;
    snapshot.hasSceneInput = true;
    snapshot.sceneActorCount = 9u;
    snapshot.visibleOpaqueDrawCount = 3u;
    snapshot.visibleSkyboxDrawCount = 1u;

    ASSERT_TRUE(lifecycle.TryPublishFrameSnapshot(snapshot));

    size_t slotIndex = std::numeric_limits<size_t>::max();
    NLS::Render::Context::RenderFrameInput renderFrameInput;
    ASSERT_TRUE(lifecycle.TryBeginNextRenderFrameBuild(&slotIndex, &renderFrameInput));

    EXPECT_EQ(slotIndex, 0u);
    EXPECT_TRUE(renderFrameInput.immutable);
    EXPECT_EQ(renderFrameInput.frameId, snapshot.frameId);
    EXPECT_EQ(renderFrameInput.sceneRevision, snapshot.sceneRevision);
    EXPECT_EQ(renderFrameInput.renderWidth, snapshot.renderWidth);
    EXPECT_EQ(renderFrameInput.renderHeight, snapshot.renderHeight);
    EXPECT_TRUE(renderFrameInput.targetsSwapchain);
    EXPECT_TRUE(renderFrameInput.hasSceneInput);
    EXPECT_EQ(renderFrameInput.sceneActorCount, snapshot.sceneActorCount);
    EXPECT_EQ(renderFrameInput.visibleDrawCount, 4u);

    const auto copiedSlot = lifecycle.CopySlot(slotIndex);
    ASSERT_TRUE(copiedSlot.has_value());
    ASSERT_TRUE(copiedSlot->renderFrameInput.has_value());
    EXPECT_TRUE(copiedSlot->renderFrameInput->immutable);
    EXPECT_FALSE(copiedSlot->renderFrameBuild.has_value());
    EXPECT_EQ(copiedSlot->stage, NLS::Render::Context::ThreadedFrameStage::RenderScenePreparing);
}

TEST(ThreadedRenderingLifecycleTests, GameThreadPublicationCarriesExternalOutputHandoffIntoRenderFrameInput)
{
    NLS::Render::Context::ThreadedRenderingLifecycle lifecycle(1u);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 602u;
    snapshot.sceneRevision = 18u;
    snapshot.renderWidth = 1280u;
    snapshot.renderHeight = 720u;
    snapshot.targetsSwapchain = false;
    snapshot.hasExternalOutput = true;
    snapshot.externalOutputTextureCount = 2u;
    snapshot.hasSceneInput = true;
    snapshot.visibleOpaqueDrawCount = 1u;

    ASSERT_TRUE(lifecycle.TryPublishFrameSnapshot(snapshot));

    size_t slotIndex = std::numeric_limits<size_t>::max();
    NLS::Render::Context::RenderFrameInput renderFrameInput;
    ASSERT_TRUE(lifecycle.TryBeginNextRenderFrameBuild(&slotIndex, &renderFrameInput));

    EXPECT_EQ(slotIndex, 0u);
    EXPECT_FALSE(renderFrameInput.targetsSwapchain);
    EXPECT_TRUE(renderFrameInput.hasExternalOutput);
    EXPECT_EQ(renderFrameInput.externalOutputTextureCount, 2u);

    const auto copiedSlot = lifecycle.CopySlot(slotIndex);
    ASSERT_TRUE(copiedSlot.has_value());
    ASSERT_TRUE(copiedSlot->renderFrameInput.has_value());
    EXPECT_TRUE(copiedSlot->renderFrameInput->hasExternalOutput);
    EXPECT_EQ(copiedSlot->renderFrameInput->externalOutputTextureCount, 2u);
}

TEST(ThreadedRenderingLifecycleTests, RenderThreadCompletionProducesRenderFrameBuildArtifactForRhiConsumption)
{
    NLS::Render::Context::ThreadedRenderingLifecycle lifecycle(1u);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 777u;
    snapshot.targetsSwapchain = false;
    snapshot.visibleOpaqueDrawCount = 2u;
    ASSERT_TRUE(lifecycle.TryPublishFrameSnapshot(snapshot));

    size_t renderSlotIndex = std::numeric_limits<size_t>::max();
    NLS::Render::Context::RenderFrameInput renderFrameInput;
    ASSERT_TRUE(lifecycle.TryBeginNextRenderFrameBuild(&renderSlotIndex, &renderFrameInput));

    NLS::Render::Context::RenderScenePackage package;
    package.frameId = renderFrameInput.frameId;
    package.targetsSwapchain = false;
    package.hasVisibleDraws = true;
    package.frameDataReady = true;
    package.objectDataReady = true;
    package.lightingDataReady = false;
    package.visibleDrawCount = 2u;
    package.passPlanCount = 1u;
    package.drawCommandCount = 2u;
    package.containsParallelCommandWorkUnits = true;
    ASSERT_TRUE(lifecycle.CompleteRenderScene(renderSlotIndex, package));

    size_t rhiSlotIndex = std::numeric_limits<size_t>::max();
    NLS::Render::Context::RenderFrameBuild renderFrameBuild;
    ASSERT_TRUE(lifecycle.TryBeginNextRhiFrameExecution(&rhiSlotIndex, &renderFrameBuild));

    EXPECT_EQ(rhiSlotIndex, renderSlotIndex);
    EXPECT_EQ(renderFrameBuild.frameId, package.frameId);
    EXPECT_TRUE(renderFrameBuild.renderThreadOwned);
    EXPECT_FALSE(renderFrameBuild.targetsSwapchain);
    EXPECT_TRUE(renderFrameBuild.hasVisibleDraws);
    EXPECT_TRUE(renderFrameBuild.frameDataReady);
    EXPECT_TRUE(renderFrameBuild.objectDataReady);
    EXPECT_FALSE(renderFrameBuild.lightingDataReady);
    EXPECT_EQ(renderFrameBuild.visibleDrawCount, package.visibleDrawCount);
    EXPECT_EQ(renderFrameBuild.passPlanCount, package.passPlanCount);
    EXPECT_EQ(renderFrameBuild.drawCommandCount, package.drawCommandCount);
    EXPECT_TRUE(renderFrameBuild.containsParallelCommandWorkUnits);

    const auto copiedSlot = lifecycle.CopySlot(rhiSlotIndex);
    ASSERT_TRUE(copiedSlot.has_value());
    ASSERT_TRUE(copiedSlot->renderFrameBuild.has_value());
    EXPECT_EQ(copiedSlot->renderFrameBuild->frameId, package.frameId);
    EXPECT_EQ(copiedSlot->stage, NLS::Render::Context::ThreadedFrameStage::RhiSubmitting);
}

TEST(ThreadedRenderingLifecycleTests, PreparedFramePublishesIntoRenderSceneOwnedStage)
{
    NLS::Render::Context::ThreadedRenderingLifecycle lifecycle(1u);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 21u;
    snapshot.visibleOpaqueDrawCount = 1u;

    NLS::Render::Context::RenderScenePackage renderScenePackage;
    renderScenePackage.frameId = 21u;
    renderScenePackage.visibleDrawCount = 1u;
    renderScenePackage.hasVisibleDraws = true;

    size_t publishedSlot = 99u;
    ASSERT_TRUE(lifecycle.TryPublishPreparedFrame(snapshot, renderScenePackage, &publishedSlot));
    EXPECT_EQ(publishedSlot, 0u);

    const auto publishedSlotState = lifecycle.CopySlot(0u);
    ASSERT_TRUE(publishedSlotState.has_value());
    EXPECT_EQ(publishedSlotState->stage, NLS::Render::Context::ThreadedFrameStage::Published);
    EXPECT_EQ(publishedSlotState->publishOrigin, NLS::Render::Context::ThreadedFramePublishOrigin::PreparedPackage);
    EXPECT_EQ(publishedSlotState->renderSceneAttribution, NLS::Render::Context::RenderSceneAttribution::Unknown);
    EXPECT_TRUE(publishedSlotState->preparedRenderSceneBuilder.has_value());
    EXPECT_FALSE(publishedSlotState->renderScenePackage.has_value());

    size_t renderSceneSlotIndex = 99u;
    NLS::Render::Context::FrameSnapshot claimedSnapshot;
    ASSERT_TRUE(lifecycle.TryBeginNextRenderScene(&renderSceneSlotIndex, &claimedSnapshot));
    EXPECT_EQ(renderSceneSlotIndex, 0u);
    EXPECT_EQ(claimedSnapshot.frameId, 21u);

    const auto preparingSlotState = lifecycle.CopySlot(0u);
    ASSERT_TRUE(preparingSlotState.has_value());
    EXPECT_EQ(preparingSlotState->stage, NLS::Render::Context::ThreadedFrameStage::RenderScenePreparing);
    EXPECT_TRUE(preparingSlotState->preparedRenderSceneBuilder.has_value());
    EXPECT_FALSE(preparingSlotState->renderScenePackage.has_value());

    NLS::Render::Context::RenderScenePreparingResolutionDesc resolutionDesc;
    ASSERT_TRUE(lifecycle.ResolveRenderScenePreparing(0u, resolutionDesc));

    const auto renderReadySlotState = lifecycle.CopySlot(0u);
    ASSERT_TRUE(renderReadySlotState.has_value());
    EXPECT_EQ(renderReadySlotState->stage, NLS::Render::Context::ThreadedFrameStage::RenderReady);
    EXPECT_EQ(renderReadySlotState->renderSceneAttribution, NLS::Render::Context::RenderSceneAttribution::RendererPrepared);
    ASSERT_TRUE(renderReadySlotState->renderScenePackage.has_value());
    EXPECT_EQ(renderReadySlotState->renderScenePackage->frameId, 21u);
}

TEST(ThreadedRenderingLifecycleTests, ReportsBackPressureWhenAllSlotsAreOccupied)
{
    NLS::Render::Context::ThreadedRenderingLifecycle lifecycle(1u);

    NLS::Render::Context::FrameSnapshot firstSnapshot;
    firstSnapshot.frameId = 1u;

    NLS::Render::Context::FrameSnapshot secondSnapshot;
    secondSnapshot.frameId = 2u;

    EXPECT_TRUE(lifecycle.TryPublishFrameSnapshot(firstSnapshot));
    EXPECT_FALSE(lifecycle.TryPublishFrameSnapshot(secondSnapshot));
    EXPECT_TRUE(lifecycle.IsBackPressured());
    EXPECT_EQ(lifecycle.GetBlockedPublishCount(), 1u);
}

TEST(ThreadedRenderingLifecycleTests, RetiresSlotsBeforeTheyCanBeReused)
{
    NLS::Render::Context::ThreadedRenderingLifecycle lifecycle(1u);

    NLS::Render::Context::FrameSnapshot firstSnapshot;
    firstSnapshot.frameId = 1u;

    NLS::Render::Context::RenderScenePackage scenePackage;
    scenePackage.frameId = 1u;

    NLS::Render::Context::RhiSubmissionFrame submissionFrame;
    submissionFrame.frameId = 1u;

    size_t publishedSlot = 0u;
    ASSERT_TRUE(lifecycle.TryPublishFrameSnapshot(firstSnapshot, &publishedSlot));
    ASSERT_TRUE(lifecycle.TryBeginRenderScene(publishedSlot));
    ASSERT_TRUE(lifecycle.CompleteRenderScene(publishedSlot, scenePackage));
    ASSERT_TRUE(lifecycle.TryBeginRhiSubmission(publishedSlot));
    ASSERT_TRUE(lifecycle.CompleteRhiSubmission(publishedSlot, submissionFrame));
    ASSERT_TRUE(lifecycle.RetireFrame(publishedSlot));

    NLS::Render::Context::FrameSnapshot secondSnapshot;
    secondSnapshot.frameId = 2u;

    EXPECT_TRUE(lifecycle.TryPublishFrameSnapshot(secondSnapshot, &publishedSlot));
    EXPECT_EQ(publishedSlot, 0u);
    EXPECT_EQ(lifecycle.GetInFlightDepth(), 1u);
}

TEST(ThreadedRenderingLifecycleTests, CompositeRendererPublishesThreadedFrameTelemetryWhenThreadedModeIsEnabled)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 2u;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    NLS::Core::ServiceLocator::Provide(*driver);

    VisibleSnapshotPublishingRenderer renderer(*driver);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 64u;
    frameDescriptor.renderHeight = 64u;
    frameDescriptor.camera = &camera;

    renderer.BeginFrame(frameDescriptor);
    renderer.EndFrame();

    const auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(*driver);
    ASSERT_NE(lifecycle, nullptr);
    EXPECT_EQ(lifecycle->GetPublishedFrameCount(), 1u);
    EXPECT_EQ(lifecycle->GetInFlightDepth(), 1u);

    const auto telemetry = NLS::Render::Context::DriverRendererAccess::GetThreadedFrameTelemetry(*driver);
    EXPECT_EQ(telemetry.inFlightFrameCount, 1u);
    EXPECT_EQ(telemetry.blockedPublishCount, 0u);
    EXPECT_EQ(telemetry.publishState, NLS::Render::Data::FramePublishState::Open);
}

TEST(ThreadedRenderingLifecycleTests, TelemetryTracksLatestPublishedAndRetiredFrameIds)
{
    NLS::Render::Context::ThreadedRenderingLifecycle lifecycle(1u);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 41u;

    size_t slotIndex = 0u;
    ASSERT_TRUE(lifecycle.TryPublishFrameSnapshot(snapshot, &slotIndex));

    auto telemetry = lifecycle.GetTelemetry();
    EXPECT_EQ(telemetry.latestPublishedFrameId, 41u);
    EXPECT_EQ(telemetry.latestRetiredFrameId, 0u);

    ASSERT_TRUE(lifecycle.TryBeginRenderScene(slotIndex));

    NLS::Render::Context::RenderScenePackage package;
    package.frameId = 41u;
    ASSERT_TRUE(lifecycle.CompleteRenderScene(slotIndex, package));
    ASSERT_TRUE(lifecycle.TryBeginRhiSubmission(slotIndex));

    NLS::Render::Context::RhiSubmissionFrame submissionFrame;
    submissionFrame.frameId = 41u;
    ASSERT_TRUE(lifecycle.CompleteRhiSubmission(slotIndex, submissionFrame));
    ASSERT_TRUE(lifecycle.RetireFrame(slotIndex));

    telemetry = lifecycle.GetTelemetry();
    EXPECT_EQ(telemetry.latestPublishedFrameId, 41u);
    EXPECT_EQ(telemetry.latestRetiredFrameId, 41u);
}

TEST(ThreadedRenderingLifecycleTests, CompositeRendererKeepsFrameDescriptorsUntilPreparedBuilderIsCaptured)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    DescriptorDependentPreparedBuilderRenderer renderer(driver);
    renderer.AddDescriptor<DescriptorRequiredForPreparedBuilder>({ 7u });

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 64u;
    frameDescriptor.renderHeight = 64u;
    frameDescriptor.camera = &camera;

    renderer.BeginFrame(frameDescriptor);
    renderer.EndFrame();

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    const auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);
    const auto* retiredSlot = lifecycle->PeekSlot(0u);
    ASSERT_NE(retiredSlot, nullptr);
    ASSERT_TRUE(retiredSlot->renderScenePackage.has_value());
    EXPECT_EQ(retiredSlot->renderScenePackage->sceneActorCount, 7u);
}

TEST(ThreadedRenderingLifecycleTests, ThreadedRenderingIsDisabledForNonTierAExplicitBackends)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    explicitDevice->SetNativeBackendType(NLS::Render::RHI::NativeBackendType::OpenGL);
    explicitDevice->MutableCapabilities().backendReady = false;
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    EXPECT_FALSE(NLS::Render::Context::DriverRendererAccess::IsThreadedRenderingEnabled(driver));
}

TEST(ThreadedRenderingLifecycleTests, ThreadedRenderingStaysEnabledForTierAFoundationBackends)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    explicitDevice->SetNativeBackendType(NLS::Render::RHI::NativeBackendType::DX12);
    auto& capabilities = explicitDevice->MutableCapabilities();
    capabilities.supportsCompute = true;
    capabilities.supportsSwapchain = true;
    capabilities.supportsCurrentSceneRenderer = true;
    capabilities.supportsOffscreenFramebuffers = true;
    capabilities.supportsMultiRenderTargets = true;
    capabilities.supportsExplicitBarriers = true;
    capabilities.supportsCentralizedDescriptorManagement = true;
    capabilities.supportsPipelineStateCache = true;
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    EXPECT_TRUE(NLS::Render::Context::DriverRendererAccess::IsThreadedRenderingEnabled(driver));
}

TEST(ThreadedRenderingLifecycleTests, ThreadedSnapshotHarnessPublishStaysAvailableForNoneRequestedBackendHarness)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 301u;
    snapshot.visibleOpaqueDrawCount = 1u;

    EXPECT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessFrameSnapshot(driver, snapshot));

    const auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);
    EXPECT_EQ(lifecycle->GetInFlightDepth(), 1u);
    const auto* slot = lifecycle->PeekSlot(0u);
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(slot->stage, NLS::Render::Context::ThreadedFrameStage::Published);
}

TEST(ThreadedRenderingLifecycleTests, ThreadedPreparedFrameHarnessPublishStaysAvailableForNoneRequestedBackendHarness)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 302u;
    snapshot.visibleOpaqueDrawCount = 1u;

    NLS::Render::Context::RenderScenePackage renderScenePackage;
    renderScenePackage.frameId = 302u;
    renderScenePackage.visibleDrawCount = 1u;
    renderScenePackage.hasVisibleDraws = true;

    EXPECT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessPreparedFrame(
        driver,
        snapshot,
        renderScenePackage));

    const auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);
    EXPECT_EQ(lifecycle->GetInFlightDepth(), 1u);
    const auto* slot = lifecycle->PeekSlot(0u);
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(slot->stage, NLS::Render::Context::ThreadedFrameStage::Published);
}

TEST(ThreadedRenderingLifecycleTests, ThreadedSnapshotHarnessPublishIsRejectedForTierARequestedBackends)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::DX12;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 303u;
    snapshot.visibleOpaqueDrawCount = 1u;

    EXPECT_FALSE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessFrameSnapshot(driver, snapshot));

    const auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);
    EXPECT_EQ(lifecycle->GetInFlightDepth(), 0u);
    const auto* slot = lifecycle->PeekSlot(0u);
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(slot->stage, NLS::Render::Context::ThreadedFrameStage::Available);
}

TEST(ThreadedRenderingLifecycleTests, ThreadedPreparedPackageHarnessPublishIsRejectedForTierARequestedBackends)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::DX12;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 304u;
    snapshot.visibleOpaqueDrawCount = 1u;

    NLS::Render::Context::RenderScenePackage renderScenePackage;
    renderScenePackage.frameId = 304u;
    renderScenePackage.visibleDrawCount = 1u;
    renderScenePackage.hasVisibleDraws = true;

    EXPECT_FALSE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessPreparedFrame(
        driver,
        snapshot,
        renderScenePackage));

    const auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);
    EXPECT_EQ(lifecycle->GetInFlightDepth(), 0u);
    const auto* slot = lifecycle->PeekSlot(0u);
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(slot->stage, NLS::Render::Context::ThreadedFrameStage::Available);
}

TEST(ThreadedRenderingLifecycleTests, ThreadedSnapshotHarnessPublishIsRejectedForUnsupportedRequestedBackends)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::OPENGL;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 305u;
    snapshot.visibleOpaqueDrawCount = 1u;

    EXPECT_FALSE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessFrameSnapshot(driver, snapshot));

    const auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);
    EXPECT_EQ(lifecycle->GetInFlightDepth(), 0u);
    const auto* slot = lifecycle->PeekSlot(0u);
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(slot->stage, NLS::Render::Context::ThreadedFrameStage::Available);
}

TEST(ThreadedRenderingLifecycleTests, DriverHarnessScenePackageResolutionIsRejected)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::DX12;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 306u;
    snapshot.hasSceneInput = true;
    snapshot.visibleOpaqueDrawCount = 2u;

    size_t slotIndex = 99u;
    ASSERT_TRUE(lifecycle->TryPublishFrameSnapshot(snapshot, &slotIndex));
    ASSERT_TRUE(lifecycle->TryBeginRenderScene(slotIndex));

    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::ResolveAndCompleteThreadedRenderScene(
        driver,
        slotIndex));

    const auto* slot = lifecycle->PeekSlot(slotIndex);
    ASSERT_NE(slot, nullptr);
    ASSERT_TRUE(slot->renderScenePackage.has_value());
    EXPECT_EQ(
        slot->renderSceneAttribution,
        NLS::Render::Context::RenderSceneAttribution::SnapshotHarness);
    EXPECT_EQ(slot->renderScenePackage->visibleDrawCount, 0u);
    EXPECT_TRUE(slot->renderScenePackage->passCommandInputs.empty());
}

TEST(ThreadedRenderingLifecycleTests, ThreadedRendererNeverUsesStandaloneExplicitFrameRecordingForRuntimeVisibility)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    settings.framesInFlight = 1u;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    NLS::Core::ServiceLocator::Provide(*driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(*driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(*driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(*driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    auto frameFence = std::make_shared<TestFence>();
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = frameFence;
    commandPool->commandBuffer = commandBuffer;

    SnapshotPublishingRenderer renderer(*driver);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 32u;
    frameDescriptor.renderHeight = 32u;
    frameDescriptor.camera = &camera;

    renderer.BeginFrame(frameDescriptor);

    EXPECT_EQ(NLS::Render::Context::DriverRendererAccess::GetActiveExplicitCommandBuffer(*driver), nullptr);
    EXPECT_EQ(frameFence->waitCalls, 0u);
    EXPECT_EQ(frameFence->resetCalls, 0u);
    EXPECT_EQ(commandPool->resetCalls, 0u);
    EXPECT_EQ(commandBuffer->resetCalls, 0u);
    EXPECT_EQ(commandBuffer->beginCalls, 0u);

    renderer.EndFrame();

    EXPECT_EQ(commandBuffer->endCalls, 0u);
    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 0u);
    EXPECT_EQ(explicitDevice->GetTestQueue()->presentCalls, 0u);
    const auto telemetry = NLS::Render::Context::DriverRendererAccess::GetThreadedFrameTelemetry(*driver);
    EXPECT_EQ(telemetry.publishedFrameCount, 1u);
    EXPECT_EQ(telemetry.inFlightFrameCount, 1u);
}

TEST(ThreadedRenderingLifecycleTests, ThreadedRendererPublishesSnapshotForRuntimeVisibility)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    NLS::Core::ServiceLocator::Provide(*driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(*driver);
    SnapshotPublishingRenderer renderer(*driver);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 64u;
    frameDescriptor.renderHeight = 64u;
    frameDescriptor.camera = &camera;

    renderer.BeginFrame(frameDescriptor);
    renderer.EndFrame();

    const auto telemetry = NLS::Render::Context::DriverRendererAccess::GetThreadedFrameTelemetry(*driver);
    EXPECT_EQ(telemetry.publishedFrameCount, 1u);
}

TEST(ThreadedRenderingLifecycleTests, ThreadedVisibleFrameRecordsSwapchainPassPlanThroughRhiWorker)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    auto swapchain = std::make_shared<TestSwapchain>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);
    NLS::Render::Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    auto frameFence = std::make_shared<TestFence>();
    commandPool->commandBuffer = commandBuffer;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = frameFence;
    frameContext.resourceStateTracker = NLS::Render::RHI::CreateDefaultResourceStateTracker();
    frameContext.resourceStateTracker = NLS::Render::RHI::CreateDefaultResourceStateTracker();
    frameContext.resourceStateTracker = NLS::Render::RHI::CreateDefaultResourceStateTracker();
    frameContext.resourceStateTracker = NLS::Render::RHI::CreateDefaultResourceStateTracker();

    VisibleSnapshotPublishingRenderer renderer(driver);

    NLS::Render::Entities::Camera camera;
    camera.SetClearColor({ 0.2f, 0.4f, 0.6f });
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 96u;
    frameDescriptor.renderHeight = 54u;
    frameDescriptor.camera = &camera;

    renderer.BeginFrame(frameDescriptor);
    EXPECT_EQ(NLS::Render::Context::DriverRendererAccess::GetActiveExplicitCommandBuffer(driver), nullptr);

    renderer.EndFrame();
    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    EXPECT_EQ(explicitDevice->GetTestQueue()->presentCalls, 1u);
    EXPECT_EQ(swapchain->acquireCalls, 1u);
    auto submittedCommandBuffer = GetSubmittedTestCommandBuffer(explicitDevice->GetTestQueue());
    ASSERT_NE(submittedCommandBuffer, nullptr);
    EXPECT_EQ(submittedCommandBuffer->beginCalls, 1u);
    EXPECT_EQ(submittedCommandBuffer->endCalls, 1u);
    EXPECT_EQ(submittedCommandBuffer->beginRenderPassCalls, 1u);
    EXPECT_EQ(submittedCommandBuffer->endRenderPassCalls, 1u);
    EXPECT_EQ(submittedCommandBuffer->setViewportCalls, 1u);
    EXPECT_EQ(submittedCommandBuffer->lastRenderPassDesc.renderArea.width, 96u);
    EXPECT_EQ(submittedCommandBuffer->lastRenderPassDesc.renderArea.height, 54u);
    ASSERT_EQ(submittedCommandBuffer->lastRenderPassDesc.colorAttachments.size(), 1u);
    EXPECT_FLOAT_EQ(submittedCommandBuffer->lastRenderPassDesc.colorAttachments[0].clearValue.r, 0.2f);
    EXPECT_FLOAT_EQ(submittedCommandBuffer->lastRenderPassDesc.colorAttachments[0].clearValue.g, 0.4f);
    EXPECT_FLOAT_EQ(submittedCommandBuffer->lastRenderPassDesc.colorAttachments[0].clearValue.b, 0.6f);
}

TEST(ThreadedRenderingLifecycleTests, ThreadedVisibleFrameRecordsPreparedDrawBindingsAndMeshByDefault)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    auto swapchain = std::make_shared<TestSwapchain>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);
    NLS::Render::Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    auto frameFence = std::make_shared<TestFence>();
    commandPool->commandBuffer = commandBuffer;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = frameFence;
    frameContext.resourceStateTracker = NLS::Render::RHI::CreateDefaultResourceStateTracker();

    auto pipeline = std::make_shared<TestGraphicsPipeline>("ThreadedPipeline");
    auto frameBindingSet = std::make_shared<TestBindingSet>("FrameBindingSet");
    auto objectBindingSet = std::make_shared<TestBindingSet>("ObjectBindingSet");
    auto materialBindingSet = std::make_shared<TestBindingSet>("MaterialBindingSet");
    auto mesh = std::make_shared<TestMesh>();

    PreparedDrawSnapshotRenderer renderer(driver, pipeline, materialBindingSet, mesh);
    auto provider = std::make_unique<ThreadedDrawCaptureProvider>(renderer, frameBindingSet, objectBindingSet);
    auto* providerPtr = provider.get();
    renderer.SetFrameObjectBindingProvider(std::move(provider));

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 128u;
    frameDescriptor.renderHeight = 72u;
    frameDescriptor.camera = &camera;

    renderer.BeginFrame(frameDescriptor);
    EXPECT_EQ(NLS::Render::Context::DriverRendererAccess::GetActiveExplicitCommandBuffer(driver), nullptr);
    EXPECT_EQ(frameFence->waitCalls, 0u);
    EXPECT_EQ(frameFence->resetCalls, 0u);
    EXPECT_EQ(commandPool->resetCalls, 0u);
    EXPECT_EQ(commandBuffer->resetCalls, 0u);
    EXPECT_EQ(commandBuffer->beginCalls, 0u);
    renderer.DrawEntity(NLS::Render::Data::PipelineState {}, NLS::Render::Entities::Drawable {});
    renderer.EndFrame();
    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    EXPECT_EQ(providerPtr->captureCalls, 1u);
    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    EXPECT_EQ(explicitDevice->GetTestQueue()->presentCalls, 1u);
    auto submittedCommandBuffer = GetSubmittedTestCommandBuffer(explicitDevice->GetTestQueue());
    ASSERT_NE(submittedCommandBuffer, nullptr);
    EXPECT_EQ(submittedCommandBuffer->bindGraphicsPipelineCalls, 1u);
    EXPECT_EQ(submittedCommandBuffer->bindBindingSetCalls, 3u);
    EXPECT_EQ(submittedCommandBuffer->bindVertexBufferCalls, 1u);
    EXPECT_EQ(submittedCommandBuffer->drawIndexedCalls, 1u);
    EXPECT_EQ(submittedCommandBuffer->boundSetIndices.size(), 3u);
    EXPECT_EQ(submittedCommandBuffer->boundSetIndices[0], NLS::Render::RHI::BindingPointMap::kFrameDescriptorSet);
    EXPECT_EQ(submittedCommandBuffer->boundSetIndices[1], NLS::Render::RHI::BindingPointMap::kObjectDescriptorSet);
    EXPECT_EQ(submittedCommandBuffer->boundSetIndices[2], NLS::Render::RHI::BindingPointMap::kMaterialDescriptorSet);
}

TEST(ThreadedRenderingLifecycleTests, StandaloneExplicitFrameIsRejectedWhileThreadedFrameOwnsFrameContext)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 301u;
    snapshot.targetsSwapchain = true;
    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessFrameSnapshot(driver, snapshot));

    EXPECT_FALSE(NLS::Render::Context::DriverTestAccess::CanBeginStandaloneExplicitFrame(driver));
}

TEST(ThreadedRenderingLifecycleTests, ThreadedUiRenderCanBeginWhileOnlyOffscreenThreadedFrameIsInFlight)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    auto swapchain = std::make_shared<TestSwapchain>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);
    NLS::Render::Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    auto frameFence = std::make_shared<TestFence>();
    commandPool->commandBuffer = commandBuffer;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = frameFence;

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 303u;
    snapshot.targetsSwapchain = false;
    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessFrameSnapshot(driver, snapshot));

    EXPECT_TRUE(NLS::Render::Context::DriverUIAccess::PrepareUIRender(driver));
    EXPECT_NE(NLS::Render::Context::DriverRendererAccess::GetActiveExplicitCommandBuffer(driver), nullptr);

    NLS::Render::Context::DriverUIAccess::PresentSwapchain(driver);
    EXPECT_EQ(NLS::Render::Context::DriverRendererAccess::GetActiveExplicitCommandBuffer(driver), nullptr);
}

TEST(ThreadedRenderingLifecycleTests, ThreadedUiRenderSkipsWhenRhiSubmissionOwnsFrameContext)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    auto swapchain = std::make_shared<TestSwapchain>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);
    NLS::Render::Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    auto frameFence = std::make_shared<TestFence>();
    commandPool->commandBuffer = commandBuffer;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = frameFence;

    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryLockThreadedRhiSubmission(driver));
    EXPECT_FALSE(NLS::Render::Context::DriverUIAccess::PrepareUIRender(driver));
    NLS::Render::Context::DriverTestAccess::UnlockThreadedRhiSubmission(driver);

    EXPECT_EQ(NLS::Render::Context::DriverRendererAccess::GetActiveExplicitCommandBuffer(driver), nullptr);
    EXPECT_EQ(frameFence->waitCalls, 0u);
    EXPECT_EQ(frameFence->resetCalls, 0u);
    EXPECT_EQ(commandPool->resetCalls, 0u);
    EXPECT_EQ(commandBuffer->resetCalls, 0u);
    EXPECT_EQ(commandBuffer->beginCalls, 0u);
}

TEST(ThreadedRenderingLifecycleTests, StandaloneExplicitFrameBeginSkipsWhileThreadedFrameOwnsFrameContext)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    auto frameFence = std::make_shared<TestFence>();
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = frameFence;
    commandPool->commandBuffer = commandBuffer;

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 302u;
    snapshot.targetsSwapchain = true;
    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessFrameSnapshot(driver, snapshot));

    NLS::Render::Context::DriverTestAccess::BeginStandaloneExplicitFrame(driver, false);

    EXPECT_EQ(frameFence->waitCalls, 0u);
    EXPECT_EQ(frameFence->resetCalls, 0u);
    EXPECT_EQ(commandPool->resetCalls, 0u);
    EXPECT_EQ(commandBuffer->resetCalls, 0u);
    EXPECT_EQ(commandBuffer->beginCalls, 0u);
    EXPECT_EQ(NLS::Render::Context::DriverRendererAccess::GetActiveExplicitCommandBuffer(driver), nullptr);
}

TEST(ThreadedRenderingLifecycleTests, CompositeRendererRecordsBackPressuredPublishDiagnosticsWhenSlotsStayOccupied)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    NLS::Core::ServiceLocator::Provide(*driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(*driver);
    SnapshotPublishingRenderer renderer(*driver);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 64u;
    frameDescriptor.renderHeight = 64u;
    frameDescriptor.camera = &camera;

    renderer.BeginFrame(frameDescriptor);
    renderer.EndFrame();
    renderer.BeginFrame(frameDescriptor);
    renderer.EndFrame();

    ASSERT_TRUE(renderer.IsFrameInfoValid());
    const auto& frameInfo = renderer.GetFrameInfo();
    EXPECT_EQ(frameInfo.inFlightFrameCount, 1u);
    EXPECT_EQ(frameInfo.blockedFrameCount, 1u);
    EXPECT_EQ(frameInfo.publishState, NLS::Render::Data::FramePublishState::BackPressured);
}

TEST(ThreadedRenderingLifecycleTests, FrameSnapshotCapturesFrameDescriptorCameraAndTargetState)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    NLS::Core::ServiceLocator::Provide(*driver);
    SnapshotPublishingRenderer renderer(*driver);

    NLS::Render::Entities::Camera camera;
    camera.SetClearColor({ 0.25f, 0.5f, 0.75f });
    camera.SetClearColorBuffer(false);
    camera.SetClearDepthBuffer(true);
    camera.SetClearStencilBuffer(false);
    camera.SetFrustumGeometryCulling(true);
    camera.SetFrustumLightCulling(true);

    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 320u;
    frameDescriptor.renderHeight = 180u;
    frameDescriptor.camera = &camera;

    const auto snapshot = renderer.CaptureFrameSnapshot(frameDescriptor);

    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->renderWidth, 320u);
    EXPECT_EQ(snapshot->renderHeight, 180u);
    EXPECT_TRUE(snapshot->targetsSwapchain);
    EXPECT_FLOAT_EQ(snapshot->clearColor.x, 0.25f);
    EXPECT_FLOAT_EQ(snapshot->clearColor.y, 0.5f);
    EXPECT_FLOAT_EQ(snapshot->clearColor.z, 0.75f);
    EXPECT_FLOAT_EQ(snapshot->clearColor.w, 1.0f);
    EXPECT_FALSE(snapshot->clearColorBuffer);
    EXPECT_TRUE(snapshot->clearDepthBuffer);
    EXPECT_FALSE(snapshot->clearStencilBuffer);
    EXPECT_TRUE(snapshot->hasGeometryFrustum);
    EXPECT_TRUE(snapshot->hasLightFrustum);
}

TEST(ThreadedRenderingLifecycleTests, FrameSnapshotMarksFramebufferTargetsAsOffscreenOnly)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    NLS::Core::ServiceLocator::Provide(*driver);
    SnapshotPublishingRenderer renderer(*driver);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 320u;
    frameDescriptor.renderHeight = 180u;
    frameDescriptor.camera = &camera;
    frameDescriptor.outputBuffer = reinterpret_cast<NLS::Render::Buffers::Framebuffer*>(1);

    const auto snapshot = renderer.CaptureFrameSnapshot(frameDescriptor);

    ASSERT_TRUE(snapshot.has_value());
    EXPECT_FALSE(snapshot->targetsSwapchain);
}

TEST(ThreadedRenderingLifecycleTests, BaseSceneRendererAddsSceneInputSummaryToFrameSnapshot)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    SceneSnapshotRenderer renderer(driver);

    NLS::Engine::SceneSystem::Scene scene;
    scene.CreateGameObject("SnapshotActor");
    renderer.AddDescriptor<NLS::Engine::Rendering::BaseSceneRenderer::SceneDescriptor>({
        scene,
        std::nullopt,
        nullptr
    });

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 128u;
    frameDescriptor.renderHeight = 96u;
    frameDescriptor.camera = &camera;

    const auto snapshot = renderer.CaptureFrameSnapshot(frameDescriptor);

    ASSERT_TRUE(snapshot.has_value());
    EXPECT_TRUE(snapshot->hasSceneInput);
    EXPECT_EQ(snapshot->sceneActorCount, 1u);
    EXPECT_EQ(snapshot->sceneModelRendererCount, 0u);
    EXPECT_EQ(snapshot->sceneLightCount, 0u);
    EXPECT_EQ(snapshot->sceneSkyboxCount, 0u);
    EXPECT_EQ(snapshot->visibleOpaqueDrawCount, 0u);
    EXPECT_EQ(snapshot->visibleTransparentDrawCount, 0u);
    EXPECT_EQ(snapshot->visibleSkyboxDrawCount, 0u);
}

TEST(ThreadedRenderingLifecycleTests, ThreadedRendererWaitsForRetiredSlotBeforeDroppingSnapshot)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    settings.threadedPublishRetirementWaitMs = 250u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    VisibleSnapshotPublishingRenderer renderer(driver);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 64u;
    frameDescriptor.renderHeight = 64u;
    frameDescriptor.camera = &camera;

    renderer.BeginFrame(frameDescriptor);
    renderer.EndFrame();

    auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);

    std::thread retireThread([lifecycle]()
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        NLS::Render::Context::RhiSubmissionFrame submissionFrame;
        submissionFrame.frameId = 1u;

        NLS::Render::Context::RenderScenePackage scenePackage;
        scenePackage.frameId = 1u;
        ASSERT_TRUE(lifecycle->TryBeginRenderScene(0u));
        ASSERT_TRUE(lifecycle->CompleteRenderScene(0u, scenePackage));
        ASSERT_TRUE(lifecycle->TryBeginRhiSubmission(0u));
        ASSERT_TRUE(lifecycle->CompleteRhiSubmission(0u, submissionFrame));
        ASSERT_TRUE(lifecycle->RetireFrame(0u));
    });

    renderer.BeginFrame(frameDescriptor);
    renderer.EndFrame();
    retireThread.join();

    const auto telemetry = NLS::Render::Context::DriverRendererAccess::GetThreadedFrameTelemetry(driver);
    EXPECT_EQ(telemetry.publishedFrameCount, 2u);
    EXPECT_EQ(telemetry.blockedPublishCount, 1u);
    EXPECT_EQ(telemetry.inFlightFrameCount, 1u);
    EXPECT_EQ(telemetry.publishState, NLS::Render::Data::FramePublishState::Open);
}

TEST(ThreadedRenderingLifecycleTests, RenderScenePackageUsesSnapshotAfterLiveSceneChanges)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    SceneSnapshotRenderer renderer(driver);

    NLS::Engine::SceneSystem::Scene scene;
    scene.CreateGameObject("SnapshotActor");
    renderer.AddDescriptor<NLS::Engine::Rendering::BaseSceneRenderer::SceneDescriptor>({
        scene,
        std::nullopt,
        nullptr
    });

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 128u;
    frameDescriptor.renderHeight = 96u;
    frameDescriptor.camera = &camera;

    const auto snapshot = renderer.CaptureFrameSnapshot(frameDescriptor);
    ASSERT_TRUE(snapshot.has_value());

    scene.CreateGameObject("LiveMutationAfterSnapshot");

    const auto package = renderer.CaptureRenderScenePackage(snapshot.value());

    EXPECT_EQ(package.frameId, snapshot->frameId);
    EXPECT_EQ(package.sceneActorCount, 1u);
    EXPECT_EQ(package.visibleDrawCount, 0u);
    EXPECT_FALSE(package.hasVisibleDraws);
    EXPECT_EQ(package.opaqueDrawCount, 0u);
    EXPECT_EQ(package.transparentDrawCount, 0u);
    EXPECT_EQ(package.skyboxDrawCount, 0u);
    EXPECT_EQ(package.passPlanCount, 0u);
    EXPECT_TRUE(package.frameDataReady);
    EXPECT_TRUE(package.objectDataReady);
}

TEST(ThreadedRenderingLifecycleTests, RenderScenePackageBuildsTypedPassPlanFromSnapshotDrawSets)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    NLS::Core::ServiceLocator::Provide(*driver);
    SceneSnapshotRenderer renderer(*driver);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 9u;
    snapshot.renderWidth = 256u;
    snapshot.renderHeight = 144u;
    snapshot.hasSceneInput = true;
    snapshot.visibleOpaqueDrawCount = 3u;
    snapshot.visibleTransparentDrawCount = 2u;
    snapshot.visibleSkyboxDrawCount = 1u;
    snapshot.visibleHelperDrawCount = 4u;
    snapshot.clearColor = { 0.1f, 0.2f, 0.3f, 1.0f };

    const auto package = renderer.CaptureRenderScenePackage(snapshot);

    EXPECT_EQ(package.frameId, 9u);
    EXPECT_EQ(package.opaqueDrawCount, 3u);
    EXPECT_EQ(package.transparentDrawCount, 2u);
    EXPECT_EQ(package.skyboxDrawCount, 1u);
    EXPECT_EQ(package.helperDrawCount, 4u);
    EXPECT_EQ(package.visibleDrawCount, 10u);
    EXPECT_TRUE(package.hasOpaquePass);
    EXPECT_TRUE(package.hasTransparentPass);
    EXPECT_TRUE(package.hasSkyboxPass);
    EXPECT_TRUE(package.hasHelperPass);
    EXPECT_EQ(package.passPlanCount, 4u);
    EXPECT_EQ(package.drawCommandCount, 10u);
    EXPECT_EQ(package.materialBatchCount, 10u);
    EXPECT_EQ(package.renderTargetUseCount, 1u);
    EXPECT_TRUE(package.containsCommandInputs);
    ASSERT_EQ(package.passCommandInputs.size(), 4u);
    EXPECT_EQ(package.passCommandInputs[0].renderWidth, 256u);
    EXPECT_EQ(package.passCommandInputs[0].renderHeight, 144u);
    EXPECT_TRUE(package.passCommandInputs[0].usesColorAttachment);
    EXPECT_TRUE(package.passCommandInputs[0].clearColor);
    EXPECT_FLOAT_EQ(package.passCommandInputs[0].clearColorValue.x, 0.1f);
    EXPECT_FALSE(package.passCommandInputs[1].clearColor);
}

TEST(ThreadedRenderingLifecycleTests, BaseSceneRendererPublishesPreparedBuilderInsteadOfPreparedPackageWhenThreadedWorkersArePaused)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    SceneSnapshotRenderer renderer(driver);
    NLS::Engine::SceneSystem::Scene scene;
    scene.CreateGameObject("SnapshotActor");
    renderer.AddDescriptor<NLS::Engine::Rendering::BaseSceneRenderer::SceneDescriptor>({
        scene,
        std::nullopt,
        nullptr
    });

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 128u;
    frameDescriptor.renderHeight = 96u;
    frameDescriptor.camera = &camera;

    ASSERT_NO_THROW(renderer.BeginFrame(frameDescriptor));
    ASSERT_NO_THROW(renderer.EndFrame());

    const auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);
    const auto* slot = lifecycle->PeekSlot(0u);
    ASSERT_NE(slot, nullptr);
    ASSERT_TRUE(slot->snapshot.has_value());
    EXPECT_EQ(slot->publishOrigin, NLS::Render::Context::ThreadedFramePublishOrigin::PreparedBuilder);
    EXPECT_TRUE(slot->preparedRenderSceneBuilder.has_value());
    EXPECT_FALSE(slot->renderScenePackage.has_value());

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    const auto* retiredSlot = lifecycle->PeekSlot(0u);
    ASSERT_NE(retiredSlot, nullptr);
    EXPECT_EQ(retiredSlot->stage, NLS::Render::Context::ThreadedFrameStage::Retired);
    EXPECT_EQ(retiredSlot->renderSceneAttribution, NLS::Render::Context::RenderSceneAttribution::RendererPrepared);
    ASSERT_TRUE(retiredSlot->renderScenePackage.has_value());
    EXPECT_EQ(retiredSlot->renderScenePackage->sceneActorCount, 1u);
}

TEST(ThreadedRenderingLifecycleTests, BaseRendererPublishesPreparedBuilderInsteadOfRawSnapshotWhenThreadedWorkersArePaused)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    VisibleSnapshotPublishingRenderer renderer(driver);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 128u;
    frameDescriptor.renderHeight = 96u;
    frameDescriptor.camera = &camera;

    ASSERT_NO_THROW(renderer.BeginFrame(frameDescriptor));
    ASSERT_NO_THROW(renderer.EndFrame());

    const auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);
    const auto* slot = lifecycle->PeekSlot(0u);
    ASSERT_NE(slot, nullptr);
    ASSERT_TRUE(slot->snapshot.has_value());
    EXPECT_EQ(slot->publishOrigin, NLS::Render::Context::ThreadedFramePublishOrigin::PreparedBuilder);
    EXPECT_TRUE(slot->preparedRenderSceneBuilder.has_value());
    EXPECT_FALSE(slot->renderScenePackage.has_value());

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    const auto* retiredSlot = lifecycle->PeekSlot(0u);
    ASSERT_NE(retiredSlot, nullptr);
    EXPECT_EQ(retiredSlot->stage, NLS::Render::Context::ThreadedFrameStage::Retired);
    EXPECT_EQ(retiredSlot->renderSceneAttribution, NLS::Render::Context::RenderSceneAttribution::RendererPrepared);
    ASSERT_TRUE(retiredSlot->renderScenePackage.has_value());
    EXPECT_EQ(retiredSlot->renderScenePackage->visibleDrawCount, 1u);
    ASSERT_EQ(retiredSlot->renderScenePackage->passCommandInputs.size(), 1u);
    EXPECT_EQ(retiredSlot->renderScenePackage->passCommandInputs[0].kind, NLS::Render::Context::RenderPassCommandKind::Opaque);
}

TEST(ThreadedRenderingLifecycleTests, PreparedBuilderPathDoesNotSilentlyUseDriverScenePackageWhenPayloadIsMissing)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    VisibleSnapshotPublishingRenderer renderer(driver);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 128u;
    frameDescriptor.renderHeight = 96u;
    frameDescriptor.camera = &camera;

    ASSERT_NO_THROW(renderer.BeginFrame(frameDescriptor));
    ASSERT_NO_THROW(renderer.EndFrame());

    auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);

    size_t slotIndex = 99u;
    NLS::Render::Context::FrameSnapshot snapshot;
    ASSERT_TRUE(lifecycle->TryBeginNextRenderScene(&slotIndex, &snapshot));
    EXPECT_EQ(slotIndex, 0u);

    auto* slot = const_cast<NLS::Render::Context::InFlightFrameSlot*>(lifecycle->PeekSlot(slotIndex));
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(slot->publishOrigin, NLS::Render::Context::ThreadedFramePublishOrigin::PreparedBuilder);
    slot->preparedRenderSceneBuilder.reset();
    slot->renderScenePackage.reset();

    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::ResolveAndCompleteThreadedRenderScene(
        driver,
        slotIndex));

    const auto* renderReadySlot = lifecycle->PeekSlot(slotIndex);
    ASSERT_NE(renderReadySlot, nullptr);
    ASSERT_TRUE(renderReadySlot->renderScenePackage.has_value());
    EXPECT_EQ(
        renderReadySlot->renderSceneAttribution,
        NLS::Render::Context::RenderSceneAttribution::PreparedBuilderMissing);
    EXPECT_EQ(renderReadySlot->renderScenePackage->visibleDrawCount, 0u);
    EXPECT_FALSE(renderReadySlot->renderScenePackage->hasVisibleDraws);
    EXPECT_TRUE(renderReadySlot->renderScenePackage->passCommandInputs.empty());
}

TEST(ThreadedRenderingLifecycleTests, Dx12PreparedBuilderDrainDoesNotUseSnapshotHarnessAttribution)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::DX12;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    VisibleSnapshotPublishingRenderer renderer(driver);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 128u;
    frameDescriptor.renderHeight = 96u;
    frameDescriptor.camera = &camera;

    ASSERT_NO_THROW(renderer.BeginFrame(frameDescriptor));
    ASSERT_NO_THROW(renderer.EndFrame());

    const auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);
    const auto* publishedSlot = lifecycle->PeekSlot(0u);
    ASSERT_NE(publishedSlot, nullptr);
    EXPECT_EQ(publishedSlot->publishOrigin, NLS::Render::Context::ThreadedFramePublishOrigin::PreparedBuilder);

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    const auto* retiredSlot = lifecycle->PeekSlot(0u);
    ASSERT_NE(retiredSlot, nullptr);
    EXPECT_EQ(retiredSlot->stage, NLS::Render::Context::ThreadedFrameStage::Retired);
    EXPECT_EQ(retiredSlot->publishOrigin, NLS::Render::Context::ThreadedFramePublishOrigin::PreparedBuilder);
    EXPECT_EQ(retiredSlot->renderSceneAttribution, NLS::Render::Context::RenderSceneAttribution::RendererPrepared);
    EXPECT_NE(retiredSlot->renderSceneAttribution, NLS::Render::Context::RenderSceneAttribution::SnapshotHarness);
}

TEST(ThreadedRenderingLifecycleTests, RenderScenePackageSplitsRecordedDrawCommandsIntoPassOwnedPlans)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    SceneSnapshotRenderer renderer(driver);

    auto opaquePipeline = std::make_shared<TestGraphicsPipeline>("OpaquePipeline");
    auto transparentPipelineA = std::make_shared<TestGraphicsPipeline>("TransparentPipelineA");
    auto transparentPipelineB = std::make_shared<TestGraphicsPipeline>("TransparentPipelineB");
    auto skyPipeline = std::make_shared<TestGraphicsPipeline>("SkyPipeline");
    auto materialBindingSet = std::make_shared<TestBindingSet>("MaterialBindingSet");
    auto mesh = std::make_shared<TestMesh>();

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 19u;
    snapshot.renderWidth = 256u;
    snapshot.renderHeight = 144u;
    snapshot.hasSceneInput = true;
    snapshot.visibleOpaqueDrawCount = 1u;
    snapshot.visibleTransparentDrawCount = 2u;
    snapshot.visibleSkyboxDrawCount = 1u;

    snapshot.recordedDrawCommands = {
        { opaquePipeline, nullptr, nullptr, nullptr, materialBindingSet, mesh, 1u },
        { transparentPipelineA, nullptr, nullptr, nullptr, materialBindingSet, mesh, 1u },
        { transparentPipelineB, nullptr, nullptr, nullptr, materialBindingSet, mesh, 1u },
        { skyPipeline, nullptr, nullptr, nullptr, materialBindingSet, mesh, 1u }
    };

    const auto package = renderer.CaptureRenderScenePackage(snapshot);

    ASSERT_EQ(package.passCommandInputs.size(), 3u);
    ASSERT_EQ(package.passCommandInputs[0].recordedDrawCommands.size(), 1u);
    EXPECT_EQ(package.passCommandInputs[0].recordedDrawCommands[0].pipeline, opaquePipeline);
    ASSERT_EQ(package.passCommandInputs[1].recordedDrawCommands.size(), 2u);
    EXPECT_EQ(package.passCommandInputs[1].recordedDrawCommands[0].pipeline, transparentPipelineA);
    EXPECT_EQ(package.passCommandInputs[1].recordedDrawCommands[1].pipeline, transparentPipelineB);
    ASSERT_EQ(package.passCommandInputs[2].recordedDrawCommands.size(), 1u);
    EXPECT_EQ(package.passCommandInputs[2].recordedDrawCommands[0].pipeline, skyPipeline);
}

TEST(ThreadedRenderingLifecycleTests, ForwardSceneRendererPublishesSnapshotOwnedSceneInputsWhenThreaded)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 2u;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    NLS::Core::ServiceLocator::Provide(*driver);
    std::unique_ptr<NLS::Engine::Rendering::ForwardSceneRenderer> renderer;
    ASSERT_NO_THROW(renderer = std::make_unique<NLS::Engine::Rendering::ForwardSceneRenderer>(*driver));
    ASSERT_NE(renderer, nullptr);

    NLS::Engine::SceneSystem::Scene scene;
    scene.CreateGameObject("ModelActor").AddComponent<NLS::Engine::Components::MeshRenderer>();
    scene.CreateGameObject("LightActor").AddComponent<NLS::Engine::Components::LightComponent>();
    renderer->AddDescriptor<NLS::Engine::Rendering::BaseSceneRenderer::SceneDescriptor>({
        scene,
        std::nullopt,
        nullptr
    });

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 128u;
    frameDescriptor.renderHeight = 96u;
    frameDescriptor.camera = &camera;

    ASSERT_NO_THROW(renderer->BeginFrame(frameDescriptor));
    scene.CreateGameObject("LateMutation").AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NO_THROW(renderer->EndFrame());

    const auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(*driver);
    ASSERT_NE(lifecycle, nullptr);
    const auto* slot = lifecycle->PeekSlot(0u);
    ASSERT_NE(slot, nullptr);
    ASSERT_TRUE(slot->snapshot.has_value());
    EXPECT_TRUE(slot->snapshot->hasSceneInput);
    EXPECT_EQ(slot->snapshot->sceneActorCount, 2u);
    EXPECT_EQ(slot->snapshot->sceneModelRendererCount, 1u);
    EXPECT_EQ(slot->snapshot->sceneLightCount, 1u);
    EXPECT_EQ(slot->snapshot->sceneSkyboxCount, 0u);
    EXPECT_EQ(slot->snapshot->visibleOpaqueDrawCount, 0u);
    EXPECT_EQ(slot->snapshot->visibleSkyboxDrawCount, 0u);
}

TEST(ThreadedRenderingLifecycleTests, DeferredSceneRendererPublishesSnapshotOwnedSceneInputsWhenThreaded)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 2u;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    NLS::Core::ServiceLocator::Provide(*driver);
    std::unique_ptr<NLS::Engine::Rendering::DeferredSceneRenderer> renderer;
    ASSERT_NO_THROW(renderer = std::make_unique<NLS::Engine::Rendering::DeferredSceneRenderer>(*driver));
    ASSERT_NE(renderer, nullptr);

    NLS::Engine::SceneSystem::Scene scene;
    scene.CreateGameObject("ModelActor").AddComponent<NLS::Engine::Components::MeshRenderer>();
    scene.CreateGameObject("LightActor").AddComponent<NLS::Engine::Components::LightComponent>();
    renderer->AddDescriptor<NLS::Engine::Rendering::BaseSceneRenderer::SceneDescriptor>({
        scene,
        std::nullopt,
        nullptr
    });

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 128u;
    frameDescriptor.renderHeight = 96u;
    frameDescriptor.camera = &camera;

    ASSERT_NO_THROW(renderer->BeginFrame(frameDescriptor));
    scene.CreateGameObject("LateMutation").AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NO_THROW(renderer->EndFrame());

    const auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(*driver);
    ASSERT_NE(lifecycle, nullptr);
    const auto* slot = lifecycle->PeekSlot(0u);
    ASSERT_NE(slot, nullptr);
    ASSERT_TRUE(slot->snapshot.has_value());
    EXPECT_TRUE(slot->snapshot->hasSceneInput);
    EXPECT_EQ(slot->snapshot->sceneActorCount, 2u);
    EXPECT_EQ(slot->snapshot->sceneModelRendererCount, 1u);
    EXPECT_EQ(slot->snapshot->sceneLightCount, 1u);
    EXPECT_EQ(slot->snapshot->sceneSkyboxCount, 0u);
    EXPECT_EQ(slot->snapshot->visibleOpaqueDrawCount, 0u);
    EXPECT_EQ(slot->snapshot->visibleSkyboxDrawCount, 0u);
}

TEST(ThreadedRenderingLifecycleTests, DeferredSceneRendererPublishesPreparedRenderScenePackageWhenThreadedWorkersArePaused)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    NLS::Engine::Rendering::DeferredSceneRenderer renderer(driver);
    NLS::Engine::SceneSystem::Scene scene;
    scene.CreateGameObject("LightActor").AddComponent<NLS::Engine::Components::LightComponent>();
    renderer.AddDescriptor<NLS::Engine::Rendering::BaseSceneRenderer::SceneDescriptor>({
        scene,
        std::nullopt,
        nullptr
    });

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 128u;
    frameDescriptor.renderHeight = 96u;
    frameDescriptor.camera = &camera;

    ASSERT_NO_THROW(renderer.BeginFrame(frameDescriptor));
    ASSERT_NO_THROW(renderer.EndFrame());

    const auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);
    const auto* slot = lifecycle->PeekSlot(0u);
    ASSERT_NE(slot, nullptr);
    ASSERT_TRUE(slot->snapshot.has_value());
    EXPECT_TRUE(slot->preparedRenderSceneBuilder.has_value() || slot->renderScenePackage.has_value());
    EXPECT_EQ(slot->stage, NLS::Render::Context::ThreadedFrameStage::Published);
    EXPECT_EQ(slot->renderSceneAttribution, NLS::Render::Context::RenderSceneAttribution::Unknown);
    EXPECT_EQ(slot->rhiSubmissionAttribution, NLS::Render::Context::RhiSubmissionAttribution::Unknown);

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    const auto* retiredSlot = lifecycle->PeekSlot(0u);
    ASSERT_NE(retiredSlot, nullptr);
    EXPECT_EQ(retiredSlot->stage, NLS::Render::Context::ThreadedFrameStage::Retired);
    EXPECT_EQ(retiredSlot->renderSceneAttribution, NLS::Render::Context::RenderSceneAttribution::RendererPrepared);
    EXPECT_EQ(retiredSlot->rhiSubmissionAttribution, NLS::Render::Context::RhiSubmissionAttribution::SynchronousDrain);
    ASSERT_TRUE(retiredSlot->renderScenePackage.has_value());
    EXPECT_TRUE(retiredSlot->renderScenePackage->frameDataReady);
    EXPECT_TRUE(retiredSlot->renderScenePackage->objectDataReady);
    EXPECT_TRUE(retiredSlot->renderScenePackage->lightingDataReady);
    ASSERT_EQ(retiredSlot->renderScenePackage->passCommandInputs.size(), 2u);
    EXPECT_EQ(retiredSlot->renderScenePackage->passCommandInputs[0].kind, NLS::Render::Context::RenderPassCommandKind::GBuffer);
    EXPECT_EQ(retiredSlot->renderScenePackage->passCommandInputs[1].kind, NLS::Render::Context::RenderPassCommandKind::Lighting);
}

TEST(ThreadedRenderingLifecycleTests, DriverRenderSceneWorkerRejectsPublishedSnapshotPackage)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 71u;
    snapshot.hasSceneInput = true;
    snapshot.renderWidth = 160u;
    snapshot.renderHeight = 90u;
    snapshot.sceneActorCount = 5u;
    snapshot.sceneLightCount = 1u;
    snapshot.visibleOpaqueDrawCount = 2u;
    snapshot.visibleTransparentDrawCount = 1u;
    snapshot.visibleSkyboxDrawCount = 1u;

    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessFrameSnapshot(driver, snapshot));

    const NLS::Render::Context::InFlightFrameSlot* readySlot = nullptr;
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        readySlot = lifecycle->PeekSlot(0u);
        if (readySlot != nullptr && readySlot->renderScenePackage.has_value())
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    ASSERT_NE(readySlot, nullptr);
    ASSERT_TRUE(readySlot->renderScenePackage.has_value());
    EXPECT_EQ(readySlot->publishOrigin, NLS::Render::Context::ThreadedFramePublishOrigin::SnapshotHarness);
    EXPECT_EQ(readySlot->renderSceneAttribution, NLS::Render::Context::RenderSceneAttribution::SnapshotHarness);
    EXPECT_EQ(readySlot->renderScenePackage->frameId, 71u);
    EXPECT_EQ(readySlot->renderScenePackage->sceneActorCount, 5u);
    EXPECT_EQ(readySlot->renderScenePackage->visibleDrawCount, 0u);
    EXPECT_FALSE(readySlot->renderScenePackage->hasVisibleDraws);
    EXPECT_FALSE(readySlot->renderScenePackage->frameDataReady);
    EXPECT_FALSE(readySlot->renderScenePackage->objectDataReady);
    EXPECT_FALSE(readySlot->renderScenePackage->lightingDataReady);
    EXPECT_FALSE(readySlot->renderScenePackage->hasLightingData);
    EXPECT_TRUE(readySlot->renderScenePackage->passCommandInputs.empty());
}

TEST(ThreadedRenderingLifecycleTests, DriverRhiWorkerConsumesRenderReadyPackageAndRetiresFrame)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 91u;
    snapshot.hasSceneInput = true;
    snapshot.visibleOpaqueDrawCount = 1u;

    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessFrameSnapshot(driver, snapshot));

    const NLS::Render::Context::InFlightFrameSlot* retiredSlot = nullptr;
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        retiredSlot = lifecycle->PeekSlot(0u);
        if (retiredSlot != nullptr && retiredSlot->stage == NLS::Render::Context::ThreadedFrameStage::Retired)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    ASSERT_NE(retiredSlot, nullptr);
    EXPECT_EQ(retiredSlot->stage, NLS::Render::Context::ThreadedFrameStage::Retired);
    EXPECT_EQ(retiredSlot->publishOrigin, NLS::Render::Context::ThreadedFramePublishOrigin::SnapshotHarness);
    ASSERT_TRUE(retiredSlot->renderScenePackage.has_value());
    ASSERT_TRUE(retiredSlot->submissionFrame.has_value());
    EXPECT_EQ(retiredSlot->renderSceneAttribution, NLS::Render::Context::RenderSceneAttribution::SnapshotHarness);
    EXPECT_EQ(retiredSlot->rhiSubmissionAttribution, NLS::Render::Context::RhiSubmissionAttribution::Worker);
    EXPECT_EQ(retiredSlot->submissionFrame->frameId, 91u);
    EXPECT_EQ(lifecycle->GetInFlightDepth(), 0u);
}

TEST(ThreadedRenderingLifecycleTests, DriverWorkersPreserveOwnershipArtifactsAndAttributionThroughRetirement)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 92u;
    snapshot.sceneRevision = 7u;
    snapshot.hasSceneInput = true;
    snapshot.targetsSwapchain = true;
    snapshot.visibleOpaqueDrawCount = 2u;
    snapshot.visibleSkyboxDrawCount = 1u;

    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessFrameSnapshot(driver, snapshot));

    const NLS::Render::Context::InFlightFrameSlot* retiredSlot = nullptr;
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        retiredSlot = lifecycle->PeekSlot(0u);
        if (retiredSlot != nullptr && retiredSlot->stage == NLS::Render::Context::ThreadedFrameStage::Retired)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    ASSERT_NE(retiredSlot, nullptr);
    ASSERT_TRUE(retiredSlot->renderFrameInput.has_value());
    ASSERT_TRUE(retiredSlot->renderFrameBuild.has_value());
    ASSERT_TRUE(retiredSlot->renderScenePackage.has_value());
    EXPECT_EQ(retiredSlot->renderSceneAttribution, NLS::Render::Context::RenderSceneAttribution::SnapshotHarness);
    EXPECT_EQ(retiredSlot->rhiSubmissionAttribution, NLS::Render::Context::RhiSubmissionAttribution::Worker);

    EXPECT_EQ(retiredSlot->renderFrameInput->frameId, 92u);
    EXPECT_EQ(retiredSlot->renderFrameInput->sceneRevision, 7u);
    EXPECT_TRUE(retiredSlot->renderFrameInput->immutable);
    EXPECT_TRUE(retiredSlot->renderFrameInput->hasSceneInput);
    EXPECT_EQ(retiredSlot->renderFrameInput->visibleDrawCount, 3u);

    EXPECT_EQ(retiredSlot->renderFrameBuild->frameId, 92u);
    EXPECT_TRUE(retiredSlot->renderFrameBuild->renderThreadOwned);
    EXPECT_TRUE(retiredSlot->renderFrameBuild->targetsSwapchain);
    EXPECT_FALSE(retiredSlot->renderFrameBuild->hasVisibleDraws);
    EXPECT_EQ(retiredSlot->renderFrameBuild->visibleDrawCount, 0u);
    EXPECT_EQ(
        retiredSlot->renderFrameBuild->passPlanCount,
        retiredSlot->renderScenePackage->passPlanCount);
}

TEST(ThreadedRenderingLifecycleTests, DriverWorkersPreserveRendererPreparedAttributionForPreparedSwapchainFrames)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    auto swapchain = std::make_shared<TestSwapchain>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);
    NLS::Render::Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    auto frameFence = std::make_shared<TestFence>();
    auto imageAcquiredSemaphore = std::make_shared<TestSemaphore>();
    auto renderFinishedSemaphore = std::make_shared<TestSemaphore>();
    commandPool->commandBuffer = commandBuffer;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = frameFence;
    frameContext.imageAcquiredSemaphore = imageAcquiredSemaphore;
    frameContext.renderFinishedSemaphore = renderFinishedSemaphore;

    auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 124u;
    snapshot.targetsSwapchain = true;

    NLS::Render::Context::RenderScenePackage renderScenePackage;
    renderScenePackage.frameId = 124u;
    renderScenePackage.targetsSwapchain = true;
    renderScenePackage.renderWidth = 128u;
    renderScenePackage.renderHeight = 72u;
    renderScenePackage.frameDataReady = true;
    renderScenePackage.objectDataReady = true;

    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessPreparedFrame(
        driver,
        snapshot,
        renderScenePackage));

    const NLS::Render::Context::InFlightFrameSlot* retiredSlot = nullptr;
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        retiredSlot = lifecycle->PeekSlot(0u);
        if (retiredSlot != nullptr && retiredSlot->stage == NLS::Render::Context::ThreadedFrameStage::Retired)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    ASSERT_NE(retiredSlot, nullptr);
    EXPECT_EQ(retiredSlot->stage, NLS::Render::Context::ThreadedFrameStage::Retired);
    EXPECT_EQ(retiredSlot->renderSceneAttribution, NLS::Render::Context::RenderSceneAttribution::RendererPrepared);
    EXPECT_EQ(retiredSlot->rhiSubmissionAttribution, NLS::Render::Context::RhiSubmissionAttribution::Worker);
    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    EXPECT_EQ(explicitDevice->GetTestQueue()->presentCalls, 1u);
    EXPECT_EQ(swapchain->acquireCalls, 1u);
}

TEST(ThreadedRenderingLifecycleTests, SynchronousDrainMarksSnapshotHarnessAndSubmissionAttribution)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 211u;
    snapshot.hasSceneInput = true;
    snapshot.visibleOpaqueDrawCount = 1u;

    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessFrameSnapshot(driver, snapshot));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    const auto* retiredSlot = lifecycle->PeekSlot(0u);
    ASSERT_NE(retiredSlot, nullptr);
    EXPECT_EQ(retiredSlot->stage, NLS::Render::Context::ThreadedFrameStage::Retired);
    EXPECT_EQ(retiredSlot->renderSceneAttribution, NLS::Render::Context::RenderSceneAttribution::SnapshotHarness);
    EXPECT_EQ(retiredSlot->rhiSubmissionAttribution, NLS::Render::Context::RhiSubmissionAttribution::SynchronousDrain);
}

TEST(ThreadedRenderingLifecycleTests, SynchronousDrainAppliesPendingSwapchainResizeAfterFrameRetires)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    auto swapchain = std::make_shared<TestSwapchain>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);
    NLS::Render::Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    auto frameFence = std::make_shared<TestFence>();
    auto imageAcquiredSemaphore = std::make_shared<TestSemaphore>();
    auto renderFinishedSemaphore = std::make_shared<TestSemaphore>();
    commandPool->commandBuffer = commandBuffer;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = frameFence;
    frameContext.imageAcquiredSemaphore = imageAcquiredSemaphore;
    frameContext.renderFinishedSemaphore = renderFinishedSemaphore;

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 212u;
    snapshot.targetsSwapchain = true;
    snapshot.visibleOpaqueDrawCount = 1u;

    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessFrameSnapshot(driver, snapshot));

    driver.ResizePlatformSwapchain(1600u, 900u);

    EXPECT_EQ(swapchain->resizeWidth, 0u);
    EXPECT_EQ(swapchain->resizeHeight, 0u);

    NLS::Render::Context::DriverTestAccess::AgePendingSwapchainResize(
        driver,
        NLS::Render::Context::GetInteractiveSwapchainResizeDebounce());
    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    EXPECT_EQ(swapchain->resizeWidth, 1600u);
    EXPECT_EQ(swapchain->resizeHeight, 900u);
    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    EXPECT_EQ(explicitDevice->GetTestQueue()->presentCalls, 1u);
}

TEST(ThreadedRenderingLifecycleTests, DriverRhiWorkerSubmitsAndPresentsSwapchainFramesOnExplicitQueue)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    auto swapchain = std::make_shared<TestSwapchain>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);
    NLS::Render::Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    auto frameFence = std::make_shared<TestFence>();
    auto imageAcquiredSemaphore = std::make_shared<TestSemaphore>();
    auto renderFinishedSemaphore = std::make_shared<TestSemaphore>();
    commandPool->commandBuffer = commandBuffer;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = frameFence;
    frameContext.imageAcquiredSemaphore = imageAcquiredSemaphore;
    frameContext.renderFinishedSemaphore = renderFinishedSemaphore;

    auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 121u;
    snapshot.targetsSwapchain = true;
    snapshot.visibleOpaqueDrawCount = 1u;

    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessFrameSnapshot(driver, snapshot));

    const NLS::Render::Context::InFlightFrameSlot* retiredSlot = nullptr;
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        retiredSlot = lifecycle->PeekSlot(0u);
        if (retiredSlot != nullptr && retiredSlot->stage == NLS::Render::Context::ThreadedFrameStage::Retired)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    ASSERT_NE(retiredSlot, nullptr);
    EXPECT_EQ(retiredSlot->stage, NLS::Render::Context::ThreadedFrameStage::Retired);
    ASSERT_TRUE(retiredSlot->submissionFrame.has_value());
    EXPECT_FALSE(retiredSlot->submissionFrame->offscreenOnly);
    EXPECT_EQ(retiredSlot->submissionFrame->frameContextIndex, 0u);
    EXPECT_TRUE(retiredSlot->submissionFrame->retirementFenceWaited);
    EXPECT_EQ(frameFence->waitCalls, 2u);
    EXPECT_EQ(frameFence->resetCalls, 1u);
    EXPECT_EQ(commandPool->resetCalls, 1u);
    EXPECT_EQ(commandBuffer->resetCalls, 1u);
    EXPECT_EQ(commandBuffer->beginCalls, 1u);
    EXPECT_EQ(commandBuffer->endCalls, 1u);
    EXPECT_EQ(swapchain->acquireCalls, 1u);
    EXPECT_EQ(swapchain->lastBackbufferIndex, 1u);
    EXPECT_EQ(frameContext.swapchainBackbufferView, swapchain->backbufferView);
    EXPECT_TRUE(frameContext.hasAcquiredSwapchainImage);
    EXPECT_EQ(frameContext.swapchainImageIndex, 1u);
    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    EXPECT_EQ(explicitDevice->GetTestQueue()->presentCalls, 1u);
    EXPECT_FALSE(retiredSlot->submissionFrame->usedSerialCommandPath);
    EXPECT_FALSE(retiredSlot->submissionFrame->usedParallelCommandPath);
    EXPECT_EQ(retiredSlot->submissionFrame->parallelRecordingWorkerCount, 0u);
    EXPECT_EQ(retiredSlot->submissionFrame->translatedWorkUnitCount, 0u);
    EXPECT_TRUE(explicitDevice->GetCreatedCommandPools().empty());
    ASSERT_EQ(explicitDevice->GetTestQueue()->lastSubmitDesc.commandBuffers.size(), 1u);
    EXPECT_EQ(explicitDevice->GetTestQueue()->lastSubmitDesc.commandBuffers.front(), commandBuffer);
}

TEST(ThreadedRenderingLifecycleTests, ThreadedMainlinePresentDoesNotRequireMainThreadPresentHandshake)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    auto swapchain = std::make_shared<TestSwapchain>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);
    NLS::Render::Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    auto frameFence = std::make_shared<TestFence>();
    auto imageAcquiredSemaphore = std::make_shared<TestSemaphore>();
    auto renderFinishedSemaphore = std::make_shared<TestSemaphore>();
    commandPool->commandBuffer = commandBuffer;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = frameFence;
    frameContext.imageAcquiredSemaphore = imageAcquiredSemaphore;
    frameContext.renderFinishedSemaphore = renderFinishedSemaphore;

    auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 122u;
    snapshot.targetsSwapchain = true;
    snapshot.visibleOpaqueDrawCount = 1u;

    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessFrameSnapshot(driver, snapshot));

    const NLS::Render::Context::InFlightFrameSlot* retiredSlot = nullptr;
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        retiredSlot = lifecycle->PeekSlot(0u);
        if (retiredSlot != nullptr && retiredSlot->stage == NLS::Render::Context::ThreadedFrameStage::Retired)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    ASSERT_NE(retiredSlot, nullptr);
    ASSERT_TRUE(retiredSlot->submissionFrame.has_value());
    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    EXPECT_EQ(explicitDevice->GetTestQueue()->presentCalls, 1u);
    EXPECT_EQ(swapchain->acquireCalls, 1u);

    NLS::Render::Context::DriverUIAccess::PresentSwapchain(driver);

    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    EXPECT_EQ(explicitDevice->GetTestQueue()->presentCalls, 1u);
    EXPECT_EQ(swapchain->acquireCalls, 1u);
}

TEST(ThreadedRenderingLifecycleTests, DriverRhiWorkerSkipsRejectedHarnessOffscreenFramesWithoutAcquireOrPresent)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    auto swapchain = std::make_shared<TestSwapchain>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);
    NLS::Render::Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    auto frameFence = std::make_shared<TestFence>();
    commandPool->commandBuffer = commandBuffer;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = frameFence;
    frameContext.resourceStateTracker = NLS::Render::RHI::CreateDefaultResourceStateTracker();

    auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 131u;
    snapshot.targetsSwapchain = false;
    snapshot.visibleOpaqueDrawCount = 1u;

    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessFrameSnapshot(driver, snapshot));

    const NLS::Render::Context::InFlightFrameSlot* retiredSlot = nullptr;
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        retiredSlot = lifecycle->PeekSlot(0u);
        if (retiredSlot != nullptr && retiredSlot->stage == NLS::Render::Context::ThreadedFrameStage::Retired)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    ASSERT_NE(retiredSlot, nullptr);
    ASSERT_TRUE(retiredSlot->submissionFrame.has_value());
    EXPECT_TRUE(retiredSlot->submissionFrame->offscreenOnly);
    EXPECT_EQ(frameFence->waitCalls, 1u);
    EXPECT_EQ(swapchain->acquireCalls, 0u);
    EXPECT_FALSE(frameContext.hasAcquiredSwapchainImage);
    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 0u);
    EXPECT_EQ(explicitDevice->GetTestQueue()->presentCalls, 0u);
    auto submittedCommandBuffer = GetSubmittedTestCommandBuffer(explicitDevice->GetTestQueue());
    EXPECT_EQ(submittedCommandBuffer, nullptr);
}

TEST(ThreadedRenderingLifecycleTests, SynchronousDrainCannotEnterRhiSubmissionWhileAnotherSubmissionIsActive)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 2u;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto blockingDescriptorAllocator = std::make_shared<BlockingBeginFrameDescriptorAllocator>();
    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.descriptorAllocator = blockingDescriptorAllocator;

    NLS::Render::Context::FrameSnapshot firstSnapshot;
    firstSnapshot.frameId = 711u;
    firstSnapshot.targetsSwapchain = true;
    firstSnapshot.renderWidth = 64u;
    firstSnapshot.renderHeight = 64u;

    NLS::Render::Context::RenderScenePackage firstPackage;
    firstPackage.frameId = firstSnapshot.frameId;
    firstPackage.targetsSwapchain = true;
    firstPackage.renderWidth = 64u;
    firstPackage.renderHeight = 64u;

    NLS::Render::Context::FrameSnapshot secondSnapshot = firstSnapshot;
    secondSnapshot.frameId = 712u;
    NLS::Render::Context::RenderScenePackage secondPackage = firstPackage;
    secondPackage.frameId = secondSnapshot.frameId;

    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessPreparedFrame(
        driver,
        firstSnapshot,
        firstPackage));
    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessPreparedFrame(
        driver,
        secondSnapshot,
        secondPackage));

    std::thread firstDrain([&driver]()
    {
        NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);
    });

    ASSERT_TRUE(blockingDescriptorAllocator->WaitForFirstBeginFrame(std::chrono::milliseconds(200)));

    std::thread secondDrain([&driver]()
    {
        NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);
    });

    EXPECT_FALSE(blockingDescriptorAllocator->WaitForConcurrentBeginFrame(std::chrono::milliseconds(50)));

    blockingDescriptorAllocator->ReleaseFirstBeginFrame();
    if (firstDrain.joinable())
        firstDrain.join();
    if (secondDrain.joinable())
        secondDrain.join();

    EXPECT_EQ(blockingDescriptorAllocator->GetMaxConcurrentBeginFrameCalls(), 1u);
}

TEST(ThreadedRenderingLifecycleTests, DriverRhiWorkerTransitionsExtractedOffscreenTexturesToShaderRead)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    explicitDevice->SetNativeBackendType(NLS::Render::RHI::NativeBackendType::DX12);
    auto& capabilities = explicitDevice->MutableCapabilities();
    capabilities.supportsCompute = true;
    capabilities.supportsSwapchain = true;
    capabilities.supportsCurrentSceneRenderer = true;
    capabilities.supportsOffscreenFramebuffers = true;
    capabilities.supportsMultiRenderTargets = true;
    capabilities.supportsExplicitBarriers = true;
    capabilities.supportsCentralizedDescriptorManagement = true;
    capabilities.supportsPipelineStateCache = true;
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    auto frameFence = std::make_shared<TestFence>();
    commandPool->commandBuffer = commandBuffer;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = frameFence;
    frameContext.resourceStateTracker = NLS::Render::RHI::CreateDefaultResourceStateTracker();

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 611u;
    snapshot.targetsSwapchain = false;
    snapshot.visibleOpaqueDrawCount = 1u;

    NLS::Render::Context::RenderScenePackage package;
    package.frameId = snapshot.frameId;
    package.targetsSwapchain = false;
    package.visibleDrawCount = 1u;
    package.opaqueDrawCount = 1u;
    package.hasVisibleDraws = true;
    package.frameDataReady = true;
    package.objectDataReady = true;
    package.containsCommandInputs = true;

    NLS::Render::RHI::RHITextureDesc outputTextureDesc;
    outputTextureDesc.debugName = "ExtractedOffscreenColor";
    outputTextureDesc.extent = { 128u, 72u, 1u };
    outputTextureDesc.format = NLS::Render::RHI::TextureFormat::RGBA8;
    outputTextureDesc.usage = NLS::Render::RHI::TextureUsageFlags::ColorAttachment | NLS::Render::RHI::TextureUsageFlags::Sampled;
    auto outputTexture = explicitDevice->CreateTexture(outputTextureDesc);

    NLS::Render::RHI::RHITextureViewDesc outputViewDesc;
    outputViewDesc.debugName = "ExtractedOffscreenColorView";
    auto outputView = explicitDevice->CreateTextureView(outputTexture, outputViewDesc);

    NLS::Render::Context::RenderPassCommandInput passInput;
    passInput.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    passInput.drawCount = 1u;
    passInput.requiresFrameData = true;
    passInput.requiresObjectData = true;
    passInput.targetsSwapchain = false;
    passInput.renderWidth = 128u;
    passInput.renderHeight = 72u;
    passInput.debugName = "GraphDerivedOpaquePass";
    passInput.clearColor = true;
    passInput.clearDepth = false;
    passInput.clearStencil = false;
    passInput.usesColorAttachment = true;
    passInput.usesDepthStencilAttachment = false;
    passInput.colorAttachmentViews.push_back(outputView);
    package.passCommandInputs.push_back(passInput);
    package.extractedTextures.push_back(outputTexture);

    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessPreparedFrame(driver, snapshot, package));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    auto renderPassCommandBuffer = FindSubmittedTestCommandBuffer(
        explicitDevice->GetTestQueue(),
        [](const TestCommandBuffer& buffer) { return buffer.beginRenderPassCalls > 0u; });
    ASSERT_NE(renderPassCommandBuffer, nullptr);
    EXPECT_EQ(renderPassCommandBuffer->lastRenderPassDesc.debugName, "GraphDerivedOpaquePass");

    auto transitionCommandBuffer = FindSubmittedTestCommandBuffer(
        explicitDevice->GetTestQueue(),
        [](const TestCommandBuffer& buffer) { return !buffer.barrierHistory.empty(); });
    ASSERT_NE(transitionCommandBuffer, nullptr);
    const auto& extractionBarrier = transitionCommandBuffer->barrierHistory.back();
    ASSERT_EQ(extractionBarrier.textureBarriers.size(), 1u);
    EXPECT_EQ(extractionBarrier.textureBarriers[0].texture, outputTexture);
    EXPECT_EQ(extractionBarrier.textureBarriers[0].after, NLS::Render::RHI::ResourceState::ShaderRead);
    EXPECT_EQ(extractionBarrier.textureBarriers[0].destinationAccessMask, NLS::Render::RHI::AccessMask::ShaderRead);
}

TEST(ThreadedRenderingLifecycleTests, RhiWorkerMarksVisiblePackageAsRecordedBeforeSubmit)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    auto frameFence = std::make_shared<TestFence>();
    commandPool->commandBuffer = commandBuffer;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = frameFence;
    frameContext.resourceStateTracker = NLS::Render::RHI::CreateDefaultResourceStateTracker();

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 401u;
    snapshot.targetsSwapchain = false;
    snapshot.visibleOpaqueDrawCount = 1u;

    NLS::Render::Context::RenderScenePackage package;
    package.frameId = snapshot.frameId;
    package.targetsSwapchain = false;
    package.visibleDrawCount = 1u;
    package.opaqueDrawCount = 1u;
    package.hasVisibleDraws = true;
    package.frameDataReady = true;
    package.objectDataReady = true;
    package.passPlanCount = 1u;

    NLS::Render::Context::RenderPassCommandInput passInput;
    passInput.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    passInput.drawCount = 1u;
    passInput.requiresFrameData = true;
    passInput.requiresObjectData = true;
    passInput.targetsSwapchain = false;
    passInput.renderWidth = 64u;
    passInput.renderHeight = 64u;
    passInput.clearColor = true;
    passInput.clearDepth = true;
    passInput.usesColorAttachment = true;
    passInput.usesDepthStencilAttachment = true;
    package.passCommandInputs.push_back(passInput);

    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessPreparedFrame(
        driver,
        snapshot,
        package));

    auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);

    const auto copiedSlots = WaitForRetiredCopiedSlots(*lifecycle);
    ASSERT_EQ(copiedSlots.size(), 1u);
    ASSERT_TRUE(copiedSlots[0].submissionFrame.has_value());
    EXPECT_TRUE(copiedSlots[0].submissionFrame->recordedVisibleWork);
    EXPECT_EQ(copiedSlots[0].submissionFrame->recordedDrawCount, 1u);
    EXPECT_EQ(copiedSlots[0].submissionFrame->recordedPassCount, 1u);
    auto submittedCommandBuffer = GetSubmittedTestCommandBuffer(explicitDevice->GetTestQueue());
    ASSERT_NE(submittedCommandBuffer, nullptr);
    EXPECT_EQ(submittedCommandBuffer->beginRenderPassCalls, 1u);
    EXPECT_EQ(submittedCommandBuffer->endRenderPassCalls, 1u);
    EXPECT_EQ(submittedCommandBuffer->setViewportCalls, 1u);
}

TEST(ThreadedRenderingLifecycleTests, RhiWorkerConsumesPassOwnedRecordedDrawCommandsInsideRenderPass)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    auto frameFence = std::make_shared<TestFence>();
    commandPool->commandBuffer = commandBuffer;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = frameFence;
    frameContext.resourceStateTracker = NLS::Render::RHI::CreateDefaultResourceStateTracker();

    auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 403u;
    snapshot.targetsSwapchain = false;
    snapshot.visibleOpaqueDrawCount = 1u;
    snapshot.visibleTransparentDrawCount = 1u;
    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessFrameSnapshot(driver, snapshot));

    size_t slotIndex = 0u;
    NLS::Render::Context::FrameSnapshot publishedSnapshot;
    ASSERT_TRUE(lifecycle->TryBeginNextRenderScene(&slotIndex, &publishedSnapshot));

    auto opaquePipeline = std::make_shared<TestGraphicsPipeline>("OpaquePipeline");
    auto transparentPipeline = std::make_shared<TestGraphicsPipeline>("TransparentPipeline");
    auto materialBindingSet = std::make_shared<TestBindingSet>("MaterialBindingSet");
    auto mesh = std::make_shared<TestMesh>();

    NLS::Render::Context::RenderScenePackage package;
    package.frameId = publishedSnapshot.frameId;
    package.targetsSwapchain = false;
    package.visibleDrawCount = 2u;
    package.opaqueDrawCount = 1u;
    package.transparentDrawCount = 1u;
    package.hasVisibleDraws = true;
    package.frameDataReady = true;
    package.objectDataReady = true;
    package.renderWidth = 64u;
    package.renderHeight = 64u;

    NLS::Render::Context::RenderPassCommandInput opaquePassInput;
    opaquePassInput.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    opaquePassInput.drawCount = 1u;
    opaquePassInput.requiresFrameData = true;
    opaquePassInput.requiresObjectData = true;
    opaquePassInput.targetsSwapchain = false;
    opaquePassInput.renderWidth = 64u;
    opaquePassInput.renderHeight = 64u;
    opaquePassInput.clearColor = true;
    opaquePassInput.clearDepth = true;
    opaquePassInput.usesColorAttachment = true;
    opaquePassInput.usesDepthStencilAttachment = true;
    opaquePassInput.recordedDrawCommands.push_back({ opaquePipeline, nullptr, nullptr, nullptr, materialBindingSet, mesh, 1u });

    NLS::Render::Context::RenderPassCommandInput transparentPassInput = opaquePassInput;
    transparentPassInput.kind = NLS::Render::Context::RenderPassCommandKind::Transparent;
    transparentPassInput.clearColor = false;
    transparentPassInput.clearDepth = false;
    transparentPassInput.usesDepthStencilAttachment = false;
    transparentPassInput.recordedDrawCommands = {
        { transparentPipeline, nullptr, nullptr, nullptr, materialBindingSet, mesh, 1u }
    };

    package.passCommandInputs = { opaquePassInput, transparentPassInput };
    ASSERT_TRUE(lifecycle->CompleteRenderScene(slotIndex, package));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    const auto copiedSlot = lifecycle->CopySlot(0u);
    ASSERT_TRUE(copiedSlot.has_value());
    ASSERT_TRUE(copiedSlot->submissionFrame.has_value());
    EXPECT_TRUE(copiedSlot->submissionFrame->recordedVisibleWork);
    EXPECT_EQ(copiedSlot->submissionFrame->recordedPassCount, 2u);
    EXPECT_EQ(copiedSlot->submissionFrame->recordedDrawCount, 2u);
    uint32_t totalBindGraphicsPipelineCalls = 0u;
    uint32_t totalDrawIndexedCalls = 0u;
    bool sawDrawBeforeEndPass = false;
    for (const auto& submittedBufferBase : explicitDevice->GetTestQueue()->lastSubmitDesc.commandBuffers)
    {
        auto submittedCommandBuffer = std::dynamic_pointer_cast<TestCommandBuffer>(submittedBufferBase);
        if (submittedCommandBuffer == nullptr)
            continue;

        totalBindGraphicsPipelineCalls += static_cast<uint32_t>(submittedCommandBuffer->bindGraphicsPipelineCalls);
        totalDrawIndexedCalls += static_cast<uint32_t>(submittedCommandBuffer->drawIndexedCalls);

        const auto firstDraw = std::find(submittedCommandBuffer->events.begin(), submittedCommandBuffer->events.end(), "DrawIndexed");
        const auto firstEndPass = std::find(submittedCommandBuffer->events.begin(), submittedCommandBuffer->events.end(), "EndRenderPass");
        if (firstDraw != submittedCommandBuffer->events.end() &&
            firstEndPass != submittedCommandBuffer->events.end() &&
            std::distance(submittedCommandBuffer->events.begin(), firstDraw) <
                std::distance(submittedCommandBuffer->events.begin(), firstEndPass))
        {
            sawDrawBeforeEndPass = true;
        }
    }

    EXPECT_EQ(totalBindGraphicsPipelineCalls, 2u);
    EXPECT_EQ(totalDrawIndexedCalls, 2u);
    EXPECT_TRUE(sawDrawBeforeEndPass);
}

TEST(ThreadedRenderingLifecycleTests, SerialCommandPathConsumesPreparedParallelCommandWorkUnits)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    explicitDevice->MutableCapabilities().supportsAsyncCompute = true;
    explicitDevice->MutableCapabilities().supportsDedicatedComputeQueue = true;
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    auto frameFence = std::make_shared<TestFence>();
    auto computeFinishedSemaphore = std::make_shared<TestSemaphore>();
    commandPool->commandBuffer = commandBuffer;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = frameFence;
    frameContext.computeFinishedSemaphore = computeFinishedSemaphore;
    frameContext.resourceStateTracker = NLS::Render::RHI::CreateDefaultResourceStateTracker();

    auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 404u;
    snapshot.targetsSwapchain = false;
    snapshot.visibleOpaqueDrawCount = 1u;
    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessFrameSnapshot(driver, snapshot));

    size_t slotIndex = 0u;
    NLS::Render::Context::FrameSnapshot publishedSnapshot;
    ASSERT_TRUE(lifecycle->TryBeginNextRenderScene(&slotIndex, &publishedSnapshot));

    auto opaquePipeline = std::make_shared<TestGraphicsPipeline>("OpaquePipeline");
    auto computePipeline = std::make_shared<TestComputePipeline>("LightGridComputePipeline");
    auto materialBindingSet = std::make_shared<TestBindingSet>("MaterialBindingSet");
    auto mesh = std::make_shared<TestMesh>();

    NLS::Render::Context::RenderScenePackage package;
    package.frameId = publishedSnapshot.frameId;
    package.targetsSwapchain = false;
    package.visibleDrawCount = 1u;
    package.opaqueDrawCount = 1u;
    package.hasVisibleDraws = true;
    package.frameDataReady = true;
    package.objectDataReady = true;
    package.renderWidth = 64u;
    package.renderHeight = 64u;

    NLS::Render::Context::RenderPassCommandInput opaquePassInput;
    opaquePassInput.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    opaquePassInput.drawCount = 1u;
    opaquePassInput.requiresFrameData = true;
    opaquePassInput.requiresObjectData = true;
    opaquePassInput.targetsSwapchain = false;
    opaquePassInput.renderWidth = 64u;
    opaquePassInput.renderHeight = 64u;
    opaquePassInput.clearColor = true;
    opaquePassInput.clearDepth = true;
    opaquePassInput.usesColorAttachment = true;
    opaquePassInput.usesDepthStencilAttachment = true;
    opaquePassInput.recordedDrawCommands.push_back({ opaquePipeline, nullptr, nullptr, nullptr, materialBindingSet, mesh, 1u });

    NLS::Render::Context::RenderPassCommandInput computePassInput;
    computePassInput.kind = NLS::Render::Context::RenderPassCommandKind::Compute;
    computePassInput.debugName = "LightGridInjection";
    computePassInput.renderWidth = 64u;
    computePassInput.renderHeight = 64u;
    computePassInput.computeDispatchInputs.push_back({
        "LightGridInjection",
        computePipeline,
        {},
        1u,
        1u,
        1u,
        {},
        {},
        {}
    });

    NLS::Render::Context::ParallelCommandWorkUnit computeWorkUnit;
    computeWorkUnit.commandInput = computePassInput;
    computeWorkUnit.debugName = computePassInput.debugName;

    NLS::Render::Context::ParallelCommandWorkUnit opaqueWorkUnit;
    opaqueWorkUnit.commandInput = opaquePassInput;
    opaqueWorkUnit.debugName = "OpaquePass";

    package.containsParallelCommandWorkUnits = true;
    package.parallelCommandWorkUnitCount = 2u;
    package.parallelCommandWorkUnits = { computeWorkUnit, opaqueWorkUnit };
    package.hasAsyncComputeWorkload = true;
    package.asyncComputeWorkloadCount = 1u;

    ASSERT_TRUE(lifecycle->CompleteRenderScene(slotIndex, package));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    const auto copiedSlot = lifecycle->CopySlot(0u);
    ASSERT_TRUE(copiedSlot.has_value());
    ASSERT_TRUE(copiedSlot->submissionFrame.has_value());
    EXPECT_TRUE(copiedSlot->submissionFrame->recordedVisibleWork);
    EXPECT_EQ(copiedSlot->submissionFrame->recordedPassCount, 2u);
    EXPECT_EQ(copiedSlot->submissionFrame->recordedWorkUnitCount, 2u);
    EXPECT_EQ(copiedSlot->submissionFrame->parallelRecordingWorkerCount, 0u);
    EXPECT_EQ(copiedSlot->submissionFrame->translatedWorkUnitCount, 0u);
    EXPECT_FALSE(copiedSlot->submissionFrame->usedParallelCommandPath);
    EXPECT_FALSE(copiedSlot->submissionFrame->usedTranslationMerge);
    EXPECT_TRUE(copiedSlot->submissionFrame->usedSerialCommandPath);
    EXPECT_EQ(
        copiedSlot->submissionFrame->asyncComputeDisposition,
        NLS::Render::Context::AsyncComputeDisposition::Submitted);
    EXPECT_EQ(copiedSlot->submissionFrame->asyncComputeCandidateWorkloadCount, 1u);
    EXPECT_TRUE(copiedSlot->submissionFrame->asyncComputeQueueAvailable);
    EXPECT_TRUE(copiedSlot->submissionFrame->usedAsyncComputeQueueSubmission);
    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    EXPECT_EQ(explicitDevice->GetComputeTestQueue()->submitCalls, 1u);
    ASSERT_EQ(explicitDevice->GetComputeTestQueue()->lastSubmitDesc.signalSemaphores.size(), 1u);
    EXPECT_EQ(explicitDevice->GetComputeTestQueue()->lastSubmitDesc.signalSemaphores.front(), computeFinishedSemaphore);
    ASSERT_EQ(explicitDevice->GetTestQueue()->lastSubmitDesc.waitSemaphores.size(), 1u);
    EXPECT_EQ(explicitDevice->GetTestQueue()->lastSubmitDesc.waitSemaphores.front(), computeFinishedSemaphore);
    ASSERT_EQ(explicitDevice->GetComputeTestQueue()->lastSubmitDesc.commandBuffers.size(), 1u);
    const auto computeSubmittedCommandBuffer = std::dynamic_pointer_cast<TestCommandBuffer>(
        explicitDevice->GetComputeTestQueue()->lastSubmitDesc.commandBuffers.front());
    ASSERT_NE(computeSubmittedCommandBuffer, nullptr);
    EXPECT_EQ(computeSubmittedCommandBuffer->dispatchCalls, 1u);
}

TEST(ThreadedRenderingLifecycleTests, AsyncComputeSchedulingBuildsGraphicsComputeGraphicsSubmissionChain)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    explicitDevice->MutableCapabilities().supportsAsyncCompute = true;
    explicitDevice->MutableCapabilities().supportsDedicatedComputeQueue = true;
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    auto frameFence = std::make_shared<TestFence>();
    auto computeFinishedSemaphore = std::make_shared<TestSemaphore>();
    commandPool->commandBuffer = commandBuffer;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = frameFence;
    frameContext.computeFinishedSemaphore = computeFinishedSemaphore;
    frameContext.resourceStateTracker = NLS::Render::RHI::CreateDefaultResourceStateTracker();

    auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 405u;
    snapshot.targetsSwapchain = false;
    snapshot.visibleOpaqueDrawCount = 2u;
    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessFrameSnapshot(driver, snapshot));

    size_t slotIndex = 0u;
    NLS::Render::Context::FrameSnapshot publishedSnapshot;
    ASSERT_TRUE(lifecycle->TryBeginNextRenderScene(&slotIndex, &publishedSnapshot));

    auto graphicsPipeline = std::make_shared<TestGraphicsPipeline>("OpaquePipeline");
    auto computePipeline = std::make_shared<TestComputePipeline>("LightGridComputePipeline");
    auto materialBindingSet = std::make_shared<TestBindingSet>("MaterialBindingSet");
    auto mesh = std::make_shared<TestMesh>();

    NLS::Render::Context::RenderPassCommandInput firstGraphicsPass;
    firstGraphicsPass.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    firstGraphicsPass.debugName = "ForwardOpaqueA";
    firstGraphicsPass.drawCount = 1u;
    firstGraphicsPass.requiresFrameData = true;
    firstGraphicsPass.requiresObjectData = true;
    firstGraphicsPass.targetsSwapchain = false;
    firstGraphicsPass.renderWidth = 64u;
    firstGraphicsPass.renderHeight = 64u;
    firstGraphicsPass.clearColor = true;
    firstGraphicsPass.clearDepth = true;
    firstGraphicsPass.usesColorAttachment = true;
    firstGraphicsPass.usesDepthStencilAttachment = true;
    firstGraphicsPass.recordedDrawCommands.push_back({ graphicsPipeline, nullptr, nullptr, nullptr, materialBindingSet, mesh, 1u });

    NLS::Render::Context::RenderPassCommandInput computePass;
    computePass.kind = NLS::Render::Context::RenderPassCommandKind::Compute;
    computePass.debugName = "LightGridCompact";
    computePass.queueDependencyPolicy = NLS::Render::Context::QueueDependencyPolicy::None;
    computePass.renderWidth = 64u;
    computePass.renderHeight = 64u;
    computePass.computeDispatchInputs.push_back({
        "LightGridCompact",
        computePipeline,
        {},
        1u,
        1u,
        1u,
        {},
        {},
        {}
    });

    NLS::Render::Context::RenderPassCommandInput secondGraphicsPass = firstGraphicsPass;
    secondGraphicsPass.debugName = "ForwardOpaqueB";

    NLS::Render::Context::ParallelCommandWorkUnit firstGraphicsWorkUnit;
    firstGraphicsWorkUnit.commandInput = firstGraphicsPass;
    firstGraphicsWorkUnit.debugName = firstGraphicsPass.debugName;

    NLS::Render::Context::ParallelCommandWorkUnit computeWorkUnit;
    computeWorkUnit.commandInput = computePass;
    computeWorkUnit.debugName = computePass.debugName;

    NLS::Render::Context::ParallelCommandWorkUnit secondGraphicsWorkUnit;
    secondGraphicsWorkUnit.commandInput = secondGraphicsPass;
    secondGraphicsWorkUnit.debugName = secondGraphicsPass.debugName;

    NLS::Render::Context::RenderScenePackage package;
    package.frameId = publishedSnapshot.frameId;
    package.targetsSwapchain = false;
    package.visibleDrawCount = 2u;
    package.opaqueDrawCount = 2u;
    package.hasVisibleDraws = true;
    package.frameDataReady = true;
    package.objectDataReady = true;
    package.renderWidth = 64u;
    package.renderHeight = 64u;
    package.containsParallelCommandWorkUnits = true;
    package.parallelCommandWorkUnitCount = 3u;
    package.parallelCommandWorkUnits = {
        firstGraphicsWorkUnit,
        computeWorkUnit,
        secondGraphicsWorkUnit
    };
    package.hasAsyncComputeWorkload = true;
    package.asyncComputeWorkloadCount = 1u;

    ASSERT_TRUE(lifecycle->CompleteRenderScene(slotIndex, package));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    const auto copiedSlot = lifecycle->CopySlot(0u);
    ASSERT_TRUE(copiedSlot.has_value());
    ASSERT_TRUE(copiedSlot->submissionFrame.has_value());
    EXPECT_TRUE(copiedSlot->submissionFrame->usedAsyncComputeQueueSubmission);
    EXPECT_EQ(
        copiedSlot->submissionFrame->asyncComputeDisposition,
        NLS::Render::Context::AsyncComputeDisposition::Submitted);

    auto graphicsQueue = explicitDevice->GetTestQueue();
    auto computeQueue = explicitDevice->GetComputeTestQueue();
    ASSERT_NE(graphicsQueue, nullptr);
    ASSERT_NE(computeQueue, nullptr);
    EXPECT_EQ(graphicsQueue->submitCalls, 2u);
    EXPECT_EQ(computeQueue->submitCalls, 1u);
    ASSERT_EQ(graphicsQueue->submitHistory.size(), 2u);
    ASSERT_EQ(computeQueue->submitHistory.size(), 1u);

    EXPECT_TRUE(graphicsQueue->submitHistory[0].signalSemaphores.empty());
    EXPECT_TRUE(computeQueue->submitHistory[0].waitSemaphores.empty());
    ASSERT_EQ(computeQueue->submitHistory[0].signalSemaphores.size(), 1u);
    ASSERT_EQ(graphicsQueue->submitHistory[1].waitSemaphores.size(), 1u);
    EXPECT_EQ(
        graphicsQueue->submitHistory[1].waitSemaphores.front(),
        computeQueue->submitHistory[0].signalSemaphores.front());
    EXPECT_EQ(
        graphicsQueue->submitHistory[1].waitSemaphores.front(),
        computeFinishedSemaphore);
}

TEST(ThreadedRenderingLifecycleTests, AsyncComputeSchedulingAllowsNonAdjacentGraphicsConsumerToWaitOnLastComputeBatch)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    explicitDevice->MutableCapabilities().supportsAsyncCompute = true;
    explicitDevice->MutableCapabilities().supportsDedicatedComputeQueue = true;
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    auto frameFence = std::make_shared<TestFence>();
    auto computeFinishedSemaphore = std::make_shared<TestSemaphore>();
    commandPool->commandBuffer = commandBuffer;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = frameFence;
    frameContext.computeFinishedSemaphore = computeFinishedSemaphore;
    frameContext.resourceStateTracker = NLS::Render::RHI::CreateDefaultResourceStateTracker();

    auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 406u;
    snapshot.targetsSwapchain = false;
    snapshot.visibleOpaqueDrawCount = 3u;
    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessFrameSnapshot(driver, snapshot));

    size_t slotIndex = 0u;
    NLS::Render::Context::FrameSnapshot publishedSnapshot;
    ASSERT_TRUE(lifecycle->TryBeginNextRenderScene(&slotIndex, &publishedSnapshot));

    auto graphicsPipeline = std::make_shared<TestGraphicsPipeline>("OpaquePipeline");
    auto computePipeline = std::make_shared<TestComputePipeline>("LightGridComputePipeline");
    auto materialBindingSet = std::make_shared<TestBindingSet>("MaterialBindingSet");
    auto mesh = std::make_shared<TestMesh>();

    NLS::Render::Context::RenderPassCommandInput graphicsPass;
    graphicsPass.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    graphicsPass.drawCount = 1u;
    graphicsPass.requiresFrameData = true;
    graphicsPass.requiresObjectData = true;
    graphicsPass.targetsSwapchain = false;
    graphicsPass.renderWidth = 64u;
    graphicsPass.renderHeight = 64u;
    graphicsPass.clearColor = true;
    graphicsPass.clearDepth = true;
    graphicsPass.usesColorAttachment = true;
    graphicsPass.usesDepthStencilAttachment = true;
    graphicsPass.recordedDrawCommands.push_back({ graphicsPipeline, nullptr, nullptr, nullptr, materialBindingSet, mesh, 1u });

    NLS::Render::Context::RenderPassCommandInput computePass;
    computePass.kind = NLS::Render::Context::RenderPassCommandKind::Compute;
    computePass.debugName = "LightGridCompact";
    computePass.queueDependencyPolicy = NLS::Render::Context::QueueDependencyPolicy::None;
    computePass.renderWidth = 64u;
    computePass.renderHeight = 64u;
    computePass.computeDispatchInputs.push_back({
        "LightGridCompact",
        computePipeline,
        {},
        1u,
        1u,
        1u,
        {},
        {},
        {}
    });

    NLS::Render::Context::RenderPassCommandInput graphicsPassB = graphicsPass;
    graphicsPassB.debugName = "ForwardOpaqueB";
    graphicsPassB.queueDependencyPolicy = NLS::Render::Context::QueueDependencyPolicy::None;

    NLS::Render::Context::RenderPassCommandInput graphicsPassC = graphicsPass;
    graphicsPassC.debugName = "ForwardOpaqueC";
    graphicsPassC.queueDependencyPolicy = NLS::Render::Context::QueueDependencyPolicy::Previous;
    graphicsPassC.dependencySourceWorkUnitIndex = 1u;
    graphicsPassC.requiresDependencyVisibility = true;

    NLS::Render::Context::ParallelCommandWorkUnit graphicsWorkUnitA;
    graphicsWorkUnitA.debugName = "ForwardOpaqueA";
    graphicsWorkUnitA.commandInput = graphicsPass;

    NLS::Render::Context::ParallelCommandWorkUnit computeWorkUnit;
    computeWorkUnit.commandInput = computePass;
    computeWorkUnit.debugName = computePass.debugName;

    NLS::Render::Context::ParallelCommandWorkUnit graphicsWorkUnitB;
    graphicsWorkUnitB.commandInput = graphicsPassB;
    graphicsWorkUnitB.debugName = graphicsPassB.debugName;

    NLS::Render::Context::ParallelCommandWorkUnit graphicsWorkUnitC;
    graphicsWorkUnitC.commandInput = graphicsPassC;
    graphicsWorkUnitC.debugName = graphicsPassC.debugName;

    NLS::Render::Context::RenderScenePackage package;
    package.frameId = publishedSnapshot.frameId;
    package.targetsSwapchain = false;
    package.visibleDrawCount = 3u;
    package.opaqueDrawCount = 3u;
    package.hasVisibleDraws = true;
    package.frameDataReady = true;
    package.objectDataReady = true;
    package.renderWidth = 64u;
    package.renderHeight = 64u;
    package.containsParallelCommandWorkUnits = true;
    package.parallelCommandWorkUnitCount = 4u;
    package.parallelCommandWorkUnits = {
        graphicsWorkUnitA,
        computeWorkUnit,
        graphicsWorkUnitB,
        graphicsWorkUnitC
    };
    package.hasAsyncComputeWorkload = true;
    package.asyncComputeWorkloadCount = 1u;

    ASSERT_TRUE(lifecycle->CompleteRenderScene(slotIndex, package));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    const auto copiedSlot = lifecycle->CopySlot(0u);
    ASSERT_TRUE(copiedSlot.has_value());
    ASSERT_TRUE(copiedSlot->submissionFrame.has_value());
    EXPECT_TRUE(copiedSlot->submissionFrame->usedAsyncComputeQueueSubmission);
    EXPECT_EQ(
        copiedSlot->submissionFrame->asyncComputeDisposition,
        NLS::Render::Context::AsyncComputeDisposition::Submitted);

    auto graphicsQueue = explicitDevice->GetTestQueue();
    auto computeQueue = explicitDevice->GetComputeTestQueue();
    ASSERT_NE(graphicsQueue, nullptr);
    ASSERT_NE(computeQueue, nullptr);
    EXPECT_EQ(graphicsQueue->submitCalls, 3u);
    EXPECT_EQ(computeQueue->submitCalls, 1u);
    ASSERT_EQ(graphicsQueue->submitHistory.size(), 3u);
    ASSERT_EQ(computeQueue->submitHistory.size(), 1u);

    EXPECT_TRUE(computeQueue->submitHistory[0].waitSemaphores.empty());
    EXPECT_TRUE(graphicsQueue->submitHistory[1].waitSemaphores.empty());
    ASSERT_EQ(computeQueue->submitHistory[0].signalSemaphores.size(), 1u);
    ASSERT_EQ(graphicsQueue->submitHistory[2].waitSemaphores.size(), 1u);
    EXPECT_EQ(
        graphicsQueue->submitHistory[2].waitSemaphores.front(),
        computeQueue->submitHistory[0].signalSemaphores.front());
    EXPECT_EQ(
        graphicsQueue->submitHistory[2].waitSemaphores.front(),
        computeFinishedSemaphore);
}

TEST(ThreadedRenderingLifecycleTests, ParallelRecordingUsesMultipleWorkersForEligibleWorkUnits)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    explicitDevice->MutableCapabilities().supportsParallelCommandRecording = true;
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    auto frameFence = std::make_shared<TestFence>();
    commandPool->commandBuffer = commandBuffer;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = frameFence;
    frameContext.resourceStateTracker = NLS::Render::RHI::CreateDefaultResourceStateTracker();

    auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 405u;
    snapshot.targetsSwapchain = false;
    snapshot.visibleOpaqueDrawCount = 2u;
    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessFrameSnapshot(driver, snapshot));

    size_t slotIndex = 0u;
    NLS::Render::Context::FrameSnapshot publishedSnapshot;
    ASSERT_TRUE(lifecycle->TryBeginNextRenderScene(&slotIndex, &publishedSnapshot));

    auto opaquePipeline = std::make_shared<TestGraphicsPipeline>("OpaquePipeline");
    auto transparentPipeline = std::make_shared<TestGraphicsPipeline>("TransparentPipeline");
    auto materialBindingSet = std::make_shared<TestBindingSet>("MaterialBindingSet");
    auto mesh = std::make_shared<TestMesh>();

    NLS::Render::Context::RenderPassCommandInput opaquePassInput;
    opaquePassInput.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    opaquePassInput.drawCount = 1u;
    opaquePassInput.requiresFrameData = true;
    opaquePassInput.requiresObjectData = true;
    opaquePassInput.targetsSwapchain = false;
    opaquePassInput.renderWidth = 64u;
    opaquePassInput.renderHeight = 64u;
    opaquePassInput.clearColor = true;
    opaquePassInput.clearDepth = true;
    opaquePassInput.usesColorAttachment = true;
    opaquePassInput.usesDepthStencilAttachment = true;
    opaquePassInput.recordedDrawCommands.push_back({ opaquePipeline, nullptr, nullptr, nullptr, materialBindingSet, mesh, 1u });

    NLS::Render::Context::RenderPassCommandInput transparentPassInput = opaquePassInput;
    transparentPassInput.kind = NLS::Render::Context::RenderPassCommandKind::Transparent;
    transparentPassInput.clearColor = false;
    transparentPassInput.clearDepth = false;
    transparentPassInput.usesDepthStencilAttachment = false;
    transparentPassInput.recordedDrawCommands = {
        { transparentPipeline, nullptr, nullptr, nullptr, materialBindingSet, mesh, 1u }
    };

    NLS::Render::Context::ParallelCommandWorkUnit opaqueWorkUnit;
    opaqueWorkUnit.commandInput = opaquePassInput;
    opaqueWorkUnit.debugName = "ParallelOpaque";
    opaqueWorkUnit.eligibleForParallelRecording = true;

    NLS::Render::Context::ParallelCommandWorkUnit transparentWorkUnit;
    transparentWorkUnit.commandInput = transparentPassInput;
    transparentWorkUnit.debugName = "ParallelTransparent";
    transparentWorkUnit.eligibleForParallelRecording = true;

    NLS::Render::Context::RenderScenePackage package;
    package.frameId = publishedSnapshot.frameId;
    package.targetsSwapchain = false;
    package.visibleDrawCount = 2u;
    package.opaqueDrawCount = 1u;
    package.transparentDrawCount = 1u;
    package.hasVisibleDraws = true;
    package.frameDataReady = true;
    package.objectDataReady = true;
    package.renderWidth = 64u;
    package.renderHeight = 64u;
    package.containsParallelCommandWorkUnits = true;
    package.parallelCommandWorkUnitCount = 2u;
    package.parallelCommandWorkUnits = { opaqueWorkUnit, transparentWorkUnit };

    ASSERT_TRUE(lifecycle->CompleteRenderScene(slotIndex, package));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    const auto copiedSlot = lifecycle->CopySlot(0u);
    ASSERT_TRUE(copiedSlot.has_value());
    ASSERT_TRUE(copiedSlot->submissionFrame.has_value());
    EXPECT_TRUE(copiedSlot->submissionFrame->recordedVisibleWork);
    EXPECT_EQ(copiedSlot->submissionFrame->recordedPassCount, 2u);
    EXPECT_EQ(copiedSlot->submissionFrame->recordedWorkUnitCount, 2u);
    EXPECT_EQ(copiedSlot->submissionFrame->parallelRecordingWorkerCount, 2u);
    EXPECT_EQ(copiedSlot->submissionFrame->translatedWorkUnitCount, 0u);
    EXPECT_TRUE(copiedSlot->submissionFrame->usedParallelCommandPath);
    EXPECT_FALSE(copiedSlot->submissionFrame->usedTranslationMerge);
    EXPECT_FALSE(copiedSlot->submissionFrame->usedSerialCommandPath);
    ASSERT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    EXPECT_EQ(explicitDevice->GetTestQueue()->lastSubmitDesc.commandBuffers.size(), 2u);
    EXPECT_EQ(explicitDevice->GetCreatedCommandPools().size(), 2u);
    for (const auto& submittedBuffer : explicitDevice->GetTestQueue()->lastSubmitDesc.commandBuffers)
        EXPECT_NE(submittedBuffer, frameContext.commandBuffer);
}

TEST(ThreadedRenderingLifecycleTests, SubmissionDiagnosticsCaptureDescriptorAndTransientLifetimeStats)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);
    auto pipelineCache = NLS::Render::Context::DriverRendererAccess::GetPipelineCache(driver);
    ASSERT_NE(pipelineCache, nullptr);

    NLS::Render::RHI::PipelineCacheKey graphicsKey;
    graphicsKey.hash = 0x1234u;
    graphicsKey.backend = NLS::Render::RHI::NativeBackendType::DX12;
    graphicsKey.stableDebugName = "DiagnosticsGraphics";
    auto graphicsPipeline = pipelineCache->GetOrCreateGraphicsPipeline(
        graphicsKey,
        []()
        {
            return std::make_shared<TestGraphicsPipeline>("DiagnosticsGraphics");
        });
    ASSERT_NE(graphicsPipeline, nullptr);
    auto graphicsPipelineHit = pipelineCache->GetOrCreateGraphicsPipeline(
        graphicsKey,
        []()
        {
            return std::make_shared<TestGraphicsPipeline>("DiagnosticsGraphicsUnexpectedRebuild");
        });
    ASSERT_EQ(graphicsPipelineHit, graphicsPipeline);

    NLS::Render::RHI::PipelineCacheKey computeKey;
    computeKey.hash = 0x5678u;
    computeKey.backend = NLS::Render::RHI::NativeBackendType::DX12;
    computeKey.stableDebugName = "DiagnosticsCompute";
    auto computePipeline = pipelineCache->GetOrCreateComputePipeline(
        computeKey,
        []()
        {
            return std::make_shared<TestComputePipeline>("DiagnosticsCompute");
        });
    ASSERT_NE(computePipeline, nullptr);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    auto frameFence = std::make_shared<TestFence>();
    auto descriptorAllocator = std::make_shared<TestDescriptorAllocator>(8u);
    auto uploadContext = std::make_shared<TestUploadContext>();
    commandPool->commandBuffer = commandBuffer;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = frameFence;
    frameContext.resourceStateTracker = NLS::Render::RHI::CreateDefaultResourceStateTracker();
    frameContext.descriptorAllocator = descriptorAllocator;
    frameContext.uploadContext = uploadContext;

    auto transientBuffer = std::make_shared<TestBuffer>(128u);
    frameContext.resourceStateTracker->RegisterTransientBuffer(transientBuffer, 0u);

    NLS::Render::RHI::RHITextureDesc transientTextureDesc;
    transientTextureDesc.debugName = "TransientTexture";
    transientTextureDesc.extent.width = 64u;
    transientTextureDesc.extent.height = 64u;
    transientTextureDesc.extent.depth = 1u;
    transientTextureDesc.arrayLayers = 1u;
    transientTextureDesc.mipLevels = 1u;
    auto transientTexture = std::make_shared<TestTexture>(transientTextureDesc);

    NLS::Render::RHI::RHISubresourceRange fullRange;
    fullRange.baseMipLevel = 0u;
    fullRange.mipLevelCount = 1u;
    fullRange.baseArrayLayer = 0u;
    fullRange.arrayLayerCount = 1u;
    frameContext.resourceStateTracker->RegisterTransientTexture(transientTexture, fullRange, 0u);

    NLS::Render::RHI::DescriptorAllocationRequest transientRequest;
    transientRequest.count = 6u;
    transientRequest.lifetime = NLS::Render::RHI::DescriptorAllocationLifetime::TransientFrame;
    transientRequest.debugName = "FrameBindings";
    const auto transientAllocation = descriptorAllocator->Allocate(transientRequest);
    ASSERT_TRUE(transientAllocation.IsValid());

    NLS::Render::RHI::DescriptorAllocationRequest overflowRequest = transientRequest;
    overflowRequest.count = 4u;
    overflowRequest.debugName = "OverflowBindings";
    const auto overflowAllocation = descriptorAllocator->Allocate(overflowRequest);
    EXPECT_FALSE(overflowAllocation.IsValid());

    NLS::Render::RHI::DescriptorAllocationRequest persistentRequest;
    persistentRequest.count = 4u;
    persistentRequest.lifetime = NLS::Render::RHI::DescriptorAllocationLifetime::Persistent;
    persistentRequest.debugName = "PersistentBindings";
    const auto persistentAllocation = descriptorAllocator->Allocate(persistentRequest);
    ASSERT_TRUE(persistentAllocation.IsValid());
    descriptorAllocator->Release(persistentAllocation);

    auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 407u;
    snapshot.targetsSwapchain = false;
    snapshot.visibleOpaqueDrawCount = 1u;
    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessFrameSnapshot(driver, snapshot));

    size_t slotIndex = 0u;
    NLS::Render::Context::FrameSnapshot publishedSnapshot;
    ASSERT_TRUE(lifecycle->TryBeginNextRenderScene(&slotIndex, &publishedSnapshot));

    auto opaquePipeline = std::make_shared<TestGraphicsPipeline>("OpaquePipeline");
    auto materialBindingSet = std::make_shared<TestBindingSet>("MaterialBindingSet");
    auto mesh = std::make_shared<TestMesh>();

    NLS::Render::Context::RenderPassCommandInput opaquePassInput;
    opaquePassInput.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    opaquePassInput.drawCount = 1u;
    opaquePassInput.requiresFrameData = true;
    opaquePassInput.requiresObjectData = true;
    opaquePassInput.targetsSwapchain = false;
    opaquePassInput.renderWidth = 64u;
    opaquePassInput.renderHeight = 64u;
    opaquePassInput.clearColor = true;
    opaquePassInput.clearDepth = true;
    opaquePassInput.usesColorAttachment = true;
    opaquePassInput.usesDepthStencilAttachment = true;
    opaquePassInput.recordedDrawCommands.push_back({ opaquePipeline, nullptr, nullptr, nullptr, materialBindingSet, mesh, 1u });

    NLS::Render::Context::RenderScenePackage package;
    package.frameId = publishedSnapshot.frameId;
    package.targetsSwapchain = false;
    package.visibleDrawCount = 1u;
    package.opaqueDrawCount = 1u;
    package.hasVisibleDraws = true;
    package.frameDataReady = true;
    package.objectDataReady = true;
    package.renderWidth = 64u;
    package.renderHeight = 64u;
    package.passCommandInputs = { opaquePassInput };

    ASSERT_TRUE(lifecycle->CompleteRenderScene(slotIndex, package));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    const auto copiedSlot = lifecycle->CopySlot(0u);
    ASSERT_TRUE(copiedSlot.has_value());
    ASSERT_TRUE(copiedSlot->submissionFrame.has_value());
    EXPECT_TRUE(copiedSlot->submissionFrame->usedResourceStateTracker);
    EXPECT_TRUE(copiedSlot->submissionFrame->usedDescriptorAllocator);
    EXPECT_EQ(copiedSlot->submissionFrame->retiredTransientBufferCount, 1u);
    EXPECT_EQ(copiedSlot->submissionFrame->retiredTransientTextureCount, 1u);
    EXPECT_EQ(copiedSlot->submissionFrame->descriptorTransientUsed, 6u);
    EXPECT_EQ(copiedSlot->submissionFrame->descriptorTransientPeak, 6u);
    EXPECT_EQ(copiedSlot->submissionFrame->descriptorPersistentUsed, 0u);
    EXPECT_EQ(copiedSlot->submissionFrame->descriptorPersistentReleased, 4u);
    EXPECT_EQ(copiedSlot->submissionFrame->descriptorAllocationFailures, 1u);
    EXPECT_EQ(descriptorAllocator->beginFrameCalls, 1u);
    EXPECT_EQ(descriptorAllocator->endFrameCalls, 1u);
    EXPECT_EQ(uploadContext->beginFrameCalls, 1u);
    EXPECT_EQ(uploadContext->endFrameCalls, 1u);
    EXPECT_EQ(uploadContext->lastBeginFrameIndex, 0u);
    EXPECT_EQ(uploadContext->lastEndFrameIndex, 0u);

    const auto telemetry = NLS::Render::Context::DriverRendererAccess::GetThreadedFrameTelemetry(driver);
    EXPECT_TRUE(telemetry.descriptorMainlineActive);
    EXPECT_TRUE(telemetry.pipelineMainlineActive);
    EXPECT_TRUE(telemetry.transientLifetimeMainlineActive);
    EXPECT_TRUE(telemetry.retirementMainlineActive);
    EXPECT_EQ(telemetry.descriptorBypassCount, 0u);
    EXPECT_EQ(telemetry.pipelineBypassCount, 0u);
    EXPECT_EQ(telemetry.transientLifetimeBypassCount, 0u);
    EXPECT_EQ(telemetry.retirementBypassCount, 0u);
    EXPECT_EQ(
        telemetry.transientBufferRegistrationCount,
        copiedSlot->submissionFrame->transientBufferRegistrationCount);
    EXPECT_EQ(
        telemetry.transientTextureRegistrationCount,
        copiedSlot->submissionFrame->transientTextureRegistrationCount);
    EXPECT_EQ(
        telemetry.retiredTransientBufferCount,
        copiedSlot->submissionFrame->retiredTransientBufferCount);
    EXPECT_EQ(
        telemetry.retiredTransientTextureCount,
        copiedSlot->submissionFrame->retiredTransientTextureCount);
    EXPECT_EQ(telemetry.descriptorTransientPeak, 6u);
    EXPECT_EQ(telemetry.descriptorAllocationFailures, 1u);
    EXPECT_EQ(telemetry.pipelineCacheGraphicsHits, 1u);
    EXPECT_EQ(telemetry.pipelineCacheGraphicsMisses, 1u);
    EXPECT_EQ(telemetry.pipelineCacheGraphicsStores, 1u);
    EXPECT_EQ(telemetry.pipelineCacheGraphicsEntries, 1u);
    EXPECT_EQ(telemetry.pipelineCacheComputeHits, 0u);
    EXPECT_EQ(telemetry.pipelineCacheComputeMisses, 1u);
    EXPECT_EQ(telemetry.pipelineCacheComputeStores, 1u);
    EXPECT_EQ(telemetry.pipelineCacheComputeEntries, 1u);
}

TEST(ThreadedRenderingLifecycleTests, CreateExplicitBindingSetTracksTransientAndPersistentDescriptorLifetime)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto descriptorAllocator = std::make_shared<TestDescriptorAllocator>(16u);
    frameContext.frameIndex = 11u;
    frameContext.descriptorAllocator = descriptorAllocator;
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, true);

    NLS::Render::RHI::RHIBindingSetDesc transientDesc;
    transientDesc.debugName = "TransientBindings";
    transientDesc.entries.resize(2u);
    transientDesc.entries[0].binding = 0u;
    transientDesc.entries[0].type = NLS::Render::RHI::BindingType::UniformBuffer;
    transientDesc.entries[1].binding = 1u;
    transientDesc.entries[1].type = NLS::Render::RHI::BindingType::Texture;

    auto transientBindingSet = NLS::Render::Context::DriverRendererAccess::CreateExplicitBindingSet(
        driver,
        transientDesc,
        NLS::Render::RHI::DescriptorAllocationLifetime::TransientFrame);
    ASSERT_NE(transientBindingSet, nullptr);

    auto descriptorStats = descriptorAllocator->GetStats();
    EXPECT_EQ(descriptorStats.transientUsed, 2u);
    EXPECT_EQ(descriptorStats.persistentUsed, 0u);
    EXPECT_EQ(descriptorStats.persistentReleased, 0u);

    NLS::Render::RHI::RHIBindingSetDesc persistentDesc;
    persistentDesc.debugName = "PersistentBindings";
    persistentDesc.entries.resize(3u);
    persistentDesc.entries[0].binding = 0u;
    persistentDesc.entries[0].type = NLS::Render::RHI::BindingType::UniformBuffer;
    persistentDesc.entries[1].binding = 1u;
    persistentDesc.entries[1].type = NLS::Render::RHI::BindingType::Texture;
    persistentDesc.entries[2].binding = 2u;
    persistentDesc.entries[2].type = NLS::Render::RHI::BindingType::Sampler;

    auto persistentBindingSet = NLS::Render::Context::DriverRendererAccess::CreateExplicitBindingSet(
        driver,
        persistentDesc,
        NLS::Render::RHI::DescriptorAllocationLifetime::Persistent);
    ASSERT_NE(persistentBindingSet, nullptr);

    descriptorStats = descriptorAllocator->GetStats();
    EXPECT_EQ(descriptorStats.transientUsed, 2u);
    EXPECT_EQ(descriptorStats.persistentUsed, 3u);
    EXPECT_EQ(descriptorStats.persistentReleased, 0u);

    transientBindingSet.reset();
    descriptorStats = descriptorAllocator->GetStats();
    EXPECT_EQ(descriptorStats.transientUsed, 2u);
    EXPECT_EQ(descriptorStats.persistentUsed, 3u);
    EXPECT_EQ(descriptorStats.persistentReleased, 0u);

    persistentBindingSet.reset();
    descriptorStats = descriptorAllocator->GetStats();
    EXPECT_EQ(descriptorStats.transientUsed, 2u);
    EXPECT_EQ(descriptorStats.persistentUsed, 0u);
    EXPECT_EQ(descriptorStats.persistentReleased, 3u);

    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, false);
}

TEST(ThreadedRenderingLifecycleTests, CreateExplicitBindingSetExposesSharedWrappedBindingSetForRhiThreadOwnership)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 17u;
    frameContext.descriptorAllocator = std::make_shared<TestDescriptorAllocator>(16u);
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, true);

    NLS::Render::RHI::RHIBindingSetDesc desc;
    desc.debugName = "RhiThreadOwnedBindings";
    desc.entries.resize(1u);
    desc.entries[0].binding = 0u;
    desc.entries[0].type = NLS::Render::RHI::BindingType::UniformBuffer;

    auto trackedBindingSet = NLS::Render::Context::DriverRendererAccess::CreateExplicitBindingSet(
        driver,
        desc,
        NLS::Render::RHI::DescriptorAllocationLifetime::Persistent);
    ASSERT_NE(trackedBindingSet, nullptr);

    auto wrappedBindingSet = trackedBindingSet->GetWrappedBindingSetShared();
    ASSERT_NE(wrappedBindingSet, nullptr);
    EXPECT_EQ(wrappedBindingSet->GetDebugName(), "RhiThreadOwnedBindings");

    std::weak_ptr<NLS::Render::RHI::RHIBindingSet> weakWrappedBindingSet = wrappedBindingSet;
    trackedBindingSet.reset();
    ASSERT_FALSE(weakWrappedBindingSet.expired());
    EXPECT_EQ(wrappedBindingSet->GetDebugName(), "RhiThreadOwnedBindings");

    wrappedBindingSet.reset();
    EXPECT_TRUE(weakWrappedBindingSet.expired());

    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, false);
}

TEST(ThreadedRenderingLifecycleTests, PipelineCacheUsesBackendAwareStableKeysAndTracksPrewarmStats)
{
    auto pipelineCache = NLS::Render::RHI::CreateDefaultPipelineCache();
    ASSERT_NE(pipelineCache, nullptr);

    NLS::Render::RHI::PipelineCacheKey dx12GraphicsKey;
    dx12GraphicsKey.hash = 0x1001u;
    dx12GraphicsKey.backend = NLS::Render::RHI::NativeBackendType::DX12;
    dx12GraphicsKey.stableDebugName = "LightingPSO";

    NLS::Render::RHI::PipelineCacheKey vulkanGraphicsKey = dx12GraphicsKey;
    vulkanGraphicsKey.backend = NLS::Render::RHI::NativeBackendType::Vulkan;

    NLS::Render::RHI::PipelineCacheKey renamedGraphicsKey = dx12GraphicsKey;
    renamedGraphicsKey.stableDebugName = "LightingPSOVariant";

    int dx12CreateCalls = 0;
    auto dx12Pipeline = pipelineCache->GetOrCreateGraphicsPipeline(
        dx12GraphicsKey,
        [&]() -> std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline>
        {
            ++dx12CreateCalls;
            return std::make_shared<TestGraphicsPipeline>("DX12LightingPSO");
        });
    ASSERT_NE(dx12Pipeline, nullptr);

    auto dx12PipelineHit = pipelineCache->GetOrCreateGraphicsPipeline(
        dx12GraphicsKey,
        [&]() -> std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline>
        {
            ++dx12CreateCalls;
            return std::make_shared<TestGraphicsPipeline>("DX12LightingPSOUnexpectedRebuild");
        });
    EXPECT_EQ(dx12PipelineHit, dx12Pipeline);
    EXPECT_EQ(dx12CreateCalls, 1);

    int vulkanCreateCalls = 0;
    auto vulkanPipeline = pipelineCache->GetOrCreateGraphicsPipeline(
        vulkanGraphicsKey,
        [&]() -> std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline>
        {
            ++vulkanCreateCalls;
            return std::make_shared<TestGraphicsPipeline>("VulkanLightingPSO");
        });
    ASSERT_NE(vulkanPipeline, nullptr);
    EXPECT_NE(vulkanPipeline, dx12Pipeline);
    EXPECT_EQ(vulkanCreateCalls, 1);

    int renamedCreateCalls = 0;
    auto renamedPipeline = pipelineCache->GetOrCreateGraphicsPipeline(
        renamedGraphicsKey,
        [&]() -> std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline>
        {
            ++renamedCreateCalls;
            return std::make_shared<TestGraphicsPipeline>("DX12LightingPSOVariant");
        });
    ASSERT_NE(renamedPipeline, nullptr);
    EXPECT_NE(renamedPipeline, dx12Pipeline);
    EXPECT_EQ(renamedCreateCalls, 1);

    NLS::Render::RHI::PipelineCacheKey computeKey;
    computeKey.hash = 0x2002u;
    computeKey.backend = NLS::Render::RHI::NativeBackendType::DX12;
    computeKey.stableDebugName = "LightGridInjectionCS";

    int computeCreateCalls = 0;
    auto computePipeline = pipelineCache->GetOrCreateComputePipeline(
        computeKey,
        [&]() -> std::shared_ptr<NLS::Render::RHI::RHIComputePipeline>
        {
            ++computeCreateCalls;
            return std::make_shared<TestComputePipeline>("LightGridInjectionCS");
        },
        NLS::Render::RHI::PipelineCacheRequestMode::Prewarm);
    ASSERT_NE(computePipeline, nullptr);

    auto computePipelineHit = pipelineCache->GetOrCreateComputePipeline(
        computeKey,
        [&]() -> std::shared_ptr<NLS::Render::RHI::RHIComputePipeline>
        {
            ++computeCreateCalls;
            return std::make_shared<TestComputePipeline>("LightGridInjectionCSUnexpectedRebuild");
        },
        NLS::Render::RHI::PipelineCacheRequestMode::Prewarm);
    EXPECT_EQ(computePipelineHit, computePipeline);
    EXPECT_EQ(computeCreateCalls, 1);

    const auto cacheStats = pipelineCache->GetStats();
    EXPECT_EQ(cacheStats.graphicsEntryCount, 3u);
    EXPECT_EQ(cacheStats.graphicsStores, 3u);
    EXPECT_EQ(cacheStats.graphicsHits, 1u);
    EXPECT_EQ(cacheStats.graphicsMisses, 3u);
    EXPECT_EQ(cacheStats.computeEntryCount, 1u);
    EXPECT_EQ(cacheStats.computeStores, 1u);
    EXPECT_EQ(cacheStats.computeHits, 1u);
    EXPECT_EQ(cacheStats.computeMisses, 1u);
    EXPECT_EQ(cacheStats.computePrewarmRequests, 2u);
    EXPECT_EQ(cacheStats.computePrewarmHits, 1u);
}

TEST(ThreadedRenderingLifecycleTests, AsyncComputeDiagnosticsReportReadyButNotScheduledWhenQueueAndCapabilityExist)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    explicitDevice->MutableCapabilities().supportsAsyncCompute = true;
    explicitDevice->MutableCapabilities().supportsDedicatedComputeQueue = true;
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    auto frameFence = std::make_shared<TestFence>();
    commandPool->commandBuffer = commandBuffer;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = frameFence;
    frameContext.resourceStateTracker = NLS::Render::RHI::CreateDefaultResourceStateTracker();

    auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 408u;
    snapshot.targetsSwapchain = false;
    snapshot.visibleOpaqueDrawCount = 1u;
    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessFrameSnapshot(driver, snapshot));

    size_t slotIndex = 0u;
    NLS::Render::Context::FrameSnapshot publishedSnapshot;
    ASSERT_TRUE(lifecycle->TryBeginNextRenderScene(&slotIndex, &publishedSnapshot));

    auto opaquePipeline = std::make_shared<TestGraphicsPipeline>("OpaquePipeline");
    auto materialBindingSet = std::make_shared<TestBindingSet>("MaterialBindingSet");
    auto mesh = std::make_shared<TestMesh>();

    NLS::Render::Context::RenderPassCommandInput opaquePassInput;
    opaquePassInput.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    opaquePassInput.drawCount = 1u;
    opaquePassInput.requiresFrameData = true;
    opaquePassInput.requiresObjectData = true;
    opaquePassInput.targetsSwapchain = false;
    opaquePassInput.renderWidth = 64u;
    opaquePassInput.renderHeight = 64u;
    opaquePassInput.clearColor = true;
    opaquePassInput.clearDepth = true;
    opaquePassInput.usesColorAttachment = true;
    opaquePassInput.usesDepthStencilAttachment = true;
    opaquePassInput.recordedDrawCommands.push_back({ opaquePipeline, nullptr, nullptr, nullptr, materialBindingSet, mesh, 1u });

    NLS::Render::Context::RenderScenePackage package;
    package.frameId = publishedSnapshot.frameId;
    package.targetsSwapchain = false;
    package.visibleDrawCount = 1u;
    package.opaqueDrawCount = 1u;
    package.hasVisibleDraws = true;
    package.frameDataReady = true;
    package.objectDataReady = true;
    package.renderWidth = 64u;
    package.renderHeight = 64u;
    package.hasAsyncComputeWorkload = true;
    package.asyncComputeWorkloadCount = 1u;
    package.passCommandInputs = { opaquePassInput };

    ASSERT_TRUE(lifecycle->CompleteRenderScene(slotIndex, package));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    const auto copiedSlot = lifecycle->CopySlot(0u);
    ASSERT_TRUE(copiedSlot.has_value());
    ASSERT_TRUE(copiedSlot->submissionFrame.has_value());
    EXPECT_EQ(
        copiedSlot->submissionFrame->asyncComputeDisposition,
        NLS::Render::Context::AsyncComputeDisposition::ReadyButNotScheduled);
    EXPECT_EQ(copiedSlot->submissionFrame->asyncComputeCandidateWorkloadCount, 1u);
    EXPECT_TRUE(copiedSlot->submissionFrame->asyncComputeQueueAvailable);
    EXPECT_FALSE(copiedSlot->submissionFrame->usedAsyncComputeQueueSubmission);
}

TEST(ThreadedRenderingLifecycleTests, AsyncComputeDiagnosticsReportDisabledWhenDedicatedQueueIsUnavailable)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    explicitDevice->MutableCapabilities().supportsAsyncCompute = true;
    explicitDevice->MutableCapabilities().supportsDedicatedComputeQueue = true;
    explicitDevice->SetComputeQueue(nullptr);
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    auto frameFence = std::make_shared<TestFence>();
    commandPool->commandBuffer = commandBuffer;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = frameFence;
    frameContext.resourceStateTracker = NLS::Render::RHI::CreateDefaultResourceStateTracker();

    auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 409u;
    snapshot.targetsSwapchain = false;
    snapshot.visibleOpaqueDrawCount = 1u;
    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessFrameSnapshot(driver, snapshot));

    size_t slotIndex = 0u;
    NLS::Render::Context::FrameSnapshot publishedSnapshot;
    ASSERT_TRUE(lifecycle->TryBeginNextRenderScene(&slotIndex, &publishedSnapshot));

    auto opaquePipeline = std::make_shared<TestGraphicsPipeline>("OpaquePipeline");
    auto materialBindingSet = std::make_shared<TestBindingSet>("MaterialBindingSet");
    auto mesh = std::make_shared<TestMesh>();

    NLS::Render::Context::RenderPassCommandInput opaquePassInput;
    opaquePassInput.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    opaquePassInput.drawCount = 1u;
    opaquePassInput.requiresFrameData = true;
    opaquePassInput.requiresObjectData = true;
    opaquePassInput.targetsSwapchain = false;
    opaquePassInput.renderWidth = 64u;
    opaquePassInput.renderHeight = 64u;
    opaquePassInput.clearColor = true;
    opaquePassInput.clearDepth = true;
    opaquePassInput.usesColorAttachment = true;
    opaquePassInput.usesDepthStencilAttachment = true;
    opaquePassInput.recordedDrawCommands.push_back({ opaquePipeline, nullptr, nullptr, nullptr, materialBindingSet, mesh, 1u });

    NLS::Render::Context::RenderScenePackage package;
    package.frameId = publishedSnapshot.frameId;
    package.targetsSwapchain = false;
    package.visibleDrawCount = 1u;
    package.opaqueDrawCount = 1u;
    package.hasVisibleDraws = true;
    package.frameDataReady = true;
    package.objectDataReady = true;
    package.renderWidth = 64u;
    package.renderHeight = 64u;
    package.hasAsyncComputeWorkload = true;
    package.asyncComputeWorkloadCount = 1u;
    package.passCommandInputs = { opaquePassInput };

    ASSERT_TRUE(lifecycle->CompleteRenderScene(slotIndex, package));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    const auto copiedSlot = lifecycle->CopySlot(0u);
    ASSERT_TRUE(copiedSlot.has_value());
    ASSERT_TRUE(copiedSlot->submissionFrame.has_value());
    EXPECT_EQ(
        copiedSlot->submissionFrame->asyncComputeDisposition,
        NLS::Render::Context::AsyncComputeDisposition::DisabledNoComputeQueue);
    EXPECT_EQ(copiedSlot->submissionFrame->asyncComputeCandidateWorkloadCount, 1u);
    EXPECT_FALSE(copiedSlot->submissionFrame->asyncComputeQueueAvailable);
    EXPECT_FALSE(copiedSlot->submissionFrame->usedAsyncComputeQueueSubmission);
}

TEST(ThreadedRenderingLifecycleTests, TranslationMergeInsertsBarrierBatchForDeferredWorkUnits)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    explicitDevice->MutableCapabilities().supportsParallelCommandTranslation = true;
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    auto frameFence = std::make_shared<TestFence>();
    commandPool->commandBuffer = commandBuffer;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = frameFence;
    frameContext.resourceStateTracker = NLS::Render::RHI::CreateDefaultResourceStateTracker();

    auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 406u;
    snapshot.targetsSwapchain = false;
    snapshot.visibleOpaqueDrawCount = 1u;
    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessFrameSnapshot(driver, snapshot));

    size_t slotIndex = 0u;
    NLS::Render::Context::FrameSnapshot publishedSnapshot;
    ASSERT_TRUE(lifecycle->TryBeginNextRenderScene(&slotIndex, &publishedSnapshot));

    auto opaquePipeline = std::make_shared<TestGraphicsPipeline>("DeferredGBufferPipeline");
    auto materialBindingSet = std::make_shared<TestBindingSet>("MaterialBindingSet");
    auto mesh = std::make_shared<TestMesh>();

    NLS::Render::RHI::RHITextureDesc gbufferTextureDesc;
    gbufferTextureDesc.debugName = "GBufferAlbedo";
    gbufferTextureDesc.extent.width = 64u;
    gbufferTextureDesc.extent.height = 64u;
    gbufferTextureDesc.extent.depth = 1u;
    gbufferTextureDesc.arrayLayers = 1u;
    gbufferTextureDesc.mipLevels = 1u;
    auto gbufferTexture = std::make_shared<TestTexture>(gbufferTextureDesc);
    auto gbufferColorView = std::make_shared<TestTextureView>(
        gbufferTexture,
        NLS::Render::RHI::RHITextureViewDesc{});
    auto gbufferDepthTexture = std::make_shared<TestTexture>(gbufferTextureDesc);
    auto gbufferDepthView = std::make_shared<TestTextureView>(
        gbufferDepthTexture,
        NLS::Render::RHI::RHITextureViewDesc{});
    NLS::Render::RHI::RHISubresourceRange gbufferFullRange;
    gbufferFullRange.baseMipLevel = 0u;
    gbufferFullRange.mipLevelCount = gbufferTexture->GetDesc().mipLevels;
    gbufferFullRange.baseArrayLayer = 0u;
    gbufferFullRange.arrayLayerCount = gbufferTexture->GetDesc().arrayLayers;

    NLS::Render::Context::RenderPassCommandInput gbufferPassInput;
    gbufferPassInput.kind = NLS::Render::Context::RenderPassCommandKind::GBuffer;
    gbufferPassInput.drawCount = 1u;
    gbufferPassInput.requiresFrameData = true;
    gbufferPassInput.requiresObjectData = true;
    gbufferPassInput.targetsSwapchain = false;
    gbufferPassInput.renderWidth = 64u;
    gbufferPassInput.renderHeight = 64u;
    gbufferPassInput.clearColor = true;
    gbufferPassInput.clearDepth = true;
    gbufferPassInput.usesColorAttachment = true;
    gbufferPassInput.usesDepthStencilAttachment = true;
    gbufferPassInput.colorAttachmentViews.push_back(gbufferColorView);
    gbufferPassInput.depthStencilAttachmentView = gbufferDepthView;
    gbufferPassInput.gbufferTextures.push_back(gbufferTexture);
    gbufferPassInput.textureResourceAccesses.push_back({
        gbufferTexture,
        gbufferFullRange,
        NLS::Render::Context::ResourceAccessMode::Write,
        NLS::Render::RHI::ResourceState::RenderTarget,
        NLS::Render::RHI::PipelineStageMask::AllCommands,
        NLS::Render::RHI::AccessMask::MemoryRead | NLS::Render::RHI::AccessMask::MemoryWrite
    });
    gbufferPassInput.recordedDrawCommands.push_back({ opaquePipeline, nullptr, nullptr, nullptr, materialBindingSet, mesh, 1u });

    NLS::Render::Context::RenderPassCommandInput lightingPassInput;
    lightingPassInput.kind = NLS::Render::Context::RenderPassCommandKind::Lighting;
    lightingPassInput.drawCount = 1u;
    lightingPassInput.requiresFrameData = true;
    lightingPassInput.targetsSwapchain = false;
    lightingPassInput.renderWidth = 64u;
    lightingPassInput.renderHeight = 64u;
    lightingPassInput.queueDependencyPolicy = NLS::Render::Context::QueueDependencyPolicy::Previous;
    lightingPassInput.dependencySourceWorkUnitIndex = 0u;
    lightingPassInput.requiresDependencyVisibility = true;
    lightingPassInput.textureResourceAccesses.push_back({
        gbufferTexture,
        gbufferFullRange,
        NLS::Render::Context::ResourceAccessMode::Read,
        NLS::Render::RHI::ResourceState::ShaderRead,
        NLS::Render::RHI::PipelineStageMask::FragmentShader | NLS::Render::RHI::PipelineStageMask::ComputeShader,
        NLS::Render::RHI::AccessMask::ShaderRead
    });

    NLS::Render::Context::ParallelCommandWorkUnit gbufferWorkUnit;
    gbufferWorkUnit.commandInput = gbufferPassInput;
    gbufferWorkUnit.debugName = "DeferredGBuffer";
    gbufferWorkUnit.eligibleForParallelTranslation = true;

    NLS::Render::Context::ParallelCommandWorkUnit lightingWorkUnit;
    lightingWorkUnit.commandInput = lightingPassInput;
    lightingWorkUnit.debugName = "DeferredLighting";
    lightingWorkUnit.submissionOrder = 1u;
    lightingWorkUnit.eligibleForParallelTranslation = true;

    NLS::Render::Context::RenderScenePackage package;
    package.frameId = publishedSnapshot.frameId;
    package.targetsSwapchain = false;
    package.visibleDrawCount = 2u;
    package.opaqueDrawCount = 1u;
    package.hasVisibleDraws = true;
    package.frameDataReady = true;
    package.objectDataReady = true;
    package.renderWidth = 64u;
    package.renderHeight = 64u;
    package.containsParallelCommandWorkUnits = true;
    package.parallelCommandWorkUnitCount = 2u;
    package.parallelCommandWorkUnits = { gbufferWorkUnit, lightingWorkUnit };

    ASSERT_TRUE(lifecycle->CompleteRenderScene(slotIndex, package));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    const auto copiedSlot = lifecycle->CopySlot(0u);
    ASSERT_TRUE(copiedSlot.has_value());
    ASSERT_TRUE(copiedSlot->submissionFrame.has_value());
    EXPECT_TRUE(copiedSlot->submissionFrame->recordedVisibleWork);
    EXPECT_EQ(copiedSlot->submissionFrame->recordedPassCount, 2u);
    EXPECT_EQ(copiedSlot->submissionFrame->recordedWorkUnitCount, 2u);
    EXPECT_EQ(copiedSlot->submissionFrame->parallelRecordingWorkerCount, 0u);
    EXPECT_EQ(copiedSlot->submissionFrame->translatedWorkUnitCount, 2u);
    EXPECT_TRUE(copiedSlot->submissionFrame->usedParallelCommandPath);
    EXPECT_TRUE(copiedSlot->submissionFrame->usedTranslationMerge);
    EXPECT_FALSE(copiedSlot->submissionFrame->usedSerialCommandPath);
    ASSERT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    EXPECT_EQ(explicitDevice->GetTestQueue()->lastSubmitDesc.commandBuffers.size(), 3u);
    auto firstSubmittedBuffer = GetSubmittedTestCommandBuffer(explicitDevice->GetTestQueue(), 0u);
    auto secondSubmittedBuffer = GetSubmittedTestCommandBuffer(explicitDevice->GetTestQueue(), 1u);
    auto thirdSubmittedBuffer = GetSubmittedTestCommandBuffer(explicitDevice->GetTestQueue(), 2u);
    ASSERT_NE(firstSubmittedBuffer, nullptr);
    ASSERT_NE(secondSubmittedBuffer, nullptr);
    ASSERT_NE(thirdSubmittedBuffer, nullptr);
    EXPECT_EQ(firstSubmittedBuffer->beginRenderPassCalls, 1u);
    EXPECT_GT(secondSubmittedBuffer->barrierCalls, 0u);
    EXPECT_EQ(thirdSubmittedBuffer->beginRenderPassCalls, 1u);

    uint32_t barrierBatchCount = 0u;
    for (const auto& submittedBufferBase : explicitDevice->GetTestQueue()->lastSubmitDesc.commandBuffers)
    {
        auto submittedBuffer = std::dynamic_pointer_cast<TestCommandBuffer>(submittedBufferBase);
        if (submittedBuffer != nullptr && submittedBuffer->barrierCalls > 0u)
            ++barrierBatchCount;
    }
    EXPECT_GE(barrierBatchCount, 1u);
}

TEST(ThreadedRenderingLifecycleTests, TranslationMergeInsertsBarrierBatchForComputeVisibilityRequests)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    explicitDevice->MutableCapabilities().supportsParallelCommandTranslation = true;
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    auto frameFence = std::make_shared<TestFence>();
    commandPool->commandBuffer = commandBuffer;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = frameFence;
    frameContext.resourceStateTracker = NLS::Render::RHI::CreateDefaultResourceStateTracker();

    auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 407u;
    snapshot.targetsSwapchain = false;
    snapshot.visibleOpaqueDrawCount = 1u;
    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessFrameSnapshot(driver, snapshot));

    size_t slotIndex = 0u;
    NLS::Render::Context::FrameSnapshot publishedSnapshot;
    ASSERT_TRUE(lifecycle->TryBeginNextRenderScene(&slotIndex, &publishedSnapshot));

    auto graphicsPipeline = std::make_shared<TestGraphicsPipeline>("OpaquePipeline");
    auto computePipeline = std::make_shared<TestComputePipeline>("VisibilityComputePipeline");
    auto materialBindingSet = std::make_shared<TestBindingSet>("MaterialBindingSet");
    auto mesh = std::make_shared<TestMesh>();
    auto visibilityBuffer = std::make_shared<TestBuffer>(NLS::Render::RHI::RHIBufferDesc{});

    NLS::Render::Context::RenderPassCommandInput computePassInput;
    computePassInput.kind = NLS::Render::Context::RenderPassCommandKind::Compute;
    computePassInput.debugName = "VisibilityCompute";
    computePassInput.queueType = NLS::Render::RHI::QueueType::Compute;
    computePassInput.renderWidth = 64u;
    computePassInput.renderHeight = 64u;
    computePassInput.bufferResourceAccesses.push_back({
        visibilityBuffer,
        NLS::Render::Context::ResourceAccessMode::Write,
        NLS::Render::RHI::ResourceState::ShaderWrite,
        NLS::Render::RHI::PipelineStageMask::ComputeShader,
        NLS::Render::RHI::AccessMask::ShaderWrite
    });
    computePassInput.computeDispatchInputs.push_back({
        "VisibilityCompute",
        computePipeline,
        {},
        1u,
        1u,
        1u,
        {},
        {},
        { visibilityBuffer }
    });

    NLS::Render::Context::RenderPassCommandInput opaquePassInput;
    opaquePassInput.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    opaquePassInput.drawCount = 1u;
    opaquePassInput.requiresFrameData = true;
    opaquePassInput.requiresObjectData = true;
    opaquePassInput.targetsSwapchain = false;
    opaquePassInput.renderWidth = 64u;
    opaquePassInput.renderHeight = 64u;
    opaquePassInput.clearColor = true;
    opaquePassInput.clearDepth = true;
    opaquePassInput.usesColorAttachment = true;
    opaquePassInput.usesDepthStencilAttachment = true;
    opaquePassInput.dependencySourceWorkUnitIndex = 0u;
    opaquePassInput.requiresDependencyVisibility = true;
    opaquePassInput.bufferResourceAccesses.push_back({
        visibilityBuffer,
        NLS::Render::Context::ResourceAccessMode::Read,
        NLS::Render::RHI::ResourceState::ShaderRead,
        NLS::Render::RHI::PipelineStageMask::FragmentShader,
        NLS::Render::RHI::AccessMask::ShaderRead
    });
    opaquePassInput.recordedDrawCommands.push_back({ graphicsPipeline, nullptr, nullptr, nullptr, materialBindingSet, mesh, 1u });

    NLS::Render::Context::ParallelCommandWorkUnit computeWorkUnit;
    computeWorkUnit.commandInput = computePassInput;
    computeWorkUnit.debugName = "VisibilityCompute";
    computeWorkUnit.eligibleForParallelTranslation = true;

    NLS::Render::Context::ParallelCommandWorkUnit opaqueWorkUnit;
    opaqueWorkUnit.commandInput = opaquePassInput;
    opaqueWorkUnit.debugName = "ForwardOpaque";
    opaqueWorkUnit.submissionOrder = 1u;
    opaqueWorkUnit.eligibleForParallelTranslation = true;

    NLS::Render::Context::RenderScenePackage package;
    package.frameId = publishedSnapshot.frameId;
    package.targetsSwapchain = false;
    package.visibleDrawCount = 1u;
    package.opaqueDrawCount = 1u;
    package.hasVisibleDraws = true;
    package.frameDataReady = true;
    package.objectDataReady = true;
    package.renderWidth = 64u;
    package.renderHeight = 64u;
    package.containsParallelCommandWorkUnits = true;
    package.parallelCommandWorkUnitCount = 2u;
    package.parallelCommandWorkUnits = { computeWorkUnit, opaqueWorkUnit };

    ASSERT_TRUE(lifecycle->CompleteRenderScene(slotIndex, package));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    const auto copiedSlot = lifecycle->CopySlot(0u);
    ASSERT_TRUE(copiedSlot.has_value());
    ASSERT_TRUE(copiedSlot->submissionFrame.has_value());
    EXPECT_TRUE(copiedSlot->submissionFrame->usedTranslationMerge);
    ASSERT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    EXPECT_EQ(explicitDevice->GetTestQueue()->lastSubmitDesc.commandBuffers.size(), 3u);
    auto firstSubmittedBuffer = GetSubmittedTestCommandBuffer(explicitDevice->GetTestQueue(), 0u);
    auto secondSubmittedBuffer = GetSubmittedTestCommandBuffer(explicitDevice->GetTestQueue(), 1u);
    auto thirdSubmittedBuffer = GetSubmittedTestCommandBuffer(explicitDevice->GetTestQueue(), 2u);
    ASSERT_NE(firstSubmittedBuffer, nullptr);
    ASSERT_NE(secondSubmittedBuffer, nullptr);
    ASSERT_NE(thirdSubmittedBuffer, nullptr);
    EXPECT_EQ(firstSubmittedBuffer->dispatchCalls, 1u);
    EXPECT_GT(secondSubmittedBuffer->barrierCalls, 0u);
    EXPECT_EQ(thirdSubmittedBuffer->beginRenderPassCalls, 1u);

    uint32_t barrierBatchCount = 0u;
    for (const auto& submittedBufferBase : explicitDevice->GetTestQueue()->lastSubmitDesc.commandBuffers)
    {
        auto submittedBuffer = std::dynamic_pointer_cast<TestCommandBuffer>(submittedBufferBase);
        if (submittedBuffer != nullptr && submittedBuffer->barrierCalls > 0u)
            ++barrierBatchCount;
    }
    EXPECT_GE(barrierBatchCount, 1u);
}

TEST(ThreadedRenderingLifecycleTests, RhiWorkerSkipsUnrecordableRecordedPassPlans)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    auto frameFence = std::make_shared<TestFence>();
    commandPool->commandBuffer = commandBuffer;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = frameFence;
    frameContext.resourceStateTracker = NLS::Render::RHI::CreateDefaultResourceStateTracker();

    auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 402u;
    snapshot.targetsSwapchain = false;
    snapshot.visibleOpaqueDrawCount = 1u;
    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessFrameSnapshot(driver, snapshot));

    size_t slotIndex = 0u;
    NLS::Render::Context::FrameSnapshot publishedSnapshot;
    ASSERT_TRUE(lifecycle->TryBeginNextRenderScene(&slotIndex, &publishedSnapshot));

    NLS::Render::Context::RenderScenePackage package;
    package.frameId = publishedSnapshot.frameId;
    package.targetsSwapchain = false;
    package.visibleDrawCount = 1u;
    package.opaqueDrawCount = 1u;
    package.hasVisibleDraws = true;
    package.frameDataReady = false;
    package.objectDataReady = true;
    NLS::Render::Context::RenderPassCommandInput passInput;
    passInput.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    passInput.drawCount = 1u;
    passInput.requiresFrameData = true;
    passInput.requiresObjectData = true;
    passInput.requiresLightingData = false;
    passInput.targetsSwapchain = false;
    passInput.renderWidth = 64u;
    passInput.renderHeight = 64u;
    passInput.clearColor = true;
    passInput.clearDepth = true;
    passInput.clearStencil = false;
    passInput.usesColorAttachment = true;
    passInput.usesDepthStencilAttachment = true;
    package.passCommandInputs.push_back(passInput);
    ASSERT_TRUE(lifecycle->CompleteRenderScene(slotIndex, package));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    const auto copiedSlot = lifecycle->CopySlot(0u);
    ASSERT_TRUE(copiedSlot.has_value());
    ASSERT_TRUE(copiedSlot->submissionFrame.has_value());
    EXPECT_FALSE(copiedSlot->submissionFrame->recordedVisibleWork);
    EXPECT_EQ(copiedSlot->submissionFrame->recordedPassCount, 0u);
    EXPECT_EQ(commandBuffer->beginCalls, 0u);
    EXPECT_EQ(commandBuffer->endCalls, 0u);
    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 0u);
}

TEST(ThreadedRenderingLifecycleTests, ThreadedRendererSkipsStandaloneExplicitFrameRecordingForOffscreenFrames)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    auto frameFence = std::make_shared<TestFence>();
    commandPool->commandBuffer = commandBuffer;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = frameFence;

    SnapshotPublishingRenderer renderer(driver);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 64u;
    frameDescriptor.renderHeight = 64u;
    frameDescriptor.camera = &camera;
    frameDescriptor.outputBuffer = reinterpret_cast<NLS::Render::Buffers::Framebuffer*>(1);

    ASSERT_NO_THROW(renderer.BeginFrame(frameDescriptor));
    EXPECT_EQ(NLS::Render::Context::DriverRendererAccess::GetActiveExplicitCommandBuffer(driver), nullptr);

    ASSERT_NO_THROW(renderer.EndFrame());

    const auto telemetry = NLS::Render::Context::DriverRendererAccess::GetThreadedFrameTelemetry(driver);
    EXPECT_EQ(telemetry.publishedFrameCount, 1u);
    EXPECT_EQ(commandBuffer->beginCalls, 0u);
    EXPECT_EQ(commandBuffer->endCalls, 0u);
}

TEST(ThreadedRenderingLifecycleTests, PendingSwapchainResizeBlocksAdditionalSwapchainSnapshotsUntilDrainCompletes)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 2u;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    NLS::Render::Context::FrameSnapshot firstSnapshot;
    firstSnapshot.frameId = 141u;
    firstSnapshot.targetsSwapchain = true;

    NLS::Render::Context::FrameSnapshot secondSnapshot;
    secondSnapshot.frameId = 142u;
    secondSnapshot.targetsSwapchain = true;

    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessFrameSnapshot(driver, firstSnapshot));

    driver.ResizePlatformSwapchain(1600u, 900u);

    EXPECT_FALSE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessFrameSnapshot(driver, secondSnapshot));

    const auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);
    EXPECT_EQ(lifecycle->GetPublishedFrameCount(), 1u);
    EXPECT_EQ(lifecycle->GetInFlightDepth(), 1u);
}

TEST(ThreadedRenderingLifecycleTests, DriverShutdownDrainsPublishedSwapchainFramesBeforeDestroyingWorkers)
{
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    auto swapchain = std::make_shared<TestSwapchain>();
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    auto frameFence = std::make_shared<TestFence>();
    auto imageAcquiredSemaphore = std::make_shared<TestSemaphore>();
    auto renderFinishedSemaphore = std::make_shared<TestSemaphore>();
    commandPool->commandBuffer = commandBuffer;

    {
        NLS::Render::Settings::DriverSettings settings;
        settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
        settings.enableExplicitRHI = false;
        settings.enableThreadedRendering = true;
        settings.threadedFrameSlotCount = 1u;
        settings.framesInFlight = 1u;

        NLS::Render::Context::Driver driver(settings);
        NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
        NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);
        NLS::Render::Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);

        auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
        frameContext.commandBuffer = commandBuffer;
        frameContext.commandPool = commandPool;
        frameContext.frameFence = frameFence;
        frameContext.imageAcquiredSemaphore = imageAcquiredSemaphore;
        frameContext.renderFinishedSemaphore = renderFinishedSemaphore;

        NLS::Render::Context::FrameSnapshot snapshot;
        snapshot.frameId = 151u;
        snapshot.targetsSwapchain = true;
        snapshot.visibleOpaqueDrawCount = 1u;

        ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessFrameSnapshot(driver, snapshot));
    }

    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    EXPECT_EQ(explicitDevice->GetTestQueue()->presentCalls, 1u);
    EXPECT_EQ(swapchain->acquireCalls, 1u);
    EXPECT_EQ(frameFence->waitCalls, 2u);
    EXPECT_EQ(frameFence->resetCalls, 1u);
}

TEST(ThreadedRenderingLifecycleTests, DriverShutdownReleasesRetainedThreadedRhiResourcesBeforeFrameContextAllocators)
{
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    auto frameFence = std::make_shared<TestFence>();
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    commandPool->commandBuffer = commandBuffer;

    auto probe = std::make_shared<ShutdownOrderProbe>();
    std::weak_ptr<NLS::Render::RHI::RHIBindingSet> retainedBindingSet;

    {
        NLS::Render::Settings::DriverSettings settings;
        settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
        settings.enableExplicitRHI = false;
        settings.enableThreadedRendering = true;
        settings.threadedFrameSlotCount = 1u;
        settings.framesInFlight = 1u;

        NLS::Render::Context::Driver driver(settings);
        NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
        NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

        auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
        frameContext.frameFence = frameFence;
        frameContext.commandBuffer = commandBuffer;
        frameContext.commandPool = commandPool;
        frameContext.descriptorAllocator = std::make_shared<ShutdownOrderDescriptorAllocator>(probe);

        NLS::Render::Context::FrameSnapshot snapshot;
        snapshot.frameId = 161u;
        snapshot.targetsSwapchain = false;
        snapshot.renderWidth = 64u;
        snapshot.renderHeight = 64u;
        snapshot.visibleOpaqueDrawCount = 1u;

        {
            auto bindingSet = std::make_shared<ShutdownOrderBindingSet>(probe);
            retainedBindingSet = bindingSet;

            NLS::Render::Context::RecordedDrawCommandInput drawCommand;
            drawCommand.materialBindingSet = bindingSet;
            drawCommand.instanceCount = 1u;

            NLS::Render::Context::RenderPassCommandInput passInput;
            passInput.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
            passInput.targetsSwapchain = false;
            passInput.renderWidth = snapshot.renderWidth;
            passInput.renderHeight = snapshot.renderHeight;
            passInput.drawCount = 1u;
            passInput.recordedDrawCommands.push_back(drawCommand);

            NLS::Render::Context::RenderScenePackage package;
            package.frameId = snapshot.frameId;
            package.targetsSwapchain = false;
            package.renderWidth = snapshot.renderWidth;
            package.renderHeight = snapshot.renderHeight;
            package.hasVisibleDraws = true;
            package.frameDataReady = true;
            package.objectDataReady = true;
            package.visibleDrawCount = 1u;
            package.opaqueDrawCount = 1u;
            package.passCommandInputs.push_back(passInput);
            package.containsCommandInputs = true;
            package.passPlanCount = 1u;

            ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessPreparedFrame(
                driver,
                snapshot,
                package));
        }

        ASSERT_FALSE(retainedBindingSet.expired());
    }

    EXPECT_TRUE(probe->bindingSetDestroyed);
    EXPECT_TRUE(probe->descriptorAllocatorDestroyed);
    EXPECT_TRUE(probe->bindingSetDestroyedBeforeDescriptorAllocator);
    EXPECT_TRUE(retainedBindingSet.expired());
}

TEST(ThreadedRenderingLifecycleTests, ThreadedExplicitFrameAccessIsRejectedWhenThreadedRenderingIsEnabled)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    auto frameFence = std::make_shared<TestFence>();
    commandPool->commandBuffer = commandBuffer;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = frameFence;

    NLS::Render::Context::DriverTestAccess::BeginStandaloneExplicitFrame(driver, true);

    EXPECT_EQ(NLS::Render::Context::DriverRendererAccess::GetActiveExplicitCommandBuffer(driver), nullptr);
    EXPECT_EQ(frameFence->waitCalls, 0u);
    EXPECT_EQ(frameFence->resetCalls, 0u);
    EXPECT_EQ(commandPool->resetCalls, 0u);
    EXPECT_EQ(commandBuffer->resetCalls, 0u);
    EXPECT_EQ(commandBuffer->beginCalls, 0u);
    EXPECT_EQ(commandBuffer->endCalls, 0u);
    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 0u);
    EXPECT_EQ(explicitDevice->GetTestQueue()->presentCalls, 0u);
}

TEST(ThreadedRenderingLifecycleTests, ThreadedUiRenderPreparesStandaloneExplicitFrameForSwapchainRendering)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    auto swapchain = std::make_shared<TestSwapchain>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);
    NLS::Render::Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    auto frameFence = std::make_shared<TestFence>();
    auto imageAcquiredSemaphore = std::make_shared<TestSemaphore>();
    auto renderFinishedSemaphore = std::make_shared<TestSemaphore>();
    commandPool->commandBuffer = commandBuffer;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = frameFence;
    frameContext.imageAcquiredSemaphore = imageAcquiredSemaphore;
    frameContext.renderFinishedSemaphore = renderFinishedSemaphore;

    ASSERT_TRUE(NLS::Render::Context::DriverUIAccess::PrepareUIRender(driver));

    EXPECT_NE(NLS::Render::Context::DriverRendererAccess::GetActiveExplicitCommandBuffer(driver), nullptr);
    EXPECT_EQ(commandPool->resetCalls, 1u);
    EXPECT_EQ(commandBuffer->resetCalls, 1u);
    EXPECT_EQ(commandBuffer->beginCalls, 1u);
    EXPECT_EQ(frameFence->waitCalls, 1u);
    EXPECT_EQ(frameFence->resetCalls, 1u);
    EXPECT_EQ(swapchain->acquireCalls, 1u);
    EXPECT_TRUE(frameContext.hasAcquiredSwapchainImage);
    EXPECT_EQ(frameContext.swapchainImageIndex, 1u);
    EXPECT_EQ(frameContext.swapchainBackbufferView, swapchain->backbufferView);
}

TEST(ThreadedRenderingLifecycleTests, ThreadedUiRenderIsRejectedWhileThreadedSwapchainFrameOwnsFrameContext)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    auto swapchain = std::make_shared<TestSwapchain>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);
    NLS::Render::Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    auto frameFence = std::make_shared<TestFence>();
    auto imageAcquiredSemaphore = std::make_shared<TestSemaphore>();
    auto renderFinishedSemaphore = std::make_shared<TestSemaphore>();
    commandPool->commandBuffer = commandBuffer;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = frameFence;
    frameContext.imageAcquiredSemaphore = imageAcquiredSemaphore;
    frameContext.renderFinishedSemaphore = renderFinishedSemaphore;

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 501u;
    snapshot.targetsSwapchain = true;
    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessFrameSnapshot(driver, snapshot));
    ASSERT_FALSE(NLS::Render::Context::DriverTestAccess::CanBeginStandaloneExplicitFrame(driver));

    EXPECT_FALSE(NLS::Render::Context::DriverUIAccess::PrepareUIRender(driver));
    EXPECT_EQ(NLS::Render::Context::DriverRendererAccess::GetActiveExplicitCommandBuffer(driver), nullptr);
    EXPECT_EQ(frameFence->waitCalls, 0u);
    EXPECT_EQ(frameFence->resetCalls, 0u);
    EXPECT_EQ(commandPool->resetCalls, 0u);
    EXPECT_EQ(commandBuffer->resetCalls, 0u);
    EXPECT_EQ(commandBuffer->beginCalls, 0u);
    EXPECT_EQ(swapchain->acquireCalls, 0u);
}

TEST(ThreadedRenderingLifecycleTests, ThreadedUiPresentSubmitsStandaloneExplicitFrameWithoutOnDemandAcquireBypass)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    auto swapchain = std::make_shared<TestSwapchain>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);
    NLS::Render::Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    auto frameFence = std::make_shared<TestFence>();
    auto imageAcquiredSemaphore = std::make_shared<TestSemaphore>();
    auto renderFinishedSemaphore = std::make_shared<TestSemaphore>();
    commandPool->commandBuffer = commandBuffer;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = frameFence;
    frameContext.imageAcquiredSemaphore = imageAcquiredSemaphore;
    frameContext.renderFinishedSemaphore = renderFinishedSemaphore;

    ASSERT_TRUE(NLS::Render::Context::DriverUIAccess::PrepareUIRender(driver));
    void* const uiSignalSemaphore = reinterpret_cast<void*>(0x1234);
    constexpr uint64_t uiSignalValue = 42u;
    NLS::Render::Context::DriverUIAccess::SetUISignalSemaphore(driver, uiSignalSemaphore, uiSignalValue);

    NLS::Render::Context::DriverUIAccess::PresentSwapchain(driver);

    auto testQueue = explicitDevice->GetTestQueue();
    EXPECT_EQ(commandBuffer->endCalls, 1u);
    EXPECT_EQ(testQueue->submitCalls, 1u);
    EXPECT_EQ(testQueue->presentCalls, 1u);
    EXPECT_EQ(testQueue->lastSubmitDesc.commandBuffers.size(), 1u);
    EXPECT_EQ(testQueue->lastSubmitDesc.waitSemaphores.size(), 1u);
    EXPECT_EQ(testQueue->lastSubmitDesc.signalSemaphores.size(), 1u);
    EXPECT_EQ(testQueue->lastPresentDesc.swapchain, swapchain);
    EXPECT_EQ(testQueue->lastPresentDesc.imageIndex, 1u);
    EXPECT_EQ(testQueue->lastPresentDesc.waitSemaphores.size(), 1u);
    EXPECT_EQ(testQueue->lastPresentDesc.uiSignalSemaphore, uiSignalSemaphore);
    EXPECT_EQ(testQueue->lastPresentDesc.uiSignalValue, uiSignalValue);
    EXPECT_EQ(swapchain->acquireCalls, 1u);
}

TEST(ThreadedRenderingLifecycleTests, ThreadedMainThreadPresentSkipsWhenNoStandaloneUiFrameIsActive)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    auto swapchain = std::make_shared<TestSwapchain>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);
    NLS::Render::Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);

    NLS::Render::Context::DriverUIAccess::PresentSwapchain(driver);

    EXPECT_EQ(swapchain->acquireCalls, 0u);
    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 0u);
    EXPECT_EQ(explicitDevice->GetTestQueue()->presentCalls, 0u);
}

TEST(ThreadedRenderingLifecycleTests, StandaloneExplicitFramePresentDoesNotRequireAdditionalPresentHandshake)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = false;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    auto swapchain = std::make_shared<TestSwapchain>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);
    NLS::Render::Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    auto frameFence = std::make_shared<TestFence>();
    auto imageAcquiredSemaphore = std::make_shared<TestSemaphore>();
    auto renderFinishedSemaphore = std::make_shared<TestSemaphore>();
    commandPool->commandBuffer = commandBuffer;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = frameFence;
    frameContext.imageAcquiredSemaphore = imageAcquiredSemaphore;
    frameContext.renderFinishedSemaphore = renderFinishedSemaphore;

    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::CanBeginStandaloneExplicitFrame(driver));

    NLS::Render::Context::DriverTestAccess::BeginStandaloneExplicitFrame(driver, true);
    NLS::Render::Context::DriverTestAccess::EndStandaloneExplicitFrame(driver, true);

    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    EXPECT_EQ(explicitDevice->GetTestQueue()->presentCalls, 1u);
    EXPECT_EQ(swapchain->acquireCalls, 1u);

    NLS::Render::Context::DriverUIAccess::PresentSwapchain(driver);

    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    EXPECT_EQ(explicitDevice->GetTestQueue()->presentCalls, 1u);
    EXPECT_EQ(swapchain->acquireCalls, 1u);
}

TEST(ThreadedRenderingLifecycleTests, ThreadedUiPresentFinalizesStandaloneFrameAllocatorsAndUploads)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    auto swapchain = std::make_shared<TestSwapchain>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);
    NLS::Render::Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    auto frameFence = std::make_shared<TestFence>();
    auto imageAcquiredSemaphore = std::make_shared<TestSemaphore>();
    auto renderFinishedSemaphore = std::make_shared<TestSemaphore>();
    auto descriptorAllocator = std::make_shared<TestDescriptorAllocator>();
    auto uploadContext = std::make_shared<TestUploadContext>();
    commandPool->commandBuffer = commandBuffer;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = frameFence;
    frameContext.imageAcquiredSemaphore = imageAcquiredSemaphore;
    frameContext.renderFinishedSemaphore = renderFinishedSemaphore;
    frameContext.descriptorAllocator = descriptorAllocator;
    frameContext.uploadContext = uploadContext;

    ASSERT_TRUE(NLS::Render::Context::DriverUIAccess::PrepareUIRender(driver));
    NLS::Render::Context::DriverUIAccess::PresentSwapchain(driver);

    EXPECT_EQ(descriptorAllocator->beginFrameCalls, 1u);
    EXPECT_EQ(descriptorAllocator->endFrameCalls, 1u);
    EXPECT_EQ(descriptorAllocator->lastBeginFrameIndex, 0u);
    EXPECT_EQ(descriptorAllocator->lastEndFrameIndex, 0u);
    EXPECT_EQ(uploadContext->beginFrameCalls, 1u);
    EXPECT_EQ(uploadContext->endFrameCalls, 1u);
    EXPECT_EQ(uploadContext->lastBeginFrameIndex, 0u);
    EXPECT_EQ(uploadContext->lastEndFrameIndex, 0u);
}

TEST(ThreadedRenderingLifecycleTests, ThreadedPreparedFramePublishesCompletedPreferredReadbackTexture)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    auto frameFence = std::make_shared<TestFence>();
    commandPool->commandBuffer = commandBuffer;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = frameFence;

    NLS::Render::RHI::RHITextureDesc readbackDesc;
    readbackDesc.debugName = "PreferredReadback";
    readbackDesc.extent = { 64u, 64u, 1u };
    auto preferredReadbackTexture = std::make_shared<TestTexture>(readbackDesc);
    NLS::Render::RHI::RHITextureViewDesc readbackViewDesc;
    readbackViewDesc.debugName = "PreferredReadbackView";
    auto preferredReadbackView = std::make_shared<TestTextureView>(
        preferredReadbackTexture,
        readbackViewDesc);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 611u;
    snapshot.targetsSwapchain = false;
    snapshot.renderWidth = 64u;
    snapshot.renderHeight = 64u;

    NLS::Render::Context::RenderScenePackage package;
    package.frameId = snapshot.frameId;
    package.targetsSwapchain = false;
    package.frameDataReady = true;
    package.objectDataReady = true;
    package.preferredReadbackTexture = preferredReadbackTexture;
    package.extractedTextures.push_back(preferredReadbackTexture);
    NLS::Render::Context::RenderPassCommandInput passInput;
    passInput.kind = NLS::Render::Context::RenderPassCommandKind::Helper;
    passInput.queueType = NLS::Render::RHI::QueueType::Graphics;
    passInput.queueDependencyPolicy = NLS::Render::Context::QueueDependencyPolicy::Previous;
    passInput.debugName = "PreferredReadbackVisibility";
    passInput.renderWidth = 64u;
    passInput.renderHeight = 64u;
    passInput.clearColor = true;
    passInput.usesColorAttachment = true;
    passInput.colorAttachmentViews = { preferredReadbackView };
    package.passCommandInputs.push_back(std::move(passInput));
    package.containsCommandInputs = true;
    package.passPlanCount = 1u;

    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessPreparedFrame(
        driver,
        snapshot,
        package));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    EXPECT_EQ(
        NLS::Render::Context::DriverRendererAccess::ResolveReadbackTexture(driver),
        preferredReadbackTexture);
}

TEST(ThreadedRenderingLifecycleTests, CompletedReadbackHistoryRetainsPreviousTextureForExplicitReadbackConsumers)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    NLS::Render::RHI::RHITextureDesc pickingDesc;
    pickingDesc.debugName = "SceneViewPickingReadback";
    pickingDesc.extent = { 64u, 64u, 1u };
    auto pickingTexture = std::make_shared<TestTexture>(pickingDesc);

    NLS::Render::RHI::RHITextureDesc gameViewDesc;
    gameViewDesc.debugName = "GameViewReadback";
    gameViewDesc.extent = { 64u, 64u, 1u };
    auto gameViewTexture = std::make_shared<TestTexture>(gameViewDesc);

    NLS::Render::Context::DriverTestAccess::SetCompletedReadbackTexture(driver, pickingTexture);
    NLS::Render::Context::DriverTestAccess::SetCompletedReadbackTexture(driver, gameViewTexture);

    EXPECT_EQ(
        NLS::Render::Context::DriverRendererAccess::ResolveReadbackTexture(driver),
        gameViewTexture);
    EXPECT_TRUE(NLS::Render::Context::DriverRendererAccess::HasCompletedReadbackTexture(
        driver,
        pickingTexture));

    uint8_t pixel[3] {};
    NLS::Render::Context::DriverRendererAccess::ReadPixels(
        driver,
        pickingTexture,
        0u,
        0u,
        1u,
        1u,
        NLS::Render::Settings::EPixelDataFormat::RGB,
        NLS::Render::Settings::EPixelDataType::UNSIGNED_BYTE,
        pixel);

    EXPECT_EQ(explicitDevice->lastReadPixelsTexture, pickingTexture);
}
