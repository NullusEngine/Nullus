#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "Core/ServiceLocator.h"
#include "Jobs/JobSystem.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Context/DriverInternal.h"
#include "Rendering/Context/RenderThreadCoordinator.h"
#include "Rendering/Context/RhiThreadCoordinator.h"
#include "Rendering/Context/SwapchainResizePolicy.h"
#include "Rendering/Context/ThreadedRenderingLifecycle.h"
#include "Rendering/Core/CompositeRenderer.h"
#include "Rendering/Data/FrameDescriptor.h"
#include "Rendering/Entities/Camera.h"
#include "Rendering/DeferredSceneRenderer.h"
#include "Rendering/ForwardSceneRenderer.h"
#include "Rendering/Buffers/Framebuffer.h"
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
    class ScopedThreadedRenderingJobSystem
    {
    public:
        explicit ScopedThreadedRenderingJobSystem(const uint32_t workerCount)
        {
            if (NLS::Base::Jobs::IsJobSystemInitialized())
                return;

            NLS::Base::Jobs::JobSystemConfig config;
            config.workerCount = workerCount;
            m_ownsRuntime = NLS::Base::Jobs::TryInitializeJobSystem(config);
        }

        ~ScopedThreadedRenderingJobSystem()
        {
            if (!m_ownsRuntime)
                return;

            NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::Immediate);
#if defined(NLS_ENABLE_TEST_HOOKS)
            NLS::Base::Jobs::ResetJobSystemForTesting();
#endif
        }

        bool IsInitialized() const
        {
            return NLS::Base::Jobs::IsJobSystemInitialized();
        }

    private:
        bool m_ownsRuntime = false;
    };

    bool ContainsRetainedResource(
        const std::vector<std::shared_ptr<void>>& keepAlive,
        const std::shared_ptr<void>& resource)
    {
        return std::find(keepAlive.begin(), keepAlive.end(), resource) != keepAlive.end();
    }

    class ScopedDriverService final
    {
    public:
        explicit ScopedDriverService(NLS::Render::Context::Driver& driver)
        {
            NLS::Core::ServiceLocator::Provide(driver);
        }

        ~ScopedDriverService()
        {
            NLS::Core::ServiceLocator::Remove<NLS::Render::Context::Driver>();
        }

        ScopedDriverService(const ScopedDriverService&) = delete;
        ScopedDriverService& operator=(const ScopedDriverService&) = delete;
    };

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
        uint64_t sceneGameObjectCount = 0u;
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
            const uint64_t sceneGameObjectCount = HasDescriptor<DescriptorRequiredForPreparedBuilder>()
                ? GetDescriptor<DescriptorRequiredForPreparedBuilder>().sceneGameObjectCount
                : 0u;

            return [snapshot, sceneGameObjectCount]()
            {
                NLS::Render::Context::RenderScenePackage package;
                package.frameId = snapshot.frameId;
                package.hasVisibleDraws = true;
                package.visibleDrawCount = 1u;
                package.frameDataReady = true;
                package.objectDataReady = true;
                package.sceneGameObjectCount = sceneGameObjectCount;
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

    class TestCompletionToken final : public NLS::Render::RHI::RHICompletionToken
    {
    public:
        explicit TestCompletionToken(NLS::Render::RHI::RHICompletionStatus status)
            : m_status(std::move(status))
        {
        }

        std::string_view GetDebugName() const override { return "TestCompletionToken"; }
        NLS::Render::RHI::RHICompletionStatus Poll() override { return m_status; }
        bool IsComplete() override { return m_status.IsComplete(); }
        NLS::Render::RHI::RHICompletionStatus GetStatus() override { return m_status; }
        NLS::Render::RHI::RHICompletionStatus Wait(uint64_t = 0) override { return m_status; }

    private:
        NLS::Render::RHI::RHICompletionStatus m_status;
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
            if (waitResultIndex < waitResults.size())
            {
                const bool result = waitResults[waitResultIndex++];
                if (result)
                    signaled = true;
                return result;
            }
            signaled = true;
            return true;
        }

        mutable size_t waitCalls = 0u;
        mutable size_t resetCalls = 0u;
        std::vector<bool> waitResults;
        size_t waitResultIndex = 0u;
        bool signaled = false;
        bool submitSignalRequested = false;
    };

    class TestSemaphore final : public NLS::Render::RHI::RHISemaphore
    {
    public:
        std::string_view GetDebugName() const override { return "TestSemaphore"; }
        bool IsSignaled() const override { return signaled; }
        void Reset() override
        {
            signaled = false;
            waitValue = 0u;
            ++resetCalls;
        }

        NLS::Render::RHI::NativeHandle GetNativeSemaphoreHandle() override
        {
            return { NLS::Render::RHI::BackendType::DX12, this, waitValue };
        }

        mutable size_t resetCalls = 0u;
        uint64_t waitValue = 0u;
        bool signaled = false;
    };

    class TestCommandBuffer final : public NLS::Render::RHI::RHICommandBuffer
    {
    public:
        std::string_view GetDebugName() const override { return "TestCommandBuffer"; }
        void Begin() override
        {
            ++beginCalls;
            recording = !stayNonRecordingOnBegin;
            closedForSubmission = false;
            events.emplace_back("Begin");
        }
        void End() override
        {
            ++endCalls;
            recording = false;
            closedForSubmission = true;
            events.emplace_back("End");
        }
        void Reset() override
        {
            ++resetCalls;
            recording = false;
            closedForSubmission = true;
        }
        bool IsRecording() const override { return recording; }
        NLS::Render::RHI::NativeHandle GetNativeCommandBuffer() const override { return {}; }
        bool IsClosedForSubmission() const override { return !recording && closedForSubmission; }
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
        NLS::Render::RHI::RHICommandRecordingResult DrawChecked(uint32_t, uint32_t, uint32_t, uint32_t) override
        {
            Draw(0u, 0u, 0u, 0u);
            if (failDrawChecked)
            {
                return {
                    NLS::Render::RHI::RHICommandRecordingStatusCode::BackendFailure,
                    "test draw failure"
                };
            }
            return {};
        }
        void DrawIndexed(uint32_t, uint32_t, uint32_t, int32_t, uint32_t) override
        {
            ++drawIndexedCalls;
            events.emplace_back("DrawIndexed");
        }
        NLS::Render::RHI::RHICommandRecordingResult DrawIndexedChecked(uint32_t, uint32_t, uint32_t, int32_t, uint32_t) override
        {
            DrawIndexed(0u, 0u, 0u, 0, 0u);
            if (failDrawChecked)
            {
                return {
                    NLS::Render::RHI::RHICommandRecordingStatusCode::BackendFailure,
                    "test indexed draw failure"
                };
            }
            return {};
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
        NLS::Render::RHI::RHICommandRecordingResult BarrierChecked(
            const NLS::Render::RHI::RHIBarrierDesc& barrierDesc) override
        {
            ++barrierCheckedCalls;
            if (failBarrierChecked)
            {
                return {
                    NLS::Render::RHI::RHICommandRecordingStatusCode::BackendFailure,
                    "test barrier failure"
                };
            }

            Barrier(barrierDesc);
            return {};
        }
        bool IsChildCommandBuffer() const override { return isChildCommandBuffer; }
        bool CanExecuteChildCommandBuffers() const override { return !isChildCommandBuffer; }
        NLS::Render::RHI::RHICommandRecordingResult ExecuteChildCommandBuffer(
            const std::shared_ptr<NLS::Render::RHI::RHICommandBuffer>& childCommandBuffer) override
        {
            ++executeChildCommandBufferCalls;
            events.emplace_back("ExecuteChildCommandBuffer");
            if (failExecuteChildCommandBuffer)
            {
                return {
                    NLS::Render::RHI::RHICommandRecordingStatusCode::BackendFailure,
                    "test execute child failure"
                };
            }

            auto testChild = std::dynamic_pointer_cast<TestCommandBuffer>(childCommandBuffer);
            if (testChild == nullptr || !testChild->IsChildCommandBuffer())
            {
                return {
                    NLS::Render::RHI::RHICommandRecordingStatusCode::InvalidArgument,
                    "expected a child test command buffer"
                };
            }
            executedChildCommandBuffers.push_back(std::move(testChild));
            return {};
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
        size_t barrierCheckedCalls = 0u;
        size_t executeChildCommandBufferCalls = 0u;
        bool recording = false;
        bool closedForSubmission = true;
        bool stayNonRecordingOnBegin = false;
        bool failBarrierChecked = false;
        bool failExecuteChildCommandBuffer = false;
        bool failDrawChecked = false;
        bool isChildCommandBuffer = false;
        NLS::Render::RHI::RHIRenderPassDesc lastRenderPassDesc {};
        NLS::Render::RHI::RHIViewport lastViewport {};
        std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline> lastGraphicsPipeline;
        std::vector<uint32_t> boundSetIndices;
        std::vector<std::shared_ptr<NLS::Render::RHI::RHIBindingSet>> boundBindingSets;
        std::vector<NLS::Render::RHI::RHIBarrierDesc> barrierHistory;
        std::vector<std::shared_ptr<TestCommandBuffer>> executedChildCommandBuffers;
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
        NLS::Render::RHI::NativeHandle GetNativeDescriptorHeapCompatibilityHandle(uint32_t heapClass) const override
        {
            return heapClass == 1u
                ? samplerDescriptorHeapHandle
                : resourceDescriptorHeapHandle;
        }

        NLS::Render::RHI::NativeHandle resourceDescriptorHeapHandle {};
        NLS::Render::RHI::NativeHandle samplerDescriptorHeapHandle {};

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

        TestGraphicsPipeline(
            std::string debugName,
            NLS::Render::RHI::RHIRenderTargetLayoutDesc renderTargetLayout)
        {
            m_desc.debugName = std::move(debugName);
            m_desc.renderTargetLayout = std::move(renderTargetLayout);
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
        std::shared_ptr<NLS::Render::RHI::RHICommandBuffer> CreateChildCommandBuffer(std::string = {}) override
        {
            std::lock_guard lock(childCommandBuffersMutex);
            if (failNextChildCommandBufferCreates > 0u)
            {
                --failNextChildCommandBufferCreates;
                return nullptr;
            }

            auto child = std::make_shared<TestCommandBuffer>();
            child->isChildCommandBuffer = true;
            child->failDrawChecked = failCreatedChildDrawChecked;
            childCommandBuffers.push_back(child);
            return child;
        }
        void Reset() override { ++resetCalls; }

        NLS::Render::RHI::QueueType queueType = NLS::Render::RHI::QueueType::Graphics;
        std::shared_ptr<NLS::Render::RHI::RHICommandBuffer> commandBuffer;
        std::vector<std::shared_ptr<TestCommandBuffer>> childCommandBuffers;
        std::mutex childCommandBuffersMutex;
        size_t failNextChildCommandBufferCreates = 0u;
        bool failCreatedChildDrawChecked = false;
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

        NLS::Render::RHI::DescriptorAllocationBatch AllocateBatch(
            const std::vector<NLS::Render::RHI::DescriptorAllocationRequest>& requests) override
        {
            NLS::Render::RHI::DescriptorAllocationBatch batch;
            batch.allocations.reserve(requests.size());
            for (const auto& request : requests)
            {
                batch.totalRequested += request.count;
                auto allocation = Allocate(request);
                if (!allocation.IsValid())
                    batch.allSucceeded = false;
                else
                    batch.totalAllocated += allocation.count;
                batch.allocations.push_back(std::move(allocation));
            }
            return batch;
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

        NLS::Render::RHI::DescriptorAllocationBatch AllocateBatch(
            const std::vector<NLS::Render::RHI::DescriptorAllocationRequest>& requests) override
        {
            NLS::Render::RHI::DescriptorAllocationBatch batch;
            batch.allocations.reserve(requests.size());
            for (const auto& request : requests)
            {
                batch.totalRequested += request.count;
                auto allocation = Allocate(request);
                if (!allocation.IsValid())
                    batch.allSucceeded = false;
                else
                    batch.totalAllocated += allocation.count;
                batch.allocations.push_back(std::move(allocation));
            }
            return batch;
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

        NLS::Render::RHI::UploadBatchSubmission SubmitUploadBatch(
            NLS::Render::RHI::RHICommandBuffer&,
            const NLS::Render::RHI::UploadBatchRequest& request) override
        {
            NLS::Render::RHI::UploadBatchSubmission submission;
            submission.accepted = !request.bufferUploads.empty() || !request.textureUploads.empty();
            submission.acceptedBufferUploads = request.bufferUploads.size();
            submission.acceptedTextureUploads = request.textureUploads.size();
            return submission;
        }

        NLS::Render::RHI::UploadSubmission SubmitUploadBuffer(
            NLS::Render::RHI::RHICommandBuffer&,
            const NLS::Render::RHI::UploadBufferRequest&) override
        {
            return { true, nullptr, {} };
        }

        NLS::Render::RHI::UploadSubmission SubmitUploadTexture(
            NLS::Render::RHI::RHICommandBuffer&,
            const NLS::Render::RHI::UploadTextureRequest&) override
        {
            return { true, nullptr, {} };
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

    class TestResourceStateTracker final : public NLS::Render::RHI::ResourceStateTracker
    {
    public:
        void BeginFrame(uint64_t frameIndex) override
        {
            ++beginFrameCalls;
            lastBeginFrameIndex = frameIndex;
            m_stats.currentFrameIndex = frameIndex;
        }

        void Reset() override
        {
            ++resetCalls;
        }

        std::optional<NLS::Render::RHI::TrackedBufferState> GetBufferState(
            const std::shared_ptr<NLS::Render::RHI::RHIBuffer>&) const override
        {
            return std::nullopt;
        }

        std::optional<NLS::Render::RHI::TrackedTextureState> GetTextureState(
            const std::shared_ptr<NLS::Render::RHI::RHITexture>&,
            const NLS::Render::RHI::RHISubresourceRange&) const override
        {
            return std::nullopt;
        }

        NLS::Render::RHI::RHIBarrierDesc BuildTransitionBarriers(
            const std::vector<NLS::Render::RHI::RHIBufferBarrier>&,
            const std::vector<NLS::Render::RHI::RHITextureBarrier>&) const override
        {
            return {};
        }

        void RegisterTransientBuffer(
            const std::shared_ptr<NLS::Render::RHI::RHIBuffer>&,
            uint64_t) override
        {
        }

        void RegisterTransientTexture(
            const std::shared_ptr<NLS::Render::RHI::RHITexture>&,
            const NLS::Render::RHI::RHISubresourceRange&,
            uint64_t) override
        {
        }

        void RegisterTransientTextureView(
            const std::shared_ptr<NLS::Render::RHI::RHITextureView>&,
            uint64_t) override
        {
        }

        void RetireTransientResources(uint64_t completedFrameIndex) override
        {
            ++retireTransientResourcesCalls;
            lastRetiredFrameIndex = completedFrameIndex;
        }

        void Commit(const NLS::Render::RHI::RHIBarrierDesc&) override {}
        NLS::Render::RHI::ResourceStateTrackerStats GetStats() const override { return m_stats; }

        size_t beginFrameCalls = 0u;
        size_t resetCalls = 0u;
        size_t retireTransientResourcesCalls = 0u;
        uint64_t lastBeginFrameIndex = 0u;
        uint64_t lastRetiredFrameIndex = 0u;

    private:
        NLS::Render::RHI::ResourceStateTrackerStats m_stats {};
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

        NLS::Render::RHI::DescriptorAllocationBatch AllocateBatch(
            const std::vector<NLS::Render::RHI::DescriptorAllocationRequest>& requests) override
        {
            NLS::Render::RHI::DescriptorAllocationBatch batch;
            batch.allocations.reserve(requests.size());
            for (const auto& request : requests)
            {
                batch.totalRequested += request.count;
                auto allocation = Allocate(request);
                if (!allocation.IsValid())
                    batch.allSucceeded = false;
                else
                    batch.totalAllocated += allocation.count;
                batch.allocations.push_back(std::move(allocation));
            }
            return batch;
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
        enum class SubmitFailureStage
        {
            Legacy,
            BeforeQueueWork,
            AfterWaits,
            AfterCommandExecution,
            AfterSemaphoreSignals,
            AfterFenceSignal
        };

        enum class PresentFailureStage
        {
            Legacy,
            BeforeQueueWork,
            AfterWaits,
            AfterUiWait,
            AfterPresent,
            AfterFenceSignal
        };

        explicit TestQueue(NLS::Render::RHI::QueueType queueType = NLS::Render::RHI::QueueType::Graphics)
            : m_queueType(queueType)
        {
        }

        std::string_view GetDebugName() const override { return "TestQueue"; }
        NLS::Render::RHI::QueueType GetType() const override { return m_queueType; }
        void Submit(const NLS::Render::RHI::RHISubmitDesc& submitDesc) override
        {
            RecordSubmitCall(submitDesc);
            ApplySubmitWaits(submitDesc);
            ApplySubmitSemaphoreSignals(submitDesc);
            ApplySubmitFenceSignal(submitDesc);
        }
        NLS::Render::RHI::RHIQueueOperationResult SubmitChecked(
            const NLS::Render::RHI::RHISubmitDesc& submitDesc) override
        {
            auto result = nextSubmitResult;
            if (submitFailureStage == SubmitFailureStage::Legacy)
            {
                if (!result.Succeeded() &&
                    !result.mayHaveQueuedGpuWork &&
                    !result.frameFenceSignalQueued)
                {
                    return result;
                }

                Submit(submitDesc);
                result.mayHaveQueuedGpuWork =
                    result.mayHaveQueuedGpuWork ||
                    !submitDesc.waitSemaphores.empty() ||
                    !submitDesc.commandBuffers.empty() ||
                    !submitDesc.signalSemaphores.empty();
                result.frameFenceSignalQueued = submitDesc.signalFence != nullptr &&
                    (result.frameFenceSignalQueued ||
                        (autoReportFrameFenceSignalOnSuccess && result.Succeeded()));
                return result;
            }

            if (submitFailureStage == SubmitFailureStage::BeforeQueueWork)
                return result;

            RecordSubmitCall(submitDesc);
            if (ReachedStage(submitFailureStage, SubmitFailureStage::AfterWaits))
            {
                ApplySubmitWaits(submitDesc);
                result.mayHaveQueuedGpuWork =
                    result.mayHaveQueuedGpuWork ||
                    !submitDesc.waitSemaphores.empty();
            }
            if (ReachedStage(submitFailureStage, SubmitFailureStage::AfterCommandExecution))
            {
                result.mayHaveQueuedGpuWork =
                    result.mayHaveQueuedGpuWork ||
                    !submitDesc.commandBuffers.empty();
            }
            if (ReachedStage(submitFailureStage, SubmitFailureStage::AfterSemaphoreSignals))
            {
                ApplySubmitSemaphoreSignals(submitDesc);
                result.mayHaveQueuedGpuWork =
                    result.mayHaveQueuedGpuWork ||
                    !submitDesc.signalSemaphores.empty();
            }
            result.frameFenceSignalQueued = false;
            if (ReachedStage(submitFailureStage, SubmitFailureStage::AfterFenceSignal))
            {
                ApplySubmitFenceSignal(submitDesc);
                result.frameFenceSignalQueued = submitDesc.signalFence != nullptr;
            }
            return result;
        }
        void Present(const NLS::Render::RHI::RHIPresentDesc& presentDesc) override
        {
            ++presentCalls;
            lastPresentDesc = presentDesc;
        }
        NLS::Render::RHI::RHIQueueOperationResult PresentChecked(
            const NLS::Render::RHI::RHIPresentDesc& presentDesc) override
        {
            auto result = nextPresentResult;
            if (presentFailureStage == PresentFailureStage::Legacy)
            {
                Present(presentDesc);
                result.mayHaveQueuedGpuWork =
                    result.mayHaveQueuedGpuWork ||
                    !presentDesc.waitSemaphores.empty() ||
                    presentDesc.uiSignalSemaphore.IsValid();
                if (presentDesc.signalFence != nullptr)
                {
                    ApplyPresentFenceSignal(presentDesc);
                    result.frameFenceSignalQueued =
                        result.frameFenceSignalQueued ||
                        (autoReportFrameFenceSignalOnSuccess && result.Succeeded());
                }
                return result;
            }

            if (presentFailureStage == PresentFailureStage::BeforeQueueWork)
                return result;

            if (ReachedStage(presentFailureStage, PresentFailureStage::AfterWaits))
            {
                ApplyPresentWaits(presentDesc);
                result.mayHaveQueuedGpuWork =
                    result.mayHaveQueuedGpuWork ||
                    !presentDesc.waitSemaphores.empty();
            }
            if (ReachedStage(presentFailureStage, PresentFailureStage::AfterUiWait))
            {
                result.mayHaveQueuedGpuWork =
                    result.mayHaveQueuedGpuWork ||
                    presentDesc.uiSignalSemaphore.IsValid();
            }
            if (ReachedStage(presentFailureStage, PresentFailureStage::AfterPresent))
            {
                Present(presentDesc);
                result.mayHaveQueuedGpuWork = true;
            }
            result.frameFenceSignalQueued = false;
            if (ReachedStage(presentFailureStage, PresentFailureStage::AfterFenceSignal))
            {
                ApplyPresentFenceSignal(presentDesc);
                result.frameFenceSignalQueued = presentDesc.signalFence != nullptr;
            }
            return result;
        }

    private:
        static bool ReachedStage(SubmitFailureStage actual, SubmitFailureStage required)
        {
            return static_cast<int>(actual) >= static_cast<int>(required);
        }

        static bool ReachedStage(PresentFailureStage actual, PresentFailureStage required)
        {
            return static_cast<int>(actual) >= static_cast<int>(required);
        }

        void RecordSubmitCall(const NLS::Render::RHI::RHISubmitDesc& submitDesc)
        {
            ++submitCalls;
            lastSubmitDesc = submitDesc;
            submitHistory.push_back(submitDesc);
        }

        void ApplySubmitWaits(const NLS::Render::RHI::RHISubmitDesc& submitDesc)
        {
            for (const auto& semaphore : submitDesc.waitSemaphores)
            {
                auto testSemaphore = std::dynamic_pointer_cast<TestSemaphore>(semaphore);
                if (testSemaphore != nullptr)
                    testSemaphore->signaled = false;
            }
        }

        void ApplySubmitSemaphoreSignals(const NLS::Render::RHI::RHISubmitDesc& submitDesc)
        {
            for (const auto& semaphore : submitDesc.signalSemaphores)
            {
                auto testSemaphore = std::dynamic_pointer_cast<TestSemaphore>(semaphore);
                if (testSemaphore != nullptr)
                {
                    testSemaphore->signaled = true;
                    ++testSemaphore->waitValue;
                }
            }
        }

        void ApplySubmitFenceSignal(const NLS::Render::RHI::RHISubmitDesc& submitDesc)
        {
            if (submitDesc.signalFence != nullptr)
            {
                auto testFence = std::dynamic_pointer_cast<TestFence>(submitDesc.signalFence);
                if (testFence != nullptr)
                {
                    testFence->Reset();
                    testFence->submitSignalRequested = true;
                }
            }
        }

        void ApplyPresentWaits(const NLS::Render::RHI::RHIPresentDesc& presentDesc)
        {
            for (const auto& semaphore : presentDesc.waitSemaphores)
            {
                auto testSemaphore = std::dynamic_pointer_cast<TestSemaphore>(semaphore);
                if (testSemaphore != nullptr)
                    testSemaphore->signaled = false;
            }
        }

        void ApplyPresentFenceSignal(const NLS::Render::RHI::RHIPresentDesc& presentDesc)
        {
            if (presentDesc.signalFence != nullptr)
            {
                auto testFence = std::dynamic_pointer_cast<TestFence>(presentDesc.signalFence);
                if (testFence != nullptr)
                {
                    testFence->Reset();
                    testFence->submitSignalRequested = true;
                }
            }
        }

        NLS::Render::RHI::QueueType m_queueType = NLS::Render::RHI::QueueType::Graphics;

    public:
        size_t submitCalls = 0u;
        size_t presentCalls = 0u;
        NLS::Render::RHI::RHISubmitDesc lastSubmitDesc {};
        NLS::Render::RHI::RHIPresentDesc lastPresentDesc {};
        NLS::Render::RHI::RHIQueueOperationResult nextSubmitResult {};
        NLS::Render::RHI::RHIQueueOperationResult nextPresentResult {};
        bool autoReportFrameFenceSignalOnSuccess = true;
        SubmitFailureStage submitFailureStage = SubmitFailureStage::Legacy;
        PresentFailureStage presentFailureStage = PresentFailureStage::Legacy;
        std::vector<NLS::Render::RHI::RHISubmitDesc> submitHistory;
    };

    class TestExplicitDevice final : public NLS::Render::RHI::RHIDevice
    {
    public:
        using NLS::Render::RHI::RHIDevice::CreateBuffer;
        using NLS::Render::RHI::RHIDevice::CreateTexture;

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
        void FailNextCommandBufferBeginCalls(const size_t count)
        {
            m_failNextCommandBufferBeginCalls.store(count, std::memory_order_relaxed);
        }
        void FailNextCommandBufferBarrierCheckedCalls(const size_t count)
        {
            m_failNextCommandBufferBarrierCheckedCalls.store(count, std::memory_order_relaxed);
        }
        void FailCommandBufferBarrierCheckedAtCommandPoolCall(const size_t callIndex)
        {
            m_failCommandBufferBarrierCheckedAtCommandPoolCall.store(callIndex, std::memory_order_relaxed);
        }
        void FailNextCreatedCommandPools(const size_t count)
        {
            m_failNextCreatedCommandPools.store(count, std::memory_order_relaxed);
        }
        void FailCreatedCommandPoolAtCall(const size_t callIndex)
        {
            m_failCreatedCommandPoolAtCall.store(callIndex, std::memory_order_relaxed);
        }
        void FailCreatedChildCommandBufferDraws(const bool fail)
        {
            m_failCreatedChildCommandBufferDraws.store(fail, std::memory_order_relaxed);
        }
        void SetComputeQueue(std::shared_ptr<TestQueue> computeQueue)
        {
            m_computeQueue = std::move(computeQueue);
            m_hasDedicatedComputeQueueOverride = true;
        }
        std::vector<std::shared_ptr<TestCommandPool>> GetCreatedCommandPools() const
        {
            std::lock_guard<std::mutex> lock(m_createdCommandPoolsMutex);
            return m_createdCommandPools;
        }

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
        std::shared_ptr<NLS::Render::RHI::RHIBuffer> CreateBuffer(
            const NLS::Render::RHI::RHIBufferDesc&,
            const NLS::Render::RHI::RHIBufferUploadDesc&) override
        {
            return nullptr;
        }
        std::shared_ptr<NLS::Render::RHI::RHITexture> CreateTexture(
            const NLS::Render::RHI::RHITextureDesc& desc,
            const NLS::Render::RHI::RHITextureUploadDesc&) override
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
        std::shared_ptr<NLS::Render::RHI::RHICommandPool> CreateCommandPool(
            NLS::Render::RHI::QueueType queueType,
            std::string debugName = {}) override
        {
            const bool isFrameContextCommandPool = debugName.rfind("FrameCommandPool", 0u) == 0u;
            if (isFrameContextCommandPool)
            {
                auto pool = std::make_shared<TestCommandPool>();
                pool->commandBuffer = std::make_shared<TestCommandBuffer>();
                pool->queueType = queueType;
                return pool;
            }

            const size_t commandPoolCallIndex =
                m_createdCommandPoolCallCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
            const size_t failAtCall = m_failCreatedCommandPoolAtCall.load(std::memory_order_relaxed);
            if (failAtCall != 0u && commandPoolCallIndex == failAtCall)
                return nullptr;

            size_t remainingPoolFailures = m_failNextCreatedCommandPools.load(std::memory_order_relaxed);
            while (remainingPoolFailures > 0u)
            {
                if (m_failNextCreatedCommandPools.compare_exchange_weak(
                        remainingPoolFailures,
                        remainingPoolFailures - 1u,
                        std::memory_order_relaxed))
                {
                    return nullptr;
                }
            }

            auto pool = std::make_shared<TestCommandPool>();
            pool->commandBuffer = std::make_shared<TestCommandBuffer>();
            pool->failCreatedChildDrawChecked =
                m_failCreatedChildCommandBufferDraws.load(std::memory_order_relaxed);
            auto testCommandBuffer = std::static_pointer_cast<TestCommandBuffer>(pool->commandBuffer);
            const size_t failBarrierAtCall =
                m_failCommandBufferBarrierCheckedAtCommandPoolCall.load(std::memory_order_relaxed);
            if (failBarrierAtCall != 0u && commandPoolCallIndex == failBarrierAtCall)
                testCommandBuffer->failBarrierChecked = true;
            size_t remainingFailures = m_failNextCommandBufferBeginCalls.load(std::memory_order_relaxed);
            while (remainingFailures > 0u)
            {
                if (m_failNextCommandBufferBeginCalls.compare_exchange_weak(
                        remainingFailures,
                        remainingFailures - 1u,
                        std::memory_order_relaxed))
                {
                    testCommandBuffer->stayNonRecordingOnBegin = true;
                    break;
                }
            }
            size_t remainingBarrierFailures = m_failNextCommandBufferBarrierCheckedCalls.load(std::memory_order_relaxed);
            while (remainingBarrierFailures > 0u)
            {
                if (m_failNextCommandBufferBarrierCheckedCalls.compare_exchange_weak(
                        remainingBarrierFailures,
                        remainingBarrierFailures - 1u,
                        std::memory_order_relaxed))
                {
                    testCommandBuffer->failBarrierChecked = true;
                    break;
                }
            }
            pool->queueType = queueType;
            std::lock_guard<std::mutex> lock(m_createdCommandPoolsMutex);
            m_createdCommandPools.push_back(pool);
            return pool;
        }
        std::shared_ptr<NLS::Render::RHI::RHIFence> CreateFence(std::string = {}) override
        {
            auto fence = std::make_shared<TestFence>();
            fence->signaled = true;
            return fence;
        }
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

        NLS::Render::RHI::RHIReadbackResult ReadPixelsChecked(
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
            return readPixelsResult;
        }

        NLS::Render::RHI::RHIReadbackResult BeginReadPixels(
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
            return beginReadPixelsResult;
        }

        std::shared_ptr<NLS::Render::RHI::RHITexture> lastReadPixelsTexture;
        NLS::Render::RHI::RHIReadbackResult readPixelsResult {
            NLS::Render::RHI::RHIReadbackStatusCode::Success,
            {}
        };
        NLS::Render::RHI::RHIReadbackResult beginReadPixelsResult {
            NLS::Render::RHI::RHIReadbackStatusCode::Success,
            {},
            std::make_shared<TestCompletionToken>(NLS::Render::RHI::RHICompletionStatus{
                NLS::Render::RHI::RHICompletionStatusCode::Success,
                {}
            })
        };

    private:
        std::shared_ptr<NLS::Render::RHI::RHIAdapter> m_adapter;
        NLS::Render::RHI::RHIDeviceCapabilities m_capabilities {};
        NLS::Render::RHI::NativeRenderDeviceInfo m_nativeDeviceInfo {};
        std::shared_ptr<TestQueue> m_queue;
        std::shared_ptr<TestQueue> m_computeQueue;
        bool m_hasDedicatedComputeQueueOverride = false;
        std::atomic_size_t m_failNextCommandBufferBeginCalls { 0u };
        std::atomic_size_t m_failNextCommandBufferBarrierCheckedCalls { 0u };
        std::atomic_size_t m_failCommandBufferBarrierCheckedAtCommandPoolCall { 0u };
        std::atomic_size_t m_failNextCreatedCommandPools { 0u };
        std::atomic_size_t m_failCreatedCommandPoolAtCall { 0u };
        std::atomic_bool m_failCreatedChildCommandBufferDraws { false };
        std::atomic_size_t m_createdCommandPoolCallCount { 0u };
        mutable std::mutex m_createdCommandPoolsMutex;
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
    snapshot.sceneGameObjectCount = 9u;
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
    EXPECT_EQ(renderFrameInput.sceneGameObjectCount, snapshot.sceneGameObjectCount);
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

TEST(ThreadedRenderingLifecycleTests, ExternalOutputFrameCompletedAfterNewerRhiSubmissionIsDiscarded)
{
    NLS::Render::Context::ThreadedRenderingLifecycle lifecycle(2u);

    NLS::Render::Context::FrameSnapshot oldSnapshot;
    oldSnapshot.frameId = 100u;
    oldSnapshot.targetsSwapchain = false;
    oldSnapshot.hasExternalOutput = true;
    oldSnapshot.externalOutputIdentity = 7u;
    oldSnapshot.externalOutputTextureCount = 1u;

    NLS::Render::Context::FrameSnapshot newSnapshot = oldSnapshot;
    newSnapshot.frameId = 101u;

    size_t oldSlotIndex = std::numeric_limits<size_t>::max();
    size_t newSlotIndex = std::numeric_limits<size_t>::max();
    ASSERT_TRUE(lifecycle.TryPublishFrameSnapshot(oldSnapshot, &oldSlotIndex));
    ASSERT_TRUE(lifecycle.TryPublishFrameSnapshot(newSnapshot, &newSlotIndex));

    ASSERT_TRUE(lifecycle.TryBeginRenderScene(oldSlotIndex));
    ASSERT_TRUE(lifecycle.TryBeginRenderScene(newSlotIndex));

    NLS::Render::Context::RenderScenePackage newPackage;
    newPackage.frameId = newSnapshot.frameId;
    newPackage.targetsSwapchain = false;
    newPackage.externalSceneOutputIdentity = newSnapshot.externalOutputIdentity;
    newPackage.externalSceneOutputTextureCount = 1u;
    ASSERT_TRUE(lifecycle.CompleteRenderScene(newSlotIndex, newPackage));

    size_t rhiSlotIndex = std::numeric_limits<size_t>::max();
    NLS::Render::Context::RenderFrameBuild renderFrameBuild;
    ASSERT_TRUE(lifecycle.TryBeginNextRhiFrameExecution(&rhiSlotIndex, &renderFrameBuild));
    EXPECT_EQ(rhiSlotIndex, newSlotIndex);
    EXPECT_EQ(renderFrameBuild.frameId, newSnapshot.frameId);

    NLS::Render::Context::RenderScenePackage oldPackage;
    oldPackage.frameId = oldSnapshot.frameId;
    oldPackage.targetsSwapchain = false;
    oldPackage.externalSceneOutputIdentity = oldSnapshot.externalOutputIdentity;
    oldPackage.externalSceneOutputTextureCount = 1u;
    ASSERT_TRUE(lifecycle.CompleteRenderScene(oldSlotIndex, oldPackage));

    EXPECT_FALSE(lifecycle.TryBeginNextRhiFrameExecution(&rhiSlotIndex, &renderFrameBuild));

    const auto oldSlot = lifecycle.CopySlot(oldSlotIndex);
    ASSERT_TRUE(oldSlot.has_value());
    EXPECT_EQ(oldSlot->stage, NLS::Render::Context::ThreadedFrameStage::Retired);
}

TEST(ThreadedRenderingLifecycleTests, NewerExternalOutputPublishedFrameRetiresOlderBeforeRenderSceneBuild)
{
    NLS::Render::Context::ThreadedRenderingLifecycle lifecycle(2u);

    NLS::Render::Context::FrameSnapshot oldSnapshot;
    oldSnapshot.frameId = 600u;
    oldSnapshot.targetsSwapchain = false;
    oldSnapshot.hasExternalOutput = true;
    oldSnapshot.externalOutputIdentity = 77u;
    oldSnapshot.externalOutputTextureCount = 1u;

    NLS::Render::Context::FrameSnapshot newSnapshot = oldSnapshot;
    newSnapshot.frameId = 601u;

    size_t oldSlotIndex = std::numeric_limits<size_t>::max();
    size_t newSlotIndex = std::numeric_limits<size_t>::max();
    ASSERT_TRUE(lifecycle.TryPublishFrameSnapshot(oldSnapshot, &oldSlotIndex));
    ASSERT_TRUE(lifecycle.TryPublishFrameSnapshot(newSnapshot, &newSlotIndex));

    size_t renderSlotIndex = std::numeric_limits<size_t>::max();
    NLS::Render::Context::FrameSnapshot claimedSnapshot;
    ASSERT_TRUE(lifecycle.TryBeginNextRenderScene(&renderSlotIndex, &claimedSnapshot));
    EXPECT_EQ(renderSlotIndex, newSlotIndex);
    EXPECT_EQ(claimedSnapshot.frameId, newSnapshot.frameId);

    const auto oldSlot = lifecycle.CopySlot(oldSlotIndex);
    ASSERT_TRUE(oldSlot.has_value());
    EXPECT_EQ(oldSlot->stage, NLS::Render::Context::ThreadedFrameStage::Retired);
}

TEST(ThreadedRenderingLifecycleTests, NewerExternalOutputPublishedFrameRetiresOlderBeforeRenderFrameBuild)
{
    NLS::Render::Context::ThreadedRenderingLifecycle lifecycle(2u);

    NLS::Render::Context::FrameSnapshot oldSnapshot;
    oldSnapshot.frameId = 610u;
    oldSnapshot.targetsSwapchain = false;
    oldSnapshot.hasExternalOutput = true;
    oldSnapshot.externalOutputIdentity = 78u;
    oldSnapshot.externalOutputTextureCount = 1u;

    NLS::Render::Context::FrameSnapshot newSnapshot = oldSnapshot;
    newSnapshot.frameId = 611u;

    size_t oldSlotIndex = std::numeric_limits<size_t>::max();
    size_t newSlotIndex = std::numeric_limits<size_t>::max();
    ASSERT_TRUE(lifecycle.TryPublishFrameSnapshot(oldSnapshot, &oldSlotIndex));
    ASSERT_TRUE(lifecycle.TryPublishFrameSnapshot(newSnapshot, &newSlotIndex));

    size_t buildSlotIndex = std::numeric_limits<size_t>::max();
    NLS::Render::Context::RenderFrameInput renderFrameInput;
    ASSERT_TRUE(lifecycle.TryBeginNextRenderFrameBuild(&buildSlotIndex, &renderFrameInput));
    EXPECT_EQ(buildSlotIndex, newSlotIndex);
    EXPECT_EQ(renderFrameInput.frameId, newSnapshot.frameId);

    const auto oldSlot = lifecycle.CopySlot(oldSlotIndex);
    ASSERT_TRUE(oldSlot.has_value());
    EXPECT_EQ(oldSlot->stage, NLS::Render::Context::ThreadedFrameStage::Retired);
}

TEST(ThreadedRenderingLifecycleTests, NewerExternalOutputPublishedFrameDoesNotRetireIndependentOutput)
{
    NLS::Render::Context::ThreadedRenderingLifecycle lifecycle(2u);

    NLS::Render::Context::FrameSnapshot oldSnapshot;
    oldSnapshot.frameId = 620u;
    oldSnapshot.targetsSwapchain = false;
    oldSnapshot.hasExternalOutput = true;
    oldSnapshot.externalOutputIdentity = 88u;
    oldSnapshot.externalOutputTextureCount = 1u;

    NLS::Render::Context::FrameSnapshot newSnapshot = oldSnapshot;
    newSnapshot.frameId = 621u;
    newSnapshot.externalOutputIdentity = 99u;

    size_t oldSlotIndex = std::numeric_limits<size_t>::max();
    size_t newSlotIndex = std::numeric_limits<size_t>::max();
    ASSERT_TRUE(lifecycle.TryPublishFrameSnapshot(oldSnapshot, &oldSlotIndex));
    ASSERT_TRUE(lifecycle.TryPublishFrameSnapshot(newSnapshot, &newSlotIndex));

    size_t renderSlotIndex = std::numeric_limits<size_t>::max();
    NLS::Render::Context::FrameSnapshot claimedSnapshot;
    ASSERT_TRUE(lifecycle.TryBeginNextRenderScene(&renderSlotIndex, &claimedSnapshot));
    EXPECT_EQ(renderSlotIndex, oldSlotIndex);
    EXPECT_EQ(claimedSnapshot.frameId, oldSnapshot.frameId);

    const auto newSlot = lifecycle.CopySlot(newSlotIndex);
    ASSERT_TRUE(newSlot.has_value());
    EXPECT_EQ(newSlot->stage, NLS::Render::Context::ThreadedFrameStage::Published);
}

TEST(ThreadedRenderingLifecycleTests, ExternalOutputStaleRetirementIsScopedToOutputIdentity)
{
    NLS::Render::Context::ThreadedRenderingLifecycle lifecycle(2u);

    NLS::Render::Context::FrameSnapshot oldSnapshot;
    oldSnapshot.frameId = 200u;
    oldSnapshot.targetsSwapchain = false;
    oldSnapshot.hasExternalOutput = true;
    oldSnapshot.externalOutputIdentity = 11u;
    oldSnapshot.externalOutputTextureCount = 1u;

    NLS::Render::Context::FrameSnapshot newSnapshot = oldSnapshot;
    newSnapshot.frameId = 201u;
    newSnapshot.externalOutputIdentity = 22u;

    size_t oldSlotIndex = std::numeric_limits<size_t>::max();
    size_t newSlotIndex = std::numeric_limits<size_t>::max();
    ASSERT_TRUE(lifecycle.TryPublishFrameSnapshot(oldSnapshot, &oldSlotIndex));
    ASSERT_TRUE(lifecycle.TryPublishFrameSnapshot(newSnapshot, &newSlotIndex));
    ASSERT_TRUE(lifecycle.TryBeginRenderScene(oldSlotIndex));
    ASSERT_TRUE(lifecycle.TryBeginRenderScene(newSlotIndex));

    NLS::Render::Context::RenderScenePackage newPackage;
    newPackage.frameId = newSnapshot.frameId;
    newPackage.targetsSwapchain = false;
    newPackage.externalSceneOutputIdentity = newSnapshot.externalOutputIdentity;
    newPackage.externalSceneOutputTextureCount = 1u;
    ASSERT_TRUE(lifecycle.CompleteRenderScene(newSlotIndex, newPackage));

    size_t rhiSlotIndex = std::numeric_limits<size_t>::max();
    NLS::Render::Context::RenderFrameBuild renderFrameBuild;
    ASSERT_TRUE(lifecycle.TryBeginNextRhiFrameExecution(&rhiSlotIndex, &renderFrameBuild));
    EXPECT_EQ(rhiSlotIndex, newSlotIndex);

    NLS::Render::Context::RenderScenePackage oldPackage;
    oldPackage.frameId = oldSnapshot.frameId;
    oldPackage.targetsSwapchain = false;
    oldPackage.externalSceneOutputIdentity = oldSnapshot.externalOutputIdentity;
    oldPackage.externalSceneOutputTextureCount = 1u;
    ASSERT_TRUE(lifecycle.CompleteRenderScene(oldSlotIndex, oldPackage));

    ASSERT_TRUE(lifecycle.TryBeginNextRhiFrameExecution(&rhiSlotIndex, &renderFrameBuild));
    EXPECT_EQ(rhiSlotIndex, oldSlotIndex);
    EXPECT_EQ(renderFrameBuild.frameId, oldSnapshot.frameId);
}

TEST(ThreadedRenderingLifecycleTests, ExternalOutputStaleRetirementDetectsSharedDepthOutputOverlap)
{
    NLS::Render::Context::ThreadedRenderingLifecycle lifecycle(2u);

    constexpr uint64_t kOldColorIdentity = 101u;
    constexpr uint64_t kNewColorIdentity = 202u;
    constexpr uint64_t kSharedDepthIdentity = 303u;

    NLS::Render::Context::FrameSnapshot oldSnapshot;
    oldSnapshot.frameId = 250u;
    oldSnapshot.targetsSwapchain = false;
    oldSnapshot.hasExternalOutput = true;
    oldSnapshot.externalOutputIdentity = kOldColorIdentity;
    oldSnapshot.externalOutputIdentities = { kOldColorIdentity, kSharedDepthIdentity };
    oldSnapshot.externalOutputTextureCount = 2u;

    NLS::Render::Context::FrameSnapshot newSnapshot = oldSnapshot;
    newSnapshot.frameId = 251u;
    newSnapshot.externalOutputIdentity = kNewColorIdentity;
    newSnapshot.externalOutputIdentities = { kNewColorIdentity, kSharedDepthIdentity };

    size_t oldSlotIndex = std::numeric_limits<size_t>::max();
    size_t newSlotIndex = std::numeric_limits<size_t>::max();
    ASSERT_TRUE(lifecycle.TryPublishFrameSnapshot(oldSnapshot, &oldSlotIndex));
    ASSERT_TRUE(lifecycle.TryPublishFrameSnapshot(newSnapshot, &newSlotIndex));
    ASSERT_TRUE(lifecycle.TryBeginRenderScene(oldSlotIndex));
    ASSERT_TRUE(lifecycle.TryBeginRenderScene(newSlotIndex));

    NLS::Render::Context::RenderScenePackage newPackage;
    newPackage.frameId = newSnapshot.frameId;
    newPackage.targetsSwapchain = false;
    newPackage.externalSceneOutputIdentity = kNewColorIdentity;
    newPackage.externalSceneOutputIdentities = { kNewColorIdentity, kSharedDepthIdentity };
    newPackage.externalSceneOutputTextureCount = 2u;
    ASSERT_TRUE(lifecycle.CompleteRenderScene(newSlotIndex, newPackage));

    size_t rhiSlotIndex = std::numeric_limits<size_t>::max();
    NLS::Render::Context::RenderFrameBuild renderFrameBuild;
    ASSERT_TRUE(lifecycle.TryBeginNextRhiFrameExecution(&rhiSlotIndex, &renderFrameBuild));
    EXPECT_EQ(rhiSlotIndex, newSlotIndex);

    NLS::Render::Context::RenderScenePackage oldPackage;
    oldPackage.frameId = oldSnapshot.frameId;
    oldPackage.targetsSwapchain = false;
    oldPackage.externalSceneOutputIdentity = kOldColorIdentity;
    oldPackage.externalSceneOutputIdentities = { kOldColorIdentity, kSharedDepthIdentity };
    oldPackage.externalSceneOutputTextureCount = 2u;
    ASSERT_TRUE(lifecycle.CompleteRenderScene(oldSlotIndex, oldPackage));

    EXPECT_FALSE(lifecycle.TryBeginNextRhiFrameExecution(&rhiSlotIndex, &renderFrameBuild));
    const auto oldSlot = lifecycle.CopySlot(oldSlotIndex);
    ASSERT_TRUE(oldSlot.has_value());
    EXPECT_EQ(oldSlot->stage, NLS::Render::Context::ThreadedFrameStage::Retired);
}

TEST(ThreadedRenderingLifecycleTests, ExternalOutputOrderingUsesRenderScenePackageAsSubmissionContract)
{
    NLS::Render::Context::ThreadedRenderingLifecycle lifecycle(2u);

    NLS::Render::Context::FrameSnapshot oldSnapshot;
    oldSnapshot.frameId = 300u;
    oldSnapshot.targetsSwapchain = false;
    oldSnapshot.hasExternalOutput = true;
    oldSnapshot.externalOutputIdentity = 33u;
    oldSnapshot.externalOutputTextureCount = 1u;

    NLS::Render::Context::FrameSnapshot nonWritingSnapshot = oldSnapshot;
    nonWritingSnapshot.frameId = 301u;

    size_t oldSlotIndex = std::numeric_limits<size_t>::max();
    size_t nonWritingSlotIndex = std::numeric_limits<size_t>::max();
    ASSERT_TRUE(lifecycle.TryPublishFrameSnapshot(oldSnapshot, &oldSlotIndex));
    ASSERT_TRUE(lifecycle.TryPublishFrameSnapshot(nonWritingSnapshot, &nonWritingSlotIndex));
    ASSERT_TRUE(lifecycle.TryBeginRenderScene(oldSlotIndex));
    ASSERT_TRUE(lifecycle.TryBeginRenderScene(nonWritingSlotIndex));

    NLS::Render::Context::RenderScenePackage nonWritingPackage;
    nonWritingPackage.frameId = nonWritingSnapshot.frameId;
    nonWritingPackage.targetsSwapchain = false;
    ASSERT_TRUE(lifecycle.CompleteRenderScene(nonWritingSlotIndex, nonWritingPackage));

    size_t rhiSlotIndex = std::numeric_limits<size_t>::max();
    NLS::Render::Context::RenderFrameBuild renderFrameBuild;
    ASSERT_TRUE(lifecycle.TryBeginNextRhiFrameExecution(&rhiSlotIndex, &renderFrameBuild));
    EXPECT_EQ(rhiSlotIndex, nonWritingSlotIndex);

    NLS::Render::Context::RenderScenePackage oldPackage;
    oldPackage.frameId = oldSnapshot.frameId;
    oldPackage.targetsSwapchain = false;
    oldPackage.externalSceneOutputIdentity = oldSnapshot.externalOutputIdentity;
    oldPackage.externalSceneOutputTextureCount = 1u;
    ASSERT_TRUE(lifecycle.CompleteRenderScene(oldSlotIndex, oldPackage));

    ASSERT_TRUE(lifecycle.TryBeginNextRhiFrameExecution(&rhiSlotIndex, &renderFrameBuild));
    EXPECT_EQ(rhiSlotIndex, oldSlotIndex);
    EXPECT_EQ(renderFrameBuild.frameId, oldSnapshot.frameId);
}

TEST(ThreadedRenderingLifecycleTests, ExternalOutputOrderingFallsBackToPublishedIdentityWhenPackageOmitsIdentity)
{
    NLS::Render::Context::ThreadedRenderingLifecycle lifecycle(2u);

    NLS::Render::Context::FrameSnapshot oldSnapshot;
    oldSnapshot.frameId = 350u;
    oldSnapshot.targetsSwapchain = false;
    oldSnapshot.hasExternalOutput = true;
    oldSnapshot.externalOutputIdentity = 66u;
    oldSnapshot.externalOutputTextureCount = 1u;

    NLS::Render::Context::FrameSnapshot newSnapshot = oldSnapshot;
    newSnapshot.frameId = 351u;

    size_t oldSlotIndex = std::numeric_limits<size_t>::max();
    size_t newSlotIndex = std::numeric_limits<size_t>::max();
    ASSERT_TRUE(lifecycle.TryPublishFrameSnapshot(oldSnapshot, &oldSlotIndex));
    ASSERT_TRUE(lifecycle.TryPublishFrameSnapshot(newSnapshot, &newSlotIndex));
    ASSERT_TRUE(lifecycle.TryBeginRenderScene(oldSlotIndex));
    ASSERT_TRUE(lifecycle.TryBeginRenderScene(newSlotIndex));

    NLS::Render::Context::RenderScenePackage newPackage;
    newPackage.frameId = newSnapshot.frameId;
    newPackage.targetsSwapchain = false;
    newPackage.externalSceneOutputTextureCount = 1u;
    ASSERT_TRUE(lifecycle.CompleteRenderScene(newSlotIndex, newPackage));

    size_t rhiSlotIndex = std::numeric_limits<size_t>::max();
    NLS::Render::Context::RenderFrameBuild renderFrameBuild;
    ASSERT_TRUE(lifecycle.TryBeginNextRhiFrameExecution(&rhiSlotIndex, &renderFrameBuild));
    EXPECT_EQ(rhiSlotIndex, newSlotIndex);

    NLS::Render::Context::RenderScenePackage oldPackage;
    oldPackage.frameId = oldSnapshot.frameId;
    oldPackage.targetsSwapchain = false;
    oldPackage.externalSceneOutputTextureCount = 1u;
    ASSERT_TRUE(lifecycle.CompleteRenderScene(oldSlotIndex, oldPackage));

    EXPECT_FALSE(lifecycle.TryBeginNextRhiFrameExecution(&rhiSlotIndex, &renderFrameBuild));
    const auto oldSlot = lifecycle.CopySlot(oldSlotIndex);
    ASSERT_TRUE(oldSlot.has_value());
    EXPECT_EQ(oldSlot->stage, NLS::Render::Context::ThreadedFrameStage::Retired);
}

TEST(ThreadedRenderingLifecycleTests, TryBeginNextRhiSubmissionDiscardsStaleExternalOutput)
{
    NLS::Render::Context::ThreadedRenderingLifecycle lifecycle(2u);

    NLS::Render::Context::FrameSnapshot oldSnapshot;
    oldSnapshot.frameId = 400u;
    oldSnapshot.targetsSwapchain = false;
    oldSnapshot.hasExternalOutput = true;
    oldSnapshot.externalOutputIdentity = 44u;
    oldSnapshot.externalOutputTextureCount = 1u;

    NLS::Render::Context::FrameSnapshot newSnapshot = oldSnapshot;
    newSnapshot.frameId = 401u;

    size_t oldSlotIndex = std::numeric_limits<size_t>::max();
    size_t newSlotIndex = std::numeric_limits<size_t>::max();
    ASSERT_TRUE(lifecycle.TryPublishFrameSnapshot(oldSnapshot, &oldSlotIndex));
    ASSERT_TRUE(lifecycle.TryPublishFrameSnapshot(newSnapshot, &newSlotIndex));
    ASSERT_TRUE(lifecycle.TryBeginRenderScene(oldSlotIndex));
    ASSERT_TRUE(lifecycle.TryBeginRenderScene(newSlotIndex));

    NLS::Render::Context::RenderScenePackage newPackage;
    newPackage.frameId = newSnapshot.frameId;
    newPackage.targetsSwapchain = false;
    newPackage.externalSceneOutputIdentity = newSnapshot.externalOutputIdentity;
    newPackage.externalSceneOutputTextureCount = 1u;
    ASSERT_TRUE(lifecycle.CompleteRenderScene(newSlotIndex, newPackage));

    size_t rhiSlotIndex = std::numeric_limits<size_t>::max();
    NLS::Render::Context::RenderScenePackage submittedPackage;
    ASSERT_TRUE(lifecycle.TryBeginNextRhiSubmission(&rhiSlotIndex, &submittedPackage));
    EXPECT_EQ(rhiSlotIndex, newSlotIndex);
    EXPECT_EQ(submittedPackage.frameId, newSnapshot.frameId);

    NLS::Render::Context::RenderScenePackage oldPackage;
    oldPackage.frameId = oldSnapshot.frameId;
    oldPackage.targetsSwapchain = false;
    oldPackage.externalSceneOutputIdentity = oldSnapshot.externalOutputIdentity;
    oldPackage.externalSceneOutputTextureCount = 1u;
    ASSERT_TRUE(lifecycle.CompleteRenderScene(oldSlotIndex, oldPackage));

    EXPECT_FALSE(lifecycle.TryBeginNextRhiSubmission(&rhiSlotIndex, &submittedPackage));
}

TEST(ThreadedRenderingLifecycleTests, TryBeginRhiSubmissionRejectsTargetedStaleExternalOutput)
{
    NLS::Render::Context::ThreadedRenderingLifecycle lifecycle(2u);

    NLS::Render::Context::FrameSnapshot oldSnapshot;
    oldSnapshot.frameId = 500u;
    oldSnapshot.targetsSwapchain = false;
    oldSnapshot.hasExternalOutput = true;
    oldSnapshot.externalOutputIdentity = 55u;
    oldSnapshot.externalOutputTextureCount = 1u;

    NLS::Render::Context::FrameSnapshot newSnapshot = oldSnapshot;
    newSnapshot.frameId = 501u;

    size_t oldSlotIndex = std::numeric_limits<size_t>::max();
    size_t newSlotIndex = std::numeric_limits<size_t>::max();
    ASSERT_TRUE(lifecycle.TryPublishFrameSnapshot(oldSnapshot, &oldSlotIndex));
    ASSERT_TRUE(lifecycle.TryPublishFrameSnapshot(newSnapshot, &newSlotIndex));
    ASSERT_TRUE(lifecycle.TryBeginRenderScene(oldSlotIndex));
    ASSERT_TRUE(lifecycle.TryBeginRenderScene(newSlotIndex));

    NLS::Render::Context::RenderScenePackage newPackage;
    newPackage.frameId = newSnapshot.frameId;
    newPackage.targetsSwapchain = false;
    newPackage.externalSceneOutputIdentity = newSnapshot.externalOutputIdentity;
    newPackage.externalSceneOutputTextureCount = 1u;
    ASSERT_TRUE(lifecycle.CompleteRenderScene(newSlotIndex, newPackage));
    ASSERT_TRUE(lifecycle.TryBeginRhiSubmission(newSlotIndex));

    NLS::Render::Context::RenderScenePackage oldPackage;
    oldPackage.frameId = oldSnapshot.frameId;
    oldPackage.targetsSwapchain = false;
    oldPackage.externalSceneOutputIdentity = oldSnapshot.externalOutputIdentity;
    oldPackage.externalSceneOutputTextureCount = 1u;
    ASSERT_TRUE(lifecycle.CompleteRenderScene(oldSlotIndex, oldPackage));

    EXPECT_FALSE(lifecycle.TryBeginRhiSubmission(oldSlotIndex));
    const auto oldSlot = lifecycle.CopySlot(oldSlotIndex);
    ASSERT_TRUE(oldSlot.has_value());
    EXPECT_EQ(oldSlot->stage, NLS::Render::Context::ThreadedFrameStage::Retired);
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

TEST(ThreadedRenderingLifecycleTests, PreparedBuilderResolveRejectsDuplicateResolutionWhileBuilderIsRunning)
{
    NLS::Render::Context::ThreadedRenderingLifecycle lifecycle(1u);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 34u;

    std::mutex builderMutex;
    std::condition_variable builderStateChanged;
    bool builderEntered = false;
    bool releaseBuilder = false;

    ASSERT_TRUE(lifecycle.TryPublishPreparedFrameBuilder(
        snapshot,
        [&builderMutex, &builderStateChanged, &builderEntered, &releaseBuilder]()
        {
            std::unique_lock<std::mutex> lock(builderMutex);
            builderEntered = true;
            builderStateChanged.notify_all();
            builderStateChanged.wait(
                lock,
                [&releaseBuilder]()
                {
                    return releaseBuilder;
                });

            NLS::Render::Context::RenderScenePackage package;
            package.frameId = 34u;
            package.visibleDrawCount = 2u;
            package.hasVisibleDraws = true;
            return package;
        }));

    size_t slotIndex = 99u;
    NLS::Render::Context::FrameSnapshot claimedSnapshot;
    ASSERT_TRUE(lifecycle.TryBeginNextRenderScene(&slotIndex, &claimedSnapshot));
    EXPECT_EQ(slotIndex, 0u);

    NLS::Render::Context::RenderScenePreparingResolutionDesc resolutionDesc;
    bool firstResolveResult = false;
    std::thread firstResolveThread(
        [&lifecycle, slotIndex, &resolutionDesc, &firstResolveResult]()
        {
            firstResolveResult = lifecycle.ResolveRenderScenePreparing(slotIndex, resolutionDesc);
        });

    {
        std::unique_lock<std::mutex> lock(builderMutex);
        const bool builderWasEntered = builderStateChanged.wait_for(
            lock,
            std::chrono::seconds(2),
            [&builderEntered]()
            {
                return builderEntered;
            });
        if (!builderWasEntered)
        {
            releaseBuilder = true;
            lock.unlock();
            builderStateChanged.notify_all();
            firstResolveThread.join();
        }
        ASSERT_TRUE(builderWasEntered);
    }

    NLS::Render::Context::RenderScenePackage competingPackage;
    competingPackage.frameId = 34u;
    competingPackage.visibleDrawCount = 99u;
    EXPECT_FALSE(lifecycle.CompleteRenderScene(slotIndex, competingPackage));

    bool missingFallbackCalled = false;
    NLS::Render::Context::RenderScenePreparingResolutionDesc duplicateResolutionDesc;
    duplicateResolutionDesc.buildPreparedBuilderMissingRenderScenePackage =
        [&missingFallbackCalled](const NLS::Render::Context::FrameSnapshot& duplicateSnapshot)
        {
            missingFallbackCalled = true;
            NLS::Render::Context::RenderScenePackage package;
            package.frameId = duplicateSnapshot.frameId;
            return package;
        };

    EXPECT_FALSE(lifecycle.ResolveRenderScenePreparing(slotIndex, duplicateResolutionDesc));
    EXPECT_FALSE(missingFallbackCalled);

    {
        std::lock_guard<std::mutex> lock(builderMutex);
        releaseBuilder = true;
    }
    builderStateChanged.notify_all();
    firstResolveThread.join();

    EXPECT_TRUE(firstResolveResult);
    const auto renderReadySlotState = lifecycle.CopySlot(slotIndex);
    ASSERT_TRUE(renderReadySlotState.has_value());
    EXPECT_EQ(renderReadySlotState->stage, NLS::Render::Context::ThreadedFrameStage::RenderReady);
    EXPECT_EQ(
        renderReadySlotState->renderSceneAttribution,
        NLS::Render::Context::RenderSceneAttribution::RendererPrepared);
    ASSERT_TRUE(renderReadySlotState->renderScenePackage.has_value());
    EXPECT_EQ(renderReadySlotState->renderScenePackage->frameId, 34u);
    EXPECT_EQ(renderReadySlotState->renderScenePackage->visibleDrawCount, 2u);
}

TEST(ThreadedRenderingLifecycleTests, PreparedBuilderResolveCompletesMissingPackageWhenBuilderThrows)
{
    NLS::Render::Context::ThreadedRenderingLifecycle lifecycle(1u);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 35u;
    snapshot.renderWidth = 320u;
    snapshot.renderHeight = 180u;

    ASSERT_TRUE(lifecycle.TryPublishPreparedFrameBuilder(
        snapshot,
        []() -> NLS::Render::Context::RenderScenePackage
        {
            throw std::runtime_error("expected prepared builder failure");
        }));

    size_t slotIndex = 99u;
    NLS::Render::Context::FrameSnapshot claimedSnapshot;
    ASSERT_TRUE(lifecycle.TryBeginNextRenderScene(&slotIndex, &claimedSnapshot));
    EXPECT_EQ(slotIndex, 0u);

    bool missingFallbackCalled = false;
    NLS::Render::Context::RenderScenePreparingResolutionDesc resolutionDesc;
    resolutionDesc.buildPreparedBuilderMissingRenderScenePackage =
        [&missingFallbackCalled](const NLS::Render::Context::FrameSnapshot& missingSnapshot)
        {
            missingFallbackCalled = true;
            NLS::Render::Context::RenderScenePackage package;
            package.frameId = missingSnapshot.frameId;
            package.renderWidth = missingSnapshot.renderWidth;
            package.renderHeight = missingSnapshot.renderHeight;
            package.visibleDrawCount = 0u;
            package.hasVisibleDraws = false;
            return package;
        };

    EXPECT_NO_THROW({
        EXPECT_TRUE(lifecycle.ResolveRenderScenePreparing(slotIndex, resolutionDesc));
    });
    EXPECT_TRUE(missingFallbackCalled);

    const auto renderReadySlotState = lifecycle.CopySlot(slotIndex);
    ASSERT_TRUE(renderReadySlotState.has_value());
    EXPECT_EQ(renderReadySlotState->stage, NLS::Render::Context::ThreadedFrameStage::RenderReady);
    EXPECT_EQ(
        renderReadySlotState->renderSceneAttribution,
        NLS::Render::Context::RenderSceneAttribution::PreparedBuilderMissing);
    EXPECT_FALSE(renderReadySlotState->preparedRenderSceneBuilder.has_value());
    ASSERT_TRUE(renderReadySlotState->renderScenePackage.has_value());
    EXPECT_EQ(renderReadySlotState->renderScenePackage->frameId, 35u);
    EXPECT_EQ(renderReadySlotState->renderScenePackage->renderWidth, 320u);
    EXPECT_EQ(renderReadySlotState->renderScenePackage->renderHeight, 180u);
    EXPECT_FALSE(renderReadySlotState->renderScenePackage->hasVisibleDraws);
}

TEST(ThreadedRenderingLifecycleTests, PreparedBuilderResolveCompletesFallbackWhenMissingPackageBuilderThrows)
{
    NLS::Render::Context::ThreadedRenderingLifecycle lifecycle(1u);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 36u;
    snapshot.renderWidth = 640u;
    snapshot.renderHeight = 360u;
    snapshot.targetsSwapchain = false;

    ASSERT_TRUE(lifecycle.TryPublishPreparedFrameBuilder(
        snapshot,
        []() -> NLS::Render::Context::RenderScenePackage
        {
            throw std::runtime_error("expected prepared builder failure");
        }));

    size_t slotIndex = 99u;
    NLS::Render::Context::FrameSnapshot claimedSnapshot;
    ASSERT_TRUE(lifecycle.TryBeginNextRenderScene(&slotIndex, &claimedSnapshot));

    NLS::Render::Context::RenderScenePreparingResolutionDesc resolutionDesc;
    resolutionDesc.buildPreparedBuilderMissingRenderScenePackage =
        [](const NLS::Render::Context::FrameSnapshot&) -> NLS::Render::Context::RenderScenePackage
        {
            throw std::runtime_error("expected missing package failure");
        };

    EXPECT_NO_THROW({
        EXPECT_TRUE(lifecycle.ResolveRenderScenePreparing(slotIndex, resolutionDesc));
    });

    const auto renderReadySlotState = lifecycle.CopySlot(slotIndex);
    ASSERT_TRUE(renderReadySlotState.has_value());
    EXPECT_EQ(renderReadySlotState->stage, NLS::Render::Context::ThreadedFrameStage::RenderReady);
    EXPECT_EQ(
        renderReadySlotState->renderSceneAttribution,
        NLS::Render::Context::RenderSceneAttribution::PreparedBuilderMissing);
    ASSERT_TRUE(renderReadySlotState->renderScenePackage.has_value());
    EXPECT_EQ(renderReadySlotState->renderScenePackage->frameId, 36u);
    EXPECT_EQ(renderReadySlotState->renderScenePackage->renderWidth, 640u);
    EXPECT_EQ(renderReadySlotState->renderScenePackage->renderHeight, 360u);
    EXPECT_FALSE(renderReadySlotState->renderScenePackage->targetsSwapchain);
    EXPECT_EQ(renderReadySlotState->renderScenePackage->renderTargetUseCount, 2u);
}

TEST(ThreadedRenderingLifecycleTests, SnapshotHarnessResolveCompletesFallbackWhenHarnessBuilderThrows)
{
    NLS::Render::Context::ThreadedRenderingLifecycle lifecycle(1u);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 37u;
    snapshot.renderWidth = 800u;
    snapshot.renderHeight = 450u;

    ASSERT_TRUE(lifecycle.TryPublishFrameSnapshot(snapshot));

    size_t slotIndex = 99u;
    NLS::Render::Context::FrameSnapshot claimedSnapshot;
    ASSERT_TRUE(lifecycle.TryBeginNextRenderScene(&slotIndex, &claimedSnapshot));

    NLS::Render::Context::RenderScenePreparingResolutionDesc resolutionDesc;
    resolutionDesc.buildSnapshotHarnessRenderScenePackage =
        [](const NLS::Render::Context::FrameSnapshot&) -> NLS::Render::Context::RenderScenePackage
        {
            throw std::runtime_error("expected snapshot harness failure");
        };

    EXPECT_NO_THROW({
        EXPECT_TRUE(lifecycle.ResolveRenderScenePreparing(slotIndex, resolutionDesc));
    });

    const auto renderReadySlotState = lifecycle.CopySlot(slotIndex);
    ASSERT_TRUE(renderReadySlotState.has_value());
    EXPECT_EQ(renderReadySlotState->stage, NLS::Render::Context::ThreadedFrameStage::RenderReady);
    EXPECT_EQ(
        renderReadySlotState->renderSceneAttribution,
        NLS::Render::Context::RenderSceneAttribution::SnapshotHarness);
    ASSERT_TRUE(renderReadySlotState->renderScenePackage.has_value());
    EXPECT_EQ(renderReadySlotState->renderScenePackage->frameId, 37u);
    EXPECT_EQ(renderReadySlotState->renderScenePackage->renderWidth, 800u);
    EXPECT_EQ(renderReadySlotState->renderScenePackage->renderHeight, 450u);
    EXPECT_TRUE(renderReadySlotState->renderScenePackage->targetsSwapchain);
    EXPECT_EQ(renderReadySlotState->renderScenePackage->renderTargetUseCount, 1u);
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

TEST(ThreadedRenderingLifecycleTests, ReservedReusableSlotIsOnlyConsumedByPreparedPublication)
{
    NLS::Render::Context::ThreadedRenderingLifecycle lifecycle(2u);

    const auto reservedSlot = lifecycle.ReserveReusableSlotIndex();
    ASSERT_TRUE(reservedSlot.has_value());
    EXPECT_EQ(reservedSlot.value(), 0u);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 1u;

    size_t snapshotSlot = 99u;
    ASSERT_TRUE(lifecycle.TryPublishFrameSnapshot(snapshot, &snapshotSlot));
    EXPECT_EQ(snapshotSlot, 1u);

    NLS::Render::Context::FrameSnapshot preparedSnapshot;
    preparedSnapshot.frameId = 2u;
    size_t preparedSlot = 99u;
    ASSERT_TRUE(lifecycle.TryPublishPreparedFrameBuilder(
        preparedSnapshot,
        []()
        {
            NLS::Render::Context::RenderScenePackage package;
            package.frameId = 2u;
            return package;
        },
        &preparedSlot));
    EXPECT_EQ(preparedSlot, 0u);

    NLS::Render::Context::FrameSnapshot backPressuredSnapshot;
    backPressuredSnapshot.frameId = 3u;
    EXPECT_FALSE(lifecycle.TryPublishFrameSnapshot(backPressuredSnapshot));
    EXPECT_EQ(lifecycle.GetBlockedPublishCount(), 1u);
}

TEST(ThreadedRenderingLifecycleTests, ReservedReusableSlotCanWaitForRetiredPreparedSlot)
{
    NLS::Render::Context::ThreadedRenderingLifecycle lifecycle(1u);

    NLS::Render::Context::FrameSnapshot firstSnapshot;
    firstSnapshot.frameId = 1u;

    size_t firstSlot = 99u;
    ASSERT_TRUE(lifecycle.TryPublishPreparedFrameBuilder(
        firstSnapshot,
        []()
        {
            NLS::Render::Context::RenderScenePackage package;
            package.frameId = 1u;
            return package;
        },
        &firstSlot));
    ASSERT_EQ(firstSlot, 0u);

    std::thread retireThread([&lifecycle, firstSlot]()
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));

        NLS::Render::Context::RenderScenePackage scenePackage;
        scenePackage.frameId = 1u;
        NLS::Render::Context::RhiSubmissionFrame submissionFrame;
        submissionFrame.frameId = 1u;

        ASSERT_TRUE(lifecycle.TryBeginRenderScene(firstSlot));
        ASSERT_TRUE(lifecycle.CompleteRenderScene(firstSlot, scenePackage));
        ASSERT_TRUE(lifecycle.TryBeginRhiSubmission(firstSlot));
        ASSERT_TRUE(lifecycle.CompleteRhiSubmission(firstSlot, submissionFrame));
        ASSERT_TRUE(lifecycle.RetireFrame(firstSlot));
    });

    const auto reservedSlot = lifecycle.ReserveReusableSlotIndex(std::chrono::milliseconds(250));
    retireThread.join();

    ASSERT_TRUE(reservedSlot.has_value());
    EXPECT_EQ(reservedSlot.value(), firstSlot);

    NLS::Render::Context::FrameSnapshot secondSnapshot;
    secondSnapshot.frameId = 2u;
    size_t secondSlot = 99u;
    ASSERT_TRUE(lifecycle.TryPublishPreparedFrameBuilder(
        secondSnapshot,
        []()
        {
            NLS::Render::Context::RenderScenePackage package;
            package.frameId = 2u;
            return package;
        },
        &secondSlot));
    EXPECT_EQ(secondSlot, firstSlot);
}

TEST(ThreadedRenderingLifecycleTests, ReservedReusableSlotWaitsAreVisibleInTelemetry)
{
    NLS::Render::Context::ThreadedRenderingLifecycle lifecycle(1u);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 1u;
    ASSERT_TRUE(lifecycle.TryPublishPreparedFrameBuilder(
        snapshot,
        []()
        {
            NLS::Render::Context::RenderScenePackage package;
            package.frameId = 1u;
            return package;
        }));

    const auto reservedSlot = lifecycle.ReserveReusableSlotIndex(std::chrono::milliseconds(1));
    EXPECT_FALSE(reservedSlot.has_value());

    const auto telemetry = lifecycle.GetTelemetry();
    EXPECT_EQ(telemetry.reservedSlotWaitCount, 1u);
    EXPECT_EQ(telemetry.reservedSlotWaitTimeoutCount, 1u);
    EXPECT_GT(telemetry.reservedSlotWaitTotalNs, 0u);
}

TEST(ThreadedRenderingLifecycleTests, ReleasedReusableSlotCanBeUsedBySnapshotPublication)
{
    NLS::Render::Context::ThreadedRenderingLifecycle lifecycle(1u);

    const auto reservedSlot = lifecycle.ReserveReusableSlotIndex();
    ASSERT_TRUE(reservedSlot.has_value());
    EXPECT_EQ(reservedSlot.value(), 0u);
    EXPECT_TRUE(lifecycle.ReleaseReservedReusableSlotIndex(reservedSlot.value()));

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 1u;

    size_t publishedSlot = 99u;
    ASSERT_TRUE(lifecycle.TryPublishFrameSnapshot(snapshot, &publishedSlot));
    EXPECT_EQ(publishedSlot, 0u);
}

TEST(ThreadedRenderingLifecycleTests, RhiSubmissionFrameRetainsCommandListSubmissionMetadata)
{
    NLS::Render::Context::ThreadedRenderingLifecycle lifecycle(1u);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 427u;

    NLS::Render::Context::RenderScenePackage scenePackage;
    scenePackage.frameId = snapshot.frameId;

    NLS::Render::RHI::RHICommandListSubmissionMetadata computeList;
    computeList.debugName = "LightGrid";
    computeList.queueType = NLS::Render::RHI::QueueType::Compute;
    computeList.lifecycleState = NLS::Render::RHI::RHICommandListLifecycleState::Closed;
    computeList.RecordCommand(NLS::Render::RHI::RHICommandListCommandKind::Dispatch);

    NLS::Render::RHI::RHICommandListSubmissionMetadata graphicsList;
    graphicsList.debugName = "BasePass";
    graphicsList.queueType = NLS::Render::RHI::QueueType::Graphics;
    graphicsList.lifecycleState = NLS::Render::RHI::RHICommandListLifecycleState::Closed;
    graphicsList.RecordCommand(NLS::Render::RHI::RHICommandListCommandKind::DrawIndexed, 3u);

    NLS::Render::Context::RhiSubmissionFrame submissionFrame;
    submissionFrame.frameId = snapshot.frameId;
    submissionFrame.commandListSubmission.debugName = "Immediate";
    submissionFrame.commandListSubmission.queueType = NLS::Render::RHI::QueueType::Graphics;
    submissionFrame.commandListSubmission.lifecycleState =
        NLS::Render::RHI::RHICommandListLifecycleState::Recording;
    submissionFrame.commandListSubmission.QueueChildSubmission(computeList);
    submissionFrame.commandListSubmission.QueueChildSubmission(graphicsList);
    submissionFrame.commandListSubmission.Close();

    size_t slotIndex = 0u;
    ASSERT_TRUE(lifecycle.TryPublishFrameSnapshot(snapshot, &slotIndex));
    ASSERT_TRUE(lifecycle.TryBeginRenderScene(slotIndex));
    ASSERT_TRUE(lifecycle.CompleteRenderScene(slotIndex, scenePackage));
    ASSERT_TRUE(lifecycle.TryBeginRhiSubmission(slotIndex));
    ASSERT_TRUE(lifecycle.CompleteRhiSubmission(slotIndex, submissionFrame));

    const auto copiedSlot = lifecycle.CopySlot(slotIndex);
    ASSERT_TRUE(copiedSlot.has_value());
    ASSERT_TRUE(copiedSlot->submissionFrame.has_value());
    const auto& commandListSubmission = copiedSlot->submissionFrame->commandListSubmission;
    EXPECT_EQ(commandListSubmission.debugName, "Immediate");
    EXPECT_EQ(commandListSubmission.lifecycleState, NLS::Render::RHI::RHICommandListLifecycleState::Closed);
    EXPECT_EQ(commandListSubmission.commandCount, 2u);
    EXPECT_EQ(commandListSubmission.visibleDrawCount, 3u);
    ASSERT_EQ(commandListSubmission.childSubmissions.size(), 2u);
    EXPECT_EQ(commandListSubmission.childSubmissions[0].debugName, "LightGrid");
    EXPECT_EQ(commandListSubmission.childSubmissions[0].queueType, NLS::Render::RHI::QueueType::Compute);
    EXPECT_EQ(commandListSubmission.childSubmissions[0].submitOrder, 0u);
    EXPECT_EQ(commandListSubmission.childSubmissions[1].debugName, "BasePass");
    EXPECT_EQ(commandListSubmission.childSubmissions[1].queueType, NLS::Render::RHI::QueueType::Graphics);
    EXPECT_EQ(commandListSubmission.childSubmissions[1].submitOrder, 1u);
}

TEST(ThreadedRenderingLifecycleTests, ParallelDrawCommandBatchMetadataGroupsPreparedWorkByPassRole)
{
    auto opaquePipeline = std::make_shared<TestGraphicsPipeline>("OpaquePipeline");
    auto transparentPipeline = std::make_shared<TestGraphicsPipeline>("TransparentPipeline");
    auto materialBindingSet = std::make_shared<TestBindingSet>("MaterialBindingSet");
    auto mesh = std::make_shared<TestMesh>();

    NLS::Render::Context::RenderPassCommandInput opaquePass;
    opaquePass.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    opaquePass.drawCount = 1u;
    opaquePass.recordedDrawCommands.push_back({ opaquePipeline, nullptr, nullptr, nullptr, materialBindingSet, mesh, 1u });

    NLS::Render::Context::RenderPassCommandInput transparentPass;
    transparentPass.kind = NLS::Render::Context::RenderPassCommandKind::Transparent;
    transparentPass.drawCount = 2u;
    transparentPass.recordedDrawCommands.push_back({ transparentPipeline, nullptr, nullptr, nullptr, materialBindingSet, mesh, 1u });
    transparentPass.recordedDrawCommands.push_back({ transparentPipeline, nullptr, nullptr, nullptr, materialBindingSet, mesh, 1u });

    NLS::Render::Context::ParallelCommandWorkUnit opaqueWorkUnit;
    opaqueWorkUnit.workUnitIndex = 4u;
    opaqueWorkUnit.submissionOrder = 2u;
    opaqueWorkUnit.debugName = "OpaqueBasePass";
    opaqueWorkUnit.commandInput = opaquePass;
    opaqueWorkUnit.eligibleForParallelRecording = true;
    opaqueWorkUnit.eligibleForParallelTranslation = true;

    NLS::Render::Context::ParallelCommandWorkUnit transparentWorkUnit;
    transparentWorkUnit.workUnitIndex = 5u;
    transparentWorkUnit.submissionOrder = 3u;
    transparentWorkUnit.debugName = "TranslucencyPass";
    transparentWorkUnit.commandInput = transparentPass;
    transparentWorkUnit.eligibleForParallelTranslation = true;

    const auto batches = NLS::Render::Context::BuildParallelDrawCommandBatchMetadata({
        opaqueWorkUnit,
        transparentWorkUnit
    });

    ASSERT_EQ(batches.size(), 2u);
    EXPECT_EQ(batches[0].passRole, NLS::Render::Context::ParallelDrawCommandPassRole::Opaque);
    EXPECT_EQ(batches[0].workUnitIndex, 4u);
    EXPECT_EQ(batches[0].submissionOrder, 2u);
    EXPECT_EQ(batches[0].drawCommandCount, 1u);
    EXPECT_TRUE(batches[0].eligibleForParallelRecording);
    EXPECT_TRUE(batches[0].eligibleForParallelTranslation);

    EXPECT_EQ(batches[1].passRole, NLS::Render::Context::ParallelDrawCommandPassRole::Transparent);
    EXPECT_EQ(batches[1].workUnitIndex, 5u);
    EXPECT_EQ(batches[1].submissionOrder, 3u);
    EXPECT_EQ(batches[1].drawCommandCount, 2u);
    EXPECT_FALSE(batches[1].eligibleForParallelRecording);
    EXPECT_TRUE(batches[1].eligibleForParallelTranslation);
}

TEST(ThreadedRenderingLifecycleTests, RhiSubmissionFrameRetainsParallelDrawCommandBatchMetadata)
{
    NLS::Render::Context::RhiSubmissionFrame submissionFrame;
    submissionFrame.frameId = 720u;
    submissionFrame.parallelDrawCommandBatches.push_back({
        NLS::Render::Context::ParallelDrawCommandPassRole::Opaque,
        1u,
        1u,
        "OpaqueBasePass",
        NLS::Render::RHI::QueueType::Graphics,
        3u,
        3u,
        true,
        true,
        {}
    });

    NLS::Render::Context::WorkUnitDependencyEdge dependency;
    dependency.sourceWorkUnitIndex = 0u;
    dependency.targetWorkUnitIndex = 1u;
    dependency.kind = NLS::Render::Context::ThreadedDependencyKind::QueueSynchronization;
    submissionFrame.parallelDrawCommandBatches[0].incomingDependencyEdges.push_back(dependency);

    NLS::Render::Context::ThreadedRenderingLifecycle lifecycle(1u);
    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = submissionFrame.frameId;

    NLS::Render::Context::RenderScenePackage scenePackage;
    scenePackage.frameId = snapshot.frameId;

    size_t slotIndex = 0u;
    ASSERT_TRUE(lifecycle.TryPublishFrameSnapshot(snapshot, &slotIndex));
    ASSERT_TRUE(lifecycle.TryBeginRenderScene(slotIndex));
    ASSERT_TRUE(lifecycle.CompleteRenderScene(slotIndex, scenePackage));
    ASSERT_TRUE(lifecycle.TryBeginRhiSubmission(slotIndex));
    ASSERT_TRUE(lifecycle.CompleteRhiSubmission(slotIndex, submissionFrame));

    const auto copiedSlot = lifecycle.CopySlot(slotIndex);
    ASSERT_TRUE(copiedSlot.has_value());
    ASSERT_TRUE(copiedSlot->submissionFrame.has_value());
    ASSERT_EQ(copiedSlot->submissionFrame->parallelDrawCommandBatches.size(), 1u);
    EXPECT_EQ(
        copiedSlot->submissionFrame->parallelDrawCommandBatches[0].passRole,
        NLS::Render::Context::ParallelDrawCommandPassRole::Opaque);
    EXPECT_EQ(copiedSlot->submissionFrame->parallelDrawCommandBatches[0].drawCommandCount, 3u);
    ASSERT_EQ(copiedSlot->submissionFrame->parallelDrawCommandBatches[0].incomingDependencyEdges.size(), 1u);
    EXPECT_EQ(
        copiedSlot->submissionFrame->parallelDrawCommandBatches[0].incomingDependencyEdges[0].sourceWorkUnitIndex,
        0u);
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

TEST(ThreadedRenderingLifecycleTests, PreparedBuilderPublishReportsActualFrameId)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.renderWidth = 64u;
    snapshot.renderHeight = 64u;

    const auto builder = []()
    {
        NLS::Render::Context::RenderScenePackage package;
        package.visibleDrawCount = 1u;
        package.hasVisibleDraws = true;
        package.frameDataReady = true;
        return package;
    };

    size_t publishedSlot = 99u;
    uint64_t publishedFrameId = 0u;
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryPublishPreparedFrameBuilder(
        driver,
        snapshot,
        builder,
        &publishedSlot,
        &publishedFrameId));

    EXPECT_EQ(publishedSlot, 0u);
    EXPECT_NE(publishedFrameId, 0u);

    const auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);
    const auto* publishedSlotState = lifecycle->PeekSlot(publishedSlot);
    ASSERT_NE(publishedSlotState, nullptr);
    ASSERT_TRUE(publishedSlotState->snapshot.has_value());
    EXPECT_EQ(publishedSlotState->snapshot->frameId, publishedFrameId);
}

TEST(ThreadedRenderingLifecycleTests, PreparedBuilderPublishRejectsAfterDeviceLost)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    auto* impl = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl, nullptr);
    impl->deviceLostDetected.store(true, std::memory_order_release);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.renderWidth = 64u;
    snapshot.renderHeight = 64u;

    bool builderCalled = false;
    const auto builder = [&builderCalled]()
    {
        builderCalled = true;
        return NLS::Render::Context::RenderScenePackage{};
    };

    size_t publishedSlot = 99u;
    uint64_t publishedFrameId = 123u;
    EXPECT_FALSE(NLS::Render::Context::DriverRendererAccess::TryPublishPreparedFrameBuilder(
        driver,
        snapshot,
        builder,
        &publishedSlot,
        &publishedFrameId));

    EXPECT_FALSE(builderCalled);
    EXPECT_EQ(publishedFrameId, 0u);
    const auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);
    EXPECT_EQ(lifecycle->GetPublishedFrameCount(), 0u);
}

TEST(ThreadedRenderingLifecycleTests, PreparedBuilderPublishRejectsAfterDeviceLostWithReservedFrameContextSlot)
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
    (void)NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);

    const auto reservedSlot =
        NLS::Render::Context::DriverRendererAccess::ReserveReusableFrameContextSlotIndexForPreparedPublication(driver);
    ASSERT_TRUE(reservedSlot.has_value());

    auto* impl = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl, nullptr);
    impl->deviceLostDetected.store(true, std::memory_order_release);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.renderWidth = 64u;
    snapshot.renderHeight = 64u;

    uint64_t publishedFrameId = 123u;
    EXPECT_FALSE(NLS::Render::Context::DriverRendererAccess::TryPublishPreparedFrameBuilder(
        driver,
        snapshot,
        []()
        {
            return NLS::Render::Context::RenderScenePackage{};
        },
        nullptr,
        &publishedFrameId));

    EXPECT_EQ(publishedFrameId, 0u);
    const auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);
    EXPECT_EQ(lifecycle->GetPublishedFrameCount(), 0u);
}

TEST(ThreadedRenderingLifecycleTests, PreparedBuilderPublishRejectsDuringUnsafeGpuQuarantine)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    auto* impl = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl, nullptr);
    impl->unsafeGpuWorkQuarantined.store(true, std::memory_order_release);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.renderWidth = 64u;
    snapshot.renderHeight = 64u;

    size_t publishedSlot = 99u;
    uint64_t publishedFrameId = 123u;
    EXPECT_FALSE(NLS::Render::Context::DriverRendererAccess::TryPublishPreparedFrameBuilder(
        driver,
        snapshot,
        []()
        {
            return NLS::Render::Context::RenderScenePackage{};
        },
        &publishedSlot,
        &publishedFrameId));

    EXPECT_EQ(publishedFrameId, 0u);
    const auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);
    EXPECT_EQ(lifecycle->GetPublishedFrameCount(), 0u);
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

TEST(ThreadedRenderingLifecycleTests, FailedRhiSubmissionDoesNotAdvanceSuccessfulRetiredFrameId)
{
    NLS::Render::Context::ThreadedRenderingLifecycle lifecycle(1u);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 73u;

    NLS::Render::Context::RenderScenePackage scenePackage;
    scenePackage.frameId = snapshot.frameId;

    size_t slotIndex = 99u;
    ASSERT_TRUE(lifecycle.TryPublishFrameSnapshot(snapshot, &slotIndex));
    ASSERT_TRUE(lifecycle.TryBeginRenderScene(slotIndex));
    ASSERT_TRUE(lifecycle.CompleteRenderScene(slotIndex, scenePackage));
    ASSERT_TRUE(lifecycle.TryBeginRhiSubmission(slotIndex));

    NLS::Render::Context::RhiSubmissionFrame failedSubmissionFrame;
    failedSubmissionFrame.frameId = snapshot.frameId;
    failedSubmissionFrame.currentFrameQueueOperationFailureCount = 1u;
    failedSubmissionFrame.currentFrameLastQueueOperationFailure = "submit failed";
    failedSubmissionFrame.deviceLostDetected = true;
    ASSERT_TRUE(lifecycle.CompleteRhiSubmission(slotIndex, failedSubmissionFrame));
    ASSERT_TRUE(lifecycle.RetireFrame(slotIndex));

    const auto telemetry = lifecycle.GetTelemetry();
    EXPECT_EQ(telemetry.latestRetiredFrameId, 0u)
        << "Cached render resources may only be reused after a successfully submitted retired frame.";
    EXPECT_EQ(telemetry.latestFailedRetiredFrameId, 73u)
        << "Consumers that cache GPU-produced resources need to reject the exact failed target frame even after a later frame succeeds.";
    EXPECT_TRUE(telemetry.deviceLostDetected);
}

TEST(ThreadedRenderingLifecycleTests, FailedRetiredFrameIdSurvivesLaterSuccessfulRetirement)
{
    NLS::Render::Context::ThreadedRenderingLifecycle lifecycle(1u);

    NLS::Render::Context::FrameSnapshot failedSnapshot;
    failedSnapshot.frameId = 81u;

    NLS::Render::Context::RenderScenePackage failedScenePackage;
    failedScenePackage.frameId = failedSnapshot.frameId;

    size_t slotIndex = 99u;
    ASSERT_TRUE(lifecycle.TryPublishFrameSnapshot(failedSnapshot, &slotIndex));
    ASSERT_TRUE(lifecycle.TryBeginRenderScene(slotIndex));
    ASSERT_TRUE(lifecycle.CompleteRenderScene(slotIndex, failedScenePackage));
    ASSERT_TRUE(lifecycle.TryBeginRhiSubmission(slotIndex));

    NLS::Render::Context::RhiSubmissionFrame failedSubmissionFrame;
    failedSubmissionFrame.frameId = failedSnapshot.frameId;
    failedSubmissionFrame.submittedSuccessfully = false;
    ASSERT_TRUE(lifecycle.CompleteRhiSubmission(slotIndex, failedSubmissionFrame));
    ASSERT_TRUE(lifecycle.RetireFrame(slotIndex));

    auto telemetry = lifecycle.GetTelemetry();
    ASSERT_EQ(telemetry.latestRetiredFrameId, 0u);
    ASSERT_EQ(telemetry.latestFailedRetiredFrameId, failedSnapshot.frameId);

    NLS::Render::Context::FrameSnapshot successfulSnapshot;
    successfulSnapshot.frameId = 82u;

    NLS::Render::Context::RenderScenePackage successfulScenePackage;
    successfulScenePackage.frameId = successfulSnapshot.frameId;

    ASSERT_TRUE(lifecycle.TryPublishFrameSnapshot(successfulSnapshot, &slotIndex));
    ASSERT_TRUE(lifecycle.TryBeginRenderScene(slotIndex));
    ASSERT_TRUE(lifecycle.CompleteRenderScene(slotIndex, successfulScenePackage));
    ASSERT_TRUE(lifecycle.TryBeginRhiSubmission(slotIndex));

    NLS::Render::Context::RhiSubmissionFrame successfulSubmissionFrame;
    successfulSubmissionFrame.frameId = successfulSnapshot.frameId;
    ASSERT_TRUE(lifecycle.CompleteRhiSubmission(slotIndex, successfulSubmissionFrame));
    ASSERT_TRUE(lifecycle.RetireFrame(slotIndex));

    telemetry = lifecycle.GetTelemetry();
    EXPECT_EQ(telemetry.latestRetiredFrameId, successfulSnapshot.frameId);
    EXPECT_EQ(telemetry.latestFailedRetiredFrameId, failedSnapshot.frameId)
        << "A later success must not make a cache target from a failed frame look valid.";
}

TEST(ThreadedRenderingLifecycleTests, UnsubmittedRhiFrameDoesNotAdvanceSuccessfulRetiredFrameId)
{
    NLS::Render::Context::ThreadedRenderingLifecycle lifecycle(1u);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 74u;

    NLS::Render::Context::RenderScenePackage scenePackage;
    scenePackage.frameId = snapshot.frameId;

    size_t slotIndex = 99u;
    ASSERT_TRUE(lifecycle.TryPublishFrameSnapshot(snapshot, &slotIndex));
    ASSERT_TRUE(lifecycle.TryBeginRenderScene(slotIndex));
    ASSERT_TRUE(lifecycle.CompleteRenderScene(slotIndex, scenePackage));
    ASSERT_TRUE(lifecycle.TryBeginRhiSubmission(slotIndex));

    NLS::Render::Context::RhiSubmissionFrame unsubmittedFrame;
    unsubmittedFrame.frameId = snapshot.frameId;
    unsubmittedFrame.submittedSuccessfully = false;
    ASSERT_TRUE(lifecycle.CompleteRhiSubmission(slotIndex, unsubmittedFrame));
    ASSERT_TRUE(lifecycle.RetireFrame(slotIndex));

    const auto telemetry = lifecycle.GetTelemetry();
    EXPECT_EQ(telemetry.latestRetiredFrameId, 0u);
}

TEST(ThreadedRenderingLifecycleTests, TryThreadedFrameTelemetryReturnsEmptyWhenThreadedLifecycleIsDisabled)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    NLS::Core::ServiceLocator::Provide(*driver);

    EXPECT_EQ(NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(*driver), nullptr);
    EXPECT_FALSE(NLS::Render::Context::DriverRendererAccess::TryGetThreadedFrameTelemetry(*driver).has_value());

    const auto telemetry = NLS::Render::Context::DriverRendererAccess::GetThreadedFrameTelemetry(*driver);
    EXPECT_EQ(telemetry.inFlightFrameCount, 0u);
}

TEST(ThreadedRenderingLifecycleTests, ExplicitFrameContextsTrackThreadedLifecycleSlotCount)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.framesInFlight = 2u;
    settings.threadedFrameSlotCount = 3u;

    NLS::Render::Context::Driver driver(settings);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    const auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);
    EXPECT_EQ(lifecycle->GetSlotCount(), 3u);
    EXPECT_EQ(NLS::Render::Context::DriverRendererAccess::GetFrameContextSlotCount(driver), 3u);
    for (size_t slotIndex = 0u; slotIndex < lifecycle->GetSlotCount(); ++slotIndex)
    {
        const auto* frameContext = NLS::Render::Context::DriverTestAccess::PeekFrameContext(driver, slotIndex);
        ASSERT_NE(frameContext, nullptr) << "slotIndex=" << slotIndex;
        EXPECT_EQ(frameContext->frameIndex, slotIndex);
        EXPECT_NE(frameContext->descriptorAllocator, nullptr);
    }
}

TEST(ThreadedRenderingLifecycleTests, ReusableFrameContextReservationWaitsForGpuFenceBeforeObjectDataReuse)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.framesInFlight = 1u;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto frameFence = std::make_shared<TestFence>();
    frameFence->signaled = false;
    frameContext.frameFence = frameFence;

    auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 801u;
    NLS::Render::Context::RenderScenePackage scenePackage;
    scenePackage.frameId = snapshot.frameId;
    NLS::Render::Context::RhiSubmissionFrame submissionFrame;
    submissionFrame.frameId = snapshot.frameId;
    submissionFrame.frameContextIndex = 0u;
    submissionFrame.deferredFrameScopedRetirement = true;

    size_t slotIndex = 99u;
    ASSERT_TRUE(lifecycle->TryPublishPreparedFrame(snapshot, scenePackage, &slotIndex));
    ASSERT_EQ(slotIndex, 0u);
    ASSERT_TRUE(lifecycle->TryBeginRenderScene(slotIndex));
    ASSERT_TRUE(lifecycle->CompleteRenderScene(slotIndex, scenePackage));
    ASSERT_TRUE(lifecycle->TryBeginRhiSubmission(slotIndex));
    ASSERT_TRUE(lifecycle->CompleteRhiSubmission(slotIndex, submissionFrame));
    ASSERT_TRUE(lifecycle->RetireFrame(slotIndex));

    EXPECT_FALSE(NLS::Render::Context::DriverRendererAccess::ReserveReusableFrameContextSlotIndex(driver).has_value())
        << "Prepared object-data slots must not be reused until the matching GPU fence is complete.";
    EXPECT_FALSE(NLS::Render::Context::DriverRendererAccess::GetReservedFrameContextSlotIndex(driver).has_value());

    frameFence->signaled = true;

    const auto safeSlot =
        NLS::Render::Context::DriverRendererAccess::ReserveReusableFrameContextSlotIndex(driver);
    ASSERT_TRUE(safeSlot.has_value());
    EXPECT_EQ(safeSlot.value(), 0u);
    EXPECT_EQ(NLS::Render::Context::DriverRendererAccess::GetReservedFrameContextSlotIndex(driver), safeSlot);
}

TEST(ThreadedRenderingLifecycleTests, PreparedPublicationZeroRetirementWaitDoesNotBlockOnGpuFence)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.framesInFlight = 1u;
    settings.threadedFrameSlotCount = 1u;
    settings.threadedPublishRetirementWaitMs = 0u;

    NLS::Render::Context::Driver driver(settings);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto frameFence = std::make_shared<TestFence>();
    frameFence->signaled = false;
    frameFence->waitResults = { false };
    frameContext.frameFence = frameFence;

    auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 803u;
    NLS::Render::Context::RenderScenePackage scenePackage;
    scenePackage.frameId = snapshot.frameId;
    NLS::Render::Context::RhiSubmissionFrame submissionFrame;
    submissionFrame.frameId = snapshot.frameId;
    submissionFrame.frameContextIndex = 0u;
    submissionFrame.deferredFrameScopedRetirement = true;

    size_t slotIndex = 99u;
    ASSERT_TRUE(lifecycle->TryPublishPreparedFrame(snapshot, scenePackage, &slotIndex));
    ASSERT_EQ(slotIndex, 0u);
    ASSERT_TRUE(lifecycle->TryBeginRenderScene(slotIndex));
    ASSERT_TRUE(lifecycle->CompleteRenderScene(slotIndex, scenePackage));
    ASSERT_TRUE(lifecycle->TryBeginRhiSubmission(slotIndex));
    ASSERT_TRUE(lifecycle->CompleteRhiSubmission(slotIndex, submissionFrame));
    ASSERT_TRUE(lifecycle->RetireFrame(slotIndex));

    EXPECT_FALSE(NLS::Render::Context::DriverRendererAccess::ReserveReusableFrameContextSlotIndexForPreparedPublication(driver).has_value());
    EXPECT_EQ(frameFence->waitCalls, 0u)
        << "A zero publish retirement wait is a non-blocking poll, not an infinite GPU fence wait.";
    EXPECT_FALSE(NLS::Render::Context::DriverRendererAccess::GetReservedFrameContextSlotIndex(driver).has_value());
}

TEST(ThreadedRenderingLifecycleTests, ReusableFrameContextReservationAllowsRetiredNoGpuWorkSlotWithoutFenceSignal)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.framesInFlight = 1u;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto frameFence = std::make_shared<TestFence>();
    frameFence->signaled = false;
    frameContext.frameFence = frameFence;

    auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 802u;
    NLS::Render::Context::RenderScenePackage scenePackage;
    scenePackage.frameId = snapshot.frameId;
    NLS::Render::Context::RhiSubmissionFrame submissionFrame;
    submissionFrame.frameId = snapshot.frameId;
    submissionFrame.frameContextIndex = 0u;
    submissionFrame.deferredFrameScopedRetirement = false;
    submissionFrame.submittedSuccessfully = false;

    size_t slotIndex = 99u;
    ASSERT_TRUE(lifecycle->TryPublishPreparedFrame(snapshot, scenePackage, &slotIndex));
    ASSERT_EQ(slotIndex, 0u);
    ASSERT_TRUE(lifecycle->TryBeginRenderScene(slotIndex));
    ASSERT_TRUE(lifecycle->CompleteRenderScene(slotIndex, scenePackage));
    ASSERT_TRUE(lifecycle->TryBeginRhiSubmission(slotIndex));
    ASSERT_TRUE(lifecycle->CompleteRhiSubmission(slotIndex, submissionFrame));
    ASSERT_TRUE(lifecycle->RetireFrame(slotIndex));

    const auto reusableSlot =
        NLS::Render::Context::DriverRendererAccess::ReserveReusableFrameContextSlotIndex(driver);
    ASSERT_TRUE(reusableSlot.has_value());
    EXPECT_EQ(reusableSlot.value(), 0u);
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
    EXPECT_EQ(retiredSlot->renderScenePackage->sceneGameObjectCount, 7u);
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

TEST(ThreadedRenderingLifecycleTests, ThreadedUiRenderCanProceedWhileOffscreenThreadedFrameIsInFlight)
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
    EXPECT_EQ(commandPool->resetCalls, 1u);
    EXPECT_EQ(commandBuffer->resetCalls, 1u);
    EXPECT_EQ(commandBuffer->beginCalls, 1u);
    EXPECT_EQ(swapchain->acquireCalls, 1u);

    NLS::Render::Context::DriverUIAccess::PresentSwapchain(driver);
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

TEST(ThreadedRenderingLifecycleTests, ThreadedUiRenderWaitsForBriefRhiSubmissionLockContention)
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

    std::mutex lockStateMutex;
    std::condition_variable lockStateChanged;
    bool lockHeld = false;
    bool releaseLockRequested = false;
    bool releaseObserved = false;
    auto releaseLock = std::thread(
        [&]
        {
            NLS::Render::Context::DriverTestAccess::LockThreadedRhiSubmission(driver);
            {
                std::lock_guard lock(lockStateMutex);
                lockHeld = true;
            }
            lockStateChanged.notify_all();
            {
                std::unique_lock lock(lockStateMutex);
                releaseObserved = lockStateChanged.wait_for(
                    lock,
                    std::chrono::milliseconds(200),
                    [&] { return releaseLockRequested; });
            }
            NLS::Render::Context::DriverTestAccess::UnlockThreadedRhiSubmission(driver);
        });
    {
        std::unique_lock lock(lockStateMutex);
        ASSERT_TRUE(lockStateChanged.wait_for(
            lock,
            std::chrono::milliseconds(200),
            [&] { return lockHeld; }));
    }

    {
        std::lock_guard lock(lockStateMutex);
        releaseLockRequested = true;
    }
    lockStateChanged.notify_all();

    EXPECT_TRUE(NLS::Render::Context::DriverUIAccess::PrepareUIRender(driver));
    releaseLock.join();
    EXPECT_TRUE(releaseObserved);

    EXPECT_NE(NLS::Render::Context::DriverRendererAccess::GetActiveExplicitCommandBuffer(driver), nullptr);
    EXPECT_EQ(commandPool->resetCalls, 1u);
    EXPECT_EQ(commandBuffer->resetCalls, 1u);
    EXPECT_EQ(commandBuffer->beginCalls, 1u);
    EXPECT_EQ(swapchain->acquireCalls, 1u);

    NLS::Render::Context::DriverUIAccess::PresentSwapchain(driver);
}

TEST(ThreadedRenderingLifecycleTests, ThreadedRhiWorkerYieldsWhileUiStandaloneFrameIsPending)
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

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 304u;
    snapshot.targetsSwapchain = false;

    NLS::Render::Context::RenderScenePackage renderScenePackage;
    renderScenePackage.frameId = 304u;
    renderScenePackage.targetsSwapchain = false;
    renderScenePackage.renderWidth = 128u;
    renderScenePackage.renderHeight = 72u;
    renderScenePackage.frameDataReady = true;
    renderScenePackage.objectDataReady = true;

    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessPreparedFrame(
        driver,
        snapshot,
        renderScenePackage));
    ASSERT_TRUE(NLS::Render::Context::RenderThreadCoordinator::DrainPendingRenderFrameBuildsSynchronously(driver));

    NLS::Render::Context::DriverTestAccess::SetUiStandaloneFramePending(driver, true);
    EXPECT_FALSE(NLS::Render::Context::RhiThreadCoordinator::TryExecuteNextThreadedSubmission(
        driver,
        NLS::Render::Context::RhiSubmissionAttribution::Worker));

    const auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);
    EXPECT_EQ(lifecycle->GetInFlightDepth(), 1u);
    EXPECT_EQ(commandPool->resetCalls, 0u);
    EXPECT_EQ(commandBuffer->beginCalls, 0u);

    EXPECT_TRUE(NLS::Render::Context::RhiThreadCoordinator::TryExecuteNextThreadedSubmission(
        driver,
        NLS::Render::Context::RhiSubmissionAttribution::SynchronousDrain));
    EXPECT_EQ(lifecycle->GetInFlightDepth(), 0u);
    EXPECT_EQ(commandPool->resetCalls, 1u);
    EXPECT_EQ(commandBuffer->beginCalls, 0u);
}

TEST(ThreadedRenderingLifecycleTests, ThreadedRhiWorkerResumesAfterUiStandaloneFramePendingLeaseExpires)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
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
    snapshot.frameId = 305u;
    snapshot.targetsSwapchain = false;

    NLS::Render::Context::RenderScenePackage renderScenePackage;
    renderScenePackage.frameId = 305u;
    renderScenePackage.targetsSwapchain = false;
    renderScenePackage.renderWidth = 128u;
    renderScenePackage.renderHeight = 72u;
    renderScenePackage.frameDataReady = true;
    renderScenePackage.objectDataReady = true;

    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessPreparedFrame(
        driver,
        snapshot,
        renderScenePackage));
    ASSERT_TRUE(NLS::Render::Context::RenderThreadCoordinator::DrainPendingRenderFrameBuildsSynchronously(driver));

    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryLockThreadedRhiSubmission(driver));
    EXPECT_FALSE(NLS::Render::Context::DriverUIAccess::PrepareUIRender(driver));
    NLS::Render::Context::DriverTestAccess::UnlockThreadedRhiSubmission(driver);

    NLS::Render::Context::DriverTestAccess::SetUiStandaloneFramePending(driver, true);
    EXPECT_FALSE(NLS::Render::Context::RhiThreadCoordinator::TryExecuteNextThreadedSubmission(
        driver,
        NLS::Render::Context::RhiSubmissionAttribution::Worker));

    NLS::Render::Context::DriverTestAccess::ExpireUiStandaloneFramePendingLease(driver);

    EXPECT_TRUE(NLS::Render::Context::RhiThreadCoordinator::TryExecuteNextThreadedSubmission(
        driver,
        NLS::Render::Context::RhiSubmissionAttribution::Worker));

    const auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);
    EXPECT_EQ(lifecycle->GetInFlightDepth(), 0u);
    EXPECT_EQ(commandPool->resetCalls, 1u);
#else
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to expire the UI standalone frame pending lease.";
#endif
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
    NLS::Render::Buffers::Framebuffer outputBuffer(320u, 180u);
    frameDescriptor.outputBuffer = &outputBuffer;

    const auto snapshot = renderer.CaptureFrameSnapshot(frameDescriptor);

    ASSERT_TRUE(snapshot.has_value());
    EXPECT_FALSE(snapshot->targetsSwapchain);
    EXPECT_FALSE(snapshot->hasExternalOutput);
    EXPECT_EQ(snapshot->externalOutputTextureCount, 0u);
}

TEST(ThreadedRenderingLifecycleTests, FrameSnapshotCountsDirectExternalOutputTextures)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    NLS::Core::ServiceLocator::Provide(*driver);
    SnapshotPublishingRenderer renderer(*driver);

    NLS::Render::Entities::Camera camera;
    NLS::Render::RHI::RHITextureDesc colorDesc;
    colorDesc.debugName = "DirectOutputColor";
    colorDesc.extent = { 320u, 180u, 1u };
    auto colorTexture = std::make_shared<TestTexture>(colorDesc);
    NLS::Render::RHI::RHITextureDesc depthDesc = colorDesc;
    depthDesc.debugName = "DirectOutputDepth";
    auto depthTexture = std::make_shared<TestTexture>(depthDesc);

    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 320u;
    frameDescriptor.renderHeight = 180u;
    frameDescriptor.camera = &camera;
    frameDescriptor.outputColorTexture = colorTexture;
    frameDescriptor.outputDepthStencilTexture = depthTexture;

    const auto snapshot = renderer.CaptureFrameSnapshot(frameDescriptor);

    ASSERT_TRUE(snapshot.has_value());
    EXPECT_FALSE(snapshot->targetsSwapchain);
    EXPECT_TRUE(snapshot->hasExternalOutput);
    EXPECT_EQ(snapshot->externalOutputTextureCount, 2u);
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
    EXPECT_EQ(snapshot->sceneGameObjectCount, 1u);
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
    EXPECT_EQ(package.sceneGameObjectCount, 1u);
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

TEST(ThreadedRenderingLifecycleTests, RenderScenePackageMarksLargeAttachmentBackedRecordedDrawPassForInRenderPassChildRecording)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    NLS::Core::ServiceLocator::Provide(*driver);
    SceneSnapshotRenderer renderer(*driver);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 10u;
    snapshot.targetsSwapchain = false;
    snapshot.renderWidth = 256u;
    snapshot.renderHeight = 144u;
    snapshot.visibleOpaqueDrawCount = 2000u;
    snapshot.clearColorBuffer = true;
    snapshot.clearDepthBuffer = true;
    snapshot.clearStencilBuffer = false;
    snapshot.recordedDrawCommands.resize(static_cast<size_t>(snapshot.visibleOpaqueDrawCount));

    const auto package = renderer.CaptureRenderScenePackage(snapshot);

    ASSERT_EQ(package.passCommandInputs.size(), 1u);
    EXPECT_EQ(package.passCommandInputs[0].drawCount, 2000u);
    EXPECT_EQ(package.passCommandInputs[0].recordedDrawCommands.size(), 2000u);

    ASSERT_GT(package.parallelCommandWorkUnits.size(), 1u);
    EXPECT_EQ(package.parallelCommandWorkUnitCount, package.parallelCommandWorkUnits.size());
    EXPECT_TRUE(package.containsParallelCommandWorkUnits);

    uint64_t coveredDrawCount = 0u;
    for (size_t index = 0u; index < package.parallelCommandWorkUnits.size(); ++index)
    {
        const auto& workUnit = package.parallelCommandWorkUnits[index];
        EXPECT_EQ(workUnit.workUnitIndex, index);
        EXPECT_EQ(workUnit.submissionOrder, index);
        EXPECT_EQ(workUnit.sourcePassIndex, 0u);
        EXPECT_EQ(workUnit.sliceIndex, index);
        EXPECT_EQ(workUnit.sliceCount, package.parallelCommandWorkUnits.size());
        EXPECT_EQ(workUnit.recordedDrawBegin, coveredDrawCount);
        EXPECT_TRUE(workUnit.commandInput.recordedDrawCommands.empty());
        EXPECT_EQ(workUnit.commandInput.drawCount, workUnit.recordedDrawCount);
        EXPECT_TRUE(workUnit.requiresOrderedSlicedSubmission);
        EXPECT_TRUE(workUnit.usesInRenderPassChildCommandRecording);
        EXPECT_FALSE(workUnit.eligibleForParallelRecording);
        EXPECT_FALSE(workUnit.eligibleForParallelTranslation);
        coveredDrawCount += workUnit.recordedDrawCount;
    }
    EXPECT_EQ(coveredDrawCount, 2000u);

    ASSERT_EQ(package.parallelDrawCommandBatches.size(), package.parallelCommandWorkUnits.size());
    EXPECT_EQ(package.parallelDrawCommandBatches[0].sourcePassIndex, 0u);
    EXPECT_EQ(package.parallelDrawCommandBatches[0].sliceIndex, 0u);
    EXPECT_TRUE(package.parallelDrawCommandBatches[0].requiresOrderedSlicedSubmission);
    EXPECT_TRUE(package.parallelDrawCommandBatches[0].usesInRenderPassChildCommandRecording);
}

TEST(ThreadedRenderingLifecycleTests, RecordedDrawCommandSlicingDoesNotCopySourceDrawVectorsIntoSlices)
{
    auto opaquePipeline = std::make_shared<TestGraphicsPipeline>("OpaquePipeline");
    auto materialBindingSet = std::make_shared<TestBindingSet>("MaterialBindingSet");
    auto mesh = std::make_shared<TestMesh>();

    NLS::Render::Context::RenderPassCommandInput passInput;
    passInput.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    passInput.drawCount = 2000u;
    passInput.renderWidth = 64u;
    passInput.renderHeight = 64u;
    passInput.usesColorAttachment = true;
    passInput.usesDepthStencilAttachment = true;
    passInput.recordedDrawCommands.reserve(2000u);
    for (uint32_t drawIndex = 0u; drawIndex < 2000u; ++drawIndex)
        passInput.recordedDrawCommands.push_back({ opaquePipeline, nullptr, nullptr, nullptr, materialBindingSet, mesh, 1u });

    const auto workUnits = NLS::Render::Context::BuildRecordedDrawCommandWorkUnitsForPass(
        passInput,
        0u,
        0u);

    ASSERT_GT(workUnits.size(), 1u);
    EXPECT_EQ(passInput.recordedDrawCommands.size(), 2000u);
    uint64_t coveredDrawCount = 0u;
    for (const auto& workUnit : workUnits)
    {
        EXPECT_TRUE(workUnit.commandInput.recordedDrawCommands.empty());
        EXPECT_EQ(workUnit.commandInput.recordedDrawCommands.capacity(), 0u);
        EXPECT_GT(workUnit.recordedDrawCount, 0u);
        coveredDrawCount += workUnit.recordedDrawCount;
    }
    EXPECT_EQ(coveredDrawCount, 2000u);
}

TEST(ThreadedRenderingLifecycleTests, InRenderPassChildCommandRecordingSplitsBundlesByRenderTargetCompatiblePipelineLayout)
{
    NLS::Render::RHI::RHIRenderTargetLayoutDesc rgbaLayout;
    rgbaLayout.colorFormats = { NLS::Render::RHI::TextureFormat::RGBA8 };
    rgbaLayout.depthFormat = NLS::Render::RHI::TextureFormat::Depth24Stencil8;
    rgbaLayout.hasDepth = true;
    rgbaLayout.sampleCount = 1u;

    auto rgba16Layout = rgbaLayout;
    rgba16Layout.colorFormats = { NLS::Render::RHI::TextureFormat::RGBA16F };

    auto rgbaPipeline = std::make_shared<TestGraphicsPipeline>("RgbaPipeline", rgbaLayout);
    auto rgba16Pipeline = std::make_shared<TestGraphicsPipeline>("Rgba16Pipeline", rgba16Layout);
    auto materialBindingSet = std::make_shared<TestBindingSet>("MaterialBindingSet");
    auto mesh = std::make_shared<TestMesh>();

    NLS::Render::Context::RenderPassCommandInput passInput;
    passInput.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    passInput.drawCount = NLS::Render::Context::kRecordedDrawCommandSliceThreshold + 4u;
    passInput.renderWidth = 64u;
    passInput.renderHeight = 64u;
    passInput.usesColorAttachment = true;
    passInput.usesDepthStencilAttachment = true;
    passInput.recordedDrawCommands.reserve(static_cast<size_t>(passInput.drawCount));
    for (uint32_t drawIndex = 0u; drawIndex < passInput.drawCount; ++drawIndex)
    {
        auto pipeline = drawIndex + 2u < NLS::Render::Context::kRecordedDrawCommandSliceThreshold
            ? rgbaPipeline
            : rgba16Pipeline;
        passInput.recordedDrawCommands.push_back({ pipeline, nullptr, nullptr, nullptr, materialBindingSet, mesh, 1u });
    }

    const auto workUnits = NLS::Render::Context::BuildRecordedDrawCommandWorkUnitsForPass(
        passInput,
        0u,
        0u);

    ASSERT_EQ(workUnits.size(), 2u);
    EXPECT_TRUE(workUnits[0].usesInRenderPassChildCommandRecording);
    EXPECT_TRUE(workUnits[1].usesInRenderPassChildCommandRecording);
    EXPECT_EQ(workUnits[0].recordedDrawBegin, 0u);
    EXPECT_EQ(workUnits[0].recordedDrawCount, NLS::Render::Context::kRecordedDrawCommandSliceThreshold - 2u);
    EXPECT_EQ(workUnits[1].recordedDrawBegin, NLS::Render::Context::kRecordedDrawCommandSliceThreshold - 2u);
    EXPECT_EQ(workUnits[1].recordedDrawCount, 6u);
}

TEST(ThreadedRenderingLifecycleTests, InRenderPassChildCommandRecordingRejectsPipelineLayoutMismatchedWithParentPass)
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
    explicitDevice->MutableCapabilities().supportsParallelCommandTranslation = true;
    explicitDevice->MutableCapabilities().supportsInRenderPassChildCommandBuffers = true;
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    commandPool->commandBuffer = commandBuffer;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = std::make_shared<TestFence>();
    frameContext.resourceStateTracker = NLS::Render::RHI::CreateDefaultResourceStateTracker();

    auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 614u;
    snapshot.targetsSwapchain = false;
    snapshot.visibleOpaqueDrawCount = 2000u;
    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessFrameSnapshot(driver, snapshot));

    size_t slotIndex = 0u;
    NLS::Render::Context::FrameSnapshot publishedSnapshot;
    ASSERT_TRUE(lifecycle->TryBeginNextRenderScene(&slotIndex, &publishedSnapshot));

    NLS::Render::RHI::RHIRenderTargetLayoutDesc parentLayout;
    parentLayout.colorFormats = { NLS::Render::RHI::TextureFormat::RGBA8 };
    parentLayout.depthFormat = NLS::Render::RHI::TextureFormat::Depth24Stencil8;
    parentLayout.hasDepth = true;
    parentLayout.sampleCount = 1u;
    auto mismatchedPipelineLayout = parentLayout;
    mismatchedPipelineLayout.colorFormats = { NLS::Render::RHI::TextureFormat::RGBA16F };

    NLS::Render::RHI::RHITextureDesc colorDesc;
    colorDesc.format = NLS::Render::RHI::TextureFormat::RGBA8;
    colorDesc.extent = { 64u, 64u, 1u };
    auto colorTexture = std::make_shared<TestTexture>(colorDesc);
    NLS::Render::RHI::RHITextureViewDesc colorViewDesc;
    colorViewDesc.format = NLS::Render::RHI::TextureFormat::RGBA8;
    auto colorView = std::make_shared<TestTextureView>(colorTexture, colorViewDesc);

    NLS::Render::RHI::RHITextureDesc depthDesc;
    depthDesc.format = NLS::Render::RHI::TextureFormat::Depth24Stencil8;
    depthDesc.extent = { 64u, 64u, 1u };
    auto depthTexture = std::make_shared<TestTexture>(depthDesc);
    NLS::Render::RHI::RHITextureViewDesc depthViewDesc;
    depthViewDesc.format = NLS::Render::RHI::TextureFormat::Depth24Stencil8;
    auto depthView = std::make_shared<TestTextureView>(depthTexture, depthViewDesc);

    auto mismatchedPipeline =
        std::make_shared<TestGraphicsPipeline>("MismatchedPipeline", mismatchedPipelineLayout);
    auto materialBindingSet = std::make_shared<TestBindingSet>("MaterialBindingSet");
    auto mesh = std::make_shared<TestMesh>();

    NLS::Render::Context::RenderPassCommandInput passInput;
    passInput.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    passInput.drawCount = 2000u;
    passInput.requiresFrameData = true;
    passInput.requiresObjectData = true;
    passInput.renderWidth = 64u;
    passInput.renderHeight = 64u;
    passInput.usesColorAttachment = true;
    passInput.usesDepthStencilAttachment = true;
    passInput.colorAttachmentViews.push_back(colorView);
    passInput.depthStencilAttachmentView = depthView;
    passInput.recordedDrawCommands.reserve(2000u);
    for (uint32_t drawIndex = 0u; drawIndex < 2000u; ++drawIndex)
        passInput.recordedDrawCommands.push_back({ mismatchedPipeline, nullptr, nullptr, nullptr, materialBindingSet, mesh, 1u });

    NLS::Render::Context::RenderScenePackage package;
    package.frameId = publishedSnapshot.frameId;
    package.targetsSwapchain = false;
    package.visibleDrawCount = 2000u;
    package.opaqueDrawCount = 2000u;
    package.hasVisibleDraws = true;
    package.frameDataReady = true;
    package.objectDataReady = true;
    package.renderWidth = 64u;
    package.renderHeight = 64u;
    package.containsParallelCommandWorkUnits = true;
    package.passCommandInputs = { passInput };
    package.parallelCommandWorkUnits = NLS::Render::Context::BuildRecordedDrawCommandWorkUnitsForPass(
        passInput,
        0u,
        0u);
    package.parallelCommandWorkUnitCount = static_cast<uint64_t>(package.parallelCommandWorkUnits.size());

    ASSERT_TRUE(lifecycle->CompleteRenderScene(slotIndex, package));
    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    const auto copiedSlot = lifecycle->CopySlot(0u);
    ASSERT_TRUE(copiedSlot.has_value());
    ASSERT_TRUE(copiedSlot->submissionFrame.has_value());
    EXPECT_FALSE(copiedSlot->submissionFrame->submittedSuccessfully);
    EXPECT_GT(copiedSlot->submissionFrame->commandRecordingFailureCount, 0u);
    EXPECT_NE(
        copiedSlot->submissionFrame->lastCommandRecordingFailure.find("render target layout"),
        std::string::npos);
    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 0u);
}

TEST(ThreadedRenderingLifecycleTests, InRenderPassChildCommandRecordingUsesOneParentPassAndChildDrawRanges)
{
    ScopedThreadedRenderingJobSystem jobSystem(2u);
    ASSERT_TRUE(jobSystem.IsInitialized());

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
    explicitDevice->MutableCapabilities().supportsParallelCommandTranslation = true;
    explicitDevice->MutableCapabilities().supportsInRenderPassChildCommandBuffers = true;
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
    snapshot.frameId = 604u;
    snapshot.targetsSwapchain = false;
    snapshot.visibleOpaqueDrawCount = 2000u;
    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessFrameSnapshot(driver, snapshot));

    size_t slotIndex = 0u;
    NLS::Render::Context::FrameSnapshot publishedSnapshot;
    ASSERT_TRUE(lifecycle->TryBeginNextRenderScene(&slotIndex, &publishedSnapshot));

    auto opaquePipeline = std::make_shared<TestGraphicsPipeline>("OpaquePipeline");
    auto materialBindingSet = std::make_shared<TestBindingSet>("MaterialBindingSet");
    auto mesh = std::make_shared<TestMesh>();
    NLS::Render::RHI::RHITextureDesc extractedTextureDesc;
    extractedTextureDesc.debugName = "ExtractedSceneColor";
    extractedTextureDesc.extent = { 64u, 64u, 1u };
    auto extractedTexture = std::make_shared<TestTexture>(extractedTextureDesc);

    NLS::Render::Context::RenderPassCommandInput passInput;
    passInput.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    passInput.drawCount = 2000u;
    passInput.requiresFrameData = true;
    passInput.requiresObjectData = true;
    passInput.targetsSwapchain = false;
    passInput.renderWidth = 64u;
    passInput.renderHeight = 64u;
    passInput.clearColor = true;
    passInput.clearDepth = true;
    passInput.usesColorAttachment = true;
    passInput.usesDepthStencilAttachment = true;
    passInput.recordedDrawCommands.reserve(2000u);
    for (uint32_t drawIndex = 0u; drawIndex < 2000u; ++drawIndex)
    {
        passInput.recordedDrawCommands.push_back({ opaquePipeline, nullptr, nullptr, nullptr, materialBindingSet, mesh, 1u });
    }

    NLS::Render::Context::RenderScenePackage package;
    package.frameId = publishedSnapshot.frameId;
    package.targetsSwapchain = false;
    package.visibleDrawCount = 2000u;
    package.opaqueDrawCount = 2000u;
    package.hasVisibleDraws = true;
    package.frameDataReady = true;
    package.objectDataReady = true;
    package.renderWidth = 64u;
    package.renderHeight = 64u;
    package.containsParallelCommandWorkUnits = true;
    package.passCommandInputs = { passInput };
    package.extractedTextures = { extractedTexture };
    package.parallelCommandWorkUnits = NLS::Render::Context::BuildRecordedDrawCommandWorkUnitsForPass(
        passInput,
        0u,
        0u);
    package.parallelCommandWorkUnitCount = static_cast<uint64_t>(package.parallelCommandWorkUnits.size());

    ASSERT_TRUE(lifecycle->CompleteRenderScene(slotIndex, package));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    const auto copiedSlot = lifecycle->CopySlot(0u);
    ASSERT_TRUE(copiedSlot.has_value());
    ASSERT_TRUE(copiedSlot->submissionFrame.has_value());
    EXPECT_TRUE(copiedSlot->submissionFrame->usedParallelCommandPath);
    EXPECT_EQ(copiedSlot->submissionFrame->recordedPassCount, 1u);
    EXPECT_EQ(copiedSlot->submissionFrame->recordedWorkUnitCount, 4u);
    EXPECT_GT(copiedSlot->submissionFrame->parallelRecordingWorkerCount, 1u);
    EXPECT_LE(copiedSlot->submissionFrame->parallelRecordingWorkerCount, 4u);
    ASSERT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    ASSERT_EQ(explicitDevice->GetTestQueue()->lastSubmitDesc.commandBuffers.size(), 1u);
    const auto submittedBuffer = GetSubmittedTestCommandBuffer(explicitDevice->GetTestQueue());
    ASSERT_NE(submittedBuffer, nullptr);
    EXPECT_EQ(submittedBuffer->beginRenderPassCalls, 1u);
    EXPECT_EQ(submittedBuffer->endRenderPassCalls, 1u);
    EXPECT_EQ(submittedBuffer->executeChildCommandBufferCalls, 4u);
    ASSERT_EQ(submittedBuffer->barrierCalls, 1u);
    ASSERT_EQ(submittedBuffer->barrierHistory.size(), 1u);
    ASSERT_EQ(submittedBuffer->barrierHistory[0].textureBarriers.size(), 1u);
    EXPECT_EQ(submittedBuffer->barrierHistory[0].textureBarriers[0].texture, extractedTexture);
    EXPECT_EQ(
        submittedBuffer->barrierHistory[0].textureBarriers[0].after,
        NLS::Render::RHI::ResourceState::ShaderRead);
    for (const auto& child : submittedBuffer->executedChildCommandBuffers)
    {
        ASSERT_NE(child, nullptr);
        EXPECT_EQ(child->beginRenderPassCalls, 0u);
        EXPECT_EQ(child->endRenderPassCalls, 0u);
        EXPECT_EQ(child->barrierCalls, 0u);
        EXPECT_GT(child->drawIndexedCalls + child->drawCalls, 0u);
    }
}

TEST(ThreadedRenderingLifecycleTests, InRenderPassChildCommandRecordingReusesChildCommandPoolsAcrossFenceConfirmedSlotReuse)
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
    explicitDevice->MutableCapabilities().supportsParallelCommandTranslation = true;
    explicitDevice->MutableCapabilities().supportsInRenderPassChildCommandBuffers = true;
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    auto frameFence = std::make_shared<TestFence>();
    commandPool->commandBuffer = commandBuffer;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = frameFence;
    frameContext.resourceStateTracker = nullptr;

    auto opaquePipeline = std::make_shared<TestGraphicsPipeline>("OpaquePipeline");
    auto materialBindingSet = std::make_shared<TestBindingSet>("MaterialBindingSet");
    auto mesh = std::make_shared<TestMesh>();

    auto buildPackage = [&](const uint64_t frameId)
    {
        NLS::Render::Context::RenderPassCommandInput passInput;
        passInput.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
        passInput.drawCount = 2000u;
        passInput.requiresFrameData = true;
        passInput.requiresObjectData = true;
        passInput.renderWidth = 64u;
        passInput.renderHeight = 64u;
        passInput.clearColor = true;
        passInput.clearDepth = true;
        passInput.usesColorAttachment = true;
        passInput.usesDepthStencilAttachment = true;
        passInput.recordedDrawCommands.reserve(2000u);
        for (uint32_t drawIndex = 0u; drawIndex < 2000u; ++drawIndex)
            passInput.recordedDrawCommands.push_back({ opaquePipeline, nullptr, nullptr, nullptr, materialBindingSet, mesh, 1u });

        NLS::Render::Context::FrameSnapshot snapshot;
        snapshot.frameId = frameId;
        snapshot.targetsSwapchain = false;
        snapshot.visibleOpaqueDrawCount = 2000u;

        NLS::Render::Context::RenderScenePackage package;
        package.frameId = snapshot.frameId;
        package.targetsSwapchain = false;
        package.visibleDrawCount = 2000u;
        package.opaqueDrawCount = 2000u;
        package.hasVisibleDraws = true;
        package.frameDataReady = true;
        package.objectDataReady = true;
        package.renderWidth = 64u;
        package.renderHeight = 64u;
        package.containsParallelCommandWorkUnits = true;
        package.passCommandInputs = { passInput };
        package.parallelCommandWorkUnits = NLS::Render::Context::BuildRecordedDrawCommandWorkUnitsForPass(
            passInput,
            0u,
            0u);
        package.parallelCommandWorkUnitCount = static_cast<uint64_t>(package.parallelCommandWorkUnits.size());
        return std::pair{ snapshot, package };
    };

    for (uint64_t frameId = 609u; frameId <= 610u; ++frameId)
    {
        auto [snapshot, package] = buildPackage(frameId);
        ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessPreparedFrame(
            driver,
            snapshot,
            package));
        NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);
        frameFence->signaled = true;
    }

    EXPECT_EQ(frameContext.childCommandPools.size(), 4u);
    EXPECT_EQ(frameContext.childCommandBuffers.size(), 4u);
    EXPECT_EQ(explicitDevice->GetCreatedCommandPools().size(), 4u);
    for (const auto& childCommandPool : frameContext.childCommandPools)
    {
        ASSERT_NE(childCommandPool, nullptr);
        auto testChildCommandPool = std::dynamic_pointer_cast<TestCommandPool>(childCommandPool);
        ASSERT_NE(testChildCommandPool, nullptr);
        EXPECT_GE(testChildCommandPool->resetCalls, 1u);
    }
    for (const auto& childCommandBuffer : frameContext.childCommandBuffers)
    {
        ASSERT_NE(childCommandBuffer, nullptr);
        auto testChildCommandBuffer = std::dynamic_pointer_cast<TestCommandBuffer>(childCommandBuffer);
        ASSERT_NE(testChildCommandBuffer, nullptr);
        EXPECT_GE(testChildCommandBuffer->resetCalls, 1u);
    }
}

TEST(ThreadedRenderingLifecycleTests, InRenderPassChildCommandRecordingUsesDenseChildResourceSlots)
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
    explicitDevice->MutableCapabilities().supportsParallelCommandTranslation = true;
    explicitDevice->MutableCapabilities().supportsInRenderPassChildCommandBuffers = true;
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    commandPool->commandBuffer = commandBuffer;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = std::make_shared<TestFence>();
    frameContext.resourceStateTracker = nullptr;

    auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 614u;
    snapshot.targetsSwapchain = false;
    snapshot.visibleOpaqueDrawCount = 2000u;
    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessFrameSnapshot(driver, snapshot));

    size_t slotIndex = 0u;
    NLS::Render::Context::FrameSnapshot publishedSnapshot;
    ASSERT_TRUE(lifecycle->TryBeginNextRenderScene(&slotIndex, &publishedSnapshot));

    auto opaquePipeline = std::make_shared<TestGraphicsPipeline>("OpaquePipeline");
    auto materialBindingSet = std::make_shared<TestBindingSet>("MaterialBindingSet");
    auto mesh = std::make_shared<TestMesh>();

    NLS::Render::Context::RenderPassCommandInput passInput;
    passInput.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    passInput.drawCount = 2000u;
    passInput.requiresFrameData = true;
    passInput.requiresObjectData = true;
    passInput.renderWidth = 64u;
    passInput.renderHeight = 64u;
    passInput.clearColor = true;
    passInput.usesColorAttachment = true;
    passInput.recordedDrawCommands.reserve(2000u);
    for (uint32_t drawIndex = 0u; drawIndex < 2000u; ++drawIndex)
        passInput.recordedDrawCommands.push_back({ opaquePipeline, nullptr, nullptr, nullptr, materialBindingSet, mesh, 1u });

    NLS::Render::Context::RenderScenePackage package;
    package.frameId = publishedSnapshot.frameId;
    package.targetsSwapchain = false;
    package.visibleDrawCount = 2000u;
    package.opaqueDrawCount = 2000u;
    package.hasVisibleDraws = true;
    package.frameDataReady = true;
    package.objectDataReady = true;
    package.renderWidth = 64u;
    package.renderHeight = 64u;
    package.containsParallelCommandWorkUnits = true;
    package.passCommandInputs = { passInput };
    package.parallelCommandWorkUnits = NLS::Render::Context::BuildRecordedDrawCommandWorkUnitsForPass(
        passInput,
        0u,
        100u);
    package.parallelCommandWorkUnitCount = static_cast<uint64_t>(package.parallelCommandWorkUnits.size());
    ASSERT_EQ(package.parallelCommandWorkUnits.size(), 4u);
    ASSERT_EQ(package.parallelCommandWorkUnits.front().workUnitIndex, 100u);

    ASSERT_TRUE(lifecycle->CompleteRenderScene(slotIndex, package));
    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    EXPECT_EQ(frameContext.childCommandPools.size(), 4u);
    EXPECT_EQ(frameContext.childCommandBuffers.size(), 4u);
    EXPECT_EQ(explicitDevice->GetCreatedCommandPools().size(), 4u);
    ASSERT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    const auto submittedBuffer = GetSubmittedTestCommandBuffer(explicitDevice->GetTestQueue());
    ASSERT_NE(submittedBuffer, nullptr);
    EXPECT_EQ(submittedBuffer->executeChildCommandBufferCalls, 4u);
}

TEST(ThreadedRenderingLifecycleTests, InRenderPassChildCommandRecordingRetainsChildBuffersWhenFenceWaitTimesOut)
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
    explicitDevice->MutableCapabilities().supportsParallelCommandTranslation = true;
    explicitDevice->MutableCapabilities().supportsInRenderPassChildCommandBuffers = true;
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    auto frameFence = std::make_shared<TestFence>();
    auto descriptorAllocator = std::make_shared<TestDescriptorAllocator>();
    auto uploadContext = std::make_shared<TestUploadContext>();
    commandPool->commandBuffer = commandBuffer;
    frameFence->waitResults = { true, false };
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = frameFence;
    frameContext.descriptorAllocator = descriptorAllocator;
    frameContext.uploadContext = uploadContext;
    frameContext.resourceStateTracker = nullptr;

    auto opaquePipeline = std::make_shared<TestGraphicsPipeline>("OpaquePipeline");
    auto materialBindingSet = std::make_shared<TestBindingSet>("MaterialBindingSet");
    auto mesh = std::make_shared<TestMesh>();

    NLS::Render::Context::RenderPassCommandInput passInput;
    passInput.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    passInput.drawCount = 2000u;
    passInput.requiresFrameData = true;
    passInput.requiresObjectData = true;
    passInput.renderWidth = 64u;
    passInput.renderHeight = 64u;
    passInput.clearColor = true;
    passInput.clearDepth = true;
    passInput.usesColorAttachment = true;
    passInput.usesDepthStencilAttachment = true;
    passInput.recordedDrawCommands.reserve(2000u);
    for (uint32_t drawIndex = 0u; drawIndex < 2000u; ++drawIndex)
    {
        passInput.recordedDrawCommands.push_back({ opaquePipeline, nullptr, nullptr, nullptr, materialBindingSet, mesh, 1u });
    }

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 605u;
    snapshot.targetsSwapchain = false;
    snapshot.visibleOpaqueDrawCount = 2000u;

    NLS::Render::Context::RenderScenePackage package;
    package.frameId = snapshot.frameId;
    package.targetsSwapchain = false;
    package.visibleDrawCount = 2000u;
    package.opaqueDrawCount = 2000u;
    package.hasVisibleDraws = true;
    package.frameDataReady = true;
    package.objectDataReady = true;
    package.renderWidth = 64u;
    package.renderHeight = 64u;
    package.containsParallelCommandWorkUnits = true;
    package.passCommandInputs = { passInput };
    package.parallelCommandWorkUnits = NLS::Render::Context::BuildRecordedDrawCommandWorkUnitsForPass(
        passInput,
        0u,
        0u);
    package.parallelCommandWorkUnitCount = static_cast<uint64_t>(package.parallelCommandWorkUnits.size());

    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessPreparedFrame(
        driver,
        snapshot,
        package));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    const auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);
    const auto* retiredSlot = lifecycle->PeekSlot(0u);
    ASSERT_NE(retiredSlot, nullptr);
    ASSERT_TRUE(retiredSlot->submissionFrame.has_value());
    EXPECT_FALSE(retiredSlot->submissionFrame->retirementFenceWaited);
    EXPECT_EQ(frameFence->waitCalls, 1u);
    EXPECT_EQ(descriptorAllocator->beginFrameCalls, 1u);
    EXPECT_EQ(descriptorAllocator->endFrameCalls, 0u);
    EXPECT_EQ(uploadContext->beginFrameCalls, 1u);
    EXPECT_EQ(uploadContext->endFrameCalls, 0u);
    auto* impl = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl, nullptr);
    ASSERT_EQ(impl->retainedThreadedSubmitResourceKeepAliveByFrameContext.size(), 1u);
    const auto& keepAlive = impl->retainedThreadedSubmitResourceKeepAliveByFrameContext[0u];
    EXPECT_TRUE(ContainsRetainedResource(keepAlive, commandPool));
    EXPECT_TRUE(ContainsRetainedResource(keepAlive, commandBuffer));

    const auto submittedBuffer = GetSubmittedTestCommandBuffer(explicitDevice->GetTestQueue());
    ASSERT_NE(submittedBuffer, nullptr);
    EXPECT_EQ(submittedBuffer->executeChildCommandBufferCalls, 4u);
}

TEST(ThreadedRenderingLifecycleTests, InRenderPassChildCommandRecordingKeepsTimedOutSlotResourcesAcrossOtherSlotReuse)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 2u;
    settings.framesInFlight = 2u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    explicitDevice->MutableCapabilities().supportsParallelCommandRecording = true;
    explicitDevice->MutableCapabilities().supportsParallelCommandTranslation = true;
    explicitDevice->MutableCapabilities().supportsInRenderPassChildCommandBuffers = true;
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    std::array<std::shared_ptr<TestDescriptorAllocator>, 2u> descriptorAllocators;
    std::array<std::shared_ptr<TestUploadContext>, 2u> uploadContexts;
    std::array<std::shared_ptr<TestCommandPool>, 2u> commandPools;
    std::array<std::shared_ptr<TestCommandBuffer>, 2u> commandBuffers;
    auto setupFrameContext = [&](const size_t index, const std::shared_ptr<TestFence>& frameFence)
    {
        auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, index);
        auto commandBuffer = std::make_shared<TestCommandBuffer>();
        auto commandPool = std::make_shared<TestCommandPool>();
        commandPools[index] = commandPool;
        commandBuffers[index] = commandBuffer;
        descriptorAllocators[index] = std::make_shared<TestDescriptorAllocator>();
        uploadContexts[index] = std::make_shared<TestUploadContext>();
        commandPool->commandBuffer = commandBuffer;
        frameContext.commandBuffer = commandBuffer;
        frameContext.commandPool = commandPool;
        frameContext.frameFence = frameFence;
        frameContext.descriptorAllocator = descriptorAllocators[index];
        frameContext.uploadContext = uploadContexts[index];
        frameContext.resourceStateTracker = nullptr;
    };

    auto firstFence = std::make_shared<TestFence>();
    firstFence->waitResults = { true, false };
    auto secondFence = std::make_shared<TestFence>();
    secondFence->waitResults = { true, true };
    setupFrameContext(0u, firstFence);
    setupFrameContext(1u, secondFence);

    auto opaquePipeline = std::make_shared<TestGraphicsPipeline>("OpaquePipeline");
    auto materialBindingSet = std::make_shared<TestBindingSet>("MaterialBindingSet");
    auto mesh = std::make_shared<TestMesh>();

    auto buildPackage = [&](const uint64_t frameId)
    {
        NLS::Render::Context::RenderPassCommandInput passInput;
        passInput.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
        passInput.drawCount = 2000u;
        passInput.requiresFrameData = true;
        passInput.requiresObjectData = true;
        passInput.renderWidth = 64u;
        passInput.renderHeight = 64u;
        passInput.clearColor = true;
        passInput.clearDepth = true;
        passInput.usesColorAttachment = true;
        passInput.usesDepthStencilAttachment = true;
        passInput.recordedDrawCommands.reserve(2000u);
        for (uint32_t drawIndex = 0u; drawIndex < 2000u; ++drawIndex)
        {
            passInput.recordedDrawCommands.push_back({ opaquePipeline, nullptr, nullptr, nullptr, materialBindingSet, mesh, 1u });
        }

        NLS::Render::Context::RenderScenePackage package;
        package.frameId = frameId;
        package.targetsSwapchain = false;
        package.visibleDrawCount = 2000u;
        package.opaqueDrawCount = 2000u;
        package.hasVisibleDraws = true;
        package.frameDataReady = true;
        package.objectDataReady = true;
        package.renderWidth = 64u;
        package.renderHeight = 64u;
        package.containsParallelCommandWorkUnits = true;
        package.passCommandInputs = { passInput };
        package.parallelCommandWorkUnits = NLS::Render::Context::BuildRecordedDrawCommandWorkUnitsForPass(
            passInput,
            0u,
            0u);
        package.parallelCommandWorkUnitCount = static_cast<uint64_t>(package.parallelCommandWorkUnits.size());
        return package;
    };

    for (uint64_t frameId = 606u; frameId <= 607u; ++frameId)
    {
        NLS::Render::Context::FrameSnapshot snapshot;
        snapshot.frameId = frameId;
        snapshot.targetsSwapchain = false;
        snapshot.visibleOpaqueDrawCount = 2000u;
        ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessPreparedFrame(
            driver,
            snapshot,
            buildPackage(frameId)));

        NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);
    }

    EXPECT_EQ(firstFence->waitCalls, 1u);
    EXPECT_EQ(secondFence->waitCalls, 1u);
    EXPECT_EQ(descriptorAllocators[0u]->beginFrameCalls, 1u);
    EXPECT_EQ(descriptorAllocators[0u]->endFrameCalls, 0u);
    EXPECT_EQ(uploadContexts[0u]->beginFrameCalls, 1u);
    EXPECT_EQ(uploadContexts[0u]->endFrameCalls, 0u);
    EXPECT_EQ(descriptorAllocators[1u]->beginFrameCalls, 1u);
    EXPECT_EQ(descriptorAllocators[1u]->endFrameCalls, 0u);
    EXPECT_EQ(uploadContexts[1u]->beginFrameCalls, 1u);
    EXPECT_EQ(uploadContexts[1u]->endFrameCalls, 0u);
    auto* impl = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl, nullptr);
    ASSERT_EQ(impl->retainedThreadedSubmitResourceKeepAliveByFrameContext.size(), 2u);
    const auto& keepAlive = impl->retainedThreadedSubmitResourceKeepAliveByFrameContext[0u];
    EXPECT_TRUE(ContainsRetainedResource(keepAlive, commandPools[0u]));
    EXPECT_TRUE(ContainsRetainedResource(keepAlive, commandBuffers[0u]));
    const auto& secondKeepAlive = impl->retainedThreadedSubmitResourceKeepAliveByFrameContext[1u];
    EXPECT_TRUE(ContainsRetainedResource(secondKeepAlive, commandPools[1u]));
    EXPECT_TRUE(ContainsRetainedResource(secondKeepAlive, commandBuffers[1u]));
}

TEST(ThreadedRenderingLifecycleTests, InRenderPassChildCommandRecordingFallsBackToUnslicedSerialPassWithoutCapability)
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
    explicitDevice->MutableCapabilities().supportsParallelCommandTranslation = true;
    explicitDevice->MutableCapabilities().supportsInRenderPassChildCommandBuffers = false;
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    auto frameFence = std::make_shared<TestFence>();
    commandPool->commandBuffer = commandBuffer;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = frameFence;
    frameContext.resourceStateTracker = nullptr;

    auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 605u;
    snapshot.targetsSwapchain = false;
    snapshot.visibleOpaqueDrawCount = 2000u;
    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessFrameSnapshot(driver, snapshot));

    size_t slotIndex = 0u;
    NLS::Render::Context::FrameSnapshot publishedSnapshot;
    ASSERT_TRUE(lifecycle->TryBeginNextRenderScene(&slotIndex, &publishedSnapshot));

    auto opaquePipeline = std::make_shared<TestGraphicsPipeline>("OpaquePipeline");
    auto materialBindingSet = std::make_shared<TestBindingSet>("MaterialBindingSet");
    auto mesh = std::make_shared<TestMesh>();

    NLS::Render::Context::RenderPassCommandInput passInput;
    passInput.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    passInput.drawCount = 2000u;
    passInput.requiresFrameData = true;
    passInput.requiresObjectData = true;
    passInput.targetsSwapchain = false;
    passInput.renderWidth = 64u;
    passInput.renderHeight = 64u;
    passInput.clearColor = true;
    passInput.usesColorAttachment = true;
    passInput.recordedDrawCommands.reserve(2000u);
    for (uint32_t drawIndex = 0u; drawIndex < 2000u; ++drawIndex)
    {
        passInput.recordedDrawCommands.push_back({ opaquePipeline, nullptr, nullptr, nullptr, materialBindingSet, mesh, 1u });
    }

    NLS::Render::Context::RenderScenePackage package;
    package.frameId = publishedSnapshot.frameId;
    package.targetsSwapchain = false;
    package.visibleDrawCount = 2000u;
    package.opaqueDrawCount = 2000u;
    package.hasVisibleDraws = true;
    package.frameDataReady = true;
    package.objectDataReady = true;
    package.renderWidth = 64u;
    package.renderHeight = 64u;
    package.containsParallelCommandWorkUnits = true;
    package.passCommandInputs = { passInput };
    package.parallelCommandWorkUnits = NLS::Render::Context::BuildRecordedDrawCommandWorkUnitsForPass(
        passInput,
        0u,
        0u);
    package.parallelCommandWorkUnitCount = static_cast<uint64_t>(package.parallelCommandWorkUnits.size());
    ASSERT_GT(package.parallelCommandWorkUnits.size(), 1u);
    ASSERT_TRUE(package.parallelCommandWorkUnits[0].usesInRenderPassChildCommandRecording);

    ASSERT_TRUE(lifecycle->CompleteRenderScene(slotIndex, package));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    const auto copiedSlot = lifecycle->CopySlot(0u);
    ASSERT_TRUE(copiedSlot.has_value());
    ASSERT_TRUE(copiedSlot->submissionFrame.has_value());
    EXPECT_FALSE(copiedSlot->submissionFrame->usedParallelCommandPath);
    EXPECT_TRUE(copiedSlot->submissionFrame->usedSerialCommandPath);
    EXPECT_EQ(copiedSlot->submissionFrame->recordedPassCount, 1u);
    EXPECT_EQ(copiedSlot->submissionFrame->recordedWorkUnitCount, 1u);
    EXPECT_EQ(copiedSlot->submissionFrame->recordedDrawCount, 2000u);
    EXPECT_NE(
        copiedSlot->submissionFrame->parallelFallbackReason.find("in-render-pass child command recording unavailable"),
        std::string::npos);
    ASSERT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    ASSERT_EQ(explicitDevice->GetTestQueue()->lastSubmitDesc.commandBuffers.size(), 1u);
    const auto submittedBuffer = GetSubmittedTestCommandBuffer(explicitDevice->GetTestQueue());
    ASSERT_NE(submittedBuffer, nullptr);
    EXPECT_EQ(submittedBuffer->beginRenderPassCalls, 1u);
    EXPECT_EQ(submittedBuffer->endRenderPassCalls, 1u);
    EXPECT_EQ(submittedBuffer->executeChildCommandBufferCalls, 0u);
    EXPECT_EQ(submittedBuffer->drawIndexedCalls, 2000u);
}

TEST(ThreadedRenderingLifecycleTests, InRenderPassChildCapabilityFallbackPreservesAttachmentFreeSlices)
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
    explicitDevice->MutableCapabilities().supportsParallelCommandTranslation = true;
    explicitDevice->MutableCapabilities().supportsInRenderPassChildCommandBuffers = false;
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    commandPool->commandBuffer = commandBuffer;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = std::make_shared<TestFence>();
    frameContext.resourceStateTracker = nullptr;

    auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 615u;
    snapshot.targetsSwapchain = false;
    snapshot.visibleOpaqueDrawCount = 4000u;
    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessFrameSnapshot(driver, snapshot));

    size_t slotIndex = 0u;
    NLS::Render::Context::FrameSnapshot publishedSnapshot;
    ASSERT_TRUE(lifecycle->TryBeginNextRenderScene(&slotIndex, &publishedSnapshot));

    auto graphicsPipeline = std::make_shared<TestGraphicsPipeline>("OpaquePipeline");
    auto materialBindingSet = std::make_shared<TestBindingSet>("MaterialBindingSet");
    auto mesh = std::make_shared<TestMesh>();

    NLS::Render::Context::RenderPassCommandInput attachmentFreePassInput;
    attachmentFreePassInput.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    attachmentFreePassInput.debugName = "AttachmentFree";
    attachmentFreePassInput.drawCount = 2000u;
    attachmentFreePassInput.requiresFrameData = true;
    attachmentFreePassInput.requiresObjectData = true;
    attachmentFreePassInput.renderWidth = 64u;
    attachmentFreePassInput.renderHeight = 64u;
    attachmentFreePassInput.recordedDrawCommands.reserve(2000u);
    for (uint32_t drawIndex = 0u; drawIndex < 2000u; ++drawIndex)
        attachmentFreePassInput.recordedDrawCommands.push_back({ graphicsPipeline, nullptr, nullptr, nullptr, materialBindingSet, mesh, 1u });

    NLS::Render::Context::RenderPassCommandInput attachmentBackedPassInput = attachmentFreePassInput;
    attachmentBackedPassInput.debugName = "AttachmentBacked";
    attachmentBackedPassInput.clearColor = true;
    attachmentBackedPassInput.usesColorAttachment = true;

    auto attachmentFreeWorkUnits = NLS::Render::Context::BuildRecordedDrawCommandWorkUnitsForPass(
        attachmentFreePassInput,
        0u,
        0u);
    auto childWorkUnits = NLS::Render::Context::BuildRecordedDrawCommandWorkUnitsForPass(
        attachmentBackedPassInput,
        1u,
        static_cast<uint64_t>(attachmentFreeWorkUnits.size()));
    ASSERT_EQ(attachmentFreeWorkUnits.size(), 4u);
    ASSERT_FALSE(attachmentFreeWorkUnits[0].usesInRenderPassChildCommandRecording);
    ASSERT_EQ(childWorkUnits.size(), 4u);
    ASSERT_TRUE(childWorkUnits[0].usesInRenderPassChildCommandRecording);

    NLS::Render::Context::RenderScenePackage package;
    package.frameId = publishedSnapshot.frameId;
    package.targetsSwapchain = false;
    package.visibleDrawCount = 4000u;
    package.opaqueDrawCount = 4000u;
    package.hasVisibleDraws = true;
    package.frameDataReady = true;
    package.objectDataReady = true;
    package.renderWidth = 64u;
    package.renderHeight = 64u;
    package.containsParallelCommandWorkUnits = true;
    package.passCommandInputs = { attachmentFreePassInput, attachmentBackedPassInput };
    package.parallelCommandWorkUnits = attachmentFreeWorkUnits;
    package.parallelCommandWorkUnits.insert(
        package.parallelCommandWorkUnits.end(),
        childWorkUnits.begin(),
        childWorkUnits.end());
    package.parallelCommandWorkUnitCount = static_cast<uint64_t>(package.parallelCommandWorkUnits.size());

    ASSERT_TRUE(lifecycle->CompleteRenderScene(slotIndex, package));
    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    const auto copiedSlot = lifecycle->CopySlot(0u);
    ASSERT_TRUE(copiedSlot.has_value());
    ASSERT_TRUE(copiedSlot->submissionFrame.has_value());
    EXPECT_TRUE(copiedSlot->submissionFrame->submittedSuccessfully);
    EXPECT_EQ(copiedSlot->submissionFrame->recordedDrawCount, 4000u);
    EXPECT_EQ(copiedSlot->submissionFrame->recordedWorkUnitCount, 5u);
    EXPECT_NE(
        copiedSlot->submissionFrame->parallelFallbackReason.find("in-render-pass child command recording unavailable"),
        std::string::npos);
    ASSERT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    EXPECT_GT(explicitDevice->GetTestQueue()->lastSubmitDesc.commandBuffers.size(), 1u);
    size_t submittedDrawCount = 0u;
    size_t submittedChildExecuteCount = 0u;
    for (const auto& submitDesc : explicitDevice->GetTestQueue()->submitHistory)
    {
        for (const auto& submittedCommandBuffer : submitDesc.commandBuffers)
        {
            const auto testCommandBuffer = std::dynamic_pointer_cast<TestCommandBuffer>(submittedCommandBuffer);
            if (testCommandBuffer == nullptr)
                continue;

            submittedDrawCount += testCommandBuffer->drawIndexedCalls + testCommandBuffer->drawCalls;
            submittedChildExecuteCount += testCommandBuffer->executeChildCommandBufferCalls;
        }
    }
    EXPECT_EQ(submittedDrawCount, 4000u);
    EXPECT_EQ(submittedChildExecuteCount, 0u);
}

TEST(ThreadedRenderingLifecycleTests, InRenderPassChildCommandRecordingReleasesDeferredFrameScopedResourcesOnResizeAfterFence)
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
    explicitDevice->MutableCapabilities().supportsParallelCommandTranslation = true;
    explicitDevice->MutableCapabilities().supportsInRenderPassChildCommandBuffers = true;
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);
    NLS::Render::Context::DriverTestAccess::SetExplicitSwapchain(driver, std::make_shared<TestSwapchain>());

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    auto frameFence = std::make_shared<TestFence>();
    auto descriptorAllocator = std::make_shared<TestDescriptorAllocator>();
    auto uploadContext = std::make_shared<TestUploadContext>();
    auto resourceStateTracker = std::make_shared<TestResourceStateTracker>();
    commandPool->commandBuffer = commandBuffer;
    frameFence->waitResults = { true, false, true };
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = frameFence;
    frameContext.descriptorAllocator = descriptorAllocator;
    frameContext.uploadContext = uploadContext;
    frameContext.resourceStateTracker = resourceStateTracker;

    auto opaquePipeline = std::make_shared<TestGraphicsPipeline>("OpaquePipeline");
    auto materialBindingSet = std::make_shared<TestBindingSet>("MaterialBindingSet");
    auto mesh = std::make_shared<TestMesh>();

    NLS::Render::Context::RenderPassCommandInput passInput;
    passInput.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    passInput.drawCount = 2000u;
    passInput.requiresFrameData = true;
    passInput.requiresObjectData = true;
    passInput.renderWidth = 64u;
    passInput.renderHeight = 64u;
    passInput.clearColor = true;
    passInput.clearDepth = true;
    passInput.usesColorAttachment = true;
    passInput.usesDepthStencilAttachment = true;
    passInput.recordedDrawCommands.reserve(2000u);
    for (uint32_t drawIndex = 0u; drawIndex < 2000u; ++drawIndex)
        passInput.recordedDrawCommands.push_back({ opaquePipeline, nullptr, nullptr, nullptr, materialBindingSet, mesh, 1u });

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 608u;
    snapshot.targetsSwapchain = false;
    snapshot.visibleOpaqueDrawCount = 2000u;

    NLS::Render::Context::RenderScenePackage package;
    package.frameId = snapshot.frameId;
    package.targetsSwapchain = false;
    package.visibleDrawCount = 2000u;
    package.opaqueDrawCount = 2000u;
    package.hasVisibleDraws = true;
    package.frameDataReady = true;
    package.objectDataReady = true;
    package.renderWidth = 64u;
    package.renderHeight = 64u;
    package.containsParallelCommandWorkUnits = true;
    package.passCommandInputs = { passInput };
    package.parallelCommandWorkUnits = NLS::Render::Context::BuildRecordedDrawCommandWorkUnitsForPass(
        passInput,
        0u,
        0u);
    package.parallelCommandWorkUnitCount = static_cast<uint64_t>(package.parallelCommandWorkUnits.size());

    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessPreparedFrame(
        driver,
        snapshot,
        package));
    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    EXPECT_EQ(descriptorAllocator->endFrameCalls, 0u);
    EXPECT_EQ(uploadContext->endFrameCalls, 0u);
    EXPECT_EQ(resourceStateTracker->retireTransientResourcesCalls, 1u);
    auto* impl = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl, nullptr);
    ASSERT_EQ(impl->retainedThreadedSubmitResourceKeepAliveByFrameContext.size(), 1u);
    {
        const auto& keepAlive = impl->retainedThreadedSubmitResourceKeepAliveByFrameContext[0u];
        EXPECT_TRUE(ContainsRetainedResource(keepAlive, commandPool));
        EXPECT_TRUE(ContainsRetainedResource(keepAlive, commandBuffer));
    }

    driver.ResizePlatformSwapchain(128u, 96u);
    NLS::Render::Context::DriverTestAccess::AgePendingSwapchainResize(
        driver,
        NLS::Render::Context::GetInteractiveSwapchainResizeDebounce() + std::chrono::milliseconds(1));
    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryDrainThreadedRendering(driver));

    EXPECT_GE(frameFence->waitCalls, 1u);
    EXPECT_EQ(descriptorAllocator->endFrameCalls, 1u);
    EXPECT_EQ(uploadContext->endFrameCalls, 1u);
    EXPECT_GE(resourceStateTracker->retireTransientResourcesCalls, 2u);
    EXPECT_EQ(NLS::Render::Context::DriverTestAccess::GetRetainedThreadedSubmitResourceCount(driver), 0u);
}

TEST(ThreadedRenderingLifecycleTests, InRenderPassChildCommandRecordingReleasesDeferredResourcesWithTimedOutSlotFrameIndex)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 2u;
    settings.framesInFlight = 2u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    explicitDevice->MutableCapabilities().supportsParallelCommandRecording = true;
    explicitDevice->MutableCapabilities().supportsParallelCommandTranslation = true;
    explicitDevice->MutableCapabilities().supportsInRenderPassChildCommandBuffers = true;
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);
    NLS::Render::Context::DriverTestAccess::SetExplicitSwapchain(driver, std::make_shared<TestSwapchain>());

    std::array<std::shared_ptr<TestCommandPool>, 2u> commandPools;
    std::array<std::shared_ptr<TestCommandBuffer>, 2u> commandBuffers;
    auto setupFrameContext = [&](const size_t index, const std::shared_ptr<TestFence>& frameFence)
    {
        auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, index);
        auto commandPool = std::make_shared<TestCommandPool>();
        commandPool->commandBuffer = std::make_shared<TestCommandBuffer>();
        commandPools[index] = commandPool;
        commandBuffers[index] = std::static_pointer_cast<TestCommandBuffer>(commandPool->commandBuffer);
        frameContext.commandPool = commandPool;
        frameContext.commandBuffer = commandPool->commandBuffer;
        frameContext.frameFence = frameFence;
        frameContext.descriptorAllocator = std::make_shared<TestDescriptorAllocator>();
        frameContext.uploadContext = std::make_shared<TestUploadContext>();
        frameContext.resourceStateTracker = std::make_shared<TestResourceStateTracker>();
    };

    auto firstFence = std::make_shared<TestFence>();
    firstFence->waitResults = { true, true };
    auto secondFence = std::make_shared<TestFence>();
    secondFence->waitResults = { true, false, true };
    setupFrameContext(0u, firstFence);
    setupFrameContext(1u, secondFence);

    auto opaquePipeline = std::make_shared<TestGraphicsPipeline>("OpaquePipeline");
    auto materialBindingSet = std::make_shared<TestBindingSet>("MaterialBindingSet");
    auto mesh = std::make_shared<TestMesh>();

    NLS::Render::Context::RenderPassCommandInput passInput;
    passInput.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    passInput.drawCount = 2000u;
    passInput.renderWidth = 64u;
    passInput.renderHeight = 64u;
    passInput.usesColorAttachment = true;
    passInput.usesDepthStencilAttachment = true;
    passInput.recordedDrawCommands.reserve(2000u);
    for (uint32_t drawIndex = 0u; drawIndex < 2000u; ++drawIndex)
        passInput.recordedDrawCommands.push_back({ opaquePipeline, nullptr, nullptr, nullptr, materialBindingSet, mesh, 1u });

    for (uint64_t frameId = 701u; frameId <= 702u; ++frameId)
    {
        NLS::Render::Context::FrameSnapshot snapshot;
        snapshot.frameId = frameId;
        snapshot.targetsSwapchain = false;
        snapshot.visibleOpaqueDrawCount = 2000u;

        NLS::Render::Context::RenderScenePackage package;
        package.frameId = frameId;
        package.targetsSwapchain = false;
        package.visibleDrawCount = 2000u;
        package.opaqueDrawCount = 2000u;
        package.hasVisibleDraws = true;
        package.frameDataReady = true;
        package.objectDataReady = true;
        package.renderWidth = 64u;
        package.renderHeight = 64u;
        package.containsParallelCommandWorkUnits = true;
        package.passCommandInputs = { passInput };
        package.parallelCommandWorkUnits = NLS::Render::Context::BuildRecordedDrawCommandWorkUnitsForPass(
            passInput,
            0u,
            0u);
        package.parallelCommandWorkUnitCount = static_cast<uint64_t>(package.parallelCommandWorkUnits.size());

        ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessPreparedFrame(
            driver,
            snapshot,
            package));
        NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);
    }

    auto* impl = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl, nullptr);
    ASSERT_EQ(impl->retainedThreadedSubmitResourceKeepAliveByFrameContext.size(), 2u);
    {
        const auto& keepAlive = impl->retainedThreadedSubmitResourceKeepAliveByFrameContext[1u];
        EXPECT_TRUE(ContainsRetainedResource(keepAlive, commandPools[1u]));
        EXPECT_TRUE(ContainsRetainedResource(keepAlive, commandBuffers[1u]));
    }

    driver.ResizePlatformSwapchain(128u, 96u);
    NLS::Render::Context::DriverTestAccess::AgePendingSwapchainResize(
        driver,
        NLS::Render::Context::GetInteractiveSwapchainResizeDebounce() + std::chrono::milliseconds(1));
    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryDrainThreadedRendering(driver));

    const auto* secondFrameContext = NLS::Render::Context::DriverTestAccess::PeekFrameContext(driver, 1u);
    ASSERT_NE(secondFrameContext, nullptr);
    const auto secondDescriptorAllocator = std::dynamic_pointer_cast<TestDescriptorAllocator>(
        secondFrameContext->descriptorAllocator);
    const auto secondUploadContext = std::dynamic_pointer_cast<TestUploadContext>(
        secondFrameContext->uploadContext);
    ASSERT_NE(secondDescriptorAllocator, nullptr);
    ASSERT_NE(secondUploadContext, nullptr);
    EXPECT_EQ(secondDescriptorAllocator->lastEndFrameIndex, 1u);
    EXPECT_EQ(secondUploadContext->lastEndFrameIndex, 1u);
}

TEST(ThreadedRenderingLifecycleTests, DeferredThreadedFrameScopedRetirementUsesSlotIndexAndCompletedFrameIndex)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 2u;
    settings.framesInFlight = 2u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 1u);
    frameContext.frameIndex = 3u;
    auto descriptorAllocator = std::make_shared<TestDescriptorAllocator>();
    auto uploadContext = std::make_shared<TestUploadContext>();
    auto resourceStateTracker = std::make_shared<TestResourceStateTracker>();
    frameContext.descriptorAllocator = descriptorAllocator;
    frameContext.uploadContext = uploadContext;
    frameContext.resourceStateTracker = resourceStateTracker;

    auto* impl = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl, nullptr);
    impl->retainedThreadedSubmitResourceKeepAliveByFrameContext.resize(2u);
    impl->retainedThreadedSubmitResourceKeepAliveByFrameContext[1u].push_back(
        std::make_shared<int>(42));
    impl->deferredThreadedFrameScopedRetirementFrameContexts.insert(1u);

    EXPECT_TRUE(NLS::Render::Context::Detail::ReleaseDeferredThreadedFrameScopedResourcesAfterFence(
        *impl,
        frameContext,
        1u));

    EXPECT_EQ(descriptorAllocator->endFrameCalls, 1u);
    EXPECT_EQ(uploadContext->endFrameCalls, 1u);
    EXPECT_EQ(descriptorAllocator->lastEndFrameIndex, 3u);
    EXPECT_EQ(uploadContext->lastEndFrameIndex, 3u);
    EXPECT_EQ(resourceStateTracker->retireTransientResourcesCalls, 1u);
    EXPECT_TRUE(impl->retainedThreadedSubmitResourceKeepAliveByFrameContext[1u].empty());
    EXPECT_TRUE(impl->deferredThreadedFrameScopedRetirementFrameContexts.empty());
}

TEST(ThreadedRenderingLifecycleTests, SubmitFailureAfterQueuedGpuWorkRetainsResourcesByFrameContextSlot)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 2u;
    settings.framesInFlight = 2u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto* impl = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl, nullptr);
    impl->frameContexts.resize(2u);

    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    explicitDevice->GetTestQueue()->nextSubmitResult = {
        NLS::Render::RHI::RHIQueueOperationStatusCode::BackendFailure,
        "simulated post ExecuteCommandLists signal failure",
        true,
        true
    };
    impl->explicitDevice = explicitDevice;

    auto& frameContext = impl->frameContexts[1u];
    frameContext.frameIndex = 3u;
    frameContext.commandPool = std::make_shared<TestCommandPool>();
    frameContext.commandBuffer = std::make_shared<TestCommandBuffer>();
    auto frameFence = std::make_shared<TestFence>();
    frameFence->waitResults = { false };
    frameContext.frameFence = frameFence;
    auto descriptorAllocator = std::make_shared<TestDescriptorAllocator>();
    auto uploadContext = std::make_shared<TestUploadContext>();
    frameContext.descriptorAllocator = descriptorAllocator;
    frameContext.uploadContext = uploadContext;

    NLS::Render::Context::Detail::AsyncComputeSubmitPlan submitPlan;
    NLS::Render::Context::Detail::ThreadedQueueSubmissionBatch batch;
    batch.queueType = NLS::Render::RHI::QueueType::Graphics;
    batch.commandPools.push_back(frameContext.commandPool);
    batch.commandBuffers.push_back(frameContext.commandBuffer);
    submitPlan.batches.push_back(std::move(batch));

    NLS::Render::Context::RenderScenePackage package;
    package.targetsSwapchain = false;
    NLS::Render::Context::RhiSubmissionFrame submissionFrame;

    NLS::Render::Context::Detail::ExecuteThreadedSubmitPlan(
        *impl,
        frameContext,
        package,
        submitPlan,
        &submissionFrame,
        1u);

    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    EXPECT_FALSE(submissionFrame.submittedSuccessfully);
    EXPECT_EQ(descriptorAllocator->endFrameCalls, 0u);
    EXPECT_EQ(uploadContext->endFrameCalls, 0u);
    ASSERT_EQ(impl->retainedThreadedSubmitResourceKeepAliveByFrameContext.size(), 2u);
    EXPECT_TRUE(impl->retainedThreadedSubmitResourceKeepAliveByFrameContext[0u].empty());
    const auto& keepAlive = impl->retainedThreadedSubmitResourceKeepAliveByFrameContext[1u];
    EXPECT_TRUE(ContainsRetainedResource(keepAlive, frameContext.commandPool));
    EXPECT_TRUE(ContainsRetainedResource(keepAlive, frameContext.commandBuffer));
    EXPECT_EQ(impl->deferredThreadedFrameScopedRetirementFrameContexts.count(1u), 1u);
    EXPECT_EQ(impl->deferredThreadedFrameScopedRetirementFrameContexts.count(3u), 0u);
}

TEST(ThreadedRenderingLifecycleTests, DeviceLostAfterQueuedGpuWorkDoesNotWaitOnUnsignalableFrameFence)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto* impl = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl, nullptr);
    impl->frameContexts.resize(1u);

    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    explicitDevice->GetTestQueue()->nextSubmitResult = {
        NLS::Render::RHI::RHIQueueOperationStatusCode::DeviceLost,
        "simulated device lost after ExecuteCommandLists",
        true
    };
    impl->explicitDevice = explicitDevice;

    auto& frameContext = impl->frameContexts[0u];
    frameContext.frameIndex = 0u;
    frameContext.commandPool = std::make_shared<TestCommandPool>();
    frameContext.commandBuffer = std::make_shared<TestCommandBuffer>();
    auto frameFence = std::make_shared<TestFence>();
    frameFence->waitResults = { false };
    frameContext.frameFence = frameFence;
    auto descriptorAllocator = std::make_shared<TestDescriptorAllocator>();
    auto uploadContext = std::make_shared<TestUploadContext>();
    frameContext.descriptorAllocator = descriptorAllocator;
    frameContext.uploadContext = uploadContext;

    NLS::Render::Context::Detail::AsyncComputeSubmitPlan submitPlan;
    NLS::Render::Context::Detail::ThreadedQueueSubmissionBatch batch;
    batch.queueType = NLS::Render::RHI::QueueType::Graphics;
    batch.commandPools.push_back(frameContext.commandPool);
    batch.commandBuffers.push_back(frameContext.commandBuffer);
    submitPlan.batches.push_back(std::move(batch));

    NLS::Render::Context::RenderScenePackage package;
    package.targetsSwapchain = false;
    NLS::Render::Context::RhiSubmissionFrame submissionFrame;

    NLS::Render::Context::Detail::ExecuteThreadedSubmitPlan(
        *impl,
        frameContext,
        package,
        submitPlan,
        &submissionFrame,
        0u);

    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    EXPECT_FALSE(submissionFrame.submittedSuccessfully);
    EXPECT_FALSE(submissionFrame.retirementFenceWaited);
    EXPECT_TRUE(submissionFrame.deviceLostDetected);
    EXPECT_EQ(frameFence->waitCalls, 0u);
    EXPECT_EQ(descriptorAllocator->endFrameCalls, 0u);
    EXPECT_EQ(uploadContext->endFrameCalls, 0u);
    EXPECT_EQ(impl->deferredThreadedFrameScopedRetirementFrameContexts.count(0u), 0u);
    ASSERT_EQ(impl->retainedThreadedSubmitResourceKeepAliveByFrameContext.size(), 1u);
    const auto& keepAlive = impl->retainedThreadedSubmitResourceKeepAliveByFrameContext[0u];
    EXPECT_TRUE(ContainsRetainedResource(keepAlive, frameContext.commandPool));
    EXPECT_TRUE(ContainsRetainedResource(keepAlive, frameContext.commandBuffer));
}

TEST(ThreadedRenderingLifecycleTests, PostExecuteSignalFailureWithoutFrameFenceSignalDoesNotWaitButRetainsSlot)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto* impl = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl, nullptr);
    impl->frameContexts.resize(1u);

    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    explicitDevice->GetTestQueue()->nextSubmitResult = {
        NLS::Render::RHI::RHIQueueOperationStatusCode::BackendFailure,
        "simulated semaphore signal failure after ExecuteCommandLists",
        true,
        false
    };
    impl->explicitDevice = explicitDevice;

    auto& frameContext = impl->frameContexts[0u];
    frameContext.frameIndex = 0u;
    frameContext.commandPool = std::make_shared<TestCommandPool>();
    frameContext.commandBuffer = std::make_shared<TestCommandBuffer>();
    auto frameFence = std::make_shared<TestFence>();
    frameFence->waitResults = { false };
    frameContext.frameFence = frameFence;
    auto descriptorAllocator = std::make_shared<TestDescriptorAllocator>();
    auto uploadContext = std::make_shared<TestUploadContext>();
    frameContext.descriptorAllocator = descriptorAllocator;
    frameContext.uploadContext = uploadContext;

    NLS::Render::Context::Detail::AsyncComputeSubmitPlan submitPlan;
    NLS::Render::Context::Detail::ThreadedQueueSubmissionBatch batch;
    batch.queueType = NLS::Render::RHI::QueueType::Graphics;
    batch.commandPools.push_back(frameContext.commandPool);
    batch.commandBuffers.push_back(frameContext.commandBuffer);
    submitPlan.batches.push_back(std::move(batch));

    NLS::Render::Context::RenderScenePackage package;
    package.targetsSwapchain = false;
    NLS::Render::Context::RhiSubmissionFrame submissionFrame;

    NLS::Render::Context::Detail::ExecuteThreadedSubmitPlan(
        *impl,
        frameContext,
        package,
        submitPlan,
        &submissionFrame,
        0u);

    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    EXPECT_FALSE(submissionFrame.submittedSuccessfully);
    EXPECT_FALSE(submissionFrame.retirementFenceWaited);
    EXPECT_EQ(frameFence->waitCalls, 0u);
    EXPECT_EQ(descriptorAllocator->endFrameCalls, 0u);
    EXPECT_EQ(uploadContext->endFrameCalls, 0u);
    EXPECT_EQ(impl->deferredThreadedFrameScopedRetirementFrameContexts.count(0u), 0u);
    ASSERT_EQ(impl->retainedThreadedSubmitResourceKeepAliveByFrameContext.size(), 1u);
    const auto& keepAlive = impl->retainedThreadedSubmitResourceKeepAliveByFrameContext[0u];
    EXPECT_TRUE(ContainsRetainedResource(keepAlive, frameContext.commandPool));
    EXPECT_TRUE(ContainsRetainedResource(keepAlive, frameContext.commandBuffer));
}

TEST(ThreadedRenderingLifecycleTests, SuccessfulSubmitWithoutReportedFrameFenceSignalQuarantinesResources)
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
    auto commandPool = std::make_shared<TestCommandPool>();
    commandPool->commandBuffer = std::make_shared<TestCommandBuffer>();
    auto frameFence = std::make_shared<TestFence>();
    frameContext.commandPool = commandPool;
    frameContext.commandBuffer = commandPool->commandBuffer;
    frameContext.frameFence = frameFence;
    explicitDevice->GetTestQueue()->autoReportFrameFenceSignalOnSuccess = false;

    NLS::Render::Context::RenderScenePackage package;
    package.frameId = 710u;
    package.targetsSwapchain = false;

    NLS::Render::Context::Detail::AsyncComputeSubmitPlan submitPlan;
    NLS::Render::Context::Detail::ThreadedQueueSubmissionBatch batch;
    batch.queueType = NLS::Render::RHI::QueueType::Graphics;
    batch.commandPools.push_back(commandPool);
    batch.commandBuffers.push_back(commandPool->commandBuffer);
    submitPlan.batches.push_back(std::move(batch));

    NLS::Render::Context::RhiSubmissionFrame submissionFrame;
    NLS::Render::Context::Detail::ExecuteThreadedSubmitPlan(
        *NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver),
        frameContext,
        package,
        submitPlan,
        &submissionFrame,
        0u);

    auto* impl = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl, nullptr);
    EXPECT_FALSE(impl->deviceLostDetected.load(std::memory_order_acquire));
    EXPECT_TRUE(impl->unsafeGpuWorkQuarantined.load(std::memory_order_acquire));
    EXPECT_FALSE(impl->unsafeGpuWorkQuarantineReason.empty());
    EXPECT_EQ(impl->deferredThreadedFrameScopedRetirementFrameContexts.count(0u), 0u);
    ASSERT_EQ(impl->retainedThreadedSubmitResourceKeepAliveByFrameContext.size(), 1u);
    const auto& keepAlive = impl->retainedThreadedSubmitResourceKeepAliveByFrameContext[0u];
    EXPECT_TRUE(ContainsRetainedResource(keepAlive, commandPool));
    EXPECT_TRUE(ContainsRetainedResource(keepAlive, commandPool->commandBuffer));

    EXPECT_FALSE(submissionFrame.deferredFrameScopedRetirement);
    EXPECT_FALSE(submissionFrame.deviceLostDetected);

    NLS::Core::ServiceLocator::Remove<NLS::Render::Context::Driver>();
}

TEST(ThreadedRenderingLifecycleTests, ThreadedFrameWithoutFrameFenceQuarantinesAndRejectsFurtherReuse)
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
    auto commandPool = std::make_shared<TestCommandPool>();
    commandPool->commandBuffer = std::make_shared<TestCommandBuffer>();
    frameContext.commandPool = commandPool;
    frameContext.commandBuffer = commandPool->commandBuffer;
    frameContext.frameFence.reset();

    NLS::Render::Context::RenderScenePackage firstPackage;
    firstPackage.frameId = 711u;
    firstPackage.targetsSwapchain = false;

    NLS::Render::Context::Detail::AsyncComputeSubmitPlan submitPlan;
    NLS::Render::Context::Detail::ThreadedQueueSubmissionBatch batch;
    batch.queueType = NLS::Render::RHI::QueueType::Graphics;
    batch.commandPools.push_back(commandPool);
    batch.commandBuffers.push_back(commandPool->commandBuffer);
    submitPlan.batches.push_back(std::move(batch));

    NLS::Render::Context::RhiSubmissionFrame firstSubmissionFrame;
    NLS::Render::Context::Detail::ExecuteThreadedSubmitPlan(
        *NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver),
        frameContext,
        firstPackage,
        submitPlan,
        &firstSubmissionFrame,
        0u);

    auto* impl = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl, nullptr);
    EXPECT_FALSE(impl->deviceLostDetected.load(std::memory_order_acquire));
    EXPECT_TRUE(impl->unsafeGpuWorkQuarantined.load(std::memory_order_acquire));
    EXPECT_FALSE(impl->unsafeGpuWorkQuarantineReason.empty());
    EXPECT_EQ(impl->deferredThreadedFrameScopedRetirementFrameContexts.count(0u), 0u);
    ASSERT_EQ(impl->retainedThreadedSubmitResourceKeepAliveByFrameContext.size(), 1u);
    EXPECT_FALSE(impl->retainedThreadedSubmitResourceKeepAliveByFrameContext[0u].empty());
    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);

    NLS::Render::Context::RenderScenePackage secondPackage = firstPackage;
    secondPackage.frameId = 712u;
    const auto secondSubmissionFrame =
        NLS::Render::Context::Detail::SubmitThreadedRhiFrame(*impl, secondPackage, 0u);

    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    EXPECT_FALSE(secondSubmissionFrame.submittedSuccessfully);

    NLS::Core::ServiceLocator::Remove<NLS::Render::Context::Driver>();
}

TEST(ThreadedRenderingLifecycleTests, ThreadedSubmissionUsesClaimedLifecycleSlotFrameContext)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 2u;
    settings.framesInFlight = 2u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& firstFrameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto firstCommandPool = std::make_shared<TestCommandPool>();
    auto firstCommandBuffer = std::make_shared<TestCommandBuffer>();
    firstCommandPool->commandBuffer = firstCommandBuffer;
    auto firstFence = std::make_shared<TestFence>();
    firstFrameContext.commandPool = firstCommandPool;
    firstFrameContext.commandBuffer = firstCommandPool->commandBuffer;
    firstFrameContext.frameFence = firstFence;

    auto& secondFrameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 1u);
    auto secondCommandPool = std::make_shared<TestCommandPool>();
    auto secondCommandBuffer = std::make_shared<TestCommandBuffer>();
    secondCommandPool->commandBuffer = secondCommandBuffer;
    auto secondFence = std::make_shared<TestFence>();
    secondFrameContext.commandPool = secondCommandPool;
    secondFrameContext.commandBuffer = secondCommandPool->commandBuffer;
    secondFrameContext.frameFence = secondFence;

    auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);

    NLS::Render::Context::FrameSnapshot olderSnapshot;
    olderSnapshot.frameId = 901u;
    olderSnapshot.visibleOpaqueDrawCount = 1u;
    NLS::Render::Context::RenderScenePackage olderPackage;
    olderPackage.frameId = olderSnapshot.frameId;
    olderPackage.targetsSwapchain = false;
    size_t firstSlot = 99u;
    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessPreparedFrame(
        driver,
        olderSnapshot,
        olderPackage,
        &firstSlot));
    ASSERT_EQ(firstSlot, 0u);
    ASSERT_TRUE(NLS::Render::Context::RenderThreadCoordinator::DrainPendingRenderFrameBuildsSynchronously(driver));
    ASSERT_TRUE(lifecycle->TryBeginRhiSubmission(firstSlot));

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 902u;
    snapshot.visibleOpaqueDrawCount = 1u;
    NLS::Render::Context::RenderScenePackage package;
    package.frameId = snapshot.frameId;
    package.targetsSwapchain = false;
    size_t publishedSlot = 99u;
    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessPreparedFrame(
        driver,
        snapshot,
        package,
        &publishedSlot));
    ASSERT_EQ(publishedSlot, 1u);
    ASSERT_TRUE(NLS::Render::Context::RenderThreadCoordinator::DrainPendingRenderFrameBuildsSynchronously(driver));

    ASSERT_TRUE(NLS::Render::Context::RhiThreadCoordinator::TryExecuteNextThreadedSubmission(
        driver,
        NLS::Render::Context::RhiSubmissionAttribution::SynchronousDrain));

    EXPECT_EQ(firstCommandPool->resetCalls, 0u);
    EXPECT_EQ(firstCommandBuffer->resetCalls, 0u);
    EXPECT_EQ(firstFence->waitCalls, 0u);
    EXPECT_EQ(secondCommandPool->resetCalls, 1u);
    EXPECT_EQ(secondCommandBuffer->resetCalls, 1u);
    EXPECT_EQ(secondFence->waitCalls, 1u);

    const auto secondSlot = lifecycle->CopySlot(1u);
    ASSERT_TRUE(secondSlot.has_value());
    ASSERT_TRUE(secondSlot->submissionFrame.has_value());
    EXPECT_EQ(secondSlot->submissionFrame->frameContextIndex, 1u);

    NLS::Render::Context::RhiSubmissionFrame firstSubmissionFrame;
    firstSubmissionFrame.frameId = olderPackage.frameId;
    firstSubmissionFrame.submittedSuccessfully = false;
    ASSERT_TRUE(lifecycle->CompleteRhiSubmission(firstSlot, firstSubmissionFrame));
    ASSERT_TRUE(lifecycle->RetireFrame(firstSlot));

    NLS::Core::ServiceLocator::Remove<NLS::Render::Context::Driver>();
}

TEST(ThreadedRenderingLifecycleTests, DeviceLostShutdownAbandonsFenceWaitAndReleasesDeferredFrameScopedResources)
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

    auto* impl = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl, nullptr);
    impl->deviceLostDetected.store(true, std::memory_order_release);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    auto frameFence = std::make_shared<TestFence>();
    auto descriptorAllocator = std::make_shared<TestDescriptorAllocator>();
    auto uploadContext = std::make_shared<TestUploadContext>();
    commandPool->commandBuffer = commandBuffer;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = frameFence;
    frameContext.descriptorAllocator = descriptorAllocator;
    frameContext.uploadContext = uploadContext;

    impl->retainedThreadedSubmitResourceKeepAliveByFrameContext.resize(1u);
    impl->retainedThreadedSubmitResourceKeepAliveByFrameContext[0u].push_back(commandBuffer);
    impl->deferredThreadedFrameScopedRetirementFrameContexts.insert(0u);

    NLS::Render::Context::DriverTestAccess::ShutdownRhiResourcesForTesting(driver);

    EXPECT_EQ(frameFence->waitCalls, 0u);
    EXPECT_EQ(descriptorAllocator->endFrameCalls, 0u);
    EXPECT_EQ(uploadContext->endFrameCalls, 0u);
    EXPECT_TRUE(impl->retainedThreadedSubmitResourceKeepAliveByFrameContext.empty());
    EXPECT_TRUE(impl->deferredThreadedFrameScopedRetirementFrameContexts.empty());
    EXPECT_TRUE(impl->frameContexts.empty());
    EXPECT_EQ(NLS::Render::Context::DriverRendererAccess::GetExplicitDevice(driver), nullptr);
}

TEST(ThreadedRenderingLifecycleTests, UnsafeGpuWorkQuarantineShutdownPreservesRetainedResources)
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

    auto* impl = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl, nullptr);
    impl->unsafeGpuWorkQuarantined.store(true, std::memory_order_release);
    impl->unsafeGpuWorkQuarantineReason = "test quarantine";

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    auto frameFence = std::make_shared<TestFence>();
    commandPool->commandBuffer = commandBuffer;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = frameFence;

    impl->retainedThreadedSubmitResourceKeepAliveByFrameContext.resize(1u);
    impl->retainedThreadedSubmitResourceKeepAliveByFrameContext[0u].push_back(commandBuffer);

    NLS::Render::Context::DriverTestAccess::ShutdownRhiResourcesForTesting(driver);

    EXPECT_EQ(frameFence->waitCalls, 0u);
    ASSERT_EQ(impl->retainedThreadedSubmitResourceKeepAliveByFrameContext.size(), 1u);
    EXPECT_FALSE(impl->retainedThreadedSubmitResourceKeepAliveByFrameContext[0u].empty());
    EXPECT_EQ(NLS::Render::Context::DriverRendererAccess::GetExplicitDevice(driver), explicitDevice);
}

TEST(ThreadedRenderingLifecycleTests, UnsafeGpuWorkQuarantineDriverDestructionPreservesRetainedResources)
{
    std::weak_ptr<TestExplicitDevice> weakDevice;
    std::weak_ptr<TestCommandBuffer> weakCommandBuffer;

    {
        NLS::Render::Settings::DriverSettings settings;
        settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
        settings.enableExplicitRHI = false;
        settings.enableThreadedRendering = true;
        settings.threadedFrameSlotCount = 1u;
        settings.framesInFlight = 1u;

        NLS::Render::Context::Driver driver(settings);
        auto explicitDevice = std::make_shared<TestExplicitDevice>();
        weakDevice = explicitDevice;
        NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

        auto* impl = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver);
        ASSERT_NE(impl, nullptr);
        impl->unsafeGpuWorkQuarantined.store(true, std::memory_order_release);
        impl->unsafeGpuWorkQuarantineReason = "test quarantine";

        auto commandBuffer = std::make_shared<TestCommandBuffer>();
        weakCommandBuffer = commandBuffer;
        impl->retainedThreadedSubmitResourceKeepAliveByFrameContext.resize(1u);
        impl->retainedThreadedSubmitResourceKeepAliveByFrameContext[0u].push_back(commandBuffer);
    }

    EXPECT_FALSE(weakDevice.expired());
    EXPECT_FALSE(weakCommandBuffer.expired());
}

TEST(ThreadedRenderingLifecycleTests, StandaloneExplicitSubmitFailureWithoutFrameFenceSignalDoesNotRegisterReusableRetirement)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = false;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    auto frameFence = std::make_shared<TestFence>();
    auto descriptorAllocator = std::make_shared<TestDescriptorAllocator>();
    auto uploadContext = std::make_shared<TestUploadContext>();
    commandPool->commandBuffer = commandBuffer;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = frameFence;
    frameContext.descriptorAllocator = descriptorAllocator;
    frameContext.uploadContext = uploadContext;
    explicitDevice->GetTestQueue()->nextSubmitResult = {
        NLS::Render::RHI::RHIQueueOperationStatusCode::BackendFailure,
        "simulated standalone signal failure",
        true
    };

    auto* impl = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl, nullptr);

    NLS::Render::Context::DriverTestAccess::BeginStandaloneExplicitFrame(driver, false);
    NLS::Render::Context::DriverTestAccess::EndStandaloneExplicitFrame(driver, false);

    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    EXPECT_EQ(descriptorAllocator->endFrameCalls, 0u);
    EXPECT_EQ(uploadContext->endFrameCalls, 0u);
    EXPECT_EQ(impl->deferredThreadedFrameScopedRetirementFrameContexts.count(0u), 0u);
    ASSERT_EQ(impl->retainedThreadedSubmitResourceKeepAliveByFrameContext.size(), 1u);
    const auto& keepAlive = impl->retainedThreadedSubmitResourceKeepAliveByFrameContext[0u];
    EXPECT_TRUE(ContainsRetainedResource(keepAlive, commandPool));
    EXPECT_TRUE(ContainsRetainedResource(keepAlive, commandBuffer));

    EXPECT_FALSE(NLS::Render::Context::DriverTestAccess::BeginStandaloneExplicitFrame(driver, false));

    EXPECT_EQ(frameFence->waitCalls, 1u);
    EXPECT_EQ(descriptorAllocator->endFrameCalls, 0u);
    EXPECT_EQ(uploadContext->endFrameCalls, 0u);
    EXPECT_EQ(impl->deferredThreadedFrameScopedRetirementFrameContexts.count(0u), 0u);
    ASSERT_EQ(impl->retainedThreadedSubmitResourceKeepAliveByFrameContext.size(), 1u);
    EXPECT_FALSE(impl->retainedThreadedSubmitResourceKeepAliveByFrameContext[0u].empty());
}

TEST(ThreadedRenderingLifecycleTests, StandaloneExplicitSubmitFailureAfterWaitRetainsResourcesWithoutFenceSignal)
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
    auto descriptorAllocator = std::make_shared<TestDescriptorAllocator>();
    auto uploadContext = std::make_shared<TestUploadContext>();
    commandPool->commandBuffer = commandBuffer;
    imageAcquiredSemaphore->signaled = true;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = frameFence;
    frameContext.imageAcquiredSemaphore = imageAcquiredSemaphore;
    frameContext.renderFinishedSemaphore = renderFinishedSemaphore;
    frameContext.descriptorAllocator = descriptorAllocator;
    frameContext.uploadContext = uploadContext;

    auto testQueue = explicitDevice->GetTestQueue();
    testQueue->submitFailureStage = TestQueue::SubmitFailureStage::AfterWaits;
    testQueue->nextSubmitResult = {
        NLS::Render::RHI::RHIQueueOperationStatusCode::BackendFailure,
        "simulated wait-stage failure"
    };

    auto* impl = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl, nullptr);

    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::BeginStandaloneExplicitFrame(driver, true));
    NLS::Render::Context::DriverTestAccess::EndStandaloneExplicitFrame(driver, false);

    EXPECT_EQ(testQueue->submitCalls, 1u);
    EXPECT_EQ(swapchain->acquireCalls, 1u);
    EXPECT_FALSE(imageAcquiredSemaphore->signaled);
    EXPECT_FALSE(renderFinishedSemaphore->signaled);
    EXPECT_FALSE(frameFence->submitSignalRequested);
    EXPECT_EQ(descriptorAllocator->endFrameCalls, 0u);
    EXPECT_EQ(uploadContext->endFrameCalls, 0u);
    EXPECT_EQ(impl->deferredThreadedFrameScopedRetirementFrameContexts.count(0u), 0u);
    EXPECT_TRUE(impl->unsafeGpuWorkQuarantined.load(std::memory_order_acquire));
    ASSERT_EQ(impl->retainedThreadedSubmitResourceKeepAliveByFrameContext.size(), 1u);
    const auto& keepAlive = impl->retainedThreadedSubmitResourceKeepAliveByFrameContext[0u];
    EXPECT_TRUE(ContainsRetainedResource(keepAlive, commandPool));
    EXPECT_TRUE(ContainsRetainedResource(keepAlive, commandBuffer));
    EXPECT_TRUE(ContainsRetainedResource(keepAlive, imageAcquiredSemaphore));
    EXPECT_TRUE(ContainsRetainedResource(keepAlive, renderFinishedSemaphore));
}

TEST(ThreadedRenderingLifecycleTests, ThreadedSubmitFailureBeforeQueueWorkDoesNotResetReusableFrameFence)
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
    frameFence->signaled = true;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = frameFence;

    auto testQueue = explicitDevice->GetTestQueue();
    testQueue->submitFailureStage = TestQueue::SubmitFailureStage::BeforeQueueWork;
    testQueue->nextSubmitResult = {
        NLS::Render::RHI::RHIQueueOperationStatusCode::BackendFailure,
        "simulated validation failure before queue work"
    };

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 901u;
    snapshot.targetsSwapchain = false;
    snapshot.renderWidth = 64u;
    snapshot.renderHeight = 64u;
    snapshot.visibleOpaqueDrawCount = 1u;

    NLS::Render::Context::RenderScenePackage package;
    package.frameId = snapshot.frameId;
    package.targetsSwapchain = false;
    package.renderWidth = snapshot.renderWidth;
    package.renderHeight = snapshot.renderHeight;
    package.frameDataReady = true;
    package.objectDataReady = true;
    package.hasVisibleDraws = true;
    package.visibleDrawCount = 1u;
    package.opaqueDrawCount = 1u;

    NLS::Render::Context::RenderPassCommandInput passInput;
    passInput.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    passInput.targetsSwapchain = false;
    passInput.renderWidth = snapshot.renderWidth;
    passInput.renderHeight = snapshot.renderHeight;
    passInput.drawCount = 1u;
    package.passCommandInputs.push_back(passInput);
    package.containsCommandInputs = true;
    package.passPlanCount = 1u;

    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessPreparedFrame(
        driver,
        snapshot,
        package));
    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    EXPECT_EQ(testQueue->submitCalls, 0u);
    EXPECT_EQ(frameFence->resetCalls, 0u);
    EXPECT_TRUE(frameFence->signaled);
    EXPECT_EQ(NLS::Render::Context::DriverTestAccess::GetRetainedThreadedSubmitResourceCount(driver), 0u);
}

TEST(ThreadedRenderingLifecycleTests, StandaloneExplicitSubmitSuccessDefersFrameScopedResourceRetirementUntilReusableFence)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = false;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    auto frameFence = std::make_shared<TestFence>();
    auto descriptorAllocator = std::make_shared<TestDescriptorAllocator>();
    auto uploadContext = std::make_shared<TestUploadContext>();
    commandPool->commandBuffer = commandBuffer;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = frameFence;
    frameContext.descriptorAllocator = descriptorAllocator;
    frameContext.uploadContext = uploadContext;

    auto* impl = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl, nullptr);

    NLS::Render::Context::DriverTestAccess::BeginStandaloneExplicitFrame(driver, false);
    NLS::Render::Context::DriverTestAccess::EndStandaloneExplicitFrame(driver, false);

    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    EXPECT_EQ(descriptorAllocator->endFrameCalls, 0u);
    EXPECT_EQ(uploadContext->endFrameCalls, 0u);
    EXPECT_EQ(impl->deferredThreadedFrameScopedRetirementFrameContexts.count(0u), 1u);
    ASSERT_EQ(impl->retainedThreadedSubmitResourceKeepAliveByFrameContext.size(), 1u);
    const auto& keepAlive = impl->retainedThreadedSubmitResourceKeepAliveByFrameContext[0u];
    EXPECT_TRUE(ContainsRetainedResource(keepAlive, commandPool));
    EXPECT_TRUE(ContainsRetainedResource(keepAlive, commandBuffer));

    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::BeginStandaloneExplicitFrame(driver, false));

    EXPECT_EQ(frameFence->waitCalls, 2u);
    EXPECT_EQ(descriptorAllocator->endFrameCalls, 1u);
    EXPECT_EQ(uploadContext->endFrameCalls, 1u);
    EXPECT_EQ(impl->deferredThreadedFrameScopedRetirementFrameContexts.count(0u), 0u);
    ASSERT_EQ(impl->retainedThreadedSubmitResourceKeepAliveByFrameContext.size(), 1u);
    EXPECT_TRUE(impl->retainedThreadedSubmitResourceKeepAliveByFrameContext[0u].empty());
}

TEST(ThreadedRenderingLifecycleTests, StandaloneExplicitBeginSkipsBlockingWaitWhenReusableFenceAlreadySignaled)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = false;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    auto frameFence = std::make_shared<TestFence>();
    commandPool->commandBuffer = commandBuffer;
    frameFence->signaled = true;
    frameFence->waitResults = { false };
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = frameFence;

    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::BeginStandaloneExplicitFrame(driver, false));

    EXPECT_EQ(frameFence->waitCalls, 0u);
    EXPECT_EQ(frameFence->resetCalls, 0u);
    EXPECT_TRUE(frameFence->signaled);
    EXPECT_EQ(commandPool->resetCalls, 1u);
    EXPECT_EQ(commandBuffer->resetCalls, 1u);

    frameFence->signaled = true;
}

TEST(ThreadedRenderingLifecycleTests, InRenderPassChildCommandRecordingRetainsResourcesWhenSubmitFailsAfterGpuWorkMayHaveBeenQueued)
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
    explicitDevice->MutableCapabilities().supportsParallelCommandTranslation = true;
    explicitDevice->MutableCapabilities().supportsInRenderPassChildCommandBuffers = true;
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandPool = std::make_shared<TestCommandPool>();
    auto frameFence = std::make_shared<TestFence>();
    auto descriptorAllocator = std::make_shared<TestDescriptorAllocator>();
    auto uploadContext = std::make_shared<TestUploadContext>();
    auto resourceStateTracker = std::make_shared<TestResourceStateTracker>();
    commandPool->commandBuffer = std::make_shared<TestCommandBuffer>();
    frameFence->waitResults = { true, false };
    frameContext.commandPool = commandPool;
    frameContext.commandBuffer = commandPool->commandBuffer;
    frameContext.frameFence = frameFence;
    frameContext.descriptorAllocator = descriptorAllocator;
    frameContext.uploadContext = uploadContext;
    frameContext.resourceStateTracker = resourceStateTracker;
    explicitDevice->GetTestQueue()->nextSubmitResult = {
        NLS::Render::RHI::RHIQueueOperationStatusCode::BackendFailure,
        "simulated post ExecuteCommandLists signal failure",
        true,
        true
    };

    auto opaquePipeline = std::make_shared<TestGraphicsPipeline>("OpaquePipeline");
    auto materialBindingSet = std::make_shared<TestBindingSet>("MaterialBindingSet");
    auto mesh = std::make_shared<TestMesh>();

    NLS::Render::Context::RenderPassCommandInput passInput;
    passInput.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    passInput.drawCount = 2000u;
    passInput.renderWidth = 64u;
    passInput.renderHeight = 64u;
    passInput.usesColorAttachment = true;
    passInput.usesDepthStencilAttachment = true;
    passInput.recordedDrawCommands.reserve(2000u);
    for (uint32_t drawIndex = 0u; drawIndex < 2000u; ++drawIndex)
        passInput.recordedDrawCommands.push_back({ opaquePipeline, nullptr, nullptr, nullptr, materialBindingSet, mesh, 1u });

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 703u;
    snapshot.targetsSwapchain = false;
    snapshot.visibleOpaqueDrawCount = 2000u;

    NLS::Render::Context::RenderScenePackage package;
    package.frameId = snapshot.frameId;
    package.targetsSwapchain = false;
    package.visibleDrawCount = 2000u;
    package.opaqueDrawCount = 2000u;
    package.hasVisibleDraws = true;
    package.frameDataReady = true;
    package.objectDataReady = true;
    package.renderWidth = 64u;
    package.renderHeight = 64u;
    package.containsParallelCommandWorkUnits = true;
    package.passCommandInputs = { passInput };
    package.parallelCommandWorkUnits = NLS::Render::Context::BuildRecordedDrawCommandWorkUnitsForPass(
        passInput,
        0u,
        0u);
    package.parallelCommandWorkUnitCount = static_cast<uint64_t>(package.parallelCommandWorkUnits.size());

    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessPreparedFrame(
        driver,
        snapshot,
        package));
    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    EXPECT_EQ(descriptorAllocator->endFrameCalls, 0u);
    EXPECT_EQ(uploadContext->endFrameCalls, 0u);
    auto* impl = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl, nullptr);
    ASSERT_EQ(impl->retainedThreadedSubmitResourceKeepAliveByFrameContext.size(), 1u);
    const auto& keepAlive = impl->retainedThreadedSubmitResourceKeepAliveByFrameContext[0u];
    EXPECT_TRUE(ContainsRetainedResource(keepAlive, commandPool));
    EXPECT_TRUE(ContainsRetainedResource(keepAlive, commandPool->commandBuffer));
}

TEST(ThreadedRenderingLifecycleTests, InRenderPassChildCommandRecordingHandlesMixedComputeAndChildSlicesInOrder)
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
    explicitDevice->MutableCapabilities().supportsParallelCommandTranslation = true;
    explicitDevice->MutableCapabilities().supportsInRenderPassChildCommandBuffers = true;
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    commandPool->commandBuffer = commandBuffer;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = std::make_shared<TestFence>();
    frameContext.resourceStateTracker = nullptr;

    auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 606u;
    snapshot.targetsSwapchain = false;
    snapshot.visibleOpaqueDrawCount = 2000u;
    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessFrameSnapshot(driver, snapshot));

    size_t slotIndex = 0u;
    NLS::Render::Context::FrameSnapshot publishedSnapshot;
    ASSERT_TRUE(lifecycle->TryBeginNextRenderScene(&slotIndex, &publishedSnapshot));

    auto graphicsPipeline = std::make_shared<TestGraphicsPipeline>("OpaquePipeline");
    auto computePipeline = std::make_shared<TestComputePipeline>("VisibilityComputePipeline");
    auto materialBindingSet = std::make_shared<TestBindingSet>("MaterialBindingSet");
    auto mesh = std::make_shared<TestMesh>();

    NLS::Render::Context::RenderPassCommandInput computePassInput;
    computePassInput.kind = NLS::Render::Context::RenderPassCommandKind::Compute;
    computePassInput.debugName = "VisibilityCompute";
    computePassInput.queueType = NLS::Render::RHI::QueueType::Compute;
    computePassInput.computeDispatchInputs.push_back({
        "VisibilityCompute",
        computePipeline,
        {},
        1u,
        1u,
        1u,
        {},
        {},
        {}
    });

    NLS::Render::Context::RenderPassCommandInput passInput;
    passInput.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    passInput.drawCount = 2000u;
    passInput.requiresFrameData = true;
    passInput.requiresObjectData = true;
    passInput.targetsSwapchain = false;
    passInput.renderWidth = 64u;
    passInput.renderHeight = 64u;
    passInput.clearColor = true;
    passInput.clearDepth = true;
    passInput.usesColorAttachment = true;
    passInput.usesDepthStencilAttachment = true;
    passInput.recordedDrawCommands.reserve(2000u);
    for (uint32_t drawIndex = 0u; drawIndex < 2000u; ++drawIndex)
    {
        passInput.recordedDrawCommands.push_back({
            graphicsPipeline,
            nullptr,
            nullptr,
            nullptr,
            materialBindingSet,
            mesh,
            1u
        });
    }

    NLS::Render::Context::ParallelCommandWorkUnit computeWorkUnit;
    computeWorkUnit.commandInput = computePassInput;
    computeWorkUnit.debugName = computePassInput.debugName;
    computeWorkUnit.eligibleForParallelRecording = false;

    auto childWorkUnits = NLS::Render::Context::BuildRecordedDrawCommandWorkUnitsForPass(
        passInput,
        0u,
        1u);
    ASSERT_GT(childWorkUnits.size(), 1u);
    ASSERT_TRUE(childWorkUnits[0].usesInRenderPassChildCommandRecording);

    NLS::Render::Context::RenderScenePackage package;
    package.frameId = publishedSnapshot.frameId;
    package.targetsSwapchain = false;
    package.visibleDrawCount = 2000u;
    package.opaqueDrawCount = 2000u;
    package.hasVisibleDraws = true;
    package.frameDataReady = true;
    package.objectDataReady = true;
    package.renderWidth = 64u;
    package.renderHeight = 64u;
    package.containsParallelCommandWorkUnits = true;
    package.passCommandInputs = { passInput };
    package.parallelCommandWorkUnits.push_back(computeWorkUnit);
    package.parallelCommandWorkUnits.insert(
        package.parallelCommandWorkUnits.end(),
        childWorkUnits.begin(),
        childWorkUnits.end());
    package.parallelCommandWorkUnitCount = static_cast<uint64_t>(package.parallelCommandWorkUnits.size());

    ASSERT_TRUE(lifecycle->CompleteRenderScene(slotIndex, package));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    const auto copiedSlot = lifecycle->CopySlot(0u);
    ASSERT_TRUE(copiedSlot.has_value());
    ASSERT_TRUE(copiedSlot->submissionFrame.has_value());
    EXPECT_TRUE(copiedSlot->submissionFrame->usedParallelCommandPath);
    EXPECT_EQ(copiedSlot->submissionFrame->recordedPassCount, 2u);
    EXPECT_EQ(copiedSlot->submissionFrame->recordedDrawCount, 2000u);
    EXPECT_EQ(copiedSlot->submissionFrame->recordedWorkUnitCount, 5u);
    ASSERT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    const auto submittedBuffer = GetSubmittedTestCommandBuffer(explicitDevice->GetTestQueue());
    ASSERT_NE(submittedBuffer, nullptr);
    EXPECT_EQ(submittedBuffer->dispatchCalls, 1u);
    EXPECT_EQ(submittedBuffer->beginRenderPassCalls, 1u);
    EXPECT_EQ(submittedBuffer->endRenderPassCalls, 1u);
    EXPECT_EQ(submittedBuffer->executeChildCommandBufferCalls, 4u);
    EXPECT_EQ(submittedBuffer->drawIndexedCalls, 0u);
}

TEST(ThreadedRenderingLifecycleTests, InRenderPassChildCommandRecordingPreservesMixedAttachmentFreeSlices)
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
    explicitDevice->MutableCapabilities().supportsParallelCommandTranslation = true;
    explicitDevice->MutableCapabilities().supportsInRenderPassChildCommandBuffers = true;
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    commandPool->commandBuffer = commandBuffer;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = std::make_shared<TestFence>();
    frameContext.resourceStateTracker = nullptr;

    auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 610u;
    snapshot.targetsSwapchain = false;
    snapshot.visibleOpaqueDrawCount = 4000u;
    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessFrameSnapshot(driver, snapshot));

    size_t slotIndex = 0u;
    NLS::Render::Context::FrameSnapshot publishedSnapshot;
    ASSERT_TRUE(lifecycle->TryBeginNextRenderScene(&slotIndex, &publishedSnapshot));

    auto graphicsPipeline = std::make_shared<TestGraphicsPipeline>("OpaquePipeline");
    auto materialBindingSet = std::make_shared<TestBindingSet>("MaterialBindingSet");
    auto mesh = std::make_shared<TestMesh>();

    NLS::Render::Context::RenderPassCommandInput attachmentFreePassInput;
    attachmentFreePassInput.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    attachmentFreePassInput.debugName = "AttachmentFree";
    attachmentFreePassInput.drawCount = 2000u;
    attachmentFreePassInput.requiresFrameData = true;
    attachmentFreePassInput.requiresObjectData = true;
    attachmentFreePassInput.targetsSwapchain = false;
    attachmentFreePassInput.renderWidth = 64u;
    attachmentFreePassInput.renderHeight = 64u;
    attachmentFreePassInput.recordedDrawCommands.reserve(2000u);
    for (uint32_t drawIndex = 0u; drawIndex < 2000u; ++drawIndex)
    {
        attachmentFreePassInput.recordedDrawCommands.push_back({
            graphicsPipeline,
            nullptr,
            nullptr,
            nullptr,
            materialBindingSet,
            mesh,
            1u
        });
    }

    NLS::Render::Context::RenderPassCommandInput attachmentBackedPassInput = attachmentFreePassInput;
    attachmentBackedPassInput.debugName = "AttachmentBacked";
    attachmentBackedPassInput.clearColor = true;
    attachmentBackedPassInput.clearDepth = true;
    attachmentBackedPassInput.usesColorAttachment = true;
    attachmentBackedPassInput.usesDepthStencilAttachment = true;

    auto attachmentFreeWorkUnits = NLS::Render::Context::BuildRecordedDrawCommandWorkUnitsForPass(
        attachmentFreePassInput,
        0u,
        0u);
    auto childWorkUnits = NLS::Render::Context::BuildRecordedDrawCommandWorkUnitsForPass(
        attachmentBackedPassInput,
        1u,
        static_cast<uint64_t>(attachmentFreeWorkUnits.size()));
    ASSERT_EQ(attachmentFreeWorkUnits.size(), 4u);
    ASSERT_FALSE(attachmentFreeWorkUnits[0].usesInRenderPassChildCommandRecording);
    ASSERT_EQ(childWorkUnits.size(), 4u);
    ASSERT_TRUE(childWorkUnits[0].usesInRenderPassChildCommandRecording);

    NLS::Render::Context::RenderScenePackage package;
    package.frameId = publishedSnapshot.frameId;
    package.targetsSwapchain = false;
    package.visibleDrawCount = 4000u;
    package.opaqueDrawCount = 4000u;
    package.hasVisibleDraws = true;
    package.frameDataReady = true;
    package.objectDataReady = true;
    package.renderWidth = 64u;
    package.renderHeight = 64u;
    package.containsParallelCommandWorkUnits = true;
    package.passCommandInputs = { attachmentFreePassInput, attachmentBackedPassInput };
    package.parallelCommandWorkUnits = attachmentFreeWorkUnits;
    package.parallelCommandWorkUnits.insert(
        package.parallelCommandWorkUnits.end(),
        childWorkUnits.begin(),
        childWorkUnits.end());
    package.parallelCommandWorkUnitCount = static_cast<uint64_t>(package.parallelCommandWorkUnits.size());

    ASSERT_TRUE(lifecycle->CompleteRenderScene(slotIndex, package));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    const auto copiedSlot = lifecycle->CopySlot(0u);
    ASSERT_TRUE(copiedSlot.has_value());
    ASSERT_TRUE(copiedSlot->submissionFrame.has_value());
    EXPECT_TRUE(copiedSlot->submissionFrame->submittedSuccessfully);
    EXPECT_EQ(copiedSlot->submissionFrame->recordedDrawCount, 4000u);
    EXPECT_EQ(copiedSlot->submissionFrame->recordedWorkUnitCount, 8u);
    ASSERT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    const auto submittedBuffer = GetSubmittedTestCommandBuffer(explicitDevice->GetTestQueue());
    ASSERT_NE(submittedBuffer, nullptr);
    EXPECT_EQ(submittedBuffer->drawIndexedCalls, 2000u);
    EXPECT_EQ(submittedBuffer->executeChildCommandBufferCalls, 4u);
}

TEST(ThreadedRenderingLifecycleTests, InRenderPassChildCommandRecordingRecordsAllSmallSliceGroupsSerially)
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
    explicitDevice->MutableCapabilities().supportsParallelCommandTranslation = true;
    explicitDevice->MutableCapabilities().supportsInRenderPassChildCommandBuffers = true;
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    commandPool->commandBuffer = commandBuffer;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = std::make_shared<TestFence>();
    frameContext.resourceStateTracker = nullptr;

    auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 612u;
    snapshot.targetsSwapchain = false;
    snapshot.visibleOpaqueDrawCount = 1024u;
    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessFrameSnapshot(driver, snapshot));

    size_t slotIndex = 0u;
    NLS::Render::Context::FrameSnapshot publishedSnapshot;
    ASSERT_TRUE(lifecycle->TryBeginNextRenderScene(&slotIndex, &publishedSnapshot));

    auto graphicsPipeline = std::make_shared<TestGraphicsPipeline>("OpaquePipeline");
    auto materialBindingSet = std::make_shared<TestBindingSet>("MaterialBindingSet");
    auto mesh = std::make_shared<TestMesh>();

    NLS::Render::Context::RenderPassCommandInput passInput;
    passInput.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    passInput.drawCount = 1024u;
    passInput.requiresFrameData = true;
    passInput.requiresObjectData = true;
    passInput.targetsSwapchain = false;
    passInput.renderWidth = 64u;
    passInput.renderHeight = 64u;
    passInput.clearColor = true;
    passInput.clearDepth = true;
    passInput.usesColorAttachment = true;
    passInput.usesDepthStencilAttachment = true;
    passInput.recordedDrawCommands.reserve(1024u);
    for (uint32_t drawIndex = 0u; drawIndex < 1024u; ++drawIndex)
    {
        passInput.recordedDrawCommands.push_back({
            graphicsPipeline,
            nullptr,
            nullptr,
            nullptr,
            materialBindingSet,
            mesh,
            1u
        });
    }

    NLS::Render::Context::RenderScenePackage package;
    package.frameId = publishedSnapshot.frameId;
    package.targetsSwapchain = false;
    package.visibleDrawCount = 1024u;
    package.opaqueDrawCount = 1024u;
    package.hasVisibleDraws = true;
    package.frameDataReady = true;
    package.objectDataReady = true;
    package.renderWidth = 64u;
    package.renderHeight = 64u;
    package.containsParallelCommandWorkUnits = true;
    package.passCommandInputs = { passInput };
    package.parallelCommandWorkUnits = NLS::Render::Context::BuildRecordedDrawCommandWorkUnitsForPass(
        passInput,
        0u,
        0u);
    package.parallelCommandWorkUnitCount = static_cast<uint64_t>(package.parallelCommandWorkUnits.size());
    ASSERT_EQ(package.parallelCommandWorkUnits.size(), 2u);

    ASSERT_TRUE(lifecycle->CompleteRenderScene(slotIndex, package));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    const auto copiedSlot = lifecycle->CopySlot(0u);
    ASSERT_TRUE(copiedSlot.has_value());
    ASSERT_TRUE(copiedSlot->submissionFrame.has_value());
    EXPECT_TRUE(copiedSlot->submissionFrame->usedParallelCommandPath);
    EXPECT_FALSE(copiedSlot->submissionFrame->usedSerialCommandPath);
    EXPECT_EQ(copiedSlot->submissionFrame->recordedPassCount, 1u);
    EXPECT_EQ(copiedSlot->submissionFrame->recordedDrawCount, 1024u);
    EXPECT_EQ(copiedSlot->submissionFrame->recordedWorkUnitCount, 2u);
    EXPECT_EQ(copiedSlot->submissionFrame->parallelRecordingWorkerCount, 1u);
    const auto submittedBuffer = GetSubmittedTestCommandBuffer(explicitDevice->GetTestQueue());
    ASSERT_NE(submittedBuffer, nullptr);
    EXPECT_EQ(submittedBuffer->executeChildCommandBufferCalls, 2u);
}

TEST(ThreadedRenderingLifecycleTests, InRenderPassChildCommandRecordingGroupsMultipleSourcePassesSeparately)
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
    explicitDevice->MutableCapabilities().supportsParallelCommandTranslation = true;
    explicitDevice->MutableCapabilities().supportsInRenderPassChildCommandBuffers = true;
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    commandPool->commandBuffer = commandBuffer;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = std::make_shared<TestFence>();
    frameContext.resourceStateTracker = nullptr;

    auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 607u;
    snapshot.targetsSwapchain = false;
    snapshot.visibleOpaqueDrawCount = 4000u;
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
    opaquePassInput.debugName = "Opaque";
    opaquePassInput.drawCount = 2000u;
    opaquePassInput.requiresFrameData = true;
    opaquePassInput.requiresObjectData = true;
    opaquePassInput.targetsSwapchain = false;
    opaquePassInput.renderWidth = 64u;
    opaquePassInput.renderHeight = 64u;
    opaquePassInput.clearColor = true;
    opaquePassInput.clearDepth = true;
    opaquePassInput.usesColorAttachment = true;
    opaquePassInput.usesDepthStencilAttachment = true;
    opaquePassInput.recordedDrawCommands.reserve(2000u);
    for (uint32_t drawIndex = 0u; drawIndex < 2000u; ++drawIndex)
    {
        opaquePassInput.recordedDrawCommands.push_back({
            opaquePipeline,
            nullptr,
            nullptr,
            nullptr,
            materialBindingSet,
            mesh,
            1u
        });
    }

    NLS::Render::Context::RenderPassCommandInput transparentPassInput = opaquePassInput;
    transparentPassInput.kind = NLS::Render::Context::RenderPassCommandKind::Transparent;
    transparentPassInput.debugName = "Transparent";
    transparentPassInput.clearColor = false;
    transparentPassInput.clearDepth = false;
    for (auto& draw : transparentPassInput.recordedDrawCommands)
    {
        draw.pipeline = transparentPipeline;
    }

    auto opaqueWorkUnits = NLS::Render::Context::BuildRecordedDrawCommandWorkUnitsForPass(
        opaquePassInput,
        0u,
        0u);
    auto transparentWorkUnits = NLS::Render::Context::BuildRecordedDrawCommandWorkUnitsForPass(
        transparentPassInput,
        1u,
        static_cast<uint64_t>(opaqueWorkUnits.size()));
    ASSERT_EQ(opaqueWorkUnits.size(), 4u);
    ASSERT_EQ(transparentWorkUnits.size(), 4u);
    ASSERT_TRUE(opaqueWorkUnits[0].usesInRenderPassChildCommandRecording);
    ASSERT_TRUE(transparentWorkUnits[0].usesInRenderPassChildCommandRecording);

    NLS::Render::Context::RenderScenePackage package;
    package.frameId = publishedSnapshot.frameId;
    package.targetsSwapchain = false;
    package.visibleDrawCount = 4000u;
    package.opaqueDrawCount = 2000u;
    package.transparentDrawCount = 2000u;
    package.hasVisibleDraws = true;
    package.frameDataReady = true;
    package.objectDataReady = true;
    package.renderWidth = 64u;
    package.renderHeight = 64u;
    package.containsParallelCommandWorkUnits = true;
    package.passCommandInputs = { opaquePassInput, transparentPassInput };
    package.parallelCommandWorkUnits = opaqueWorkUnits;
    package.parallelCommandWorkUnits.insert(
        package.parallelCommandWorkUnits.end(),
        transparentWorkUnits.begin(),
        transparentWorkUnits.end());
    package.parallelCommandWorkUnitCount = static_cast<uint64_t>(package.parallelCommandWorkUnits.size());

    ASSERT_TRUE(lifecycle->CompleteRenderScene(slotIndex, package));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    const auto copiedSlot = lifecycle->CopySlot(0u);
    ASSERT_TRUE(copiedSlot.has_value());
    ASSERT_TRUE(copiedSlot->submissionFrame.has_value());
    EXPECT_TRUE(copiedSlot->submissionFrame->usedParallelCommandPath);
    EXPECT_EQ(copiedSlot->submissionFrame->recordedPassCount, 2u);
    EXPECT_EQ(copiedSlot->submissionFrame->recordedDrawCount, 4000u);
    EXPECT_EQ(copiedSlot->submissionFrame->recordedWorkUnitCount, 8u);
    ASSERT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    const auto submittedBuffer = GetSubmittedTestCommandBuffer(explicitDevice->GetTestQueue());
    ASSERT_NE(submittedBuffer, nullptr);
    EXPECT_EQ(submittedBuffer->beginRenderPassCalls, 2u);
    EXPECT_EQ(submittedBuffer->endRenderPassCalls, 2u);
    EXPECT_EQ(submittedBuffer->executeChildCommandBufferCalls, 8u);
}

TEST(ThreadedRenderingLifecycleTests, InRenderPassChildCommandRecordingFallsBackToSerialWhenChildCreationFails)
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
    explicitDevice->MutableCapabilities().supportsParallelCommandTranslation = true;
    explicitDevice->MutableCapabilities().supportsInRenderPassChildCommandBuffers = true;
    explicitDevice->FailCreatedCommandPoolAtCall(1u);
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    commandPool->commandBuffer = commandBuffer;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = std::make_shared<TestFence>();
    frameContext.resourceStateTracker = nullptr;

    auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 608u;
    snapshot.targetsSwapchain = false;
    snapshot.visibleOpaqueDrawCount = 2000u;
    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessFrameSnapshot(driver, snapshot));

    size_t slotIndex = 0u;
    NLS::Render::Context::FrameSnapshot publishedSnapshot;
    ASSERT_TRUE(lifecycle->TryBeginNextRenderScene(&slotIndex, &publishedSnapshot));

    auto opaquePipeline = std::make_shared<TestGraphicsPipeline>("OpaquePipeline");
    auto materialBindingSet = std::make_shared<TestBindingSet>("MaterialBindingSet");
    auto mesh = std::make_shared<TestMesh>();

    NLS::Render::Context::RenderPassCommandInput passInput;
    passInput.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    passInput.drawCount = 2000u;
    passInput.requiresFrameData = true;
    passInput.requiresObjectData = true;
    passInput.targetsSwapchain = false;
    passInput.renderWidth = 64u;
    passInput.renderHeight = 64u;
    passInput.clearColor = true;
    passInput.clearDepth = true;
    passInput.usesColorAttachment = true;
    passInput.usesDepthStencilAttachment = true;
    passInput.recordedDrawCommands.reserve(2000u);
    for (uint32_t drawIndex = 0u; drawIndex < 2000u; ++drawIndex)
    {
        passInput.recordedDrawCommands.push_back({
            opaquePipeline,
            nullptr,
            nullptr,
            nullptr,
            materialBindingSet,
            mesh,
            1u
        });
    }

    NLS::Render::Context::RenderScenePackage package;
    package.frameId = publishedSnapshot.frameId;
    package.targetsSwapchain = false;
    package.visibleDrawCount = 2000u;
    package.opaqueDrawCount = 2000u;
    package.hasVisibleDraws = true;
    package.frameDataReady = true;
    package.objectDataReady = true;
    package.renderWidth = 64u;
    package.renderHeight = 64u;
    package.containsParallelCommandWorkUnits = true;
    package.passCommandInputs = { passInput };
    package.parallelCommandWorkUnits = NLS::Render::Context::BuildRecordedDrawCommandWorkUnitsForPass(
        passInput,
        0u,
        0u);
    package.parallelCommandWorkUnitCount = static_cast<uint64_t>(package.parallelCommandWorkUnits.size());

    ASSERT_TRUE(lifecycle->CompleteRenderScene(slotIndex, package));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    const auto copiedSlot = lifecycle->CopySlot(0u);
    ASSERT_TRUE(copiedSlot.has_value());
    ASSERT_TRUE(copiedSlot->submissionFrame.has_value());
    EXPECT_TRUE(copiedSlot->submissionFrame->submittedSuccessfully);
    EXPECT_FALSE(copiedSlot->submissionFrame->usedParallelCommandPath);
    EXPECT_TRUE(copiedSlot->submissionFrame->usedSerialCommandPath);
    EXPECT_EQ(copiedSlot->submissionFrame->commandRecordingFailureCount, 0u);
    EXPECT_EQ(copiedSlot->submissionFrame->recordedPassCount, 1u);
    EXPECT_EQ(copiedSlot->submissionFrame->recordedDrawCount, 2000u);
    ASSERT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    const auto submittedBuffer = GetSubmittedTestCommandBuffer(explicitDevice->GetTestQueue());
    ASSERT_NE(submittedBuffer, nullptr);
    EXPECT_EQ(submittedBuffer->executeChildCommandBufferCalls, 0u);
    EXPECT_EQ(submittedBuffer->beginRenderPassCalls, 1u);
    EXPECT_EQ(submittedBuffer->endRenderPassCalls, 1u);
    EXPECT_EQ(submittedBuffer->drawIndexedCalls, 2000u);
}

TEST(ThreadedRenderingLifecycleTests, InRenderPassChildCommandRecordingMarksFrameFailedWhenExecuteChildFails)
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
    explicitDevice->MutableCapabilities().supportsParallelCommandTranslation = true;
    explicitDevice->MutableCapabilities().supportsInRenderPassChildCommandBuffers = true;
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    commandBuffer->failExecuteChildCommandBuffer = true;
    commandPool->commandBuffer = commandBuffer;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = std::make_shared<TestFence>();
    frameContext.resourceStateTracker = nullptr;

    auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 609u;
    snapshot.targetsSwapchain = false;
    snapshot.visibleOpaqueDrawCount = 2000u;
    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessFrameSnapshot(driver, snapshot));

    size_t slotIndex = 0u;
    NLS::Render::Context::FrameSnapshot publishedSnapshot;
    ASSERT_TRUE(lifecycle->TryBeginNextRenderScene(&slotIndex, &publishedSnapshot));

    auto opaquePipeline = std::make_shared<TestGraphicsPipeline>("OpaquePipeline");
    auto materialBindingSet = std::make_shared<TestBindingSet>("MaterialBindingSet");
    auto mesh = std::make_shared<TestMesh>();

    NLS::Render::Context::RenderPassCommandInput passInput;
    passInput.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    passInput.drawCount = 2000u;
    passInput.requiresFrameData = true;
    passInput.requiresObjectData = true;
    passInput.targetsSwapchain = false;
    passInput.renderWidth = 64u;
    passInput.renderHeight = 64u;
    passInput.clearColor = true;
    passInput.clearDepth = true;
    passInput.usesColorAttachment = true;
    passInput.usesDepthStencilAttachment = true;
    passInput.recordedDrawCommands.reserve(2000u);
    for (uint32_t drawIndex = 0u; drawIndex < 2000u; ++drawIndex)
    {
        passInput.recordedDrawCommands.push_back({
            opaquePipeline,
            nullptr,
            nullptr,
            nullptr,
            materialBindingSet,
            mesh,
            1u
        });
    }

    NLS::Render::Context::RenderScenePackage package;
    package.frameId = publishedSnapshot.frameId;
    package.targetsSwapchain = false;
    package.visibleDrawCount = 2000u;
    package.opaqueDrawCount = 2000u;
    package.hasVisibleDraws = true;
    package.frameDataReady = true;
    package.objectDataReady = true;
    package.renderWidth = 64u;
    package.renderHeight = 64u;
    package.containsParallelCommandWorkUnits = true;
    package.passCommandInputs = { passInput };
    package.parallelCommandWorkUnits = NLS::Render::Context::BuildRecordedDrawCommandWorkUnitsForPass(
        passInput,
        0u,
        0u);
    package.parallelCommandWorkUnitCount = static_cast<uint64_t>(package.parallelCommandWorkUnits.size());

    ASSERT_TRUE(lifecycle->CompleteRenderScene(slotIndex, package));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    const auto copiedSlot = lifecycle->CopySlot(0u);
    ASSERT_TRUE(copiedSlot.has_value());
    ASSERT_TRUE(copiedSlot->submissionFrame.has_value());
    EXPECT_FALSE(copiedSlot->submissionFrame->submittedSuccessfully);
    EXPECT_GT(copiedSlot->submissionFrame->commandRecordingFailureCount, 0u);
    EXPECT_NE(
        copiedSlot->submissionFrame->lastCommandRecordingFailure.find("test execute child failure"),
        std::string::npos);
    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 0u);
    EXPECT_EQ(commandBuffer->endCalls, commandBuffer->beginCalls);
    EXPECT_TRUE(commandBuffer->IsClosedForSubmission());
}

TEST(ThreadedRenderingLifecycleTests, InRenderPassChildCommandRecordingFallsBackWhenChildRangeChangesDescriptorHeap)
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
    explicitDevice->MutableCapabilities().supportsParallelCommandTranslation = true;
    explicitDevice->MutableCapabilities().supportsInRenderPassChildCommandBuffers = true;
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    commandPool->commandBuffer = commandBuffer;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = std::make_shared<TestFence>();

    auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 614u;
    snapshot.targetsSwapchain = false;
    snapshot.visibleOpaqueDrawCount = 2000u;
    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessFrameSnapshot(driver, snapshot));

    size_t slotIndex = 0u;
    NLS::Render::Context::FrameSnapshot publishedSnapshot;
    ASSERT_TRUE(lifecycle->TryBeginNextRenderScene(&slotIndex, &publishedSnapshot));

    auto opaquePipeline = std::make_shared<TestGraphicsPipeline>("OpaquePipeline");
    auto firstHeapBindingSet = std::make_shared<TestBindingSet>("FirstHeapMaterial");
    auto secondHeapBindingSet = std::make_shared<TestBindingSet>("SecondHeapMaterial");
    int firstHeap = 1;
    int secondHeap = 2;
    firstHeapBindingSet->resourceDescriptorHeapHandle = {
        NLS::Render::RHI::BackendType::DX12,
        &firstHeap
    };
    secondHeapBindingSet->resourceDescriptorHeapHandle = {
        NLS::Render::RHI::BackendType::DX12,
        &secondHeap
    };
    auto mesh = std::make_shared<TestMesh>();

    NLS::Render::Context::RenderPassCommandInput passInput;
    passInput.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    passInput.drawCount = 2000u;
    passInput.requiresFrameData = true;
    passInput.requiresObjectData = true;
    passInput.renderWidth = 64u;
    passInput.renderHeight = 64u;
    passInput.usesColorAttachment = true;
    passInput.usesDepthStencilAttachment = true;
    passInput.recordedDrawCommands.reserve(2000u);
    for (uint32_t drawIndex = 0u; drawIndex < 2000u; ++drawIndex)
    {
        passInput.recordedDrawCommands.push_back({
            opaquePipeline,
            nullptr,
            nullptr,
            nullptr,
            drawIndex == 1u ? secondHeapBindingSet : firstHeapBindingSet,
            mesh,
            1u
        });
    }

    NLS::Render::Context::RenderScenePackage package;
    package.frameId = publishedSnapshot.frameId;
    package.targetsSwapchain = false;
    package.visibleDrawCount = 2000u;
    package.opaqueDrawCount = 2000u;
    package.hasVisibleDraws = true;
    package.frameDataReady = true;
    package.objectDataReady = true;
    package.renderWidth = 64u;
    package.renderHeight = 64u;
    package.containsParallelCommandWorkUnits = true;
    package.passCommandInputs = { passInput };
    package.parallelCommandWorkUnits = NLS::Render::Context::BuildRecordedDrawCommandWorkUnitsForPass(
        passInput,
        0u,
        0u);
    package.parallelCommandWorkUnitCount = static_cast<uint64_t>(package.parallelCommandWorkUnits.size());

    ASSERT_TRUE(lifecycle->CompleteRenderScene(slotIndex, package));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    const auto copiedSlot = lifecycle->CopySlot(0u);
    ASSERT_TRUE(copiedSlot.has_value());
    ASSERT_TRUE(copiedSlot->submissionFrame.has_value());
    EXPECT_TRUE(copiedSlot->submissionFrame->submittedSuccessfully);
    EXPECT_FALSE(copiedSlot->submissionFrame->usedParallelCommandPath);
    EXPECT_TRUE(copiedSlot->submissionFrame->usedSerialCommandPath);
    EXPECT_EQ(copiedSlot->submissionFrame->commandRecordingFailureCount, 0u);
    EXPECT_EQ(copiedSlot->submissionFrame->recordedPassCount, 1u);
    EXPECT_EQ(copiedSlot->submissionFrame->recordedWorkUnitCount, 1u);
    EXPECT_EQ(copiedSlot->submissionFrame->recordedDrawCount, 2000u);
    EXPECT_NE(
        copiedSlot->submissionFrame->parallelFallbackReason.find("in-render-pass child command recording failed"),
        std::string::npos);
    ASSERT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    ASSERT_EQ(explicitDevice->GetTestQueue()->lastSubmitDesc.commandBuffers.size(), 1u);
    const auto submittedBuffer = GetSubmittedTestCommandBuffer(explicitDevice->GetTestQueue());
    ASSERT_NE(submittedBuffer, nullptr);
    EXPECT_EQ(submittedBuffer->executeChildCommandBufferCalls, 0u);
    EXPECT_EQ(submittedBuffer->drawIndexedCalls, 2000u);
}

TEST(ThreadedRenderingLifecycleTests, SetExplicitDeviceClearsReusableChildCommandResources)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto firstDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, firstDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.childCommandPools.push_back(std::make_shared<TestCommandPool>());
    frameContext.childCommandBuffers.push_back(std::make_shared<TestCommandBuffer>());
    frameContext.commandPool = std::make_shared<TestCommandPool>();
    frameContext.commandBuffer = std::make_shared<TestCommandBuffer>();
    auto swapchain = std::make_shared<TestSwapchain>();
    std::weak_ptr<TestSwapchain> weakSwapchain = swapchain;
    NLS::Render::Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);
    swapchain.reset();
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, true);

    auto secondDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, secondDevice);

    EXPECT_EQ(NLS::Render::Context::DriverTestAccess::PeekFrameContext(driver, 0u), nullptr);
    EXPECT_EQ(NLS::Render::Context::DriverTestAccess::GetRetainedThreadedSubmitResourceCount(driver), 0u);
    EXPECT_TRUE(weakSwapchain.expired());

    NLS::Core::ServiceLocator::Remove<NLS::Render::Context::Driver>();
}

TEST(ThreadedRenderingLifecycleTests, SetExplicitDeviceWaitsForThreadedRhiSubmissionBeforeReplacingResources)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto firstDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, firstDevice);
    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.commandPool = std::make_shared<TestCommandPool>();
    frameContext.commandBuffer = std::make_shared<TestCommandBuffer>();

    NLS::Render::Context::DriverTestAccess::LockThreadedRhiSubmission(driver);

    auto secondDevice = std::make_shared<TestExplicitDevice>();
    std::atomic<bool> replacementFinished = false;
    std::thread replacementThread(
        [&driver, secondDevice, &replacementFinished]()
        {
            NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, secondDevice);
            replacementFinished.store(true, std::memory_order_release);
        });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(replacementFinished.load(std::memory_order_acquire));
    EXPECT_NE(NLS::Render::Context::DriverTestAccess::PeekFrameContext(driver, 0u), nullptr);

    NLS::Render::Context::DriverTestAccess::UnlockThreadedRhiSubmission(driver);
    replacementThread.join();

    EXPECT_TRUE(replacementFinished.load(std::memory_order_acquire));
    EXPECT_EQ(NLS::Render::Context::DriverRendererAccess::GetExplicitDevice(driver), secondDevice);
    EXPECT_EQ(NLS::Render::Context::DriverTestAccess::PeekFrameContext(driver, 0u), nullptr);

    NLS::Core::ServiceLocator::Remove<NLS::Render::Context::Driver>();
}

TEST(ThreadedRenderingLifecycleTests, InRenderPassChildCommandRecordingFallsBackToSerialWhenChildDrawFailsBeforeParentPass)
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
    explicitDevice->MutableCapabilities().supportsParallelCommandTranslation = true;
    explicitDevice->MutableCapabilities().supportsInRenderPassChildCommandBuffers = true;
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    commandPool->commandBuffer = commandBuffer;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = std::make_shared<TestFence>();

    auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 613u;
    snapshot.targetsSwapchain = false;
    snapshot.visibleOpaqueDrawCount = 2000u;
    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessFrameSnapshot(driver, snapshot));

    size_t slotIndex = 0u;
    NLS::Render::Context::FrameSnapshot publishedSnapshot;
    ASSERT_TRUE(lifecycle->TryBeginNextRenderScene(&slotIndex, &publishedSnapshot));

    auto opaquePipeline = std::make_shared<TestGraphicsPipeline>("OpaquePipeline");
    auto materialBindingSet = std::make_shared<TestBindingSet>("MaterialBindingSet");
    auto mesh = std::make_shared<TestMesh>();

    NLS::Render::Context::RenderPassCommandInput passInput;
    passInput.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    passInput.drawCount = 2000u;
    passInput.requiresFrameData = true;
    passInput.requiresObjectData = true;
    passInput.renderWidth = 64u;
    passInput.renderHeight = 64u;
    passInput.usesColorAttachment = true;
    passInput.usesDepthStencilAttachment = true;
    passInput.recordedDrawCommands.reserve(2000u);
    for (uint32_t drawIndex = 0u; drawIndex < 2000u; ++drawIndex)
        passInput.recordedDrawCommands.push_back({ opaquePipeline, nullptr, nullptr, nullptr, materialBindingSet, mesh, 1u });

    NLS::Render::Context::RenderScenePackage package;
    package.frameId = publishedSnapshot.frameId;
    package.targetsSwapchain = false;
    package.visibleDrawCount = 2000u;
    package.opaqueDrawCount = 2000u;
    package.hasVisibleDraws = true;
    package.frameDataReady = true;
    package.objectDataReady = true;
    package.renderWidth = 64u;
    package.renderHeight = 64u;
    package.containsParallelCommandWorkUnits = true;
    package.passCommandInputs = { passInput };
    package.parallelCommandWorkUnits = NLS::Render::Context::BuildRecordedDrawCommandWorkUnitsForPass(
        passInput,
        0u,
        0u);
    package.parallelCommandWorkUnitCount = static_cast<uint64_t>(package.parallelCommandWorkUnits.size());

    ASSERT_TRUE(lifecycle->CompleteRenderScene(slotIndex, package));
    explicitDevice->FailCreatedChildCommandBufferDraws(true);

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    const auto copiedSlot = lifecycle->CopySlot(0u);
    ASSERT_TRUE(copiedSlot.has_value());
    ASSERT_TRUE(copiedSlot->submissionFrame.has_value());
    EXPECT_TRUE(copiedSlot->submissionFrame->submittedSuccessfully);
    EXPECT_FALSE(copiedSlot->submissionFrame->usedParallelCommandPath);
    EXPECT_TRUE(copiedSlot->submissionFrame->usedSerialCommandPath);
    EXPECT_EQ(copiedSlot->submissionFrame->commandRecordingFailureCount, 0u);
    EXPECT_NE(
        copiedSlot->submissionFrame->parallelFallbackReason.find("in-render-pass child command recording failed"),
        std::string::npos);
    ASSERT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    const auto submittedBuffer = GetSubmittedTestCommandBuffer(explicitDevice->GetTestQueue());
    ASSERT_NE(submittedBuffer, nullptr);
    EXPECT_EQ(submittedBuffer->executeChildCommandBufferCalls, 0u);
    EXPECT_EQ(submittedBuffer->drawIndexedCalls, 2000u);
}

TEST(ThreadedRenderingLifecycleTests, InRenderPassChildCommandRecordingRecordsDependencyVisibilityBeforeParentPass)
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
    explicitDevice->MutableCapabilities().supportsParallelCommandTranslation = true;
    explicitDevice->MutableCapabilities().supportsInRenderPassChildCommandBuffers = true;
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    commandPool->commandBuffer = commandBuffer;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = std::make_shared<TestFence>();
    frameContext.resourceStateTracker = nullptr;

    auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 611u;
    snapshot.targetsSwapchain = false;
    snapshot.visibleOpaqueDrawCount = 2000u;
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

    NLS::Render::Context::RenderPassCommandInput passInput;
    passInput.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    passInput.drawCount = 2000u;
    passInput.requiresFrameData = true;
    passInput.requiresObjectData = true;
    passInput.targetsSwapchain = false;
    passInput.renderWidth = 64u;
    passInput.renderHeight = 64u;
    passInput.clearColor = true;
    passInput.clearDepth = true;
    passInput.usesColorAttachment = true;
    passInput.usesDepthStencilAttachment = true;
    passInput.requiresDependencyVisibility = true;
    passInput.dependencySourceWorkUnitIndex = 0u;
    passInput.bufferResourceAccesses.push_back({
        visibilityBuffer,
        NLS::Render::Context::ResourceAccessMode::Read,
        NLS::Render::RHI::ResourceState::ShaderRead,
        NLS::Render::RHI::PipelineStageMask::FragmentShader,
        NLS::Render::RHI::AccessMask::ShaderRead
    });
    passInput.recordedDrawCommands.reserve(2000u);
    for (uint32_t drawIndex = 0u; drawIndex < 2000u; ++drawIndex)
    {
        passInput.recordedDrawCommands.push_back({
            graphicsPipeline,
            nullptr,
            nullptr,
            nullptr,
            materialBindingSet,
            mesh,
            1u
        });
    }

    NLS::Render::Context::ParallelCommandWorkUnit computeWorkUnit;
    computeWorkUnit.commandInput = computePassInput;
    computeWorkUnit.debugName = computePassInput.debugName;

    auto childWorkUnits = NLS::Render::Context::BuildRecordedDrawCommandWorkUnitsForPass(
        passInput,
        0u,
        1u);
    ASSERT_GT(childWorkUnits.size(), 1u);
    childWorkUnits[0].incomingDependencyEdges.push_back({
        0u,
        childWorkUnits[0].workUnitIndex,
        NLS::Render::Context::ThreadedDependencyKind::ResourceVisibility,
        NLS::Render::Context::ThreadedDependencyResourceKind::Buffer,
        NLS::Render::Context::BufferResourceAccess{
            visibilityBuffer,
            NLS::Render::Context::ResourceAccessMode::Write,
            NLS::Render::RHI::ResourceState::ShaderWrite,
            NLS::Render::RHI::PipelineStageMask::ComputeShader,
            NLS::Render::RHI::AccessMask::ShaderWrite
        },
        NLS::Render::Context::BufferResourceAccess{
            visibilityBuffer,
            NLS::Render::Context::ResourceAccessMode::Read,
            NLS::Render::RHI::ResourceState::ShaderRead,
            NLS::Render::RHI::PipelineStageMask::FragmentShader,
            NLS::Render::RHI::AccessMask::ShaderRead
        },
        std::nullopt,
        std::nullopt
    });

    NLS::Render::Context::RenderScenePackage package;
    package.frameId = publishedSnapshot.frameId;
    package.targetsSwapchain = false;
    package.visibleDrawCount = 2000u;
    package.opaqueDrawCount = 2000u;
    package.hasVisibleDraws = true;
    package.frameDataReady = true;
    package.objectDataReady = true;
    package.renderWidth = 64u;
    package.renderHeight = 64u;
    package.containsParallelCommandWorkUnits = true;
    package.passCommandInputs = { passInput };
    package.parallelCommandWorkUnits.push_back(computeWorkUnit);
    package.parallelCommandWorkUnits.insert(
        package.parallelCommandWorkUnits.end(),
        childWorkUnits.begin(),
        childWorkUnits.end());
    package.parallelCommandWorkUnitCount = static_cast<uint64_t>(package.parallelCommandWorkUnits.size());

    ASSERT_TRUE(lifecycle->CompleteRenderScene(slotIndex, package));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    const auto copiedSlot = lifecycle->CopySlot(0u);
    ASSERT_TRUE(copiedSlot.has_value());
    ASSERT_TRUE(copiedSlot->submissionFrame.has_value());
    EXPECT_TRUE(copiedSlot->submissionFrame->submittedSuccessfully);
    ASSERT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    const auto submittedBuffer = GetSubmittedTestCommandBuffer(explicitDevice->GetTestQueue());
    ASSERT_NE(submittedBuffer, nullptr);
    ASSERT_GT(submittedBuffer->barrierCalls, 0u);
    auto visibilityBarrierIt = submittedBuffer->events.end();
    size_t barrierHistoryIndex = 0u;
    for (auto eventIt = submittedBuffer->events.begin(); eventIt != submittedBuffer->events.end(); ++eventIt)
    {
        if (*eventIt != "Barrier")
            continue;

        if (barrierHistoryIndex >= submittedBuffer->barrierHistory.size())
            continue;

        const auto& barrierDesc = submittedBuffer->barrierHistory[barrierHistoryIndex++];
        const auto bufferBarrierIt = std::find_if(
            barrierDesc.bufferBarriers.begin(),
            barrierDesc.bufferBarriers.end(),
            [&visibilityBuffer](const NLS::Render::RHI::RHIBufferBarrier& barrier)
            {
                return barrier.buffer == visibilityBuffer &&
                    barrier.before == NLS::Render::RHI::ResourceState::ShaderWrite &&
                    barrier.after == NLS::Render::RHI::ResourceState::ShaderRead &&
                    barrier.sourceAccessMask == NLS::Render::RHI::AccessMask::ShaderWrite &&
                    barrier.destinationAccessMask == NLS::Render::RHI::AccessMask::ShaderRead;
            });
        if (bufferBarrierIt != barrierDesc.bufferBarriers.end())
        {
            visibilityBarrierIt = eventIt;
            break;
        }
    }
    const auto beginPassIt = std::find(
        submittedBuffer->events.begin(),
        submittedBuffer->events.end(),
        "BeginRenderPass");
    ASSERT_NE(visibilityBarrierIt, submittedBuffer->events.end());
    ASSERT_NE(beginPassIt, submittedBuffer->events.end());
    EXPECT_LT(visibilityBarrierIt, beginPassIt);
}

TEST(ThreadedRenderingLifecycleTests, AttachmentFreeRecordedDrawPassSlicesIntoOrderedSerialWorkUnits)
{
    NLS::Render::Context::RenderPassCommandInput passInput;
    passInput.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    passInput.debugName = "AttachmentFreeOpaque";
    passInput.drawCount = 2000u;
    passInput.recordedDrawCommands.resize(2000u);

    const auto workUnits = NLS::Render::Context::BuildRecordedDrawCommandWorkUnitsForPass(
        passInput,
        0u,
        0u);

    ASSERT_GT(workUnits.size(), 1u);
    uint64_t coveredDrawCount = 0u;
    for (size_t index = 0u; index < workUnits.size(); ++index)
    {
        const auto& workUnit = workUnits[index];
        EXPECT_EQ(workUnit.workUnitIndex, index);
        EXPECT_EQ(workUnit.submissionOrder, index);
        EXPECT_EQ(workUnit.sourcePassIndex, 0u);
        EXPECT_EQ(workUnit.sliceIndex, index);
        EXPECT_EQ(workUnit.sliceCount, workUnits.size());
        EXPECT_EQ(workUnit.recordedDrawBegin, coveredDrawCount);
        EXPECT_TRUE(workUnit.commandInput.recordedDrawCommands.empty());
        EXPECT_EQ(workUnit.commandInput.drawCount, workUnit.recordedDrawCount);
        EXPECT_TRUE(workUnit.requiresOrderedSlicedSubmission);
        EXPECT_FALSE(workUnit.eligibleForParallelRecording);
        EXPECT_FALSE(workUnit.eligibleForParallelTranslation);
        coveredDrawCount += workUnit.recordedDrawCount;
    }
    EXPECT_EQ(coveredDrawCount, 2000u);
}

TEST(ThreadedRenderingLifecycleTests, SlicedParallelWorkUnitsFallbackToUnslicedPassInputsWhenOrderedSubmissionIsUnsafe)
{
    NLS::Render::Context::RenderScenePackage package;
    package.containsCommandInputs = true;
    package.containsParallelCommandWorkUnits = true;

    NLS::Render::Context::ParallelCommandWorkUnit additionalWorkUnit;
    additionalWorkUnit.workUnitIndex = 0u;
    additionalWorkUnit.submissionOrder = 0u;
    additionalWorkUnit.sourcePassIndex = NLS::Render::Context::kInvalidParallelCommandSourcePassIndex;
    additionalWorkUnit.commandInput.kind = NLS::Render::Context::RenderPassCommandKind::Helper;
    additionalWorkUnit.commandInput.debugName = "FallbackSafeAdditionalWorkUnit";
    additionalWorkUnit.commandInput.drawCount = 1u;
    package.parallelCommandWorkUnits.push_back(additionalWorkUnit);

    NLS::Render::Context::RenderPassCommandInput passInput;
    passInput.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    passInput.debugName = "UnsafeFallbackOpaque";
    passInput.drawCount = 2000u;
    passInput.recordedDrawCommands.resize(2000u);
    package.passCommandInputs.push_back(passInput);

    NLS::Render::Context::RenderPassCommandInput dependentPassInput;
    dependentPassInput.kind = NLS::Render::Context::RenderPassCommandKind::Transparent;
    dependentPassInput.debugName = "UnsafeFallbackDependent";
    dependentPassInput.drawCount = 1u;
    dependentPassInput.recordedDrawCommands.resize(1u);
    package.passCommandInputs.push_back(dependentPassInput);

    auto slicedWorkUnits = NLS::Render::Context::BuildRecordedDrawCommandWorkUnitsForPass(
        passInput,
        0u,
        static_cast<uint64_t>(package.parallelCommandWorkUnits.size()));
    package.parallelCommandWorkUnits.insert(
        package.parallelCommandWorkUnits.end(),
        std::make_move_iterator(slicedWorkUnits.begin()),
        std::make_move_iterator(slicedWorkUnits.end()));
    const auto dependentWorkUnitIndex = static_cast<uint64_t>(package.parallelCommandWorkUnits.size());
    auto dependentWorkUnits = NLS::Render::Context::BuildRecordedDrawCommandWorkUnitsForPass(
        dependentPassInput,
        1u,
        dependentWorkUnitIndex);
    package.parallelCommandWorkUnits.insert(
        package.parallelCommandWorkUnits.end(),
        std::make_move_iterator(dependentWorkUnits.begin()),
        std::make_move_iterator(dependentWorkUnits.end()));
    ASSERT_GT(package.parallelCommandWorkUnits.size(), 1u);

    NLS::Render::Context::WorkUnitDependencyEdge dependency;
    dependency.sourceWorkUnitIndex = dependentWorkUnitIndex - 1u;
    dependency.targetWorkUnitIndex = dependentWorkUnitIndex;
    dependency.kind = NLS::Render::Context::ThreadedDependencyKind::ResourceVisibility;
    package.workUnitDependencyEdges.push_back(dependency);
    NLS::Render::Context::WorkUnitDependencyEdge additionalDependency;
    additionalDependency.sourceWorkUnitIndex = 0u;
    additionalDependency.targetWorkUnitIndex = dependentWorkUnitIndex;
    additionalDependency.kind = NLS::Render::Context::ThreadedDependencyKind::ResourceVisibility;
    package.workUnitDependencyEdges.push_back(additionalDependency);

    const auto unsafeWorkUnits = NLS::Render::Context::Detail::BuildParallelCommandWorkUnits(
        package,
        false,
        false,
        false);

    ASSERT_EQ(unsafeWorkUnits.size(), 3u);
    EXPECT_EQ(unsafeWorkUnits[0].sourcePassIndex, NLS::Render::Context::kInvalidParallelCommandSourcePassIndex);
    EXPECT_EQ(unsafeWorkUnits[0].commandInput.debugName, "FallbackSafeAdditionalWorkUnit");
    EXPECT_EQ(unsafeWorkUnits[1].commandInput.recordedDrawCommands.size(), 2000u);
    EXPECT_FALSE(unsafeWorkUnits[1].requiresOrderedSlicedSubmission);
    EXPECT_EQ(unsafeWorkUnits[1].sliceCount, 1u);
    ASSERT_EQ(unsafeWorkUnits[2].incomingDependencyEdges.size(), 2u);
    EXPECT_EQ(unsafeWorkUnits[2].incomingDependencyEdges[0].sourceWorkUnitIndex, 1u);
    EXPECT_EQ(unsafeWorkUnits[2].incomingDependencyEdges[0].targetWorkUnitIndex, 2u);
    EXPECT_EQ(unsafeWorkUnits[2].incomingDependencyEdges[1].sourceWorkUnitIndex, 0u);
    EXPECT_EQ(unsafeWorkUnits[2].incomingDependencyEdges[1].targetWorkUnitIndex, 2u);

    const auto orderedWorkUnits = NLS::Render::Context::Detail::BuildParallelCommandWorkUnits(
        package,
        true,
        true,
        true);

    ASSERT_EQ(orderedWorkUnits.size(), package.parallelCommandWorkUnits.size());
    const auto firstSlicedWorkUnit = std::find_if(
        orderedWorkUnits.begin(),
        orderedWorkUnits.end(),
        [](const NLS::Render::Context::ParallelCommandWorkUnit& workUnit)
        {
            return workUnit.requiresOrderedSlicedSubmission;
        });
    ASSERT_NE(firstSlicedWorkUnit, orderedWorkUnits.end());
    EXPECT_TRUE(firstSlicedWorkUnit->commandInput.recordedDrawCommands.empty());
    EXPECT_EQ(firstSlicedWorkUnit->recordedDrawCount, NLS::Render::Context::kRecordedDrawCommandSliceThreshold);
}

TEST(ThreadedRenderingLifecycleTests, RecordedDrawCommandSlicingAllowsStencilClearInParentOwnedChildRecording)
{
    NLS::Render::Context::RenderPassCommandInput passInput;
    passInput.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    passInput.debugName = "StencilClearOpaque";
    passInput.drawCount = 2000u;
    passInput.clearStencil = true;
    passInput.recordedDrawCommands.resize(2000u);

    const auto workUnits = NLS::Render::Context::BuildRecordedDrawCommandWorkUnitsForPass(
        passInput,
        0u,
        0u);

    ASSERT_GT(workUnits.size(), 1u);
    EXPECT_TRUE(workUnits[0].commandInput.recordedDrawCommands.empty());
    EXPECT_TRUE(workUnits[0].requiresOrderedSlicedSubmission);
    EXPECT_TRUE(workUnits[0].usesInRenderPassChildCommandRecording);
    EXPECT_GT(workUnits[0].sliceCount, 1u);
}

TEST(ThreadedRenderingLifecycleTests, RecordedDrawCommandSlicingRejectsPassesWithSidecarComputeDispatches)
{
    NLS::Render::Context::RenderPassCommandInput passInput;
    passInput.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    passInput.debugName = "SidecarDispatchOpaque";
    passInput.drawCount = 2000u;
    passInput.recordedDrawCommands.resize(2000u);
    passInput.computeDispatchInputs.resize(1u);

    const auto workUnits = NLS::Render::Context::BuildRecordedDrawCommandWorkUnitsForPass(
        passInput,
        0u,
        0u);

    ASSERT_EQ(workUnits.size(), 1u);
    EXPECT_EQ(workUnits[0].commandInput.recordedDrawCommands.size(), 2000u);
    EXPECT_EQ(workUnits[0].commandInput.computeDispatchInputs.size(), 1u);
    EXPECT_FALSE(workUnits[0].requiresOrderedSlicedSubmission);
    EXPECT_EQ(workUnits[0].sliceCount, 1u);
}

TEST(ThreadedRenderingLifecycleTests, RecordedDrawCommandSlicingRejectsSwapchainPasses)
{
    NLS::Render::Context::RenderPassCommandInput passInput;
    passInput.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    passInput.debugName = "SwapchainOpaque";
    passInput.drawCount = 2000u;
    passInput.targetsSwapchain = true;
    passInput.recordedDrawCommands.resize(2000u);

    const auto workUnits = NLS::Render::Context::BuildRecordedDrawCommandWorkUnitsForPass(
        passInput,
        0u,
        0u);

    ASSERT_EQ(workUnits.size(), 1u);
    EXPECT_EQ(workUnits[0].commandInput.recordedDrawCommands.size(), 2000u);
    EXPECT_FALSE(workUnits[0].requiresOrderedSlicedSubmission);
    EXPECT_EQ(workUnits[0].sliceCount, 1u);
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
    EXPECT_EQ(retiredSlot->renderScenePackage->sceneGameObjectCount, 1u);
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
    EXPECT_EQ(slot->snapshot->sceneGameObjectCount, 2u);
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
    EXPECT_EQ(slot->snapshot->sceneGameObjectCount, 2u);
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
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

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
    const auto& gbufferPass = retiredSlot->renderScenePackage->passCommandInputs[0];
    const auto& lightingPass = retiredSlot->renderScenePackage->passCommandInputs[1];
    EXPECT_EQ(gbufferPass.kind, NLS::Render::Context::RenderPassCommandKind::GBuffer);
    EXPECT_EQ(lightingPass.kind, NLS::Render::Context::RenderPassCommandKind::Lighting);
    EXPECT_TRUE(gbufferPass.usesColorAttachment);
    EXPECT_TRUE(gbufferPass.usesDepthStencilAttachment);
    ASSERT_EQ(gbufferPass.gbufferTextures.size(), 4u);
    ASSERT_EQ(gbufferPass.colorAttachmentViews.size(), 3u);
    for (const auto& texture : gbufferPass.gbufferTextures)
        EXPECT_NE(texture, nullptr);
    for (const auto& colorView : gbufferPass.colorAttachmentViews)
    {
        ASSERT_NE(colorView, nullptr);
        EXPECT_NE(colorView->GetTexture(), nullptr);
    }
    ASSERT_NE(gbufferPass.depthStencilAttachmentView, nullptr);
    EXPECT_NE(gbufferPass.depthStencilAttachmentView->GetTexture(), nullptr);
    ASSERT_EQ(gbufferPass.textureResourceAccesses.size(), 4u);
    ASSERT_EQ(lightingPass.gbufferTextures.size(), 4u);
    for (const auto& texture : lightingPass.gbufferTextures)
        EXPECT_NE(texture, nullptr);
    ASSERT_EQ(lightingPass.textureResourceAccesses.size(), 4u);
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
    snapshot.sceneGameObjectCount = 5u;
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
    EXPECT_EQ(readySlot->renderScenePackage->sceneGameObjectCount, 5u);
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

TEST(ThreadedRenderingLifecycleTests, ThreadedSubmitFailureSkipsPresentAndRetirementFenceWait)
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

    explicitDevice->GetTestQueue()->nextSubmitResult = {
        NLS::Render::RHI::RHIQueueOperationStatusCode::DeviceLost,
        "threaded submit failed"
    };

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 213u;
    snapshot.targetsSwapchain = true;

    NLS::Render::Context::RenderScenePackage renderScenePackage;
    renderScenePackage.frameId = snapshot.frameId;
    renderScenePackage.targetsSwapchain = true;
    renderScenePackage.renderWidth = 128u;
    renderScenePackage.renderHeight = 72u;
    renderScenePackage.frameDataReady = true;
    renderScenePackage.objectDataReady = true;

    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessPreparedFrame(
        driver,
        snapshot,
        renderScenePackage));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    auto testQueue = explicitDevice->GetTestQueue();
    EXPECT_EQ(testQueue->submitCalls, 0u);
    EXPECT_EQ(testQueue->presentCalls, 0u);
    EXPECT_EQ(swapchain->acquireCalls, 1u);
    EXPECT_EQ(frameFence->waitCalls, 1u);

    const auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);
    const auto* retiredSlot = lifecycle->PeekSlot(0u);
    ASSERT_NE(retiredSlot, nullptr);
    ASSERT_TRUE(retiredSlot->submissionFrame.has_value());
    EXPECT_FALSE(retiredSlot->submissionFrame->retirementFenceWaited);
    EXPECT_TRUE(retiredSlot->submissionFrame->deviceLostDetected);
    EXPECT_NE(
        retiredSlot->submissionFrame->lastQueueOperationFailure.find("threaded submit failed"),
        std::string::npos);
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
    EXPECT_FALSE(retiredSlot->submissionFrame->retirementFenceWaited);
    EXPECT_TRUE(retiredSlot->submissionFrame->deferredFrameScopedRetirement);
    EXPECT_EQ(frameFence->waitCalls, 1u);
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

TEST(ThreadedRenderingLifecycleTests, RejectedOffscreenFrameFinalizesContextBeforeNextSubmission)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    settings.framesInFlight = 2u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);
    NLS::Render::Context::DriverTestAccess::RebuildExplicitFrameContexts(driver, 2u);

    auto& firstFrameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto firstCommandBuffer = std::make_shared<TestCommandBuffer>();
    auto firstCommandPool = std::make_shared<TestCommandPool>();
    auto firstFrameFence = std::make_shared<TestFence>();
    auto firstDescriptorAllocator = std::make_shared<TestDescriptorAllocator>();
    auto firstUploadContext = std::make_shared<TestUploadContext>();
    firstCommandPool->commandBuffer = firstCommandBuffer;
    firstFrameContext.commandBuffer = firstCommandBuffer;
    firstFrameContext.commandPool = firstCommandPool;
    firstFrameContext.frameFence = firstFrameFence;
    firstFrameContext.descriptorAllocator = firstDescriptorAllocator;
    firstFrameContext.uploadContext = firstUploadContext;

    auto& secondFrameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 1u);
    auto secondCommandBuffer = std::make_shared<TestCommandBuffer>();
    auto secondCommandPool = std::make_shared<TestCommandPool>();
    auto secondFrameFence = std::make_shared<TestFence>();
    secondCommandPool->commandBuffer = secondCommandBuffer;
    secondFrameContext.commandBuffer = secondCommandBuffer;
    secondFrameContext.commandPool = secondCommandPool;
    secondFrameContext.frameFence = secondFrameFence;
    secondFrameContext.resourceStateTracker = NLS::Render::RHI::CreateDefaultResourceStateTracker();

    NLS::Render::Context::FrameSnapshot rejectedSnapshot;
    rejectedSnapshot.frameId = 131u;
    rejectedSnapshot.targetsSwapchain = false;
    rejectedSnapshot.visibleOpaqueDrawCount = 1u;

    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessFrameSnapshot(driver, rejectedSnapshot));
    explicitDevice->SetNativeBackendType(NLS::Render::RHI::NativeBackendType::None);
    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 0u);
    EXPECT_EQ(firstDescriptorAllocator->beginFrameCalls, 1u);
    EXPECT_EQ(firstDescriptorAllocator->endFrameCalls, 1u);
    EXPECT_EQ(firstUploadContext->beginFrameCalls, 1u);
    EXPECT_EQ(firstUploadContext->endFrameCalls, 1u);
    EXPECT_EQ(firstFrameFence->resetCalls, 0u)
        << "Offscreen frames with no GPU submit must not leave a reset unsignaled fence behind.";

    const auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);
    auto telemetry = lifecycle->GetTelemetry();
    ASSERT_EQ(telemetry.latestRetiredFrameId, 0u);
    ASSERT_EQ(telemetry.latestFailedRetiredFrameId, rejectedSnapshot.frameId);

    NLS::Render::Context::FrameSnapshot submittedSnapshot;
    submittedSnapshot.frameId = 132u;
    submittedSnapshot.targetsSwapchain = false;
    submittedSnapshot.visibleOpaqueDrawCount = 1u;

    NLS::Render::Context::RenderScenePackage submittedPackage;
    submittedPackage.frameId = submittedSnapshot.frameId;
    submittedPackage.targetsSwapchain = false;
    submittedPackage.visibleDrawCount = 1u;
    submittedPackage.opaqueDrawCount = 1u;
    submittedPackage.hasVisibleDraws = true;
    submittedPackage.frameDataReady = true;
    submittedPackage.objectDataReady = true;
    submittedPackage.passPlanCount = 1u;

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
    submittedPackage.passCommandInputs.push_back(passInput);

    explicitDevice->SetNativeBackendType(NLS::Render::RHI::NativeBackendType::DX12);
    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessPreparedFrame(
        driver,
        submittedSnapshot,
        submittedPackage));
    explicitDevice->SetNativeBackendType(NLS::Render::RHI::NativeBackendType::None);
    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    auto submittedCommandBuffer = GetSubmittedTestCommandBuffer(explicitDevice->GetTestQueue());
    EXPECT_EQ(submittedCommandBuffer, firstCommandBuffer)
        << "Threaded RHI submissions must use the lifecycle-claimed reusable slot, not currentFrameIndex modulo.";
    EXPECT_EQ(firstFrameContext.frameIndex, 1u);
    EXPECT_EQ(secondFrameContext.frameIndex, 1u);

    telemetry = lifecycle->GetTelemetry();
    EXPECT_EQ(telemetry.latestRetiredFrameId, submittedSnapshot.frameId);
    EXPECT_EQ(telemetry.latestFailedRetiredFrameId, rejectedSnapshot.frameId);
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
    ScopedThreadedRenderingJobSystem jobSystem(2u);
    ASSERT_TRUE(jobSystem.IsInitialized());

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
    frameContext.resourceStateTracker = nullptr;

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

TEST(ThreadedRenderingLifecycleTests, ParallelRecordingFallsBackToSerialWhenResourceStateTrackerIsActive)
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
    snapshot.frameId = 406u;
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
    opaqueWorkUnit.debugName = "TrackedOpaque";
    opaqueWorkUnit.eligibleForParallelRecording = true;

    NLS::Render::Context::ParallelCommandWorkUnit transparentWorkUnit;
    transparentWorkUnit.commandInput = transparentPassInput;
    transparentWorkUnit.debugName = "TrackedTransparent";
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
    EXPECT_EQ(copiedSlot->submissionFrame->parallelRecordingWorkerCount, 0u);
    EXPECT_FALSE(copiedSlot->submissionFrame->usedParallelCommandPath);
    EXPECT_TRUE(copiedSlot->submissionFrame->usedSerialCommandPath);
    EXPECT_EQ(explicitDevice->GetCreatedCommandPools().size(), 2u)
        << "The ordered work-unit path may still use per-pass command buffers, but it must not record them concurrently while sharing the frame resource-state tracker.";
}

TEST(ThreadedRenderingLifecycleTests, ParallelRecordingFailureMarksSubmissionAsFailedRetirement)
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
    explicitDevice->FailNextCommandBufferBeginCalls(1u);
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    auto frameFence = std::make_shared<TestFence>();
    commandPool->commandBuffer = commandBuffer;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = frameFence;
    frameContext.resourceStateTracker = nullptr;

    auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 407u;
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

    NLS::Render::Context::RenderPassCommandInput passInput;
    passInput.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    passInput.drawCount = 1u;
    passInput.requiresFrameData = true;
    passInput.requiresObjectData = true;
    passInput.targetsSwapchain = false;
    passInput.renderWidth = 64u;
    passInput.renderHeight = 64u;
    passInput.clearColor = true;
    passInput.usesColorAttachment = true;
    passInput.recordedDrawCommands.push_back({ opaquePipeline, nullptr, nullptr, nullptr, materialBindingSet, mesh, 1u });

    NLS::Render::Context::ParallelCommandWorkUnit firstWorkUnit;
    firstWorkUnit.commandInput = passInput;
    firstWorkUnit.debugName = "ParallelFailureA";
    firstWorkUnit.eligibleForParallelRecording = true;

    NLS::Render::Context::ParallelCommandWorkUnit secondWorkUnit;
    secondWorkUnit.commandInput = passInput;
    secondWorkUnit.commandInput.kind = NLS::Render::Context::RenderPassCommandKind::Transparent;
    secondWorkUnit.commandInput.recordedDrawCommands = {
        { transparentPipeline, nullptr, nullptr, nullptr, materialBindingSet, mesh, 1u }
    };
    secondWorkUnit.debugName = "ParallelFailureB";
    secondWorkUnit.eligibleForParallelRecording = true;

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
    package.parallelCommandWorkUnits = { firstWorkUnit, secondWorkUnit };

    ASSERT_TRUE(lifecycle->CompleteRenderScene(slotIndex, package));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    const auto copiedSlot = lifecycle->CopySlot(0u);
    ASSERT_TRUE(copiedSlot.has_value());
    ASSERT_TRUE(copiedSlot->submissionFrame.has_value());
    EXPECT_FALSE(copiedSlot->submissionFrame->submittedSuccessfully);
    EXPECT_FALSE(copiedSlot->submissionFrame->recordedVisibleWork);
    EXPECT_GT(copiedSlot->submissionFrame->commandRecordingFailureCount, 0u);
    EXPECT_NE(
        copiedSlot->submissionFrame->lastCommandRecordingFailure.find("BeginPassCommandPlan failed"),
        std::string::npos);

    const auto telemetry = lifecycle->GetTelemetry();
    EXPECT_EQ(telemetry.latestRetiredFrameId, 0u);
    EXPECT_EQ(telemetry.latestFailedRetiredFrameId, snapshot.frameId);
}

TEST(ThreadedRenderingLifecycleTests, SerialRecordingFailureMarksSubmissionAsFailedRetirement)
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
    snapshot.frameId = 412u;
    snapshot.targetsSwapchain = false;
    snapshot.hasExternalOutput = true;
    snapshot.externalOutputTextureCount = 1u;
    snapshot.visibleOpaqueDrawCount = 1u;
    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessFrameSnapshot(driver, snapshot));

    size_t slotIndex = 0u;
    NLS::Render::Context::FrameSnapshot publishedSnapshot;
    ASSERT_TRUE(lifecycle->TryBeginNextRenderScene(&slotIndex, &publishedSnapshot));

    NLS::Render::RHI::RHITextureDesc colorDesc;
    colorDesc.debugName = "EditorSceneColor";
    colorDesc.extent.width = 64u;
    colorDesc.extent.height = 64u;
    colorDesc.extent.depth = 1u;
    colorDesc.arrayLayers = 1u;
    colorDesc.mipLevels = 1u;
    colorDesc.usage =
        NLS::Render::RHI::TextureUsageFlags::ColorAttachment |
        NLS::Render::RHI::TextureUsageFlags::Sampled;
    auto colorTexture = std::make_shared<TestTexture>(colorDesc);
    auto colorView = std::make_shared<TestTextureView>(
        colorTexture,
        NLS::Render::RHI::RHITextureViewDesc{});

    auto graphicsPipeline = std::make_shared<TestGraphicsPipeline>("ScenePipeline");
    auto materialBindingSet = std::make_shared<TestBindingSet>("MaterialBindingSet");
    auto mesh = std::make_shared<TestMesh>();

    NLS::Render::Context::RenderPassCommandInput passInput;
    passInput.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    passInput.drawCount = 1u;
    passInput.requiresFrameData = true;
    passInput.requiresObjectData = true;
    passInput.targetsSwapchain = false;
    passInput.renderWidth = 64u;
    passInput.renderHeight = 64u;
    passInput.clearColor = true;
    passInput.usesColorAttachment = true;
    passInput.colorAttachmentViews.push_back(colorView);
    passInput.recordedDrawCommands.push_back({ graphicsPipeline, nullptr, nullptr, nullptr, nullptr, mesh, 1u });

    NLS::Render::Context::ParallelCommandWorkUnit workUnit;
    workUnit.commandInput = passInput;
    workUnit.debugName = "ExternalSceneBrokenDraw";
    workUnit.eligibleForParallelRecording = true;

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
    package.parallelCommandWorkUnitCount = 1u;
    package.parallelCommandWorkUnits = { workUnit };
    package.extractedTextures = { colorTexture };
    package.externalSceneOutputTextureCount = 1u;

    ASSERT_TRUE(lifecycle->CompleteRenderScene(slotIndex, package));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    const auto copiedSlot = lifecycle->CopySlot(0u);
    ASSERT_TRUE(copiedSlot.has_value());
    ASSERT_TRUE(copiedSlot->submissionFrame.has_value());
    EXPECT_TRUE(copiedSlot->submissionFrame->usedSerialCommandPath);
    EXPECT_FALSE(copiedSlot->submissionFrame->submittedSuccessfully);
    EXPECT_GT(copiedSlot->submissionFrame->commandRecordingFailureCount, 0u);
    EXPECT_NE(
        copiedSlot->submissionFrame->lastCommandRecordingFailure.find("No draws recorded but commands were expected"),
        std::string::npos);
    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 0u);

    const auto telemetry = lifecycle->GetTelemetry();
    EXPECT_EQ(telemetry.latestRetiredFrameId, 0u);
    EXPECT_EQ(telemetry.latestFailedRetiredFrameId, snapshot.frameId);
}

TEST(ThreadedRenderingLifecycleTests, SerialRecordingFailureBlocksPreviouslyRecordedSubmit)
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
    snapshot.frameId = 414u;
    snapshot.targetsSwapchain = false;
    snapshot.visibleOpaqueDrawCount = 2u;
    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessFrameSnapshot(driver, snapshot));

    size_t slotIndex = 0u;
    NLS::Render::Context::FrameSnapshot publishedSnapshot;
    ASSERT_TRUE(lifecycle->TryBeginNextRenderScene(&slotIndex, &publishedSnapshot));

    auto graphicsPipeline = std::make_shared<TestGraphicsPipeline>("ScenePipeline");
    auto materialBindingSet = std::make_shared<TestBindingSet>("MaterialBindingSet");
    auto mesh = std::make_shared<TestMesh>();

    NLS::Render::Context::RenderPassCommandInput goodPassInput;
    goodPassInput.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    goodPassInput.drawCount = 1u;
    goodPassInput.requiresFrameData = true;
    goodPassInput.requiresObjectData = true;
    goodPassInput.targetsSwapchain = false;
    goodPassInput.renderWidth = 64u;
    goodPassInput.renderHeight = 64u;
    goodPassInput.clearColor = true;
    goodPassInput.usesColorAttachment = true;
    goodPassInput.recordedDrawCommands.push_back({ graphicsPipeline, nullptr, nullptr, nullptr, materialBindingSet, mesh, 1u });

    NLS::Render::Context::RenderPassCommandInput badPassInput = goodPassInput;
    badPassInput.kind = NLS::Render::Context::RenderPassCommandKind::Transparent;
    badPassInput.clearColor = false;
    badPassInput.recordedDrawCommands = {
        { graphicsPipeline, nullptr, nullptr, nullptr, nullptr, mesh, 1u }
    };

    NLS::Render::Context::ParallelCommandWorkUnit goodWorkUnit;
    goodWorkUnit.commandInput = goodPassInput;
    goodWorkUnit.debugName = "RecordedBeforeFailure";
    goodWorkUnit.eligibleForParallelRecording = true;

    NLS::Render::Context::ParallelCommandWorkUnit badWorkUnit;
    badWorkUnit.commandInput = badPassInput;
    badWorkUnit.debugName = "FailsAfterRecordedWork";
    badWorkUnit.eligibleForParallelRecording = true;

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
    package.parallelCommandWorkUnits = { goodWorkUnit, badWorkUnit };

    ASSERT_TRUE(lifecycle->CompleteRenderScene(slotIndex, package));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    const auto copiedSlot = lifecycle->CopySlot(0u);
    ASSERT_TRUE(copiedSlot.has_value());
    ASSERT_TRUE(copiedSlot->submissionFrame.has_value());
    EXPECT_TRUE(copiedSlot->submissionFrame->usedSerialCommandPath);
    EXPECT_FALSE(copiedSlot->submissionFrame->submittedSuccessfully);
    EXPECT_FALSE(copiedSlot->submissionFrame->recordedVisibleWork);
    EXPECT_EQ(copiedSlot->submissionFrame->recordedPassCount, 1u);
    EXPECT_EQ(copiedSlot->submissionFrame->recordedDrawCount, 1u);
    EXPECT_GT(copiedSlot->submissionFrame->commandRecordingFailureCount, 0u);
    EXPECT_NE(
        copiedSlot->submissionFrame->lastCommandRecordingFailure.find("No draws recorded but commands were expected"),
        std::string::npos);
    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 0u);
    EXPECT_EQ(frameFence->waitCalls, 1u);

    const auto telemetry = lifecycle->GetTelemetry();
    EXPECT_EQ(telemetry.latestRetiredFrameId, 0u);
    EXPECT_EQ(telemetry.latestFailedRetiredFrameId, snapshot.frameId);
}

TEST(ThreadedRenderingLifecycleTests, ExternalSceneOutputUsesSerialCommandPathEvenWhenParallelRecordingIsAvailable)
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
    snapshot.frameId = 410u;
    snapshot.targetsSwapchain = false;
    snapshot.hasExternalOutput = true;
    snapshot.externalOutputTextureCount = 2u;
    snapshot.visibleOpaqueDrawCount = 2u;
    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessFrameSnapshot(driver, snapshot));

    size_t slotIndex = 0u;
    NLS::Render::Context::FrameSnapshot publishedSnapshot;
    ASSERT_TRUE(lifecycle->TryBeginNextRenderScene(&slotIndex, &publishedSnapshot));

    NLS::Render::RHI::RHITextureDesc colorDesc;
    colorDesc.debugName = "EditorSceneColor";
    colorDesc.extent.width = 64u;
    colorDesc.extent.height = 64u;
    colorDesc.extent.depth = 1u;
    colorDesc.arrayLayers = 1u;
    colorDesc.mipLevels = 1u;
    colorDesc.usage =
        NLS::Render::RHI::TextureUsageFlags::ColorAttachment |
        NLS::Render::RHI::TextureUsageFlags::Sampled;
    auto colorTexture = std::make_shared<TestTexture>(colorDesc);
    auto colorView = std::make_shared<TestTextureView>(
        colorTexture,
        NLS::Render::RHI::RHITextureViewDesc{});

    NLS::Render::RHI::RHITextureDesc depthDesc = colorDesc;
    depthDesc.debugName = "EditorSceneDepth";
    depthDesc.usage = NLS::Render::RHI::TextureUsageFlags::DepthStencilAttachment;
    auto depthTexture = std::make_shared<TestTexture>(depthDesc);
    auto depthView = std::make_shared<TestTextureView>(
        depthTexture,
        NLS::Render::RHI::RHITextureViewDesc{});

    auto graphicsPipeline = std::make_shared<TestGraphicsPipeline>("ScenePipeline");
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
    opaquePassInput.colorAttachmentViews.push_back(colorView);
    opaquePassInput.depthStencilAttachmentView = depthView;
    opaquePassInput.recordedDrawCommands.push_back({ graphicsPipeline, nullptr, nullptr, nullptr, materialBindingSet, mesh, 1u });

    NLS::Render::Context::RenderPassCommandInput transparentPassInput = opaquePassInput;
    transparentPassInput.kind = NLS::Render::Context::RenderPassCommandKind::Transparent;
    transparentPassInput.clearColor = false;
    transparentPassInput.clearDepth = false;
    transparentPassInput.usesDepthStencilAttachment = false;

    NLS::Render::Context::ParallelCommandWorkUnit opaqueWorkUnit;
    opaqueWorkUnit.commandInput = opaquePassInput;
    opaqueWorkUnit.debugName = "ExternalSceneOpaque";
    opaqueWorkUnit.eligibleForParallelRecording = true;

    NLS::Render::Context::ParallelCommandWorkUnit transparentWorkUnit;
    transparentWorkUnit.commandInput = transparentPassInput;
    transparentWorkUnit.debugName = "ExternalSceneTransparent";
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
    package.extractedTextures = { colorTexture, depthTexture };
    package.externalSceneOutputTextureCount = 2u;

    ASSERT_TRUE(lifecycle->CompleteRenderScene(slotIndex, package));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    const auto copiedSlot = lifecycle->CopySlot(0u);
    ASSERT_TRUE(copiedSlot.has_value());
    ASSERT_TRUE(copiedSlot->submissionFrame.has_value());
    EXPECT_TRUE(copiedSlot->submissionFrame->usedExternalOutputBridge);
    EXPECT_EQ(copiedSlot->submissionFrame->externalOutputTextureCount, 1u);
    EXPECT_FALSE(copiedSlot->submissionFrame->usedParallelCommandPath);
    EXPECT_TRUE(copiedSlot->submissionFrame->usedSerialCommandPath);
    EXPECT_EQ(copiedSlot->submissionFrame->parallelRecordingWorkerCount, 0u);
    EXPECT_EQ(explicitDevice->GetCreatedCommandPools().size(), 0u);
    ASSERT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    ASSERT_EQ(explicitDevice->GetTestQueue()->lastSubmitDesc.commandBuffers.size(), 1u);
    EXPECT_EQ(explicitDevice->GetTestQueue()->lastSubmitDesc.commandBuffers.front(), frameContext.commandBuffer);
    EXPECT_EQ(commandBuffer->beginRenderPassCalls, 2u);
}

TEST(ThreadedRenderingLifecycleTests, ExternalSceneOutputTelemetryIgnoresPreferredReadbackTexture)
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
    snapshot.frameId = 411u;
    snapshot.targetsSwapchain = false;
    snapshot.hasExternalOutput = true;
    snapshot.externalOutputTextureCount = 1u;
    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessFrameSnapshot(driver, snapshot));

    size_t slotIndex = 0u;
    NLS::Render::Context::FrameSnapshot publishedSnapshot;
    ASSERT_TRUE(lifecycle->TryBeginNextRenderScene(&slotIndex, &publishedSnapshot));

    NLS::Render::RHI::RHITextureDesc sceneDesc;
    sceneDesc.debugName = "EditorSceneColor";
    sceneDesc.extent.width = 64u;
    sceneDesc.extent.height = 64u;
    sceneDesc.extent.depth = 1u;
    sceneDesc.arrayLayers = 1u;
    sceneDesc.mipLevels = 1u;
    sceneDesc.usage =
        NLS::Render::RHI::TextureUsageFlags::ColorAttachment |
        NLS::Render::RHI::TextureUsageFlags::Sampled;
    auto sceneTexture = std::make_shared<TestTexture>(sceneDesc);
    auto sceneView = std::make_shared<TestTextureView>(
        sceneTexture,
        NLS::Render::RHI::RHITextureViewDesc{});

    NLS::Render::RHI::RHITextureDesc pickingDesc = sceneDesc;
    pickingDesc.debugName = "PickingReadbackColor";
    auto pickingTexture = std::make_shared<TestTexture>(pickingDesc);

    auto graphicsPipeline = std::make_shared<TestGraphicsPipeline>("ScenePipeline");
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
    opaquePassInput.usesColorAttachment = true;
    opaquePassInput.colorAttachmentViews.push_back(sceneView);
    opaquePassInput.recordedDrawCommands.push_back({ graphicsPipeline, nullptr, nullptr, nullptr, materialBindingSet, mesh, 1u });

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
    package.extractedTextures = { sceneTexture, pickingTexture };
    package.preferredReadbackTexture = pickingTexture;
    package.externalSceneOutputTextureCount = 1u;

    ASSERT_TRUE(lifecycle->CompleteRenderScene(slotIndex, package));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    const auto copiedSlot = lifecycle->CopySlot(0u);
    ASSERT_TRUE(copiedSlot.has_value());
    ASSERT_TRUE(copiedSlot->submissionFrame.has_value());
    EXPECT_TRUE(copiedSlot->submissionFrame->usedExternalOutputBridge);
    EXPECT_EQ(copiedSlot->submissionFrame->externalOutputTextureCount, 1u);
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
    EXPECT_EQ(descriptorAllocator->endFrameCalls, 0u);
    EXPECT_EQ(uploadContext->beginFrameCalls, 1u);
    EXPECT_EQ(uploadContext->endFrameCalls, 0u);
    EXPECT_EQ(uploadContext->lastBeginFrameIndex, 0u);
    EXPECT_EQ(uploadContext->lastEndFrameIndex, 0u);
    EXPECT_TRUE(copiedSlot->submissionFrame->deferredFrameScopedRetirement);

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
    gbufferTextureDesc.arrayLayers = 2u;
    gbufferTextureDesc.mipLevels = 2u;
    gbufferTextureDesc.usage =
        NLS::Render::RHI::TextureUsageFlags::ColorAttachment |
        NLS::Render::RHI::TextureUsageFlags::Sampled;
    auto gbufferTexture = std::make_shared<TestTexture>(gbufferTextureDesc);
    NLS::Render::RHI::RHITextureViewDesc gbufferViewDesc;
    gbufferViewDesc.subresourceRange.baseMipLevel = 0u;
    gbufferViewDesc.subresourceRange.mipLevelCount = 1u;
    gbufferViewDesc.subresourceRange.baseArrayLayer = 0u;
    gbufferViewDesc.subresourceRange.arrayLayerCount = 1u;
    auto gbufferColorView = std::make_shared<TestTextureView>(
        gbufferTexture,
        gbufferViewDesc);

    NLS::Render::RHI::RHITextureDesc gbufferDepthTextureDesc = gbufferTextureDesc;
    gbufferDepthTextureDesc.debugName = "GBufferDepth";
    gbufferDepthTextureDesc.usage =
        NLS::Render::RHI::TextureUsageFlags::DepthStencilAttachment |
        NLS::Render::RHI::TextureUsageFlags::Sampled;
    auto gbufferDepthTexture = std::make_shared<TestTexture>(gbufferDepthTextureDesc);
    auto gbufferDepthView = std::make_shared<TestTextureView>(
        gbufferDepthTexture,
        gbufferViewDesc);
    const auto viewTrackedRange = gbufferDepthView->GetDesc().subresourceRange;
    const auto colorViewTrackedRange = gbufferColorView->GetDesc().subresourceRange;

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
    gbufferPassInput.gbufferTextures.push_back(gbufferDepthTexture);
    gbufferPassInput.textureResourceAccesses.push_back({
        gbufferTexture,
        colorViewTrackedRange,
        NLS::Render::Context::ResourceAccessMode::Write,
        NLS::Render::RHI::ResourceState::RenderTarget,
        NLS::Render::RHI::PipelineStageMask::AllCommands,
        NLS::Render::RHI::AccessMask::MemoryRead | NLS::Render::RHI::AccessMask::MemoryWrite
    });
    gbufferPassInput.textureResourceAccesses.push_back({
        gbufferDepthTexture,
        viewTrackedRange,
        NLS::Render::Context::ResourceAccessMode::Write,
        NLS::Render::RHI::ResourceState::DepthWrite,
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
        colorViewTrackedRange,
        NLS::Render::Context::ResourceAccessMode::Read,
        NLS::Render::RHI::ResourceState::ShaderRead,
        NLS::Render::RHI::PipelineStageMask::FragmentShader | NLS::Render::RHI::PipelineStageMask::ComputeShader,
        NLS::Render::RHI::AccessMask::ShaderRead
    });
    lightingPassInput.textureResourceAccesses.push_back({
        gbufferDepthTexture,
        viewTrackedRange,
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
    package.extractedTextures = { gbufferTexture, gbufferDepthTexture };

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
    ASSERT_GE(explicitDevice->GetTestQueue()->lastSubmitDesc.commandBuffers.size(), 3u);
    auto firstSubmittedBuffer = GetSubmittedTestCommandBuffer(explicitDevice->GetTestQueue(), 0u);
    auto secondSubmittedBuffer = GetSubmittedTestCommandBuffer(explicitDevice->GetTestQueue(), 1u);
    auto thirdSubmittedBuffer = GetSubmittedTestCommandBuffer(explicitDevice->GetTestQueue(), 2u);
    ASSERT_NE(firstSubmittedBuffer, nullptr);
    ASSERT_NE(secondSubmittedBuffer, nullptr);
    ASSERT_NE(thirdSubmittedBuffer, nullptr);
    EXPECT_EQ(firstSubmittedBuffer->beginRenderPassCalls, 1u);
    EXPECT_GT(secondSubmittedBuffer->barrierCalls, 0u);
    EXPECT_EQ(thirdSubmittedBuffer->beginRenderPassCalls, 1u);

    const auto matchesRange =
        [](const NLS::Render::RHI::RHISubresourceRange& lhs, const NLS::Render::RHI::RHISubresourceRange& rhs)
    {
        return lhs.baseMipLevel == rhs.baseMipLevel &&
            lhs.mipLevelCount == rhs.mipLevelCount &&
            lhs.baseArrayLayer == rhs.baseArrayLayer &&
            lhs.arrayLayerCount == rhs.arrayLayerCount;
    };
    const auto isColorAttachmentEndTransition =
        [&gbufferTexture, &colorViewTrackedRange, &matchesRange](const NLS::Render::RHI::RHITextureBarrier& textureBarrier)
    {
        return textureBarrier.texture == gbufferTexture &&
            textureBarrier.before == NLS::Render::RHI::ResourceState::RenderTarget &&
            textureBarrier.after == NLS::Render::RHI::ResourceState::ShaderRead &&
            matchesRange(textureBarrier.subresourceRange, colorViewTrackedRange);
    };
    const auto countColorAttachmentEndTransitions =
        [&isColorAttachmentEndTransition](const TestCommandBuffer& submittedBuffer)
    {
        uint32_t count = 0u;
        for (const auto& barrierDesc : submittedBuffer.barrierHistory)
        {
            for (const auto& textureBarrier : barrierDesc.textureBarriers)
            {
                if (isColorAttachmentEndTransition(textureBarrier))
                    ++count;
            }
        }
        return count;
    };

    EXPECT_EQ(countColorAttachmentEndTransitions(*firstSubmittedBuffer), 1u);
    EXPECT_EQ(countColorAttachmentEndTransitions(*secondSubmittedBuffer), 0u);
    EXPECT_EQ(countColorAttachmentEndTransitions(*thirdSubmittedBuffer), 0u);
    for (size_t commandBufferIndex = 1u;
        commandBufferIndex < explicitDevice->GetTestQueue()->lastSubmitDesc.commandBuffers.size();
        ++commandBufferIndex)
    {
        const auto submittedBuffer = GetSubmittedTestCommandBuffer(
            explicitDevice->GetTestQueue(),
            commandBufferIndex);
        ASSERT_NE(submittedBuffer, nullptr);
        EXPECT_EQ(countColorAttachmentEndTransitions(*submittedBuffer), 0u);
    }

    bool foundDepthVisibilityTransition = false;
    bool foundStaleColorVisibilityTransition = false;
    bool foundStaleDepthVisibilityTransition = false;
    for (const auto& submittedBufferBase : explicitDevice->GetTestQueue()->lastSubmitDesc.commandBuffers)
    {
        const auto submittedBuffer = std::dynamic_pointer_cast<TestCommandBuffer>(submittedBufferBase);
        if (submittedBuffer == nullptr)
            continue;

        for (const auto& barrierDesc : submittedBuffer->barrierHistory)
        {
            for (const auto& textureBarrier : barrierDesc.textureBarriers)
            {
                if (textureBarrier.texture == gbufferDepthTexture &&
                    textureBarrier.before ==
                        (NLS::Render::RHI::ResourceState::DepthRead |
                         NLS::Render::RHI::ResourceState::ShaderRead) &&
                    textureBarrier.after == NLS::Render::RHI::ResourceState::ShaderRead)
                {
                    foundDepthVisibilityTransition = true;
                    EXPECT_TRUE(matchesRange(textureBarrier.subresourceRange, viewTrackedRange));
                    EXPECT_EQ(
                        textureBarrier.sourceAccessMask,
                        NLS::Render::RHI::AccessMask::DepthStencilRead | NLS::Render::RHI::AccessMask::ShaderRead);
                }

                if (textureBarrier.texture == gbufferTexture &&
                    textureBarrier.before == NLS::Render::RHI::ResourceState::RenderTarget &&
                    textureBarrier.after == NLS::Render::RHI::ResourceState::ShaderRead &&
                    !matchesRange(textureBarrier.subresourceRange, colorViewTrackedRange))
                {
                    foundStaleColorVisibilityTransition = true;
                }

                if (textureBarrier.texture == gbufferDepthTexture &&
                    textureBarrier.before == NLS::Render::RHI::ResourceState::DepthWrite &&
                    textureBarrier.after == NLS::Render::RHI::ResourceState::ShaderRead)
                {
                    foundStaleDepthVisibilityTransition = true;
                }
            }
        }
    }
    EXPECT_TRUE(foundDepthVisibilityTransition);
    EXPECT_FALSE(foundStaleColorVisibilityTransition);
    EXPECT_FALSE(foundStaleDepthVisibilityTransition);

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

TEST(ThreadedRenderingLifecycleTests, TranslationVisibilityBatchFailureMarksSubmissionAsFailedRetirement)
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
    explicitDevice->FailCreatedCommandPoolAtCall(3u);
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
    snapshot.frameId = 413u;
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
    EXPECT_FALSE(copiedSlot->submissionFrame->submittedSuccessfully);
    EXPECT_GT(copiedSlot->submissionFrame->commandRecordingFailureCount, 0u);
    EXPECT_NE(
        copiedSlot->submissionFrame->lastCommandRecordingFailure.find("Dependency visibility transition recording failed"),
        std::string::npos);
    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 0u);

    const auto telemetry = lifecycle->GetTelemetry();
    EXPECT_EQ(telemetry.latestRetiredFrameId, 0u);
    EXPECT_EQ(telemetry.latestFailedRetiredFrameId, snapshot.frameId);
}

TEST(ThreadedRenderingLifecycleTests, TranslationVisibilityBarrierFailureMarksSubmissionAsFailedRetirement)
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
    explicitDevice->FailCommandBufferBarrierCheckedAtCommandPoolCall(3u);
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
    snapshot.frameId = 415u;
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
    EXPECT_FALSE(copiedSlot->submissionFrame->submittedSuccessfully);
    EXPECT_GT(copiedSlot->submissionFrame->commandRecordingFailureCount, 0u);
    EXPECT_NE(
        copiedSlot->submissionFrame->lastCommandRecordingFailure.find("Dependency visibility transition recording failed"),
        std::string::npos);
    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 0u);
    const auto createdCommandPools = explicitDevice->GetCreatedCommandPools();
    ASSERT_GE(createdCommandPools.size(), 3u);
    auto visibilityCommandBuffer =
        std::dynamic_pointer_cast<TestCommandBuffer>(createdCommandPools[2]->commandBuffer);
    ASSERT_NE(visibilityCommandBuffer, nullptr);
    EXPECT_GT(visibilityCommandBuffer->barrierCheckedCalls, 0u);

    const auto telemetry = lifecycle->GetTelemetry();
    EXPECT_EQ(telemetry.latestRetiredFrameId, 0u);
    EXPECT_EQ(telemetry.latestFailedRetiredFrameId, snapshot.frameId);
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

TEST(ThreadedRenderingLifecycleTests, RhiWorkerMarksBeginPassFailureAsFailedRetirement)
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
    commandBuffer->stayNonRecordingOnBegin = true;
    commandPool->commandBuffer = commandBuffer;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = frameFence;
    frameContext.resourceStateTracker = nullptr;

    auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 405u;
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
    passInput.usesColorAttachment = true;
    package.passCommandInputs.push_back(passInput);

    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessPreparedFrame(
        driver,
        snapshot,
        package));
    explicitDevice->SetNativeBackendType(NLS::Render::RHI::NativeBackendType::None);
    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    const auto copiedSlot = lifecycle->CopySlot(0u);
    ASSERT_TRUE(copiedSlot.has_value());
    ASSERT_TRUE(copiedSlot->submissionFrame.has_value());
    EXPECT_FALSE(copiedSlot->submissionFrame->submittedSuccessfully);
    EXPECT_FALSE(copiedSlot->submissionFrame->recordedVisibleWork);
    EXPECT_EQ(copiedSlot->submissionFrame->recordedPassCount, 0u);
    EXPECT_EQ(commandBuffer->beginRenderPassCalls, 0u);
    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 0u);

    const auto telemetry = lifecycle->GetTelemetry();
    EXPECT_EQ(telemetry.latestRetiredFrameId, 0u);
    EXPECT_EQ(telemetry.latestFailedRetiredFrameId, snapshot.frameId);
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
    NLS::Render::RHI::RHITextureDesc outputTextureDesc;
    outputTextureDesc.extent = { 64u, 64u, 1u };
    outputTextureDesc.usage = NLS::Render::RHI::TextureUsageFlags::ColorAttachment;
    outputTextureDesc.debugName = "OffscreenThreadedOutput";
    frameDescriptor.outputColorTexture = std::make_shared<TestTexture>(outputTextureDesc);

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

TEST(ThreadedRenderingLifecycleTests, DriverShutdownQuarantinePreservesLifecycleRecordedDrawResources)
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

        NLS::Render::Context::FrameSnapshot snapshot;
        snapshot.frameId = 162u;
        snapshot.targetsSwapchain = false;
        snapshot.renderWidth = 64u;
        snapshot.renderHeight = 64u;
        snapshot.visibleOpaqueDrawCount = 1u;

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

        auto* impl = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver);
        ASSERT_NE(impl, nullptr);
        impl->unsafeGpuWorkQuarantined.store(true, std::memory_order_release);
        impl->unsafeGpuWorkQuarantineReason = "test quarantine";
        bindingSet.reset();
        ASSERT_FALSE(retainedBindingSet.expired());
    }

    EXPECT_FALSE(retainedBindingSet.expired())
        << "Unsafe GPU quarantine must preserve lifecycle packages that own recorded draw RHI resources.";
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
    EXPECT_EQ(frameFence->resetCalls, 0u);
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

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);
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
    const NLS::Render::RHI::NativeHandle uiSignalSemaphore{
        NLS::Render::RHI::BackendType::DX12,
        reinterpret_cast<void*>(0x1234)
    };
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
    EXPECT_EQ(testQueue->lastPresentDesc.uiSignalSemaphore.backend, NLS::Render::RHI::BackendType::DX12);
    EXPECT_EQ(testQueue->lastPresentDesc.uiSignalSemaphore.handle, uiSignalSemaphore.handle);
    EXPECT_EQ(testQueue->lastPresentDesc.uiSignalValue, uiSignalValue);
    EXPECT_EQ(swapchain->acquireCalls, 1u);
}

TEST(ThreadedRenderingLifecycleTests, UiCompositionSyncBoundaryDescribesSceneUiAndPresentOrdering)
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

    auto boundary = NLS::Render::Context::DriverUIAccess::BuildUICompositionSyncBoundary(driver);
    EXPECT_FALSE(boundary.sceneToUiWaitSemaphore.IsValid());
    EXPECT_FALSE(boundary.uiToPresentSignalSemaphore.IsValid());
    EXPECT_EQ(boundary.uiToPresentSignalValue, 0u);

    const NLS::Render::RHI::NativeHandle uiSignalSemaphore{
        NLS::Render::RHI::BackendType::DX12,
        reinterpret_cast<void*>(0x1234)
    };
    constexpr uint64_t uiSignalValue = 42u;
    NLS::Render::Context::DriverUIAccess::SetUICompositionSignal(driver, uiSignalSemaphore, uiSignalValue);

    boundary = NLS::Render::Context::DriverUIAccess::BuildUICompositionSyncBoundary(driver);
    EXPECT_FALSE(boundary.sceneToUiWaitSemaphore.IsValid());
    EXPECT_EQ(boundary.uiToPresentSignalSemaphore.backend, NLS::Render::RHI::BackendType::DX12);
    EXPECT_EQ(boundary.uiToPresentSignalSemaphore.handle, uiSignalSemaphore.handle);
    EXPECT_EQ(boundary.uiToPresentSignalValue, uiSignalValue);

    NLS::Render::Context::DriverUIAccess::PresentSwapchain(driver);

    auto testQueue = explicitDevice->GetTestQueue();
    ASSERT_EQ(testQueue->presentCalls, 1u);
    EXPECT_EQ(testQueue->lastPresentDesc.uiSignalSemaphore.backend, NLS::Render::RHI::BackendType::DX12);
    EXPECT_EQ(testQueue->lastPresentDesc.uiSignalSemaphore.handle, uiSignalSemaphore.handle);
    EXPECT_EQ(testQueue->lastPresentDesc.uiSignalValue, uiSignalValue);

    boundary = NLS::Render::Context::DriverUIAccess::BuildUICompositionSyncBoundary(driver);
    EXPECT_FALSE(boundary.uiToPresentSignalSemaphore.IsValid());
    EXPECT_EQ(boundary.uiToPresentSignalValue, 0u);
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

TEST(ThreadedRenderingLifecycleTests, StandaloneExplicitFrameIndexStaysMonotonicAcrossRingSlotReuse)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = false;
    settings.framesInFlight = 2u;

    NLS::Render::Context::Driver driver(settings);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto setupFrameContext = [&](const size_t index)
    {
        auto commandBuffer = std::make_shared<TestCommandBuffer>();
        auto commandPool = std::make_shared<TestCommandPool>();
        auto frameFence = std::make_shared<TestFence>();
        auto descriptorAllocator = std::make_shared<TestDescriptorAllocator>();
        auto uploadContext = std::make_shared<TestUploadContext>();
        commandPool->commandBuffer = commandBuffer;

        auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, index);
        frameContext.commandBuffer = std::move(commandBuffer);
        frameContext.commandPool = std::move(commandPool);
        frameContext.frameFence = std::move(frameFence);
        frameContext.descriptorAllocator = descriptorAllocator;
        frameContext.uploadContext = uploadContext;
        return std::pair{descriptorAllocator, uploadContext};
    };

    const auto slot0Resources = setupFrameContext(0u);
    const auto slot1Resources = setupFrameContext(1u);

    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::BeginStandaloneExplicitFrame(driver, false));
    EXPECT_EQ(NLS::Render::Context::DriverTestAccess::PeekFrameContext(driver, 0u)->frameIndex, 0u);
    EXPECT_EQ(slot0Resources.first->lastBeginFrameIndex, 0u);
    EXPECT_EQ(slot0Resources.second->lastBeginFrameIndex, 0u);
    NLS::Render::Context::DriverTestAccess::EndStandaloneExplicitFrame(driver, false);

    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::BeginStandaloneExplicitFrame(driver, false));
    EXPECT_EQ(NLS::Render::Context::DriverTestAccess::PeekFrameContext(driver, 1u)->frameIndex, 1u);
    EXPECT_EQ(slot1Resources.first->lastBeginFrameIndex, 1u);
    EXPECT_EQ(slot1Resources.second->lastBeginFrameIndex, 1u);
    NLS::Render::Context::DriverTestAccess::EndStandaloneExplicitFrame(driver, false);

    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::BeginStandaloneExplicitFrame(driver, false));
    EXPECT_EQ(NLS::Render::Context::DriverTestAccess::PeekFrameContext(driver, 0u)->frameIndex, 2u);
    EXPECT_EQ(slot0Resources.first->lastBeginFrameIndex, 2u);
    EXPECT_EQ(slot0Resources.second->lastBeginFrameIndex, 2u);
    NLS::Render::Context::DriverTestAccess::EndStandaloneExplicitFrame(driver, false);

    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 3u);
}

TEST(ThreadedRenderingLifecycleTests, StandaloneExplicitFramePresentUsesUiCompositionBoundaryAndClearsSignal)
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

    NLS::Render::Context::DriverTestAccess::BeginStandaloneExplicitFrame(driver, true);

    const NLS::Render::RHI::NativeHandle uiSignalSemaphore{
        NLS::Render::RHI::BackendType::DX12,
        reinterpret_cast<void*>(0x5678)
    };
    constexpr uint64_t uiSignalValue = 77u;
    NLS::Render::Context::DriverUIAccess::SetUICompositionSignal(driver, uiSignalSemaphore, uiSignalValue);

    NLS::Render::Context::DriverTestAccess::EndStandaloneExplicitFrame(driver, true);

    auto testQueue = explicitDevice->GetTestQueue();
    ASSERT_EQ(testQueue->presentCalls, 1u);
    EXPECT_EQ(testQueue->lastPresentDesc.uiSignalSemaphore.backend, NLS::Render::RHI::BackendType::DX12);
    EXPECT_EQ(testQueue->lastPresentDesc.uiSignalSemaphore.handle, uiSignalSemaphore.handle);
    EXPECT_EQ(testQueue->lastPresentDesc.uiSignalValue, uiSignalValue);

    const auto boundary = NLS::Render::Context::DriverUIAccess::BuildUICompositionSyncBoundary(driver);
    EXPECT_FALSE(boundary.uiToPresentSignalSemaphore.IsValid());
    EXPECT_EQ(boundary.uiToPresentSignalValue, 0u);
}

TEST(ThreadedRenderingLifecycleTests, StandaloneExplicitFramePresentIgnoresZeroUiCompositionSignalValue)
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

    NLS::Render::Context::DriverTestAccess::BeginStandaloneExplicitFrame(driver, true);

    const NLS::Render::RHI::NativeHandle uiSignalSemaphore{
        NLS::Render::RHI::BackendType::DX12,
        reinterpret_cast<void*>(0x5678)
    };
    NLS::Render::Context::DriverUIAccess::SetUICompositionSignal(driver, uiSignalSemaphore, 0u);
    auto boundary = NLS::Render::Context::DriverUIAccess::BuildUICompositionSyncBoundary(driver);
    EXPECT_FALSE(boundary.uiToPresentSignalSemaphore.IsValid());
    EXPECT_EQ(boundary.uiToPresentSignalValue, 0u);

    NLS::Render::Context::DriverTestAccess::EndStandaloneExplicitFrame(driver, true);

    auto testQueue = explicitDevice->GetTestQueue();
    ASSERT_EQ(testQueue->presentCalls, 1u);
    EXPECT_FALSE(testQueue->lastPresentDesc.uiSignalSemaphore.IsValid());
    EXPECT_EQ(testQueue->lastPresentDesc.uiSignalValue, 0u);

    boundary = NLS::Render::Context::DriverUIAccess::BuildUICompositionSyncBoundary(driver);
    EXPECT_FALSE(boundary.uiToPresentSignalSemaphore.IsValid());
    EXPECT_EQ(boundary.uiToPresentSignalValue, 0u);
}

TEST(ThreadedRenderingLifecycleTests, StandaloneExplicitPresentFailureAfterPresentRetainsResourcesWithoutFenceSignal)
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

    auto testQueue = explicitDevice->GetTestQueue();
    testQueue->presentFailureStage = TestQueue::PresentFailureStage::AfterPresent;
    testQueue->nextPresentResult = {
        NLS::Render::RHI::RHIQueueOperationStatusCode::BackendFailure,
        "simulated present-time fence failure"
    };

    auto* impl = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl, nullptr);

    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::BeginStandaloneExplicitFrame(driver, true));
    NLS::Render::Context::DriverTestAccess::EndStandaloneExplicitFrame(driver, true);

    EXPECT_EQ(testQueue->submitCalls, 1u);
    EXPECT_EQ(testQueue->presentCalls, 1u);
    EXPECT_EQ(swapchain->acquireCalls, 1u);
    EXPECT_FALSE(frameFence->submitSignalRequested);
    EXPECT_EQ(descriptorAllocator->endFrameCalls, 0u);
    EXPECT_EQ(uploadContext->endFrameCalls, 0u);
    EXPECT_EQ(impl->deferredThreadedFrameScopedRetirementFrameContexts.count(0u), 0u);
    EXPECT_TRUE(impl->unsafeGpuWorkQuarantined.load(std::memory_order_acquire));
    ASSERT_EQ(impl->retainedThreadedSubmitResourceKeepAliveByFrameContext.size(), 1u);
    const auto& keepAlive = impl->retainedThreadedSubmitResourceKeepAliveByFrameContext[0u];
    EXPECT_TRUE(ContainsRetainedResource(keepAlive, commandPool));
    EXPECT_TRUE(ContainsRetainedResource(keepAlive, commandBuffer));
    EXPECT_TRUE(ContainsRetainedResource(keepAlive, imageAcquiredSemaphore));
    EXPECT_TRUE(ContainsRetainedResource(keepAlive, renderFinishedSemaphore));
}

TEST(ThreadedRenderingLifecycleTests, StandaloneExplicitFrameWithoutPresentClearsUiCompositionSignal)
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

    NLS::Render::Context::DriverTestAccess::BeginStandaloneExplicitFrame(driver, true);

    const NLS::Render::RHI::NativeHandle uiSignalSemaphore{
        NLS::Render::RHI::BackendType::DX12,
        reinterpret_cast<void*>(0x2468)
    };
    NLS::Render::Context::DriverUIAccess::SetUICompositionSignal(driver, uiSignalSemaphore, 91u);

    NLS::Render::Context::DriverTestAccess::EndStandaloneExplicitFrame(driver, false);

    EXPECT_EQ(explicitDevice->GetTestQueue()->presentCalls, 0u);
    const auto boundary = NLS::Render::Context::DriverUIAccess::BuildUICompositionSyncBoundary(driver);
    EXPECT_FALSE(boundary.uiToPresentSignalSemaphore.IsValid());
    EXPECT_EQ(boundary.uiToPresentSignalValue, 0u);
}

TEST(ThreadedRenderingLifecycleTests, ThreadedUiPresentDefersStandaloneFrameAllocatorsAndUploadsUntilReusableFence)
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

    auto* impl = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl, nullptr);

    ASSERT_TRUE(NLS::Render::Context::DriverUIAccess::PrepareUIRender(driver));
    NLS::Render::Context::DriverUIAccess::PresentSwapchain(driver);

    EXPECT_EQ(descriptorAllocator->beginFrameCalls, 1u);
    EXPECT_EQ(descriptorAllocator->endFrameCalls, 0u);
    EXPECT_EQ(descriptorAllocator->lastBeginFrameIndex, 0u);
    EXPECT_EQ(uploadContext->beginFrameCalls, 1u);
    EXPECT_EQ(uploadContext->endFrameCalls, 0u);
    EXPECT_EQ(uploadContext->lastBeginFrameIndex, 0u);
    EXPECT_EQ(impl->deferredThreadedFrameScopedRetirementFrameContexts.count(0u), 1u);

    ASSERT_TRUE(NLS::Render::Context::DriverUIAccess::PrepareUIRender(driver));

    EXPECT_EQ(frameFence->waitCalls, 2u);
    EXPECT_EQ(descriptorAllocator->beginFrameCalls, 2u);
    EXPECT_EQ(descriptorAllocator->endFrameCalls, 1u);
    EXPECT_EQ(descriptorAllocator->lastEndFrameIndex, 0u);
    EXPECT_EQ(uploadContext->beginFrameCalls, 2u);
    EXPECT_EQ(uploadContext->endFrameCalls, 1u);
    EXPECT_EQ(uploadContext->lastEndFrameIndex, 0u);
    EXPECT_EQ(impl->deferredThreadedFrameScopedRetirementFrameContexts.count(0u), 0u);
}

TEST(ThreadedRenderingLifecycleTests, ThreadedUiStandaloneSubmitFailureWithoutFrameFenceSignalDoesNotRegisterReusableRetirement)
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
    explicitDevice->GetTestQueue()->nextSubmitResult = {
        NLS::Render::RHI::RHIQueueOperationStatusCode::BackendFailure,
        "simulated standalone ui signal failure",
        true
    };

    auto* impl = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl, nullptr);

    ASSERT_TRUE(NLS::Render::Context::DriverUIAccess::PrepareUIRender(driver));
    NLS::Render::Context::DriverUIAccess::PresentSwapchain(driver);

    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    EXPECT_EQ(descriptorAllocator->endFrameCalls, 0u);
    EXPECT_EQ(uploadContext->endFrameCalls, 0u);
    EXPECT_EQ(impl->deferredThreadedFrameScopedRetirementFrameContexts.count(0u), 0u);
    ASSERT_EQ(impl->retainedThreadedSubmitResourceKeepAliveByFrameContext.size(), 1u);
    const auto& keepAlive = impl->retainedThreadedSubmitResourceKeepAliveByFrameContext[0u];
    EXPECT_TRUE(ContainsRetainedResource(keepAlive, commandPool));
    EXPECT_TRUE(ContainsRetainedResource(keepAlive, commandBuffer));
}

TEST(ThreadedRenderingLifecycleTests, ThreadedUiStandaloneSubmitSuccessDefersFrameScopedResourceRetirementUntilReusableFence)
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

    auto* impl = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl, nullptr);

    ASSERT_TRUE(NLS::Render::Context::DriverUIAccess::PrepareUIRender(driver));
    NLS::Render::Context::DriverUIAccess::PresentSwapchain(driver);

    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    EXPECT_EQ(descriptorAllocator->endFrameCalls, 0u);
    EXPECT_EQ(uploadContext->endFrameCalls, 0u);
    EXPECT_EQ(impl->deferredThreadedFrameScopedRetirementFrameContexts.count(0u), 1u);
    ASSERT_EQ(impl->retainedThreadedSubmitResourceKeepAliveByFrameContext.size(), 1u);
    const auto& keepAlive = impl->retainedThreadedSubmitResourceKeepAliveByFrameContext[0u];
    EXPECT_TRUE(ContainsRetainedResource(keepAlive, commandPool));
    EXPECT_TRUE(ContainsRetainedResource(keepAlive, commandBuffer));
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
    const ScopedDriverService driverService(driver);
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

TEST(ThreadedRenderingLifecycleTests, DriverReadPixelsCheckedPropagatesExplicitReadbackFailure)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    explicitDevice->readPixelsResult = {
        NLS::Render::RHI::RHIReadbackStatusCode::UnsupportedFormat,
        "test unsupported format"
    };
    explicitDevice->beginReadPixelsResult = explicitDevice->readPixelsResult;
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    NLS::Render::RHI::RHITextureDesc pickingDesc;
    pickingDesc.debugName = "SceneViewPickingReadback";
    pickingDesc.extent = { 64u, 64u, 1u };
    auto pickingTexture = std::make_shared<TestTexture>(pickingDesc);

    uint8_t pixel[3] {};
    const auto result = NLS::Render::Context::DriverRendererAccess::ReadPixelsChecked(
        driver,
        pickingTexture,
        0u,
        0u,
        1u,
        1u,
        NLS::Render::Settings::EPixelDataFormat::RGB,
        NLS::Render::Settings::EPixelDataType::UNSIGNED_BYTE,
        pixel);

    EXPECT_EQ(result.code, NLS::Render::RHI::RHIReadbackStatusCode::UnsupportedFormat);
    EXPECT_EQ(result.message, "test unsupported format");
    EXPECT_EQ(explicitDevice->lastReadPixelsTexture, pickingTexture);
}

TEST(ThreadedRenderingLifecycleTests, DriverReadPixelsCheckedMarksDeviceLostWhenCompletionDetectsDeviceRemoved)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    explicitDevice->beginReadPixelsResult = {
        NLS::Render::RHI::RHIReadbackStatusCode::Success,
        {},
        std::make_shared<TestCompletionToken>(NLS::Render::RHI::RHICompletionStatus{
            NLS::Render::RHI::RHICompletionStatusCode::DeviceLost,
            "ReadPixels completion detected device removed"
        })
    };
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);
    auto* impl = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl, nullptr);

    NLS::Render::RHI::RHITextureDesc pickingDesc;
    pickingDesc.debugName = "SceneViewPickingReadback";
    pickingDesc.extent = { 64u, 64u, 1u };
    auto pickingTexture = std::make_shared<TestTexture>(pickingDesc);

    uint8_t pixel[3] {};
    const auto result = NLS::Render::Context::DriverRendererAccess::ReadPixelsChecked(
        driver,
        pickingTexture,
        0u,
        0u,
        1u,
        1u,
        NLS::Render::Settings::EPixelDataFormat::RGB,
        NLS::Render::Settings::EPixelDataType::UNSIGNED_BYTE,
        pixel);

    EXPECT_EQ(result.code, NLS::Render::RHI::RHIReadbackStatusCode::DeviceLost);
    EXPECT_EQ(result.message, "ReadPixels completion detected device removed");
    EXPECT_EQ(explicitDevice->lastReadPixelsTexture, pickingTexture);
    EXPECT_TRUE(impl->deviceLostDetected.load(std::memory_order_acquire));
    EXPECT_NE(impl->deviceLostReason.find("device removed"), std::string::npos);
}

TEST(ThreadedRenderingLifecycleTests, DriverBeginReadPixelsReturnsCompletionTokenWithoutWaiting)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    auto completion = std::make_shared<TestCompletionToken>(NLS::Render::RHI::RHICompletionStatus{
        NLS::Render::RHI::RHICompletionStatusCode::Pending,
        {}
    });
    explicitDevice->beginReadPixelsResult = {
        NLS::Render::RHI::RHIReadbackStatusCode::Success,
        {},
        completion
    };
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    NLS::Render::RHI::RHITextureDesc pickingDesc;
    pickingDesc.debugName = "SceneViewPickingReadback";
    pickingDesc.extent = { 64u, 64u, 1u };
    auto pickingTexture = std::make_shared<TestTexture>(pickingDesc);

    uint8_t pixel[3] {};
    const auto result = NLS::Render::Context::DriverRendererAccess::BeginReadPixels(
        driver,
        pickingTexture,
        0u,
        0u,
        1u,
        1u,
        NLS::Render::Settings::EPixelDataFormat::RGB,
        NLS::Render::Settings::EPixelDataType::UNSIGNED_BYTE,
        pixel);

    EXPECT_TRUE(result.Succeeded());
    EXPECT_EQ(result.completion, completion);
    EXPECT_EQ(result.completion->GetStatus().code, NLS::Render::RHI::RHICompletionStatusCode::Pending);
    EXPECT_EQ(explicitDevice->lastReadPixelsTexture, pickingTexture);
}

TEST(ThreadedRenderingLifecycleTests, DriverBeginReadPixelsRejectsDeviceLostBeforeTouchingExplicitDevice)
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
    auto* impl = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl, nullptr);
    impl->deviceLostDetected.store(true, std::memory_order_release);

    NLS::Render::RHI::RHITextureDesc pickingDesc;
    pickingDesc.debugName = "SceneViewPickingReadback";
    pickingDesc.extent = { 64u, 64u, 1u };
    auto pickingTexture = std::make_shared<TestTexture>(pickingDesc);

    uint8_t pixel[3] {};
    const auto result = NLS::Render::Context::DriverRendererAccess::BeginReadPixels(
        driver,
        pickingTexture,
        0u,
        0u,
        1u,
        1u,
        NLS::Render::Settings::EPixelDataFormat::RGB,
        NLS::Render::Settings::EPixelDataType::UNSIGNED_BYTE,
        pixel);

    EXPECT_EQ(result.code, NLS::Render::RHI::RHIReadbackStatusCode::BackendFailure);
    EXPECT_NE(result.message.find("device is lost"), std::string::npos);
    EXPECT_EQ(explicitDevice->lastReadPixelsTexture, nullptr);
}

TEST(ThreadedRenderingLifecycleTests, DriverBeginReadPixelsMarksDeviceLostWhenReadbackDetectsDeviceRemoved)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    explicitDevice->beginReadPixelsResult = {
        NLS::Render::RHI::RHIReadbackStatusCode::DeviceLost,
        "ReadPixels after ExecuteCommandLists: DX12 device removed/lost before readback could safely complete; hr=-2005270522"
    };
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);
    auto* impl = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl, nullptr);

    NLS::Render::RHI::RHITextureDesc pickingDesc;
    pickingDesc.debugName = "SceneViewPickingReadback";
    pickingDesc.extent = { 64u, 64u, 1u };
    auto pickingTexture = std::make_shared<TestTexture>(pickingDesc);

    uint8_t pixel[3] {};
    const auto result = NLS::Render::Context::DriverRendererAccess::BeginReadPixels(
        driver,
        pickingTexture,
        0u,
        0u,
        1u,
        1u,
        NLS::Render::Settings::EPixelDataFormat::RGB,
        NLS::Render::Settings::EPixelDataType::UNSIGNED_BYTE,
        pixel);

    EXPECT_EQ(result.code, NLS::Render::RHI::RHIReadbackStatusCode::DeviceLost);
    EXPECT_EQ(explicitDevice->lastReadPixelsTexture, pickingTexture);
    EXPECT_TRUE(impl->deviceLostDetected.load(std::memory_order_acquire));
    EXPECT_NE(impl->deviceLostReason.find("device removed"), std::string::npos);
}

TEST(ThreadedRenderingLifecycleTests, DriverBeginReadPixelsRejectsUnsafeGpuQuarantineBeforeTouchingExplicitDevice)
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
    auto* impl = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl, nullptr);
    impl->unsafeGpuWorkQuarantined.store(true, std::memory_order_release);

    NLS::Render::RHI::RHITextureDesc pickingDesc;
    pickingDesc.debugName = "SceneViewPickingReadback";
    pickingDesc.extent = { 64u, 64u, 1u };
    auto pickingTexture = std::make_shared<TestTexture>(pickingDesc);

    uint8_t pixel[3] {};
    const auto result = NLS::Render::Context::DriverRendererAccess::BeginReadPixels(
        driver,
        pickingTexture,
        0u,
        0u,
        1u,
        1u,
        NLS::Render::Settings::EPixelDataFormat::RGB,
        NLS::Render::Settings::EPixelDataType::UNSIGNED_BYTE,
        pixel);

    EXPECT_EQ(result.code, NLS::Render::RHI::RHIReadbackStatusCode::BackendFailure);
    EXPECT_NE(result.message.find("quarantined"), std::string::npos);
    EXPECT_EQ(explicitDevice->lastReadPixelsTexture, nullptr);
}
