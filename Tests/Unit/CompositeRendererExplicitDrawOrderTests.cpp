#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "Core/ServiceLocator.h"
#include "Rendering/Core/CompositeRenderer.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Core/FrameObjectBindingProvider.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/RHI/Core/RHIBinding.h"
#include "Rendering/RHI/Core/RHICommand.h"
#include "Rendering/RHI/Core/RHIMesh.h"
#include "Rendering/RHI/Core/RHIPipeline.h"
#include "Rendering/Settings/DriverSettings.h"
#include "Rendering/Entities/Camera.h"

namespace
{
    class TestCommandBuffer final : public NLS::Render::RHI::RHICommandBuffer
    {
    public:
        std::string_view GetDebugName() const override { return "TestCommandBuffer"; }
        void Begin() override {}
        void End() override {}
        void Reset() override {}
        bool IsRecording() const override { return true; }
        NLS::Render::RHI::NativeHandle GetNativeCommandBuffer() const override { return {}; }
        void BeginRenderPass(const NLS::Render::RHI::RHIRenderPassDesc&) override {}
        void EndRenderPass() override {}
        void SetViewport(const NLS::Render::RHI::RHIViewport&) override {}
        void SetScissor(const NLS::Render::RHI::RHIRect2D&) override {}
        void BindGraphicsPipeline(const std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline>&) override {}
        void BindComputePipeline(const std::shared_ptr<NLS::Render::RHI::RHIComputePipeline>&) override {}
        void BindBindingSet(uint32_t, const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>&) override {}
        void PushConstants(NLS::Render::RHI::ShaderStageMask, uint32_t, uint32_t, const void*) override {}
        void BindVertexBuffer(uint32_t, const NLS::Render::RHI::RHIVertexBufferView&) override {}
        void BindIndexBuffer(const NLS::Render::RHI::RHIIndexBufferView&) override {}
        void Draw(uint32_t, uint32_t, uint32_t, uint32_t) override {}
        void DrawIndexed(uint32_t, uint32_t, uint32_t, int32_t, uint32_t) override {}
        void Dispatch(uint32_t, uint32_t, uint32_t) override {}
        void CopyBuffer(
            const std::shared_ptr<NLS::Render::RHI::RHIBuffer>&,
            const std::shared_ptr<NLS::Render::RHI::RHIBuffer>&,
            const NLS::Render::RHI::RHIBufferCopyRegion&) override {}
        void CopyBufferToTexture(const NLS::Render::RHI::RHIBufferToTextureCopyDesc&) override {}
        void CopyTexture(const NLS::Render::RHI::RHITextureCopyDesc&) override {}
        void Barrier(const NLS::Render::RHI::RHIBarrierDesc&) override {}
    };

    class OrderRecordingBindingProvider final : public NLS::Render::Core::FrameObjectBindingProvider
    {
    public:
        OrderRecordingBindingProvider(
            NLS::Render::Core::CompositeRenderer& renderer,
            std::vector<std::string>& events)
            : FrameObjectBindingProvider(renderer)
            , m_events(events)
        {
        }

    protected:
        bool OnPrepareDraw(PipelineState&, const NLS::Render::Entities::Drawable&) override
        {
            m_events.push_back("provider-before");
            return true;
        }

        void OnPrepareExplicitDraw(
            NLS::Render::RHI::RHICommandBuffer&,
            PipelineState&,
            const NLS::Render::Entities::Drawable&) override
        {
            m_events.push_back("provider-prepare");
        }

    private:
        std::vector<std::string>& m_events;
    };

    class OrderRecordingRenderer final : public NLS::Render::Core::CompositeRenderer
    {
    public:
        OrderRecordingRenderer(NLS::Render::Context::Driver& driver, std::vector<std::string>& events)
            : CompositeRenderer(driver)
            , m_events(events)
            , m_commandBuffer(std::make_shared<TestCommandBuffer>())
        {
        }

    protected:
        bool PrepareRecordedDraw(
            PipelineState,
            const NLS::Render::Entities::Drawable&,
            PreparedRecordedDraw& outDraw) const override
        {
            outDraw.commandBuffer = m_commandBuffer;
            outDraw.instanceCount = 1u;
            return true;
        }

        void BindPreparedGraphicsPipeline(const PreparedRecordedDraw&) const override
        {
            m_events.push_back("pipeline");
        }

        void BindPreparedMaterialBindingSet(const PreparedRecordedDraw&) const override
        {
            m_events.push_back("material");
        }

        void SubmitPreparedDraw(const PreparedRecordedDraw&) const override
        {
            m_events.push_back("draw");
        }

    private:
        std::vector<std::string>& m_events;
        std::shared_ptr<NLS::Render::RHI::RHICommandBuffer> m_commandBuffer;
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

    class TestMesh final : public NLS::Render::RHI::RHIMesh
    {
    public:
        std::shared_ptr<NLS::Render::RHI::RHIBuffer> GetVertexBuffer() const override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHIBuffer> GetIndexBuffer() const override { return nullptr; }
        uint32_t GetVertexCount() const override { return 3u; }
        uint32_t GetIndexCount() const override { return 0u; }
        NLS::Render::Settings::EPrimitiveMode GetPrimitiveMode() const override { return NLS::Render::Settings::EPrimitiveMode::TRIANGLES; }
        uint32_t GetVertexStride() const override { return 0u; }
        NLS::Render::RHI::IndexType GetIndexType() const override { return NLS::Render::RHI::IndexType::UInt32; }
    };

    class ThreadedRecordingRenderer final : public NLS::Render::Core::CompositeRenderer
    {
    public:
        explicit ThreadedRecordingRenderer(NLS::Render::Context::Driver& driver)
            : CompositeRenderer(driver)
            , m_pipeline(std::make_shared<TestGraphicsPipeline>("ThreadedPipeline"))
            , m_materialBindingSet(std::make_shared<TestBindingSet>("ThreadedMaterialBindingSet"))
            , m_mesh(std::make_shared<TestMesh>())
        {
        }

    protected:
        std::optional<NLS::Render::Context::FrameSnapshot> BuildFrameSnapshot(
            const NLS::Render::Data::FrameDescriptor& frameDescriptor) const override
        {
            auto snapshot = NLS::Render::Core::ABaseRenderer::BuildFrameSnapshot(frameDescriptor);
            if (!snapshot.has_value())
                return snapshot;

            snapshot->hasSceneInput = true;
            snapshot->visibleOpaqueDrawCount = static_cast<uint64_t>(snapshot->recordedDrawCommands.size());
            return snapshot;
        }

        bool PrepareRecordedDraw(
            PipelineState,
            const NLS::Render::Entities::Drawable&,
            PreparedRecordedDraw& outDraw) const override
        {
            outDraw.commandBuffer.reset();
            outDraw.pipeline = m_pipeline;
            outDraw.materialBindingSet = m_materialBindingSet;
            outDraw.mesh = m_mesh;
            outDraw.instanceCount = 1u;
            return true;
        }

    private:
        std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline> m_pipeline;
        std::shared_ptr<NLS::Render::RHI::RHIBindingSet> m_materialBindingSet;
        std::shared_ptr<NLS::Render::RHI::RHIMesh> m_mesh;
    };

    class CountingRenderPass final : public NLS::Render::Core::ARenderPass
    {
    public:
        explicit CountingRenderPass(NLS::Render::Core::CompositeRenderer& renderer)
            : ARenderPass(renderer)
        {
        }

        uint32_t drawCalls = 0u;

    protected:
        void Draw(PipelineState) override
        {
            ++drawCalls;
        }
    };

    NLS::Render::Data::FrameDescriptor MakeFrameDescriptor(NLS::Render::Entities::Camera& camera)
    {
        NLS::Render::Data::FrameDescriptor frameDescriptor;
        frameDescriptor.renderWidth = 64u;
        frameDescriptor.renderHeight = 64u;
        frameDescriptor.camera = &camera;
        return frameDescriptor;
    }

    NLS::Render::Settings::DriverSettings MakeThreadedDrawOrderSettings()
    {
        NLS::Render::Settings::DriverSettings settings;
        settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
        settings.enableExplicitRHI = false;
        settings.enableThreadedRendering = true;
        settings.threadedFrameSlotCount = 1u;
        return settings;
    }
}

TEST(CompositeRendererExplicitDrawOrderTests, SubmitsDrawWithoutOptionalFeatureRegistry)
{
    NLS::Render::Context::Driver driver(MakeThreadedDrawOrderSettings());
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    std::vector<std::string> events;
    OrderRecordingRenderer renderer(driver, events);

    NLS::Render::Data::PipelineState pipelineState;
    NLS::Render::Entities::Drawable drawable;
    NLS::Render::Entities::Camera camera;
    const auto frameDescriptor = MakeFrameDescriptor(camera);

    renderer.BeginFrame(frameDescriptor);
    renderer.DrawEntity(pipelineState, drawable);
    renderer.EndFrame();

    const std::vector<std::string> expected = { "pipeline", "material", "draw" };
    EXPECT_EQ(events, expected);
}

TEST(CompositeRendererExplicitDrawOrderTests, RunsRendererOwnedBindingPreparationBeforeMaterialSubmission)
{
    NLS::Render::Context::Driver driver(MakeThreadedDrawOrderSettings());
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    std::vector<std::string> events;
    OrderRecordingRenderer renderer(driver, events);
    renderer.SetFrameObjectBindingProvider(std::make_unique<OrderRecordingBindingProvider>(renderer, events));

    NLS::Render::Data::PipelineState pipelineState;
    NLS::Render::Entities::Drawable drawable;
    NLS::Render::Entities::Camera camera;
    const auto frameDescriptor = MakeFrameDescriptor(camera);

    renderer.BeginFrame(frameDescriptor);
    renderer.DrawEntity(pipelineState, drawable);
    renderer.EndFrame();

    const std::vector<std::string> expected = {
        "provider-before",
        "pipeline",
        "provider-prepare",
        "material",
        "draw"
    };
    EXPECT_EQ(events, expected);
}

TEST(CompositeRendererExplicitDrawOrderTests, CompositeRendererFinalizesThreadedPublishDiagnosticsAfterFrameEnd)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    NLS::Core::ServiceLocator::Provide(*driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(*driver);
    std::vector<std::string> events;
    OrderRecordingRenderer renderer(*driver, events);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 64u;
    frameDescriptor.renderHeight = 64u;
    frameDescriptor.camera = &camera;

    renderer.BeginFrame(frameDescriptor);
    renderer.EndFrame();

    ASSERT_TRUE(renderer.IsFrameInfoValid());
    const auto& frameInfo = renderer.GetFrameInfo();
    EXPECT_EQ(frameInfo.inFlightFrameCount, 1u);
    EXPECT_EQ(frameInfo.blockedFrameCount, 0u);
    EXPECT_EQ(frameInfo.publishState, NLS::Render::Data::FramePublishState::Open);
}

TEST(CompositeRendererExplicitDrawOrderTests, ThreadedCompositeRendererPublishesSnapshotAndDefersPassSchedulingToPreparedBuilder)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    ThreadedRecordingRenderer renderer(driver);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 64u;
    frameDescriptor.renderHeight = 64u;
    frameDescriptor.camera = &camera;

    NLS::Render::Data::PipelineState pipelineState;
    NLS::Render::Entities::Drawable drawable;

    renderer.BeginFrame(frameDescriptor);
    renderer.DrawEntity(pipelineState, drawable);
    renderer.EndFrame();

    const auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);
    const auto* slot = lifecycle->PeekSlot(0u);
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(slot->publishOrigin, NLS::Render::Context::ThreadedFramePublishOrigin::PreparedBuilder);
    ASSERT_TRUE(slot->snapshot.has_value());
    EXPECT_EQ(slot->snapshot->recordedDrawCommands.size(), 1u);
    EXPECT_EQ(slot->snapshot->visibleOpaqueDrawCount, 1u);
    EXPECT_TRUE(slot->preparedRenderSceneBuilder.has_value());
    EXPECT_FALSE(slot->renderScenePackage.has_value());
}

TEST(CompositeRendererExplicitDrawOrderTests, ExplicitSwapchainPassDoesNotDrawWhenBackbufferViewIsMissing)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.commandBuffer = std::make_shared<TestCommandBuffer>();
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, true);

    NLS::Render::Core::CompositeRenderer renderer(driver);
    CountingRenderPass pass(renderer);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 64u;
    frameDescriptor.renderHeight = 64u;
    frameDescriptor.camera = &camera;

    renderer.BeginFrame(frameDescriptor);
    renderer.ExecutePass(pass);
    renderer.EndFrame();

    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, false);
    EXPECT_EQ(pass.drawCalls, 0u);
}
