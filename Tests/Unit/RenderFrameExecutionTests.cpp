#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cstring>
#include <fstream>
#include <future>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "Core/ServiceLocator.h"
#include "Core/ResourceManagement/ShaderManager.h"
#include "Assets/EditorThumbnailPreviewRenderer.h"
#include "Assets/PreviewRenderableSnapshot.h"
#include "Guid.h"
#include "Jobs/JobSystem.h"
#include "Profiling/PerformanceStageStats.h"
#include "Rendering/Assets/ShaderArtifact.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Context/DriverInternal.h"
#include "Rendering/Context/RenderThreadCoordinator.h"
#include "Rendering/Context/RhiCommandListExecutor.h"
#include "Rendering/Context/SwapchainResizePolicy.h"
#include "Rendering/Context/FrameSubmissionTelemetry.h"
#include "Tests/RenderingThreadTestHelpers.h"
#include "Rendering/Core/CompositeRenderer.h"
#include "Rendering/Data/FrameDescriptor.h"
#include "Rendering/Data/DrawableObjectDescriptor.h"
#include "Rendering/Entities/Camera.h"
#include "Rendering/DeferredSceneRenderer.h"
#include "Rendering/ForwardSceneRenderer.h"
#include "Rendering/Buffers/Framebuffer.h"
#include "Rendering/FrameGraph/ExternalResourceBridge.h"
#include "Rendering/Context/RenderCommandPipe.h"
#include "Rendering/Context/RenderCommandFence.h"
#include "Rendering/Geometry/Vertex.h"
#include "Rendering/FrameGraph/SceneRenderGraphBuilderDeferred.h"
#include "Rendering/RHI/BindingPointMap.h"
#include "Rendering/RHI/Core/RHICommand.h"
#include "Rendering/RHI/Core/RHIBinding.h"
#include "Rendering/RHI/Core/RHIPipeline.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/RHI/Core/RHIMesh.h"
#include "Rendering/Resources/Loaders/ShaderLoader.h"
#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/RHI/Core/RHISwapchain.h"
#include "Rendering/RHI/Utils/DescriptorAllocator/DescriptorAllocator.h"
#include "Rendering/RHI/Utils/PipelineCache/PipelineCache.h"
#include "Rendering/RHI/Utils/ResourceStateTracker/ResourceStateTracker.h"
#include "Rendering/RHI/Utils/UploadContext/UploadContext.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/Mesh.h"
#include "Rendering/Settings/DriverSettings.h"
#include "Rendering/BaseSceneRenderer.h"
#include "Components/LightComponent.h"
#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "SceneSystem/Scene.h"
#include "ImGui/imgui.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#undef CreateSemaphore
#endif

namespace
{
    struct ImGuiContextGuard
    {
        ImGuiContextGuard()
        {
            IMGUI_CHECKVERSION();
            context = ImGui::CreateContext();
            ImGui::GetIO().DisplaySize = ImVec2(320.0f, 200.0f);
            ImGui::GetIO().Fonts->AddFontDefault();
        }

        ~ImGuiContextGuard()
        {
            ImGui::DestroyContext(context);
        }

        ImGuiContext* context = nullptr;
    };

    std::shared_ptr<NLS::Render::UI::UiDrawDataSnapshot> MakeVisibleUiSnapshot(
        const uint64_t frameId,
        const float width,
        const float height,
        const NLS::Render::UI::UiTextureId textureId = {})
    {
        auto snapshot = std::make_shared<NLS::Render::UI::UiDrawDataSnapshot>();
        snapshot->frameId = frameId;
        snapshot->hasVisibleDraws = true;
        snapshot->displaySize[0] = width;
        snapshot->displaySize[1] = height;
        snapshot->framebufferScale[0] = 1.0f;
        snapshot->framebufferScale[1] = 1.0f;
        snapshot->totalVertexCount = 3u;
        snapshot->totalIndexCount = 3u;

        NLS::Render::UI::UiDrawListSnapshot drawList;
        drawList.vertices.resize(3u);
        drawList.indices = { 0u, 1u, 2u };
        drawList.commands.push_back({
            3u,
            0u,
            0u,
            { 0.0f, 0.0f, width, height },
            textureId,
            NLS::Render::UI::UiDrawCallbackKind::None,
            false
        });
        snapshot->drawLists.push_back(std::move(drawList));
        return snapshot;
    }

    const NLS::Base::Profiling::PerformanceStageEntry* FindStage(
        const NLS::Base::Profiling::PerformanceStageStatsSnapshot& snapshot,
        const NLS::Base::Profiling::PerformanceStageDomain domain,
        const char* stageName)
    {
        const auto found = std::find_if(
            snapshot.stages.begin(),
            snapshot.stages.end(),
            [domain, stageName](const NLS::Base::Profiling::PerformanceStageEntry& entry)
            {
                return entry.domain == domain && entry.stageName == stageName;
            });
        return found == snapshot.stages.end() ? nullptr : &(*found);
    }

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

        std::optional<NLS::Render::Context::RenderFramePacket> CaptureFrameSnapshot(
            const NLS::Render::Data::FrameDescriptor& frameDescriptor) const
        {
            return BuildRenderFramePacket(frameDescriptor);
        }
    };

    class VisibleSnapshotPublishingRenderer final : public SnapshotPublishingRenderer
    {
    public:
        using SnapshotPublishingRenderer::SnapshotPublishingRenderer;

    protected:
        NLS::Render::Context::RenderSceneBuildCommand BuildRenderSceneCommand(
            const NLS::Render::Context::RenderFramePacket& packet) override
        {
            return [packet](const size_t)
            {
                NLS::Render::Context::RenderSceneBuildData scene;
                scene.visibleOpaqueDrawCount = 1u;
                return NLS::Render::Context::BuildRenderScenePackage(packet, std::move(scene));
            };
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
        NLS::Render::Context::RenderSceneBuildCommand BuildRenderSceneCommand(
            const NLS::Render::Context::RenderFramePacket& snapshot) override
        {
            const uint64_t sceneGameObjectCount = HasDescriptor<DescriptorRequiredForPreparedBuilder>()
                ? GetDescriptor<DescriptorRequiredForPreparedBuilder>().sceneGameObjectCount
                : 0u;

            return [snapshot, sceneGameObjectCount](const size_t)
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
        NLS::Render::Context::RenderSceneBuildCommand BuildRenderSceneCommand(
            const NLS::Render::Context::RenderFramePacket& packet) override
        {
            return [packet, commands = std::move(m_threadedRecordedDrawCommands)](const size_t) mutable
            {
                NLS::Render::Context::RenderSceneBuildData scene;
                scene.visibleOpaqueDrawCount = 1u;
                scene.recordedDrawCommands = std::move(commands);
                return NLS::Render::Context::BuildRenderScenePackage(packet, std::move(scene));
            };
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

        bool PrepareRecordedDraw(
            PipelineState pipelineState,
            const NLS::Render::Entities::Drawable& drawable,
            std::string_view lightMode,
            PreparedRecordedDraw& outDraw) const override
        {
            (void) lightMode;
            return PrepareRecordedDraw(pipelineState, drawable, outDraw);
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

        std::optional<NLS::Render::Context::RenderFramePacket> CaptureFrameSnapshot(
            const NLS::Render::Data::FrameDescriptor& frameDescriptor) const
        {
            return BuildRenderFramePacket(frameDescriptor);
        }

        NLS::Render::Context::RenderScenePackage CaptureRenderScenePackage(
            const NLS::Render::Context::RenderFramePacket& packet,
            NLS::Render::Context::RenderSceneBuildData scene = {}) const
        {
            return NLS::Render::Context::BuildRenderScenePackage(packet, std::move(scene));
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
        explicit TestCompletionToken(
            NLS::Render::RHI::RHICompletionStatus status,
            std::chrono::microseconds waitDelay = std::chrono::microseconds{0})
            : m_status(std::move(status))
            , m_waitDelay(waitDelay)
        {
        }

        std::string_view GetDebugName() const override { return "TestCompletionToken"; }
        NLS::Render::RHI::RHICompletionStatus Poll() override { return m_status; }
        bool IsComplete() override { return m_status.IsComplete(); }
        NLS::Render::RHI::RHICompletionStatus GetStatus() override { return m_status; }
        NLS::Render::RHI::RHICompletionStatus Wait(uint64_t = 0) override
        {
            ++waitCalls;
            if (m_waitDelay.count() > 0)
                std::this_thread::sleep_for(m_waitDelay);
            return m_status;
        }

        size_t waitCalls = 0u;

    private:
        NLS::Render::RHI::RHICompletionStatus m_status;
        std::chrono::microseconds m_waitDelay{0};
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
        bool completeOnSubmit = false;
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
            if (onBegin)
                onBegin();
            if (!beginFailure.empty())
                throw std::runtime_error(beginFailure);
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
        void PushConstants(
            NLS::Render::RHI::ShaderStageMask stageMask,
            uint32_t offset,
            uint32_t size,
            const void* data) override
        {
            ++pushConstantsCalls;
            lastPushConstantsStageMask = stageMask;
            lastPushConstantsOffset = offset;
            lastPushConstantsSize = size;
            lastPushConstantsBytes.resize(size);
            if (data != nullptr && size != 0u)
                std::memcpy(lastPushConstantsBytes.data(), data, size);
            events.emplace_back("PushConstants");
        }
        void BindVertexBuffer(uint32_t, const NLS::Render::RHI::RHIVertexBufferView&) override
        {
            ++bindVertexBufferCalls;
        }
        void BindIndexBuffer(const NLS::Render::RHI::RHIIndexBufferView&) override
        {
            ++bindIndexBufferCalls;
        }
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
        size_t bindIndexBufferCalls = 0u;
        size_t drawCalls = 0u;
        size_t drawIndexedCalls = 0u;
        size_t dispatchCalls = 0u;
        size_t barrierCalls = 0u;
        size_t barrierCheckedCalls = 0u;
        size_t executeChildCommandBufferCalls = 0u;
        bool recording = false;
        bool closedForSubmission = true;
        bool stayNonRecordingOnBegin = false;
        std::string beginFailure;
        std::function<void()> onBegin;
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
        uint32_t pushConstantsCalls = 0u;
        NLS::Render::RHI::ShaderStageMask lastPushConstantsStageMask = NLS::Render::RHI::ShaderStageMask::None;
        uint32_t lastPushConstantsOffset = 0u;
        uint32_t lastPushConstantsSize = 0u;
        std::vector<uint8_t> lastPushConstantsBytes;
        std::vector<std::string> events;
    };

    class ScopedOverlayShaderManager final
    {
    public:
        explicit ScopedOverlayShaderManager(const bool withLightGrid = false)
        {
            m_tempRoot = std::filesystem::temp_directory_path() /
                ("nullus_threaded_ui_overlay_shader_" + NLS::Guid::New().ToString());
            std::filesystem::create_directories(m_tempRoot);

            NLS::Render::Assets::ShaderArtifact artifact;
            artifact.sourcePath = "App/Assets/Engine/Shaders/RHIImGuiOverlay.hlsl";
            artifact.subAssetKey = "shader:rhi-imgui-overlay";
            artifact.stages.push_back({
                NLS::Render::ShaderCompiler::ShaderStage::Vertex,
                NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL,
                "VSMain",
                "vs_6_0",
                {
                    NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded,
                    { 0x44u, 0x58u, 0x49u, 0x4cu, 0x56u, 0x53u },
                    {},
                    {},
                    "ui-overlay-vs-cache",
                    (m_tempRoot / "RHIImGuiOverlay.vs.dxil").string()
                }
            });
            artifact.stages.push_back({
                NLS::Render::ShaderCompiler::ShaderStage::Pixel,
                NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL,
                "PSMain",
                "ps_6_0",
                {
                    NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded,
                    { 0x44u, 0x58u, 0x49u, 0x4cu, 0x50u, 0x53u },
                    {},
                    {},
                    "ui-overlay-ps-cache",
                    (m_tempRoot / "RHIImGuiOverlay.ps.dxil").string()
                }
            });

            const auto shaderArtifactPath = m_tempRoot / "e1ac067b4307ee69c2cae08bf3e7715d98799884c16e12156b66224707eecae9";
            const auto bytes = NLS::Render::Assets::SerializeShaderArtifact(artifact);
            std::ofstream output(shaderArtifactPath, std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            output.close();

            auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(shaderArtifactPath.string());
            m_shaderManager.RegisterResource(":Shaders/RHIImGuiOverlay.hlsl", shader);
            m_shaderManager.RegisterResource(":Shaders/SwapchainOutput.hlsl",
                NLS::Render::Resources::Loaders::ShaderLoader::Create(shaderArtifactPath.string()));
            if (withLightGrid)
            {
                artifact.stages.resize(1u);
                auto& compute = artifact.stages.front();
                compute.stage = NLS::Render::ShaderCompiler::ShaderStage::Compute;
                compute.entryPoint = "CSMain";
                compute.targetProfile = "cs_6_0";
                const auto computePath = m_tempRoot / "71ac067b4307ee69c2cae08bf3e7715d98799884c16e12156b66224707eecae9";
                const auto computeBytes = NLS::Render::Assets::SerializeShaderArtifact(artifact);
                std::ofstream computeOutput(computePath, std::ios::binary | std::ios::trunc);
                computeOutput.write(reinterpret_cast<const char*>(computeBytes.data()), static_cast<std::streamsize>(computeBytes.size()));
                computeOutput.close();
                for (const auto* name : { ":Shaders/LightGridReset.hlsl", ":Shaders/LightGridInjection.hlsl", ":Shaders/LightGridCompact.hlsl" })
                    m_shaderManager.RegisterResource(name,
                        NLS::Render::Resources::Loaders::ShaderLoader::Create(computePath.string()));
            }
            NLS::Core::ServiceLocator::Provide(m_shaderManager);
        }

        ~ScopedOverlayShaderManager()
        {
            NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::ShaderManager>();
            std::filesystem::remove_all(m_tempRoot);
        }

        ScopedOverlayShaderManager(const ScopedOverlayShaderManager&) = delete;
        ScopedOverlayShaderManager& operator=(const ScopedOverlayShaderManager&) = delete;

    private:
        std::filesystem::path m_tempRoot;
        NLS::Core::ResourceManagement::ShaderManager m_shaderManager;
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
        NLS::Render::RHI::RHIUpdateResult UpdateData(
            const NLS::Render::RHI::RHIBufferUploadDesc& uploadDesc) override
        {
            ++updateCalls;
            lastUpload = uploadDesc;
            return updateResult;
        }

        uint32_t updateCalls = 0u;
        NLS::Render::RHI::RHIBufferUploadDesc lastUpload {};
        NLS::Render::RHI::RHIUpdateResult updateResult { NLS::Render::RHI::RHIUpdateStatusCode::Success, {} };

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

        explicit TestBindingSet(NLS::Render::RHI::RHIBindingSetDesc desc) : m_desc(std::move(desc))
        {
            if (m_desc.debugName.empty())
                m_desc.debugName = "TestBindingSet";
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

        explicit TestGraphicsPipeline(NLS::Render::RHI::RHIGraphicsPipelineDesc desc)
            : m_desc(std::move(desc))
        {
        }

        std::string_view GetDebugName() const override { return m_desc.debugName; }
        const NLS::Render::RHI::RHIGraphicsPipelineDesc& GetDesc() const override { return m_desc; }

    private:
        NLS::Render::RHI::RHIGraphicsPipelineDesc m_desc {};
    };

    class TestPipelineLayout final : public NLS::Render::RHI::RHIPipelineLayout
    {
    public:
        explicit TestPipelineLayout(NLS::Render::RHI::RHIPipelineLayoutDesc desc)
            : m_desc(std::move(desc))
        {
        }

        std::string_view GetDebugName() const override { return m_desc.debugName; }
        const NLS::Render::RHI::RHIPipelineLayoutDesc& GetDesc() const override { return m_desc; }

    private:
        NLS::Render::RHI::RHIPipelineLayoutDesc m_desc {};
    };

    class TestShaderModule final : public NLS::Render::RHI::RHIShaderModule
    {
    public:
        explicit TestShaderModule(NLS::Render::RHI::RHIShaderModuleDesc desc)
            : m_desc(std::move(desc))
        {
        }

        std::string_view GetDebugName() const override { return m_desc.debugName; }
        const NLS::Render::RHI::RHIShaderModuleDesc& GetDesc() const override { return m_desc; }

    private:
        NLS::Render::RHI::RHIShaderModuleDesc m_desc {};
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

    template <typename SceneRenderer>
    class LateFrameCommandRenderer final : public SceneRenderer
    {
    public:
        using SceneRenderer::SceneRenderer;

        bool QueueHelper(const std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline>& pipeline,
            const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& binding,
            const std::shared_ptr<NLS::Render::RHI::RHIMesh>& mesh)
        {
            typename SceneRenderer::PreparedRecordedDraw draw;
            draw.pipeline = pipeline;
            draw.materialBindingSet = binding;
            draw.mesh = mesh;
            draw.instanceCount = 1u;
            return this->QueueThreadedRecordedDraw(std::move(draw));
        }
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
                    if (m_sharedAllocationFailureCount != nullptr)
                        m_sharedAllocationFailureCount->fetch_add(1u, std::memory_order_relaxed);
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

        void SetSharedAllocationFailureCounter(
            std::shared_ptr<std::atomic_uint64_t> sharedAllocationFailureCount) override
        {
            m_sharedAllocationFailureCount = std::move(sharedAllocationFailureCount);
        }

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
        std::shared_ptr<std::atomic_uint64_t> m_sharedAllocationFailureCount;
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
        void SetSharedAllocationFailureCounter(std::shared_ptr<std::atomic_uint64_t>) override {}

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
        void SetSharedAllocationFailureCounter(std::shared_ptr<std::atomic_uint64_t>) override {}

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
        explicit TestTexture(
            NLS::Render::RHI::RHITextureDesc desc,
            const NLS::Render::RHI::ResourceState state =
                NLS::Render::RHI::ResourceState::Unknown)
            : m_desc(std::move(desc))
            , m_state(state)
        {
        }

        std::string_view GetDebugName() const override { return m_desc.debugName; }
        const NLS::Render::RHI::RHITextureDesc& GetDesc() const override { return m_desc; }
        NLS::Render::RHI::ResourceState GetState() const override { return m_state; }

    private:
        NLS::Render::RHI::RHITextureDesc m_desc {};
        NLS::Render::RHI::ResourceState m_state = NLS::Render::RHI::ResourceState::Unknown;
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

    class TestSampler final : public NLS::Render::RHI::RHISampler
    {
    public:
        explicit TestSampler(NLS::Render::RHI::SamplerDesc desc)
            : m_desc(std::move(desc))
        {
        }

        std::string_view GetDebugName() const override { return "TestSampler"; }
        const NLS::Render::RHI::SamplerDesc& GetDesc() const override { return m_desc; }

    private:
        NLS::Render::RHI::SamplerDesc m_desc {};
    };

    class TestBindingLayout final : public NLS::Render::RHI::RHIBindingLayout
    {
    public:
        explicit TestBindingLayout(NLS::Render::RHI::RHIBindingLayoutDesc desc)
            : m_desc(std::move(desc))
        {
        }

        std::string_view GetDebugName() const override { return m_desc.debugName; }
        const NLS::Render::RHI::RHIBindingLayoutDesc& GetDesc() const override { return m_desc; }

    private:
        NLS::Render::RHI::RHIBindingLayoutDesc m_desc {};
    };

    NLS::Render::FrameGraph::DeferredPreparedSceneResources BuildDeferredTestPreparedSceneResources(
        const uint16_t width = 64u,
        const uint16_t height = 64u)
    {
        NLS::Render::FrameGraph::DeferredPreparedSceneResources resources;
        resources.gbufferColorViews.reserve(NLS::Render::FrameGraph::kDeferredGBufferColorAttachmentCount);
        resources.gbufferTextures.reserve(NLS::Render::FrameGraph::kDeferredGBufferTextureCount);

        for (const auto& slot : NLS::Render::FrameGraph::kDeferredGBufferColorSlots)
        {
            NLS::Render::RHI::RHITextureDesc textureDesc;
            textureDesc.extent = { width, height, 1u };
            textureDesc.format = slot.format;
            textureDesc.usage = slot.usage;
            textureDesc.debugName = slot.debugName;
            auto texture = std::make_shared<TestTexture>(textureDesc);

            NLS::Render::RHI::RHITextureViewDesc viewDesc;
            viewDesc.viewType = NLS::Render::RHI::TextureViewType::Texture2D;
            viewDesc.format = slot.format;
            viewDesc.debugName = slot.capturedViewName;
            resources.gbufferColorViews.push_back(std::make_shared<TestTextureView>(texture, viewDesc));
            resources.gbufferTextures.push_back(texture);
        }

        NLS::Render::RHI::RHITextureDesc depthDesc;
        depthDesc.extent = { width, height, 1u };
        depthDesc.format = NLS::Render::FrameGraph::kDeferredGBufferDepthSlot.format;
        depthDesc.usage = NLS::Render::FrameGraph::kDeferredGBufferDepthSlot.usage;
        depthDesc.debugName = NLS::Render::FrameGraph::kDeferredGBufferDepthSlot.debugName;
        auto depthTexture = std::make_shared<TestTexture>(depthDesc);

        NLS::Render::RHI::RHITextureViewDesc depthViewDesc;
        depthViewDesc.viewType = NLS::Render::RHI::TextureViewType::Texture2D;
        depthViewDesc.format = NLS::Render::FrameGraph::kDeferredGBufferDepthSlot.format;
        depthViewDesc.debugName = NLS::Render::FrameGraph::kDeferredGBufferDepthSlot.capturedViewName;
        resources.gbufferDepthView = std::make_shared<TestTextureView>(depthTexture, depthViewDesc);
        resources.gbufferTextures.push_back(depthTexture);

        return resources;
    }

    std::shared_ptr<TestTextureView> MakeTestSwapchainBackbufferView()
    {
        NLS::Render::RHI::RHITextureDesc textureDesc;
        textureDesc.extent = { 128u, 72u, 1u };
        textureDesc.format = NLS::Render::RHI::TextureFormat::RGBA8;
        textureDesc.usage =
            NLS::Render::RHI::TextureUsageFlags::ColorAttachment |
            NLS::Render::RHI::TextureUsageFlags::Present;
        textureDesc.debugName = "TestSwapchainBackbuffer";

        NLS::Render::RHI::RHITextureViewDesc viewDesc;
        viewDesc.viewType = NLS::Render::RHI::TextureViewType::Texture2D;
        viewDesc.format = textureDesc.format;
        viewDesc.subresourceRange.baseMipLevel = 0u;
        viewDesc.subresourceRange.mipLevelCount = 1u;
        viewDesc.subresourceRange.baseArrayLayer = 0u;
        viewDesc.subresourceRange.arrayLayerCount = 1u;
        viewDesc.debugName = "TestSwapchainBackbufferView";

        return std::make_shared<TestTextureView>(
            std::make_shared<TestTexture>(std::move(textureDesc)),
            std::move(viewDesc));
    }

    class TestSwapchain final : public NLS::Render::RHI::RHISwapchain
    {
    public:
        TestSwapchain()
            : backbufferView(MakeTestSwapchainBackbufferView())
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
            if (onAcquire)
                onAcquire();
            if (!acquireResult)
                return std::nullopt;
            return NLS::Render::RHI::RHIAcquiredImage{ 1u, backbufferView, false };
        }
        std::shared_ptr<NLS::Render::RHI::RHITextureView> GetBackbufferView(uint32_t index) override
        {
            lastBackbufferIndex = index;
            if (onGetBackbufferView)
                onGetBackbufferView();
            return backbufferView;
        }
        bool Resize(uint32_t width, uint32_t height) override
        {
            ++resizeCalls;
            resizeWidth = width;
            resizeHeight = height;
            return resizeResult;
        }

        NLS::Render::RHI::SwapchainDesc desc{};
        std::shared_ptr<NLS::Render::RHI::RHITextureView> backbufferView;
        size_t acquireCalls = 0u;
        bool acquireResult = true;
        std::function<void()> onAcquire;
        std::function<void()> onGetBackbufferView;
        uint32_t lastBackbufferIndex = 0u;
        size_t resizeCalls = 0u;
        uint32_t resizeWidth = 0u;
        uint32_t resizeHeight = 0u;
        bool resizeResult = true;
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
            if (onPresent)
                onPresent(presentDesc);
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
            if (onSubmit)
                onSubmit(submitDesc);
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
                    testFence->signaled = testFence->completeOnSubmit;
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
        std::function<void(const NLS::Render::RHI::RHISubmitDesc&)> onSubmit;
        std::function<void(const NLS::Render::RHI::RHIPresentDesc&)> onPresent;
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
        std::function<void()> createdCommandBufferBegin;

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
        std::vector<std::shared_ptr<TestBuffer>> createdBuffers;
        size_t createPipelineLayoutCalls = 0u;
        size_t createBindingLayoutCalls = 0u;
        size_t createShaderModuleCalls = 0u;
        size_t createGraphicsPipelineCalls = 0u;
        size_t createSamplerCalls = 0u;
        NLS::Render::RHI::RHIPipelineLayoutDesc lastPipelineLayoutDesc {};
        NLS::Render::RHI::RHIBindingLayoutDesc lastBindingLayoutDesc {};
        NLS::Render::RHI::SamplerDesc lastSamplerDesc {};
        std::vector<NLS::Render::RHI::RHIShaderModuleDesc> shaderModuleDescs;
        NLS::Render::RHI::RHIGraphicsPipelineDesc lastGraphicsPipelineDesc {};
        std::shared_ptr<TestPipelineLayout> createdPipelineLayout;
        std::shared_ptr<TestBindingLayout> createdBindingLayout;
        std::shared_ptr<TestSampler> createdSampler;
        std::vector<std::shared_ptr<TestShaderModule>> createdShaderModules;
        std::shared_ptr<TestGraphicsPipeline> createdGraphicsPipeline;
        std::shared_ptr<NLS::Render::RHI::RHISwapchain> CreateSwapchain(const NLS::Render::RHI::SwapchainDesc&) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHIBuffer> CreateBuffer(
            const NLS::Render::RHI::RHIBufferDesc& desc,
            const NLS::Render::RHI::RHIBufferUploadDesc&) override
        {
            auto buffer = std::make_shared<TestBuffer>(desc);
            createdBuffers.push_back(buffer);
            return buffer;
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
        std::shared_ptr<NLS::Render::RHI::RHISampler> CreateSampler(
            const NLS::Render::RHI::SamplerDesc& desc,
            std::string = {}) override
        {
            ++createSamplerCalls;
            lastSamplerDesc = desc;
            createdSampler = std::make_shared<TestSampler>(desc);
            return createdSampler;
        }
        std::shared_ptr<NLS::Render::RHI::RHIBindingLayout> CreateBindingLayout(
            const NLS::Render::RHI::RHIBindingLayoutDesc& desc) override
        {
            ++createBindingLayoutCalls;
            lastBindingLayoutDesc = desc;
            createdBindingLayout = std::make_shared<TestBindingLayout>(desc);
            return createdBindingLayout;
        }
        std::shared_ptr<NLS::Render::RHI::RHIBindingSet> CreateBindingSet(const NLS::Render::RHI::RHIBindingSetDesc& desc) override
        {
            return std::make_shared<TestBindingSet>(desc);
        }
        std::shared_ptr<NLS::Render::RHI::RHIPipelineLayout> CreatePipelineLayout(
            const NLS::Render::RHI::RHIPipelineLayoutDesc& desc) override
        {
            ++createPipelineLayoutCalls;
            lastPipelineLayoutDesc = desc;
            createdPipelineLayout = std::make_shared<TestPipelineLayout>(desc);
            return createdPipelineLayout;
        }
        std::shared_ptr<NLS::Render::RHI::RHIShaderModule> CreateShaderModule(
            const NLS::Render::RHI::RHIShaderModuleDesc& desc) override
        {
            ++createShaderModuleCalls;
            shaderModuleDescs.push_back(desc);
            auto shaderModule = std::make_shared<TestShaderModule>(desc);
            createdShaderModules.push_back(shaderModule);
            return shaderModule;
        }
        std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline> CreateGraphicsPipeline(
            const NLS::Render::RHI::RHIGraphicsPipelineDesc& desc) override
        {
            ++createGraphicsPipelineCalls;
            lastGraphicsPipelineDesc = desc;
            createdGraphicsPipeline = std::make_shared<TestGraphicsPipeline>(desc);
            return createdGraphicsPipeline;
        }
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
            testCommandBuffer->onBegin = createdCommandBufferBegin;
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
        std::shared_ptr<NLS::Render::RHI::RHIFence> CreateFence(std::string debugName = {}) override
        {
            auto fence = std::make_shared<TestFence>();
            fence->signaled = true;
            fence->completeOnSubmit = debugName.find("ResourceRetirement") == 0u;
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
            if (onBeginReadPixels)
                onBeginReadPixels();
            return beginReadPixelsResult;
        }

        NLS::Render::RHI::RHIReadbackResult BeginReadBuffer(
            const NLS::Render::RHI::RHIBufferReadbackDesc& desc) override
        {
            ++beginBufferReadbackCalls;
            lastBeginBufferReadbackDesc = desc;
            if (!beginReadBufferFailure.empty())
                throw std::runtime_error(beginReadBufferFailure);
            return beginReadBufferResult;
        }

        std::shared_ptr<NLS::Render::RHI::RHITexture> lastReadPixelsTexture;
        std::function<void()> onBeginReadPixels;
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
        size_t beginBufferReadbackCalls = 0u;
        std::string beginReadBufferFailure;
        NLS::Render::RHI::RHIBufferReadbackDesc lastBeginBufferReadbackDesc {};
        NLS::Render::RHI::RHIReadbackResult beginReadBufferResult {
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

    void ConfigureUiOverlayFrameContext(
        NLS::Render::Context::Driver& driver,
        const size_t slotIndex)
    {
        auto& frameContext =
            NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, slotIndex);
        auto commandBuffer = std::make_shared<TestCommandBuffer>();
        auto commandPool = std::make_shared<TestCommandPool>();
        commandPool->commandBuffer = commandBuffer;
        frameContext.commandBuffer = commandBuffer;
        frameContext.commandPool = commandPool;
        frameContext.frameFence = std::make_shared<TestFence>();
        frameContext.imageAcquiredSemaphore = std::make_shared<TestSemaphore>();
        frameContext.renderFinishedSemaphore = std::make_shared<TestSemaphore>();
        frameContext.descriptorAllocator = std::make_shared<TestDescriptorAllocator>();
        frameContext.uploadContext = std::make_shared<TestUploadContext>();
        frameContext.resourceStateTracker =
            NLS::Render::RHI::CreateDefaultResourceStateTracker();
    }
}

TEST(RenderFrameExecutionTests, ThumbnailPreviewReadbackPollDoesNotWaitForPendingFence)
{
    NLS::Editor::Assets::EditorThumbnailPreviewReadbackState state;
    auto completion = std::make_shared<TestCompletionToken>(NLS::Render::RHI::RHICompletionStatus{
        NLS::Render::RHI::RHICompletionStatusCode::Pending,
        {}
    });
    state.active = true;
    state.requestKey = "thumbnail:hero";
    state.width = 2u;
    state.height = 2u;
    state.rgbaPixels = std::make_shared<std::vector<uint8_t>>(16u, 255u);
    state.completion = completion;

    const auto polled = NLS::Editor::Assets::PollEditorThumbnailPreviewReadback(
        state,
        "thumbnail:hero");

    EXPECT_EQ(polled.status, NLS::Editor::Assets::EditorThumbnailPreviewReadbackPollStatus::Pending);
    EXPECT_TRUE(polled.preview.rgbaPixels.empty());
    EXPECT_EQ(completion->waitCalls, 0u);
    EXPECT_TRUE(state.active);
}

TEST(RenderFrameExecutionTests, ThumbnailPreviewReadbackStateKeepsRenderInputsAliveWhilePending)
{
    NLS::Editor::Assets::EditorThumbnailPreviewReadbackState state;
    auto completion = std::make_shared<TestCompletionToken>(NLS::Render::RHI::RHICompletionStatus{
        NLS::Render::RHI::RHICompletionStatusCode::Pending,
        {}
    });
    bool destroyed = false;
    state.active = true;
    state.requestKey = "thumbnail:hero";
    state.width = 2u;
    state.height = 2u;
    state.rgbaPixels = std::make_shared<std::vector<uint8_t>>(16u, 255u);
    state.completion = completion;
    state.renderInputsKeepAlive = std::shared_ptr<void>(
        new int(7),
        [&destroyed](void* value)
        {
            destroyed = true;
            delete static_cast<int*>(value);
        });

    const auto polled = NLS::Editor::Assets::PollEditorThumbnailPreviewReadback(
        state,
        "thumbnail:hero");

    EXPECT_EQ(polled.status, NLS::Editor::Assets::EditorThumbnailPreviewReadbackPollStatus::Pending);
    EXPECT_TRUE(state.renderInputsKeepAlive);
    EXPECT_FALSE(destroyed);
    state = {};
    EXPECT_TRUE(destroyed);
}

TEST(RenderFrameExecutionTests, ThumbnailPreviewReadbackPollPublishesReadyPixelsAndClearsState)
{
    NLS::Editor::Assets::EditorThumbnailPreviewReadbackState state;
    auto completion = std::make_shared<TestCompletionToken>(NLS::Render::RHI::RHICompletionStatus{
        NLS::Render::RHI::RHICompletionStatusCode::Success,
        {}
    });
    state.active = true;
    state.requestKey = "thumbnail:hero";
    state.width = 1u;
    state.height = 1u;
    state.rgbaPixels = std::make_shared<std::vector<uint8_t>>(
        std::initializer_list<uint8_t>{1u, 2u, 3u, 4u});
    state.completion = completion;
    NLS::Render::RHI::RHITextureDesc textureDesc;
    textureDesc.extent = {1u, 1u, 1u};
    textureDesc.debugName = "ThumbnailPreviewReadback";
    auto texture = std::make_shared<TestTexture>(textureDesc);
    NLS::Render::RHI::RHITextureViewDesc viewDesc;
    viewDesc.debugName = "ThumbnailPreviewReadbackView";
    auto textureView = std::make_shared<TestTextureView>(texture, viewDesc);
    auto renderTargetLease = std::make_shared<int>(1);
    state.gpuTexture = {texture, textureView, renderTargetLease, 1u, 1u};
    auto preparedSnapshot = std::make_shared<NLS::Editor::Assets::PreviewRenderableSnapshot>();
    preparedSnapshot->expectedDrawItemCount = 1u;
    state.previewSnapshot = preparedSnapshot;

    const auto polled = NLS::Editor::Assets::PollEditorThumbnailPreviewReadback(
        state,
        "thumbnail:hero");

    EXPECT_EQ(polled.status, NLS::Editor::Assets::EditorThumbnailPreviewReadbackPollStatus::Ready);
    EXPECT_EQ(polled.preview.width, 1u);
    EXPECT_EQ(polled.preview.height, 1u);
    EXPECT_EQ(polled.preview.rgbaPixels, (std::vector<uint8_t>{1u, 2u, 3u, 4u}));
    EXPECT_EQ(polled.preview.gpuTexture.texture, texture);
    EXPECT_EQ(polled.preview.gpuTexture.textureView, textureView);
    EXPECT_EQ(polled.preview.gpuTexture.renderTargetLease, renderTargetLease);
    EXPECT_TRUE(polled.preview.gpuTexture.IsValid());
    EXPECT_EQ(polled.preview.previewSnapshot, preparedSnapshot);
    EXPECT_EQ(completion->waitCalls, 0u);
    EXPECT_FALSE(state.active);
}

TEST(RenderFrameExecutionTests, ThumbnailPreviewReadbackPollClearsCompletedDifferentKeyWithoutWaiting)
{
    NLS::Editor::Assets::EditorThumbnailPreviewReadbackState state;
    auto completion = std::make_shared<TestCompletionToken>(NLS::Render::RHI::RHICompletionStatus{
        NLS::Render::RHI::RHICompletionStatusCode::Success,
        {}
    });
    state.active = true;
    state.requestKey = "thumbnail:old";
    state.width = 1u;
    state.height = 1u;
    state.rgbaPixels = std::make_shared<std::vector<uint8_t>>(
        std::initializer_list<uint8_t>{9u, 8u, 7u, 6u});
    state.completion = completion;

    const auto polled = NLS::Editor::Assets::PollEditorThumbnailPreviewReadback(
        state,
        "thumbnail:new");

    EXPECT_EQ(polled.status, NLS::Editor::Assets::EditorThumbnailPreviewReadbackPollStatus::Superseded);
    EXPECT_EQ(completion->waitCalls, 0u);
    EXPECT_FALSE(state.active);
    EXPECT_TRUE(polled.preview.rgbaPixels.empty());
}

TEST(RenderFrameExecutionTests, DriverPollReadbackCompletionMarksDeviceLostWithoutWaiting)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);
    auto* impl = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl, nullptr);

    auto completion = std::make_shared<TestCompletionToken>(NLS::Render::RHI::RHICompletionStatus{
        NLS::Render::RHI::RHICompletionStatusCode::DeviceLost,
        "async readback detected device removed"
    });
    NLS::Render::RHI::RHIReadbackResult pending{
        NLS::Render::RHI::RHIReadbackStatusCode::Success,
        {},
        completion
    };

    const auto result = NLS::Render::Context::DriverRendererAccess::PollReadbackCompletion(
        driver,
        pending);

    EXPECT_EQ(result.code, NLS::Render::RHI::RHIReadbackStatusCode::DeviceLost);
    EXPECT_EQ(result.message, "async readback detected device removed");
    EXPECT_EQ(completion->waitCalls, 0u);
    EXPECT_TRUE(impl->deviceLostDetected.load(std::memory_order_acquire));
    EXPECT_NE(impl->deviceLostReason.find("device removed"), std::string::npos);
}

TEST(RenderFrameExecutionTests, DriverDescriptorAllocationFailuresAccumulateAcrossSlotsAndAllocatorReset)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);
    NLS::Render::Context::DriverTestAccess::RebuildExplicitFrameContexts(driver, 2u);

    const auto* firstContext = NLS::Render::Context::DriverTestAccess::PeekFrameContext(driver, 0u);
    const auto* secondContext = NLS::Render::Context::DriverTestAccess::PeekFrameContext(driver, 1u);
    ASSERT_NE(firstContext, nullptr);
    ASSERT_NE(secondContext, nullptr);
    ASSERT_NE(firstContext->descriptorAllocator, nullptr);
    ASSERT_NE(secondContext->descriptorAllocator, nullptr);

    NLS::Render::RHI::DescriptorAllocationRequest overflowRequest;
    overflowRequest.count = 65537u;
    overflowRequest.lifetime = NLS::Render::RHI::DescriptorAllocationLifetime::TransientFrame;
    const auto before = NLS::Render::Context::DriverRendererAccess::GetThreadedFrameTelemetry(driver);

    EXPECT_FALSE(firstContext->descriptorAllocator->Allocate(overflowRequest).IsValid());
    firstContext->descriptorAllocator->Reset();
    EXPECT_FALSE(firstContext->descriptorAllocator->Allocate(overflowRequest).IsValid());
    EXPECT_FALSE(secondContext->descriptorAllocator->Allocate(overflowRequest).IsValid());

    const auto after = NLS::Render::Context::DriverRendererAccess::GetThreadedFrameTelemetry(driver);
    EXPECT_EQ(after.descriptorAllocationFailures - before.descriptorAllocationFailures, 3u);
}

TEST(RenderFrameExecutionTests, ParallelDrawCommandBatchMetadataGroupsPreparedWorkByPassRole)
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

TEST(RenderFrameExecutionTests, CompositeRendererPublishesThreadedFrameTelemetryWhenThreadedModeIsEnabled)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 2u;

    auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    NLS::Core::ServiceLocator::Provide(*driver);

    VisibleSnapshotPublishingRenderer renderer(*driver);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 64u;
    frameDescriptor.renderHeight = 64u;
    frameDescriptor.camera = &camera;

    renderer.BeginFrame(frameDescriptor);
    renderer.EndFrame();

    const auto telemetry = NLS::Render::Context::DriverRendererAccess::GetThreadedFrameTelemetry(*driver);
    EXPECT_EQ(telemetry.publishedFrameCount, 1u);
    EXPECT_EQ(telemetry.inFlightFrameCount, 1u);
    EXPECT_EQ(telemetry.blockedPublishCount, 0u);
    EXPECT_EQ(telemetry.publishState, NLS::Render::Data::FramePublishState::Open);
}

TEST(RenderFrameExecutionTests, PreparedBuilderPublishReportsActualFrameId)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.renderWidth = 64u;
    snapshot.renderHeight = 64u;

    const auto builder = [](const size_t)
    {
        NLS::Render::Context::RenderScenePackage package;
        package.visibleDrawCount = 1u;
        package.hasVisibleDraws = true;
        package.frameDataReady = true;
        return package;
    };

    size_t publishedSlot = 99u;
    uint64_t publishedFrameId = 0u;
    auto result = NLS::Tests::ObserveNextRenderSceneForTest(driver);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
        driver,
        snapshot,
        builder,
        &publishedSlot,
        &publishedFrameId));

    EXPECT_EQ(publishedSlot, 0u);
    EXPECT_NE(publishedFrameId, 0u);

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);
    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(result.get().frameId, publishedFrameId);
}

TEST(RenderFrameExecutionTests, GpuRetirementTargetsFollowInjectedAndReplacementDevice)
{
    using namespace NLS::Render;
    Settings::DriverSettings settings;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;
    Context::Driver driver(settings);
    Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto firstDevice = std::make_shared<TestExplicitDevice>();
    Context::DriverTestAccess::SetExplicitDevice(driver, firstDevice);
    auto& executor = Context::RhiCommandListExecutor::Get();
    const size_t initialQuarantineCount = executor.GetQuarantinedResourceReleaseCount();
    ASSERT_TRUE(executor.RunOnSubmitLane("FirstDeviceWork", [] { return RHI::RHIQueueOperationResult {}; }).Succeeded());
    ASSERT_TRUE(executor.Flush(Context::RhiFlushFlags::Gpu | Context::RhiFlushFlags::ReleaseResources));
    EXPECT_FALSE(executor.HasFailed());
    const size_t firstSubmits = firstDevice->GetTestQueue()->submitCalls;
    EXPECT_GT(firstSubmits, 0u);

    auto secondDevice = std::make_shared<TestExplicitDevice>();
    Context::DriverTestAccess::SetExplicitDevice(driver, secondDevice);
    ASSERT_TRUE(executor.RunOnSubmitLane("SecondDeviceWork", [] { return RHI::RHIQueueOperationResult {}; }).Succeeded());
    ASSERT_TRUE(executor.Flush(Context::RhiFlushFlags::Gpu | Context::RhiFlushFlags::ReleaseResources));
    EXPECT_FALSE(executor.HasFailed());
    EXPECT_GT(secondDevice->GetTestQueue()->submitCalls, 0u);
    EXPECT_EQ(firstDevice->GetTestQueue()->submitCalls, firstSubmits);
    EXPECT_EQ(executor.GetPendingResourceReleaseCount(), 0u);
    EXPECT_EQ(executor.GetQuarantinedResourceReleaseCount(), initialQuarantineCount);
}

TEST(RenderFrameExecutionTests, ThreadedWorkerWakeObservedBeforeWaitReturnsWithoutTimeout)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto* impl = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl, nullptr);

    const uint64_t observedGeneration =
        impl->threadedWorkerWake.Generation();
    NLS::Render::Context::Detail::NotifyThreadedWorkers(*impl);

    EXPECT_TRUE(NLS::Render::Context::Detail::WaitForThreadedWorkerWake(
        *impl,
        observedGeneration));
}

TEST(RenderFrameExecutionTests, OffscreenPreparedBuilderLeavesPendingUiSnapshotForUiOnlyFrame)
{
    ImGuiContextGuard imguiContext;
    ScopedOverlayShaderManager shaderManagerScope;
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 2u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    explicitDevice->MutableCapabilities().SetFeature(
        NLS::Render::RHI::RHIDeviceFeature::UIOverlayFrameGraph,
        true,
        "test overlay support");
    auto swapchain = std::make_shared<TestSwapchain>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);
    NLS::Render::Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);

    auto uiSnapshot = MakeVisibleUiSnapshot(918u, 128.0f, 72.0f);
    NLS::Render::Context::DriverUIAccess::PublishUiDrawDataSnapshot(driver, uiSnapshot);

    NLS::Render::Context::RenderFramePacket offscreenSnapshot;
    offscreenSnapshot.frameId = 917u;
    offscreenSnapshot.targetsSwapchain = false;
    offscreenSnapshot.renderWidth = 64u;
    offscreenSnapshot.renderHeight = 64u;

    size_t offscreenSlot = 99u;
    auto result = NLS::Tests::ObserveNextRenderSceneForTest(driver);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
        driver,
        offscreenSnapshot,
        [](const size_t)
        {
            NLS::Render::Context::RenderScenePackage package;
            package.frameId = 917u;
            package.targetsSwapchain = false;
            package.renderWidth = 64u;
            package.renderHeight = 64u;
            package.visibleDrawCount = 1u;
            package.opaqueDrawCount = 1u;
            package.hasVisibleDraws = true;
            package.hasOpaquePass = true;
            package.frameDataReady = true;
            package.objectDataReady = true;
            return package;
        }, &offscreenSlot));

    NLS::Tests::RunOnRenderThread([] {});
    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto package = result.get();
    EXPECT_EQ(package.frameId, offscreenSnapshot.frameId);
    EXPECT_FALSE(package.targetsSwapchain);
    EXPECT_FALSE(package.hasUIOverlayPass);

    EXPECT_EQ(
        NLS::Render::Context::DriverUIAccess::ConsumePendingUiDrawDataSnapshot(driver),
        uiSnapshot)
        << "Resolved offscreen prepared scene packages must not consume a pending swapchain UI overlay snapshot.";
    NLS::Render::Context::DriverUIAccess::PublishUiDrawDataSnapshot(driver, uiSnapshot);

    size_t publishedUiSlot = 99u;
    uint64_t publishedUiFrameId = 0u;
    ASSERT_TRUE(NLS::Render::Context::DriverUIAccess::PublishUiOnlyFrame(
        driver,
        128u,
        72u,
        &publishedUiSlot,
        &publishedUiFrameId));

    EXPECT_EQ(publishedUiFrameId, uiSnapshot->frameId);
    EXPECT_NE(publishedUiSlot, 99u);
    EXPECT_EQ(
        NLS::Render::Context::DriverUIAccess::ConsumePendingUiDrawDataSnapshot(driver),
        nullptr)
        << "Successful UI-only publication should clear the consumed pending snapshot.";
}

TEST(RenderFrameExecutionTests, UiOnlyFrameSuppressesUnchangedContentUntilTextureOrSwapchainInvalidation)
{
    ImGuiContextGuard imguiContext;
    ScopedOverlayShaderManager shaderManagerScope;
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 3u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    explicitDevice->MutableCapabilities().SetFeature(
        NLS::Render::RHI::RHIDeviceFeature::UIOverlayFrameGraph,
        true,
        "test overlay support");
    auto swapchain = std::make_shared<TestSwapchain>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);
    NLS::Render::Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);
    auto* impl = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl, nullptr);

    NLS::Render::RHI::RHITextureDesc changedTextureDesc;
    changedTextureDesc.extent = {128u, 72u, 1u};
    changedTextureDesc.debugName = "ChangedUiTexture";
    auto changedTexture = std::make_shared<TestTexture>(
        changedTextureDesc,
        NLS::Render::RHI::ResourceState::ShaderRead);
    NLS::Render::RHI::RHITextureViewDesc changedTextureViewDesc;
    changedTextureViewDesc.debugName = "ChangedUiTextureView";
    auto changedTextureView = std::make_shared<TestTextureView>(
        changedTexture,
        changedTextureViewDesc);
    const auto changedTextureId =
        NLS::Render::Context::DriverUIAccess::RegisterUiTextureView(
            driver,
            changedTextureView,
            NLS::Render::UI::UiTextureSynchronizationScope::PreviousFrameOrStatic);
    ASSERT_NE(changedTextureId.value, 0u);

    auto firstSnapshot = MakeVisibleUiSnapshot(920u, 128.0f, 72.0f, changedTextureId);
    NLS::Render::Context::DriverUIAccess::PublishUiDrawDataSnapshot(driver, firstSnapshot);
    uint64_t firstPresentationInvalidationGeneration = 0u;
    {
        std::lock_guard lock(impl->pendingUiOverlaySnapshotMutex);
        firstPresentationInvalidationGeneration =
            impl->uiOverlayPresentationInvalidationGeneration;
    }
    ASSERT_TRUE(NLS::Render::Context::DriverUIAccess::PublishUiOnlyFrame(driver, 128u, 72u));
    NLS::Render::Context::DriverUIAccess::RecordUiOverlayPresentationSucceeded(
        driver,
        firstSnapshot,
        firstPresentationInvalidationGeneration);

    auto unchangedSnapshot = MakeVisibleUiSnapshot(921u, 128.0f, 72.0f, changedTextureId);
    const uint64_t wakeGenerationBeforeUnchangedPublish =
        impl->threadedWorkerWake.Generation();
    NLS::Render::Context::DriverUIAccess::PublishUiDrawDataSnapshot(driver, unchangedSnapshot);
    EXPECT_EQ(
        impl->threadedWorkerWake.Generation(),
        wakeGenerationBeforeUnchangedPublish)
        << "Publishing a new snapshot object with identical UI content must not wake workers.";
    EXPECT_FALSE(NLS::Render::Context::DriverUIAccess::PublishUiOnlyFrame(driver, 128u, 72u));
    EXPECT_EQ(impl->skippedUnchangedUiOnlyFrameCount, 1u);
    {
        std::lock_guard lock(impl->pendingUiOverlaySnapshotMutex);
        EXPECT_EQ(impl->pendingUiOverlaySnapshot, unchangedSnapshot)
            << "Suppression must retain the latest snapshot for a subsequent scene frame.";
    }

    NLS::Render::RHI::RHITextureDesc unregisteredTextureDesc;
    unregisteredTextureDesc.extent = {128u, 72u, 1u};
    unregisteredTextureDesc.debugName = "UnregisteredUiTexture";
    auto unregisteredTexture = std::make_shared<TestTexture>(
        unregisteredTextureDesc,
        NLS::Render::RHI::ResourceState::ShaderRead);
    auto unregisteredTextureView = std::make_shared<TestTextureView>(
        unregisteredTexture,
        NLS::Render::RHI::RHITextureViewDesc{});
    uint64_t invalidationGenerationBeforeUnregisteredNotification = 0u;
    {
        std::lock_guard lock(impl->pendingUiOverlaySnapshotMutex);
        invalidationGenerationBeforeUnregisteredNotification =
            impl->uiOverlayPresentationInvalidationGeneration;
    }
    NLS::Render::Context::DriverUIAccess::NotifyUiTextureContentChanged(
        driver,
        unregisteredTextureView);
    {
        std::lock_guard lock(impl->pendingUiOverlaySnapshotMutex);
        EXPECT_EQ(
            impl->uiOverlayPresentationInvalidationGeneration,
            invalidationGenerationBeforeUnregisteredNotification);
    }
    EXPECT_FALSE(NLS::Render::Context::DriverUIAccess::PublishUiOnlyFrame(driver, 128u, 72u))
        << "An unregistered texture must not invalidate unchanged UI presentation.";

    uint64_t consumedSnapshotGeneration = 0u;
    uint64_t observedPresentationInvalidationGeneration = 0u;
    auto consumedSnapshot =
        NLS::Render::Context::DriverUIAccess::ConsumePendingUiDrawDataSnapshot(
            driver,
            &consumedSnapshotGeneration,
            &observedPresentationInvalidationGeneration);
    ASSERT_EQ(consumedSnapshot, unchangedSnapshot);
    {
        std::lock_guard lock(impl->pendingUiOverlaySnapshotMutex);
        EXPECT_EQ(
            impl->uiOverlayPresentationInvalidationGeneration,
            observedPresentationInvalidationGeneration);
    }
    NLS::Render::Context::DriverUIAccess::NotifyUiTextureContentChanged(
        driver,
        changedTextureView);
    uint64_t changedContentGeneration = 0u;
    {
        std::lock_guard lock(impl->pendingUiOverlaySnapshotMutex);
        changedContentGeneration = impl->uiOverlayPresentationInvalidationGeneration;
        EXPECT_NE(
            impl->uiOverlayPresentationInvalidationGeneration,
            observedPresentationInvalidationGeneration)
            << "A content change after snapshot consumption must remain newer than the observed generation.";
    }
    ASSERT_TRUE(NLS::Render::Context::DriverUIAccess::RestoreConsumedUiDrawDataSnapshotIfUnchanged(
        driver,
        consumedSnapshot,
        consumedSnapshotGeneration));
    EXPECT_TRUE(NLS::Render::Context::DriverUIAccess::PublishUiOnlyFrame(driver, 128u, 72u))
        << "New pixels in a sampled UI texture must refresh unchanged ImGui draw commands.";
    NLS::Render::Context::DriverUIAccess::RecordUiOverlayPresentationSucceeded(
        driver,
        consumedSnapshot,
        changedContentGeneration);

    auto stillUnchangedSnapshot = MakeVisibleUiSnapshot(922u, 128.0f, 72.0f, changedTextureId);
    NLS::Render::Context::DriverUIAccess::PublishUiDrawDataSnapshot(driver, stillUnchangedSnapshot);
    EXPECT_FALSE(NLS::Render::Context::DriverUIAccess::PublishUiOnlyFrame(driver, 128u, 72u))
        << "Texture invalidation must force exactly one refresh; truly static UI remains suppressed.";

    NLS::Render::Context::DriverUIAccess::PublishUiDrawDataSnapshot(driver, unchangedSnapshot);
    NLS::Render::Context::DriverUIAccess::NotifyUiOverlaySwapchainWillResize(driver);
    EXPECT_TRUE(NLS::Render::Context::DriverUIAccess::PublishUiOnlyFrame(driver, 128u, 72u))
        << "Resize invalidation must force one UI-only refresh even when draw content is unchanged.";
}

TEST(RenderFrameExecutionTests, ConsumedUiSnapshotRestoreDoesNotOverwriteNewerPendingSnapshot)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 2u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    auto oldSnapshot = MakeVisibleUiSnapshot(918u, 128.0f, 72.0f);
    auto newerSnapshot = MakeVisibleUiSnapshot(919u, 128.0f, 72.0f);
    NLS::Render::Context::DriverUIAccess::PublishUiDrawDataSnapshot(driver, oldSnapshot);

    uint64_t consumedGeneration = 0u;
    EXPECT_EQ(
        NLS::Render::Context::DriverUIAccess::ConsumePendingUiDrawDataSnapshot(driver, &consumedGeneration),
        oldSnapshot);

    NLS::Render::Context::DriverUIAccess::PublishUiDrawDataSnapshot(driver, newerSnapshot);
    EXPECT_FALSE(NLS::Render::Context::DriverUIAccess::RestoreConsumedUiDrawDataSnapshotIfUnchanged(
        driver,
        oldSnapshot,
        consumedGeneration));

    EXPECT_EQ(
        NLS::Render::Context::DriverUIAccess::ConsumePendingUiDrawDataSnapshot(driver),
        newerSnapshot)
        << "A failed scene-attach path must not requeue an older UI snapshot over a newer pending snapshot.";
}

TEST(RenderFrameExecutionTests, TelemetryTracksLatestPublishedAndRetiredFrameIds)
{
    using namespace NLS::Render;
    Settings::DriverSettings settings;
    settings.graphicsBackend = Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;
    Context::Driver driver(settings);
    Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    Context::DriverTestAccess::SetExplicitDevice(driver, std::make_shared<TestExplicitDevice>());
    Context::DriverTestAccess::SetExplicitSwapchain(driver, std::make_shared<TestSwapchain>());
    Context::RenderFramePacket packet;
    packet.frameId = 41u;
    ASSERT_TRUE(Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, packet,
        [packet](size_t) { return Context::BuildRenderFramePackage(packet); }));

    auto telemetry = Context::DriverRendererAccess::GetThreadedFrameTelemetry(driver);
    EXPECT_EQ(telemetry.latestPublishedFrameId, 41u);
    EXPECT_EQ(telemetry.latestRetiredFrameId, 0u);
    EXPECT_EQ(telemetry.inFlightFrameCount, 1u);
    ASSERT_TRUE(Context::DriverRendererAccess::TryDrainThreadedRendering(driver, false));

    telemetry = Context::DriverRendererAccess::GetThreadedFrameTelemetry(driver);
    EXPECT_EQ(telemetry.latestPublishedFrameId, 41u);
    EXPECT_EQ(telemetry.latestRetiredFrameId, 41u);
    EXPECT_EQ(telemetry.inFlightFrameCount, 0u);
}

TEST(RenderFrameExecutionTests, FailedRhiSubmissionDoesNotAdvanceSuccessfulRetiredFrameId)
{
    using namespace NLS::Render;
    Settings::DriverSettings settings;
    settings.graphicsBackend = Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;
    Context::Driver driver(settings);
    Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto device = std::make_shared<TestExplicitDevice>();
    device->GetTestQueue()->nextSubmitResult = {RHI::RHIQueueOperationStatusCode::DeviceLost, "submit failed"};
    Context::DriverTestAccess::SetExplicitDevice(driver, device);
    Context::DriverTestAccess::SetExplicitSwapchain(driver, std::make_shared<TestSwapchain>());
    Context::RenderFramePacket packet;
    packet.frameId = 73u;
    auto result = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    ASSERT_TRUE(Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, packet,
        [packet](size_t) { return Context::BuildRenderFramePackage(packet); }));
    ASSERT_TRUE(Context::DriverRendererAccess::TryDrainThreadedRendering(driver, false));
    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto frame = result.get();
    EXPECT_FALSE(frame.submittedSuccessfully);
    EXPECT_EQ(frame.currentFrameQueueOperationFailureCount, 1u);
    EXPECT_NE(frame.currentFrameLastQueueOperationFailure.find("submit failed"), std::string::npos);

    const auto telemetry = Context::DriverRendererAccess::GetThreadedFrameTelemetry(driver);
    EXPECT_EQ(telemetry.latestRetiredFrameId, 0u)
        << "Cached render resources may only be reused after a successfully submitted retired frame.";
    EXPECT_EQ(telemetry.latestFailedRetiredFrameId, 73u)
        << "Consumers that cache GPU-produced resources need to reject the exact failed target frame even after a later frame succeeds.";
    EXPECT_TRUE(telemetry.deviceLostDetected);
}

TEST(RenderFrameExecutionTests, FailedRetiredFrameIdSurvivesLaterSuccessfulRetirement)
{
    using namespace NLS::Render;
    Settings::DriverSettings settings;
    settings.graphicsBackend = Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;
    Context::Driver driver(settings);
    Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto device = std::make_shared<TestExplicitDevice>();
    Context::DriverTestAccess::SetExplicitDevice(driver, device);
    Context::DriverTestAccess::SetExplicitSwapchain(driver, std::make_shared<TestSwapchain>());
    auto buffer = std::dynamic_pointer_cast<TestCommandBuffer>(
        Context::DriverTestAccess::EnsureFrameContext(driver, 0u).commandBuffer);
    ASSERT_NE(buffer, nullptr);
    buffer->stayNonRecordingOnBegin = true;
    Context::RenderFramePacket failedSnapshot;
    failedSnapshot.frameId = 81u;
    auto failedResult = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    ASSERT_TRUE(Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, failedSnapshot,
        [failedSnapshot](size_t) { return Context::BuildRenderFramePackage(failedSnapshot); }));
    ASSERT_TRUE(Context::DriverRendererAccess::TryDrainThreadedRendering(driver, false));
    ASSERT_EQ(failedResult.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto failedFrame = failedResult.get();
    EXPECT_FALSE(failedFrame.submittedSuccessfully);
    EXPECT_NE(failedFrame.lastCommandRecordingFailure.find("failed to begin"), std::string::npos);
    EXPECT_EQ(buffer->beginCalls, 1u);
    EXPECT_EQ(device->GetTestQueue()->submitCalls, 0u);

    auto telemetry = Context::DriverRendererAccess::GetThreadedFrameTelemetry(driver);
    ASSERT_EQ(telemetry.latestRetiredFrameId, 0u);
    ASSERT_EQ(telemetry.latestFailedRetiredFrameId, failedSnapshot.frameId);

    buffer->stayNonRecordingOnBegin = false;
    Context::RenderFramePacket successfulSnapshot;
    successfulSnapshot.frameId = 82u;
    ASSERT_TRUE(Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, successfulSnapshot,
        [successfulSnapshot](size_t) { return Context::BuildRenderFramePackage(successfulSnapshot); }));
    ASSERT_TRUE(Context::DriverRendererAccess::TryDrainThreadedRendering(driver, false));

    telemetry = Context::DriverRendererAccess::GetThreadedFrameTelemetry(driver);
    EXPECT_EQ(buffer->beginCalls, 2u);
    EXPECT_EQ(device->GetTestQueue()->submitCalls, 1u);
    EXPECT_EQ(telemetry.latestRetiredFrameId, successfulSnapshot.frameId);
    EXPECT_EQ(telemetry.latestFailedRetiredFrameId, failedSnapshot.frameId)
        << "A later success must not make a cache target from a failed frame look valid.";
}

TEST(RenderFrameExecutionTests, UnsubmittedRhiFrameDoesNotAdvanceSuccessfulRetiredFrameId)
{
    using namespace NLS::Render;
    Settings::DriverSettings settings;
    settings.graphicsBackend = Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;
    Context::Driver driver(settings);
    Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto device = std::make_shared<TestExplicitDevice>();
    Context::DriverTestAccess::SetExplicitDevice(driver, device);
    Context::DriverTestAccess::SetExplicitSwapchain(driver, std::make_shared<TestSwapchain>());
    auto buffer = std::dynamic_pointer_cast<TestCommandBuffer>(
        Context::DriverTestAccess::EnsureFrameContext(driver, 0u).commandBuffer);
    ASSERT_NE(buffer, nullptr);
    buffer->stayNonRecordingOnBegin = true;
    Context::RenderFramePacket packet;
    packet.frameId = 74u;
    auto result = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    ASSERT_TRUE(Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, packet,
        [packet](size_t) { return Context::BuildRenderFramePackage(packet); }));
    ASSERT_TRUE(Context::DriverRendererAccess::TryDrainThreadedRendering(driver, false));
    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto frame = result.get();
    EXPECT_FALSE(frame.submittedSuccessfully);
    EXPECT_NE(frame.lastCommandRecordingFailure.find("failed to begin"), std::string::npos);
    EXPECT_EQ(buffer->beginCalls, 1u);
    EXPECT_EQ(device->GetTestQueue()->submitCalls, 0u);

    const auto telemetry = Context::DriverRendererAccess::GetThreadedFrameTelemetry(driver);
    EXPECT_EQ(telemetry.latestRetiredFrameId, 0u);
    EXPECT_EQ(telemetry.latestFailedRetiredFrameId, 74u);
}

TEST(RenderFrameExecutionTests, DefaultDriverAlwaysCreatesRenderThreadsAndFrameTelemetry)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    NLS::Core::ServiceLocator::Provide(*driver);

    EXPECT_TRUE(NLS::Render::Context::RenderingThreadSystem::HasActiveRenderThread());
    EXPECT_TRUE(NLS::Render::Context::DriverRendererAccess::IsThreadedRenderingEnabled(*driver));
    EXPECT_TRUE(NLS::Render::Context::DriverRendererAccess::TryGetThreadedFrameTelemetry(*driver).has_value());

    const auto telemetry = NLS::Render::Context::DriverRendererAccess::GetThreadedFrameTelemetry(*driver);
    EXPECT_EQ(telemetry.inFlightFrameCount, 0u);
}

TEST(RenderFrameExecutionTests, ExplicitFrameResourcesAndLatencyUseConfiguredFramesInFlight)
{
    for (const uint32_t requestedCount : {0u, 1u, 3u})
    {
        SCOPED_TRACE(requestedCount);
        NLS::Render::Settings::DriverSettings settings;
        settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
        settings.enableExplicitRHI = false;
        settings.framesInFlight = requestedCount;

        NLS::Render::Context::Driver driver(settings);
        auto explicitDevice = std::make_shared<TestExplicitDevice>();
        NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

        const size_t expectedCount = (std::max)(size_t{1u}, size_t{requestedCount});
        EXPECT_EQ(NLS::Render::Context::DriverRendererAccess::GetFrameContextSlotCount(driver), expectedCount);
        auto* impl = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver);
        ASSERT_NE(impl->frameResourceSlots, nullptr);
        EXPECT_EQ(impl->frameResourceSlots->GetSlotCount(), expectedCount);
        ASSERT_NE(impl->frameLatency, nullptr);
        EXPECT_EQ(impl->frameLatency->GetMaxFramesInFlight(), expectedCount);
        for (size_t slotIndex = 0u; slotIndex < expectedCount; ++slotIndex)
        {
            const auto* frameContext = NLS::Render::Context::DriverTestAccess::PeekFrameContext(driver, slotIndex);
            ASSERT_NE(frameContext, nullptr) << "slotIndex=" << slotIndex;
            EXPECT_EQ(frameContext->frameIndex, slotIndex);
            EXPECT_NE(frameContext->descriptorAllocator, nullptr);
        }
    }
}

TEST(RenderFrameExecutionTests, ReusableFrameContextReservationWaitsForGpuFenceBeforeObjectDataReuse)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto frameFence = std::make_shared<TestFence>();
    frameFence->signaled = false;
    frameContext.frameFence = frameFence;

    auto& ring = *NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver)->frameResourceSlots;
    ASSERT_NE(ring.AcquireSlot(0u), nullptr);
    ring.RetireSlot(0u, frameFence);
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

TEST(RenderFrameExecutionTests, PreparedPublicationZeroRetirementWaitDoesNotBlockOnGpuFence)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto frameFence = std::make_shared<TestFence>();
    frameFence->signaled = false;
    frameFence->waitResults = { false };
    frameContext.frameFence = frameFence;

    auto& ring = *NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver)->frameResourceSlots;
    ASSERT_NE(ring.AcquireSlot(0u), nullptr);
    ring.RetireSlot(0u, frameFence);
    EXPECT_FALSE(NLS::Render::Context::DriverRendererAccess::ReserveReusableFrameContextSlotIndexForPreparedPublication(driver).has_value());
    EXPECT_EQ(frameFence->waitCalls, 0u)
        << "A zero publish retirement wait is a non-blocking poll, not an infinite GPU fence wait.";
    EXPECT_FALSE(NLS::Render::Context::DriverRendererAccess::GetReservedFrameContextSlotIndex(driver).has_value());

    // Keep Driver teardown focused on the reservation path already asserted above.
    frameFence->signaled = true;
}

TEST(RenderFrameExecutionTests, ReusableFrameContextReservationAllowsRetiredNoGpuWorkSlotWithoutFenceSignal)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto frameFence = std::make_shared<TestFence>();
    frameFence->signaled = false;
    frameContext.frameFence = frameFence;

    auto& ring = *NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver)->frameResourceSlots;
    ASSERT_NE(ring.AcquireSlot(0u), nullptr);
    ring.RetireSlot(0u, nullptr);

    const auto reusableSlot =
        NLS::Render::Context::DriverRendererAccess::ReserveReusableFrameContextSlotIndex(driver);
    ASSERT_TRUE(reusableSlot.has_value());
    EXPECT_EQ(reusableSlot.value(), 0u);

    // Keep Driver teardown focused on the reservation path already asserted above.
    frameFence->signaled = true;
}

TEST(RenderFrameExecutionTests, CompositeRendererKeepsFrameDescriptorsUntilPreparedBuilderIsCaptured)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

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

    auto result = NLS::Tests::ObserveNextRenderSceneForTest(driver);
    renderer.BeginFrame(frameDescriptor);
    renderer.EndFrame();

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(result.get().sceneGameObjectCount, 7u);
}

TEST(RenderFrameExecutionTests, ThreadedRenderingIsDisabledForNonTierAExplicitBackends)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    explicitDevice->SetNativeBackendType(NLS::Render::RHI::NativeBackendType::OpenGL);
    explicitDevice->MutableCapabilities().backendReady = false;
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    EXPECT_FALSE(NLS::Render::Context::DriverRendererAccess::IsThreadedRenderingEnabled(driver));
}

TEST(RenderFrameExecutionTests, ThreadedRenderingStaysEnabledForTierAFoundationBackends)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

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

TEST(RenderFrameExecutionTests, RendererFrameAdmissionReservesBeforeCaptureAndReusesOneLatencyBudget)
{
    using namespace NLS::Render::Context;
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;
    Driver driver(settings);
    DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto device = std::make_shared<TestExplicitDevice>();
    DriverTestAccess::SetExplicitDevice(driver, device);
    auto* impl = DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl->frameLatency, nullptr);

    for (uint64_t frame = 1u; frame <= 3u; ++frame)
    {
        ASSERT_TRUE(RenderThreadCoordinator::BeginRendererFrame(driver));
        const auto reservation = DriverRendererAccess::GetReservedFrameContextSlotIndex(driver);
        EXPECT_TRUE(reservation.has_value());
        EXPECT_EQ(impl->frameLatency->GetInFlightFrameCount(), 1u);
        EXPECT_EQ(DriverRendererAccess::ReserveReusableFrameContextSlotIndex(driver), reservation);
        EXPECT_EQ(impl->frameLatency->GetOpenedFrameCount(), frame);

        RenderThreadCoordinator::EndRendererFrame(driver);
        EXPECT_FALSE(DriverRendererAccess::GetReservedFrameContextSlotIndex(driver).has_value());
        EXPECT_EQ(impl->frameLatency->GetInFlightFrameCount(), 0u);
        EXPECT_EQ(impl->frameLatency->GetRetiredFrameCount(), frame);
    }
    EXPECT_EQ(device->GetTestQueue()->submitCalls, 0u);
}

TEST(RenderFrameExecutionTests, CancellingQueuedFramesWithoutADeviceReturnsResourceLeasesAndLatencyBudget)
{
    using namespace NLS::Render::Context;
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 2u;
    Driver driver(settings);
    DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    ASSERT_FALSE(DriverRendererAccess::HasExplicitRHI(driver));
    auto* impl = DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl->frameResourceSlots, nullptr);
    ASSERT_NE(impl->frameLatency, nullptr);
    EXPECT_EQ(impl->frameResourceSlots->GetSlotCount(), 2u);

    size_t builderCalls = 0u;
    std::array<std::weak_ptr<int>, 2> capturedResources;
    for (size_t index = 0u; index < capturedResources.size(); ++index)
    {
        auto resource = std::make_shared<int>(static_cast<int>(index));
        capturedResources[index] = resource;
        RenderFramePacket packet;
        packet.frameId = index + 1u;
        packet.targetsSwapchain = false;
        ASSERT_TRUE(RenderThreadCoordinator::TryEnqueueRenderFrame(driver, packet,
            [resource, &builderCalls](const size_t)
            {
                ++builderCalls;
                return RenderScenePackage{};
            }, false, nullptr, nullptr, false, false));
    }
    EXPECT_EQ(impl->frameResourceSlots->GetInFlightSlotCount(), 2u);
    EXPECT_EQ(impl->frameLatency->GetInFlightFrameCount(), 2u);
    EXPECT_EQ(impl->pendingRenderFrameCommands.load(), 2u);
    for (const auto& resource : capturedResources)
        EXPECT_FALSE(resource.expired());

    RenderFramePacket rejected;
    rejected.targetsSwapchain = false;
    EXPECT_FALSE(RenderThreadCoordinator::TryEnqueueRenderFrame(driver, rejected,
        [&builderCalls](const size_t) { ++builderCalls; return RenderScenePackage{}; },
        false, nullptr, nullptr, false, false));
    EXPECT_EQ(impl->frameLatency->GetOpenedFrameCount(), 2u);

    RenderCommandDispatcher::CancelPendingCommands("Queued frame cancellation regression");
    EXPECT_EQ(builderCalls, 0u);
    EXPECT_EQ(impl->pendingRenderFrameCommands.load(), 0u);
    EXPECT_EQ(impl->frameResourceSlots->GetInFlightSlotCount(), 0u);
    EXPECT_EQ(impl->frameLatency->GetInFlightFrameCount(), 0u);
    EXPECT_EQ(impl->frameLatency->GetRetiredFrameCount(), 2u);
    for (const auto& resource : capturedResources)
        EXPECT_TRUE(resource.expired());

    ASSERT_TRUE(RenderThreadCoordinator::BeginRendererFrame(driver, false));
    EXPECT_EQ(impl->frameLatency->GetOpenedFrameCount(), 3u);
    EXPECT_EQ(impl->frameResourceSlots->GetInFlightSlotCount(), 1u);
    RenderThreadCoordinator::EndRendererFrame(driver);
    EXPECT_EQ(impl->frameResourceSlots->GetInFlightSlotCount(), 0u);
    EXPECT_EQ(impl->frameLatency->GetRetiredFrameCount(), 3u);
}

TEST(RenderFrameExecutionTests, BackgroundPreviewAdmissionReturnsImmediatelyWhenTheFrameRingIsFull)
{
    using namespace NLS::Render::Context;
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;
    Driver driver(settings);
    DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    DriverTestAccess::SetExplicitDevice(driver, std::make_shared<TestExplicitDevice>());
    ASSERT_TRUE(RenderThreadCoordinator::BeginRendererFrame(driver));
    RenderFramePacket snapshot;
    snapshot.targetsSwapchain = false;
    ASSERT_TRUE(DriverRendererAccess::TryEnqueueRenderFrame(driver, snapshot, [](const size_t)
    {
        return RenderScenePackage{};
    }));
    RenderThreadCoordinator::EndRendererFrame(driver);

    const auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(RenderThreadCoordinator::BeginRendererFrame(driver, false));
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(200));
    EXPECT_FALSE(DriverRendererAccess::GetReservedFrameContextSlotIndex(driver).has_value());
    EXPECT_EQ(DriverTestAccess::GetImplForTesting(driver)->frameLatency->GetInFlightFrameCount(), 1u);
    DriverTestAccess::DrainThreadedRendering(driver);
}

TEST(RenderFrameExecutionTests, ThreadedRendererNeverUsesStandaloneExplicitFrameRecordingForRuntimeVisibility)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

    auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
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

TEST(RenderFrameExecutionTests, ThreadedRendererPublishesSnapshotForRuntimeVisibility)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

    auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
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

TEST(RenderFrameExecutionTests, ThreadedVisibleFrameRecordsSwapchainPassPlanThroughRhiWorker)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

TEST(RenderFrameExecutionTests, ThreadedUiOverlayPassRecordsSnapshotDrawsThroughRhiWorker)
{
    ImGuiContextGuard imguiContext;
    ScopedOverlayShaderManager shaderManagerScope;
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 407u;
    snapshot.targetsSwapchain = true;
    snapshot.renderWidth = 128u;
    snapshot.renderHeight = 72u;

    auto package = NLS::Render::Context::BuildRenderFramePackage(snapshot);
    auto uiSnapshot = std::make_shared<NLS::Render::UI::UiDrawDataSnapshot>();
    uiSnapshot->frameId = snapshot.frameId;
    uiSnapshot->hasVisibleDraws = true;
    uiSnapshot->displaySize[0] = 128.0f;
    uiSnapshot->displaySize[1] = 72.0f;
    uiSnapshot->framebufferScale[0] = 1.0f;
    uiSnapshot->framebufferScale[1] = 1.0f;
    uiSnapshot->totalVertexCount = 3u;
    uiSnapshot->totalIndexCount = 3u;
    NLS::Render::UI::UiDrawListSnapshot drawList;
    drawList.vertices.resize(3u);
    drawList.indices = { 0u, 1u, 2u };
    drawList.commands.push_back({
        3u,
        0u,
        0u,
        { 4.0f, 5.0f, 44.0f, 45.0f },
        {},
        NLS::Render::UI::UiDrawCallbackKind::None,
        false
    });
    uiSnapshot->drawLists.push_back(std::move(drawList));
    ASSERT_TRUE(NLS::Render::Context::AttachUiOverlaySnapshotToRenderScenePackage(package, uiSnapshot));
    auto result = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, snapshot, [package](size_t) { return package; }));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto submissionFrame = result.get();
    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u)
        << submissionFrame.lastCommandRecordingFailure;
    EXPECT_EQ(explicitDevice->GetTestQueue()->presentCalls, 1u)
        << submissionFrame.lastCommandRecordingFailure;
    auto submittedCommandBuffer = GetSubmittedTestCommandBuffer(explicitDevice->GetTestQueue());
    ASSERT_NE(submittedCommandBuffer, nullptr);
    EXPECT_EQ(submittedCommandBuffer->beginRenderPassCalls, 1u);
    EXPECT_EQ(submittedCommandBuffer->endRenderPassCalls, 1u);
    EXPECT_EQ(submittedCommandBuffer->drawIndexedCalls, 1u);
    EXPECT_EQ(submittedCommandBuffer->bindGraphicsPipelineCalls, 1u);
    EXPECT_EQ(submittedCommandBuffer->bindBindingSetCalls, 1u);
    ASSERT_EQ(submittedCommandBuffer->boundSetIndices.size(), 1u);
    EXPECT_EQ(submittedCommandBuffer->boundSetIndices[0], 0u);
    EXPECT_EQ(submittedCommandBuffer->bindVertexBufferCalls, 1u);
    EXPECT_EQ(submittedCommandBuffer->bindIndexBufferCalls, 1u);
    EXPECT_EQ(submittedCommandBuffer->pushConstantsCalls, 1u);
    EXPECT_EQ(submittedCommandBuffer->lastPushConstantsStageMask, NLS::Render::RHI::ShaderStageMask::Vertex);
    EXPECT_EQ(submittedCommandBuffer->lastPushConstantsOffset, 0u);
    EXPECT_EQ(submittedCommandBuffer->lastPushConstantsSize, 16u);
    EXPECT_GE(submittedCommandBuffer->setViewportCalls, 1u);
    EXPECT_EQ(explicitDevice->createBindingLayoutCalls, 1u);
    ASSERT_EQ(explicitDevice->lastBindingLayoutDesc.entries.size(), 2u);
    EXPECT_EQ(explicitDevice->lastBindingLayoutDesc.entries[0].type, NLS::Render::RHI::BindingType::Texture);
    EXPECT_EQ(explicitDevice->lastBindingLayoutDesc.entries[1].type, NLS::Render::RHI::BindingType::Sampler);
    EXPECT_EQ(explicitDevice->createSamplerCalls, 1u);
    EXPECT_EQ(explicitDevice->createPipelineLayoutCalls, 1u);
    EXPECT_EQ(explicitDevice->lastPipelineLayoutDesc.debugName, "RHIImGuiOverlayPipelineLayout");
    ASSERT_EQ(explicitDevice->lastPipelineLayoutDesc.pushConstants.size(), 1u);
    EXPECT_EQ(explicitDevice->lastPipelineLayoutDesc.pushConstants[0].stageMask, NLS::Render::RHI::ShaderStageMask::Vertex);
    EXPECT_EQ(explicitDevice->lastPipelineLayoutDesc.pushConstants[0].size, 16u);
    EXPECT_EQ(explicitDevice->createShaderModuleCalls, 2u);
    ASSERT_EQ(explicitDevice->shaderModuleDescs.size(), 2u);
    EXPECT_EQ(explicitDevice->shaderModuleDescs[0].stage, NLS::Render::RHI::ShaderStage::Vertex);
    EXPECT_EQ(explicitDevice->shaderModuleDescs[0].shaderToolchainFingerprint.find("DXIL|vs_6_0|VSMain|"), 0u);
    EXPECT_NE(explicitDevice->shaderModuleDescs[0].shaderToolchainFingerprint, "RHIImGuiOverlay:placeholder");
    EXPECT_EQ(explicitDevice->shaderModuleDescs[1].stage, NLS::Render::RHI::ShaderStage::Fragment);
    EXPECT_EQ(explicitDevice->shaderModuleDescs[1].shaderToolchainFingerprint.find("DXIL|ps_6_0|PSMain|"), 0u);
    EXPECT_NE(explicitDevice->shaderModuleDescs[1].shaderToolchainFingerprint, "RHIImGuiOverlay:placeholder");
    EXPECT_EQ(explicitDevice->createGraphicsPipelineCalls, 1u);
    EXPECT_EQ(explicitDevice->lastGraphicsPipelineDesc.debugName, "RHIImGuiOverlayPipeline");
    EXPECT_EQ(explicitDevice->lastGraphicsPipelineDesc.pipelineLayout, explicitDevice->createdPipelineLayout);
    ASSERT_EQ(explicitDevice->createdShaderModules.size(), 2u);
    EXPECT_EQ(explicitDevice->lastGraphicsPipelineDesc.vertexShader, explicitDevice->createdShaderModules[0]);
    EXPECT_EQ(explicitDevice->lastGraphicsPipelineDesc.fragmentShader, explicitDevice->createdShaderModules[1]);
    const auto barrierEvent = std::find(
        submittedCommandBuffer->events.begin(),
        submittedCommandBuffer->events.end(),
        "Barrier");
    const auto beginRenderPassEvent = std::find(
        submittedCommandBuffer->events.begin(),
        submittedCommandBuffer->events.end(),
        "BeginRenderPass");
    const auto drawIndexedEvent = std::find(
        submittedCommandBuffer->events.begin(),
        submittedCommandBuffer->events.end(),
        "DrawIndexed");
    const auto bindGraphicsPipelineEvent = std::find(
        submittedCommandBuffer->events.begin(),
        submittedCommandBuffer->events.end(),
        "BindGraphicsPipeline");
    const auto endRenderPassEvent = std::find(
        submittedCommandBuffer->events.begin(),
        submittedCommandBuffer->events.end(),
        "EndRenderPass");
    ASSERT_NE(barrierEvent, submittedCommandBuffer->events.end());
    ASSERT_NE(beginRenderPassEvent, submittedCommandBuffer->events.end());
    ASSERT_NE(bindGraphicsPipelineEvent, submittedCommandBuffer->events.end());
    ASSERT_NE(drawIndexedEvent, submittedCommandBuffer->events.end());
    ASSERT_NE(endRenderPassEvent, submittedCommandBuffer->events.end());
    EXPECT_LT(barrierEvent, beginRenderPassEvent);
    EXPECT_LT(beginRenderPassEvent, bindGraphicsPipelineEvent);
    EXPECT_LT(bindGraphicsPipelineEvent, drawIndexedEvent);
    EXPECT_LT(drawIndexedEvent, endRenderPassEvent);
    const auto vertexBufferIt = std::find_if(
        explicitDevice->createdBuffers.begin(),
        explicitDevice->createdBuffers.end(),
        [](const std::shared_ptr<TestBuffer>& buffer)
        {
            return buffer && NLS::Render::RHI::HasBufferUsage(
                buffer->GetDesc().usage,
                NLS::Render::RHI::BufferUsageFlags::Vertex);
        });
    const auto indexBufferIt = std::find_if(
        explicitDevice->createdBuffers.begin(),
        explicitDevice->createdBuffers.end(),
        [](const std::shared_ptr<TestBuffer>& buffer)
        {
            return buffer && NLS::Render::RHI::HasBufferUsage(
                buffer->GetDesc().usage,
                NLS::Render::RHI::BufferUsageFlags::Index);
        });
    ASSERT_NE(vertexBufferIt, explicitDevice->createdBuffers.end());
    ASSERT_NE(indexBufferIt, explicitDevice->createdBuffers.end());
    EXPECT_EQ((*vertexBufferIt)->updateCalls, 1u);
    EXPECT_EQ((*indexBufferIt)->updateCalls, 1u);
    ASSERT_EQ(submittedCommandBuffer->lastRenderPassDesc.colorAttachments.size(), 1u);
    EXPECT_EQ(submittedCommandBuffer->lastRenderPassDesc.colorAttachments[0].loadOp, NLS::Render::RHI::LoadOp::Load);
    EXPECT_EQ(submittedCommandBuffer->lastRenderPassDesc.renderArea.width, 128u);
    EXPECT_EQ(submittedCommandBuffer->lastRenderPassDesc.renderArea.height, 72u);
}

TEST(RenderFrameExecutionTests, SnapshotPackageCarriesPostSubmitTextureReadbackState)
{
    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 912u;

    auto state = std::make_shared<NLS::Render::Context::PostSubmitTextureReadbackState>();
    NLS::Render::Context::PostSubmitTextureReadbackRequest request;
    request.state = state;
    snapshot.postSubmitTextureReadbacks.push_back(std::move(request));

    {
        std::lock_guard lock(state->mutex);
        EXPECT_FALSE(state->carriedIntoRenderScenePackage);
        EXPECT_EQ(state->renderScenePackageFrameId, 0u);
    }

    const auto package = NLS::Render::Context::BuildRenderFramePackage(snapshot);
    ASSERT_EQ(package.postSubmitTextureReadbacks.size(), 1u);
    {
        std::lock_guard lock(state->mutex);
        EXPECT_TRUE(state->carriedIntoRenderScenePackage);
        EXPECT_EQ(state->renderScenePackageFrameId, snapshot.frameId);
    }
}

TEST(RenderFrameExecutionTests, ThreadedVisibleFrameRecordsPreparedDrawBindingsAndMeshByDefault)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

TEST(RenderFrameExecutionTests, RendererFrameReservationIsRejectedWhileFrameOwnsFrameContext)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 301u;
    snapshot.targetsSwapchain = true;
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, snapshot,
        [snapshot](size_t) { return NLS::Render::Context::BuildRenderFramePackage(snapshot); }));

    EXPECT_FALSE(NLS::Render::Context::DriverRendererAccess::ReserveReusableFrameContextSlotIndex(driver).has_value());
}

TEST(RenderFrameExecutionTests, UiOnlyFrameQueuesBehindOffscreenFrameWithoutCallerSideResourceReuse)
{
    using namespace NLS::Render::Context;
    ImGuiContextGuard imguiContext;
    ScopedOverlayShaderManager shaderManagerScope;
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 2u;
    Driver driver(settings);
    const ScopedDriverService driverService(driver);
    DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto device = std::make_shared<TestExplicitDevice>();
    device->MutableCapabilities().SetFeature(
        NLS::Render::RHI::RHIDeviceFeature::UIOverlayFrameGraph, true, "test overlay support");
    auto swapchain = std::make_shared<TestSwapchain>();
    swapchain->desc.width = 128u;
    swapchain->desc.height = 72u;
    DriverTestAccess::SetExplicitDevice(driver, device);
    DriverTestAccess::SetExplicitSwapchain(driver, swapchain);

    RenderFramePacket offscreen;
    offscreen.frameId = 303u;
    offscreen.targetsSwapchain = false;
    size_t offscreenSlot = 99u;
    ASSERT_TRUE(DriverRendererAccess::TryEnqueueRenderFrame(driver, offscreen,
        [offscreen](size_t) { return BuildRenderFramePackage(offscreen); }, &offscreenSlot));
    auto uiSnapshot = MakeVisibleUiSnapshot(304u, 128.0f, 72.0f);
    auto result = NLS::Tests::ObserveNextRenderSceneForTest(driver, uiSnapshot->frameId);
    DriverUIAccess::PublishUiDrawDataSnapshot(driver, uiSnapshot);
    size_t uiSlot = 99u;
    ASSERT_TRUE(DriverUIAccess::PublishUiOnlyFrame(driver, 128u, 72u, &uiSlot, nullptr, false));
    EXPECT_NE(uiSlot, offscreenSlot);
    EXPECT_EQ(DriverRendererAccess::GetActiveExplicitCommandBuffer(driver), nullptr);
    EXPECT_EQ(swapchain->acquireCalls, 0u);
    EXPECT_EQ(device->GetTestQueue()->submitCalls, 0u);
    EXPECT_EQ(device->GetTestQueue()->presentCalls, 0u);
    const auto uiPool = std::dynamic_pointer_cast<TestCommandPool>(
        DriverTestAccess::PeekFrameContext(driver, uiSlot)->commandPool);
    ASSERT_NE(uiPool, nullptr);
    EXPECT_EQ(uiPool->resetCalls, 0u);

    DriverTestAccess::DrainThreadedRendering(driver);
    EXPECT_EQ(swapchain->acquireCalls, 1u);
    EXPECT_EQ(device->GetTestQueue()->submitCalls, 1u);
    EXPECT_EQ(device->GetTestQueue()->presentCalls, 1u);
    EXPECT_EQ(DriverRendererAccess::GetThreadedFrameTelemetry(driver).inFlightFrameCount, 0u);
    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto package = result.get();
    EXPECT_TRUE(package.hasUIOverlayPass);
    EXPECT_EQ(package.uiDrawDataSnapshot, uiSnapshot);
}

TEST(RenderFrameExecutionTests, OffscreenFramesLeaveCapacityForSwapchainPresentationUnderBackpressure)
{
    using namespace NLS::Render::Context;
    ImGuiContextGuard imguiContext;
    ScopedOverlayShaderManager shaderManagerScope;
    for (const uint32_t resourceCount : { 3u, 16u })
    {
        SCOPED_TRACE(resourceCount);
        NLS::Render::Settings::DriverSettings settings;
        settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
        settings.enableExplicitRHI = false;
        settings.framesInFlight = resourceCount;
        settings.cpuFrameLatencyLimit = 3u;
        Driver driver(settings);
        const ScopedDriverService driverService(driver);
        DriverTestAccess::PauseThreadedRenderingWorkers(driver);
        auto device = std::make_shared<TestExplicitDevice>();
        device->MutableCapabilities().SetFeature(
            NLS::Render::RHI::RHIDeviceFeature::UIOverlayFrameGraph, true, "test overlay support");
        auto swapchain = std::make_shared<TestSwapchain>();
        swapchain->desc.width = 128u;
        swapchain->desc.height = 72u;
        DriverTestAccess::SetExplicitDevice(driver, device);
        DriverTestAccess::SetExplicitSwapchain(driver, swapchain);
        ASSERT_EQ(DriverRendererAccess::GetFrameContextSlotCount(driver), resourceCount);

        for (size_t index = 0u; index < 2u; ++index)
        {
            RenderFramePacket offscreen;
            offscreen.frameId = 305u + index;
            offscreen.targetsSwapchain = false;
            ASSERT_TRUE(DriverRendererAccess::TryEnqueueRenderFrame(driver, offscreen,
                [offscreen](size_t) { return BuildRenderFramePackage(offscreen); }));
        }
        RenderFramePacket nextOffscreen;
        nextOffscreen.frameId = 307u;
        nextOffscreen.targetsSwapchain = false;
        EXPECT_FALSE(DriverRendererAccess::TryEnqueueRenderFrame(driver, nextOffscreen,
            [nextOffscreen](size_t) { return BuildRenderFramePackage(nextOffscreen); }));
        EXPECT_EQ(DriverRendererAccess::GetFrameLatencyDebugStats(driver).openFrameCount, 2u)
            << "A producer waiting for a scene slot must leave the presentation CPU budget available.";

        auto snapshot = MakeVisibleUiSnapshot(308u, 128.0f, 72.0f);
        DriverUIAccess::PublishUiDrawDataSnapshot(driver, snapshot);
        ASSERT_TRUE(DriverUIAccess::PublishUiOnlyFrame(driver, 128u, 72u, nullptr, nullptr, false))
            << "Present must enter the FIFO even while scene producers saturate their slots.";
        EXPECT_EQ(DriverRendererAccess::GetFrameLatencyDebugStats(driver).openFrameCount, 3u);
        DriverTestAccess::DrainThreadedRendering(driver);
        EXPECT_EQ(device->GetTestQueue()->presentCalls, 1u);
    }
}

TEST(RenderFrameExecutionTests, UiOnlyFrameKeepsPendingSnapshotWhenTheFrameBudgetIsOccupied)
{
    using namespace NLS::Render::Context;
    ImGuiContextGuard imguiContext;
    ScopedOverlayShaderManager shaderManagerScope;
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;
    Driver driver(settings);
    const ScopedDriverService driverService(driver);
    DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto device = std::make_shared<TestExplicitDevice>();
    device->MutableCapabilities().SetFeature(
        NLS::Render::RHI::RHIDeviceFeature::UIOverlayFrameGraph, true, "test overlay support");
    auto swapchain = std::make_shared<TestSwapchain>();
    swapchain->desc.width = 128u;
    swapchain->desc.height = 72u;
    DriverTestAccess::SetExplicitDevice(driver, device);
    DriverTestAccess::SetExplicitSwapchain(driver, swapchain);

    RenderFramePacket offscreen;
    offscreen.frameId = 303u;
    offscreen.targetsSwapchain = false;
    ASSERT_TRUE(DriverRendererAccess::TryEnqueueRenderFrame(driver, offscreen,
        [offscreen](size_t) { return BuildRenderFramePackage(offscreen); }));
    auto uiSnapshot = MakeVisibleUiSnapshot(304u, 128.0f, 72.0f);
    DriverUIAccess::PublishUiDrawDataSnapshot(driver, uiSnapshot);
    EXPECT_FALSE(DriverUIAccess::PublishUiOnlyFrame(driver, 128u, 72u, nullptr, nullptr, false));
    EXPECT_EQ(DriverRendererAccess::GetActiveExplicitCommandBuffer(driver), nullptr);
    EXPECT_EQ(swapchain->acquireCalls, 0u);
    EXPECT_EQ(device->GetTestQueue()->submitCalls, 0u);
    uint64_t generation = 0u;
    auto pending = DriverUIAccess::ConsumePendingUiDrawDataSnapshot(driver, &generation);
    EXPECT_EQ(pending, uiSnapshot);
    ASSERT_TRUE(DriverUIAccess::RestoreConsumedUiDrawDataSnapshotIfUnchanged(driver, pending, generation));

    DriverTestAccess::DrainThreadedRendering(driver);
    ASSERT_TRUE(DriverUIAccess::PublishUiOnlyFrame(driver, 128u, 72u, nullptr, nullptr, false));
    DriverTestAccess::DrainThreadedRendering(driver);
    EXPECT_EQ(device->GetTestQueue()->presentCalls, 1u);
    EXPECT_EQ(DriverUIAccess::ConsumePendingUiDrawDataSnapshot(driver), nullptr);
}

TEST(RenderFrameExecutionTests, PendingUiFrameGetsFreedCapacityBeforeTheNextOffscreenProducer)
{
    using namespace NLS::Render::Context;
    ImGuiContextGuard imguiContext;
    ScopedOverlayShaderManager shaderManagerScope;
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;
    Driver driver(settings);
    const ScopedDriverService driverService(driver);
    DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto device = std::make_shared<TestExplicitDevice>();
    device->MutableCapabilities().SetFeature(
        NLS::Render::RHI::RHIDeviceFeature::UIOverlayFrameGraph, true, "test overlay support");
    auto swapchain = std::make_shared<TestSwapchain>();
    swapchain->desc.width = 128u;
    swapchain->desc.height = 72u;
    DriverTestAccess::SetExplicitDevice(driver, device);
    DriverTestAccess::SetExplicitSwapchain(driver, swapchain);
    ConfigureUiOverlayFrameContext(driver, 0u);
    const auto gpuFence = std::dynamic_pointer_cast<TestFence>(
        DriverTestAccess::PeekFrameContext(driver, 0u)->frameFence);
    ASSERT_NE(gpuFence, nullptr);
    std::vector<uint64_t> submittedFrames;
    DriverTestAccess::SetRhiFrameObserver(driver,
        [&submittedFrames](const auto& frame) { submittedFrames.push_back(frame.frameId); });

    for (uint64_t iteration = 0u; iteration < 8u; ++iteration)
    {
        SCOPED_TRACE(iteration);
        RenderFramePacket offscreen;
        offscreen.frameId = 940u + iteration * 2u;
        offscreen.targetsSwapchain = false;
        ASSERT_TRUE(DriverRendererAccess::TryEnqueueRenderFrame(driver, offscreen,
            [offscreen](size_t) { return BuildRenderFramePackage(offscreen); }));
        auto uiSnapshot = MakeVisibleUiSnapshot(offscreen.frameId + 1u, 128.0f, 72.0f);
        uiSnapshot->drawLists.front().commands.front().clipRect[2] -= static_cast<float>(iteration);
        DriverUIAccess::PublishUiDrawDataSnapshot(driver, uiSnapshot);
        EXPECT_FALSE(DriverUIAccess::PublishUiOnlyFrame(driver, 128u, 72u, nullptr, nullptr, false));
        DriverTestAccess::DrainThreadedRendering(driver);

        const bool reservedProducer = RenderThreadCoordinator::BeginRendererFrame(driver, false, false);
        EXPECT_FALSE(reservedProducer)
            << "The pending UI consumer must get the freed capacity before another offscreen producer.";
        if (reservedProducer)
            RenderThreadCoordinator::EndRendererFrame(driver);
        EXPECT_FALSE(DriverRendererAccess::GetReservedFrameContextSlotIndex(driver).has_value());
        EXPECT_EQ(DriverUIAccess::ConsumePendingUiDrawDataSnapshot(driver), nullptr);
        DriverTestAccess::DrainThreadedRendering(driver);
        EXPECT_EQ(device->GetTestQueue()->presentCalls, iteration + 1u);
        ASSERT_TRUE(gpuFence->submitSignalRequested);
        ASSERT_TRUE(gpuFence->Wait());
        (void)DriverTestAccess::GetImplForTesting(driver)->frameLatency->PollCompletedFrames();
        EXPECT_EQ(DriverRendererAccess::GetFrameLatencyDebugStats(driver).openFrameCount, 0u);
    }

    EXPECT_EQ(swapchain->acquireCalls, 8u);
    std::vector<uint64_t> expectedFrames;
    for (uint64_t frame = 940u; frame < 956u; ++frame)
        expectedFrames.push_back(frame);
    EXPECT_EQ(submittedFrames, expectedFrames);
}

TEST(RenderFrameExecutionTests, OffscreenProducerDoesNotPublishUnchangedUiOrConsumeItsReservedLease)
{
    using namespace NLS::Render::Context;
    ImGuiContextGuard imguiContext;
    ScopedOverlayShaderManager shaderManagerScope;
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;
    Driver driver(settings);
    const ScopedDriverService driverService(driver);
    DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto device = std::make_shared<TestExplicitDevice>();
    device->MutableCapabilities().SetFeature(
        NLS::Render::RHI::RHIDeviceFeature::UIOverlayFrameGraph, true, "test overlay support");
    auto swapchain = std::make_shared<TestSwapchain>();
    swapchain->desc.width = 128u;
    swapchain->desc.height = 72u;
    DriverTestAccess::SetExplicitDevice(driver, device);
    DriverTestAccess::SetExplicitSwapchain(driver, swapchain);
    ConfigureUiOverlayFrameContext(driver, 0u);
    const auto gpuFence = std::dynamic_pointer_cast<TestFence>(
        DriverTestAccess::PeekFrameContext(driver, 0u)->frameFence);
    ASSERT_NE(gpuFence, nullptr);

    DriverUIAccess::PublishUiDrawDataSnapshot(driver, MakeVisibleUiSnapshot(960u, 128.0f, 72.0f));
    ASSERT_TRUE(DriverUIAccess::PublishUiOnlyFrame(driver, 128u, 72u, nullptr, nullptr, false));
    DriverTestAccess::DrainThreadedRendering(driver);
    ASSERT_TRUE(gpuFence->submitSignalRequested);
    ASSERT_TRUE(gpuFence->Wait());
    auto unchanged = MakeVisibleUiSnapshot(961u, 128.0f, 72.0f);
    DriverUIAccess::PublishUiDrawDataSnapshot(driver, unchanged);
    ASSERT_TRUE(RenderThreadCoordinator::BeginRendererFrame(driver, false, false));
    const auto producerSlot = DriverRendererAccess::GetReservedFrameContextSlotIndex(driver);
    ASSERT_TRUE(producerSlot.has_value());
    EXPECT_EQ(DriverUIAccess::ConsumePendingUiDrawDataSnapshot(driver), unchanged);

    auto changed = MakeVisibleUiSnapshot(962u, 128.0f, 72.0f);
    changed->drawLists.front().commands.front().clipRect[2] -= 1.0f;
    DriverUIAccess::PublishUiDrawDataSnapshot(driver, changed);
    ASSERT_TRUE(RenderThreadCoordinator::BeginRendererFrame(driver, false, false));
    EXPECT_EQ(DriverRendererAccess::GetReservedFrameContextSlotIndex(driver), producerSlot);
    EXPECT_EQ(DriverUIAccess::ConsumePendingUiDrawDataSnapshot(driver), changed);
    EXPECT_EQ(DriverRendererAccess::GetThreadedFrameTelemetry(driver).publishedFrameCount, 1u);
    EXPECT_EQ(device->GetTestQueue()->presentCalls, 1u);
    RenderThreadCoordinator::EndRendererFrame(driver);
}

TEST(RenderFrameExecutionTests, ChangedUiFramesQueueBeforeEarlierUiFramesRetire)
{
    using namespace NLS::Render::Context;
    ImGuiContextGuard imguiContext;
    ScopedOverlayShaderManager shaderManagerScope;
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 3u;
    Driver driver(settings);
    const ScopedDriverService driverService(driver);
    DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto device = std::make_shared<TestExplicitDevice>();
    device->MutableCapabilities().SetFeature(
        NLS::Render::RHI::RHIDeviceFeature::UIOverlayFrameGraph, true, "test overlay support");
    auto swapchain = std::make_shared<TestSwapchain>();
    swapchain->desc.width = 128u;
    swapchain->desc.height = 72u;
    DriverTestAccess::SetExplicitDevice(driver, device);
    DriverTestAccess::SetExplicitSwapchain(driver, swapchain);
    for (size_t slot = 0u; slot < 3u; ++slot)
        ConfigureUiOverlayFrameContext(driver, slot);
    std::vector<uint64_t> retiredFrames;
    DriverTestAccess::SetRhiFrameObserver(driver,
        [&retiredFrames](const auto& frame) { retiredFrames.push_back(frame.frameId); });

    for (uint64_t index = 0u; index < 3u; ++index)
    {
        auto snapshot = MakeVisibleUiSnapshot(970u + index, 128.0f, 72.0f);
        snapshot->drawLists.front().commands.front().clipRect[2] -= static_cast<float>(index);
        DriverUIAccess::PublishUiDrawDataSnapshot(driver, snapshot);
        if (index == 1u)
        {
            EXPECT_TRUE(RenderThreadCoordinator::BeginRendererFrame(driver, false, false));
            RenderThreadCoordinator::EndRendererFrame(driver);
        }
        else
            DriverUIAccess::PresentSwapchain(driver);
        EXPECT_EQ(DriverUIAccess::ConsumePendingUiDrawDataSnapshot(driver), nullptr)
            << "An earlier UI frame must not prevent a newer update from entering the FIFO.";
        EXPECT_EQ(DriverRendererAccess::GetThreadedFrameTelemetry(driver).publishedFrameCount, index + 1u);
    }
    EXPECT_EQ(device->GetTestQueue()->presentCalls, 0u);
    DriverTestAccess::DrainThreadedRendering(driver);
    EXPECT_EQ(device->GetTestQueue()->presentCalls, 3u);
    EXPECT_EQ(swapchain->acquireCalls, 3u);
    EXPECT_EQ(retiredFrames, std::vector<uint64_t>({970u, 971u, 972u}));
}

TEST(RenderFrameExecutionTests, IdenticalPendingUiContentDoesNotUseAnotherResourceLease)
{
    using namespace NLS::Render::Context;
    ImGuiContextGuard imguiContext;
    ScopedOverlayShaderManager shaderManagerScope;
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 3u;
    Driver driver(settings);
    const ScopedDriverService driverService(driver);
    DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto device = std::make_shared<TestExplicitDevice>();
    device->MutableCapabilities().SetFeature(
        NLS::Render::RHI::RHIDeviceFeature::UIOverlayFrameGraph, true, "test overlay support");
    auto swapchain = std::make_shared<TestSwapchain>();
    swapchain->desc.width = 128u;
    swapchain->desc.height = 72u;
    DriverTestAccess::SetExplicitDevice(driver, device);
    DriverTestAccess::SetExplicitSwapchain(driver, swapchain);

    DriverUIAccess::PublishUiDrawDataSnapshot(driver, MakeVisibleUiSnapshot(980u, 128.0f, 72.0f));
    ASSERT_TRUE(DriverUIAccess::PublishUiOnlyFrame(driver, 128u, 72u, nullptr, nullptr, false));
    auto unchanged = MakeVisibleUiSnapshot(981u, 128.0f, 72.0f);
    DriverUIAccess::PublishUiDrawDataSnapshot(driver, unchanged);
    EXPECT_FALSE(DriverUIAccess::PublishUiOnlyFrame(driver, 128u, 72u, nullptr, nullptr, false));
    EXPECT_EQ(DriverRendererAccess::GetThreadedFrameTelemetry(driver).publishedFrameCount, 1u);
    EXPECT_EQ(DriverRendererAccess::GetFrameLatencyDebugStats(driver).openFrameCount, 1u);
    EXPECT_EQ(DriverUIAccess::ConsumePendingUiDrawDataSnapshot(driver), unchanged);
}

TEST(RenderFrameExecutionTests, ReturningToPresentedUiContentQueuesBehindADifferentPendingUpdate)
{
    using namespace NLS::Render::Context;
    ImGuiContextGuard imguiContext;
    ScopedOverlayShaderManager shaderManagerScope;
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 3u;
    Driver driver(settings);
    const ScopedDriverService driverService(driver);
    DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto device = std::make_shared<TestExplicitDevice>();
    device->MutableCapabilities().SetFeature(
        NLS::Render::RHI::RHIDeviceFeature::UIOverlayFrameGraph, true, "test overlay support");
    auto swapchain = std::make_shared<TestSwapchain>();
    swapchain->desc.width = 128u;
    swapchain->desc.height = 72u;
    DriverTestAccess::SetExplicitDevice(driver, device);
    DriverTestAccess::SetExplicitSwapchain(driver, swapchain);
    for (size_t slot = 0u; slot < 3u; ++slot)
        ConfigureUiOverlayFrameContext(driver, slot);

    DriverUIAccess::PublishUiDrawDataSnapshot(driver, MakeVisibleUiSnapshot(983u, 128.0f, 72.0f));
    ASSERT_TRUE(DriverUIAccess::PublishUiOnlyFrame(driver, 128u, 72u, nullptr, nullptr, false));
    DriverTestAccess::DrainThreadedRendering(driver);
    ASSERT_EQ(device->GetTestQueue()->presentCalls, 1u);
    auto changed = MakeVisibleUiSnapshot(984u, 128.0f, 72.0f);
    changed->drawLists.front().commands.front().clipRect[2] -= 1.0f;
    DriverUIAccess::PublishUiDrawDataSnapshot(driver, changed);
    ASSERT_TRUE(DriverUIAccess::PublishUiOnlyFrame(driver, 128u, 72u, nullptr, nullptr, false));
    DriverUIAccess::PublishUiDrawDataSnapshot(driver, MakeVisibleUiSnapshot(985u, 128.0f, 72.0f));
    EXPECT_TRUE(DriverUIAccess::PublishUiOnlyFrame(driver, 128u, 72u, nullptr, nullptr, false))
        << "Compare against the queued update: presenting old content must not leave a newer update visible.";
    EXPECT_EQ(DriverRendererAccess::GetThreadedFrameTelemetry(driver).publishedFrameCount, 3u);
    DriverTestAccess::DrainThreadedRendering(driver);
    EXPECT_EQ(device->GetTestQueue()->presentCalls, 3u);
}

TEST(RenderFrameExecutionTests, CancelledSceneUiConsumerDoesNotBlockLaterUiPublication)
{
    using namespace NLS::Render::Context;
    ImGuiContextGuard imguiContext;
    ScopedOverlayShaderManager shaderManagerScope;
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;
    Driver driver(settings);
    const ScopedDriverService driverService(driver);
    DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto device = std::make_shared<TestExplicitDevice>();
    device->MutableCapabilities().SetFeature(
        NLS::Render::RHI::RHIDeviceFeature::UIOverlayFrameGraph, true, "test overlay support");
    auto swapchain = std::make_shared<TestSwapchain>();
    swapchain->desc.width = 128u;
    swapchain->desc.height = 72u;
    DriverTestAccess::SetExplicitDevice(driver, device);
    DriverTestAccess::SetExplicitSwapchain(driver, swapchain);
    auto* impl = DriverTestAccess::GetImplForTesting(driver);
    RenderFramePacket scene;
    scene.frameId = 990u;
    scene.renderWidth = 128u;
    scene.renderHeight = 72u;
    ASSERT_TRUE(DriverRendererAccess::TryEnqueueRenderFrame(driver, scene,
        [scene](size_t) { return BuildRenderFramePackage(scene); }));
    EXPECT_EQ(impl->pendingUiSnapshotConsumerCommands.load(), 1u);
    EXPECT_FALSE(DriverRendererAccess::TryEnqueueRenderFrame(driver, scene,
        [scene](size_t) { return BuildRenderFramePackage(scene); }));
    EXPECT_EQ(impl->pendingUiSnapshotConsumerCommands.load(), 1u);
    auto snapshot = MakeVisibleUiSnapshot(991u, 128.0f, 72.0f);
    DriverUIAccess::PublishUiDrawDataSnapshot(driver, snapshot);
    DriverUIAccess::PresentSwapchain(driver);
    EXPECT_EQ(DriverRendererAccess::GetThreadedFrameTelemetry(driver).publishedFrameCount, 1u);

    RenderCommandDispatcher::GetMainPipe().Clear();
    EXPECT_EQ(impl->pendingUiSnapshotConsumerCommands.load(), 0u);
    EXPECT_EQ(DriverRendererAccess::GetFrameLatencyDebugStats(driver).openFrameCount, 0u);
    DriverUIAccess::PresentSwapchain(driver);
    EXPECT_EQ(DriverRendererAccess::GetThreadedFrameTelemetry(driver).publishedFrameCount, 2u);
    EXPECT_EQ(DriverUIAccess::ConsumePendingUiDrawDataSnapshot(driver), nullptr);
}

TEST(RenderFrameExecutionTests, RenderTickServicesCompletedFencesWhileCommandsKeepArriving)
{
    using namespace NLS::Render::Context;
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;
    Driver driver(settings);
    DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto* impl = DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl->renderingTick, nullptr);
    const auto latencyFrame = impl->frameLatency->ReserveFrame(std::chrono::nanoseconds::zero());
    ASSERT_TRUE(latencyFrame.has_value());
    RenderCommandFence fence;
    fence.BeginFence(RenderCommandSyncDepth::Render);
    ASSERT_TRUE(impl->frameLatency->ArmFrameFence(*latencyFrame, std::move(fence)));

    std::vector<size_t> executedCommands;
    std::function<void()> producer;
    producer = [&]
    {
        executedCommands.push_back(executedCommands.size());
        if (executedCommands.size() < 512u)
            EnqueueRenderCommand<struct ContinuousRenderProducer>(producer);
    };
    EnqueueRenderCommand<struct ContinuousRenderProducer>(producer);
    {
        ScopedRenderThreadForTesting renderThread;
        EXPECT_TRUE(impl->renderingTick->TickRenderThread());
        EXPECT_FALSE(RenderCommandDispatcher::IsEmpty());
        EXPECT_GT(executedCommands.size(), 0u);
        EXPECT_LT(executedCommands.size(), 512u)
            << "A continuously replenished pipe must yield to frame retirement and resource maintenance.";
        EXPECT_EQ(impl->frameLatency->GetInFlightFrameCount(), 0u);
        (void)RenderCommandDispatcher::Dispatch();
    }
    ASSERT_EQ(executedCommands.size(), 512u);
    for (size_t index = 0u; index < executedCommands.size(); ++index)
        EXPECT_EQ(executedCommands[index], index);
}




TEST(RenderFrameExecutionTests, UiOnlyFrameRejectsUnrecordableCommandsWithoutConsumingPendingSnapshot)
{
    using namespace NLS::Render::Context;
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    Driver driver(settings);
    DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto device = std::make_shared<TestExplicitDevice>();
    device->MutableCapabilities().SetFeature(
        NLS::Render::RHI::RHIDeviceFeature::UIOverlayFrameGraph, true, "test overlay support");
    DriverTestAccess::SetExplicitDevice(driver, device);
    DriverTestAccess::SetExplicitSwapchain(driver, std::make_shared<TestSwapchain>());
    for (uint32_t rejectedKind = 0u; rejectedKind < 3u; ++rejectedKind)
    {
        auto snapshot = MakeVisibleUiSnapshot(310u + rejectedKind, 128.0f, 72.0f);
        auto& command = snapshot->drawLists.front().commands.front();
        if (rejectedKind == 0u)
            command.elementCount = 0u;
        else if (rejectedKind == 1u)
            command.callbackKind = NLS::Render::UI::UiDrawCallbackKind::Unsupported;
        else
            command.hasUnsupportedTextureId = true;
        DriverUIAccess::PublishUiDrawDataSnapshot(driver, snapshot);
        EXPECT_FALSE(DriverUIAccess::PublishUiOnlyFrame(driver, 128u, 72u, nullptr, nullptr, false));
        EXPECT_EQ(DriverRendererAccess::GetThreadedFrameTelemetry(driver).publishedFrameCount, 0u);
        EXPECT_EQ(DriverUIAccess::ConsumePendingUiDrawDataSnapshot(driver), snapshot);
        EXPECT_EQ(device->GetTestQueue()->submitCalls, 0u);
        EXPECT_EQ(device->GetTestQueue()->presentCalls, 0u);
    }
}

TEST(RenderFrameExecutionTests, RendererFrameReservationSkipsWhileFrameOwnsFrameContext)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 302u;
    snapshot.targetsSwapchain = true;
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, snapshot,
        [snapshot](size_t) { return NLS::Render::Context::BuildRenderFramePackage(snapshot); }));

    EXPECT_FALSE(NLS::Render::Context::RenderThreadCoordinator::BeginRendererFrame(driver));

    EXPECT_EQ(frameFence->waitCalls, 0u);
    EXPECT_EQ(frameFence->resetCalls, 0u);
    EXPECT_EQ(commandPool->resetCalls, 0u);
    EXPECT_EQ(commandBuffer->resetCalls, 0u);
    EXPECT_EQ(commandBuffer->beginCalls, 0u);
    EXPECT_EQ(NLS::Render::Context::DriverRendererAccess::GetActiveExplicitCommandBuffer(driver), nullptr);
}

TEST(RenderFrameExecutionTests, CompositeRendererReservesCapacityBeforeOpeningAnotherPausedFrame)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

    auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    const ScopedDriverService driverService(*driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(*driver);
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(*driver, std::make_shared<TestExplicitDevice>());
    SnapshotPublishingRenderer renderer(*driver);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 64u;
    frameDescriptor.renderHeight = 64u;
    frameDescriptor.camera = &camera;

    renderer.BeginFrame(frameDescriptor);
    renderer.EndFrame();
    ASSERT_TRUE(renderer.WasLastThreadedFramePublished());
    EXPECT_EQ(NLS::Render::Context::DriverRendererAccess::GetThreadedFrameTelemetry(*driver).publishedFrameCount, 1u);
    renderer.BeginFrame(frameDescriptor);
    EXPECT_FALSE(renderer.IsFrameActive());
    renderer.EndFrame();

    EXPECT_EQ(NLS::Render::Context::DriverRendererAccess::GetThreadedFrameTelemetry(*driver).publishedFrameCount, 1u);
}

TEST(RenderFrameExecutionTests, FrameSnapshotCapturesFrameDescriptorCameraAndTargetState)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
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

TEST(RenderFrameExecutionTests, FrameSnapshotMarksFramebufferTargetsAsOffscreenOnly)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
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

TEST(RenderFrameExecutionTests, FrameSnapshotCountsDirectExternalOutputTextures)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
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

TEST(RenderFrameExecutionTests, SceneFramePacketOwnsItsViewAcrossLiveSceneAndCameraChanges)
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
    ASSERT_NE(snapshot->view, nullptr);
    EXPECT_NE(snapshot->view->descriptor.camera, &camera);
    camera.SetClearColor({ 1.0f, 0.0f, 0.0f });
    scene.CreateGameObject("LaterActor");
    EXPECT_EQ(snapshot->view->descriptor.renderWidth, 128u);
    EXPECT_EQ(snapshot->view->descriptor.renderHeight, 96u);
    EXPECT_FLOAT_EQ(snapshot->view->descriptor.camera->GetClearColor().x, snapshot->clearColor.x);
}

TEST(RenderFrameExecutionTests, QueuedFrameKeepsItsPublicationIdentityAfterSlotReuse)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto device = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, device);
    NLS::Render::Context::DriverTestAccess::SetExplicitSwapchain(driver, std::make_shared<TestSwapchain>());
    VisibleSnapshotPublishingRenderer renderer(driver);

    std::vector<uint64_t> frameIds;
    NLS::Render::Context::DriverTestAccess::SetRenderSceneObserver(driver,
        [&frameIds](const auto& packet, const auto& package)
        {
            EXPECT_EQ(package.frameId, packet.frameId);
            frameIds.push_back(packet.frameId);
        });

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 64u;
    frameDescriptor.renderHeight = 64u;
    frameDescriptor.camera = &camera;

    renderer.BeginFrame(frameDescriptor);
    renderer.EndFrame();
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryWaitForSubmittedGpuWork(driver));
    ASSERT_EQ(frameIds.size(), 1u);
    const auto* ring = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver)->frameResourceSlots.get();
    ASSERT_NE(ring, nullptr);
    const auto firstLeaseGeneration = ring->PeekSlot(0u)->leaseGeneration;

    renderer.BeginFrame(frameDescriptor);
    renderer.EndFrame();
    EXPECT_GT(ring->PeekSlot(0u)->leaseGeneration, firstLeaseGeneration);

    const auto telemetry = NLS::Render::Context::DriverRendererAccess::GetThreadedFrameTelemetry(driver);
    EXPECT_EQ(telemetry.publishedFrameCount, 2u);
    EXPECT_EQ(telemetry.blockedPublishCount, 0u);
    EXPECT_EQ(telemetry.inFlightFrameCount, 1u);
    EXPECT_EQ(telemetry.publishState, NLS::Render::Data::FramePublishState::Open);
    EXPECT_TRUE(NLS::Render::Context::DriverRendererAccess::TryDrainThreadedRendering(driver, false));
    ASSERT_EQ(frameIds.size(), 2u);
    EXPECT_EQ(frameIds[0], 1u);
    EXPECT_EQ(frameIds[1], 2u);
    EXPECT_EQ(NLS::Render::Context::DriverRendererAccess::GetThreadedFrameTelemetry(driver).latestRetiredFrameId, 2u);
    NLS::Render::Context::DriverTestAccess::SetRenderSceneObserver(driver, {});
}

TEST(RenderFrameExecutionTests, RenderSceneBuildResultIsIndependentOfFramePacketAndLiveScene)
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

    NLS::Render::Context::RenderSceneBuildData sceneResult;
    sceneResult.sceneGameObjectCount = 3u;
    sceneResult.sceneLightCount = 2u;
    const auto package = renderer.CaptureRenderScenePackage(snapshot.value(), std::move(sceneResult));

    EXPECT_EQ(package.frameId, snapshot->frameId);
    EXPECT_EQ(package.sceneGameObjectCount, 3u);
    EXPECT_TRUE(package.hasLightingData);
    EXPECT_EQ(package.visibleDrawCount, 0u);
    EXPECT_FALSE(package.hasVisibleDraws);
    EXPECT_EQ(package.opaqueDrawCount, 0u);
    EXPECT_EQ(package.transparentDrawCount, 0u);
    EXPECT_EQ(package.skyboxDrawCount, 0u);
    EXPECT_EQ(package.passPlanCount, 0u);
    EXPECT_TRUE(package.frameDataReady);
    EXPECT_TRUE(package.objectDataReady);
}

TEST(RenderFrameExecutionTests, RenderScenePackageBuildsTypedPassPlanFromSnapshotDrawSets)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    NLS::Core::ServiceLocator::Provide(*driver);
    SceneSnapshotRenderer renderer(*driver);

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 9u;
    snapshot.renderWidth = 256u;
    snapshot.renderHeight = 144u;
    NLS::Render::Context::RenderSceneBuildData scene;
    scene.visibleOpaqueDrawCount = 3u;
    scene.visibleDecalDrawCount = 5u;
    scene.visibleTransparentDrawCount = 2u;
    scene.visibleSkyboxDrawCount = 1u;
    scene.visibleHelperDrawCount = 4u;
    snapshot.clearColor = { 0.1f, 0.2f, 0.3f, 1.0f };

    const auto package = renderer.CaptureRenderScenePackage(snapshot, std::move(scene));

    EXPECT_EQ(package.frameId, 9u);
    EXPECT_EQ(package.opaqueDrawCount, 3u);
    EXPECT_EQ(package.decalDrawCount, 5u);
    EXPECT_EQ(package.transparentDrawCount, 2u);
    EXPECT_EQ(package.skyboxDrawCount, 1u);
    EXPECT_EQ(package.helperDrawCount, 4u);
    EXPECT_EQ(package.visibleDrawCount, 15u);
    EXPECT_TRUE(package.hasOpaquePass);
    EXPECT_TRUE(package.hasDecalPass);
    EXPECT_TRUE(package.hasTransparentPass);
    EXPECT_TRUE(package.hasSkyboxPass);
    EXPECT_TRUE(package.hasHelperPass);
    EXPECT_EQ(package.passPlanCount, 5u);
    EXPECT_EQ(package.drawCommandCount, 15u);
    EXPECT_EQ(package.materialBatchCount, 15u);
    EXPECT_EQ(package.renderTargetUseCount, 1u);
    EXPECT_TRUE(package.containsCommandInputs);
    ASSERT_EQ(package.passCommandInputs.size(), 5u);
    EXPECT_EQ(package.passCommandInputs[0].kind, NLS::Render::Context::RenderPassCommandKind::Opaque);
    EXPECT_EQ(package.passCommandInputs[1].kind, NLS::Render::Context::RenderPassCommandKind::Decal);
    EXPECT_EQ(package.passCommandInputs[2].kind, NLS::Render::Context::RenderPassCommandKind::Skybox);
    EXPECT_EQ(package.passCommandInputs[3].kind, NLS::Render::Context::RenderPassCommandKind::Transparent);
    EXPECT_EQ(package.passCommandInputs[4].kind, NLS::Render::Context::RenderPassCommandKind::Helper);
    EXPECT_EQ(package.passCommandInputs[0].renderWidth, 256u);
    EXPECT_EQ(package.passCommandInputs[0].renderHeight, 144u);
    EXPECT_TRUE(package.passCommandInputs[0].usesColorAttachment);
    EXPECT_TRUE(package.passCommandInputs[0].clearColor);
    EXPECT_FLOAT_EQ(package.passCommandInputs[0].clearColorValue.x, 0.1f);
    EXPECT_FALSE(package.passCommandInputs[1].clearColor);
}

TEST(RenderFrameExecutionTests, RenderScenePackageMarksLargeAttachmentBackedRecordedDrawPassForInRenderPassChildRecording)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    NLS::Core::ServiceLocator::Provide(*driver);
    SceneSnapshotRenderer renderer(*driver);

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 10u;
    snapshot.targetsSwapchain = false;
    snapshot.renderWidth = 256u;
    snapshot.renderHeight = 144u;
    NLS::Render::Context::RenderSceneBuildData scene;
    scene.visibleOpaqueDrawCount = 2000u;
    snapshot.clearColorBuffer = true;
    snapshot.clearDepthBuffer = true;
    snapshot.clearStencilBuffer = false;
    scene.recordedDrawCommands.resize(static_cast<size_t>(scene.visibleOpaqueDrawCount));

    const auto package = renderer.CaptureRenderScenePackage(snapshot, std::move(scene));

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

TEST(RenderFrameExecutionTests, RecordedDrawCommandSlicingDoesNotCopySourceDrawVectorsIntoSlices)
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

TEST(RenderFrameExecutionTests, InRenderPassChildCommandRecordingSplitsBundlesByRenderTargetCompatiblePipelineLayout)
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

TEST(RenderFrameExecutionTests, InRenderPassChildCommandRecordingRejectsPipelineLayoutMismatchedWithParentPass)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 614u;
    snapshot.targetsSwapchain = false;

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

    auto result = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
        driver, snapshot, [package](size_t) { return package; }));
    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto submissionFrame = result.get();
    EXPECT_FALSE(submissionFrame.submittedSuccessfully);
    EXPECT_GT(submissionFrame.commandRecordingFailureCount, 0u);
    EXPECT_NE(
        submissionFrame.lastCommandRecordingFailure.find("render target layout"),
        std::string::npos);
    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 0u);
}

TEST(RenderFrameExecutionTests, InRenderPassChildCommandRecordingUsesOneParentPassAndChildDrawRanges)
{
    ScopedThreadedRenderingJobSystem jobSystem(2u);
    ASSERT_TRUE(jobSystem.IsInitialized());

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 604u;
    snapshot.targetsSwapchain = false;

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
    package.extractedTextures = { extractedTexture };
    package.parallelCommandWorkUnits = NLS::Render::Context::BuildRecordedDrawCommandWorkUnitsForPass(
        passInput,
        0u,
        0u);
    package.parallelCommandWorkUnitCount = static_cast<uint64_t>(package.parallelCommandWorkUnits.size());

    auto result = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
        driver, snapshot, [package](size_t) { return package; }));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto submissionFrame = result.get();
    EXPECT_TRUE(submissionFrame.usedParallelCommandPath);
    EXPECT_EQ(submissionFrame.recordedPassCount, 1u);
    EXPECT_EQ(submissionFrame.recordedWorkUnitCount, 4u);
    EXPECT_GT(submissionFrame.parallelRecordingWorkerCount, 1u);
    EXPECT_LE(submissionFrame.parallelRecordingWorkerCount, 4u);
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

TEST(RenderFrameExecutionTests, ExternalSceneOutputPreservesOrderedChildRecording)
{
    ScopedThreadedRenderingJobSystem jobSystem(2u);
    ASSERT_TRUE(jobSystem.IsInitialized());

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 617u;
    snapshot.targetsSwapchain = false;

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
        passInput.recordedDrawCommands.push_back({ opaquePipeline, nullptr, nullptr, nullptr, materialBindingSet, mesh, 1u });
    }

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
    package.externalSceneOutputTextureCount = 1u;
    package.externalSceneOutputIdentity = 7u;
    package.externalSceneOutputIdentities = { 7u };
    package.passCommandInputs = { passInput };
    package.parallelCommandWorkUnits = NLS::Render::Context::BuildRecordedDrawCommandWorkUnitsForPass(
        passInput,
        0u,
        0u);
    package.parallelCommandWorkUnitCount = static_cast<uint64_t>(package.parallelCommandWorkUnits.size());

    auto result = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
        driver, snapshot, [package](size_t) { return package; }));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto submissionFrame = result.get();
    EXPECT_TRUE(submissionFrame.usedParallelCommandPath);
    EXPECT_FALSE(submissionFrame.usedSerialCommandPath);
    EXPECT_GT(submissionFrame.parallelRecordingWorkerCount, 0u);
    EXPECT_EQ(submissionFrame.parallelFallbackReason.find("external scene output"), std::string::npos);
}

TEST(RenderFrameExecutionTests, InRenderPassChildCommandRecordingReusesChildCommandPoolsAcrossFenceConfirmedSlotReuse)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

        NLS::Render::Context::RenderFramePacket snapshot;
        snapshot.frameId = frameId;
        snapshot.targetsSwapchain = false;

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
        ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, snapshot, [package](size_t) { return package; }));
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

TEST(RenderFrameExecutionTests, InRenderPassChildCommandRecordingUsesDenseChildResourceSlots)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 614u;
    snapshot.targetsSwapchain = false;

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
        100u);
    package.parallelCommandWorkUnitCount = static_cast<uint64_t>(package.parallelCommandWorkUnits.size());
    ASSERT_EQ(package.parallelCommandWorkUnits.size(), 4u);
    ASSERT_EQ(package.parallelCommandWorkUnits.front().workUnitIndex, 100u);

    auto result = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
        driver, snapshot, [package](size_t) { return package; }));
    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);
    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);

    EXPECT_EQ(frameContext.childCommandPools.size(), 4u);
    EXPECT_EQ(frameContext.childCommandBuffers.size(), 4u);
    EXPECT_EQ(explicitDevice->GetCreatedCommandPools().size(), 4u);
    ASSERT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    const auto submittedBuffer = GetSubmittedTestCommandBuffer(explicitDevice->GetTestQueue());
    ASSERT_NE(submittedBuffer, nullptr);
    EXPECT_EQ(submittedBuffer->executeChildCommandBufferCalls, 4u);
}

TEST(RenderFrameExecutionTests, InRenderPassChildCommandRecordingRetainsChildBuffersWhenFenceWaitTimesOut)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 605u;
    snapshot.targetsSwapchain = false;

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

    auto result = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, snapshot, [package](size_t) { return package; }));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto submissionFrame = result.get();
    EXPECT_FALSE(submissionFrame.retirementFenceWaited);
    EXPECT_EQ(frameFence->waitCalls, 0u);
    EXPECT_EQ(descriptorAllocator->beginFrameCalls, 1u);
    EXPECT_EQ(descriptorAllocator->endFrameCalls, 0u);
    EXPECT_EQ(uploadContext->beginFrameCalls, 1u);
    EXPECT_EQ(uploadContext->endFrameCalls, 0u);
    EXPECT_GT(NLS::Render::Context::DriverTestAccess::GetRetainedThreadedSubmitResourceCount(driver), 0u);

    const auto submittedBuffer = GetSubmittedTestCommandBuffer(explicitDevice->GetTestQueue());
    ASSERT_NE(submittedBuffer, nullptr);
    EXPECT_EQ(submittedBuffer->executeChildCommandBufferCalls, 4u);
}

TEST(RenderFrameExecutionTests, InRenderPassChildCommandRecordingKeepsTimedOutSlotResourcesAcrossOtherSlotReuse)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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
        NLS::Render::Context::RenderFramePacket snapshot;
        snapshot.frameId = frameId;
        snapshot.targetsSwapchain = false;
        ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, snapshot, [package = buildPackage(frameId)](size_t) { return package; }));

        NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);
    }

    EXPECT_EQ(firstFence->waitCalls, 0u);
    EXPECT_EQ(secondFence->waitCalls, 0u);
    EXPECT_EQ(descriptorAllocators[0u]->beginFrameCalls, 1u);
    EXPECT_EQ(descriptorAllocators[0u]->endFrameCalls, 0u);
    EXPECT_EQ(uploadContexts[0u]->beginFrameCalls, 1u);
    EXPECT_EQ(uploadContexts[0u]->endFrameCalls, 0u);
    EXPECT_EQ(descriptorAllocators[1u]->beginFrameCalls, 1u);
    EXPECT_EQ(descriptorAllocators[1u]->endFrameCalls, 0u);
    EXPECT_EQ(uploadContexts[1u]->beginFrameCalls, 1u);
    EXPECT_EQ(uploadContexts[1u]->endFrameCalls, 0u);
    EXPECT_GT(NLS::Render::Context::DriverTestAccess::GetRetainedThreadedSubmitResourceCount(driver), 0u);
}

TEST(RenderFrameExecutionTests, InRenderPassChildCommandRecordingFallsBackToUnslicedSerialPassWithoutCapability)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 605u;
    snapshot.targetsSwapchain = false;

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
    ASSERT_GT(package.parallelCommandWorkUnits.size(), 1u);
    ASSERT_TRUE(package.parallelCommandWorkUnits[0].usesInRenderPassChildCommandRecording);

    auto result = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
        driver, snapshot, [package](size_t) { return package; }));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto submissionFrame = result.get();
    EXPECT_FALSE(submissionFrame.usedParallelCommandPath);
    EXPECT_TRUE(submissionFrame.usedSerialCommandPath);
    EXPECT_EQ(submissionFrame.recordedPassCount, 1u);
    EXPECT_EQ(submissionFrame.recordedWorkUnitCount, 1u);
    EXPECT_EQ(submissionFrame.recordedDrawCount, 2000u);
    EXPECT_NE(
        submissionFrame.parallelFallbackReason.find("in-render-pass child command recording unavailable"),
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

TEST(RenderFrameExecutionTests, InRenderPassChildCapabilityFallbackPreservesAttachmentFreeSlices)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 615u;
    snapshot.targetsSwapchain = false;

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
    package.frameId = snapshot.frameId;
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

    auto result = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
        driver, snapshot, [package](size_t) { return package; }));
    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto submissionFrame = result.get();
    EXPECT_TRUE(submissionFrame.submittedSuccessfully);
    EXPECT_EQ(submissionFrame.recordedDrawCount, 4000u);
    EXPECT_EQ(submissionFrame.recordedWorkUnitCount, 5u);
    EXPECT_NE(
        submissionFrame.parallelFallbackReason.find("in-render-pass child command recording unavailable"),
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

TEST(RenderFrameExecutionTests, InRenderPassChildCommandRecordingReleasesDeferredFrameScopedResourcesOnResizeAfterFence)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 608u;
    snapshot.targetsSwapchain = false;

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

    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, snapshot, [package](size_t) { return package; }));
    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    EXPECT_EQ(descriptorAllocator->endFrameCalls, 0u);
    EXPECT_EQ(uploadContext->endFrameCalls, 0u);
    EXPECT_EQ(resourceStateTracker->retireTransientResourcesCalls, 1u);
    EXPECT_GT(NLS::Render::Context::DriverTestAccess::GetRetainedThreadedSubmitResourceCount(driver), 0u);

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

TEST(RenderFrameExecutionTests, DriverRendererAccessWaitsForSubmittedGpuWorkFrameFences)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto pendingFence = std::make_shared<TestFence>();
    auto signaledFence = std::make_shared<TestFence>();
    signaledFence->signaled = true;
    NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u).frameFence = pendingFence;
    NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 1u).frameFence = signaledFence;
    auto* impl = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver);
    impl->frameResourceSlots->RetireSlot(0u, pendingFence);
    impl->frameResourceSlots->RetireSlot(1u, signaledFence);

    EXPECT_TRUE(NLS::Render::Context::DriverRendererAccess::TryWaitForSubmittedGpuWork(driver));
    EXPECT_EQ(pendingFence->waitCalls, 1u);
    EXPECT_EQ(signaledFence->waitCalls, 0u);
}

TEST(RenderFrameExecutionTests, DriverRendererAccessReportsSubmittedGpuWorkFenceTimeout)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto frameFence = std::make_shared<TestFence>();
    frameFence->waitResults = { false };
    NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u).frameFence = frameFence;
    NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver)
        ->frameResourceSlots->RetireSlot(0u, frameFence);

    EXPECT_FALSE(NLS::Render::Context::DriverRendererAccess::TryWaitForSubmittedGpuWork(driver));
    EXPECT_EQ(frameFence->waitCalls, 1u);
}

TEST(RenderFrameExecutionTests, InRenderPassChildCommandRecordingReleasesDeferredResourcesWithTimedOutSlotFrameIndex)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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
        NLS::Render::Context::RenderFramePacket snapshot;
        snapshot.frameId = frameId;
        snapshot.targetsSwapchain = false;

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

        ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, snapshot, [package](size_t) { return package; }));
        NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);
    }

    EXPECT_GT(NLS::Render::Context::DriverTestAccess::GetRetainedThreadedSubmitResourceCount(driver), 0u);

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

TEST(RenderFrameExecutionTests, PostSubmitBufferReadbackDoesNotRequireSemaphoreForGraphicsQueueProducer)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto* impl = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl, nullptr);
    impl->EnsureFrameContexts(1u);

    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    impl->explicitDevice = explicitDevice;

    auto& frameContext = impl->GetFrameContext(0u);
    frameContext.frameIndex = 13u;
    frameContext.commandPool = std::make_shared<TestCommandPool>();
    frameContext.commandBuffer = std::make_shared<TestCommandBuffer>();
    frameContext.frameFence = std::make_shared<TestFence>();

    NLS::Render::Context::Detail::AsyncComputeSubmitPlan submitPlan;
    NLS::Render::Context::Detail::ThreadedQueueSubmissionBatch graphicsBatch;
    graphicsBatch.queueType = NLS::Render::RHI::QueueType::Graphics;
    graphicsBatch.commandPools.push_back(frameContext.commandPool);
    graphicsBatch.commandBuffers.push_back(frameContext.commandBuffer);
    submitPlan.batches.push_back(std::move(graphicsBatch));

    auto sourceBuffer = std::make_shared<TestBuffer>(64u);
    auto destination = std::make_shared<std::array<uint32_t, 4>>();
    auto readbackState =
        std::make_shared<NLS::Render::Context::PostSubmitBufferReadbackState>();

    NLS::Render::Context::PostSubmitBufferReadbackRequest readbackRequest;
    readbackRequest.desc.source = sourceBuffer;
    readbackRequest.desc.sourceState = NLS::Render::RHI::ResourceState::ShaderWrite;
    readbackRequest.desc.size = sizeof(*destination);
    readbackRequest.desc.data = destination->data();
    readbackRequest.desc.debugName = "ThreadedHZBResultReadbackAfterGraphics";
    readbackRequest.state = readbackState;
    readbackRequest.destinationKeepAlive = destination;
    readbackRequest.waitForLastComputeQueueCompletion = true;

    NLS::Render::Context::RenderScenePackage package;
    package.frameId = 9005u;
    package.targetsSwapchain = false;
    package.postSubmitBufferReadbacks.push_back(readbackRequest);

    NLS::Render::Context::RhiSubmissionFrame submissionFrame;
    NLS::Render::Context::Detail::ExecuteThreadedSubmitPlan(
        *impl,
        frameContext,
        package,
        submitPlan,
        &submissionFrame,
        0u);

    EXPECT_TRUE(submissionFrame.submittedSuccessfully);
    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    EXPECT_EQ(explicitDevice->beginBufferReadbackCalls, 1u);
    EXPECT_TRUE(explicitDevice->lastBeginBufferReadbackDesc.waitSemaphores.empty());

    std::lock_guard lock(readbackState->mutex);
    EXPECT_TRUE(readbackState->beginAttempted);
    EXPECT_TRUE(readbackState->beginSucceeded);
}

TEST(RenderFrameExecutionTests, RenderCommandBeginSkipsBlockingWaitWhenReusableFenceAlreadySignaled)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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
    frameFence->waitResults = { false };
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = frameFence;

    auto swapchain = std::make_shared<TestSwapchain>();
    NLS::Render::Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);
    commandBuffer->onBegin = [&]
    {
        EXPECT_EQ(frameFence->waitCalls, 0u);
        EXPECT_EQ(frameFence->resetCalls, 0u);
        EXPECT_TRUE(frameFence->signaled);
        EXPECT_EQ(commandPool->resetCalls, 1u);
        EXPECT_EQ(commandBuffer->resetCalls, 1u);
    };

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.targetsSwapchain = true;
    NLS::Render::Context::RenderScenePackage package;
    package.targetsSwapchain = true;
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, snapshot, [package](size_t) { return package; }));
    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);
    EXPECT_EQ(commandBuffer->beginCalls, 1u);
    EXPECT_EQ(frameFence->waitCalls, 0u);

    frameFence->signaled = true;
}

TEST(RenderFrameExecutionTests, InRenderPassChildCommandRecordingRetainsResourcesWhenSubmitFailsAfterGpuWorkMayHaveBeenQueued)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 703u;
    snapshot.targetsSwapchain = false;

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

    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, snapshot, [package](size_t) { return package; }));
    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    EXPECT_EQ(descriptorAllocator->endFrameCalls, 0u);
    EXPECT_EQ(uploadContext->endFrameCalls, 0u);
    EXPECT_GT(NLS::Render::Context::DriverTestAccess::GetRetainedThreadedSubmitResourceCount(driver), 0u);
}

TEST(RenderFrameExecutionTests, InRenderPassChildCommandRecordingHandlesMixedComputeAndChildSlicesInOrder)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 606u;
    snapshot.targetsSwapchain = false;

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
    package.parallelCommandWorkUnits.push_back(computeWorkUnit);
    package.parallelCommandWorkUnits.insert(
        package.parallelCommandWorkUnits.end(),
        childWorkUnits.begin(),
        childWorkUnits.end());
    package.parallelCommandWorkUnitCount = static_cast<uint64_t>(package.parallelCommandWorkUnits.size());

    auto result = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
        driver, snapshot, [package](size_t) { return package; }));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto submissionFrame = result.get();
    EXPECT_TRUE(submissionFrame.usedParallelCommandPath);
    EXPECT_EQ(submissionFrame.recordedPassCount, 2u);
    EXPECT_EQ(submissionFrame.recordedDrawCount, 2000u);
    EXPECT_EQ(submissionFrame.recordedWorkUnitCount, 5u);
    ASSERT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    const auto submittedBuffer = GetSubmittedTestCommandBuffer(explicitDevice->GetTestQueue());
    ASSERT_NE(submittedBuffer, nullptr);
    EXPECT_EQ(submittedBuffer->dispatchCalls, 1u);
    EXPECT_EQ(submittedBuffer->beginRenderPassCalls, 1u);
    EXPECT_EQ(submittedBuffer->endRenderPassCalls, 1u);
    EXPECT_EQ(submittedBuffer->executeChildCommandBufferCalls, 4u);
    EXPECT_EQ(submittedBuffer->drawIndexedCalls, 0u);
}

TEST(RenderFrameExecutionTests, InRenderPassChildCommandRecordingPreservesMixedAttachmentFreeSlices)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 610u;
    snapshot.targetsSwapchain = false;

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
    package.frameId = snapshot.frameId;
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

    auto result = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
        driver, snapshot, [package](size_t) { return package; }));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto submissionFrame = result.get();
    EXPECT_TRUE(submissionFrame.submittedSuccessfully);
    EXPECT_EQ(submissionFrame.recordedDrawCount, 4000u);
    EXPECT_EQ(submissionFrame.recordedWorkUnitCount, 8u);
    ASSERT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    const auto submittedBuffer = GetSubmittedTestCommandBuffer(explicitDevice->GetTestQueue());
    ASSERT_NE(submittedBuffer, nullptr);
    EXPECT_EQ(submittedBuffer->drawIndexedCalls, 2000u);
    EXPECT_EQ(submittedBuffer->executeChildCommandBufferCalls, 4u);
}

TEST(RenderFrameExecutionTests, InRenderPassChildCommandRecordingRecordsAllSmallSliceGroupsSerially)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 612u;
    snapshot.targetsSwapchain = false;

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
    package.frameId = snapshot.frameId;
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

    auto result = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
        driver, snapshot, [package](size_t) { return package; }));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto submissionFrame = result.get();
    EXPECT_TRUE(submissionFrame.usedParallelCommandPath);
    EXPECT_FALSE(submissionFrame.usedSerialCommandPath);
    EXPECT_EQ(submissionFrame.recordedPassCount, 1u);
    EXPECT_EQ(submissionFrame.recordedDrawCount, 1024u);
    EXPECT_EQ(submissionFrame.recordedWorkUnitCount, 2u);
    EXPECT_EQ(submissionFrame.parallelRecordingWorkerCount, 1u);
    const auto submittedBuffer = GetSubmittedTestCommandBuffer(explicitDevice->GetTestQueue());
    ASSERT_NE(submittedBuffer, nullptr);
    EXPECT_EQ(submittedBuffer->executeChildCommandBufferCalls, 2u);
}

TEST(RenderFrameExecutionTests, InRenderPassChildCommandRecordingGroupsMultipleSourcePassesSeparately)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 607u;
    snapshot.targetsSwapchain = false;

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
    package.frameId = snapshot.frameId;
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

    auto result = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
        driver, snapshot, [package](size_t) { return package; }));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto submissionFrame = result.get();
    EXPECT_TRUE(submissionFrame.usedParallelCommandPath);
    EXPECT_EQ(submissionFrame.recordedPassCount, 2u);
    EXPECT_EQ(submissionFrame.recordedDrawCount, 4000u);
    EXPECT_EQ(submissionFrame.recordedWorkUnitCount, 8u);
    ASSERT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    const auto submittedBuffer = GetSubmittedTestCommandBuffer(explicitDevice->GetTestQueue());
    ASSERT_NE(submittedBuffer, nullptr);
    EXPECT_EQ(submittedBuffer->beginRenderPassCalls, 2u);
    EXPECT_EQ(submittedBuffer->endRenderPassCalls, 2u);
    EXPECT_EQ(submittedBuffer->executeChildCommandBufferCalls, 8u);
}

TEST(RenderFrameExecutionTests, InRenderPassChildCommandRecordingFallsBackToSerialWhenChildCreationFails)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 608u;
    snapshot.targetsSwapchain = false;

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

    auto result = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
        driver, snapshot, [package](size_t) { return package; }));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto submissionFrame = result.get();
    EXPECT_TRUE(submissionFrame.submittedSuccessfully);
    EXPECT_FALSE(submissionFrame.usedParallelCommandPath);
    EXPECT_TRUE(submissionFrame.usedSerialCommandPath);
    EXPECT_EQ(submissionFrame.commandRecordingFailureCount, 0u);
    EXPECT_EQ(submissionFrame.recordedPassCount, 1u);
    EXPECT_EQ(submissionFrame.recordedDrawCount, 2000u);
    ASSERT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    const auto submittedBuffer = GetSubmittedTestCommandBuffer(explicitDevice->GetTestQueue());
    ASSERT_NE(submittedBuffer, nullptr);
    EXPECT_EQ(submittedBuffer->executeChildCommandBufferCalls, 0u);
    EXPECT_EQ(submittedBuffer->beginRenderPassCalls, 1u);
    EXPECT_EQ(submittedBuffer->endRenderPassCalls, 1u);
    EXPECT_EQ(submittedBuffer->drawIndexedCalls, 2000u);
}

TEST(RenderFrameExecutionTests, InRenderPassChildCommandRecordingMarksFrameFailedWhenExecuteChildFails)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 609u;
    snapshot.targetsSwapchain = false;

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

    auto result = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
        driver, snapshot, [package](size_t) { return package; }));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto submissionFrame = result.get();
    EXPECT_FALSE(submissionFrame.submittedSuccessfully);
    EXPECT_GT(submissionFrame.commandRecordingFailureCount, 0u);
    EXPECT_NE(
        submissionFrame.lastCommandRecordingFailure.find("test execute child failure"),
        std::string::npos);
    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 0u);
    EXPECT_EQ(commandBuffer->endCalls, commandBuffer->beginCalls);
    EXPECT_TRUE(commandBuffer->IsClosedForSubmission());
}

TEST(RenderFrameExecutionTests, InRenderPassChildCommandRecordingFallsBackWhenChildRangeChangesDescriptorHeap)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 614u;
    snapshot.targetsSwapchain = false;

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

    auto result = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
        driver, snapshot, [package](size_t) { return package; }));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto submissionFrame = result.get();
    EXPECT_TRUE(submissionFrame.submittedSuccessfully);
    EXPECT_FALSE(submissionFrame.usedParallelCommandPath);
    EXPECT_TRUE(submissionFrame.usedSerialCommandPath);
    EXPECT_EQ(submissionFrame.commandRecordingFailureCount, 0u);
    EXPECT_EQ(submissionFrame.recordedPassCount, 1u);
    EXPECT_EQ(submissionFrame.recordedWorkUnitCount, 1u);
    EXPECT_EQ(submissionFrame.recordedDrawCount, 2000u);
    EXPECT_NE(
        submissionFrame.parallelFallbackReason.find("in-render-pass child command recording failed"),
        std::string::npos);
    ASSERT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    ASSERT_EQ(explicitDevice->GetTestQueue()->lastSubmitDesc.commandBuffers.size(), 1u);
    const auto submittedBuffer = GetSubmittedTestCommandBuffer(explicitDevice->GetTestQueue());
    ASSERT_NE(submittedBuffer, nullptr);
    EXPECT_EQ(submittedBuffer->executeChildCommandBufferCalls, 0u);
    EXPECT_EQ(submittedBuffer->drawIndexedCalls, 2000u);
}

TEST(RenderFrameExecutionTests, SetExplicitDeviceClearsReusableChildCommandResources)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

    auto secondDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, secondDevice);

    EXPECT_EQ(NLS::Render::Context::DriverTestAccess::PeekFrameContext(driver, 0u), nullptr);
    EXPECT_EQ(NLS::Render::Context::DriverTestAccess::GetRetainedThreadedSubmitResourceCount(driver), 0u);
    EXPECT_TRUE(weakSwapchain.expired());

    NLS::Core::ServiceLocator::Remove<NLS::Render::Context::Driver>();
}

TEST(RenderFrameExecutionTests, SetExplicitDeviceWaitsForThreadedRhiSubmissionBeforeReplacingResources)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

TEST(RenderFrameExecutionTests, InRenderPassChildCommandRecordingFallsBackToSerialWhenChildDrawFailsBeforeParentPass)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 613u;
    snapshot.targetsSwapchain = false;

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

    auto result = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
        driver, snapshot, [package](size_t) { return package; }));
    explicitDevice->FailCreatedChildCommandBufferDraws(true);

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto submissionFrame = result.get();
    EXPECT_TRUE(submissionFrame.submittedSuccessfully);
    EXPECT_FALSE(submissionFrame.usedParallelCommandPath);
    EXPECT_TRUE(submissionFrame.usedSerialCommandPath);
    EXPECT_EQ(submissionFrame.commandRecordingFailureCount, 0u);
    EXPECT_NE(
        submissionFrame.parallelFallbackReason.find("in-render-pass child command recording failed"),
        std::string::npos);
    ASSERT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    const auto submittedBuffer = GetSubmittedTestCommandBuffer(explicitDevice->GetTestQueue());
    ASSERT_NE(submittedBuffer, nullptr);
    EXPECT_EQ(submittedBuffer->executeChildCommandBufferCalls, 0u);
    EXPECT_EQ(submittedBuffer->drawIndexedCalls, 2000u);
}

TEST(RenderFrameExecutionTests, InRenderPassChildCommandRecordingRecordsDependencyVisibilityBeforeParentPass)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 611u;
    snapshot.targetsSwapchain = false;

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
    package.parallelCommandWorkUnits.push_back(computeWorkUnit);
    package.parallelCommandWorkUnits.insert(
        package.parallelCommandWorkUnits.end(),
        childWorkUnits.begin(),
        childWorkUnits.end());
    package.parallelCommandWorkUnitCount = static_cast<uint64_t>(package.parallelCommandWorkUnits.size());

    auto result = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
        driver, snapshot, [package](size_t) { return package; }));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto submissionFrame = result.get();
    EXPECT_TRUE(submissionFrame.submittedSuccessfully);
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

TEST(RenderFrameExecutionTests, AttachmentFreeRecordedDrawPassSlicesTranslateInParallelAndSubmitInOrder)
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
        EXPECT_TRUE(workUnit.eligibleForParallelRecording);
        EXPECT_TRUE(workUnit.eligibleForParallelTranslation);
        coveredDrawCount += workUnit.recordedDrawCount;
    }
    EXPECT_EQ(coveredDrawCount, 2000u);
}

TEST(RenderFrameExecutionTests, SlicedParallelWorkUnitsFallbackToUnslicedPassInputsWhenOrderedSubmissionIsUnsafe)
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

TEST(RenderFrameExecutionTests, FallbackComputeWorkUnitsReceiveUniqueResourceIndices)
{
    NLS::Render::Context::RenderScenePackage package;
    package.computeDispatchInputs.resize(2u);

    NLS::Render::Context::RenderPassCommandInput graphicsPassInput;
    graphicsPassInput.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    graphicsPassInput.queueType = NLS::Render::RHI::QueueType::Graphics;
    package.passCommandInputs.push_back(graphicsPassInput);

    const auto workUnits = NLS::Render::Context::Detail::BuildParallelCommandWorkUnits(
        package,
        false,
        false,
        false);

    ASSERT_EQ(workUnits.size(), 3u);
    for (size_t index = 0u; index < workUnits.size(); ++index)
    {
        EXPECT_EQ(workUnits[index].workUnitIndex, index);
        EXPECT_EQ(workUnits[index].submissionOrder, index);
    }
    EXPECT_EQ(workUnits[0].commandInput.queueType, NLS::Render::RHI::QueueType::Compute);
    EXPECT_EQ(workUnits[1].commandInput.queueType, NLS::Render::RHI::QueueType::Compute);
    EXPECT_EQ(workUnits[2].commandInput.queueType, NLS::Render::RHI::QueueType::Graphics);
}

TEST(RenderFrameExecutionTests, RecordedDrawCommandSlicingAllowsStencilClearInParentOwnedChildRecording)
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

TEST(RenderFrameExecutionTests, RecordedDrawCommandSlicingRejectsPassesWithSidecarComputeDispatches)
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

TEST(RenderFrameExecutionTests, RecordedDrawCommandSlicingRejectsSwapchainPasses)
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

TEST(RenderFrameExecutionTests, BaseSceneRendererPublishesPacketWithoutCopyingGameSceneSummary)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

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

    auto result = NLS::Tests::ObserveNextRenderSceneForTest(driver);
    ASSERT_NO_THROW(renderer.BeginFrame(frameDescriptor));
    ASSERT_NO_THROW(renderer.EndFrame());

    EXPECT_EQ(result.wait_for(std::chrono::milliseconds(0)), std::future_status::timeout);
    EXPECT_EQ(NLS::Render::Context::DriverRendererAccess::GetThreadedFrameTelemetry(driver).inFlightFrameCount, 1u);

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto package = result.get();
    EXPECT_EQ(package.sceneGameObjectCount, 0u);
    EXPECT_EQ(package.renderWidth, 128u);
    EXPECT_EQ(package.renderHeight, 96u);
    EXPECT_EQ(NLS::Render::Context::DriverRendererAccess::GetThreadedFrameTelemetry(driver).inFlightFrameCount, 0u);
}

TEST(RenderFrameExecutionTests, BaseRendererPublishesPreparedBuilderInsteadOfRawSnapshotWhenThreadedWorkersArePaused)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    VisibleSnapshotPublishingRenderer renderer(driver);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 128u;
    frameDescriptor.renderHeight = 96u;
    frameDescriptor.camera = &camera;

    auto result = NLS::Tests::ObserveNextRenderSceneForTest(driver);
    ASSERT_NO_THROW(renderer.BeginFrame(frameDescriptor));
    ASSERT_NO_THROW(renderer.EndFrame());

    EXPECT_EQ(result.wait_for(std::chrono::milliseconds(0)), std::future_status::timeout);
    EXPECT_EQ(NLS::Render::Context::DriverRendererAccess::GetThreadedFrameTelemetry(driver).inFlightFrameCount, 1u);

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto package = result.get();
    EXPECT_EQ(package.visibleDrawCount, 1u);
    ASSERT_EQ(package.passCommandInputs.size(), 1u);
    EXPECT_EQ(package.passCommandInputs[0].kind, NLS::Render::Context::RenderPassCommandKind::Opaque);
    EXPECT_EQ(NLS::Render::Context::DriverRendererAccess::GetThreadedFrameTelemetry(driver).inFlightFrameCount, 0u);
}

TEST(RenderFrameExecutionTests, PreparedBuilderPathDoesNotSilentlyUseDriverScenePackageWhenPayloadIsMissing)
{
    const NLS::Tests::ScopedRenderFailureReset resetFailure;
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    auto device = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, device);
    auto result = NLS::Tests::ObserveNextRenderSceneForTest(driver);
    NLS::Render::Context::RenderFramePacket packet;
    packet.renderWidth = 128u;
    packet.renderHeight = 96u;
    uint64_t publishedFrameId = 99u;
    EXPECT_FALSE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
        driver, packet, {}, nullptr, &publishedFrameId));
    EXPECT_EQ(publishedFrameId, 0u);
    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);
    EXPECT_EQ(result.wait_for(std::chrono::milliseconds(0)), std::future_status::timeout);
    const auto telemetry = NLS::Render::Context::DriverRendererAccess::GetThreadedFrameTelemetry(driver);
    EXPECT_EQ(telemetry.publishedFrameCount, 0u);
    EXPECT_EQ(telemetry.inFlightFrameCount, 0u);
    EXPECT_EQ(device->GetTestQueue()->submitCalls, 0u);
}

TEST(RenderFrameExecutionTests, Dx12PreparedBuilderDrainReportsRendererAttribution)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::DX12;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    VisibleSnapshotPublishingRenderer renderer(driver);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 128u;
    frameDescriptor.renderHeight = 96u;
    frameDescriptor.camera = &camera;

    auto result = NLS::Tests::ObserveNextRenderSceneForTest(driver);
    ASSERT_NO_THROW(renderer.BeginFrame(frameDescriptor));
    ASSERT_NO_THROW(renderer.EndFrame());

    EXPECT_EQ(result.wait_for(std::chrono::milliseconds(0)), std::future_status::timeout);

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto package = result.get();
    EXPECT_EQ(package.visibleDrawCount, 1u);
    EXPECT_EQ(package.renderWidth, 128u);
    EXPECT_EQ(package.renderHeight, 96u);
    EXPECT_EQ(NLS::Render::Context::DriverRendererAccess::GetThreadedFrameTelemetry(driver).inFlightFrameCount, 0u);
}

TEST(RenderFrameExecutionTests, RenderScenePackageSplitsRecordedDrawCommandsIntoPassOwnedPlans)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    SceneSnapshotRenderer renderer(driver);

    auto opaquePipeline = std::make_shared<TestGraphicsPipeline>("OpaquePipeline");
    auto decalPipeline = std::make_shared<TestGraphicsPipeline>("DecalPipeline");
    auto transparentPipelineA = std::make_shared<TestGraphicsPipeline>("TransparentPipelineA");
    auto transparentPipelineB = std::make_shared<TestGraphicsPipeline>("TransparentPipelineB");
    auto skyPipeline = std::make_shared<TestGraphicsPipeline>("SkyPipeline");
    auto materialBindingSet = std::make_shared<TestBindingSet>("MaterialBindingSet");
    auto mesh = std::make_shared<TestMesh>();

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 19u;
    snapshot.renderWidth = 256u;
    snapshot.renderHeight = 144u;
    NLS::Render::Context::RenderSceneBuildData scene;
    scene.visibleOpaqueDrawCount = 1u;
    scene.visibleDecalDrawCount = 1u;
    scene.visibleTransparentDrawCount = 2u;
    scene.visibleSkyboxDrawCount = 1u;

    scene.recordedDrawCommands = {
        { opaquePipeline, nullptr, nullptr, nullptr, materialBindingSet, mesh, 1u },
        { decalPipeline, nullptr, nullptr, nullptr, materialBindingSet, mesh, 1u },
        { skyPipeline, nullptr, nullptr, nullptr, materialBindingSet, mesh, 1u },
        { transparentPipelineA, nullptr, nullptr, nullptr, materialBindingSet, mesh, 1u },
        { transparentPipelineB, nullptr, nullptr, nullptr, materialBindingSet, mesh, 1u }
    };

    const auto package = renderer.CaptureRenderScenePackage(snapshot, std::move(scene));

    ASSERT_EQ(package.passCommandInputs.size(), 4u);
    EXPECT_EQ(package.passCommandInputs[0].kind, NLS::Render::Context::RenderPassCommandKind::Opaque);
    EXPECT_EQ(package.passCommandInputs[1].kind, NLS::Render::Context::RenderPassCommandKind::Decal);
    EXPECT_EQ(package.passCommandInputs[2].kind, NLS::Render::Context::RenderPassCommandKind::Skybox);
    EXPECT_EQ(package.passCommandInputs[3].kind, NLS::Render::Context::RenderPassCommandKind::Transparent);
    ASSERT_EQ(package.passCommandInputs[0].recordedDrawCommands.size(), 1u);
    EXPECT_EQ(package.passCommandInputs[0].recordedDrawCommands[0].pipeline, opaquePipeline);
    ASSERT_EQ(package.passCommandInputs[1].recordedDrawCommands.size(), 1u);
    EXPECT_EQ(package.passCommandInputs[1].recordedDrawCommands[0].pipeline, decalPipeline);
    ASSERT_EQ(package.passCommandInputs[2].recordedDrawCommands.size(), 1u);
    EXPECT_EQ(package.passCommandInputs[2].recordedDrawCommands[0].pipeline, skyPipeline);
    ASSERT_EQ(package.passCommandInputs[3].recordedDrawCommands.size(), 2u);
    EXPECT_EQ(package.passCommandInputs[3].recordedDrawCommands[0].pipeline, transparentPipelineA);
    EXPECT_EQ(package.passCommandInputs[3].recordedDrawCommands[1].pipeline, transparentPipelineB);
}

TEST(RenderFrameExecutionTests, DestroyingARendererDrainsTheFramesItPublished)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

    auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    NLS::Core::ServiceLocator::Provide(*driver);
    // Workers stay paused, so a published frame cannot be consumed by the render
    // thread and the renderer really is destroyed with frames still in flight.
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(*driver);

    ASSERT_EQ(NLS::Render::Context::DriverRendererAccess::GetThreadedFrameTelemetry(*driver).inFlightFrameCount, 0u);

    {
        NLS::Engine::SceneSystem::Scene scene;
        scene.CreateGameObject("ModelActor").AddComponent<NLS::Engine::Components::MeshRenderer>();

        NLS::Engine::Rendering::ForwardSceneRenderer renderer(*driver);
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

        renderer.BeginFrame(frameDescriptor);
        renderer.EndFrame();

        // The published builder holds this renderer, its frame-object binding
        // provider and its scene state by raw pointer.
        ASSERT_GT(NLS::Render::Context::DriverRendererAccess::GetThreadedFrameTelemetry(*driver).inFlightFrameCount, 0u);
    }

    // Destruction therefore has to retire those frames before releasing any of
    // that state. Draining late, or not at all, leaves the render thread
    // dereferencing freed memory once it is resumed.
    EXPECT_EQ(NLS::Render::Context::DriverRendererAccess::GetThreadedFrameTelemetry(*driver).inFlightFrameCount, 0u);
}

TEST(RenderFrameExecutionTests, ForwardThreadedPathResolvesAndSubmitsOnRealDx12Device)
{
    // Real-device coverage for the threaded Forward path. The unit suites run on
    // the None backend, where nothing reaches a GPU queue, so they cannot show that
    // the game-thread capture / render-thread resolve split actually produces a
    // submitted frame. This test asks for DX12 with an explicit device, which the
    // validation runner enables the debug layer and DRED so both the submission
    // and any validation error surface here.
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::DX12;
    settings.enableExplicitRHI = true;
    settings.framesInFlight = 2u;

    // Deliberately not a function-local static. A static driver outlives the test
    // and is torn down during process exit, where its render thread can outlive the
    // process-wide state it reads; destroying it inside the test keeps this test from
    // contributing to that teardown hang. The renderer below is declared after it, so
    // the renderer is destroyed first and the driver can still drain.
    auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    if (driver->GetActiveGraphicsBackend() != NLS::Render::Settings::EGraphicsBackend::DX12)
        GTEST_SKIP() << "No DX12 device is available on this host.";

    // Scoped, not a bare Provide: this driver is a local, so leaving it registered
    // would hand every later test in the process a dangling service reference.
    ScopedDriverService driverService(*driver);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::HasExplicitRHI(*driver));
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::IsThreadedRenderingEnabled(*driver))
        << "The threaded path is the one under test; a non-threaded fallback would not cover it.";

#if defined(_WIN32)
    const auto explicitDevice =
        NLS::Render::Context::DriverRendererAccess::GetExplicitDevice(*driver);
    auto* nativeDevice = static_cast<ID3D12Device*>(explicitDevice->GetNativeDeviceInfo().device);
    ASSERT_NE(nativeDevice, nullptr);
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> nativeMessages;
    (void)nativeDevice->QueryInterface(IID_PPV_ARGS(nativeMessages.GetAddressOf()));
#endif

    std::unique_ptr<NLS::Engine::Rendering::ForwardSceneRenderer> renderer;
    ASSERT_NO_THROW(renderer = std::make_unique<NLS::Engine::Rendering::ForwardSceneRenderer>(*driver));
    ASSERT_NE(renderer, nullptr);

    // Real geometry, not just a light. An offscreen frame that records no passes is
    // deliberately not submitted, so a zero-draw scene would never reach the queue
    // and could not show that the threaded path actually submits.
    NLS::Render::Resources::Shader* shader =
        NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl(
            "App/Assets/Engine/Shaders/Standard.hlsl");
    if (shader == nullptr)
        GTEST_SKIP() << "The built-in Standard shader is unavailable on this host.";

    NLS::Render::Resources::Material material;
    material.SetShader(shader);

    // Constructed through a raw pointer: the vertex/index arguments are braced
    // lists, which make_unique's forwarding cannot deduce.
    std::unique_ptr<NLS::Render::Resources::Mesh> mesh(
        new NLS::Render::Resources::Mesh(
        std::vector<NLS::Render::Geometry::Vertex>{
            [] { NLS::Render::Geometry::Vertex v{}; v.position[0] = -0.5f; v.normals[2] = 1.0f; return v; }(),
            [] { NLS::Render::Geometry::Vertex v{}; v.position[0] = 0.5f; v.normals[2] = 1.0f; return v; }(),
            [] { NLS::Render::Geometry::Vertex v{}; v.position[1] = 0.5f; v.normals[2] = 1.0f; return v; }()
        },
        std::vector<uint32_t>{0u, 1u, 2u},
        0u,
        NLS::Render::Resources::MeshBufferUploadMode::GpuOnly,
        {{0.0f, 0.0f, 0.0f}, 1.0f}));

    NLS::Engine::SceneSystem::Scene scene;
    scene.CreateGameObject("LightActor").AddComponent<NLS::Engine::Components::LightComponent>();
    {
        auto& object = scene.CreateGameObject("MeshActor");
        auto* meshFilter = object.AddComponent<NLS::Engine::Components::MeshFilter>();
        auto* meshRenderer = object.AddComponent<NLS::Engine::Components::MeshRenderer>();
        meshFilter->SetMesh(mesh.get());
        meshRenderer->SetFrustumBehaviour(
            NLS::Engine::Components::MeshRenderer::EFrustumBehaviour::CULL_MODEL);
        meshRenderer->FillWithMaterial(material);
        // In front of the default camera so the frame has something to submit.
        object.GetTransform()->SetWorldPosition({0.0f, 0.0f, -6.0f});
    }
    NLS::Render::Buffers::Framebuffer outputBuffer(160u, 120u);
    ASSERT_NE(outputBuffer.GetExplicitTextureHandle(), nullptr)
        << "The offscreen target must be a real device resource for this test to mean anything.";

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 160u;
    frameDescriptor.renderHeight = 120u;
    frameDescriptor.camera = &camera;
    NLS::Render::FrameGraph::SetExternalSceneOutputFramebuffer(frameDescriptor, &outputBuffer);

    std::vector<NLS::Render::Context::RhiSubmissionFrame> completedFrames;

    for (uint32_t frameIndex = 0u; frameIndex < 8u; ++frameIndex)
    {
        // Re-attached every frame: EndFrame clears the descriptor set, which is how
        // the renderer is driven in production too.
        renderer->AddDescriptor<NLS::Engine::Rendering::BaseSceneRenderer::SceneDescriptor>({
            scene,
            std::nullopt,
            nullptr
        });
        auto result = NLS::Tests::ObserveNextRhiFrameForTest(*driver);
        ASSERT_NO_THROW(renderer->BeginFrame(frameDescriptor));
        ASSERT_NO_THROW(renderer->DrawFrame());
        ASSERT_NO_THROW(renderer->EndFrame());

        // Drain between frames the way the production frame loop does at its
        // present/UI synchronisation point. Without this the game thread races eight
        // frames ahead of the render thread, the render scene refuses the excess
        // captures, and whether any frame is actually submitted depends on which
        // thread wins - which is a property of the host, not of the threaded path
        // this test exists to cover.
        ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryDrainThreadedRendering(*driver));
        ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
        completedFrames.push_back(result.get());
    }

    // Retire everything before reading telemetry, so the counters below describe
    // frames that actually completed rather than frames still in flight.
    NLS::Render::Context::DriverRendererAccess::DrainThreadedRendering(*driver);
    NLS::Render::Context::FlushRenderingCommands(
        NLS::Render::Context::RhiFlushFlags::Submit | NLS::Render::Context::RhiFlushFlags::Gpu);

#if defined(_WIN32)
    // GPU Validation reports draw errors asynchronously; inspect after the GPU drain.
    if (nativeMessages != nullptr)
    {
        const auto messageCount = nativeMessages->GetNumStoredMessagesAllowedByRetrievalFilter();
        for (UINT64 index = 0u; index < messageCount; ++index)
        {
            SIZE_T size = 0u;
            ASSERT_TRUE(SUCCEEDED(nativeMessages->GetMessage(index, nullptr, &size)));
            std::vector<std::byte> bytes(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
            ASSERT_TRUE(SUCCEEDED(nativeMessages->GetMessage(index, message, &size)));
            EXPECT_NE(message->Severity, D3D12_MESSAGE_SEVERITY_ERROR) << message->pDescription;
            EXPECT_NE(message->Severity, D3D12_MESSAGE_SEVERITY_CORRUPTION) << message->pDescription;
        }
    }
#endif

    const auto telemetry =
        NLS::Render::Context::DriverRendererAccess::GetThreadedFrameTelemetry(*driver);

    EXPECT_FALSE(telemetry.deviceLostDetected) << telemetry.deviceLostReason;
    EXPECT_FALSE(telemetry.unsafeGpuWorkQuarantined)
        << "GPU work was queued without a retirement fence, so every later frame is "
           "refused at BeginThreadedRhiFrame: "
        << telemetry.unsafeGpuWorkQuarantineReason;
    EXPECT_EQ(telemetry.queueOperationFailureCount, 0u) << telemetry.lastQueueOperationFailure;
    EXPECT_GT(telemetry.publishedFrameCount, 0u)
        << "The threaded path must publish frames, not silently drop every one.";
    EXPECT_GT(telemetry.latestRetiredFrameId, 0u)
        << "A published frame must actually retire on the real device. ";
    EXPECT_EQ(telemetry.inFlightFrameCount, 0u)
        << "Every frame must be handed back once the device work completed. ";

    ASSERT_EQ(completedFrames.size(), 8u);
    for (const auto& frame : completedFrames)
        EXPECT_TRUE(frame.submittedSuccessfully) << frame.lastCommandRecordingFailure;
    mesh.reset();
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
}

TEST(RenderFrameExecutionTests, ForwardSceneRendererKeepsFrozenViewAndResolvesObjectCountOnRenderThread)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 2u;

    auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(*driver);
    NLS::Core::ServiceLocator::Provide(*driver);
    std::unique_ptr<NLS::Engine::Rendering::ForwardSceneRenderer> renderer;
    ASSERT_NO_THROW(renderer = std::make_unique<NLS::Engine::Rendering::ForwardSceneRenderer>(*driver));
    ASSERT_NE(renderer, nullptr);

    NLS::Engine::SceneSystem::Scene scene;
    scene.CreateGameObject("ModelActor").AddComponent<NLS::Engine::Components::MeshRenderer>();
    scene.CreateGameObject("LightActor").AddComponent<NLS::Engine::Components::LightComponent>();
    scene.CreateGameObject("ObjectWithoutRenderProxy");
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

    auto result = NLS::Tests::ObserveNextRenderFrameForTest(*driver);
    ASSERT_NO_THROW(renderer->BeginFrame(frameDescriptor));
    const auto capturedFov = camera.GetFov();
    camera.SetFov(20.0f);
    scene.CreateGameObject("LateMutation").AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NO_THROW(renderer->EndFrame());

    EXPECT_EQ(result.wait_for(std::chrono::milliseconds(0)), std::future_status::timeout);
    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(*driver);
    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto [packet, package] = result.get();
    ASSERT_NE(packet.view, nullptr);
    ASSERT_NE(packet.view->camera, nullptr);
    EXPECT_FLOAT_EQ(packet.view->camera->GetFov(), capturedFov);
    EXPECT_EQ(packet.renderWidth, 128u);
    EXPECT_EQ(packet.renderHeight, 96u);
    EXPECT_EQ(package.sceneGameObjectCount, 3u);
    EXPECT_EQ(package.visibleDrawCount, 0u);
}

TEST(RenderFrameExecutionTests, FrozenLightGridContextNeverCarriesAnotherFramesCamera)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableLightGrid = true;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    std::unique_ptr<NLS::Engine::Rendering::ForwardSceneRenderer> renderer;
    ASSERT_NO_THROW(renderer = std::make_unique<NLS::Engine::Rendering::ForwardSceneRenderer>(driver));
    ASSERT_NE(renderer, nullptr);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 160u;
    frameDescriptor.renderHeight = 120u;
    frameDescriptor.camera = &camera;

    auto firstFrame = NLS::Render::FrameGraph::FreezeFrameDescriptor(frameDescriptor);
    ASSERT_NE(firstFrame.camera, nullptr);
    const auto firstContext = renderer->BuildLightGridCompileContext(firstFrame, false);
    ASSERT_EQ(firstContext.frameDescriptor.camera, firstFrame.camera.get());

    // Same inputs, next frame. The compile-context cache is expected to hit here,
    // and a hit still has to be handed this frame's camera: the frozen camera that
    // produced the cached entry belongs to a frame that is already over, and the
    // pass compiler dereferences this pointer for the frame's clear state.
    auto secondFrame = NLS::Render::FrameGraph::FreezeFrameDescriptor(frameDescriptor);
    ASSERT_NE(secondFrame.camera.get(), firstFrame.camera.get());
    const auto secondContext = renderer->BuildLightGridCompileContext(secondFrame, false);
    EXPECT_EQ(secondContext.frameDescriptor.camera, secondFrame.camera.get());
    EXPECT_NE(secondContext.frameDescriptor.camera, firstFrame.camera.get());
    EXPECT_EQ(secondContext.frameDescriptor.renderWidth, frameDescriptor.renderWidth);
    EXPECT_EQ(secondContext.frameDescriptor.renderHeight, frameDescriptor.renderHeight);
}

TEST(RenderFrameExecutionTests, DeferredSceneRendererQueuesFrozenViewBeforeResolvingAnEmptyNativeScene)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 2u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    std::unique_ptr<NLS::Engine::Rendering::DeferredSceneRenderer> renderer;
    ASSERT_NO_THROW(renderer = std::make_unique<NLS::Engine::Rendering::DeferredSceneRenderer>(driver));
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

    auto result = NLS::Tests::ObserveNextRenderSceneForTest(driver);
    ASSERT_NO_THROW(renderer->BeginFrame(frameDescriptor));
    EXPECT_FALSE(renderer->IsThreadedFramePublishSkippedForCurrentFrame());
    EXPECT_EQ(renderer->GetCumulativeFrameInfo().parseSceneCallCount, 0u);
    scene.CreateGameObject("LateMutation").AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NO_THROW(renderer->EndFrame());
    EXPECT_EQ(renderer->GetFrameInfo().parseSceneCallCount, 0u);

    EXPECT_EQ(result.wait_for(std::chrono::milliseconds(0)), std::future_status::timeout);
    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);
    EXPECT_EQ(renderer->GetFrameInfo().parseSceneCallCount, 1u);
    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto package = result.get();
    EXPECT_EQ(package.sceneGameObjectCount, 2u);
    EXPECT_EQ(package.visibleDrawCount, 0u);
}

TEST(RenderFrameExecutionTests, DeferredSceneRendererPublishesPreparedRenderScenePackageWhenThreadedWorkersArePaused)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    NLS::Engine::Rendering::DeferredSceneRenderer renderer(driver);
    std::vector<NLS::Render::Geometry::Vertex> vertices(3u);
    vertices[1].position[0] = 1.0f;
    vertices[2].position[1] = 1.0f;
    NLS::Render::Resources::Mesh sceneMesh(
        vertices,
        {},
        0u,
        NLS::Render::Resources::MeshBufferUploadMode::CpuToGpu,
        {{0.0f, 0.0f, 0.0f}, 1.0f});
    auto* gbufferShader = NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferShader(renderer);
    ASSERT_NE(gbufferShader, nullptr);
    if (gbufferShader->GetCompiledArtifacts().empty())
        GTEST_SKIP() << "Deferred threaded scene packaging requires compiled shader artifacts.";
    NLS::Render::Resources::Material sceneMaterial;
    sceneMaterial.SetShader(gbufferShader);
    NLS::Engine::SceneSystem::Scene scene;
    auto& modelActor = scene.CreateGameObject("ModelActor");
    auto* meshFilter = modelActor.AddComponent<NLS::Engine::Components::MeshFilter>();
    ASSERT_NE(meshFilter, nullptr);
    meshFilter->SetMesh(&sceneMesh);
    auto* meshRenderer = modelActor.AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(meshRenderer, nullptr);
    meshRenderer->FillWithMaterial(sceneMaterial);
    modelActor.GetTransform()->SetWorldPosition({0.0f, 0.0f, -6.0f});
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

    auto result = NLS::Tests::ObserveNextRenderFrameForTest(driver);
    ASSERT_NO_THROW(renderer.BeginFrame(frameDescriptor));
    EXPECT_FALSE(renderer.IsThreadedFramePublishSkippedForCurrentFrame());
    EXPECT_EQ(renderer.GetCumulativeFrameInfo().parseSceneCallCount, 0u);
    scene.CreateGameObject("LateMutation").AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NO_THROW(renderer.EndFrame());
    EXPECT_EQ(renderer.GetFrameInfo().parseSceneCallCount, 0u);

    EXPECT_EQ(result.wait_for(std::chrono::milliseconds(0)), std::future_status::timeout);

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);
    EXPECT_EQ(renderer.GetFrameInfo().parseSceneCallCount, 1u);

    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto [packet, package] = result.get();
    ASSERT_NE(packet.view, nullptr);
    EXPECT_EQ(packet.view->descriptor.renderWidth, 128u);
    EXPECT_EQ(packet.view->descriptor.renderHeight, 96u);
    EXPECT_EQ(package.sceneGameObjectCount, 2u);
    EXPECT_TRUE(package.frameDataReady);
    EXPECT_TRUE(package.objectDataReady);
    EXPECT_TRUE(package.lightingDataReady);
    ASSERT_EQ(package.passCommandInputs.size(), 2u);
    const auto& gbufferPass = package.passCommandInputs[0];
    const auto& lightingPass = package.passCommandInputs[1];
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
    // Lighting reads the four GBuffer textures plus the retained depth input.
    ASSERT_EQ(lightingPass.textureResourceAccesses.size(), 5u);
}

namespace
{
    template <typename SceneRenderer>
    void VerifyLateSceneFrameCommands()
    {
        using namespace NLS::Render::Context;
        NLS::Render::Settings::DriverSettings settings;
        settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
        settings.enableExplicitRHI = false;
        Driver driver(settings);
        const ScopedDriverService driverService(driver);
        DriverTestAccess::PauseThreadedRenderingWorkers(driver);
        DriverTestAccess::SetExplicitDevice(driver, std::make_shared<TestExplicitDevice>());
        NLS::Engine::SceneSystem::Scene scene;
        LateFrameCommandRenderer<SceneRenderer> renderer(driver);
        renderer.template AddDescriptor<NLS::Engine::Rendering::BaseSceneRenderer::SceneDescriptor>(
            {scene, std::nullopt, nullptr});
        NLS::Render::Entities::Camera camera;
        NLS::Render::Data::FrameDescriptor frame;
        frame.renderWidth = 128u;
        frame.renderHeight = 96u;
        frame.camera = &camera;
        const auto capturedFov = camera.GetFov();
        auto result = NLS::Tests::ObserveNextRenderFrameForTest(driver);
        ASSERT_NO_THROW(renderer.BeginFrame(frame));
        camera.SetFov(20.0f);
        auto pipeline = std::make_shared<TestGraphicsPipeline>("LateHelper");
        auto binding = std::make_shared<TestBindingSet>("LateHelperBinding");
        auto mesh = std::make_shared<TestMesh>();
        ASSERT_TRUE(renderer.QueueHelper(pipeline, binding, mesh));
        PostSubmitTextureReadbackRequest readback;
        readback.state = std::make_shared<PostSubmitTextureReadbackState>();
        renderer.SetNextFramePostSubmitTextureReadback(readback);
        ASSERT_NO_THROW(renderer.EndFrame());
        DriverTestAccess::DrainThreadedRendering(driver);
        ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
        const auto [packet, package] = result.get();
        ASSERT_EQ(package.recordedDrawCommands.size(), 1u);
        EXPECT_EQ(package.recordedDrawCommands.front().mesh, mesh);
        const auto helper = std::find_if(package.passCommandInputs.begin(), package.passCommandInputs.end(),
            [](const auto& pass) { return pass.kind == RenderPassCommandKind::Helper; });
        ASSERT_NE(helper, package.passCommandInputs.end());
        ASSERT_EQ(helper->recordedDrawCommands.size(), 1u);
        EXPECT_EQ(helper->recordedDrawCommands.front().pipeline, pipeline);
        ASSERT_EQ(package.postSubmitTextureReadbacks.size(), 1u);
        EXPECT_EQ(package.postSubmitTextureReadbacks.front().state, readback.state);
        ASSERT_NE(packet.view, nullptr);
        EXPECT_FLOAT_EQ(packet.view->camera->GetFov(), capturedFov);
    }
}

TEST(RenderFrameExecutionTests, DeferredSceneRendererPreservesLateHelperAndReadbackWithFrozenView)
{
    VerifyLateSceneFrameCommands<NLS::Engine::Rendering::DeferredSceneRenderer>();
}

TEST(RenderFrameExecutionTests, ForwardSceneRendererPreservesLateHelperAndReadbackWithFrozenView)
{
    VerifyLateSceneFrameCommands<NLS::Engine::Rendering::ForwardSceneRenderer>();
}

TEST(RenderFrameExecutionTests, DeferredSceneRendererSkipsThreadedPublishWhenPipelineResourcesAreMissing)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    NLS::Engine::Rendering::DeferredSceneRenderer::ConstructionOptions options;
    options.loadPipelineResources = false;
    NLS::Engine::Rendering::DeferredSceneRenderer renderer(driver, options);

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
    EXPECT_TRUE(renderer.IsThreadedFramePublishSkippedForCurrentFrame());
    ASSERT_NO_THROW(renderer.EndFrame());

    EXPECT_EQ(NLS::Render::Context::DriverRendererAccess::GetThreadedFrameTelemetry(driver).publishedFrameCount, 0u);
}

TEST(RenderFrameExecutionTests, DeferredLightGridSceneExecutionPreservesTransparentPassAfterLighting)
{
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 64u;
    frameDescriptor.renderHeight = 64u;

    NLS::Render::FrameGraph::LightGridCompileContext lightGridContext;
    lightGridContext.frameDescriptor = frameDescriptor;
    lightGridContext.graphicsPassBindingSet = std::make_shared<TestBindingSet>("LightGridGraphicsPass");

    auto gbufferPipeline = std::make_shared<TestGraphicsPipeline>("DeferredGBufferPipeline");
    auto lightingPipeline = std::make_shared<TestGraphicsPipeline>("DeferredLightingPipeline");
    auto transparentPipeline = std::make_shared<TestGraphicsPipeline>("DeferredTransparentPipeline");
    auto materialBindingSet = std::make_shared<TestBindingSet>("MaterialBindingSet");
    auto mesh = std::make_shared<TestMesh>();

    NLS::Render::Context::RenderScenePackage package;
    package.frameId = 604u;
    package.targetsSwapchain = false;
    package.renderWidth = frameDescriptor.renderWidth;
    package.renderHeight = frameDescriptor.renderHeight;
    package.opaqueDrawCount = 1u;
    package.transparentDrawCount = 1u;
    package.visibleDrawCount = 3u;
    package.drawCommandCount = 3u;
    package.hasVisibleDraws = true;
    package.hasOpaquePass = true;
    package.hasTransparentPass = true;
    package.frameDataReady = true;
    package.objectDataReady = true;
    package.lightingDataReady = true;
    package.recordedDrawCommands = {
        { gbufferPipeline, nullptr, nullptr, nullptr, materialBindingSet, mesh, 1u },
        { lightingPipeline, nullptr, nullptr, nullptr, materialBindingSet, mesh, 1u },
        { transparentPipeline, nullptr, nullptr, nullptr, materialBindingSet, mesh, 1u }
    };

    auto resources = BuildDeferredTestPreparedSceneResources(
        frameDescriptor.renderWidth,
        frameDescriptor.renderHeight);
    NLS::Render::FrameGraph::DeferredPreparedQueuedDrawCounts queuedDrawCounts;
    queuedDrawCounts.decalDrawCount = 0u;
    queuedDrawCounts.lightingDrawCount = 1u;
    queuedDrawCounts.transparentDrawCount = 1u;
    const auto execution =
        NLS::Render::FrameGraph::CompileAndApplyPreparedDeferredLightGridSceneExecution(
            package,
            lightGridContext,
            resources,
            {},
            {},
            queuedDrawCounts);
    (void)execution;

    ASSERT_EQ(package.passCommandInputs.size(), 3u);
    const auto& gbufferPass = package.passCommandInputs[0];
    const auto& lightingPass = package.passCommandInputs[1];
    const auto& transparentPass = package.passCommandInputs[2];
    EXPECT_EQ(gbufferPass.kind, NLS::Render::Context::RenderPassCommandKind::GBuffer);
    EXPECT_EQ(lightingPass.kind, NLS::Render::Context::RenderPassCommandKind::Lighting);
    EXPECT_EQ(transparentPass.kind, NLS::Render::Context::RenderPassCommandKind::Transparent);
    ASSERT_EQ(transparentPass.recordedDrawCommands.size(), 1u);
    EXPECT_EQ(transparentPass.recordedDrawCommands[0].pipeline, transparentPipeline);
    EXPECT_EQ(transparentPass.drawCount, 1u);
    EXPECT_TRUE(transparentPass.usesColorAttachment);
    EXPECT_TRUE(transparentPass.usesDepthStencilAttachment);
    EXPECT_FALSE(transparentPass.writesDepthStencilAttachment);
    ASSERT_NE(transparentPass.depthStencilAttachmentView, nullptr);
    EXPECT_EQ(transparentPass.depthStencilAttachmentView, resources.gbufferDepthView);
}

TEST(RenderFrameExecutionTests, DriverRhiWorkerConsumesRenderReadyPackageAndRetiresFrame)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 91u;

    auto result = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    auto sceneResult = NLS::Tests::ObserveNextRenderSceneForTest(driver);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, snapshot, [snapshot](size_t) { return NLS::Render::Context::BuildRenderFramePackage(snapshot); }));

    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryDrainThreadedRendering(driver, false));
    const auto submissionFrame = result.get();
    ASSERT_EQ(sceneResult.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto renderedPackage = sceneResult.get();
    EXPECT_EQ(renderedPackage.frameId, snapshot.frameId);
    EXPECT_EQ(submissionFrame.frameId, snapshot.frameId);
    EXPECT_EQ(submissionFrame.frameId, 91u);
    EXPECT_EQ(NLS::Render::Context::DriverRendererAccess::GetThreadedFrameTelemetry(driver).inFlightFrameCount, 0u);
}

TEST(RenderFrameExecutionTests, DriverWorkersPreserveOwnershipArtifactsAndAttributionThroughRetirement)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 92u;
    snapshot.targetsSwapchain = true;

    auto result = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    auto sceneResult = NLS::Tests::ObserveNextRenderSceneForTest(driver);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, snapshot, [snapshot](size_t) { return NLS::Render::Context::BuildRenderFramePackage(snapshot); }));

    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryDrainThreadedRendering(driver, false));
    const auto submissionFrame = result.get();
    ASSERT_EQ(sceneResult.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto renderedPackage = sceneResult.get();
    EXPECT_EQ(renderedPackage.frameId, snapshot.frameId);
    EXPECT_EQ(submissionFrame.frameId, snapshot.frameId);

    EXPECT_EQ(snapshot.frameId, 92u);
    EXPECT_TRUE(snapshot.targetsSwapchain);

    EXPECT_EQ(renderedPackage.frameId, 92u);
    EXPECT_TRUE(renderedPackage.targetsSwapchain);
    EXPECT_FALSE(renderedPackage.hasVisibleDraws);
    EXPECT_EQ(renderedPackage.visibleDrawCount, 0u);
}

TEST(RenderFrameExecutionTests, DriverWorkersPreserveRendererPreparedAttributionForPreparedSwapchainFrames)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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


    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 124u;
    snapshot.targetsSwapchain = true;

    NLS::Render::Context::RenderScenePackage renderScenePackage;
    renderScenePackage.frameId = 124u;
    renderScenePackage.targetsSwapchain = true;
    renderScenePackage.renderWidth = 128u;
    renderScenePackage.renderHeight = 72u;
    renderScenePackage.frameDataReady = true;
    renderScenePackage.objectDataReady = true;

    auto result = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    auto sceneResult = NLS::Tests::ObserveNextRenderSceneForTest(driver);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, snapshot, [renderScenePackage](size_t) { return renderScenePackage; }));

    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryDrainThreadedRendering(driver, false));
    const auto submissionFrame = result.get();
    ASSERT_EQ(sceneResult.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto renderedPackage = sceneResult.get();
    EXPECT_EQ(renderedPackage.frameId, snapshot.frameId);
    EXPECT_EQ(submissionFrame.frameId, snapshot.frameId);
    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    EXPECT_EQ(explicitDevice->GetTestQueue()->presentCalls, 1u);
    EXPECT_EQ(swapchain->acquireCalls, 1u);
}

TEST(RenderFrameExecutionTests, RenderCommandDrainReportsRendererAndSubmissionAttribution)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 211u;

    auto result = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    auto sceneResult = NLS::Tests::ObserveNextRenderSceneForTest(driver);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, snapshot, [snapshot](size_t) { return NLS::Render::Context::BuildRenderFramePackage(snapshot); }));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryDrainThreadedRendering(driver, false));
    const auto submissionFrame = result.get();
    ASSERT_EQ(sceneResult.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto renderedPackage = sceneResult.get();
    EXPECT_EQ(renderedPackage.frameId, snapshot.frameId);
    EXPECT_EQ(submissionFrame.frameId, snapshot.frameId);
}

TEST(RenderFrameExecutionTests, SynchronousDrainAppliesPendingSwapchainResizeAfterFrameRetires)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 212u;
    snapshot.targetsSwapchain = true;

    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, snapshot, [snapshot](size_t) { return NLS::Render::Context::BuildRenderFramePackage(snapshot); }));

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

TEST(RenderFrameExecutionTests, ThreadedSubmitFailureSkipsPresentAndRetirementFenceWait)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 213u;
    snapshot.targetsSwapchain = true;

    NLS::Render::Context::RenderScenePackage renderScenePackage;
    renderScenePackage.frameId = snapshot.frameId;
    renderScenePackage.targetsSwapchain = true;
    renderScenePackage.renderWidth = 128u;
    renderScenePackage.renderHeight = 72u;
    renderScenePackage.frameDataReady = true;
    renderScenePackage.objectDataReady = true;

    auto result = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, snapshot, [renderScenePackage](size_t) { return renderScenePackage; }));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    auto testQueue = explicitDevice->GetTestQueue();
    EXPECT_EQ(testQueue->submitCalls, 0u);
    EXPECT_EQ(testQueue->presentCalls, 0u);
    EXPECT_EQ(swapchain->acquireCalls, 1u);
    EXPECT_EQ(frameFence->waitCalls, 0u);

    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto submissionFrame = result.get();
    EXPECT_FALSE(submissionFrame.retirementFenceWaited);
    EXPECT_TRUE(submissionFrame.deviceLostDetected);
    EXPECT_NE(
        submissionFrame.lastQueueOperationFailure.find("threaded submit failed"),
        std::string::npos);
}

TEST(RenderFrameExecutionTests, RenderThreadServicesCommandsWhileExecutorTranslatesAProductionFrame)
{
    using namespace NLS::Render;
    Settings::DriverSettings settings;
    settings.graphicsBackend = Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;
    settings.rhiThreadMode = Settings::RhiThreadModePreference::ForceNone;
    Context::Driver driver(settings);
    auto device = std::make_shared<TestExplicitDevice>();
    device->MutableCapabilities().supportsParallelCommandRecording = true;
    device->MutableCapabilities().supportsParallelCommandTranslation = true;
    std::promise<void> translating;
    std::promise<void> release;
    auto started = translating.get_future();
    auto released = release.get_future().share();
    device->createdCommandBufferBegin = [&translating, released]
    {
        EXPECT_FALSE(Context::RenderingThreadSystem::IsInRenderingThread());
        EXPECT_FALSE(Context::RenderingThreadSystem::IsInRhiThread());
        translating.set_value();
        (void)released.wait_for(std::chrono::seconds(3));
    };
    Context::DriverTestAccess::SetExplicitDevice(driver, device);
    auto* impl = Context::DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl->taskGraph, nullptr);
    impl->rhiThreadMode = Context::RhiThreadMode::Tasks;
    Context::RhiCommandListExecutor::Get().Initialize(Context::RhiThreadMode::Tasks,
        Context::RhiCommandListExecutor::CreateDeviceTargets(device), impl->taskGraph, &driver);

    Context::RenderFramePacket snapshot;
    snapshot.frameId = 120u;
    snapshot.targetsSwapchain = false;
    Context::RenderScenePackage package;
    package.frameId = snapshot.frameId;
    package.targetsSwapchain = false;
    package.visibleDrawCount = 1u;
    package.hasVisibleDraws = true;
    package.frameDataReady = true;
    package.objectDataReady = true;
    package.renderWidth = 64u;
    package.renderHeight = 64u;
    Context::RenderPassCommandInput pass;
    pass.kind = Context::RenderPassCommandKind::Opaque;
    pass.debugName = "BlockedNativeTranslation";
    pass.drawCount = 1u;
    pass.targetsSwapchain = false;
    pass.renderWidth = 64u;
    pass.renderHeight = 64u;
    pass.recordedDrawCommands.push_back({std::make_shared<TestGraphicsPipeline>("Opaque"),
        nullptr, nullptr, nullptr, std::make_shared<TestBindingSet>("Material"), std::make_shared<TestMesh>(), 1u});
    package.passCommandInputs.push_back(std::move(pass));
    package.containsCommandInputs = true;
    EXPECT_TRUE(Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, snapshot, [package](size_t) { return package; }));
    EXPECT_EQ(started.wait_for(std::chrono::seconds(2)), std::future_status::ready);

    Context::RenderCommandEvent marker;
    auto markerOnRender = std::make_shared<std::atomic_bool>(false);
    Context::RenderCommandDispatcher::Enqueue([markerOnRender]
    {
        markerOnRender->store(Context::RenderingThreadSystem::IsInRenderingThread());
    }, marker);
    Context::Detail::NotifyThreadedWorkers(*impl);
    EXPECT_TRUE(marker.WaitFor(std::chrono::seconds(1)));
    EXPECT_TRUE(markerOnRender->load());
    EXPECT_EQ(device->GetTestQueue()->submitCalls, 0u);
    release.set_value();
    EXPECT_TRUE(Context::DriverTestAccess::TryDrainThreadedRendering(driver));
    EXPECT_EQ(device->GetTestQueue()->submitCalls, 1u);
    const auto telemetry = Context::DriverRendererAccess::GetThreadedFrameTelemetry(driver);
    EXPECT_EQ(telemetry.successfullyRetiredFrameCount, 1u);
}

TEST(RenderFrameExecutionTests, DriverRhiWorkerSubmitsAndPresentsSwapchainFramesOnExplicitQueue)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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


    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 121u;
    snapshot.targetsSwapchain = true;

    auto result = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    auto sceneResult = NLS::Tests::ObserveNextRenderSceneForTest(driver);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, snapshot, [snapshot](size_t) { return NLS::Render::Context::BuildRenderFramePackage(snapshot); }));

    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryDrainThreadedRendering(driver, false));
    const auto submissionFrame = result.get();
    ASSERT_EQ(sceneResult.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto renderedPackage = sceneResult.get();
    EXPECT_EQ(renderedPackage.frameId, snapshot.frameId);
    EXPECT_EQ(submissionFrame.frameId, snapshot.frameId);
    EXPECT_FALSE(submissionFrame.offscreenOnly);
    EXPECT_EQ(submissionFrame.frameContextIndex, 0u);
    EXPECT_FALSE(submissionFrame.retirementFenceWaited);
    EXPECT_TRUE(submissionFrame.deferredFrameScopedRetirement);
    EXPECT_EQ(frameFence->waitCalls, 0u);
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
    EXPECT_FALSE(submissionFrame.usedSerialCommandPath);
    EXPECT_FALSE(submissionFrame.usedParallelCommandPath);
    EXPECT_EQ(submissionFrame.parallelRecordingWorkerCount, 0u);
    EXPECT_EQ(submissionFrame.translatedWorkUnitCount, 0u);
    EXPECT_TRUE(explicitDevice->GetCreatedCommandPools().empty());
    ASSERT_EQ(explicitDevice->GetTestQueue()->lastSubmitDesc.commandBuffers.size(), 1u);
    EXPECT_EQ(explicitDevice->GetTestQueue()->lastSubmitDesc.commandBuffers.front(), commandBuffer);
}

TEST(RenderFrameExecutionTests, ThreadedMainlinePresentDoesNotRequireMainThreadPresentHandshake)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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


    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 122u;
    snapshot.targetsSwapchain = true;

    auto result = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    auto sceneResult = NLS::Tests::ObserveNextRenderSceneForTest(driver);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, snapshot, [snapshot](size_t) { return NLS::Render::Context::BuildRenderFramePackage(snapshot); }));

    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryDrainThreadedRendering(driver, false));
    const auto submissionFrame = result.get();
    ASSERT_EQ(sceneResult.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto renderedPackage = sceneResult.get();
    EXPECT_EQ(renderedPackage.frameId, snapshot.frameId);
    EXPECT_EQ(submissionFrame.frameId, snapshot.frameId);
    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    EXPECT_EQ(explicitDevice->GetTestQueue()->presentCalls, 1u);
    EXPECT_EQ(swapchain->acquireCalls, 1u);

    NLS::Render::Context::DriverUIAccess::PresentSwapchain(driver);

    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    EXPECT_EQ(explicitDevice->GetTestQueue()->presentCalls, 1u);
    EXPECT_EQ(swapchain->acquireCalls, 1u);
}

TEST(RenderFrameExecutionTests, DriverRhiWorkerSkipsRejectedHarnessOffscreenFramesWithoutAcquireOrPresent)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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


    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 131u;
    snapshot.targetsSwapchain = false;

    auto result = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    auto sceneResult = NLS::Tests::ObserveNextRenderSceneForTest(driver);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, snapshot, [snapshot](size_t) { return NLS::Render::Context::BuildRenderFramePackage(snapshot); }));

    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryDrainThreadedRendering(driver, false));
    const auto submissionFrame = result.get();
    ASSERT_EQ(sceneResult.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto renderedPackage = sceneResult.get();
    EXPECT_EQ(renderedPackage.frameId, snapshot.frameId);
    EXPECT_EQ(submissionFrame.frameId, snapshot.frameId);
    EXPECT_TRUE(submissionFrame.offscreenOnly);
    EXPECT_EQ(frameFence->waitCalls, 0u);
    EXPECT_EQ(swapchain->acquireCalls, 0u);
    EXPECT_FALSE(frameContext.hasAcquiredSwapchainImage);
    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 0u);
    EXPECT_EQ(explicitDevice->GetTestQueue()->presentCalls, 0u);
    auto submittedCommandBuffer = GetSubmittedTestCommandBuffer(explicitDevice->GetTestQueue());
    EXPECT_EQ(submittedCommandBuffer, nullptr);
}

TEST(RenderFrameExecutionTests, RejectedOffscreenFrameFinalizesContextBeforeNextSubmission)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 2u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
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

    NLS::Render::Context::RenderFramePacket rejectedSnapshot;
    rejectedSnapshot.frameId = 131u;
    rejectedSnapshot.targetsSwapchain = false;

    NLS::Render::Context::RenderScenePackage rejectedPackage;
    rejectedPackage.frameId = rejectedSnapshot.frameId;
    rejectedPackage.targetsSwapchain = false;
    rejectedPackage.visibleDrawCount = 1u;
    rejectedPackage.hasVisibleDraws = true;
    rejectedPackage.passPlanCount = 1u;
    NLS::Render::Context::RenderPassCommandInput rejectedPass;
    rejectedPass.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    rejectedPass.drawCount = 1u;
    rejectedPass.requiresFrameData = true;
    rejectedPass.requiresObjectData = true;
    rejectedPass.targetsSwapchain = false;
    rejectedPackage.passCommandInputs.push_back(rejectedPass);
    size_t rejectedSlot = 99u;
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, rejectedSnapshot, [rejectedPackage](size_t) { return rejectedPackage; }, &rejectedSlot));
    ASSERT_EQ(rejectedSlot, 0u);
    explicitDevice->SetNativeBackendType(NLS::Render::RHI::NativeBackendType::None);
    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 0u);
    EXPECT_EQ(firstDescriptorAllocator->beginFrameCalls, 1u);
    EXPECT_EQ(firstDescriptorAllocator->endFrameCalls, 1u);
    EXPECT_EQ(firstUploadContext->beginFrameCalls, 1u);
    EXPECT_EQ(firstUploadContext->endFrameCalls, 1u);
    EXPECT_EQ(firstFrameFence->resetCalls, 0u)
        << "Offscreen frames with no GPU submit must not leave a reset unsignaled fence behind.";

    auto telemetry = NLS::Render::Context::DriverRendererAccess::GetThreadedFrameTelemetry(driver);
    ASSERT_EQ(telemetry.latestRetiredFrameId, 0u);
    ASSERT_EQ(telemetry.latestFailedRetiredFrameId, rejectedSnapshot.frameId);
    const auto* impl = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl->frameResourceSlots, nullptr);
    EXPECT_TRUE(impl->frameResourceSlots->IsSlotAvailable(rejectedSlot));

    NLS::Render::Context::RenderFramePacket submittedSnapshot;
    submittedSnapshot.frameId = 132u;
    submittedSnapshot.targetsSwapchain = false;

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
    size_t submittedSlot = 99u;
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, submittedSnapshot, [submittedPackage](size_t) { return submittedPackage; },
        &submittedSlot));
    ASSERT_LT(submittedSlot, 2u);
    explicitDevice->SetNativeBackendType(NLS::Render::RHI::NativeBackendType::None);
    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    auto submittedCommandBuffer = GetSubmittedTestCommandBuffer(explicitDevice->GetTestQueue());
    const std::array commandBuffers { firstCommandBuffer, secondCommandBuffer };
    EXPECT_EQ(submittedCommandBuffer, commandBuffers[submittedSlot])
        << "RHI submission must use the published resource lease.";
    EXPECT_EQ(NLS::Render::Context::DriverTestAccess::PeekFrameContext(driver, submittedSlot)->frameIndex, 1u);

    telemetry = NLS::Render::Context::DriverRendererAccess::GetThreadedFrameTelemetry(driver);
    EXPECT_EQ(telemetry.latestRetiredFrameId, submittedSnapshot.frameId);
    EXPECT_EQ(telemetry.latestFailedRetiredFrameId, rejectedSnapshot.frameId);
}

TEST(RenderFrameExecutionTests, SynchronousDrainCannotEnterRhiSubmissionWhileAnotherSubmissionIsActive)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 2u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto blockingDescriptorAllocator = std::make_shared<BlockingBeginFrameDescriptorAllocator>();
    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.descriptorAllocator = blockingDescriptorAllocator;
    NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 1u).descriptorAllocator =
        blockingDescriptorAllocator;

    NLS::Render::Context::RenderFramePacket firstSnapshot;
    firstSnapshot.frameId = 711u;
    firstSnapshot.targetsSwapchain = true;
    firstSnapshot.renderWidth = 64u;
    firstSnapshot.renderHeight = 64u;

    NLS::Render::Context::RenderScenePackage firstPackage;
    firstPackage.frameId = firstSnapshot.frameId;
    firstPackage.targetsSwapchain = true;
    firstPackage.renderWidth = 64u;
    firstPackage.renderHeight = 64u;

    NLS::Render::Context::RenderFramePacket secondSnapshot = firstSnapshot;
    secondSnapshot.frameId = 712u;
    NLS::Render::Context::RenderScenePackage secondPackage = firstPackage;
    secondPackage.frameId = secondSnapshot.frameId;

    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, firstSnapshot, [firstPackage](size_t) { return firstPackage; }));
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, secondSnapshot, [secondPackage](size_t) { return secondPackage; }));

    std::thread firstDrain([&driver]()
    {
        NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);
    });

    const bool firstStarted = blockingDescriptorAllocator->WaitForFirstBeginFrame(std::chrono::seconds(2));
    EXPECT_TRUE(firstStarted);
    std::thread secondDrain;
    if (firstStarted)
    {
        secondDrain = std::thread([&driver]()
        {
            NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);
        });
        EXPECT_FALSE(blockingDescriptorAllocator->WaitForConcurrentBeginFrame(std::chrono::milliseconds(50)));
    }

    blockingDescriptorAllocator->ReleaseFirstBeginFrame();
    if (firstDrain.joinable())
        firstDrain.join();
    if (secondDrain.joinable())
        secondDrain.join();

    EXPECT_EQ(blockingDescriptorAllocator->GetMaxConcurrentBeginFrameCalls(), 1u);
}

TEST(RenderFrameExecutionTests, DriverRhiWorkerTransitionsExtractedOffscreenTexturesToShaderRead)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 611u;
    snapshot.targetsSwapchain = false;

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

    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, snapshot, [package](size_t) { return package; }));

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

TEST(RenderFrameExecutionTests, RhiWorkerMarksVisiblePackageAsRecordedBeforeSubmit)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 401u;
    snapshot.targetsSwapchain = false;

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

    auto result = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, snapshot, [package](size_t) { return package; }));


    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);
    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto submissionFrame = result.get();
    EXPECT_TRUE(submissionFrame.recordedVisibleWork);
    EXPECT_EQ(submissionFrame.recordedDrawCount, 1u);
    EXPECT_EQ(submissionFrame.recordedPassCount, 1u);
    auto submittedCommandBuffer = GetSubmittedTestCommandBuffer(explicitDevice->GetTestQueue());
    ASSERT_NE(submittedCommandBuffer, nullptr);
    EXPECT_EQ(submittedCommandBuffer->beginRenderPassCalls, 1u);
    EXPECT_EQ(submittedCommandBuffer->endRenderPassCalls, 1u);
    EXPECT_EQ(submittedCommandBuffer->setViewportCalls, 1u);
}

TEST(RenderFrameExecutionTests, RhiWorkerConsumesPassOwnedRecordedDrawCommandsInsideRenderPass)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 403u;
    snapshot.targetsSwapchain = false;

    auto opaquePipeline = std::make_shared<TestGraphicsPipeline>("OpaquePipeline");
    auto transparentPipeline = std::make_shared<TestGraphicsPipeline>("TransparentPipeline");
    auto materialBindingSet = std::make_shared<TestBindingSet>("MaterialBindingSet");
    auto mesh = std::make_shared<TestMesh>();

    NLS::Render::Context::RenderScenePackage package;
    package.frameId = snapshot.frameId;
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
    auto result = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
        driver, snapshot, [package](size_t) { return package; }));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto submissionFrame = result.get();
    EXPECT_TRUE(submissionFrame.recordedVisibleWork);
    EXPECT_EQ(submissionFrame.recordedPassCount, 2u);
    EXPECT_EQ(submissionFrame.recordedDrawCount, 2u);
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

TEST(RenderFrameExecutionTests, RecordedObjectConstantsRemainImmutableAndVisibleToBothGraphicsStages)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto commandBuffer = std::make_shared<TestCommandBuffer>();
    auto commandPool = std::make_shared<TestCommandPool>();
    commandPool->commandBuffer = commandBuffer;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = std::make_shared<TestFence>();
    frameContext.resourceStateTracker = NLS::Render::RHI::CreateDefaultResourceStateTracker();

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 408u;
    snapshot.targetsSwapchain = false;

    NLS::Render::Data::DrawableObjectDescriptor mutableDescriptor;
    mutableDescriptor.objectIndex = NLS::Render::Data::DrawableObjectDescriptor::kInvalidObjectIndex;
    mutableDescriptor.objectFlags = NLS::Render::Data::kDrawableObjectFlagReceiveShadows;

    NLS::Render::Context::RecordedDrawCommandInput recordedDraw;
    recordedDraw.pipeline = std::make_shared<TestGraphicsPipeline>("ObjectConstantsPipeline");
    recordedDraw.materialBindingSet = std::make_shared<TestBindingSet>("MaterialBindingSet");
    recordedDraw.mesh = std::make_shared<TestMesh>();
    recordedDraw.instanceCount = 1u;
    recordedDraw.objectConstants.objectIndex = mutableDescriptor.objectIndex;
    recordedDraw.objectConstants.objectFlags = mutableDescriptor.objectFlags;
    recordedDraw.usesObjectIndex = false;

    NLS::Render::Context::RenderPassCommandInput passInput;
    passInput.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    passInput.drawCount = 1u;
    passInput.targetsSwapchain = false;
    passInput.renderWidth = 64u;
    passInput.renderHeight = 64u;
    passInput.clearColor = true;
    passInput.clearDepth = true;
    passInput.usesColorAttachment = true;
    passInput.usesDepthStencilAttachment = true;
    passInput.recordedDrawCommands.push_back(recordedDraw);

    NLS::Render::Context::RenderScenePackage package;
    package.frameId = snapshot.frameId;
    package.targetsSwapchain = false;
    package.visibleDrawCount = 1u;
    package.opaqueDrawCount = 1u;
    package.hasVisibleDraws = true;
    package.frameDataReady = true;
    package.objectDataReady = true;
    package.renderWidth = 64u;
    package.renderHeight = 64u;
    package.passCommandInputs.push_back(passInput);
    auto result = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
        driver, snapshot, [package](size_t) { return package; }));

    mutableDescriptor.objectFlags = NLS::Render::Data::kDrawableObjectFlagCastShadows;
    recordedDraw.objectConstants.objectFlags = mutableDescriptor.objectFlags;
    package.passCommandInputs[0].recordedDrawCommands[0].objectConstants.objectFlags = mutableDescriptor.objectFlags;

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);
    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);

    auto submittedCommandBuffer = GetSubmittedTestCommandBuffer(explicitDevice->GetTestQueue());
    ASSERT_NE(submittedCommandBuffer, nullptr);
    ASSERT_EQ(submittedCommandBuffer->lastPushConstantsBytes.size(),
        sizeof(NLS::Render::Data::ObjectDrawConstants));
    EXPECT_EQ(submittedCommandBuffer->lastPushConstantsSize,
        sizeof(NLS::Render::Data::ObjectDrawConstants));
    EXPECT_TRUE(NLS::Render::RHI::HasShaderStage(
        submittedCommandBuffer->lastPushConstantsStageMask,
        NLS::Render::RHI::ShaderStageMask::Vertex));
    EXPECT_TRUE(NLS::Render::RHI::HasShaderStage(
        submittedCommandBuffer->lastPushConstantsStageMask,
        NLS::Render::RHI::ShaderStageMask::Fragment));

    NLS::Render::Data::ObjectDrawConstants capturedConstants;
    std::memcpy(
        &capturedConstants,
        submittedCommandBuffer->lastPushConstantsBytes.data(),
        sizeof(capturedConstants));
    EXPECT_EQ(capturedConstants.objectIndex,
        NLS::Render::Data::DrawableObjectDescriptor::kInvalidObjectIndex);
    EXPECT_EQ(capturedConstants.objectFlags, NLS::Render::Data::kDrawableObjectFlagReceiveShadows);
    EXPECT_EQ(capturedConstants.padding0, 0u);
    EXPECT_EQ(capturedConstants.padding1, 0u);

    NLS::Core::ServiceLocator::Remove<NLS::Render::Context::Driver>();
}

TEST(RenderFrameExecutionTests, SerialCommandPathConsumesPreparedParallelCommandWorkUnits)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 404u;
    snapshot.targetsSwapchain = false;

    auto opaquePipeline = std::make_shared<TestGraphicsPipeline>("OpaquePipeline");
    auto computePipeline = std::make_shared<TestComputePipeline>("LightGridComputePipeline");
    auto materialBindingSet = std::make_shared<TestBindingSet>("MaterialBindingSet");
    auto mesh = std::make_shared<TestMesh>();

    NLS::Render::Context::RenderScenePackage package;
    package.frameId = snapshot.frameId;
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

    auto result = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
        driver, snapshot, [package](size_t) { return package; }));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto submissionFrame = result.get();
    EXPECT_TRUE(submissionFrame.recordedVisibleWork);
    EXPECT_EQ(submissionFrame.recordedPassCount, 2u);
    EXPECT_EQ(submissionFrame.recordedWorkUnitCount, 2u);
    EXPECT_EQ(submissionFrame.parallelRecordingWorkerCount, 0u);
    EXPECT_EQ(submissionFrame.translatedWorkUnitCount, 0u);
    EXPECT_FALSE(submissionFrame.usedParallelCommandPath);
    EXPECT_FALSE(submissionFrame.usedTranslationMerge);
    EXPECT_TRUE(submissionFrame.usedSerialCommandPath);
    EXPECT_EQ(
        submissionFrame.asyncComputeDisposition,
        NLS::Render::Context::AsyncComputeDisposition::Submitted);
    EXPECT_EQ(submissionFrame.asyncComputeCandidateWorkloadCount, 1u);
    EXPECT_TRUE(submissionFrame.asyncComputeQueueAvailable);
    EXPECT_TRUE(submissionFrame.usedAsyncComputeQueueSubmission);
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

TEST(RenderFrameExecutionTests, RhiWorkerRecordsPreparedComputeTextureBarriersAroundDispatch)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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
    frameContext.computeFinishedSemaphore = std::make_shared<TestSemaphore>();
    frameContext.resourceStateTracker = NLS::Render::RHI::CreateDefaultResourceStateTracker();

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 405u;
    snapshot.targetsSwapchain = false;

    NLS::Render::RHI::RHITextureDesc textureDesc;
    textureDesc.debugName = "ThreadedPreparedComputeTexture";
    textureDesc.extent = { 64u, 64u, 1u };
    textureDesc.dimension = NLS::Render::RHI::TextureDimension::Texture2D;
    textureDesc.format = NLS::Render::RHI::TextureFormat::R32F;
    textureDesc.usage =
        NLS::Render::RHI::TextureUsageFlags::Sampled |
        NLS::Render::RHI::TextureUsageFlags::Storage;
    auto texture = explicitDevice->CreateTexture(textureDesc);
    auto computePipeline = std::make_shared<TestComputePipeline>("PreparedTextureComputePipeline");

    NLS::Render::RHI::RHISubresourceRange range;
    range.baseMipLevel = 1u;
    range.mipLevelCount = 1u;
    range.baseArrayLayer = 0u;
    range.arrayLayerCount = 1u;

    NLS::Render::Context::RecordedComputeDispatchInput dispatchInput;
    dispatchInput.debugName = "PreparedTextureCompute";
    dispatchInput.pipeline = computePipeline;
    dispatchInput.groupCountX = 4u;
    dispatchInput.groupCountY = 2u;
    dispatchInput.groupCountZ = 1u;
    dispatchInput.textureVisibilityTransitionsBefore.push_back({
        texture,
        range,
        NLS::Render::RHI::ResourceState::Unknown,
        NLS::Render::RHI::ResourceState::ShaderWrite,
        NLS::Render::RHI::PipelineStageMask::AllCommands,
        NLS::Render::RHI::PipelineStageMask::ComputeShader,
        NLS::Render::RHI::AccessMask::MemoryRead | NLS::Render::RHI::AccessMask::MemoryWrite,
        NLS::Render::RHI::AccessMask::ShaderWrite
    });
    dispatchInput.exportedTextureVisibilityTransitions.push_back({
        texture,
        range,
        NLS::Render::RHI::ResourceState::ShaderWrite,
        NLS::Render::RHI::ResourceState::ShaderRead,
        NLS::Render::RHI::PipelineStageMask::ComputeShader,
        NLS::Render::RHI::PipelineStageMask::ComputeShader,
        NLS::Render::RHI::AccessMask::ShaderWrite,
        NLS::Render::RHI::AccessMask::ShaderRead
    });

    NLS::Render::Context::RenderPassCommandInput computePassInput;
    computePassInput.kind = NLS::Render::Context::RenderPassCommandKind::Compute;
    computePassInput.debugName = "PreparedTextureCompute";
    computePassInput.queueType = NLS::Render::RHI::QueueType::Compute;
    computePassInput.renderWidth = 64u;
    computePassInput.renderHeight = 64u;
    computePassInput.computeDispatchInputs.push_back(dispatchInput);

    NLS::Render::Context::ParallelCommandWorkUnit computeWorkUnit;
    computeWorkUnit.commandInput = computePassInput;
    computeWorkUnit.debugName = computePassInput.debugName;

    NLS::Render::Context::RenderScenePackage package;
    package.frameId = snapshot.frameId;
    package.targetsSwapchain = false;
    package.visibleDrawCount = 1u;
    package.hasVisibleDraws = true;
    package.frameDataReady = true;
    package.objectDataReady = true;
    package.renderWidth = 64u;
    package.renderHeight = 64u;
    package.containsParallelCommandWorkUnits = true;
    package.parallelCommandWorkUnitCount = 1u;
    package.parallelCommandWorkUnits = { computeWorkUnit };
    package.hasAsyncComputeWorkload = true;
    package.asyncComputeWorkloadCount = 1u;

    auto result = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
        driver, snapshot, [package](size_t) { return package; }));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);
    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);

    ASSERT_EQ(explicitDevice->GetComputeTestQueue()->submitCalls, 1u);
    ASSERT_EQ(explicitDevice->GetComputeTestQueue()->lastSubmitDesc.commandBuffers.size(), 1u);
    const auto computeSubmittedCommandBuffer = std::dynamic_pointer_cast<TestCommandBuffer>(
        explicitDevice->GetComputeTestQueue()->lastSubmitDesc.commandBuffers.front());
    ASSERT_NE(computeSubmittedCommandBuffer, nullptr);

    EXPECT_EQ(
        computeSubmittedCommandBuffer->events,
        std::vector<std::string>({ "Begin", "Barrier", "Dispatch", "Barrier", "End" }));
    ASSERT_EQ(computeSubmittedCommandBuffer->barrierHistory.size(), 2u);
    ASSERT_EQ(computeSubmittedCommandBuffer->barrierHistory[0].textureBarriers.size(), 1u);
    EXPECT_EQ(computeSubmittedCommandBuffer->barrierHistory[0].textureBarriers[0].texture, texture);
    EXPECT_EQ(computeSubmittedCommandBuffer->barrierHistory[0].textureBarriers[0].subresourceRange.baseMipLevel, 1u);
    EXPECT_EQ(computeSubmittedCommandBuffer->barrierHistory[0].textureBarriers[0].before, NLS::Render::RHI::ResourceState::Unknown);
    EXPECT_EQ(computeSubmittedCommandBuffer->barrierHistory[0].textureBarriers[0].after, NLS::Render::RHI::ResourceState::ShaderWrite);
    EXPECT_EQ(computeSubmittedCommandBuffer->barrierHistory[0].textureBarriers[0].destinationAccessMask, NLS::Render::RHI::AccessMask::ShaderWrite);
    ASSERT_EQ(computeSubmittedCommandBuffer->barrierHistory[1].textureBarriers.size(), 1u);
    EXPECT_EQ(computeSubmittedCommandBuffer->barrierHistory[1].textureBarriers[0].texture, texture);
    EXPECT_EQ(computeSubmittedCommandBuffer->barrierHistory[1].textureBarriers[0].subresourceRange.baseMipLevel, 1u);
    EXPECT_EQ(computeSubmittedCommandBuffer->barrierHistory[1].textureBarriers[0].before, NLS::Render::RHI::ResourceState::ShaderWrite);
    EXPECT_EQ(computeSubmittedCommandBuffer->barrierHistory[1].textureBarriers[0].after, NLS::Render::RHI::ResourceState::ShaderRead);
    EXPECT_EQ(computeSubmittedCommandBuffer->barrierHistory[1].textureBarriers[0].sourceAccessMask, NLS::Render::RHI::AccessMask::ShaderWrite);
    EXPECT_EQ(computeSubmittedCommandBuffer->barrierHistory[1].textureBarriers[0].destinationAccessMask, NLS::Render::RHI::AccessMask::ShaderRead);
}

TEST(RenderFrameExecutionTests, AsyncComputeSchedulingBuildsGraphicsComputeGraphicsSubmissionChain)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 405u;
    snapshot.targetsSwapchain = false;

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
    package.frameId = snapshot.frameId;
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

    auto result = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
        driver, snapshot, [package](size_t) { return package; }));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto submissionFrame = result.get();
    EXPECT_TRUE(submissionFrame.usedAsyncComputeQueueSubmission);
    EXPECT_EQ(
        submissionFrame.asyncComputeDisposition,
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

TEST(RenderFrameExecutionTests, AsyncComputeSchedulingAllowsNonAdjacentGraphicsConsumerToWaitOnLastComputeBatch)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 406u;
    snapshot.targetsSwapchain = false;

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
    package.frameId = snapshot.frameId;
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

    auto result = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
        driver, snapshot, [package](size_t) { return package; }));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto submissionFrame = result.get();
    EXPECT_TRUE(submissionFrame.usedAsyncComputeQueueSubmission);
    EXPECT_EQ(
        submissionFrame.asyncComputeDisposition,
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

TEST(RenderFrameExecutionTests, ParallelRecordingUsesMultipleWorkersForEligibleWorkUnits)
{
    ScopedThreadedRenderingJobSystem jobSystem(2u);
    ASSERT_TRUE(jobSystem.IsInitialized());

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    explicitDevice->MutableCapabilities().supportsParallelCommandRecording = true;
    std::atomic_uint32_t nativeTranslations {0u};
    explicitDevice->createdCommandBufferBegin = [&nativeTranslations]
    {
        nativeTranslations.fetch_add(1u);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (nativeTranslations.load() < 2u && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        EXPECT_EQ(nativeTranslations.load(), 2u);
    };
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

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 405u;
    snapshot.targetsSwapchain = false;

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
    package.frameId = snapshot.frameId;
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

    auto result = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
        driver, snapshot, [package](size_t) { return package; }));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto submissionFrame = result.get();
    EXPECT_TRUE(submissionFrame.recordedVisibleWork);
    EXPECT_EQ(submissionFrame.recordedPassCount, 2u);
    EXPECT_EQ(submissionFrame.recordedWorkUnitCount, 2u);
    EXPECT_EQ(submissionFrame.parallelRecordingWorkerCount, 2u);
    EXPECT_EQ(submissionFrame.translatedWorkUnitCount, 0u);
    EXPECT_TRUE(submissionFrame.usedParallelCommandPath);
    EXPECT_FALSE(submissionFrame.usedTranslationMerge);
    EXPECT_FALSE(submissionFrame.usedSerialCommandPath);
    ASSERT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    EXPECT_EQ(explicitDevice->GetTestQueue()->lastSubmitDesc.commandBuffers.size(), 2u);
    EXPECT_EQ(explicitDevice->GetCreatedCommandPools().size(), 2u);
    for (const auto& submittedBuffer : explicitDevice->GetTestQueue()->lastSubmitDesc.commandBuffers)
        EXPECT_NE(submittedBuffer, frameContext.commandBuffer);
}

TEST(RenderFrameExecutionTests, ParallelCommandPoolsRecreateWhenAFrameSlotChangesQueueType)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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
    frameFence->signaled = true;
    commandPool->commandBuffer = commandBuffer;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = frameFence;
    frameContext.resourceStateTracker = nullptr;

    auto computePipeline = std::make_shared<TestComputePipeline>("QueueTypeSwitchCompute");
    auto graphicsPipeline = std::make_shared<TestGraphicsPipeline>("QueueTypeSwitchGraphics");
    auto materialBindingSet = std::make_shared<TestBindingSet>("QueueTypeSwitchBindings");
    auto mesh = std::make_shared<TestMesh>();

    const auto buildPackage = [&](const uint64_t frameId, const bool compute)
    {
        NLS::Render::Context::RenderFramePacket snapshot;
        snapshot.frameId = frameId;
        snapshot.targetsSwapchain = false;

        NLS::Render::Context::RenderPassCommandInput passInput;
        passInput.kind = compute
            ? NLS::Render::Context::RenderPassCommandKind::Compute
            : NLS::Render::Context::RenderPassCommandKind::Opaque;
        passInput.debugName = compute ? "QueueTypeSwitchCompute" : "QueueTypeSwitchGraphics";
        passInput.queueType = compute
            ? NLS::Render::RHI::QueueType::Compute
            : NLS::Render::RHI::QueueType::Graphics;
        passInput.renderWidth = 64u;
        passInput.renderHeight = 64u;
        if (compute)
        {
            passInput.computeDispatchInputs.push_back({
                "QueueTypeSwitchCompute",
                computePipeline,
                {},
                1u,
                1u,
                1u,
                {},
                {},
                {}
            });
        }
        else
        {
            passInput.drawCount = 1u;
            passInput.requiresFrameData = true;
            passInput.requiresObjectData = true;
            passInput.clearColor = true;
            passInput.clearDepth = true;
            passInput.usesColorAttachment = true;
            passInput.usesDepthStencilAttachment = true;
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

        NLS::Render::Context::ParallelCommandWorkUnit workUnit;
        workUnit.commandInput = passInput;
        workUnit.debugName = passInput.debugName;

        NLS::Render::Context::RenderScenePackage package;
        package.frameId = frameId;
        package.targetsSwapchain = false;
        package.visibleDrawCount = compute ? 0u : 1u;
        package.opaqueDrawCount = compute ? 0u : 1u;
        package.hasVisibleDraws = true;
        package.frameDataReady = true;
        package.objectDataReady = true;
        package.renderWidth = 64u;
        package.renderHeight = 64u;
        package.passCommandInputs = { passInput };
        package.containsParallelCommandWorkUnits = true;
        package.parallelCommandWorkUnits = { workUnit };
        package.parallelCommandWorkUnitCount = 1u;
        return std::pair{ snapshot, package };
    };

    auto [computeSnapshot, computePackage] = buildPackage(618u, true);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, computeSnapshot, [computePackage](size_t) { return computePackage; }));
    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    ASSERT_EQ(frameContext.parallelCommandPools.size(), 1u);
    auto firstPool = std::dynamic_pointer_cast<TestCommandPool>(frameContext.parallelCommandPools[0]);
    ASSERT_NE(firstPool, nullptr);
    EXPECT_EQ(firstPool->GetQueueType(), NLS::Render::RHI::QueueType::Compute);
    auto firstBuffer = frameContext.parallelCommandBuffers[0];
    ASSERT_NE(firstBuffer, nullptr);

    frameFence->signaled = true;
    auto [graphicsSnapshot, graphicsPackage] = buildPackage(619u, false);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, graphicsSnapshot, [graphicsPackage](size_t) { return graphicsPackage; }));
    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    const auto createdPools = explicitDevice->GetCreatedCommandPools();
    ASSERT_EQ(createdPools.size(), 2u);
    EXPECT_EQ(createdPools[0]->GetQueueType(), NLS::Render::RHI::QueueType::Compute);
    EXPECT_EQ(createdPools[1]->GetQueueType(), NLS::Render::RHI::QueueType::Graphics);
    auto currentPool = std::dynamic_pointer_cast<TestCommandPool>(frameContext.parallelCommandPools[0]);
    ASSERT_NE(currentPool, nullptr);
    EXPECT_EQ(currentPool->GetQueueType(), NLS::Render::RHI::QueueType::Graphics);
    EXPECT_NE(frameContext.parallelCommandBuffers[0], firstBuffer);
}

TEST(RenderFrameExecutionTests, GatedAsyncComputeRecordsGraphicsListBeforeSubmitting)
{
    using namespace NLS::Render;
    Settings::DriverSettings settings;
    settings.graphicsBackend = Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;
    Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto device = std::make_shared<TestExplicitDevice>();
    device->MutableCapabilities().supportsAsyncCompute = false;
    device->MutableCapabilities().supportsDedicatedComputeQueue = true;
    Context::DriverTestAccess::SetExplicitDevice(driver, device);
    auto& frame = Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto pool = std::make_shared<TestCommandPool>();
    auto buffer = std::make_shared<TestCommandBuffer>();
    pool->commandBuffer = buffer;
    frame.commandPool = pool;
    frame.commandBuffer = buffer;
    frame.frameFence = std::make_shared<TestFence>();
    frame.resourceStateTracker = RHI::CreateDefaultResourceStateTracker();

    Context::RenderPassCommandInput pass;
    pass.kind = Context::RenderPassCommandKind::Compute;
    pass.queueType = RHI::QueueType::Compute;
    pass.debugName = "GatedAsyncCompute";
    pass.computeDispatchInputs.push_back({"GatedDispatch",
        std::make_shared<TestComputePipeline>("GatedPipeline"), {}, 1u, 1u, 1u, {}, {}, {}});
    Context::ParallelCommandWorkUnit work;
    work.commandInput = pass;
    Context::RenderFramePacket snapshot;
    snapshot.frameId = 620u;
    snapshot.targetsSwapchain = false;
    Context::RenderScenePackage package;
    package.frameId = snapshot.frameId;
    package.targetsSwapchain = false;
    package.frameDataReady = package.objectDataReady = true;
    package.containsParallelCommandWorkUnits = true;
    package.passCommandInputs = {pass};
    package.parallelCommandWorkUnits = {work};
    package.parallelCommandWorkUnitCount = 1u;
    ASSERT_TRUE(Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, snapshot, [package](size_t) { return package; }));
    ASSERT_TRUE(Context::DriverTestAccess::TryDrainThreadedRendering(driver));
    ASSERT_EQ(frame.parallelCommandPools.size(), 1u);
    EXPECT_EQ(frame.parallelCommandPools.front()->GetQueueType(), RHI::QueueType::Graphics);
    EXPECT_EQ(device->GetComputeTestQueue()->submitCalls, 0u);
    EXPECT_EQ(device->GetTestQueue()->submitCalls, 1u);
    const auto submitted = GetSubmittedTestCommandBuffer(device->GetTestQueue());
    ASSERT_NE(submitted, nullptr);
    EXPECT_EQ(submitted->dispatchCalls, 1u);
}

TEST(RenderFrameExecutionTests, ParallelRecordingPlansTrackedResourcesBeforeNativeTranslation)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    explicitDevice->MutableCapabilities().supportsParallelCommandRecording = true;
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

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 406u;
    snapshot.targetsSwapchain = false;

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
    opaqueWorkUnit.eligibleForParallelTranslation = true;

    NLS::Render::Context::ParallelCommandWorkUnit transparentWorkUnit;
    transparentWorkUnit.commandInput = transparentPassInput;
    transparentWorkUnit.debugName = "TrackedTransparent";
    transparentWorkUnit.eligibleForParallelRecording = true;
    transparentWorkUnit.eligibleForParallelTranslation = true;

    NLS::Render::Context::RenderScenePackage package;
    package.frameId = snapshot.frameId;
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

    auto result = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
        driver, snapshot, [package](size_t) { return package; }));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto submissionFrame = result.get();
    EXPECT_TRUE(submissionFrame.recordedVisibleWork);
    EXPECT_EQ(submissionFrame.recordedPassCount, 2u);
    EXPECT_EQ(submissionFrame.recordedWorkUnitCount, 2u);
    EXPECT_GT(submissionFrame.parallelRecordingWorkerCount, 0u);
    EXPECT_EQ(submissionFrame.translatedWorkUnitCount, 2u);
    EXPECT_TRUE(submissionFrame.usedParallelCommandPath);
    EXPECT_TRUE(submissionFrame.usedTranslationMerge);
    EXPECT_FALSE(submissionFrame.usedSerialCommandPath);
    EXPECT_EQ(explicitDevice->GetCreatedCommandPools().size(), 2u);
}

TEST(RenderFrameExecutionTests, ParallelRecordingFailureMarksSubmissionAsFailedRetirement)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 407u;
    snapshot.targetsSwapchain = false;

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
    package.frameId = snapshot.frameId;
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

    auto result = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
        driver, snapshot, [package](size_t) { return package; }));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto submissionFrame = result.get();
    EXPECT_FALSE(submissionFrame.submittedSuccessfully);
    EXPECT_FALSE(submissionFrame.recordedVisibleWork);
    EXPECT_GT(submissionFrame.commandRecordingFailureCount, 0u);
    EXPECT_NE(
        submissionFrame.lastCommandRecordingFailure.find("Native command list Begin failed"),
        std::string::npos);

    const auto telemetry = NLS::Render::Context::DriverRendererAccess::GetThreadedFrameTelemetry(driver);
    EXPECT_EQ(telemetry.latestRetiredFrameId, 0u);
    EXPECT_EQ(telemetry.latestFailedRetiredFrameId, snapshot.frameId);
}

TEST(RenderFrameExecutionTests, SerialRecordingFailureMarksSubmissionAsFailedRetirement)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 412u;
    snapshot.targetsSwapchain = false;
    snapshot.hasExternalOutput = true;
    snapshot.externalOutputTextureCount = 1u;

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
    package.frameId = snapshot.frameId;
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

    auto result = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
        driver, snapshot, [package](size_t) { return package; }));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto submissionFrame = result.get();
    EXPECT_TRUE(submissionFrame.usedSerialCommandPath);
    EXPECT_FALSE(submissionFrame.submittedSuccessfully);
    EXPECT_GT(submissionFrame.commandRecordingFailureCount, 0u);
    EXPECT_NE(
        submissionFrame.lastCommandRecordingFailure.find("No draws recorded but commands were expected"),
        std::string::npos);
    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 0u);

    const auto telemetry = NLS::Render::Context::DriverRendererAccess::GetThreadedFrameTelemetry(driver);
    EXPECT_EQ(telemetry.latestRetiredFrameId, 0u);
    EXPECT_EQ(telemetry.latestFailedRetiredFrameId, snapshot.frameId);
}

TEST(RenderFrameExecutionTests, PreparationFailureBlocksPreviouslyPreparedSubmit)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 414u;
    snapshot.targetsSwapchain = false;

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
    package.frameId = snapshot.frameId;
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

    auto result = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
        driver, snapshot, [package](size_t) { return package; }));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto submissionFrame = result.get();
    EXPECT_FALSE(submissionFrame.usedParallelCommandPath);
    EXPECT_FALSE(submissionFrame.submittedSuccessfully);
    EXPECT_FALSE(submissionFrame.recordedVisibleWork);
    EXPECT_EQ(submissionFrame.recordedPassCount, 1u);
    EXPECT_EQ(submissionFrame.recordedDrawCount, 1u);
    EXPECT_GT(submissionFrame.commandRecordingFailureCount, 0u);
    EXPECT_NE(
        submissionFrame.lastCommandRecordingFailure.find("No draws recorded but commands were expected"),
        std::string::npos);
    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 0u);
    EXPECT_EQ(frameFence->waitCalls, 0u);

    const auto telemetry = NLS::Render::Context::DriverRendererAccess::GetThreadedFrameTelemetry(driver);
    EXPECT_EQ(telemetry.latestRetiredFrameId, 0u);
    EXPECT_EQ(telemetry.latestFailedRetiredFrameId, snapshot.frameId);
}

TEST(RenderFrameExecutionTests, ExternalSceneOutputUsesIndependentNativeCommandLists)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 410u;
    snapshot.targetsSwapchain = false;
    snapshot.hasExternalOutput = true;
    snapshot.externalOutputTextureCount = 2u;

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
    package.frameId = snapshot.frameId;
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

    auto result = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
        driver, snapshot, [package](size_t) { return package; }));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto submissionFrame = result.get();
    EXPECT_TRUE(submissionFrame.usedExternalOutputBridge);
    EXPECT_EQ(submissionFrame.externalOutputTextureCount, 1u);
    EXPECT_TRUE(submissionFrame.usedParallelCommandPath);
    EXPECT_FALSE(submissionFrame.usedSerialCommandPath);
    EXPECT_GT(submissionFrame.parallelRecordingWorkerCount, 0u);
    EXPECT_EQ(explicitDevice->GetCreatedCommandPools().size(), 3u);
    ASSERT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    const auto& submittedBuffers = explicitDevice->GetTestQueue()->lastSubmitDesc.commandBuffers;
    ASSERT_EQ(submittedBuffers.size(), 3u);
    const auto opaqueBuffer = std::dynamic_pointer_cast<TestCommandBuffer>(submittedBuffers[0]);
    const auto transparentBuffer = std::dynamic_pointer_cast<TestCommandBuffer>(submittedBuffers[1]);
    const auto extractionBuffer = std::dynamic_pointer_cast<TestCommandBuffer>(submittedBuffers[2]);
    ASSERT_NE(opaqueBuffer, nullptr);
    ASSERT_NE(transparentBuffer, nullptr);
    ASSERT_NE(extractionBuffer, nullptr);
    ASSERT_FALSE(opaqueBuffer->lastRenderPassDesc.colorAttachments.empty());
    ASSERT_FALSE(transparentBuffer->lastRenderPassDesc.colorAttachments.empty());
    EXPECT_EQ(opaqueBuffer->lastRenderPassDesc.colorAttachments.front().loadOp, NLS::Render::RHI::LoadOp::Clear);
    EXPECT_EQ(transparentBuffer->lastRenderPassDesc.colorAttachments.front().loadOp, NLS::Render::RHI::LoadOp::Load);
    EXPECT_EQ(extractionBuffer->beginRenderPassCalls, 0u);
    ASSERT_FALSE(extractionBuffer->barrierHistory.empty());
    const auto& extractionBarrier = extractionBuffer->barrierHistory.back();
    EXPECT_TRUE(extractionBarrier.statesAreExplicit);
    ASSERT_FALSE(extractionBarrier.textureBarriers.empty());
    EXPECT_EQ(extractionBarrier.textureBarriers.front().texture, depthTexture);
    EXPECT_EQ(extractionBarrier.textureBarriers.front().after, NLS::Render::RHI::ResourceState::ShaderRead);
    EXPECT_NE(explicitDevice->GetTestQueue()->lastSubmitDesc.commandBuffers.front(), frameContext.commandBuffer);
    EXPECT_EQ(commandBuffer->beginRenderPassCalls, 0u);
}

TEST(RenderFrameExecutionTests, ExternalSceneOutputTelemetryIgnoresPreferredReadbackTexture)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 411u;
    snapshot.targetsSwapchain = false;
    snapshot.hasExternalOutput = true;
    snapshot.externalOutputTextureCount = 1u;

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
    package.frameId = snapshot.frameId;
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

    auto result = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
        driver, snapshot, [package](size_t) { return package; }));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto submissionFrame = result.get();
    EXPECT_TRUE(submissionFrame.usedExternalOutputBridge);
    EXPECT_EQ(submissionFrame.externalOutputTextureCount, 1u);
}

TEST(RenderFrameExecutionTests, SubmissionDiagnosticsCaptureDescriptorAndTransientLifetimeStats)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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
    NLS::Render::Context::DriverTestAccess::SetFrameContextDescriptorAllocator(
        driver,
        0u,
        descriptorAllocator);
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

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 407u;
    snapshot.targetsSwapchain = false;

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
    package.frameId = snapshot.frameId;
    package.targetsSwapchain = false;
    package.visibleDrawCount = 1u;
    package.opaqueDrawCount = 1u;
    package.hasVisibleDraws = true;
    package.frameDataReady = true;
    package.objectDataReady = true;
    package.renderWidth = 64u;
    package.renderHeight = 64u;
    package.passCommandInputs = { opaquePassInput };

    auto result = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
        driver, snapshot, [package](size_t) { return package; }));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto submissionFrame = result.get();
    EXPECT_TRUE(submissionFrame.usedResourceStateTracker);
    EXPECT_TRUE(submissionFrame.usedDescriptorAllocator);
    EXPECT_EQ(submissionFrame.retiredTransientBufferCount, 1u);
    EXPECT_EQ(submissionFrame.retiredTransientTextureCount, 1u);
    EXPECT_EQ(submissionFrame.descriptorTransientUsed, 6u);
    EXPECT_EQ(submissionFrame.descriptorTransientPeak, 6u);
    EXPECT_EQ(submissionFrame.descriptorPersistentUsed, 0u);
    EXPECT_EQ(submissionFrame.descriptorPersistentReleased, 4u);
    EXPECT_EQ(submissionFrame.descriptorAllocationFailures, 1u);
    EXPECT_EQ(descriptorAllocator->beginFrameCalls, 1u);
    EXPECT_EQ(descriptorAllocator->endFrameCalls, 0u);
    EXPECT_EQ(uploadContext->beginFrameCalls, 1u);
    EXPECT_EQ(uploadContext->endFrameCalls, 0u);
    EXPECT_EQ(uploadContext->lastBeginFrameIndex, 0u);
    EXPECT_EQ(uploadContext->lastEndFrameIndex, 0u);
    EXPECT_TRUE(submissionFrame.deferredFrameScopedRetirement);

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
        submissionFrame.transientBufferRegistrationCount);
    EXPECT_EQ(
        telemetry.transientTextureRegistrationCount,
        submissionFrame.transientTextureRegistrationCount);
    EXPECT_EQ(
        telemetry.retiredTransientBufferCount,
        submissionFrame.retiredTransientBufferCount);
    EXPECT_EQ(
        telemetry.retiredTransientTextureCount,
        submissionFrame.retiredTransientTextureCount);
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

TEST(RenderFrameExecutionTests, CreateExplicitBindingSetTracksTransientAndPersistentDescriptorLifetime)
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
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

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

    explicitFrameResources.reset();
}

TEST(RenderFrameExecutionTests, CreateExplicitBindingSetExposesSharedWrappedBindingSetForRhiThreadOwnership)
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
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

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

    explicitFrameResources.reset();
}

TEST(RenderFrameExecutionTests, PipelineCacheUsesBackendAwareStableKeysAndTracksPrewarmStats)
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

TEST(RenderFrameExecutionTests, AsyncComputeDiagnosticsReportReadyButNotScheduledWhenQueueAndCapabilityExist)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 408u;
    snapshot.targetsSwapchain = false;

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
    package.frameId = snapshot.frameId;
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

    auto result = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
        driver, snapshot, [package](size_t) { return package; }));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto submissionFrame = result.get();
    EXPECT_EQ(
        submissionFrame.asyncComputeDisposition,
        NLS::Render::Context::AsyncComputeDisposition::ReadyButNotScheduled);
    EXPECT_EQ(submissionFrame.asyncComputeCandidateWorkloadCount, 1u);
    EXPECT_TRUE(submissionFrame.asyncComputeQueueAvailable);
    EXPECT_FALSE(submissionFrame.usedAsyncComputeQueueSubmission);
}

TEST(RenderFrameExecutionTests, AsyncComputeDiagnosticsReportDisabledWhenDedicatedQueueIsUnavailable)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 409u;
    snapshot.targetsSwapchain = false;

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
    package.frameId = snapshot.frameId;
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

    auto result = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
        driver, snapshot, [package](size_t) { return package; }));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto submissionFrame = result.get();
    EXPECT_EQ(
        submissionFrame.asyncComputeDisposition,
        NLS::Render::Context::AsyncComputeDisposition::DisabledNoComputeQueue);
    EXPECT_EQ(submissionFrame.asyncComputeCandidateWorkloadCount, 1u);
    EXPECT_FALSE(submissionFrame.asyncComputeQueueAvailable);
    EXPECT_FALSE(submissionFrame.usedAsyncComputeQueueSubmission);
}

TEST(RenderFrameExecutionTests, DeferredVisibilityTranslatesInsideConsumerCommandList)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 406u;
    snapshot.targetsSwapchain = false;

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
    package.frameId = snapshot.frameId;
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

    auto result = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
        driver, snapshot, [package](size_t) { return package; }));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto submissionFrame = result.get();
    EXPECT_TRUE(submissionFrame.recordedVisibleWork);
    EXPECT_EQ(submissionFrame.recordedPassCount, 2u);
    EXPECT_EQ(submissionFrame.recordedWorkUnitCount, 2u);
    EXPECT_GT(submissionFrame.parallelRecordingWorkerCount, 0u);
    EXPECT_EQ(submissionFrame.translatedWorkUnitCount, 2u);
    EXPECT_TRUE(submissionFrame.usedParallelCommandPath);
    EXPECT_TRUE(submissionFrame.usedTranslationMerge);
    EXPECT_FALSE(submissionFrame.usedSerialCommandPath);
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
    EXPECT_EQ(secondSubmittedBuffer->beginRenderPassCalls, 1u);
    EXPECT_EQ(thirdSubmittedBuffer->beginRenderPassCalls, 0u);
    EXPECT_LT(std::find(secondSubmittedBuffer->events.begin(), secondSubmittedBuffer->events.end(), "Barrier"),
        std::find(secondSubmittedBuffer->events.begin(), secondSubmittedBuffer->events.end(), "BeginRenderPass"));

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

TEST(RenderFrameExecutionTests, ComputeVisibilityTranslatesBeforeConsumerRenderPass)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 407u;
    snapshot.targetsSwapchain = false;

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
    package.frameId = snapshot.frameId;
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

    auto result = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
        driver, snapshot, [package](size_t) { return package; }));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto submissionFrame = result.get();
    EXPECT_TRUE(submissionFrame.usedTranslationMerge);
    ASSERT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    ASSERT_EQ(explicitDevice->GetTestQueue()->lastSubmitDesc.commandBuffers.size(), 2u);
    auto firstSubmittedBuffer = GetSubmittedTestCommandBuffer(explicitDevice->GetTestQueue(), 0u);
    auto secondSubmittedBuffer = GetSubmittedTestCommandBuffer(explicitDevice->GetTestQueue(), 1u);
    ASSERT_NE(firstSubmittedBuffer, nullptr);
    ASSERT_NE(secondSubmittedBuffer, nullptr);
    EXPECT_EQ(firstSubmittedBuffer->dispatchCalls, 1u);
    EXPECT_GT(secondSubmittedBuffer->barrierCalls, 0u);
    EXPECT_EQ(secondSubmittedBuffer->beginRenderPassCalls, 1u);
    EXPECT_LT(std::find(secondSubmittedBuffer->events.begin(), secondSubmittedBuffer->events.end(), "Barrier"),
        std::find(secondSubmittedBuffer->events.begin(), secondSubmittedBuffer->events.end(), "BeginRenderPass"));

    uint32_t barrierBatchCount = 0u;
    for (const auto& submittedBufferBase : explicitDevice->GetTestQueue()->lastSubmitDesc.commandBuffers)
    {
        auto submittedBuffer = std::dynamic_pointer_cast<TestCommandBuffer>(submittedBufferBase);
        if (submittedBuffer != nullptr && submittedBuffer->barrierCalls > 0u)
            ++barrierBatchCount;
    }
    EXPECT_GE(barrierBatchCount, 1u);
}

TEST(RenderFrameExecutionTests, DependencyTextureVisibilityPreservesProducerStateForPerMipHazards)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 411u;
    snapshot.targetsSwapchain = false;

    NLS::Render::RHI::RHITextureDesc textureDesc;
    textureDesc.debugName = "PerMipHZB";
    textureDesc.extent = { 64u, 64u, 1u };
    textureDesc.dimension = NLS::Render::RHI::TextureDimension::Texture2D;
    textureDesc.format = NLS::Render::RHI::TextureFormat::R32F;
    textureDesc.usage =
        NLS::Render::RHI::TextureUsageFlags::Sampled |
        NLS::Render::RHI::TextureUsageFlags::Storage;
    textureDesc.mipLevels = 3u;
    auto texture = std::make_shared<TestTexture>(textureDesc);
    auto graphicsPipeline = std::make_shared<TestGraphicsPipeline>("HZBReadConsumer");
    auto materialBindingSet = std::make_shared<TestBindingSet>("HZBReadMaterial");
    auto mesh = std::make_shared<TestMesh>();

    NLS::Render::RHI::RHISubresourceRange mipRange;
    mipRange.baseMipLevel = 1u;
    mipRange.mipLevelCount = 1u;
    mipRange.baseArrayLayer = 0u;
    mipRange.arrayLayerCount = 1u;

    auto computePipeline = std::make_shared<TestComputePipeline>("HZBWriteMip");

    NLS::Render::Context::RenderPassCommandInput computePassInput;
    computePassInput.kind = NLS::Render::Context::RenderPassCommandKind::Compute;
    computePassInput.debugName = "HZBWriteMip";
    computePassInput.queueType = NLS::Render::RHI::QueueType::Compute;
    computePassInput.renderWidth = 64u;
    computePassInput.renderHeight = 64u;
    computePassInput.textureResourceAccesses.push_back({
        texture,
        mipRange,
        NLS::Render::Context::ResourceAccessMode::Write,
        NLS::Render::RHI::ResourceState::ShaderWrite,
        NLS::Render::RHI::PipelineStageMask::ComputeShader,
        NLS::Render::RHI::AccessMask::ShaderWrite
    });
    computePassInput.computeDispatchInputs.push_back({
        "HZBWriteMip",
        computePipeline,
        {},
        1u,
        1u,
        1u
    });

    NLS::Render::Context::RenderPassCommandInput readPassInput;
    readPassInput.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    readPassInput.debugName = "HZBReadMip";
    readPassInput.queueType = NLS::Render::RHI::QueueType::Graphics;
    readPassInput.renderWidth = 64u;
    readPassInput.renderHeight = 64u;
    readPassInput.drawCount = 1u;
    readPassInput.requiresFrameData = true;
    readPassInput.requiresObjectData = true;
    readPassInput.usesColorAttachment = true;
    readPassInput.usesDepthStencilAttachment = true;
    readPassInput.queueDependencyPolicy = NLS::Render::Context::QueueDependencyPolicy::Previous;
    readPassInput.dependencySourceWorkUnitIndex = 0u;
    readPassInput.requiresDependencyVisibility = true;
    readPassInput.textureResourceAccesses.push_back({
        texture,
        mipRange,
        NLS::Render::Context::ResourceAccessMode::Read,
        NLS::Render::RHI::ResourceState::ShaderRead,
        NLS::Render::RHI::PipelineStageMask::FragmentShader,
        NLS::Render::RHI::AccessMask::ShaderRead
    });
    readPassInput.recordedDrawCommands.push_back({
        graphicsPipeline,
        nullptr,
        nullptr,
        nullptr,
        materialBindingSet,
        mesh,
        1u
    });

    NLS::Render::Context::ParallelCommandWorkUnit computeWorkUnit;
    computeWorkUnit.commandInput = computePassInput;
    computeWorkUnit.debugName = computePassInput.debugName;
    computeWorkUnit.eligibleForParallelTranslation = true;

    NLS::Render::Context::ParallelCommandWorkUnit readWorkUnit;
    readWorkUnit.commandInput = readPassInput;
    readWorkUnit.debugName = readPassInput.debugName;
    readWorkUnit.submissionOrder = 1u;
    readWorkUnit.eligibleForParallelTranslation = true;

    NLS::Render::Context::WorkUnitDependencyEdge textureDependency;
    textureDependency.sourceWorkUnitIndex = 0u;
    textureDependency.targetWorkUnitIndex = 1u;
    textureDependency.kind = NLS::Render::Context::ThreadedDependencyKind::ResourceVisibility;
    textureDependency.resourceKind = NLS::Render::Context::ThreadedDependencyResourceKind::Texture;
    textureDependency.sourceTextureAccess = computePassInput.textureResourceAccesses.front();
    textureDependency.targetTextureAccess = readPassInput.textureResourceAccesses.front();
    readWorkUnit.incomingDependencyEdges.push_back(textureDependency);

    NLS::Render::Context::RenderScenePackage package;
    package.frameId = snapshot.frameId;
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
    package.parallelCommandWorkUnits = { computeWorkUnit, readWorkUnit };

    auto result = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
        driver, snapshot, [package](size_t) { return package; }));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);
    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);

    bool foundPerMipWriteToRead = false;
    bool foundUnknownToRead = false;
    for (const auto& submittedBufferBase : explicitDevice->GetTestQueue()->lastSubmitDesc.commandBuffers)
    {
        const auto submittedBuffer = std::dynamic_pointer_cast<TestCommandBuffer>(submittedBufferBase);
        if (submittedBuffer == nullptr)
            continue;

        for (const auto& barrierBatch : submittedBuffer->barrierHistory)
        {
            for (const auto& textureBarrier : barrierBatch.textureBarriers)
            {
                if (textureBarrier.texture != texture ||
                    textureBarrier.subresourceRange.baseMipLevel != mipRange.baseMipLevel ||
                    textureBarrier.subresourceRange.mipLevelCount != mipRange.mipLevelCount)
                {
                    continue;
                }

                if (textureBarrier.before == NLS::Render::RHI::ResourceState::ShaderWrite &&
                    textureBarrier.after == NLS::Render::RHI::ResourceState::ShaderRead)
                {
                    foundPerMipWriteToRead = true;
                }
                if (textureBarrier.before == NLS::Render::RHI::ResourceState::Unknown &&
                    textureBarrier.after == NLS::Render::RHI::ResourceState::ShaderRead)
                {
                    foundUnknownToRead = true;
                }
            }
        }
    }

    EXPECT_TRUE(foundPerMipWriteToRead);
    EXPECT_FALSE(foundUnknownToRead);
}

TEST(RenderFrameExecutionTests, ConsumerCommandPoolFailureMarksSubmissionAsFailedRetirement)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    explicitDevice->MutableCapabilities().supportsParallelCommandTranslation = true;
    explicitDevice->FailCreatedCommandPoolAtCall(2u);
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

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 413u;
    snapshot.targetsSwapchain = false;

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
    package.frameId = snapshot.frameId;
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

    auto result = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
        driver, snapshot, [package](size_t) { return package; }));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto submissionFrame = result.get();
    EXPECT_FALSE(submissionFrame.submittedSuccessfully);
    EXPECT_GT(submissionFrame.commandRecordingFailureCount, 0u);
    EXPECT_NE(
        submissionFrame.lastCommandRecordingFailure.find("PrepareParallelCommandResources failed"),
        std::string::npos);
    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 0u);

    const auto telemetry = NLS::Render::Context::DriverRendererAccess::GetThreadedFrameTelemetry(driver);
    EXPECT_EQ(telemetry.latestRetiredFrameId, 0u);
    EXPECT_EQ(telemetry.latestFailedRetiredFrameId, snapshot.frameId);
}

TEST(RenderFrameExecutionTests, TranslationVisibilityBarrierFailureMarksSubmissionAsFailedRetirement)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    explicitDevice->MutableCapabilities().supportsParallelCommandTranslation = true;
    explicitDevice->FailCommandBufferBarrierCheckedAtCommandPoolCall(2u);
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

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 415u;
    snapshot.targetsSwapchain = false;

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
    package.frameId = snapshot.frameId;
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

    auto result = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
        driver, snapshot, [package](size_t) { return package; }));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto submissionFrame = result.get();
    EXPECT_FALSE(submissionFrame.submittedSuccessfully);
    EXPECT_GT(submissionFrame.commandRecordingFailureCount, 0u);
    EXPECT_NE(
        submissionFrame.lastCommandRecordingFailure.find("test barrier failure"),
        std::string::npos);
    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 0u);
    const auto createdCommandPools = explicitDevice->GetCreatedCommandPools();
    ASSERT_EQ(createdCommandPools.size(), 2u);
    auto visibilityCommandBuffer =
        std::dynamic_pointer_cast<TestCommandBuffer>(createdCommandPools[1]->commandBuffer);
    ASSERT_NE(visibilityCommandBuffer, nullptr);
    EXPECT_GT(visibilityCommandBuffer->barrierCheckedCalls, 0u);

    const auto telemetry = NLS::Render::Context::DriverRendererAccess::GetThreadedFrameTelemetry(driver);
    EXPECT_EQ(telemetry.latestRetiredFrameId, 0u);
    EXPECT_EQ(telemetry.latestFailedRetiredFrameId, snapshot.frameId);
}

TEST(RenderFrameExecutionTests, RhiWorkerSkipsUnrecordableRecordedPassPlans)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 402u;
    snapshot.targetsSwapchain = false;

    NLS::Render::Context::RenderScenePackage package;
    package.frameId = snapshot.frameId;
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
    auto result = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
        driver, snapshot, [package](size_t) { return package; }));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto submissionFrame = result.get();
    EXPECT_FALSE(submissionFrame.recordedVisibleWork);
    EXPECT_EQ(submissionFrame.recordedPassCount, 0u);
    EXPECT_EQ(commandBuffer->beginCalls, 0u);
    EXPECT_EQ(commandBuffer->endCalls, 0u);
    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 0u);
}

TEST(RenderFrameExecutionTests, RhiStageExceptionPreservesItsDiagnosticAndReleasesTheFrame)
{
    using namespace NLS::Render::Context;
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;
    Driver driver(settings);
    DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto device = std::make_shared<TestExplicitDevice>();
    DriverTestAccess::SetExplicitDevice(driver, device);
    auto& context = DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto buffer = std::make_shared<TestCommandBuffer>();
    auto pool = std::make_shared<TestCommandPool>();
    buffer->beginFailure = "expected record failure";
    pool->commandBuffer = buffer;
    context.commandBuffer = buffer;
    context.commandPool = pool;
    context.frameFence = std::make_shared<TestFence>();

    RenderFramePacket snapshot;
    snapshot.frameId = 404u;
    snapshot.targetsSwapchain = false;
    RenderScenePackage package;
    package.frameId = snapshot.frameId;
    package.targetsSwapchain = false;
    NLS::Render::Context::RenderPassCommandInput pass;
    pass.targetsSwapchain = false;
    pass.renderWidth = 64u;
    pass.renderHeight = 64u;
    pass.usesColorAttachment = true;
    package.passCommandInputs.push_back(pass);
    auto result = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    ASSERT_TRUE(DriverRendererAccess::TryEnqueueRenderFrame(driver, snapshot, [package](size_t) { return package; }));
    device->SetNativeBackendType(NLS::Render::RHI::NativeBackendType::None);
    DriverTestAccess::DrainThreadedRendering(driver);

    const auto telemetry = DriverRendererAccess::GetThreadedFrameTelemetry(driver);
    EXPECT_EQ(telemetry.inFlightFrameCount, 0u);
    EXPECT_EQ(telemetry.latestFailedRetiredFrameId, snapshot.frameId);
    EXPECT_EQ(telemetry.failedRetiredFrameCount, 1u);
    EXPECT_EQ(telemetry.successfullyRetiredFrameCount, 0u);
    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto submissionFrame = result.get();
    EXPECT_NE(submissionFrame.lastCommandRecordingFailure.find("Prepare: expected record failure"), std::string::npos)
        << submissionFrame.lastCommandRecordingFailure;
    EXPECT_EQ(device->GetTestQueue()->submitCalls, 0u);
}

TEST(RenderFrameExecutionTests, DeferredFrameReleaseRequiresAConfirmedGpuFence)
{
    using namespace NLS::Render::Context;
    DriverImpl impl;
    impl.EnsureFrameContexts(1u);
    auto& context = impl.GetFrameContext(0u);
    context.frameIndex = 93u;
    auto fence = std::make_shared<TestFence>();
    context.frameFence = fence;
    auto allocator = std::make_shared<TestDescriptorAllocator>();
    auto upload = std::make_shared<TestUploadContext>();
    auto tracker = std::make_shared<TestResourceStateTracker>();
    context.descriptorAllocator = allocator;
    context.uploadContext = upload;
    context.resourceStateTracker = tracker;
    impl.EnsureFrameContexts(1u);
    auto resource = std::make_shared<int>(42);
    std::weak_ptr<int> resourceProbe = resource;
    impl.frameResourceSlots->PeekSlot(0u)->ownedResources.push_back(resource);
    resource.reset();
    impl.frameResourceSlots->PeekSlot(0u)->retirementPending = true;
    impl.frameResourceSlots->RetireSlot(0u, fence);

    EXPECT_FALSE(Detail::ReleaseDeferredThreadedFrameScopedResourcesAfterFence(impl, context, 0u));
    EXPECT_FALSE(resourceProbe.expired());
    EXPECT_EQ(allocator->endFrameCalls, 0u);
    EXPECT_EQ(upload->endFrameCalls, 0u);
    EXPECT_EQ(tracker->retireTransientResourcesCalls, 0u);
    EXPECT_EQ(impl.frameResourceSlots->IsRetirementPending(0u), 1u);

    impl.frameResourceSlots->PeekSlot(0u)->gpuFence.reset();
    EXPECT_FALSE(Detail::ReleaseDeferredThreadedFrameScopedResourcesAfterFence(impl, context, 0u));
    impl.frameResourceSlots->RetireSlot(0u, fence);
    context.frameFence = std::make_shared<TestFence>();
    fence->signaled = true;
    impl.unsafeGpuWorkQuarantined.store(true);
    EXPECT_FALSE(Detail::ReleaseDeferredThreadedFrameScopedResourcesAfterFence(impl, context, 0u));
    EXPECT_FALSE(resourceProbe.expired());

    impl.unsafeGpuWorkQuarantined.store(false);
    EXPECT_TRUE(Detail::ReleaseDeferredThreadedFrameScopedResourcesAfterFence(impl, context, 0u));
    EXPECT_TRUE(resourceProbe.expired());
    EXPECT_EQ(allocator->lastEndFrameIndex, 93u);
    EXPECT_EQ(upload->lastEndFrameIndex, 93u);
    EXPECT_EQ(allocator->endFrameCalls, 1u);
    EXPECT_EQ(upload->endFrameCalls, 1u);
    EXPECT_EQ(tracker->retireTransientResourcesCalls, 1u);
    EXPECT_FALSE(Detail::ReleaseDeferredThreadedFrameScopedResourcesAfterFence(impl, context, 0u));
}

TEST(RenderFrameExecutionTests, ConcurrentDeferredFrameReleaseRetiresEachSlotOnce)
{
    using namespace NLS::Render::Context;
    DriverImpl impl;
    constexpr size_t kSlotCount = 64u;
    impl.EnsureFrameContexts(kSlotCount);
    impl.EnsureFrameContexts(kSlotCount);
    std::vector<std::shared_ptr<TestDescriptorAllocator>> allocators;
    for (size_t slot = 0u; slot < kSlotCount; ++slot)
    {
        auto& context = impl.GetFrameContext(slot);
        context.frameIndex = slot + 1000u;
        auto fence = std::make_shared<TestFence>();
        fence->signaled = true;
        context.frameFence = fence;
        auto allocator = std::make_shared<TestDescriptorAllocator>();
        context.descriptorAllocator = allocator;
        allocators.push_back(std::move(allocator));
        impl.frameResourceSlots->PeekSlot(slot)->ownedResources.push_back(std::make_shared<int>(42));
        impl.frameResourceSlots->PeekSlot(slot)->retirementPending = true;
        impl.frameResourceSlots->RetireSlot(slot, fence);
    }

    std::promise<void> start;
    auto ready = start.get_future().share();
    std::vector<std::thread> releasers;
    for (size_t caller = 0u; caller < 8u; ++caller)
    {
        releasers.emplace_back([&impl, ready]
        {
            ready.wait();
            Detail::ReleaseDeferredThreadedFrameScopedResourcesAfterFence(impl);
        });
    }
    start.set_value();
    for (auto& releaser : releasers)
        releaser.join();

    EXPECT_TRUE((impl.frameResourceSlots == nullptr || !impl.frameResourceSlots->HasPendingRetirements()));
    for (size_t slot = 0u; slot < kSlotCount; ++slot)
    {
        EXPECT_EQ(allocators[slot]->endFrameCalls, 1u) << slot;
        EXPECT_EQ(allocators[slot]->lastEndFrameIndex, slot + 1000u);
        EXPECT_TRUE(impl.frameResourceSlots->PeekSlot(slot)->ownedResources.empty());
    }
}

TEST(RenderFrameExecutionTests, ReadbackExceptionDefersFrameResourcesAndCompletesPendingReadbacks)
{
    using namespace NLS::Render::Context;
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;
    Driver driver(settings);
    ScopedDriverService driverService(driver);
    DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto device = std::make_shared<TestExplicitDevice>();
    device->beginReadBufferFailure = "injected post-submit readback exception";
    DriverTestAccess::SetExplicitDevice(driver, device);
    auto& context = DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto buffer = std::make_shared<TestCommandBuffer>();
    auto pool = std::make_shared<TestCommandPool>();
    pool->commandBuffer = buffer;
    context.commandBuffer = buffer;
    context.commandPool = pool;
    auto fence = std::make_shared<TestFence>();
    context.frameFence = fence;
    auto descriptorAllocator = std::make_shared<TestDescriptorAllocator>();
    auto upload = std::make_shared<TestUploadContext>();
    context.descriptorAllocator = descriptorAllocator;
    context.uploadContext = upload;

    RenderFramePacket snapshot;
    snapshot.frameId = 9004u;
    snapshot.targetsSwapchain = false;
    RenderScenePackage package;
    package.frameId = snapshot.frameId;
    package.targetsSwapchain = false;
    RenderPassCommandInput pass;
    pass.targetsSwapchain = false;
    pass.renderWidth = 64u;
    pass.renderHeight = 64u;
    pass.usesColorAttachment = true;
    pass.clearColor = true;
    package.passCommandInputs.push_back(pass);
    PostSubmitBufferReadbackRequest request;
    request.desc.source = std::make_shared<TestBuffer>(32u);
    request.desc.size = 4u;
    request.state = std::make_shared<PostSubmitBufferReadbackState>();
    package.postSubmitBufferReadbacks.push_back(request);
    auto unstartedBuffer = std::make_shared<PostSubmitBufferReadbackState>();
    auto secondRequest = request;
    secondRequest.state = unstartedBuffer;
    package.postSubmitBufferReadbacks.push_back(secondRequest);
    PostSubmitTextureReadbackRequest textureRequest;
    textureRequest.state = std::make_shared<PostSubmitTextureReadbackState>();
    package.postSubmitTextureReadbacks.push_back(textureRequest);
    auto result = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    ASSERT_TRUE(DriverRendererAccess::TryEnqueueRenderFrame(driver, snapshot, [package](size_t) { return package; }));
    device->SetNativeBackendType(NLS::Render::RHI::NativeBackendType::None);
    DriverTestAccess::DrainThreadedRendering(driver);

    EXPECT_EQ(device->GetTestQueue()->submitCalls, 1u);
    EXPECT_EQ(device->beginBufferReadbackCalls, 1u);
    EXPECT_EQ(descriptorAllocator->endFrameCalls, 0u);
    EXPECT_EQ(upload->endFrameCalls, 0u);
    auto* impl = DriverTestAccess::GetImplForTesting(driver);
    EXPECT_EQ(impl->frameResourceSlots->IsRetirementPending(0u), 1u);
    EXPECT_EQ(DriverRendererAccess::GetThreadedFrameTelemetry(driver).inFlightFrameCount, 0u);
    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto submissionFrame = result.get();
    EXPECT_TRUE(submissionFrame.deferredFrameScopedRetirement);
    EXPECT_FALSE(submissionFrame.submittedSuccessfully);
    EXPECT_NE(submissionFrame.lastCommandRecordingFailure.find(
        "Readback: injected post-submit readback exception"), std::string::npos);
    const auto expectFailedReadback = [](const auto& state)
    {
        std::lock_guard lock(state->mutex);
        EXPECT_TRUE(state->beginAttempted);
        EXPECT_FALSE(state->beginInProgress);
        EXPECT_FALSE(state->beginSucceeded);
        EXPECT_EQ(state->completion, nullptr);
        EXPECT_NE(state->resultMessage.find("injected post-submit readback exception"), std::string::npos);
    };
    expectFailedReadback(request.state);
    expectFailedReadback(unstartedBuffer);
    expectFailedReadback(textureRequest.state);

    fence->signaled = true;
    EXPECT_TRUE(Detail::ReleaseDeferredThreadedFrameScopedResourcesAfterFence(*impl, context, 0u));
    EXPECT_EQ(descriptorAllocator->endFrameCalls, 1u);
    EXPECT_EQ(upload->endFrameCalls, 1u);
    EXPECT_FALSE(Detail::ReleaseDeferredThreadedFrameScopedResourcesAfterFence(*impl, context, 0u));
}

TEST(RenderFrameExecutionTests, RhiWorkerMarksBeginPassFailureAsFailedRetirement)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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


    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 405u;
    snapshot.targetsSwapchain = false;

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

    auto result = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, snapshot, [package](size_t) { return package; }));
    explicitDevice->SetNativeBackendType(NLS::Render::RHI::NativeBackendType::None);
    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto submissionFrame = result.get();
    EXPECT_FALSE(submissionFrame.submittedSuccessfully);
    EXPECT_FALSE(submissionFrame.recordedVisibleWork);
    EXPECT_EQ(submissionFrame.recordedPassCount, 0u);
    EXPECT_EQ(commandBuffer->beginRenderPassCalls, 0u);
    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 0u);

    const auto telemetry = NLS::Render::Context::DriverRendererAccess::GetThreadedFrameTelemetry(driver);
    EXPECT_EQ(telemetry.latestRetiredFrameId, 0u);
    EXPECT_EQ(telemetry.latestFailedRetiredFrameId, snapshot.frameId);
}

TEST(RenderFrameExecutionTests, ThreadedRendererSkipsStandaloneExplicitFrameRecordingForOffscreenFrames)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

TEST(RenderFrameExecutionTests, PendingSwapchainResizeBlocksAdditionalSwapchainSnapshotsUntilDrainCompletes)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 2u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    NLS::Render::Context::RenderFramePacket firstSnapshot;
    firstSnapshot.frameId = 141u;
    firstSnapshot.targetsSwapchain = true;

    NLS::Render::Context::RenderFramePacket secondSnapshot;
    secondSnapshot.frameId = 142u;
    secondSnapshot.targetsSwapchain = true;

    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, firstSnapshot, [firstSnapshot](size_t) { return NLS::Render::Context::BuildRenderFramePackage(firstSnapshot); }));

    driver.ResizePlatformSwapchain(1600u, 900u);

    EXPECT_FALSE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, secondSnapshot, [secondSnapshot](size_t) { return NLS::Render::Context::BuildRenderFramePackage(secondSnapshot); }));

    EXPECT_EQ(NLS::Render::Context::DriverRendererAccess::GetThreadedFrameTelemetry(driver).publishedFrameCount, 1u);
    EXPECT_EQ(NLS::Render::Context::DriverRendererAccess::GetThreadedFrameTelemetry(driver).inFlightFrameCount, 1u);
}

TEST(RenderFrameExecutionTests, DriverShutdownDrainsPublishedSwapchainFramesBeforeDestroyingWorkers)
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

        NLS::Render::Context::RenderFramePacket snapshot;
        snapshot.frameId = 151u;
        snapshot.targetsSwapchain = true;

        ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, snapshot, [snapshot](size_t) { return NLS::Render::Context::BuildRenderFramePackage(snapshot); }));
    }

    const auto& submits = explicitDevice->GetTestQueue()->submitHistory;
    ASSERT_EQ(submits.size(), 2u);
    ASSERT_EQ(submits.front().commandBuffers.size(), 1u);
    EXPECT_EQ(submits.front().commandBuffers.front(), commandBuffer);
    EXPECT_TRUE(submits.back().commandBuffers.empty());
    ASSERT_NE(submits.back().signalFence, nullptr);
    EXPECT_NE(submits.back().signalFence, frameFence);
    EXPECT_TRUE(submits.back().signalFence->IsSignaled());
    EXPECT_EQ(explicitDevice->GetTestQueue()->presentCalls, 1u);
    EXPECT_EQ(swapchain->acquireCalls, 1u);
    EXPECT_EQ(frameFence->waitCalls, 1u);
    EXPECT_EQ(frameFence->resetCalls, 1u);
}

TEST(RenderFrameExecutionTests, DriverShutdownReleasesRetainedThreadedRhiResourcesBeforeFrameContextAllocators)
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
        settings.framesInFlight = 1u;

        NLS::Render::Context::Driver driver(settings);
        NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
        NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

        auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
        frameContext.frameFence = frameFence;
        frameContext.commandBuffer = commandBuffer;
        frameContext.commandPool = commandPool;
        frameContext.descriptorAllocator = std::make_shared<ShutdownOrderDescriptorAllocator>(probe);

        NLS::Render::Context::RenderFramePacket snapshot;
        snapshot.frameId = 161u;
        snapshot.targetsSwapchain = false;
        snapshot.renderWidth = 64u;
        snapshot.renderHeight = 64u;

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

            ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, snapshot, [package](size_t) { return package; }));
        }

        ASSERT_FALSE(retainedBindingSet.expired());
    }

    EXPECT_TRUE(probe->bindingSetDestroyed);
    EXPECT_TRUE(probe->descriptorAllocatorDestroyed);
    EXPECT_TRUE(probe->bindingSetDestroyedBeforeDescriptorAllocator);
    EXPECT_TRUE(retainedBindingSet.expired());
}

TEST(RenderFrameExecutionTests, RendererFrameReservationDoesNotOpenNativeCommandBuffers)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

    ASSERT_TRUE(NLS::Render::Context::RenderThreadCoordinator::BeginRendererFrame(driver));
    NLS::Render::Context::RenderThreadCoordinator::EndRendererFrame(driver);

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



TEST(RenderFrameExecutionTests, UiOnlyFramePresentUsesTheCapturedCompositionBoundary)
{
    using namespace NLS::Render::Context;
    ImGuiContextGuard imguiContext;
    ScopedOverlayShaderManager shaderManagerScope;
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;
    Driver driver(settings);
    ASSERT_EQ(TryGetLocatedDriver(), nullptr);
    DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto device = std::make_shared<TestExplicitDevice>();
    device->MutableCapabilities().SetFeature(
        NLS::Render::RHI::RHIDeviceFeature::UIOverlayFrameGraph, true, "test overlay support");
    auto swapchain = std::make_shared<TestSwapchain>();
    swapchain->desc.width = 128u;
    swapchain->desc.height = 72u;
    DriverTestAccess::SetExplicitDevice(driver, device);
    DriverTestAccess::SetExplicitSwapchain(driver, swapchain);

    const NLS::Render::RHI::NativeHandle uiSignal{
        NLS::Render::RHI::BackendType::DX12, reinterpret_cast<void*>(0x1234)};
    DriverUIAccess::SetUICompositionSignal(driver, uiSignal, 42u);
    DriverUIAccess::PublishUiDrawDataSnapshot(driver, MakeVisibleUiSnapshot(306u, 128.0f, 72.0f));
    ASSERT_TRUE(DriverUIAccess::PublishUiOnlyFrame(driver, 128u, 72u));
    EXPECT_EQ(swapchain->acquireCalls, 0u);
    EXPECT_EQ(device->GetTestQueue()->presentCalls, 0u);
    DriverTestAccess::DrainThreadedRendering(driver);

    const auto queue = device->GetTestQueue();
    EXPECT_EQ(queue->submitCalls, 1u);
    EXPECT_EQ(queue->presentCalls, 1u);
    EXPECT_EQ(queue->lastSubmitDesc.commandBuffers.size(), 1u);
    EXPECT_EQ(queue->lastPresentDesc.swapchain, swapchain);
    EXPECT_EQ(queue->lastPresentDesc.imageIndex, 1u);
    EXPECT_EQ(queue->lastPresentDesc.uiSignalSemaphore.handle, uiSignal.handle);
    EXPECT_EQ(queue->lastPresentDesc.uiSignalValue, 42u);
    EXPECT_EQ(swapchain->acquireCalls, 1u);
    const auto boundary = DriverUIAccess::BuildUICompositionSyncBoundary(driver);
    EXPECT_FALSE(boundary.uiToPresentSignalSemaphore.IsValid());
    EXPECT_EQ(boundary.uiToPresentSignalValue, 0u);
}


TEST(RenderFrameExecutionTests, MainThreadPresentSkipsWhenNoUiSnapshotWasPublished)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

TEST(RenderFrameExecutionTests, RenderCommandFramePresentDoesNotRequireAdditionalPresentHandshake)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.targetsSwapchain = true;
    NLS::Render::Context::RenderScenePackage package;
    package.targetsSwapchain = true;
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, snapshot, [package](size_t) { return package; }));
    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    EXPECT_EQ(explicitDevice->GetTestQueue()->presentCalls, 1u);
    EXPECT_EQ(swapchain->acquireCalls, 1u);

    NLS::Render::Context::DriverUIAccess::PresentSwapchain(driver);

    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    EXPECT_EQ(explicitDevice->GetTestQueue()->presentCalls, 1u);
    EXPECT_EQ(swapchain->acquireCalls, 1u);
}

TEST(RenderFrameExecutionTests, RenderCommandFrameIndexStaysMonotonicAcrossRingSlotReuse)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 2u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
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

    auto swapchain = std::make_shared<TestSwapchain>();
    NLS::Render::Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);
    const std::array resources { slot0Resources, slot1Resources };
    for (uint64_t frameIndex = 0u; frameIndex != 3u; ++frameIndex)
    {
        SCOPED_TRACE(frameIndex);
        const size_t expectedSlot = static_cast<size_t>(frameIndex % 2u);
        NLS::Render::Context::RenderFramePacket snapshot;
        snapshot.targetsSwapchain = true;
        NLS::Render::Context::RenderScenePackage package;
        package.targetsSwapchain = true;
        size_t publishedSlot = 99u;
        ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, snapshot, [package](size_t) { return package; }, &publishedSlot));
        ASSERT_EQ(publishedSlot, expectedSlot);
        NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);
        EXPECT_EQ(NLS::Render::Context::DriverTestAccess::PeekFrameContext(driver, expectedSlot)->frameIndex, frameIndex);
        EXPECT_EQ(resources[expectedSlot].first->lastBeginFrameIndex, frameIndex);
        EXPECT_EQ(resources[expectedSlot].second->lastBeginFrameIndex, frameIndex);
        auto fence = std::dynamic_pointer_cast<TestFence>(
            NLS::Render::Context::DriverTestAccess::PeekFrameContext(driver, expectedSlot)->frameFence);
        ASSERT_NE(fence, nullptr);
        fence->signaled = true;
    }

    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 3u);
}

TEST(RenderFrameExecutionTests, RenderCommandFramePresentUsesUiCompositionBoundaryAndClearsSignal)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

    const NLS::Render::RHI::NativeHandle uiSignalSemaphore{
        NLS::Render::RHI::BackendType::DX12,
        reinterpret_cast<void*>(0x5678)
    };
    constexpr uint64_t uiSignalValue = 77u;
    NLS::Render::Context::DriverUIAccess::SetUICompositionSignal(driver, uiSignalSemaphore, uiSignalValue);

    const NLS::Render::RHI::NativeHandle nextUiSignal{
        NLS::Render::RHI::BackendType::DX12,
        reinterpret_cast<void*>(0x6789)
    };
    commandBuffer->onBegin = [&]
    {
        NLS::Render::Context::DriverUIAccess::SetUICompositionSignal(driver, nextUiSignal, 88u);
    };

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.targetsSwapchain = true;
    NLS::Render::Context::RenderScenePackage package;
    package.targetsSwapchain = true;
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, snapshot, [package](size_t) { return package; }));
    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    auto testQueue = explicitDevice->GetTestQueue();
    ASSERT_EQ(testQueue->presentCalls, 1u);
    EXPECT_EQ(testQueue->lastPresentDesc.uiSignalSemaphore.backend, NLS::Render::RHI::BackendType::DX12);
    EXPECT_EQ(testQueue->lastPresentDesc.uiSignalSemaphore.handle, uiSignalSemaphore.handle);
    EXPECT_EQ(testQueue->lastPresentDesc.uiSignalValue, uiSignalValue);

    auto boundary = NLS::Render::Context::DriverUIAccess::BuildUICompositionSyncBoundary(driver);
    EXPECT_EQ(boundary.uiToPresentSignalSemaphore.handle, nextUiSignal.handle);
    EXPECT_EQ(boundary.uiToPresentSignalValue, 88u);

    commandBuffer->onBegin = {};
    frameFence->signaled = true;
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, snapshot, [package](size_t) { return package; }));
    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);
    ASSERT_EQ(testQueue->presentCalls, 2u);
    EXPECT_EQ(testQueue->lastPresentDesc.uiSignalSemaphore.handle, nextUiSignal.handle);
    EXPECT_EQ(testQueue->lastPresentDesc.uiSignalValue, 88u);
    boundary = NLS::Render::Context::DriverUIAccess::BuildUICompositionSyncBoundary(driver);
    EXPECT_FALSE(boundary.uiToPresentSignalSemaphore.IsValid());
    EXPECT_EQ(boundary.uiToPresentSignalValue, 0u);
}

TEST(RenderFrameExecutionTests, RenderCommandFramePresentIgnoresZeroUiCompositionSignalValue)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

    const NLS::Render::RHI::NativeHandle uiSignalSemaphore{
        NLS::Render::RHI::BackendType::DX12,
        reinterpret_cast<void*>(0x5678)
    };
    NLS::Render::Context::DriverUIAccess::SetUICompositionSignal(driver, uiSignalSemaphore, 0u);
    auto boundary = NLS::Render::Context::DriverUIAccess::BuildUICompositionSyncBoundary(driver);
    EXPECT_FALSE(boundary.uiToPresentSignalSemaphore.IsValid());
    EXPECT_EQ(boundary.uiToPresentSignalValue, 0u);

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.targetsSwapchain = true;
    NLS::Render::Context::RenderScenePackage package;
    package.targetsSwapchain = true;
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, snapshot, [package](size_t) { return package; }));
    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    auto testQueue = explicitDevice->GetTestQueue();
    ASSERT_EQ(testQueue->presentCalls, 1u);
    EXPECT_FALSE(testQueue->lastPresentDesc.uiSignalSemaphore.IsValid());
    EXPECT_EQ(testQueue->lastPresentDesc.uiSignalValue, 0u);

    boundary = NLS::Render::Context::DriverUIAccess::BuildUICompositionSyncBoundary(driver);
    EXPECT_FALSE(boundary.uiToPresentSignalSemaphore.IsValid());
    EXPECT_EQ(boundary.uiToPresentSignalValue, 0u);
}

TEST(RenderFrameExecutionTests, RenderCommandFrameWithoutPresentClearsUiCompositionSignal)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

    const NLS::Render::RHI::NativeHandle uiSignalSemaphore{
        NLS::Render::RHI::BackendType::DX12,
        reinterpret_cast<void*>(0x2468)
    };
    NLS::Render::Context::DriverUIAccess::SetUICompositionSignal(driver, uiSignalSemaphore, 91u);

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.targetsSwapchain = false;
    NLS::Render::Context::RenderScenePackage package;
    package.targetsSwapchain = false;
    NLS::Render::Context::RenderPassCommandInput pass;
    pass.kind = NLS::Render::Context::RenderPassCommandKind::Compute;
    NLS::Render::Context::RecordedComputeDispatchInput dispatch;
    dispatch.pipeline = std::make_shared<TestComputePipeline>("OffscreenUiSignalFrame");
    pass.computeDispatchInputs.push_back(std::move(dispatch));
    package.passCommandInputs.push_back(std::move(pass));
    package.containsCommandInputs = true;
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, snapshot, [package](size_t) { return package; }));
    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    EXPECT_EQ(explicitDevice->GetTestQueue()->presentCalls, 0u);
    const auto boundary = NLS::Render::Context::DriverUIAccess::BuildUICompositionSyncBoundary(driver);
    EXPECT_FALSE(boundary.uiToPresentSignalSemaphore.IsValid());
    EXPECT_EQ(boundary.uiToPresentSignalValue, 0u);
}

TEST(RenderFrameExecutionTests, UiOnlyFrameReclaimsAllocatorsAndUploadsOnlyAfterItsGpuFence)
{
    using namespace NLS::Render::Context;
    ImGuiContextGuard imguiContext;
    ScopedOverlayShaderManager shaderManagerScope;
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;
    Driver driver(settings);
    DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto device = std::make_shared<TestExplicitDevice>();
    device->MutableCapabilities().SetFeature(
        NLS::Render::RHI::RHIDeviceFeature::UIOverlayFrameGraph, true, "test overlay support");
    auto swapchain = std::make_shared<TestSwapchain>();
    swapchain->desc.width = 128u;
    swapchain->desc.height = 72u;
    DriverTestAccess::SetExplicitDevice(driver, device);
    DriverTestAccess::SetExplicitSwapchain(driver, swapchain);

    auto& frame = DriverTestAccess::EnsureFrameContext(driver, 0u);
    const auto fence = std::dynamic_pointer_cast<TestFence>(frame.frameFence);
    ASSERT_NE(fence, nullptr);
    auto descriptors = std::make_shared<TestDescriptorAllocator>();
    auto uploads = std::make_shared<TestUploadContext>();
    frame.descriptorAllocator = descriptors;
    frame.uploadContext = uploads;
    DriverUIAccess::PublishUiDrawDataSnapshot(driver, MakeVisibleUiSnapshot(307u, 128.0f, 72.0f));
    ASSERT_TRUE(DriverUIAccess::PublishUiOnlyFrame(driver, 128u, 72u));
    DriverTestAccess::DrainThreadedRendering(driver);

    auto* impl = DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl, nullptr);
    ASSERT_NE(impl->frameResourceSlots, nullptr);
    EXPECT_FALSE(fence->IsSignaled());
    EXPECT_TRUE(impl->frameResourceSlots->IsRetirementPending(0u));
    EXPECT_GT(impl->frameResourceSlots->GetOwnedResourceCount(), 0u);
    EXPECT_EQ(descriptors->beginFrameCalls, 1u);
    EXPECT_EQ(descriptors->endFrameCalls, 0u);
    EXPECT_EQ(uploads->endFrameCalls, 0u);
    EXPECT_FALSE(Detail::ReleaseDeferredThreadedFrameScopedResourcesAfterFence(*impl, frame, 0u));
    EXPECT_EQ(descriptors->endFrameCalls, 0u);
    EXPECT_EQ(uploads->endFrameCalls, 0u);

    fence->signaled = true;
    EXPECT_TRUE(Detail::ReleaseDeferredThreadedFrameScopedResourcesAfterFence(*impl, frame, 0u));
    EXPECT_FALSE(impl->frameResourceSlots->IsRetirementPending(0u));
    EXPECT_EQ(impl->frameResourceSlots->GetOwnedResourceCount(), 0u);
    EXPECT_EQ(descriptors->endFrameCalls, 1u);
    EXPECT_EQ(uploads->endFrameCalls, 1u);
    EXPECT_EQ(descriptors->beginFrameCalls, 1u);
    EXPECT_EQ(uploads->beginFrameCalls, 1u);
    EXPECT_EQ(device->GetTestQueue()->presentCalls, 1u);
}

TEST(RenderFrameExecutionTests, UiOnlyFramePublishesOverlayOnlySwapchainPackageAndUsesSingleSubmitPresent)
{
    ImGuiContextGuard imguiContext;
    ScopedOverlayShaderManager shaderManagerScope;
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    explicitDevice->MutableCapabilities().SetFeature(
        NLS::Render::RHI::RHIDeviceFeature::UIOverlayFrameGraph,
        true,
        "test overlay support");
    auto swapchain = std::make_shared<TestSwapchain>();
    swapchain->desc.width = 128u;
    swapchain->desc.height = 72u;
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
    frameContext.resourceStateTracker = NLS::Render::RHI::CreateDefaultResourceStateTracker();

    auto uiSnapshot = MakeVisibleUiSnapshot(901u, 128.0f, 72.0f);
    NLS::Render::Context::DriverUIAccess::PublishUiDrawDataSnapshot(driver, uiSnapshot);

    auto result = NLS::Tests::ObserveNextRenderSceneForTest(driver);
    auto rhiResult = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    size_t publishedSlot = 99u;
    uint64_t publishedFrameId = 0u;
    ASSERT_TRUE(NLS::Render::Context::DriverUIAccess::PublishUiOnlyFrame(
        driver,
        128u,
        72u,
        &publishedSlot,
        &publishedFrameId));
    EXPECT_EQ(publishedSlot, 0u);
    EXPECT_EQ(publishedFrameId, uiSnapshot->frameId);
    EXPECT_EQ(result.wait_for(std::chrono::milliseconds(0)), std::future_status::timeout);
    EXPECT_EQ(NLS::Render::Context::DriverRendererAccess::GetActiveExplicitCommandBuffer(driver), nullptr)
        << "UI-only overlay frames must not start the legacy standalone explicit UI frame.";

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    auto testQueue = explicitDevice->GetTestQueue();
    EXPECT_EQ(testQueue->submitCalls, 1u);
    EXPECT_EQ(testQueue->presentCalls, 1u);
    EXPECT_EQ(swapchain->acquireCalls, 1u);
    EXPECT_EQ(testQueue->lastSubmitDesc.commandBuffers.size(), 1u);
    auto submittedCommandBuffer = GetSubmittedTestCommandBuffer(testQueue);
    ASSERT_NE(submittedCommandBuffer, nullptr);
    EXPECT_EQ(submittedCommandBuffer->beginCalls, 1u);

    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto package = result.get();
    EXPECT_TRUE(package.targetsSwapchain);
    EXPECT_TRUE(package.hasUIOverlayPass);
    EXPECT_EQ(package.passCommandInputs.size(), 1u);
    ASSERT_FALSE(package.passCommandInputs.empty());
    EXPECT_EQ(package.passCommandInputs.front().kind, NLS::Render::Context::RenderPassCommandKind::UIOverlay);
    EXPECT_EQ(package.passCommandInputs.front().debugName, NLS::Render::Context::kUIOverlayRenderPassDebugName);
    EXPECT_EQ(package.opaqueDrawCount, 0u);
    EXPECT_EQ(package.transparentDrawCount, 0u);
    EXPECT_EQ(package.uiDrawDataSnapshot, uiSnapshot);
    ASSERT_EQ(rhiResult.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto submissionFrame = rhiResult.get();
    EXPECT_EQ(submissionFrame.uiOverlaySnapshotFrameId, uiSnapshot->frameId);
}

TEST(RenderFrameExecutionTests, IdleRenderThreadRetiresGpuResourcesWithoutAnotherFrameOrFlush)
{
    using namespace NLS::Render;
    for (const auto mode : {Context::RhiThreadMode::None, Context::RhiThreadMode::DedicatedThread,
        Context::RhiThreadMode::Tasks})
    {
        SCOPED_TRACE(static_cast<int>(mode));
        Settings::DriverSettings settings;
        settings.graphicsBackend = Settings::EGraphicsBackend::NONE;
        settings.enableExplicitRHI = false;
        settings.rhiThreadMode = mode == Context::RhiThreadMode::None
            ? Settings::RhiThreadModePreference::ForceNone
            : Settings::RhiThreadModePreference::ForceDedicatedThread;
        Context::Driver driver(settings);
        auto& executor = Context::RhiCommandListExecutor::Get();
        const size_t initialQuarantineCount = executor.GetQuarantinedResourceReleaseCount();
        auto* impl = Context::DriverTestAccess::GetImplForTesting(driver);
        executor.Initialize(mode, {}, impl->taskGraph, &driver);
        Context::DriverTestAccess::SetExplicitDevice(driver, std::make_shared<TestExplicitDevice>());
        auto owner = std::make_shared<int>(42);
        std::weak_ptr<int> weakOwner = owner;
        auto result = std::make_shared<std::promise<Context::RhiGpuFence>>();
        auto dispatched = result->get_future();
        const auto command = Context::EnqueueRenderCommand<struct IdleGpuReleaseCommand>(
            [result, mode, graph = impl->taskGraph, owner = std::move(owner)]() mutable
            {
                EXPECT_TRUE(Context::RenderingThreadSystem::IsInRenderingThread());
                auto& lane = Context::RhiCommandListExecutor::Get();
                // Give retirement a preceding FIFO operation to cover.
                const auto preceding = lane.RunOnSubmitLane("IdleGpuResourceUse", [mode, graph]
                    {
                        EXPECT_TRUE(Context::RenderingThreadSystem::IsInRhiThread());
                        EXPECT_EQ(graph->IsWorkerThread(), mode == Context::RhiThreadMode::Tasks);
                        return RHI::RHIQueueOperationResult{};
                    });
                EXPECT_TRUE(preceding.Succeeded());
                result->set_value(lane.DeferRelease({std::move(owner)}));
            });
        ASSERT_EQ(dispatched.wait_for(std::chrono::seconds(2)), std::future_status::ready);
        const auto gpu = dispatched.get();
        ASSERT_TRUE(command.WaitFor(std::chrono::seconds(2)));
        EXPECT_TRUE(gpu.WaitFor(std::chrono::seconds(2)));
        EXPECT_TRUE(gpu.Succeeded()) << gpu.GetDiagnostic();
        for (size_t attempt = 0u; attempt < 200u && executor.GetPendingResourceReleaseCount() != 0u; ++attempt)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        EXPECT_TRUE(weakOwner.expired());
        EXPECT_EQ(executor.GetPendingResourceReleaseCount(), 0u);
        EXPECT_EQ(executor.GetQuarantinedResourceReleaseCount(), initialQuarantineCount);
        EXPECT_EQ(Context::DriverRendererAccess::GetThreadedFrameTelemetry(driver).publishedFrameCount, 0u);
    }
}

TEST(RenderFrameExecutionTests, IdleMeshUploadsCompleteOnTheAvailableLaneInEveryRhiMode)
{
    using namespace NLS::Render::Context;
    using NLS::Base::TaskGraph::NamedThread;
    for (const auto mode : {RhiThreadMode::None, RhiThreadMode::Tasks, RhiThreadMode::DedicatedThread})
    {
        SCOPED_TRACE(static_cast<int>(mode));
        NLS::Base::TaskGraph::TaskGraph graph({1u, false, true});
        ASSERT_TRUE(graph.Start());
        NLS::Render::Settings::DriverSettings settings;
        settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
        settings.enableExplicitRHI = false;
        settings.rhiThreadMode = NLS::Render::Settings::RhiThreadModePreference::ForceNone;
        Driver driver(settings);
        DriverTestAccess::PauseThreadedRenderingWorkers(driver);
        DriverTestAccess::SetExplicitDevice(driver, std::make_shared<TestExplicitDevice>());
        auto* impl = DriverTestAccess::GetImplForTesting(driver);
        ASSERT_NE(impl, nullptr);
        impl->taskGraph = &graph;
        impl->rhiThreadMode = mode;
        const auto lane = mode == RhiThreadMode::DedicatedThread ? NamedThread::Rhi
            : (mode == RhiThreadMode::None ? NamedThread::Render : NamedThread::Any);
        if (lane != NamedThread::Any)
            ASSERT_TRUE(graph.RegisterCurrentThread(lane));

        MeshRuntimeUploadRequest request;
        request.vertices.resize(3u);
        request.indices = {0u, 1u, 2u};
        const auto requestId = DriverResourceAccess::RequestMeshRuntimeUpload(driver, std::move(request));
        ASSERT_NE(requestId, 0u);
        EXPECT_TRUE(RhiCommandListExecutor::TryScheduleIdleMeshRuntimeUploads(driver));
        auto completion = impl->pendingIdleMeshUploadCompletion;
        EXPECT_TRUE(completion.WaitFor(std::chrono::seconds(1)));
        EXPECT_EQ(completion.GetStatus(), NLS::Base::TaskGraph::TaskStatus::Succeeded);
        auto result = DriverResourceAccess::ConsumeMeshRuntimeUploadResult(driver, requestId);
        EXPECT_TRUE(result.ready);
        EXPECT_TRUE(result.success);
        EXPECT_TRUE(result.uploadRequest.has_value());
        if (result.uploadRequest.has_value())
        {
            EXPECT_EQ(result.uploadRequest->vertices.size(), 3u);
            EXPECT_EQ(result.uploadRequest->indices.size(), 3u);
        }
        graph.Shutdown(NLS::Base::TaskGraph::TaskGraphShutdownMode::Immediate);
        RhiCommandListExecutor::FinalizeThreadedSubmissionProduction(driver, false);
    }
}

TEST(RenderFrameExecutionTests, UiOnlySubmitFailureRequiresExecutorRecoveryBeforeIdenticalRetry)
{
    ImGuiContextGuard imguiContext;
    ScopedOverlayShaderManager shaderManagerScope;
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    explicitDevice->MutableCapabilities().SetFeature(
        NLS::Render::RHI::RHIDeviceFeature::UIOverlayFrameGraph,
        true,
        "test overlay support");
    auto swapchain = std::make_shared<TestSwapchain>();
    swapchain->desc.width = 128u;
    swapchain->desc.height = 72u;
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);
    NLS::Render::Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);
    ConfigureUiOverlayFrameContext(driver, 0u);

    auto firstSnapshot = MakeVisibleUiSnapshot(932u, 128.0f, 72.0f);
    NLS::Render::Context::DriverUIAccess::PublishUiDrawDataSnapshot(driver, firstSnapshot);
    ASSERT_TRUE(NLS::Render::Context::DriverUIAccess::PublishUiOnlyFrame(driver, 128u, 72u));

    auto testQueue = explicitDevice->GetTestQueue();
    testQueue->submitFailureStage = TestQueue::SubmitFailureStage::BeforeQueueWork;
    testQueue->nextSubmitResult = {
        NLS::Render::RHI::RHIQueueOperationStatusCode::BackendFailure,
        "recoverable UI-only submit failure"
    };
    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    auto* impl = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl, nullptr);
    {
        std::lock_guard lock(impl->pendingUiOverlaySnapshotMutex);
        EXPECT_EQ(impl->lastPublishedSwapchainUiContentSignature, 0u);
        EXPECT_EQ(impl->lastPublishedSwapchainUiInvalidationGeneration, 0u)
            << "A failed submit must not commit UI presentation state.";
    }
    EXPECT_EQ(testQueue->presentCalls, 0u);

    auto& executor = NLS::Render::Context::RhiCommandListExecutor::Get();
    EXPECT_TRUE(executor.HasFailed());
    EXPECT_EQ(executor.GetFailureDiagnostic(), "recoverable UI-only submit failure");
    bool touchedQueueAfterFailure = false;
    EXPECT_FALSE(executor.RunOnSubmitLane("UI retry before recovery", [&touchedQueueAfterFailure]
    {
        touchedQueueAfterFailure = true;
        return NLS::Render::RHI::RHIQueueOperationResult {};
    }).Succeeded());
    EXPECT_FALSE(touchedQueueAfterFailure);
    executor.Initialize(NLS::Render::Context::RhiThreadMode::None,
        NLS::Render::Context::RhiCommandListExecutor::CreateDeviceTargets(explicitDevice), nullptr, &driver);

    testQueue->submitFailureStage = TestQueue::SubmitFailureStage::Legacy;
    testQueue->nextSubmitResult = {};
    auto retrySnapshot = MakeVisibleUiSnapshot(933u, 128.0f, 72.0f);
    NLS::Render::Context::DriverUIAccess::PublishUiDrawDataSnapshot(driver, retrySnapshot);
    ASSERT_TRUE(NLS::Render::Context::DriverUIAccess::PublishUiOnlyFrame(driver, 128u, 72u))
        << "After explicit executor recovery, identical UI content must remain retryable.";
    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    EXPECT_EQ(testQueue->submitCalls, 1u);
    EXPECT_EQ(testQueue->presentCalls, 1u);
    {
        std::lock_guard lock(impl->pendingUiOverlaySnapshotMutex);
        EXPECT_EQ(
            impl->lastPublishedSwapchainUiContentSignature,
            NLS::Render::UI::ResolveUiDrawDataContentSignature(*retrySnapshot));
        EXPECT_NE(impl->lastPublishedSwapchainUiInvalidationGeneration, 0u);
    }
}

TEST(RenderFrameExecutionTests, UiOnlyPresentFailureRequiresExecutorRecoveryBeforeIdenticalRetry)
{
    ImGuiContextGuard imguiContext;
    ScopedOverlayShaderManager shaderManagerScope;
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    explicitDevice->MutableCapabilities().SetFeature(
        NLS::Render::RHI::RHIDeviceFeature::UIOverlayFrameGraph,
        true,
        "test overlay support");
    auto swapchain = std::make_shared<TestSwapchain>();
    swapchain->desc.width = 128u;
    swapchain->desc.height = 72u;
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);
    NLS::Render::Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);
    ConfigureUiOverlayFrameContext(driver, 0u);

    auto firstSnapshot = MakeVisibleUiSnapshot(934u, 128.0f, 72.0f);
    NLS::Render::Context::DriverUIAccess::PublishUiDrawDataSnapshot(driver, firstSnapshot);
    ASSERT_TRUE(NLS::Render::Context::DriverUIAccess::PublishUiOnlyFrame(driver, 128u, 72u));

    auto testQueue = explicitDevice->GetTestQueue();
    testQueue->presentFailureStage = TestQueue::PresentFailureStage::AfterFenceSignal;
    testQueue->nextPresentResult = {
        NLS::Render::RHI::RHIQueueOperationStatusCode::BackendFailure,
        "recoverable UI-only present failure"
    };
    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    auto* impl = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl, nullptr);
    EXPECT_EQ(testQueue->submitCalls, 1u);
    EXPECT_EQ(testQueue->presentCalls, 1u);
    auto& executor = NLS::Render::Context::RhiCommandListExecutor::Get();
    EXPECT_TRUE(executor.HasFailed());
    EXPECT_EQ(executor.GetFailureDiagnostic(), "recoverable UI-only present failure");
    bool touchedQueueAfterFailure = false;
    EXPECT_FALSE(executor.RunOnSubmitLane("UI retry before recovery", [&touchedQueueAfterFailure]
    {
        touchedQueueAfterFailure = true;
        return NLS::Render::RHI::RHIQueueOperationResult {};
    }).Succeeded());
    EXPECT_FALSE(touchedQueueAfterFailure);
    {
        std::lock_guard lock(impl->pendingUiOverlaySnapshotMutex);
        EXPECT_EQ(impl->lastPublishedSwapchainUiContentSignature, 0u);
        EXPECT_EQ(impl->lastPublishedSwapchainUiInvalidationGeneration, 0u)
            << "A failed Present must not commit UI presentation state.";
    }

    testQueue->presentFailureStage = TestQueue::PresentFailureStage::Legacy;
    testQueue->nextPresentResult = {};
    auto retrySnapshot = MakeVisibleUiSnapshot(935u, 128.0f, 72.0f);
    EXPECT_EQ(
        NLS::Render::UI::ResolveUiDrawDataContentSignature(*retrySnapshot),
        NLS::Render::UI::ResolveUiDrawDataContentSignature(*firstSnapshot));
    NLS::Render::Context::DriverUIAccess::PublishUiDrawDataSnapshot(driver, retrySnapshot);
    EXPECT_FALSE(NLS::Render::Context::DriverUIAccess::PublishUiOnlyFrame(driver, 128u, 72u))
        << "A frame context whose Present fence is still pending must not be reused.";
    {
        std::lock_guard lock(impl->pendingUiOverlaySnapshotMutex);
        EXPECT_EQ(impl->pendingUiOverlaySnapshot, retrySnapshot)
            << "Fence backpressure must retain identical UI content for a later retry.";
    }

    ASSERT_EQ(impl->GetFrameContextCount(), 1u);
    auto retryFence = std::dynamic_pointer_cast<TestFence>(impl->GetFrameContext(0).frameFence);
    ASSERT_NE(retryFence, nullptr);
    EXPECT_FALSE(retryFence->IsSignaled());
    ASSERT_TRUE(retryFence->Wait())
        << "The test must simulate completion of the fence queued by the failed Present.";
    executor.Initialize(NLS::Render::Context::RhiThreadMode::None,
        NLS::Render::Context::RhiCommandListExecutor::CreateDeviceTargets(explicitDevice), nullptr, &driver);
    EXPECT_FALSE(impl->frameLatency->GetFailureDiagnostic().empty());
    impl->frameLatency->Reset();

    ASSERT_TRUE(NLS::Render::Context::DriverUIAccess::PublishUiOnlyFrame(driver, 128u, 72u))
        << "After GPU retirement and explicit executor/fence recovery, identical UI content remains retryable.";
    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    EXPECT_EQ(testQueue->submitCalls, 2u);
    EXPECT_EQ(testQueue->presentCalls, 2u);
    {
        std::lock_guard lock(impl->pendingUiOverlaySnapshotMutex);
        EXPECT_EQ(
            impl->lastPublishedSwapchainUiContentSignature,
            NLS::Render::UI::ResolveUiDrawDataContentSignature(*retrySnapshot));
        EXPECT_NE(impl->lastPublishedSwapchainUiInvalidationGeneration, 0u);
    }
}

TEST(RenderFrameExecutionTests, ResizeDuringUiOnlyFrameWaitsForNormalFrameRetirement)
{
    ImGuiContextGuard imguiContext;
    ScopedOverlayShaderManager shaderManagerScope;
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    explicitDevice->MutableCapabilities().SetFeature(
        NLS::Render::RHI::RHIDeviceFeature::UIOverlayFrameGraph,
        true,
        "test overlay support");
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
    frameContext.resourceStateTracker = NLS::Render::RHI::CreateDefaultResourceStateTracker();

    auto uiSnapshot = MakeVisibleUiSnapshot(902u, 128.0f, 72.0f);
    NLS::Render::Context::DriverUIAccess::PublishUiDrawDataSnapshot(driver, uiSnapshot);
    ASSERT_TRUE(NLS::Render::Context::DriverUIAccess::PublishUiOnlyFrame(driver, 128u, 72u));

    driver.ResizePlatformSwapchain(1600u, 900u);
    NLS::Render::Context::DriverTestAccess::AgePendingSwapchainResize(
        driver,
        NLS::Render::Context::GetInteractiveSwapchainResizeDebounce());
    NLS::Render::Context::DriverUIAccess::PresentSwapchain(driver);

    EXPECT_EQ(swapchain->resizeWidth, 0u)
        << "Pending resize must wait while the UI-only frame is still in flight.";

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    EXPECT_EQ(explicitDevice->GetTestQueue()->submitCalls, 1u);
    EXPECT_EQ(explicitDevice->GetTestQueue()->presentCalls, 1u);
    EXPECT_EQ(swapchain->resizeWidth, 1600u);
    EXPECT_EQ(swapchain->resizeHeight, 900u);
}

TEST(RenderFrameExecutionTests, ThreadedOverlayPresentRetriesPendingResizeWithoutRenderWork)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    explicitDevice->MutableCapabilities().SetFeature(
        NLS::Render::RHI::RHIDeviceFeature::UIOverlayFrameGraph,
        true,
        "test overlay support");
    auto swapchain = std::make_shared<TestSwapchain>();
    swapchain->resizeResult = false;
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);
    NLS::Render::Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);

    driver.ResizePlatformSwapchain(1600u, 900u);
    NLS::Render::Context::DriverTestAccess::AgePendingSwapchainResize(
        driver,
        NLS::Render::Context::GetInteractiveSwapchainResizeDebounce());
    NLS::Render::Context::DriverUIAccess::PresentSwapchain(driver);

    EXPECT_EQ(swapchain->resizeCalls, 2u);
    EXPECT_EQ(swapchain->GetDesc().width, 0u);
    EXPECT_EQ(swapchain->GetDesc().height, 0u);

    swapchain->resizeResult = true;
    NLS::Render::Context::DriverTestAccess::AgePendingSwapchainResize(
        driver,
        NLS::Render::Context::GetInteractiveSwapchainResizeDebounce());
    NLS::Render::Context::DriverUIAccess::PresentSwapchain(driver);

    EXPECT_EQ(swapchain->resizeCalls, 3u);
    EXPECT_EQ(swapchain->resizeWidth, 1600u);
    EXPECT_EQ(swapchain->resizeHeight, 900u);
}

TEST(RenderFrameExecutionTests, PresentSwapchainPublishesPendingUiSnapshotWithoutSynchronousSubmission)
{
    ImGuiContextGuard imguiContext;
    ScopedOverlayShaderManager shaderManagerScope;
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    explicitDevice->MutableCapabilities().SetFeature(
        NLS::Render::RHI::RHIDeviceFeature::UIOverlayFrameGraph,
        true,
        "test overlay support");
    auto swapchain = std::make_shared<TestSwapchain>();
    swapchain->desc.width = 128u;
    swapchain->desc.height = 72u;
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
    frameContext.resourceStateTracker = NLS::Render::RHI::CreateDefaultResourceStateTracker();

    auto uiSnapshot = MakeVisibleUiSnapshot(903u, 128.0f, 72.0f);
    NLS::Render::Context::DriverUIAccess::PublishUiDrawDataSnapshot(driver, uiSnapshot);

    EXPECT_EQ(NLS::Render::Context::DriverRendererAccess::GetActiveExplicitCommandBuffer(driver), nullptr);
    NLS::Render::Context::DriverUIAccess::PresentSwapchain(driver);

    auto testQueue = explicitDevice->GetTestQueue();
    EXPECT_EQ(testQueue->submitCalls, 0u)
        << "PresentSwapchain must not synchronously execute threaded RHI work on the caller thread.";
    EXPECT_EQ(testQueue->presentCalls, 0u);
    EXPECT_EQ(swapchain->acquireCalls, 0u);
    EXPECT_EQ(NLS::Render::Context::DriverRendererAccess::GetThreadedFrameTelemetry(driver).inFlightFrameCount, 1u);
    EXPECT_EQ(NLS::Render::Context::DriverUIAccess::ConsumePendingUiDrawDataSnapshot(driver), nullptr)
        << "A successfully published UI-only frame must own the pending snapshot.";

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    EXPECT_EQ(testQueue->submitCalls, 1u);
    EXPECT_EQ(testQueue->presentCalls, 1u);
    EXPECT_EQ(swapchain->acquireCalls, 1u);
    EXPECT_EQ(testQueue->lastSubmitDesc.commandBuffers.size(), 1u);
    EXPECT_EQ(NLS::Render::Context::DriverRendererAccess::GetActiveExplicitCommandBuffer(driver), nullptr);
}

TEST(RenderFrameExecutionTests, PresentSwapchainLeavesPreparedSceneSubmissionToRhiExecution)
{
    ImGuiContextGuard imguiContext;
    ScopedOverlayShaderManager shaderManagerScope;
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 2u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    explicitDevice->MutableCapabilities().SetFeature(
        NLS::Render::RHI::RHIDeviceFeature::UIOverlayFrameGraph,
        true,
        "test overlay support");
    auto swapchain = std::make_shared<TestSwapchain>();
    swapchain->desc.width = 128u;
    swapchain->desc.height = 72u;
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
    frameContext.resourceStateTracker = NLS::Render::RHI::CreateDefaultResourceStateTracker();

    NLS::Render::RHI::RHITextureDesc sampledTextureDesc;
    sampledTextureDesc.extent = {128u, 72u, 1u};
    sampledTextureDesc.format = NLS::Render::RHI::TextureFormat::RGBA8;
    sampledTextureDesc.usage = NLS::Render::RHI::TextureUsageFlags::Sampled;
    sampledTextureDesc.debugName = "PreparedSceneUiTexture";
    auto sampledTexture = std::make_shared<TestTexture>(
        sampledTextureDesc,
        NLS::Render::RHI::ResourceState::ShaderRead);
    auto sampledTextureView = std::make_shared<TestTextureView>(
        sampledTexture,
        NLS::Render::RHI::RHITextureViewDesc{});
    const auto sampledTextureId =
        NLS::Render::Context::DriverUIAccess::RegisterUiTextureView(
            driver,
            sampledTextureView,
            NLS::Render::UI::UiTextureSynchronizationScope::PreviousFrameOrStatic);
    ASSERT_TRUE(sampledTextureId.IsValid());
    const uint64_t sampledTextureIdentity = static_cast<uint64_t>(
        reinterpret_cast<std::uintptr_t>(sampledTexture.get()));

    auto uiSnapshot = MakeVisibleUiSnapshot(904u, 128.0f, 72.0f, sampledTextureId);
    NLS::Render::Context::DriverUIAccess::PublishUiDrawDataSnapshot(driver, uiSnapshot);

    NLS::Render::Context::RenderFramePacket sceneSnapshot;
    sceneSnapshot.frameId = 904u;
    sceneSnapshot.targetsSwapchain = true;
    sceneSnapshot.renderWidth = 128u;
    sceneSnapshot.renderHeight = 72u;

    auto result = NLS::Tests::ObserveNextRenderSceneForTest(driver);
    size_t sceneSlot = 99u;
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
        driver,
        sceneSnapshot,
        [](const size_t)
        {
            NLS::Render::Context::RenderScenePackage package;
            package.frameId = 904u;
            package.targetsSwapchain = true;
            package.renderWidth = 128u;
            package.renderHeight = 72u;
            package.hasVisibleDraws = true;
            package.frameDataReady = true;
            return package;
        },
        &sceneSlot,
        nullptr));

    NLS::Render::Context::DriverUIAccess::PresentSwapchain(driver);

    auto testQueue = explicitDevice->GetTestQueue();
    EXPECT_EQ(testQueue->submitCalls, 0u)
        << "PresentSwapchain must leave an already-published scene frame on the threaded pipeline.";
    EXPECT_EQ(testQueue->presentCalls, 0u);
    EXPECT_EQ(swapchain->acquireCalls, 0u);

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    EXPECT_EQ(testQueue->submitCalls, 1u)
        << "The already-published scene builder must consume the UI snapshot instead of publishing a second "
           "UI-only frame first.";
    EXPECT_EQ(testQueue->presentCalls, 1u);
    EXPECT_EQ(swapchain->acquireCalls, 1u);
    EXPECT_EQ(NLS::Render::Context::DriverUIAccess::ConsumePendingUiDrawDataSnapshot(driver), nullptr);

    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto package = result.get();
    EXPECT_TRUE(package.targetsSwapchain);
    EXPECT_TRUE(package.hasUIOverlayPass);
    EXPECT_EQ(package.uiDrawDataSnapshot, uiSnapshot);
    EXPECT_EQ(
        package.uiReferencedTextureIdentities,
        std::vector<uint64_t>({sampledTextureIdentity}));
}

TEST(RenderFrameExecutionTests, PresentSwapchainPrepublishesUiOnlyFrameAheadOfUnrelatedOffscreenWork)
{
    ImGuiContextGuard imguiContext;
    ScopedOverlayShaderManager shaderManagerScope;
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 2u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    explicitDevice->MutableCapabilities().SetFeature(
        NLS::Render::RHI::RHIDeviceFeature::UIOverlayFrameGraph,
        true,
        "test overlay support");
    auto swapchain = std::make_shared<TestSwapchain>();
    swapchain->desc.width = 128u;
    swapchain->desc.height = 72u;
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);
    NLS::Render::Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);

    for (size_t slotIndex = 0u; slotIndex < 2u; ++slotIndex)
    {
        auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, slotIndex);
        auto commandBuffer = std::make_shared<TestCommandBuffer>();
        auto commandPool = std::make_shared<TestCommandPool>();
        commandPool->commandBuffer = commandBuffer;
        frameContext.commandBuffer = commandBuffer;
        frameContext.commandPool = commandPool;
        frameContext.frameFence = std::make_shared<TestFence>();
        frameContext.imageAcquiredSemaphore = std::make_shared<TestSemaphore>();
        frameContext.renderFinishedSemaphore = std::make_shared<TestSemaphore>();
        frameContext.descriptorAllocator = std::make_shared<TestDescriptorAllocator>();
        frameContext.uploadContext = std::make_shared<TestUploadContext>();
        frameContext.resourceStateTracker = NLS::Render::RHI::CreateDefaultResourceStateTracker();
    }

    auto* impl = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl, nullptr);
    bool uiSnapshotWasPendingWhenOffscreenBuilderRan = false;
    NLS::Render::Context::RenderFramePacket sceneSnapshot;
    sceneSnapshot.frameId = 905u;
    sceneSnapshot.targetsSwapchain = false;
    sceneSnapshot.renderWidth = 128u;
    sceneSnapshot.renderHeight = 72u;
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
        driver,
        sceneSnapshot,
        [&](const size_t)
        {
            {
                std::lock_guard lock(impl->pendingUiOverlaySnapshotMutex);
                uiSnapshotWasPendingWhenOffscreenBuilderRan = impl->pendingUiOverlaySnapshot != nullptr;
            }
            NLS::Render::Context::RenderScenePackage package;
            package.frameId = 905u;
            package.targetsSwapchain = false;
            package.renderWidth = 128u;
            package.renderHeight = 72u;
            package.hasVisibleDraws = true;
            package.frameDataReady = true;
            return package;
        }));

    auto uiSnapshot = MakeVisibleUiSnapshot(905u, 128.0f, 72.0f);
    NLS::Render::Context::DriverUIAccess::PublishUiDrawDataSnapshot(driver, uiSnapshot);
    NLS::Render::Context::DriverUIAccess::PresentSwapchain(driver);

    EXPECT_FALSE(uiSnapshotWasPendingWhenOffscreenBuilderRan)
        << "PresentSwapchain must not synchronously execute the offscreen builder.";
    auto testQueue = explicitDevice->GetTestQueue();
    EXPECT_EQ(testQueue->submitCalls, 0u);
    EXPECT_EQ(testQueue->presentCalls, 0u);
    EXPECT_EQ(swapchain->acquireCalls, 0u);
    EXPECT_EQ(NLS::Render::Context::DriverUIAccess::ConsumePendingUiDrawDataSnapshot(driver), nullptr)
        << "The prepublished UI-only frame must own the pending snapshot.";

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    EXPECT_FALSE(uiSnapshotWasPendingWhenOffscreenBuilderRan)
        << "UI-only work should reserve the free slot before the offscreen scene drain.";
    EXPECT_EQ(testQueue->submitCalls, 1u);
    EXPECT_EQ(testQueue->presentCalls, 1u);
    EXPECT_EQ(swapchain->acquireCalls, 1u);
    EXPECT_EQ(NLS::Render::Context::DriverUIAccess::ConsumePendingUiDrawDataSnapshot(driver), nullptr);
}

TEST(RenderFrameExecutionTests, PresentSwapchainQueuesUiConsumerBehindReferencedOffscreenProducer)
{
    ImGuiContextGuard imguiContext;
    ScopedOverlayShaderManager shaderManagerScope;
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 2u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    explicitDevice->MutableCapabilities().SetFeature(
        NLS::Render::RHI::RHIDeviceFeature::UIOverlayFrameGraph,
        true,
        "test overlay support");
    auto swapchain = std::make_shared<TestSwapchain>();
    swapchain->desc.width = 128u;
    swapchain->desc.height = 72u;
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);
    NLS::Render::Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);
    ConfigureUiOverlayFrameContext(driver, 0u);
    ConfigureUiOverlayFrameContext(driver, 1u);

    NLS::Render::RHI::RHITextureDesc outputTextureDesc;
    outputTextureDesc.extent = {128u, 72u, 1u};
    outputTextureDesc.format = NLS::Render::RHI::TextureFormat::RGBA8;
    outputTextureDesc.usage =
        NLS::Render::RHI::TextureUsageFlags::ColorAttachment |
        NLS::Render::RHI::TextureUsageFlags::Sampled;
    outputTextureDesc.debugName = "ReferencedOffscreenUiTexture";
    auto outputTexture = std::make_shared<TestTexture>(
        outputTextureDesc,
        NLS::Render::RHI::ResourceState::ShaderRead);
    NLS::Render::RHI::RHITextureViewDesc outputViewDesc;
    outputViewDesc.format = NLS::Render::RHI::TextureFormat::RGBA8;
    outputViewDesc.debugName = "ReferencedOffscreenUiTextureView";
    auto outputView = std::make_shared<TestTextureView>(outputTexture, outputViewDesc);
    const auto outputTextureId =
        NLS::Render::Context::DriverUIAccess::RegisterUiTextureView(
            driver,
            outputView,
            NLS::Render::UI::UiTextureSynchronizationScope::PreviousFrameOrStatic);
    ASSERT_TRUE(outputTextureId.IsValid());

    const uint64_t outputIdentity = static_cast<uint64_t>(
        reinterpret_cast<std::uintptr_t>(outputTexture.get()));
    NLS::Render::Context::RenderFramePacket offscreenSnapshot;
    offscreenSnapshot.frameId = 930u;
    offscreenSnapshot.targetsSwapchain = false;
    offscreenSnapshot.hasExternalOutput = true;
    offscreenSnapshot.renderWidth = 128u;
    offscreenSnapshot.renderHeight = 72u;
    offscreenSnapshot.externalOutputIdentity = outputIdentity;
    offscreenSnapshot.externalOutputIdentities = {outputIdentity};
    offscreenSnapshot.externalOutputTextureCount = 1u;
    offscreenSnapshot.externalOutputColorView = outputView;

    NLS::Render::Context::RenderScenePackage offscreenPackage;
    offscreenPackage.frameId = offscreenSnapshot.frameId;
    offscreenPackage.targetsSwapchain = false;
    offscreenPackage.frameDataReady = true;
    offscreenPackage.objectDataReady = true;
    offscreenPackage.renderWidth = 128u;
    offscreenPackage.renderHeight = 72u;
    offscreenPackage.containsCommandInputs = true;
    offscreenPackage.passPlanCount = 1u;
    offscreenPackage.externalSceneOutputIdentity = outputIdentity;
    offscreenPackage.externalSceneOutputIdentities = {outputIdentity};
    offscreenPackage.externalSceneOutputTextureCount = 1u;
    offscreenPackage.externalSceneOutputColorView = outputView;
    offscreenPackage.extractedTextures.push_back(outputTexture);
    NLS::Render::Context::RenderPassCommandInput outputPass;
    outputPass.kind = NLS::Render::Context::RenderPassCommandKind::Helper;
    outputPass.queueType = NLS::Render::RHI::QueueType::Graphics;
    outputPass.debugName = "ReferencedOffscreenProducer";
    outputPass.targetsSwapchain = false;
    outputPass.renderWidth = 128u;
    outputPass.renderHeight = 72u;
    outputPass.clearColor = true;
    outputPass.usesColorAttachment = true;
    outputPass.colorAttachmentViews = {outputView};
    offscreenPackage.passCommandInputs.push_back(std::move(outputPass));

    std::vector<uint64_t> builtFrames;
    std::vector<uint64_t> submittedFrames;
    auto consumerResult = NLS::Tests::ObserveNextRenderSceneForTest(driver, 931u);
    NLS::Render::Context::DriverTestAccess::SetRhiFrameObserver(driver,
        [&submittedFrames](const auto& frame) { submittedFrames.push_back(frame.frameId); });
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
        driver, offscreenSnapshot, [offscreenPackage, &builtFrames](size_t)
        {
            builtFrames.push_back(offscreenPackage.frameId);
            return offscreenPackage;
        }));

    auto uiSnapshot = MakeVisibleUiSnapshot(931u, 128.0f, 72.0f, outputTextureId);
    NLS::Render::Context::DriverUIAccess::PublishUiDrawDataSnapshot(driver, uiSnapshot);
    auto* impl = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl, nullptr);
    uint64_t invalidationGenerationBeforeProducerSubmit = 0u;
    {
        std::lock_guard lock(impl->pendingUiOverlaySnapshotMutex);
        invalidationGenerationBeforeProducerSubmit =
            impl->uiOverlayPresentationInvalidationGeneration;
    }

    NLS::Render::Context::DriverUIAccess::PresentSwapchain(driver);

    EXPECT_TRUE(NLS::Render::Context::Detail::HasPendingSwapchainFrame(*impl))
        << "The UI consumer should enter the FIFO render pipe behind its producer.";
    {
        std::lock_guard lock(impl->pendingUiOverlaySnapshotMutex);
        EXPECT_EQ(impl->pendingUiOverlaySnapshot, nullptr)
            << "A published UI-only consumer must own the pending snapshot.";
    }
    EXPECT_EQ(NLS::Render::Context::DriverRendererAccess::GetThreadedFrameTelemetry(driver).inFlightFrameCount, 2u);
    EXPECT_EQ(consumerResult.wait_for(std::chrono::milliseconds(0)), std::future_status::timeout);

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    auto testQueue = explicitDevice->GetTestQueue();
    EXPECT_EQ(testQueue->submitCalls, 2u);
    EXPECT_EQ(testQueue->presentCalls, 1u);
    EXPECT_EQ(swapchain->acquireCalls, 1u);
    EXPECT_EQ(builtFrames, std::vector<uint64_t>({930u}));
    EXPECT_EQ(submittedFrames, std::vector<uint64_t>({930u, 931u}));
    {
        std::lock_guard lock(impl->pendingUiOverlaySnapshotMutex);
        EXPECT_GT(
            impl->uiOverlayPresentationInvalidationGeneration,
            invalidationGenerationBeforeProducerSubmit)
            << "Only successful offscreen submission may publish a new sampled-texture generation.";
    }
    ASSERT_EQ(consumerResult.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto consumerPackage = consumerResult.get();
    EXPECT_EQ(
        consumerPackage.uiReferencedTextureIdentities,
        std::vector<uint64_t>({outputIdentity}));
}

TEST(RenderFrameExecutionTests, ReportsWhetherInFlightWorkTargetsTheSwapchain)
{
    using namespace NLS::Render::Context;
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 2u;
    Driver driver(settings);
    DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    DriverTestAccess::SetExplicitDevice(driver, std::make_shared<TestExplicitDevice>());
    auto* impl = DriverTestAccess::GetImplForTesting(driver);
    RenderFramePacket offscreenSnapshot;
    offscreenSnapshot.frameId = 906u;
    offscreenSnapshot.targetsSwapchain = false;
    ASSERT_TRUE(DriverRendererAccess::TryEnqueueRenderFrame(driver, offscreenSnapshot,
        [offscreenSnapshot](size_t) { return BuildRenderFramePackage(offscreenSnapshot); }));
    EXPECT_FALSE(Detail::HasPendingSwapchainFrame(*impl));

    RenderFramePacket swapchainSnapshot;
    swapchainSnapshot.frameId = 907u;
    swapchainSnapshot.targetsSwapchain = true;
    ASSERT_TRUE(DriverRendererAccess::TryEnqueueRenderFrame(driver, swapchainSnapshot,
        [swapchainSnapshot](size_t) { return BuildRenderFramePackage(swapchainSnapshot); }));
    EXPECT_TRUE(Detail::HasPendingSwapchainFrame(*impl));
}

TEST(RenderFrameExecutionTests, FailedBackgroundPreviewPublishDoesNotBlockLaterSceneFrames)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    NLS::Render::Context::RenderFramePacket occupiedSnapshot;
    occupiedSnapshot.frameId = 908u;
    occupiedSnapshot.targetsSwapchain = false;
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
        driver,
        occupiedSnapshot,
        [](const size_t)
        {
            return NLS::Render::Context::RenderScenePackage{};
        }));

    NLS::Render::Context::RenderFramePacket previewSnapshot;
    previewSnapshot.frameId = 909u;
    previewSnapshot.targetsSwapchain = false;
    EXPECT_FALSE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
        driver,
        previewSnapshot,
        [](const size_t)
        {
            return NLS::Render::Context::RenderScenePackage{};
        },
        nullptr,
        nullptr,
        true));

    auto* impl = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl, nullptr);
    EXPECT_FALSE(impl->backgroundPreviewPublicationRequested.load(std::memory_order_acquire))
        << "A failed background preview attempt must release its publication priority.";

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    NLS::Render::Context::RenderFramePacket nextSceneSnapshot;
    nextSceneSnapshot.frameId = 910u;
    nextSceneSnapshot.targetsSwapchain = true;
    EXPECT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
        driver,
        nextSceneSnapshot,
        [](const size_t)
        {
            return NLS::Render::Context::RenderScenePackage{};
        }))
        << "A failed background preview attempt must not reject subsequent normal scene publication.";
}

TEST(RenderFrameExecutionTests, ThreadedPreparedFramePublishesCompletedPreferredReadbackTexture)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

    NLS::Render::Context::RenderFramePacket snapshot;
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
    package.preferredReadbackTextureGeneration =
        NLS::Render::Context::DriverRendererAccess::BeginReadbackTextureSubmission(
            driver,
            preferredReadbackTexture);
    ASSERT_NE(package.preferredReadbackTextureGeneration, 0u);
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

    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, snapshot, [package](size_t) { return package; }));

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    EXPECT_EQ(
        NLS::Render::Context::DriverRendererAccess::ResolveReadbackTexture(driver),
        preferredReadbackTexture);
    EXPECT_TRUE(NLS::Render::Context::DriverRendererAccess::HasCompletedReadbackTexture(
        driver,
        preferredReadbackTexture,
        package.preferredReadbackTextureGeneration));
}

TEST(RenderFrameExecutionTests, CompletedReadbackHistoryRetainsPreviousTextureForExplicitReadbackConsumers)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

TEST(RenderFrameExecutionTests, DriverReadPixelsCheckedPropagatesExplicitReadbackFailure)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

TEST(RenderFrameExecutionTests, DriverReadPixelsCheckedReportsWaitPreviewFenceStage)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    auto completion = std::make_shared<TestCompletionToken>(
        NLS::Render::RHI::RHICompletionStatus{
            NLS::Render::RHI::RHICompletionStatusCode::Success,
            {}
        },
        std::chrono::microseconds{100});
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

    NLS::Base::Profiling::PerformanceStageStats stats;
    uint8_t pixel[3] {};
    NLS::Render::RHI::RHIReadbackResult result;
    {
        NLS::Base::Profiling::PerformanceStageStatsCapture capture(stats);
        result = NLS::Render::Context::DriverRendererAccess::ReadPixelsChecked(
            driver,
            pickingTexture,
            0u,
            0u,
            1u,
            1u,
            NLS::Render::Settings::EPixelDataFormat::RGB,
            NLS::Render::Settings::EPixelDataType::UNSIGNED_BYTE,
            pixel);
    }

    EXPECT_EQ(result.code, NLS::Render::RHI::RHIReadbackStatusCode::Success);
    EXPECT_EQ(explicitDevice->lastReadPixelsTexture, pickingTexture);
    EXPECT_EQ(completion->waitCalls, 1u);

    const auto snapshot = stats.Snapshot();
    const auto* waitStage = FindStage(
        snapshot,
        NLS::Base::Profiling::PerformanceStageDomain::Thumbnail,
        "WaitPreviewFence");
    ASSERT_NE(waitStage, nullptr);
    EXPECT_EQ(waitStage->callCount, 1u);
    EXPECT_GT(waitStage->mainThreadDuration.count(), 0);
    EXPECT_EQ(waitStage->backgroundThreadDuration.count(), 0);
}

TEST(RenderFrameExecutionTests, DriverReadPixelsCheckedMarksDeviceLostWhenCompletionDetectsDeviceRemoved)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

TEST(RenderFrameExecutionTests, DriverBeginReadPixelsReturnsCompletionTokenWithoutWaiting)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

TEST(RenderFrameExecutionTests, DriverBeginReadPixelsRejectsDeviceLostBeforeTouchingExplicitDevice)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

TEST(RenderFrameExecutionTests, DriverBeginReadPixelsMarksDeviceLostWhenReadbackDetectsDeviceRemoved)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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

TEST(RenderFrameExecutionTests, DriverBeginReadPixelsRejectsUnsafeGpuQuarantineBeforeTouchingExplicitDevice)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
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
TEST(RenderFrameExecutionTests, DriverBeginReadPixelsUsesOrderedSubmitLaneInEveryMode)
{
    using namespace NLS::Render;
    for (const auto mode : {Context::RhiThreadMode::None,
         Context::RhiThreadMode::DedicatedThread, Context::RhiThreadMode::Tasks})
    {
        SCOPED_TRACE(static_cast<int>(mode));
        Settings::DriverSettings settings;
        settings.graphicsBackend = Settings::EGraphicsBackend::NONE;
        settings.enableExplicitRHI = false;
        settings.framesInFlight = 1u;
        Context::Driver driver(settings);
        auto device = std::make_shared<TestExplicitDevice>();
        Context::DriverTestAccess::SetExplicitDevice(driver, device);
        auto* impl = Context::DriverTestAccess::GetImplForTesting(driver);
        auto& executor = Context::RhiCommandListExecutor::Get();
        executor.Initialize(mode, Context::RhiCommandListExecutor::CreateDeviceTargets(device), impl->taskGraph, &driver);
        std::promise<void> submitting;
        std::promise<void> release;
        auto started = submitting.get_future();
        auto released = release.get_future().share();
        auto preceding = std::async(std::launch::async, [&executor, &submitting, released]
        {
            return executor.RunOnSubmitLane("BeforeReadback", [&submitting, released]
            {
                submitting.set_value();
                (void)released.wait_for(std::chrono::seconds(3));
                return RHI::RHIQueueOperationResult {};
            });
        });
        EXPECT_EQ(started.wait_for(std::chrono::seconds(2)), std::future_status::ready);
        std::atomic_bool readbackBegan = false;
        device->onBeginReadPixels = [&readbackBegan]
        {
            EXPECT_TRUE(Context::RenderingThreadSystem::IsInRhiThread());
            readbackBegan.store(true);
        };
        RHI::RHITextureDesc desc;
        desc.extent = {1u, 1u, 1u};
        auto texture = std::make_shared<TestTexture>(desc);
        Context::DriverTestAccess::SetCompletedReadbackTexture(driver, texture);
        const auto beforeReadback = executor.GetDispatchSequence();
        uint8_t pixel[3] {};
        auto readback = std::async(std::launch::async, [&driver, &pixel]
        {
            return Context::DriverRendererAccess::BeginReadPixels(driver,
                0u, 0u, 1u, 1u, Settings::EPixelDataFormat::RGB,
                Settings::EPixelDataType::UNSIGNED_BYTE, pixel);
        });
        for (size_t attempt = 0u; attempt < 200u && executor.GetDispatchSequence() == beforeReadback; ++attempt)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        EXPECT_GT(executor.GetDispatchSequence(), beforeReadback);
        EXPECT_FALSE(readbackBegan.load());
        EXPECT_EQ(readback.wait_for(std::chrono::milliseconds(1)), std::future_status::timeout);
        release.set_value();
        EXPECT_TRUE(preceding.get().Succeeded());
        const auto result = readback.get();
        EXPECT_TRUE(result.Succeeded()) << result.message;
        EXPECT_EQ(result.completion, device->beginReadPixelsResult.completion);
        EXPECT_TRUE(readbackBegan.load());
        EXPECT_FALSE(executor.HasFailed());
    }
}

TEST(RenderFrameExecutionTests, DriverBeginReadPixelsOrdersAfterPendingRenderCommandsInEveryMode)
{
    using namespace NLS::Render;
    for (const auto mode : {Context::RhiThreadMode::None,
         Context::RhiThreadMode::DedicatedThread, Context::RhiThreadMode::Tasks})
    {
        for (const bool explicitTexture : {false, true})
        {
            SCOPED_TRACE(static_cast<int>(mode));
            SCOPED_TRACE(explicitTexture);
            Settings::DriverSettings settings;
            settings.graphicsBackend = Settings::EGraphicsBackend::NONE;
            settings.enableExplicitRHI = false;
            Context::Driver driver(settings);
            auto device = std::make_shared<TestExplicitDevice>();
            Context::DriverTestAccess::SetExplicitDevice(driver, device);
            auto* impl = Context::DriverTestAccess::GetImplForTesting(driver);
            auto& executor = Context::RhiCommandListExecutor::Get();
            executor.Initialize(mode, Context::RhiCommandListExecutor::CreateDeviceTargets(device),
                impl->taskGraph, &driver);
            RHI::RHITextureDesc desc;
            desc.extent = {1u, 1u, 1u};
            auto texture = std::make_shared<TestTexture>(desc);
            std::promise<void> enteredRender;
            std::promise<void> releaseRender;
            auto entered = enteredRender.get_future();
            auto released = releaseRender.get_future().share();
            std::atomic_bool submitted = false;
            std::atomic_bool readbackBegan = false;
            Context::EnqueueRenderCommand<struct PendingFrameBeforeReadback>([&]
            {
                EXPECT_TRUE(Context::RenderingThreadSystem::IsInRenderingThread());
                enteredRender.set_value();
                (void)released.wait_for(std::chrono::seconds(3));
                const auto result = executor.RunOnSubmitLane("FrameBeforeReadback", [&]
                {
                    submitted.store(true);
                    return RHI::RHIQueueOperationResult {};
                });
                EXPECT_TRUE(result.Succeeded()) << result.message;
                Context::DriverTestAccess::SetCompletedReadbackTexture(driver, texture);
            });
            device->onBeginReadPixels = [&]
            {
                EXPECT_TRUE(Context::RenderingThreadSystem::IsInRhiThread());
                EXPECT_TRUE(submitted.load());
                readbackBegan.store(true);
            };
            uint8_t pixel[4] {};
            auto readback = std::async(std::launch::async, [&]
            {
                if (explicitTexture)
                    return Context::DriverRendererAccess::BeginReadPixels(driver, texture,
                        0u, 0u, 1u, 1u, Settings::EPixelDataFormat::RGBA,
                        Settings::EPixelDataType::UNSIGNED_BYTE, pixel);
                return Context::DriverRendererAccess::BeginReadPixels(driver,
                    0u, 0u, 1u, 1u, Settings::EPixelDataFormat::RGBA,
                    Settings::EPixelDataType::UNSIGNED_BYTE, pixel);
            });
            EXPECT_EQ(entered.wait_for(std::chrono::seconds(2)), std::future_status::ready);
            EXPECT_FALSE(readbackBegan.load());
            EXPECT_EQ(readback.wait_for(std::chrono::milliseconds(10)), std::future_status::timeout);
            releaseRender.set_value();
            const auto result = readback.get();
            (void)Context::RenderCommandDispatcher::Dispatch();
            EXPECT_TRUE(result.Succeeded()) << result.message;
            EXPECT_TRUE(readbackBegan.load());
            EXPECT_EQ(device->lastReadPixelsTexture, texture);
            EXPECT_FALSE(executor.HasFailed());
        }
    }
}

TEST(RenderFrameExecutionTests, DriverBeginReadPixelsReportsPrecedingRenderFailureWithoutTouchingDevice)
{
    using namespace NLS::Render;
    NLS::Tests::ScopedRenderFailureReset resetFailure;
    Settings::DriverSettings settings;
    settings.graphicsBackend = Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    Context::Driver driver(settings);
    auto device = std::make_shared<TestExplicitDevice>();
    Context::DriverTestAccess::SetExplicitDevice(driver, device);
    RHI::RHITextureDesc desc;
    desc.extent = {1u, 1u, 1u};
    auto texture = std::make_shared<TestTexture>(desc);
    Context::EnqueueRenderCommand<struct FailedFrameBeforeReadback>([]
    {
        throw std::runtime_error("preceding frame failed");
    });
    uint8_t pixel[4] {};
    const auto result = Context::DriverRendererAccess::BeginReadPixels(driver, texture,
        0u, 0u, 1u, 1u, Settings::EPixelDataFormat::RGBA,
        Settings::EPixelDataType::UNSIGNED_BYTE, pixel);
    EXPECT_FALSE(result.Succeeded());
    EXPECT_NE(result.message.find("preceding frame failed"), std::string::npos);
    EXPECT_EQ(device->lastReadPixelsTexture, nullptr);
}

TEST(RenderFrameExecutionTests, FrameResourceLeaseReleaseIgnoresAnOlderGeneration)
{
    using namespace NLS::Render::Context;
    DriverImpl impl;
    impl.EnsureFrameContexts(1u);
    impl.frameLatency = std::make_unique<FrameLatencyController>(1u);
    const auto firstFrame = impl.frameLatency->ReserveFrame();
    ASSERT_TRUE(firstFrame.has_value());
    auto* first = impl.frameResourceSlots->AcquireSlot(0u);
    ASSERT_NE(first, nullptr);
    const auto firstGeneration = first->leaseGeneration;
    Detail::ReleaseUnsubmittedFrameResourceLease(impl, 0u, firstGeneration, *firstFrame);
    EXPECT_EQ(impl.frameLatency->GetInFlightFrameCount(), 0u);
    const auto secondFrame = impl.frameLatency->ReserveFrame();
    ASSERT_TRUE(secondFrame.has_value());
    auto* second = impl.frameResourceSlots->AcquireSlot(0u);
    ASSERT_NE(second, nullptr);
    const auto secondGeneration = second->leaseGeneration;
    Detail::ReleaseUnsubmittedFrameResourceLease(impl, 0u, firstGeneration, *firstFrame);
    EXPECT_FALSE(impl.frameResourceSlots->IsSlotAvailable(0u));
    EXPECT_EQ(impl.frameLatency->GetInFlightFrameCount(), 1u);
    Detail::ReleaseUnsubmittedFrameResourceLease(impl, 0u, secondGeneration, *secondFrame);
    EXPECT_TRUE(impl.frameResourceSlots->IsSlotAvailable(0u));
    EXPECT_EQ(impl.frameLatency->GetInFlightFrameCount(), 0u);
}

TEST(RenderFrameExecutionTests, AllocatorRetirementUsesEachFramesIndexWithOverlappingCpuFrames)
{
    using namespace NLS::Render;
    Settings::DriverSettings settings;
    settings.graphicsBackend = Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 2u;
    Context::Driver driver(settings);
    Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    Context::DriverTestAccess::SetExplicitDevice(driver, std::make_shared<TestExplicitDevice>());
    auto* impl = Context::DriverTestAccess::GetImplForTesting(driver);
    std::shared_ptr<TestDescriptorAllocator> allocators[2];
    std::shared_ptr<TestUploadContext> uploads[2];
    Context::RenderScenePackage packages[2];
    for (size_t index = 0u; index < 2u; ++index)
    {
        auto& context = impl->GetFrameContext(index);
        context.frameFence.reset();
        context.descriptorAllocator = allocators[index] = std::make_shared<TestDescriptorAllocator>();
        context.uploadContext = uploads[index] = std::make_shared<TestUploadContext>();
        packages[index].frameId = 200u + index;
        packages[index].targetsSwapchain = true;
        ASSERT_NE(Context::Detail::BeginThreadedRhiFrame(*impl, packages[index], index, nullptr, nullptr, nullptr), nullptr);
        EXPECT_EQ(context.frameIndex, index);
        EXPECT_EQ(allocators[index]->lastBeginFrameIndex, index);
        EXPECT_EQ(uploads[index]->lastBeginFrameIndex, index);
    }
    for (const auto index : {1u, 0u})
    {
        Context::Detail::AsyncComputeSubmitPlan plan;
        Context::RhiSubmissionFrame submission;
        Context::Detail::ExecuteThreadedSubmitPlan(*impl, impl->GetFrameContext(index),
            packages[index], plan, &submission, index);
        EXPECT_TRUE(submission.submittedSuccessfully);
        EXPECT_FALSE(Context::RhiCommandListExecutor::Get().HasFailed());
        EXPECT_EQ(allocators[index]->endFrameCalls, 1u);
        EXPECT_EQ(uploads[index]->endFrameCalls, 1u);
        EXPECT_EQ(allocators[index]->lastEndFrameIndex, index);
        EXPECT_EQ(uploads[index]->lastEndFrameIndex, index);
        EXPECT_TRUE(impl->frameResourceSlots->IsSlotAvailable(index));
    }
    EXPECT_EQ(impl->nextRhiFrameIndex.load(), 2u);
}

TEST(RenderFrameExecutionTests, FailedFrameBeginPreservesThePreviousGpuResourceLease)
{
    using namespace NLS::Render::Context;
    DriverImpl impl;
    impl.explicitDevice = std::make_shared<TestExplicitDevice>();
    impl.EnsureFrameContexts(1u);
    auto& context = impl.GetFrameContext(0u);
    context.frameIndex = 73u;
    auto fence = std::make_shared<TestFence>();
    fence->waitResults = {false};
    context.frameFence = fence;
    auto* slot = impl.frameResourceSlots->AcquireSlot(0u);
    ASSERT_NE(slot, nullptr);
    const auto generation = slot->leaseGeneration;
    auto resource = std::make_shared<int>(42);
    std::weak_ptr<int> retained = resource;
    slot->ownedResources.push_back(std::move(resource));
    slot->retirementPending = true;
    impl.frameResourceSlots->RetireSlot(0u, fence);
    RenderScenePackage package;
    package.frameId = 301u;
    package.targetsSwapchain = false;
    const auto result = Detail::SubmitThreadedRhiFrame(impl, package, 0u);
    EXPECT_FALSE(result.submittedSuccessfully);
    EXPECT_FALSE(retained.expired());
    EXPECT_FALSE(impl.frameResourceSlots->IsSlotAvailable(0u));
    EXPECT_EQ(slot->leaseGeneration, generation);
    EXPECT_EQ(slot->gpuFence, fence);
    EXPECT_TRUE(slot->retirementPending);
    EXPECT_EQ(impl.nextRhiFrameIndex.load(), 0u);
    fence->signaled = true;
    EXPECT_TRUE(Detail::ReleaseDeferredThreadedFrameScopedResourcesAfterFence(impl, context, 0u));
    EXPECT_TRUE(retained.expired());
}

TEST(RenderFrameExecutionTests, FrameBeginDoesNotWaitOnAnUnusedSubmissionFence)
{
    using namespace NLS::Render::Context;
    DriverImpl impl;
    impl.explicitDevice = std::make_shared<TestExplicitDevice>();
    impl.EnsureFrameContexts(1u);
    auto& context = impl.GetFrameContext(0u);
    auto fence = std::make_shared<TestFence>();
    fence->waitResults = {false};
    context.frameFence = fence;
    auto pool = std::make_shared<TestCommandPool>();
    context.commandPool = pool;
    RenderScenePackage package;
    package.targetsSwapchain = false;
    uint64_t generation = 0u;
    EXPECT_EQ(Detail::BeginThreadedRhiFrame(impl, package, 0u, nullptr, nullptr, nullptr,
        &generation), &context);
    EXPECT_EQ(fence->waitCalls, 0u);
    EXPECT_EQ(fence->resetCalls, 0u);
    EXPECT_EQ(pool->resetCalls, 1u);
    EXPECT_NE(generation, 0u);
    EXPECT_FALSE(impl.frameResourceSlots->IsSlotAvailable(0u));
    Detail::ReleaseUnsubmittedFrameResourceLease(impl, 0u, generation);
}

TEST(RenderFrameExecutionTests, SwapchainAcquireUsesOrderedRhiLaneInEveryMode)
{
    using namespace NLS::Render;
    for (const auto mode : {Context::RhiThreadMode::None,
         Context::RhiThreadMode::DedicatedThread, Context::RhiThreadMode::Tasks})
    {
        SCOPED_TRACE(static_cast<int>(mode));
        Settings::DriverSettings settings;
        settings.graphicsBackend = Settings::EGraphicsBackend::NONE;
        settings.enableExplicitRHI = false;
        settings.framesInFlight = 1u;
        Context::Driver driver(settings);
        auto device = std::make_shared<TestExplicitDevice>();
        Context::DriverTestAccess::SetExplicitDevice(driver, device);
        auto swapchain = std::make_shared<TestSwapchain>();
        Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);
        auto* impl = Context::DriverTestAccess::GetImplForTesting(driver);
        auto& executor = Context::RhiCommandListExecutor::Get();
        executor.Initialize(mode, Context::RhiCommandListExecutor::CreateDeviceTargets(device),
            impl->taskGraph, &driver);

        std::promise<void> presentEntered;
        std::promise<void> releasePresent;
        auto entered = presentEntered.get_future();
        const auto released = releasePresent.get_future().share();
        std::atomic_bool presentProcessed = false;
        auto present = std::async(std::launch::async, [&]
        {
            return executor.RunOnSubmitLane("PresentBeforeAcquire", [&]
            {
                presentEntered.set_value();
                (void)released.wait_for(std::chrono::seconds(3));
                presentProcessed.store(true);
                return RHI::RHIQueueOperationResult {};
            }, true);
        });
        EXPECT_EQ(entered.wait_for(std::chrono::seconds(2)), std::future_status::ready);
        std::atomic_bool imageAcquired = false;
        std::atomic_bool viewResolved = false;
        swapchain->onAcquire = [&]
        {
            EXPECT_TRUE(Context::RenderingThreadSystem::IsInRhiThread());
            EXPECT_TRUE(presentProcessed.load());
            imageAcquired.store(true);
        };
        swapchain->onGetBackbufferView = [&]
        {
            EXPECT_TRUE(Context::RenderingThreadSystem::IsInRhiThread());
            EXPECT_TRUE(imageAcquired.load());
            viewResolved.store(true);
        };
        Context::RenderScenePackage package;
        package.frameId = 602u;
        package.targetsSwapchain = true;
        uint64_t generation = 0u;
        const auto beforeAcquire = executor.GetDispatchSequence();
        auto beginFrame = std::async(std::launch::async, [&]
        {
            return NLS::Tests::RunOnRenderThread([&]
            {
                return Context::Detail::BeginThreadedRhiFrame(*impl, package, 0u,
                    nullptr, nullptr, nullptr, &generation);
            });
        });
        for (size_t attempt = 0u; attempt < 200u && executor.GetDispatchSequence() == beforeAcquire; ++attempt)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        EXPECT_GT(executor.GetDispatchSequence(), beforeAcquire);
        EXPECT_FALSE(imageAcquired.load());
        EXPECT_FALSE(viewResolved.load());
        EXPECT_EQ(beginFrame.wait_for(std::chrono::milliseconds(1)), std::future_status::timeout);
        releasePresent.set_value();
        EXPECT_TRUE(present.get().Succeeded());
        EXPECT_EQ(beginFrame.get(), &impl->GetFrameContext(0u));
        EXPECT_TRUE(imageAcquired.load());
        EXPECT_TRUE(viewResolved.load());
        EXPECT_EQ(swapchain->acquireCalls, 1u);
        EXPECT_EQ(impl->GetFrameContext(0u).swapchainImageIndex, 1u);
        EXPECT_EQ(impl->GetFrameContext(0u).swapchainBackbufferView, swapchain->backbufferView);
        EXPECT_FALSE(executor.HasFailed()) << executor.GetFailureDiagnostic();
        Context::Detail::ReleaseUnsubmittedFrameResourceLease(*impl, 0u, generation);
    }
}

TEST(RenderFrameExecutionTests, UnavailableSwapchainImageLeavesTheExecutorUsableInEveryMode)
{
    using namespace NLS::Render;
    for (const auto mode : {Context::RhiThreadMode::None,
         Context::RhiThreadMode::DedicatedThread, Context::RhiThreadMode::Tasks})
    {
        SCOPED_TRACE(static_cast<int>(mode));
        Settings::DriverSettings settings;
        settings.graphicsBackend = Settings::EGraphicsBackend::NONE;
        settings.enableExplicitRHI = false;
        settings.framesInFlight = 1u;
        Context::Driver driver(settings);
        auto device = std::make_shared<TestExplicitDevice>();
        Context::DriverTestAccess::SetExplicitDevice(driver, device);
        auto swapchain = std::make_shared<TestSwapchain>();
        swapchain->acquireResult = false;
        Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);
        auto* impl = Context::DriverTestAccess::GetImplForTesting(driver);
        auto& executor = Context::RhiCommandListExecutor::Get();
        executor.Initialize(mode, Context::RhiCommandListExecutor::CreateDeviceTargets(device),
            impl->taskGraph, &driver);
        bool resolvedView = false;
        swapchain->onGetBackbufferView = [&] { resolvedView = true; };
        Context::RenderScenePackage package;
        package.targetsSwapchain = true;
        uint64_t generation = 0u;
        EXPECT_EQ(NLS::Tests::RunOnRenderThread([&]
        {
            return Context::Detail::BeginThreadedRhiFrame(*impl, package, 0u,
                nullptr, nullptr, nullptr, &generation);
        }), &impl->GetFrameContext(0u));
        EXPECT_EQ(swapchain->acquireCalls, 1u);
        EXPECT_FALSE(resolvedView);
        EXPECT_FALSE(impl->GetFrameContext(0u).hasAcquiredSwapchainImage);
        EXPECT_EQ(impl->GetFrameContext(0u).swapchainBackbufferView, nullptr);
        EXPECT_FALSE(executor.HasFailed());
        EXPECT_TRUE(executor.RunOnSubmitLane("AfterUnavailableImage", []
        {
            return RHI::RHIQueueOperationResult {};
        }).Succeeded());
        Context::Detail::ReleaseUnsubmittedFrameResourceLease(*impl, 0u, generation);
    }
}

TEST(RenderFrameExecutionTests, SwapchainAcquireExceptionsCancelLaterWorkAndQuarantineTheLease)
{
    using namespace NLS::Render;
    for (const auto mode : {Context::RhiThreadMode::None,
         Context::RhiThreadMode::DedicatedThread, Context::RhiThreadMode::Tasks})
    {
        for (const bool failView : {false, true})
        {
            SCOPED_TRACE(static_cast<int>(mode));
            SCOPED_TRACE(failView);
            Settings::DriverSettings settings;
            settings.graphicsBackend = Settings::EGraphicsBackend::NONE;
            settings.enableExplicitRHI = false;
            settings.framesInFlight = 1u;
            Context::Driver driver(settings);
            auto device = std::make_shared<TestExplicitDevice>();
            Context::DriverTestAccess::SetExplicitDevice(driver, device);
            auto swapchain = std::make_shared<TestSwapchain>();
            Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);
            auto* impl = Context::DriverTestAccess::GetImplForTesting(driver);
            auto& executor = Context::RhiCommandListExecutor::Get();
            executor.Initialize(mode, Context::RhiCommandListExecutor::CreateDeviceTargets(device),
                impl->taskGraph, &driver);
            const auto fail = [] { throw std::runtime_error("swapchain acquisition failed"); };
            if (failView)
                swapchain->onGetBackbufferView = fail;
            else
                swapchain->onAcquire = fail;
            Context::RenderScenePackage package;
            package.targetsSwapchain = true;
            Context::RhiSubmissionFrame submission;
            uint64_t generation = 0u;
            EXPECT_EQ(NLS::Tests::RunOnRenderThread([&]
            {
                return Context::Detail::BeginThreadedRhiFrame(*impl, package, 0u,
                    &submission, nullptr, nullptr, &generation);
            }), nullptr);
            EXPECT_TRUE(executor.HasFailed());
            EXPECT_NE(executor.GetFailureDiagnostic().find("swapchain acquisition failed"), std::string::npos);
            EXPECT_EQ(submission.commandRecordingFailureCount, 1u);
            EXPECT_TRUE(impl->unsafeGpuWorkQuarantined.load());
            EXPECT_TRUE(impl->frameResourceSlots->PeekSlot(0u)->quarantined);
            EXPECT_FALSE(impl->frameResourceSlots->IsSlotAvailable(0u));
            Context::Detail::ReleaseUnsubmittedFrameResourceLease(*impl, 0u, generation);
            EXPECT_FALSE(impl->frameResourceSlots->IsSlotAvailable(0u));
            bool laterWorkRan = false;
            const auto later = executor.RunOnSubmitLane("AfterFailedAcquire", [&]
            {
                laterWorkRan = true;
                return RHI::RHIQueueOperationResult {};
            });
            EXPECT_FALSE(later.Succeeded());
            EXPECT_FALSE(laterWorkRan);
        }
    }
}

TEST(RenderFrameExecutionTests, RejectedSwapchainAcquireClosesAllocatorsWithoutTouchingTheSwapchain)
{
    using namespace NLS::Render;
    Settings::DriverSettings settings;
    settings.graphicsBackend = Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;
    Context::Driver driver(settings);
    Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto device = std::make_shared<TestExplicitDevice>();
    Context::DriverTestAccess::SetExplicitDevice(driver, device);
    auto swapchain = std::make_shared<TestSwapchain>();
    Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);
    auto* impl = Context::DriverTestAccess::GetImplForTesting(driver);
    auto& context = impl->GetFrameContext(0u);
    auto descriptors = std::make_shared<TestDescriptorAllocator>();
    auto uploads = std::make_shared<TestUploadContext>();
    context.descriptorAllocator = descriptors;
    context.uploadContext = uploads;
    auto& executor = Context::RhiCommandListExecutor::Get();
    executor.NotifyDeviceLost("reject acquisition before device access");
    Context::RenderScenePackage package;
    package.targetsSwapchain = true;
    Context::RhiSubmissionFrame submission;
    uint64_t generation = 0u;
    EXPECT_EQ(NLS::Tests::RunOnRenderThread([&]
    {
        return Context::Detail::BeginThreadedRhiFrame(*impl, package, 0u,
            &submission, nullptr, nullptr, &generation);
    }), nullptr);
    EXPECT_EQ(swapchain->acquireCalls, 0u);
    EXPECT_EQ(descriptors->endFrameCalls, 1u);
    EXPECT_EQ(uploads->endFrameCalls, 1u);
    EXPECT_FALSE(impl->unsafeGpuWorkQuarantined.load());
    EXPECT_EQ(submission.commandRecordingFailureCount, 1u);
    Context::Detail::ReleaseUnsubmittedFrameResourceLease(*impl, 0u, generation);
    EXPECT_TRUE(impl->frameResourceSlots->IsSlotAvailable(0u));
}

TEST(RenderFrameExecutionTests, FrameBeginWaitsForTheResourceRingsFenceBeforeReset)
{
    using namespace NLS::Render::Context;
    DriverImpl impl;
    impl.explicitDevice = std::make_shared<TestExplicitDevice>();
    impl.EnsureFrameContexts(1u);
    auto& context = impl.GetFrameContext(0u);
    context.frameIndex = 41u;
    auto unusedFence = std::make_shared<TestFence>();
    unusedFence->signaled = true;
    context.frameFence = unusedFence;
    auto pool = std::make_shared<TestCommandPool>();
    context.commandPool = pool;
    auto allocator = std::make_shared<TestDescriptorAllocator>();
    context.descriptorAllocator = allocator;
    auto retirementFence = std::make_shared<TestFence>();
    retirementFence->waitResults = {false};
    auto* slot = impl.frameResourceSlots->AcquireSlot(0u);
    ASSERT_NE(slot, nullptr);
    const auto oldGeneration = slot->leaseGeneration;
    auto resource = std::make_shared<int>(42);
    std::weak_ptr<int> retained = resource;
    slot->ownedResources.push_back(std::move(resource));
    slot->retirementPending = true;
    impl.frameResourceSlots->RetireSlot(0u, retirementFence);
    RenderScenePackage package;
    package.targetsSwapchain = false;
    EXPECT_EQ(Detail::BeginThreadedRhiFrame(impl, package, 0u, nullptr, nullptr, nullptr), nullptr);
    EXPECT_EQ(retirementFence->waitCalls, 1u);
    EXPECT_EQ(unusedFence->waitCalls, 0u);
    EXPECT_EQ(pool->resetCalls, 0u);
    EXPECT_EQ(allocator->beginFrameCalls, 0u);
    EXPECT_FALSE(retained.expired());
    EXPECT_EQ(slot->leaseGeneration, oldGeneration);

    retirementFence->signaled = true;
    uint64_t generation = 0u;
    ASSERT_EQ(Detail::BeginThreadedRhiFrame(impl, package, 0u, nullptr, nullptr, nullptr,
        &generation), &context);
    EXPECT_TRUE(retained.expired());
    EXPECT_GT(generation, oldGeneration);
    EXPECT_EQ(allocator->lastEndFrameIndex, 41u);
    EXPECT_EQ(allocator->endFrameCalls, 1u);
    EXPECT_EQ(pool->resetCalls, 1u);
    Detail::ReleaseUnsubmittedFrameResourceLease(impl, 0u, generation);
}

TEST(RenderFrameExecutionTests, PreparedAdmissionWaitsForTheResourceRingsFence)
{
    using namespace NLS::Render;
    Settings::DriverSettings settings;
    settings.graphicsBackend = Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;
    Context::Driver driver(settings);
    Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    Context::DriverTestAccess::SetExplicitDevice(driver, std::make_shared<TestExplicitDevice>());
    auto* impl = Context::DriverTestAccess::GetImplForTesting(driver);
    auto unusedFence = std::make_shared<TestFence>();
    unusedFence->waitResults = {false};
    impl->GetFrameContext(0u).frameFence = unusedFence;
    auto retirementFence = std::make_shared<TestFence>();
    retirementFence->waitResults = {true};
    impl->frameResourceSlots->RetireSlot(0u, retirementFence);
    const auto reservation = Context::DriverRendererAccess::ReserveReusableFrameContextSlotIndexForPreparedPublication(
        driver, std::chrono::steady_clock::now() + std::chrono::milliseconds(100), true);
    ASSERT_EQ(reservation, std::optional<size_t>(0u));
    EXPECT_EQ(retirementFence->waitCalls, 1u);
    EXPECT_EQ(unusedFence->waitCalls, 0u);
    EXPECT_FALSE(impl->frameResourceSlots->IsSlotAvailable(0u));
    EXPECT_TRUE(Context::DriverRendererAccess::ReleaseReservedFrameContextSlotIndex(driver, 0u));
    EXPECT_TRUE(impl->frameResourceSlots->IsSlotAvailable(0u));
}

TEST(RenderFrameExecutionTests, ReservationReleasePreservesAnOlderGpuLease)
{
    using namespace NLS::Render;
    Settings::DriverSettings settings;
    settings.graphicsBackend = Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;
    Context::Driver driver(settings);
    Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto* impl = Context::DriverTestAccess::GetImplForTesting(driver);
    impl->EnsureFrameContexts(1u);
    ASSERT_EQ(Context::DriverRendererAccess::ReserveReusableFrameContextSlotIndex(driver), std::optional<size_t>(0u));
    auto fence = std::make_shared<TestFence>();
    auto resource = std::make_shared<int>(42);
    std::weak_ptr<int> retained = resource;
    impl->frameResourceSlots->PeekSlot(0u)->ownedResources.push_back(std::move(resource));
    impl->frameResourceSlots->RetireSlot(0u, fence);
    EXPECT_TRUE(Context::DriverRendererAccess::ReleaseReservedFrameContextSlotIndex(driver, 0u));
    EXPECT_EQ(impl->frameResourceSlots->PeekSlot(0u)->gpuFence, fence);
    EXPECT_FALSE(retained.expired());
    EXPECT_FALSE(impl->frameResourceSlots->IsSlotAvailable(0u));
    fence->signaled = true;
}

TEST(RenderFrameExecutionTests, ReservationReleaseIgnoresAReusedLeaseGeneration)
{
    using namespace NLS::Render;
    Settings::DriverSettings settings;
    settings.graphicsBackend = Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;
    Context::Driver driver(settings);
    Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    Context::DriverTestAccess::SetExplicitDevice(driver, std::make_shared<TestExplicitDevice>());
    auto* impl = Context::DriverTestAccess::GetImplForTesting(driver);
    ASSERT_EQ(Context::DriverRendererAccess::ReserveReusableFrameContextSlotIndex(driver), std::optional<size_t>(0u));
    const auto generation = impl->frameResourceSlots->PeekSlot(0u)->leaseGeneration;
    impl->frameResourceSlots->RetireSlot(0u, nullptr);
    const auto* nextLease = impl->frameResourceSlots->AcquireSlot(0u);
    ASSERT_NE(nextLease, nullptr);
    ASSERT_GT(nextLease->leaseGeneration, generation);
    EXPECT_TRUE(Context::DriverRendererAccess::ReleaseReservedFrameContextSlotIndex(driver, 0u));
    EXPECT_FALSE(impl->frameResourceSlots->IsSlotAvailable(0u));
    EXPECT_FALSE(impl->reservedFrameResourceLease.has_value());
    impl->frameResourceSlots->RetireSlot(0u, nullptr);
}

TEST(RenderFrameExecutionTests, RenderFrameCommandDispatchesBeforeRhiAndSwapchainFenceMarkers)
{
    using namespace NLS::Render;
    Settings::DriverSettings settings;
    settings.graphicsBackend = Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;
    Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto device = std::make_shared<TestExplicitDevice>();
    device->MutableCapabilities().supportsParallelCommandRecording = true;
    device->MutableCapabilities().supportsParallelCommandTranslation = true;
    Context::DriverTestAccess::SetExplicitDevice(driver, device);
    Context::DriverTestAccess::SetExplicitSwapchain(driver, std::make_shared<TestSwapchain>());
    ConfigureUiOverlayFrameContext(driver, 0u);
    auto* impl = Context::DriverTestAccess::GetImplForTesting(driver);
    auto& executor = Context::RhiCommandListExecutor::Get();
    executor.Initialize(Context::RhiThreadMode::Tasks,
        Context::RhiCommandListExecutor::CreateDeviceTargets(device), impl->taskGraph, &driver);

    std::promise<void> releaseTranslation;
    const auto released = releaseTranslation.get_future().share();
    device->createdCommandBufferBegin = [released]
    {
        (void)released.wait_for(std::chrono::seconds(3));
    };
    Context::RenderPassCommandInput pass;
    pass.kind = Context::RenderPassCommandKind::Opaque;
    pass.targetsSwapchain = true;
    pass.renderWidth = 64u;
    pass.renderHeight = 64u;
    pass.usesColorAttachment = true;
    pass.drawCount = 1u;
    RHI::RHIRenderTargetLayoutDesc layout;
    layout.colorFormats = {RHI::TextureFormat::RGBA8};
    pass.recordedDrawCommands.push_back({std::make_shared<TestGraphicsPipeline>("FenceFrame", layout),
        nullptr, nullptr, nullptr, std::make_shared<TestBindingSet>("Material"),
        std::make_shared<TestMesh>(), 1u});
    Context::RenderScenePackage package;
    package.targetsSwapchain = true;
    package.renderWidth = 64u;
    package.renderHeight = 64u;
    package.hasVisibleDraws = true;
    package.visibleDrawCount = 1u;
    package.opaqueDrawCount = 1u;
    package.frameDataReady = true;
    package.objectDataReady = true;
    package.containsCommandInputs = true;
    package.passCommandInputs = {pass};
    package.containsParallelCommandWorkUnits = true;
    package.parallelCommandWorkUnits = Context::BuildRecordedDrawCommandWorkUnitsForPass(pass, 0u, 0u);
    package.parallelCommandWorkUnitCount = package.parallelCommandWorkUnits.size();
    Context::RenderFramePacket snapshot;
    snapshot.frameId = 601u;
    snapshot.renderWidth = 64u;
    snapshot.renderHeight = 64u;
    const bool published = Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, snapshot,
        [package = std::move(package)](const size_t) mutable { return std::move(package); });
    Context::RenderCommandFence renderFence;
    Context::RenderCommandFence rhiFence;
    Context::RenderCommandFence swapchainFence;
    renderFence.BeginFence(Context::RenderCommandSyncDepth::Render);
    rhiFence.BeginFence(Context::RenderCommandSyncDepth::Rhi);
    swapchainFence.BeginFence(Context::RenderCommandSyncDepth::Swapchain);
    (void)Context::RenderCommandDispatcher::Dispatch();
    const bool renderComplete = renderFence.IsFenced();
    const bool rhiCompleteBeforeTranslation = rhiFence.IsFenced();
    const bool presentCompleteBeforeTranslation = swapchainFence.IsFenced();
    const auto targetSubmit = rhiFence.GetTargetSubmitSequence();
    const auto targetPresent = swapchainFence.GetTargetPresentSequence();
    releaseTranslation.set_value();

    EXPECT_TRUE(published);
    EXPECT_TRUE(renderComplete);
    EXPECT_GT(targetSubmit, 0u);
    EXPECT_EQ(targetPresent, targetSubmit);
    EXPECT_FALSE(rhiCompleteBeforeTranslation);
    EXPECT_FALSE(presentCompleteBeforeTranslation);
    EXPECT_TRUE(swapchainFence.WaitFor(std::chrono::seconds(3)));
    EXPECT_TRUE(Context::DriverRendererAccess::TryDrainThreadedRendering(driver, false));
    EXPECT_EQ(device->GetTestQueue()->submitCalls, 1u);
    EXPECT_EQ(device->GetTestQueue()->presentCalls, 1u);
    EXPECT_FALSE(executor.HasFailed()) << executor.GetFailureDiagnostic();
}

TEST(RenderFrameExecutionTests, DriverResourceReleaseFlushOnlyWaitsForGpuWhenExplicitlyRequested)
{
    using namespace NLS::Render;
    Settings::DriverSettings settings;
    settings.graphicsBackend = Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;
    Context::Driver driver(settings);
    Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    Context::DriverTestAccess::SetExplicitDevice(driver, std::make_shared<TestExplicitDevice>());
    auto* impl = Context::DriverTestAccess::GetImplForTesting(driver);
    auto fence = std::make_shared<TestFence>();
    auto& frameContext = Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameFence = fence;
    auto resource = std::make_shared<int>(1);
    std::weak_ptr<int> retained = resource;
    impl->EnsureFrameContexts(1u);
    impl->frameResourceSlots->PeekSlot(0u)->ownedResources.push_back(resource);
    impl->frameResourceSlots->PeekSlot(0u)->retirementPending = true;
    impl->frameResourceSlots->RetireSlot(0u, fence);
    resource.reset();

    Context::FlushRenderingCommands(Context::RhiFlushFlags::Submit | Context::RhiFlushFlags::ReleaseResources);
    EXPECT_EQ(fence->waitCalls, 0u);
    EXPECT_FALSE(retained.expired());
    fence->signaled = true;
    Context::FlushRenderingCommands(Context::RhiFlushFlags::ReleaseResources);
    EXPECT_EQ(fence->waitCalls, 0u);
    EXPECT_TRUE(retained.expired());

    resource = std::make_shared<int>(2);
    retained = resource;
    impl->frameResourceSlots->PeekSlot(0u)->ownedResources.push_back(resource);
    impl->frameResourceSlots->PeekSlot(0u)->retirementPending = true;
    resource.reset();
    fence->signaled = false;
    impl->frameResourceSlots->RetireSlot(0u, fence);
    Context::FlushRenderingCommands(Context::RhiFlushFlags::Gpu | Context::RhiFlushFlags::ReleaseResources);
    EXPECT_GT(fence->waitCalls, 0u);
    EXPECT_TRUE(retained.expired());
}

TEST(RenderFrameExecutionTests, DriverBeginReadPixelsExceptionPreservesUnfencedResources)
{
    using namespace NLS::Render;
    Settings::DriverSettings settings;
    settings.graphicsBackend = Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;
    Context::Driver driver(settings);
    auto device = std::make_shared<TestExplicitDevice>();
    size_t beginCalls = 0u;
    device->onBeginReadPixels = [&beginCalls]
    {
        ++beginCalls;
        throw std::runtime_error("readback backend exception");
    };
    Context::DriverTestAccess::SetExplicitDevice(driver, device);
    auto* impl = Context::DriverTestAccess::GetImplForTesting(driver);
    RHI::RHITextureDesc desc;
    desc.extent = {1u, 1u, 1u};
    auto texture = std::make_shared<TestTexture>(desc);
    std::weak_ptr<RHI::RHITexture> retainedTexture = texture;
    uint8_t pixel[3] {};
    const auto begin = [&]
    {
        return Context::DriverRendererAccess::BeginReadPixels(driver, texture,
            0u, 0u, 1u, 1u, Settings::EPixelDataFormat::RGB,
            Settings::EPixelDataType::UNSIGNED_BYTE, pixel);
    };
    const auto result = begin();
    EXPECT_FALSE(result.Succeeded());
    EXPECT_NE(result.message.find("readback backend exception"), std::string::npos);
    EXPECT_TRUE(Context::RhiCommandListExecutor::Get().HasFailed());
    EXPECT_TRUE(impl->unsafeGpuWorkQuarantined.load());
    EXPECT_EQ(impl->unfencedReadbackResources.size(), 1u);
    EXPECT_FALSE(begin().Succeeded());
    EXPECT_EQ(beginCalls, 1u);
    texture.reset();
    device->lastReadPixelsTexture.reset();
    EXPECT_FALSE(retainedTexture.expired());
}

TEST(RenderFrameExecutionTests, PostSubmitBufferReadbackRejectsUnsafeGpuQuarantineBeforeTouchingExplicitDevice)
{
    const auto sourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Rendering/Context/RhiCommandListExecutorFrame.cpp";
    std::ifstream input(sourcePath);
    const std::string source{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    };

    ASSERT_FALSE(source.empty());
    const auto beginPostSubmit = source.find("void BeginPostSubmitBufferReadbacks");
    const auto beginReadBuffer = source.find("BeginReadBuffer(readbackDesc)", beginPostSubmit);
    ASSERT_NE(beginPostSubmit, std::string::npos);
    ASSERT_NE(beginReadBuffer, std::string::npos);

    const auto preReadbackBody = source.substr(beginPostSubmit, beginReadBuffer - beginPostSubmit);
    EXPECT_NE(preReadbackBody.find("deviceLostDetected.load(std::memory_order_acquire)"), std::string::npos);
    EXPECT_NE(preReadbackBody.find("unsafeGpuWorkQuarantined.load(std::memory_order_acquire)"), std::string::npos);
    EXPECT_NE(preReadbackBody.find("post-submit buffer readback rejected because RHI device is lost or GPU work is quarantined"), std::string::npos);
    EXPECT_NE(preReadbackBody.find("request.state->beginAttempted = true"), std::string::npos);
    EXPECT_NE(preReadbackBody.find("request.state->beginSucceeded = false"), std::string::npos);
}

TEST(RenderFrameExecutionTests, PostSubmitTextureReadbackTerminatesOnThreadedFrameEarlyExit)
{
    const auto sourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Rendering/Context/RhiCommandListExecutorFrame.cpp";
    std::ifstream input(sourcePath);
    const std::string source{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    };

    ASSERT_FALSE(source.empty());
    const auto failHelper = source.find("void FailPostSubmitReadbacks");
    const auto submitFrame = source.find("RhiSubmissionFrame Detail::SubmitThreadedRhiFrame");
    ASSERT_NE(failHelper, std::string::npos);
    ASSERT_NE(submitFrame, std::string::npos);

    const auto helperBody = source.substr(failHelper, submitFrame - failHelper);
    EXPECT_NE(helperBody.find("request.state->beginAttempted = true"), std::string::npos);
    EXPECT_NE(helperBody.find("request.state->beginInProgress = false"), std::string::npos);
    EXPECT_NE(helperBody.find("request.state->beginSucceeded = false"), std::string::npos);
    EXPECT_NE(helperBody.find("RHIReadbackStatusCode::BackendFailure"), std::string::npos);
    EXPECT_NE(helperBody.find("request.state->completion.reset()"), std::string::npos);

    // Every frame-dispatch failure must release the readback waiter through abort.
    const auto abortHelper = source.find("void AbortThreadedRhiSubmission");
    ASSERT_NE(abortHelper, std::string::npos);
    const auto abortBody = source.substr(abortHelper, submitFrame - abortHelper);
    EXPECT_NE(abortBody.find("FailPostSubmitReadbacks("), std::string::npos);

    const auto submitBody = source.substr(submitFrame);
    EXPECT_NE(submitBody.find("AbortThreadedRhiSubmission(execution)"), std::string::npos);

    const auto failStageHelper = source.find("void FailThreadedRhiSubmissionStage");
    ASSERT_NE(failStageHelper, std::string::npos);
    const auto failStageBody = source.substr(failStageHelper, abortHelper - failStageHelper);
    EXPECT_NE(failStageBody.find("throw ThreadedRhiSubmissionStageAbort"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Normal (view-independent) versus swapchain batch split.
//
// The renderer records the view-independent prefix of a frame into its own
// batch and only resolves the acquired swapchain image at the boundary, so the
// normal batch can make progress while the ordered lane is still draining the
// previous frame's submit/Present. These tests pin the boundary decision and the
// recording order that follows from it.
// ---------------------------------------------------------------------------

namespace
{
    NLS::Render::Context::ParallelCommandWorkUnit MakeBatchSplitTestWorkUnit(
        const uint64_t index,
        const bool targetsSwapchain,
        std::string debugName)
    {
        NLS::Render::Context::ParallelCommandWorkUnit workUnit;
        workUnit.workUnitIndex = index;
        workUnit.submissionOrder = index;
        workUnit.debugName = std::move(debugName);
        workUnit.commandInput.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
        workUnit.commandInput.debugName = workUnit.debugName;
        workUnit.commandInput.drawCount = 1u;
        workUnit.commandInput.usesColorAttachment = true;
        workUnit.commandInput.renderWidth = 128u;
        workUnit.commandInput.renderHeight = 72u;
        workUnit.commandInput.targetsSwapchain = targetsSwapchain;
        return workUnit;
    }
}

TEST(RenderFrameExecutionTests, SwapchainBatchBoundaryFollowsSubmissionOrder)
{
    using namespace NLS::Render::Context;

    // No work unit consumes the acquired image: the whole frame is the normal batch.
    {
        const std::vector<ParallelCommandWorkUnit> workUnits{
            MakeBatchSplitTestWorkUnit(0u, false, "LightGridInjection"),
            MakeBatchSplitTestWorkUnit(1u, false, "HZB")};
        const std::vector<size_t> submissionOrder{0u, 1u};
        EXPECT_EQ(
            Detail::FindFirstSwapchainDependentWorkUnitPosition(workUnits, submissionOrder),
            submissionOrder.size());
    }

    // The very first work unit already consumes the image: there is no normal batch.
    {
        const std::vector<ParallelCommandWorkUnit> workUnits{
            MakeBatchSplitTestWorkUnit(0u, true, "DeferredGBuffer")};
        const std::vector<size_t> submissionOrder{0u};
        EXPECT_EQ(Detail::FindFirstSwapchainDependentWorkUnitPosition(workUnits, submissionOrder), 0u);
    }

    // Mixed frame: everything before the first swapchain consumer is the normal batch.
    {
        const std::vector<ParallelCommandWorkUnit> workUnits{
            MakeBatchSplitTestWorkUnit(0u, false, "LightGridInjection"),
            MakeBatchSplitTestWorkUnit(1u, false, "HZB"),
            MakeBatchSplitTestWorkUnit(2u, true, "DeferredGBuffer"),
            MakeBatchSplitTestWorkUnit(3u, true, "RHIFrameGraph::UIOverlay")};
        const std::vector<size_t> submissionOrder{0u, 1u, 2u, 3u};
        EXPECT_EQ(Detail::FindFirstSwapchainDependentWorkUnitPosition(workUnits, submissionOrder), 2u);
    }

    // The boundary follows submission order, not work-unit index order: a swapchain
    // consumer scheduled first leaves no room for the normal batch to run ahead.
    {
        const std::vector<ParallelCommandWorkUnit> workUnits{
            MakeBatchSplitTestWorkUnit(0u, false, "ViewIndependent"),
            MakeBatchSplitTestWorkUnit(1u, true, "Swapchain")};
        const std::vector<size_t> submissionOrder{1u, 0u};
        EXPECT_EQ(Detail::FindFirstSwapchainDependentWorkUnitPosition(workUnits, submissionOrder), 0u);
    }

    // A stale index cannot be classified, so it terminates the normal batch rather
    // than being skipped over.
    {
        const std::vector<ParallelCommandWorkUnit> workUnits{
            MakeBatchSplitTestWorkUnit(0u, false, "ViewIndependent")};
        const std::vector<size_t> submissionOrder{0u, 99u};
        EXPECT_EQ(Detail::FindFirstSwapchainDependentWorkUnitPosition(workUnits, submissionOrder), 1u);
    }
}


// BuildParallelCommandWorkUnits normalises the package into the submission order
// the batcher splits on, so a realistic mixed package must still report a normal
// batch that contains only view-independent work.
TEST(RenderFrameExecutionTests, BuildParallelCommandWorkUnitsKeepsTheNormalBatchViewIndependent)
{
    using namespace NLS::Render::Context;

    RenderScenePackage package;
    package.frameId = 908u;
    package.targetsSwapchain = true;
    package.renderWidth = 128u;
    package.renderHeight = 72u;
    package.containsParallelCommandWorkUnits = true;
    package.parallelCommandWorkUnits = {
        MakeBatchSplitTestWorkUnit(0u, false, "LightGridInjection"),
        MakeBatchSplitTestWorkUnit(1u, false, "HZB"),
        MakeBatchSplitTestWorkUnit(2u, true, "DeferredGBuffer"),
        MakeBatchSplitTestWorkUnit(3u, true, "RHIFrameGraph::UIOverlay")};
    package.parallelCommandWorkUnitCount =
        static_cast<uint64_t>(package.parallelCommandWorkUnits.size());

    const auto workUnits = Detail::BuildParallelCommandWorkUnits(
        package,
        /*parallelRecordingReady*/ true,
        /*parallelTranslationReady*/ true,
        /*allowOrderedSlicedSubmission*/ true,
        /*allowInRenderPassChildCommandRecording*/ true);
    ASSERT_GE(workUnits.size(), 4u);

    std::vector<size_t> submissionOrder;
    submissionOrder.reserve(workUnits.size());
    for (size_t index = 0u; index < workUnits.size(); ++index)
    {
        // BuildParallelCommandWorkUnits is expected to publish a dense, already
        // ordered index set; the batcher relies on that to split the frame.
        EXPECT_EQ(workUnits[index].workUnitIndex, static_cast<uint64_t>(index));
        EXPECT_EQ(workUnits[index].submissionOrder, static_cast<uint64_t>(index));
        submissionOrder.push_back(index);
    }

    const auto swapchainBatchBegin =
        Detail::FindFirstSwapchainDependentWorkUnitPosition(workUnits, submissionOrder);
    ASSERT_EQ(swapchainBatchBegin, 2u);

    for (size_t position = 0u; position < swapchainBatchBegin; ++position)
    {
        const auto& workUnit = workUnits[submissionOrder[position]];
        EXPECT_FALSE(workUnit.commandInput.targetsSwapchain)
            << "the normal batch must not contain swapchain-consuming work units";
    }
    for (size_t position = swapchainBatchBegin; position < submissionOrder.size(); ++position)
    {
        const auto& workUnit = workUnits[submissionOrder[position]];
        EXPECT_TRUE(workUnit.commandInput.targetsSwapchain)
            << "every work unit at or after the boundary consumes the acquired image";
    }
}

namespace
{
    /**
     * Installs a RHI recording observer for the duration of a scope.
     *
     * The observer is global to the RHI command list, so the guard also keeps the
     * hook alive only for the test that needs it.
     */
    class ScopedRhiRecordObserver final
    {
    public:
        explicit ScopedRhiRecordObserver(NLS::Render::Context::RhiCommandList::RecordObserverForTesting observer)
        {
            NLS::Render::Context::RhiCommandList::SetRecordObserverForTesting(std::move(observer));
        }

        ~ScopedRhiRecordObserver()
        {
            NLS::Render::Context::RhiCommandList::ClearRecordObserverForTesting();
        }

        ScopedRhiRecordObserver(const ScopedRhiRecordObserver&) = delete;
        ScopedRhiRecordObserver& operator=(const ScopedRhiRecordObserver&) = delete;
    };
}

/**
 * Behavioural proof that the view-independent batch records while the ordered lane
 * is still blocked on the swapchain image.
 *
 * The lane is blocked by the frame's own acquisition, driven through the production
 * dispatch path: the frame enqueues `AcquireSwapchainImage` at begin, the test's
 * swapchain stalls inside it, and the frame is expected to record the work unit that
 * does not consume the backbuffer anyway. The probe is a RHI recording observer, not
 * a native command buffer: a deferred recorder only flips its own flag in Begin() and
 * touches the native buffer during translation, so a native-begin probe fires after
 * the acquisition regardless of when recording happened.
 */
TEST(RenderFrameExecutionTests, ViewIndependentBatchRecordsWhileAcquisitionIsStalled)
{
    ScopedThreadedRenderingJobSystem jobSystem(2u);
    ASSERT_TRUE(jobSystem.IsInitialized());

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 2u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    explicitDevice->MutableCapabilities().supportsParallelCommandRecording = true;
    explicitDevice->MutableCapabilities().supportsParallelCommandTranslation = true;
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
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(64u);
    ASSERT_NE(frameContext.descriptorAllocator, nullptr);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);

    std::atomic<bool> viewIndependentRecorded { false };
    std::atomic<bool> swapchainBatchRecorded { false };
    std::atomic<bool> acquisitionFinished { false };
    {
        ScopedRhiRecordObserver observer(
            [&viewIndependentRecorded, &swapchainBatchRecorded](
                NLS::Render::Context::RhiCommandList& list, std::string_view)
            {
                if (list.GetDebugName() == "ViewIndependentCompute")
                    viewIndependentRecorded.store(true, std::memory_order_release);
                if (list.GetDebugName() == "SwapchainBatch")
                    swapchainBatchRecorded.store(true, std::memory_order_release);
            });

        // Stall the acquisition on the lane. The swapchain runs this from inside the
        // lane operation the frame enqueued, so the lane stays busy while it is held.
        std::promise<void> releaseAcquisition;
        auto acquisitionGate = releaseAcquisition.get_future().share();
        std::atomic<bool> acquisitionStalled { false };
        swapchain->onAcquire = [&acquisitionGate, &acquisitionStalled, &acquisitionFinished]()
        {
            acquisitionStalled.store(true, std::memory_order_release);
            acquisitionGate.wait();
            acquisitionFinished.store(true, std::memory_order_release);
        };

        const auto computePipeline = std::make_shared<TestComputePipeline>("ViewIndependentCompute");

        NLS::Render::Context::RenderFramePacket snapshot;
        snapshot.frameId = 4201u;
        snapshot.targetsSwapchain = true;
        snapshot.renderWidth = 64u;
        snapshot.renderHeight = 64u;

        ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
            driver,
            snapshot,
            [&computePipeline](const size_t)
            {
                NLS::Render::Context::RenderScenePackage package;
                package.frameId = 4201u;
                package.targetsSwapchain = true;
                package.renderWidth = 64u;
                package.renderHeight = 64u;
                package.hasVisibleDraws = true;
                package.frameDataReady = true;
                package.objectDataReady = true;
                package.containsParallelCommandWorkUnits = true;

                // View-independent: does not touch the backbuffer, so it may record
                // while the frame is still waiting for the acquired image.
                NLS::Render::Context::ParallelCommandWorkUnit computeUnit;
                computeUnit.workUnitIndex = 0u;
                computeUnit.submissionOrder = 0u;
                computeUnit.commandInput.kind = NLS::Render::Context::RenderPassCommandKind::Compute;
                computeUnit.commandInput.queueType = NLS::Render::RHI::QueueType::Graphics;
                computeUnit.commandInput.targetsSwapchain = false;
                computeUnit.commandInput.renderWidth = 64u;
                computeUnit.commandInput.renderHeight = 64u;
                computeUnit.commandInput.debugName = "ViewIndependentCompute";
                computeUnit.commandInput.computeDispatchInputs.push_back({
                    "ViewIndependentCompute",
                    computePipeline,
                    {},
                    1u,
                    1u,
                    1u,
                    {},
                    {},
                    {}
                });
                computeUnit.debugName = "ViewIndependentCompute";
                computeUnit.eligibleForParallelRecording = true;
                computeUnit.eligibleForParallelTranslation = true;

                // Swapchain batch: the first work unit that consumes the acquired image.
                NLS::Render::Context::ParallelCommandWorkUnit swapchainUnit;
                swapchainUnit.workUnitIndex = 1u;
                swapchainUnit.submissionOrder = 1u;
                swapchainUnit.commandInput.kind = NLS::Render::Context::RenderPassCommandKind::Transparent;
                swapchainUnit.commandInput.queueType = NLS::Render::RHI::QueueType::Graphics;
                swapchainUnit.commandInput.targetsSwapchain = true;
                swapchainUnit.commandInput.renderWidth = 64u;
                swapchainUnit.commandInput.renderHeight = 64u;
                swapchainUnit.commandInput.debugName = "SwapchainBatch";
                swapchainUnit.commandInput.usesColorAttachment = true;
                swapchainUnit.debugName = "SwapchainBatch";
                swapchainUnit.eligibleForParallelRecording = true;
                swapchainUnit.eligibleForParallelTranslation = true;

                package.parallelCommandWorkUnits = { computeUnit, swapchainUnit };
                package.parallelCommandWorkUnitCount =
                    static_cast<uint64_t>(package.parallelCommandWorkUnits.size());
                return package;
            }));

        std::thread drainThread([&driver]()
        {
            NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);
        });

        const auto waitUntil = [](const std::atomic<bool>& flag, const std::chrono::milliseconds timeout)
        {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (!flag.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return flag.load(std::memory_order_acquire);
        };

        ASSERT_TRUE(waitUntil(acquisitionStalled, std::chrono::milliseconds(5000)))
            << "the acquisition never reached the lane";
        const bool recordedWhileStalled = waitUntil(viewIndependentRecorded, std::chrono::milliseconds(5000));
        EXPECT_TRUE(recordedWhileStalled)
            << "the frame recorded nothing while the acquisition was stalled, so the "
               "view-independent batch waits for the acquired image again";
        EXPECT_FALSE(acquisitionFinished.load(std::memory_order_acquire))
            << "the acquisition must still be stalled at the moment the view-independent "
               "batch records";
        EXPECT_FALSE(swapchainBatchRecorded.load(std::memory_order_acquire))
            << "the swapchain batch must not record before the image is available";

        releaseAcquisition.set_value();
        drainThread.join();
    }

    EXPECT_TRUE(acquisitionFinished.load(std::memory_order_acquire));
    EXPECT_EQ(swapchain->acquireCalls, 1u)
        << "the swapchain image is acquired exactly once";
}

namespace
{
    class ScopedLaneBlock final
    {
        std::promise<void> m_release;
        std::atomic<bool> m_released { false };

    public:
        ScopedLaneBlock() : gate(m_release.get_future().share()) {}
        ~ScopedLaneBlock() { Release(); }
        void Release()
        {
            if (!m_released.exchange(true))
                m_release.set_value();
        }
        std::shared_future<void> gate;

    };

    bool WaitForFlag(const std::atomic<bool>& flag)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!flag.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return flag.load(std::memory_order_acquire);
    }

    void VerifyForwardSceneOverlap(const bool blockPresent, const NLS::Render::Context::RhiThreadMode mode)
    {
        using namespace NLS::Render;
        ScopedThreadedRenderingJobSystem jobs(2u);
        ASSERT_TRUE(jobs.IsInitialized());
        ScopedOverlayShaderManager shaders(true);
        Settings::DriverSettings settings;
        settings.graphicsBackend = Settings::EGraphicsBackend::NONE;
        settings.enableExplicitRHI = false;
        settings.framesInFlight = 2u;
        settings.rhiThreadMode = Settings::RhiThreadModePreference::ForceDedicatedThread;
        Context::Driver driver(settings);
        const ScopedDriverService driverService(driver);
        auto device = std::make_shared<TestExplicitDevice>();
        device->MutableCapabilities().supportsParallelCommandRecording = true;
        device->MutableCapabilities().supportsParallelCommandTranslation = true;
        auto swapchain = std::make_shared<TestSwapchain>();
        Context::DriverTestAccess::SetExplicitDevice(driver, device);
        Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);
        auto* impl = Context::DriverTestAccess::GetImplForTesting(driver);
        impl->rhiThreadMode = mode;
        Context::RhiCommandListExecutor::Get().Initialize(mode,
            Context::RhiCommandListExecutor::CreateDeviceTargets(device), impl->taskGraph, &driver);
        for (size_t slot = 0u; slot < 2u; ++slot)
        {
            auto& frame = Context::DriverTestAccess::EnsureFrameContext(driver, slot);
            std::dynamic_pointer_cast<TestFence>(frame.frameFence)->completeOnSubmit = true;
        }

        std::mutex eventsMutex;
        std::vector<std::string> events;
        const auto observe = [&](std::string event)
        {
            std::lock_guard lock(eventsMutex);
            events.push_back(std::move(event));
        };
        std::atomic<bool> stalled { false };
        std::atomic<bool> ordinaryRecorded { false };
        std::atomic<uint32_t> ordinaryCount { 0u };
        std::atomic<uint32_t> outputCount { 0u };
        uint32_t acquireCount = 0u;
        uint32_t presentCount = 0u;
        const auto queue = device->GetTestQueue();
        queue->onSubmit = [&](const auto& submit)
        {
            EXPECT_TRUE(Context::RenderingThreadSystem::IsInRhiThread());
            if (!submit.commandBuffers.empty())
                observe("Submit" + std::to_string(presentCount + 1u));
        };
        ScopedLaneBlock block;
        swapchain->onAcquire = [&]
        {
            observe("Acquire" + std::to_string(++acquireCount));
            if (acquireCount > 1u)
                swapchain->backbufferView = MakeTestSwapchainBackbufferView();
            if (!blockPresent && acquireCount == 1u)
            {
                stalled.store(true, std::memory_order_release);
                block.gate.wait();
            }
        };
        queue->onPresent = [&](const auto&)
        {
            EXPECT_TRUE(Context::RenderingThreadSystem::IsInRhiThread());
            observe("PresentBegin" + std::to_string(++presentCount));
            if (blockPresent && presentCount == 1u)
            {
                stalled.store(true, std::memory_order_release);
                block.gate.wait();
            }
            observe("PresentEnd" + std::to_string(presentCount));
        };
        NLS::Engine::SceneSystem::Scene scene;
        LateFrameCommandRenderer<NLS::Engine::Rendering::ForwardSceneRenderer> renderer(driver);
        Entities::Camera camera;
        Data::FrameDescriptor descriptor;
        descriptor.renderWidth = 64u;
        descriptor.renderHeight = 64u;
        descriptor.camera = &camera;
        auto pipeline = std::make_shared<TestGraphicsPipeline>("ForwardSceneDraw");
        auto binding = std::make_shared<TestBindingSet>("ForwardSceneMaterial");
        auto mesh = std::make_shared<TestMesh>();
        const auto publish = [&]
        {
            renderer.AddDescriptor<NLS::Engine::Rendering::BaseSceneRenderer::SceneDescriptor>(
                { scene, std::nullopt, nullptr });
            renderer.BeginFrame(descriptor);
            const auto resourceSlot = Context::DriverRendererAccess::GetReservedFrameContextSlotIndex(driver);
            EXPECT_TRUE(resourceSlot.has_value());
            EXPECT_TRUE(renderer.QueueHelper(pipeline, binding, mesh));
            renderer.DrawFrame();
            renderer.EndFrame();
            return resourceSlot;
        };
        // Release before renderer teardown even if publication throws. The lane's
        // callbacks and their state outlive that teardown.
        auto releaseBeforeDrain = std::shared_ptr<void>(nullptr, [&](void*) { block.Release(); });
        ScopedRhiRecordObserver recordObserver([&](Context::RhiCommandList& list, const std::string_view operation)
        {
            if (operation != "BeginRenderPass")
                return;
            if (list.GetDebugName() == "ForwardHelper")
            {
                const auto count = ++ordinaryCount;
                if (count == (blockPresent ? 2u : 1u))
                    ordinaryRecorded.store(true, std::memory_order_release);
            }
            if (list.GetDebugName() == Context::kSwapchainOutputRenderPassDebugName)
                ++outputCount;
        });
        auto result = NLS::Tests::ObserveNextRenderSceneForTest(driver);
        publish();
        const bool reachedLane = WaitForFlag(stalled);
        EXPECT_TRUE(reachedLane);
        if (reachedLane && blockPresent)
            publish();
        EXPECT_TRUE(WaitForFlag(ordinaryRecorded))
            << "Forward scene recording did not progress while the ordered lane was blocked";
        EXPECT_EQ(outputCount.load(), blockPresent ? 1u : 0u)
            << "The next output batch recorded before its backbuffer was available";
        block.Release();
        Context::DriverRendererAccess::DrainThreadedRendering(driver);
        Context::FlushRenderingCommands(Context::RhiFlushFlags::Swapchain);
        EXPECT_FALSE(Context::RhiCommandListExecutor::Get().HasFailed())
            << Context::RhiCommandListExecutor::Get().GetFailureDiagnostic();
        EXPECT_EQ(outputCount.load(), blockPresent ? 2u : 1u);
        EXPECT_EQ(queue->presentCalls, blockPresent ? 2u : 1u);

        ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
        auto package = result.get();
        ASSERT_GE(package.passCommandInputs.size(), 2u);
        const auto helper = std::find_if(package.passCommandInputs.begin(), package.passCommandInputs.end(),
            [](const auto& pass) { return pass.debugName == "ForwardHelper"; });
        ASSERT_NE(helper, package.passCommandInputs.end());
        for (auto pass = package.passCommandInputs.begin(); pass != std::prev(package.passCommandInputs.end()); ++pass)
            EXPECT_FALSE(pass->targetsSwapchain) << pass->debugName;
        const auto& output = package.passCommandInputs.back();
        EXPECT_EQ(output.kind, Context::RenderPassCommandKind::SwapchainOutput);
        EXPECT_TRUE(output.targetsSwapchain);
        EXPECT_EQ(package.drawCommandCount, 2u);
        ASSERT_EQ(helper->colorAttachmentViews.size(), 1u);
        const auto sceneColor = helper->colorAttachmentViews.front()->GetTexture();
        EXPECT_EQ(output.outputBindingSet->GetDesc().entries.front().textureView->GetTexture(), sceneColor);
        EXPECT_EQ(Context::DriverRendererAccess::ResolveReadbackTexture(driver), swapchain->backbufferView->GetTexture());
        ASSERT_FALSE(package.parallelCommandWorkUnits.empty());
        const auto outputWorkUnitIndex = package.parallelCommandWorkUnits.back().workUnitIndex;
        EXPECT_EQ(package.parallelCommandWorkUnits.back().commandInput.kind, Context::RenderPassCommandKind::SwapchainOutput);
        ASSERT_TRUE(Context::AttachUiOverlaySnapshotToRenderScenePackage(package, MakeVisibleUiSnapshot(5000u, 64.0f, 64.0f)));
        EXPECT_EQ(package.passCommandInputs.back().kind, Context::RenderPassCommandKind::UIOverlay);
        EXPECT_EQ(package.drawCommandCount, 3u);
        const auto& uiEdge = package.parallelCommandWorkUnits.back().incomingDependencyEdges.back();
        EXPECT_EQ(uiEdge.sourceWorkUnitIndex, outputWorkUnitIndex);
        ASSERT_TRUE(uiEdge.sourceTextureAccess.has_value());
        EXPECT_EQ(uiEdge.sourceTextureAccess->texture, nullptr);
        EXPECT_EQ(uiEdge.sourceTextureAccess->state, RHI::ResourceState::RenderTarget);

        const auto eventIndex = [&](const std::string& event)
        {
            const auto found = std::find(events.begin(), events.end(), event);
            EXPECT_NE(found, events.end()) << event;
            return std::distance(events.begin(), found);
        };
        EXPECT_LT(eventIndex("Acquire1"), eventIndex("Submit1"));
        EXPECT_LT(eventIndex("Submit1"), eventIndex("PresentBegin1"));
        if (blockPresent)
        {
            EXPECT_LT(eventIndex("PresentEnd1"), eventIndex("Acquire2"));
            EXPECT_LT(eventIndex("Acquire2"), eventIndex("Submit2"));
            EXPECT_LT(eventIndex("Submit2"), eventIndex("PresentBegin2"));
            const auto& first = Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
            const auto& second = Context::DriverTestAccess::EnsureFrameContext(driver, 1u);
            EXPECT_NE(first.sceneColorTexture, second.sceneColorTexture);
            EXPECT_NE(first.sceneDepthStencilTexture, second.sceneDepthStencilTexture);
        }
        queue->onPresent = {};
        queue->onSubmit = {};
        swapchain->onAcquire = {};
        if (blockPresent)
        {
            const std::array colors {
                Context::DriverTestAccess::EnsureFrameContext(driver, 0u).sceneColorTexture,
                Context::DriverTestAccess::EnsureFrameContext(driver, 1u).sceneColorTexture };
            const std::array bindings {
                Context::DriverTestAccess::EnsureFrameContext(driver, 0u).sceneOutputBindingSet,
                Context::DriverTestAccess::EnsureFrameContext(driver, 1u).sceneOutputBindingSet };
            const auto reusedSlot = publish();
            ASSERT_TRUE(reusedSlot.has_value());
            Context::DriverRendererAccess::DrainThreadedRendering(driver);
            EXPECT_EQ(Context::DriverTestAccess::EnsureFrameContext(driver, *reusedSlot).sceneColorTexture, colors.at(*reusedSlot));
            EXPECT_EQ(Context::DriverTestAccess::EnsureFrameContext(driver, *reusedSlot).sceneOutputBindingSet, bindings.at(*reusedSlot));
            descriptor.renderWidth = 80u;
            descriptor.renderHeight = 48u;
            const auto resizedSlot = publish();
            ASSERT_TRUE(resizedSlot.has_value());
            Context::DriverRendererAccess::DrainThreadedRendering(driver);
            const auto& resized = Context::DriverTestAccess::EnsureFrameContext(driver, *resizedSlot);
            ASSERT_NE(resized.sceneColorTexture, nullptr);
            EXPECT_NE(resized.sceneColorTexture, colors.at(*resizedSlot));
            EXPECT_EQ(resized.sceneColorTexture->GetDesc().extent.width, 80u);
            EXPECT_EQ(resized.sceneDepthStencilTexture->GetDesc().extent.height, 48u);
        }
    }
}

TEST(RenderFrameExecutionTests, ForwardSceneRecordsWhileSwapchainAcquisitionIsStalled)
{
    for (const auto mode : { NLS::Render::Context::RhiThreadMode::DedicatedThread, NLS::Render::Context::RhiThreadMode::Tasks })
    {
        SCOPED_TRACE(static_cast<int>(mode));
        VerifyForwardSceneOverlap(false, mode);
    }
}

TEST(RenderFrameExecutionTests, NextForwardSceneRecordsWhilePreviousPresentIsStalled)
{
    for (const auto mode : { NLS::Render::Context::RhiThreadMode::DedicatedThread, NLS::Render::Context::RhiThreadMode::Tasks })
    {
        SCOPED_TRACE(static_cast<int>(mode));
        VerifyForwardSceneOverlap(true, mode);
    }
}

TEST(RenderFrameExecutionTests, NextFrameUsesRecordedResourceStateWhilePreviousSubmitIsStalled)
{
    using namespace NLS::Render;
    for (const auto mode : { Context::RhiThreadMode::DedicatedThread, Context::RhiThreadMode::Tasks })
    {
        SCOPED_TRACE(static_cast<int>(mode));
        ScopedThreadedRenderingJobSystem jobs(2u);
        ASSERT_TRUE(jobs.IsInitialized());
        Settings::DriverSettings settings;
        settings.graphicsBackend = Settings::EGraphicsBackend::NONE;
        settings.enableExplicitRHI = false;
        settings.framesInFlight = 2u;
        Context::Driver driver(settings);
        const ScopedDriverService driverService(driver);
        auto device = std::make_shared<TestExplicitDevice>();
        device->MutableCapabilities().supportsParallelCommandRecording = true;
        device->MutableCapabilities().supportsParallelCommandTranslation = true;
        Context::DriverTestAccess::SetExplicitDevice(driver, device);
        auto* impl = Context::DriverTestAccess::GetImplForTesting(driver);
        impl->rhiThreadMode = mode;
        Context::RhiCommandListExecutor::Get().Initialize(mode,
            Context::RhiCommandListExecutor::CreateDeviceTargets(device), impl->taskGraph, &driver);
        for (size_t slot = 0u; slot < 2u; ++slot)
            std::dynamic_pointer_cast<TestFence>(Context::DriverTestAccess::EnsureFrameContext(driver, slot).frameFence)
                ->completeOnSubmit = true;

        RHI::RHITextureDesc desc;
        desc.extent = { 64u, 64u, 1u };
        desc.usage = RHI::TextureUsageFlags::ColorAttachment | RHI::TextureUsageFlags::Sampled;
        auto texture = std::make_shared<TestTexture>(desc, RHI::ResourceState::RenderTarget);
        auto view = std::make_shared<TestTextureView>(texture, RHI::RHITextureViewDesc {});
        Context::RenderFramePacket packet;
        packet.targetsSwapchain = false;
        packet.renderWidth = packet.renderHeight = 64u;
        const auto build = [view, texture](size_t)
        {
            Context::RenderScenePackage package;
            package.targetsSwapchain = false;
            package.renderWidth = package.renderHeight = 64u;
            package.containsCommandInputs = true;
            Context::RenderPassCommandInput pass;
            pass.debugName = "SharedOutputClear";
            pass.targetsSwapchain = false;
            pass.renderWidth = pass.renderHeight = 64u;
            pass.usesColorAttachment = pass.clearColor = true;
            pass.colorAttachmentViews = { view };
            package.passCommandInputs.push_back(std::move(pass));
            package.extractedTextures = { texture };
            package.preferredReadbackTexture = texture;
            return package;
        };
        ScopedLaneBlock block;
        std::atomic<bool> stalled { false };
        const auto queue = device->GetTestQueue();
        queue->onSubmit = [&](const auto& submit)
        {
            if (!submit.commandBuffers.empty() && !stalled.exchange(true))
                block.gate.wait();
        };
        const auto releaseBeforeTeardown = std::shared_ptr<void>(nullptr, [&](void*)
        {
            block.Release();
            if (queue->onSubmit)
            {
                (void)Context::DriverRendererAccess::TryDrainThreadedRendering(driver);
                queue->onSubmit = {};
            }
        });
        ASSERT_TRUE(Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, packet, build));
        EXPECT_TRUE(WaitForFlag(stalled));
        EXPECT_TRUE(Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, packet, build));
        Context::RenderCommandFence recordingFence;
        recordingFence.BeginFence(Context::RenderCommandSyncDepth::Render);
        EXPECT_TRUE(recordingFence.WaitFor(std::chrono::seconds(5)));
        block.Release();
        Context::DriverRendererAccess::DrainThreadedRendering(driver);
        queue->onSubmit = {};

        const auto& second = Context::DriverTestAccess::EnsureFrameContext(driver, 1u);
        bool foundBeginTransition = false;
        for (const auto& buffer : second.parallelCommandBuffers)
            for (const auto& batch : std::dynamic_pointer_cast<TestCommandBuffer>(buffer)->barrierHistory)
                for (const auto& barrier : batch.textureBarriers)
                    if (barrier.texture == texture && barrier.after == RHI::ResourceState::RenderTarget)
                    {
                        foundBeginTransition = true;
                        EXPECT_EQ(barrier.before, RHI::ResourceState::ShaderRead);
                    }
        EXPECT_TRUE(foundBeginTransition);
        EXPECT_FALSE(Context::RhiCommandListExecutor::Get().HasFailed())
            << Context::RhiCommandListExecutor::Get().GetFailureDiagnostic();
    }
}

namespace
{
    class DeferredPixelCopyToken final : public NLS::Render::RHI::RHICompletionToken
    {
    public:
        explicit DeferredPixelCopyToken(void* destination) : m_destination(static_cast<uint8_t*>(destination)) {}
        std::string_view GetDebugName() const override { return "DeferredPixelCopyToken"; }
        NLS::Render::RHI::RHICompletionStatus Poll() override
        {
            if (!ready)
                return {NLS::Render::RHI::RHICompletionStatusCode::Pending, {}};
            if (!copied)
            {
                *m_destination = 0xa5u;
                copied = true;
            }
            return {NLS::Render::RHI::RHICompletionStatusCode::Success, {}};
        }
        NLS::Render::RHI::RHICompletionStatus Wait(uint64_t = 0u) override { return Poll(); }
        bool ready = false;
        bool copied = false;

    private:
        uint8_t* m_destination;
    };

    class FrameTextureReadbackTests : public testing::TestWithParam<NLS::Render::Context::RhiThreadMode>
    {
    protected:
        void SetUp() override
        {
            using namespace NLS::Render;
            Settings::DriverSettings settings;
            settings.graphicsBackend = Settings::EGraphicsBackend::NONE;
            settings.enableExplicitRHI = false;
            settings.framesInFlight = 1u;
            settings.rhiThreadMode = Settings::RhiThreadModePreference::ForceDedicatedThread;
            driver = std::make_unique<Context::Driver>(settings);
            device = std::make_shared<TestExplicitDevice>();
            swapchain = std::make_shared<TestSwapchain>();
            Context::DriverTestAccess::SetExplicitDevice(*driver, device);
            Context::DriverTestAccess::SetExplicitSwapchain(*driver, swapchain);
            impl = Context::DriverTestAccess::GetImplForTesting(*driver);
            impl->rhiThreadMode = GetParam();
            Context::RhiCommandListExecutor::Get().Initialize(GetParam(),
                Context::RhiCommandListExecutor::CreateDeviceTargets(device), impl->taskGraph, driver.get());
            frame = &Context::DriverTestAccess::EnsureFrameContext(*driver, 0u);
            fence = std::dynamic_pointer_cast<TestFence>(frame->frameFence);
            ASSERT_NE(fence, nullptr);
            fence->completeOnSubmit = false;
            fence->signaled = true;
        }

        void TearDown() override
        {
            device->onBeginReadPixels = {};
            device->GetTestQueue()->onPresent = {};
            fence->signaled = true;
            driver.reset();
        }

        NLS::Render::Context::PostSubmitTextureReadbackRequest MakeReadback()
        {
            using namespace NLS::Render::Context;
            PostSubmitTextureReadbackRequest request;
            pixels = std::make_shared<std::vector<uint8_t>>(16u);
            request.width = 2u;
            request.height = 2u;
            request.destination = pixels->data();
            request.destinationKeepAlive = pixels;
            request.state = std::make_shared<PostSubmitTextureReadbackState>();
            request.source = TextureReadbackSource::FrameSwapchain;
            return request;
        }

        std::future<NLS::Render::Context::RhiSubmissionFrame> Submit(
            NLS::Render::Context::RenderScenePackage package)
        {
            using namespace NLS::Render::Context;
            RenderFramePacket packet;
            packet.frameId = 99001u;
            packet.targetsSwapchain = package.targetsSwapchain;
            package.frameId = packet.frameId;
            auto result = NLS::Tests::ObserveNextRhiFrameForTest(*driver);
            EXPECT_TRUE(DriverRendererAccess::TryEnqueueRenderFrame(*driver, packet,
                [package = std::move(package)](size_t) mutable { return std::move(package); }));
            DriverTestAccess::DrainThreadedRendering(*driver);
            return result;
        }

        std::unique_ptr<NLS::Render::Context::Driver> driver;
        std::shared_ptr<TestExplicitDevice> device;
        std::shared_ptr<TestSwapchain> swapchain;
        std::shared_ptr<TestFence> fence;
        std::shared_ptr<std::vector<uint8_t>> pixels;
        NLS::Render::Context::DriverImpl* impl = nullptr;
        NLS::Render::RHI::RHIFrameContext* frame = nullptr;
    };
}

TEST_P(FrameTextureReadbackTests, AcquiredImageIsCopiedAfterSubmitBeforePresentAndRetainedThroughGpuFence)
{
    using namespace NLS::Render;
    auto request = MakeReadback();
    const auto state = request.state;
    const auto completion = std::make_shared<DeferredPixelCopyToken>(request.destination);
    device->beginReadPixelsResult.completion = completion;
    // The frame source must override a stale caller-side texture.
    request.texture = MakeTestSwapchainBackbufferView()->GetTexture();
    const auto acquiredTexture = swapchain->backbufferView->GetTexture();
    EXPECT_NE(request.texture, acquiredTexture);
    std::weak_ptr<std::vector<uint8_t>> retainedPixels = pixels;
    std::vector<std::string> events;
    device->onBeginReadPixels = [&]
    {
        EXPECT_TRUE(Context::RenderingThreadSystem::IsInRhiThread());
        EXPECT_EQ(device->lastReadPixelsTexture, acquiredTexture);
        EXPECT_GT(device->GetTestQueue()->submitCalls, 0u);
        EXPECT_EQ(device->GetTestQueue()->presentCalls, 0u);
        EXPECT_EQ(device->GetTestQueue()->lastSubmitDesc.signalFence, nullptr);
        events.emplace_back("Readback");
    };
    device->GetTestQueue()->onPresent = [&](const auto&)
    {
        EXPECT_TRUE(Context::RenderingThreadSystem::IsInRhiThread());
        std::lock_guard lock(state->mutex);
        EXPECT_TRUE(state->beginSucceeded);
        events.emplace_back("Present");
    };
    Context::RenderScenePackage package;
    package.targetsSwapchain = true;
    package.postSubmitTextureReadbacks.push_back(std::move(request));
    auto result = Submit(std::move(package));
    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_TRUE(result.get().submittedSuccessfully);
    EXPECT_EQ(events, (std::vector<std::string>{"Readback", "Present"}));
    pixels.reset();
    EXPECT_FALSE(retainedPixels.expired());
    EXPECT_FALSE(Context::Detail::ReleaseDeferredThreadedFrameScopedResourcesAfterFence(*impl, *frame, 0u));
    fence->signaled = true;
    EXPECT_TRUE(Context::Detail::ReleaseDeferredThreadedFrameScopedResourcesAfterFence(*impl, *frame, 0u));
    ASSERT_FALSE(retainedPixels.expired());
    ASSERT_NE(state->completion, nullptr);
    EXPECT_FALSE(state->completion->Poll().IsComplete());
    EXPECT_FALSE(retainedPixels.expired());
    completion->ready = true;
    EXPECT_TRUE(state->completion->Wait().Succeeded());
    EXPECT_TRUE(completion->copied);
    EXPECT_TRUE(retainedPixels.expired());
}

TEST_P(FrameTextureReadbackTests, ExplicitTextureReadbackInSwapchainFrameAlsoRunsBeforePresent)
{
    using namespace NLS::Render;
    auto request = MakeReadback();
    request.source = Context::TextureReadbackSource::Texture;
    request.texture = MakeTestSwapchainBackbufferView()->GetTexture();
    const auto source = request.texture;
    size_t readbackCalls = 0u;
    device->onBeginReadPixels = [&]
    {
        EXPECT_TRUE(Context::RenderingThreadSystem::IsInRhiThread());
        EXPECT_EQ(device->lastReadPixelsTexture, source);
        EXPECT_EQ(device->GetTestQueue()->presentCalls, 0u);
        ++readbackCalls;
    };
    Context::RenderScenePackage package;
    package.targetsSwapchain = true;
    package.postSubmitTextureReadbacks.push_back(std::move(request));
    auto result = Submit(std::move(package));
    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_TRUE(result.get().submittedSuccessfully);
    EXPECT_EQ(readbackCalls, 1u);
    EXPECT_EQ(device->GetTestQueue()->presentCalls, 1u);
}

TEST_P(FrameTextureReadbackTests, ReadbackFailureTerminatesItsRequestAndStillPresents)
{
    using namespace NLS::Render;
    auto request = MakeReadback();
    const auto state = request.state;
    device->beginReadPixelsResult = {RHI::RHIReadbackStatusCode::BackendFailure, "injected capture failure"};
    Context::RenderScenePackage package;
    package.targetsSwapchain = true;
    package.postSubmitTextureReadbacks.push_back(std::move(request));
    auto result = Submit(std::move(package));
    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_TRUE(result.get().submittedSuccessfully);
    EXPECT_EQ(device->GetTestQueue()->presentCalls, 1u);
    std::lock_guard lock(state->mutex);
    EXPECT_TRUE(state->beginAttempted);
    EXPECT_FALSE(state->beginInProgress);
    EXPECT_FALSE(state->beginSucceeded);
    EXPECT_EQ(state->resultMessage, "injected capture failure");
    EXPECT_EQ(state->completion, nullptr);
}

TEST_P(FrameTextureReadbackTests, ReadbackExceptionCancelsPresentAndTerminatesRemainingRequests)
{
    using namespace NLS::Render;
    NLS::Tests::ScopedRenderFailureReset failureReset;
    auto first = MakeReadback();
    auto second = first;
    second.state = std::make_shared<Context::PostSubmitTextureReadbackState>();
    const auto firstState = first.state;
    const auto secondState = second.state;
    device->onBeginReadPixels = [] { throw std::runtime_error("injected pre-present exception"); };
    Context::RenderScenePackage package;
    package.targetsSwapchain = true;
    package.postSubmitTextureReadbacks.push_back(std::move(first));
    package.postSubmitTextureReadbacks.push_back(std::move(second));
    auto result = Submit(std::move(package));
    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_FALSE(result.get().submittedSuccessfully);
    EXPECT_EQ(device->GetTestQueue()->presentCalls, 0u);
    EXPECT_TRUE(impl->unsafeGpuWorkQuarantined.load());
    EXPECT_FALSE(impl->frameResourceSlots->IsSlotAvailable(0u));
    for (const auto& state : {firstState, secondState})
    {
        std::lock_guard lock(state->mutex);
        EXPECT_TRUE(state->beginAttempted);
        EXPECT_FALSE(state->beginInProgress);
        EXPECT_FALSE(state->beginSucceeded);
        EXPECT_EQ(state->completion, nullptr);
        EXPECT_NE(state->resultMessage.find("injected pre-present exception"), std::string::npos);
    }
    Context::RenderCommandFence waiter;
    waiter.BeginFence(Context::RenderCommandSyncDepth::Swapchain);
    EXPECT_TRUE(waiter.WaitFor(std::chrono::seconds(2)));
    EXPECT_FALSE(waiter.Succeeded());
}

TEST_P(FrameTextureReadbackTests, FrameSwapchainSourceRejectsOffscreenFrameWithoutReadingDevice)
{
    using namespace NLS::Render;
    auto request = MakeReadback();
    const auto state = request.state;
    size_t readbackCalls = 0u;
    device->onBeginReadPixels = [&] { ++readbackCalls; };
    Context::RenderScenePackage package;
    package.targetsSwapchain = false;
    Context::RenderPassCommandInput pass;
    pass.targetsSwapchain = false;
    pass.renderWidth = 64u;
    pass.renderHeight = 64u;
    pass.usesColorAttachment = true;
    pass.clearColor = true;
    package.passCommandInputs.push_back(pass);
    package.postSubmitTextureReadbacks.push_back(std::move(request));
    auto result = Submit(std::move(package));
    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_TRUE(result.get().submittedSuccessfully);
    EXPECT_EQ(readbackCalls, 0u);
    EXPECT_EQ(device->GetTestQueue()->presentCalls, 0u);
    std::lock_guard lock(state->mutex);
    EXPECT_TRUE(state->beginAttempted);
    EXPECT_FALSE(state->beginSucceeded);
    EXPECT_NE(state->resultMessage.find("input is unavailable"), std::string::npos);
}

INSTANTIATE_TEST_SUITE_P(AllRhiModes, FrameTextureReadbackTests,
    testing::Values(NLS::Render::Context::RhiThreadMode::None,
        NLS::Render::Context::RhiThreadMode::DedicatedThread,
        NLS::Render::Context::RhiThreadMode::Tasks));
