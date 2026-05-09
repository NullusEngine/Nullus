#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "Core/ServiceLocator.h"
#include "Rendering/Core/CompositeRenderer.h"
#include "Rendering/Core/RendererStats.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Context/ThreadedRenderingLifecycle.h"
#include "Rendering/Geometry/Vertex.h"
#include "Rendering/RHI/Core/RHICommand.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/Mesh.h"
#include "Rendering/Settings/DriverSettings.h"

namespace
{
    NLS::Render::Context::Driver& EnsureTestDriver()
    {
        static auto driver = std::make_unique<NLS::Render::Context::Driver>([]()
        {
            NLS::Render::Settings::DriverSettings settings;
            settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
            settings.enableExplicitRHI = false;
            return settings;
        }());
        NLS::Core::ServiceLocator::Provide(*driver);
        return *driver;
    }

    class TestCommandBuffer final : public NLS::Render::RHI::RHICommandBuffer
    {
    public:
        std::string_view GetDebugName() const override { return "StatsOnlyTestCommandBuffer"; }
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

    class StatsOnlyRenderer final : public NLS::Render::Core::CompositeRenderer
    {
    public:
        explicit StatsOnlyRenderer(NLS::Render::Context::Driver& driver)
            : CompositeRenderer(driver)
        {
        }

    protected:
        bool PrepareRecordedDraw(
            PipelineState,
            const NLS::Render::Entities::Drawable& drawable,
            PreparedRecordedDraw& outDraw) const override
        {
            if (drawable.material == nullptr)
                return false;

            outDraw.commandBuffer = std::make_shared<TestCommandBuffer>();
            outDraw.instanceCount = static_cast<uint32_t>(std::max(drawable.material->GetGPUInstances(), 0));
            return outDraw.instanceCount > 0u;
        }

        void BindPreparedGraphicsPipeline(const PreparedRecordedDraw&) const override {}
        void BindPreparedMaterialBindingSet(const PreparedRecordedDraw&) const override {}
        void SubmitPreparedDraw(const PreparedRecordedDraw&) const override {}
    };

    std::unique_ptr<NLS::Render::Resources::Mesh> CreateTriangleMesh()
    {
        EnsureTestDriver();
        std::vector<NLS::Render::Geometry::Vertex> vertices(3);
        vertices[0].position[0] = 0.0f;
        vertices[1].position[0] = 1.0f;
        vertices[2].position[1] = 1.0f;
        std::vector<uint32_t> indices { 0u, 1u, 2u };
        return std::make_unique<NLS::Render::Resources::Mesh>(vertices, indices, 0u);
    }
}

TEST(RendererStatsTests, RendererStatsTracksSubmittedDrawCounts)
{
    NLS::Render::Core::RendererStats stats;
    NLS::Render::Resources::Material material;
    material.SetGPUInstances(2);
    const auto mesh = CreateTriangleMesh();

    NLS::Render::Entities::Drawable drawable;
    drawable.mesh = mesh.get();
    drawable.material = &material;

    stats.BeginFrame();
    stats.RecordSubmittedDraw(drawable, static_cast<uint32_t>(material.GetGPUInstances()));
    stats.EndFrame();

    const auto& frameInfo = stats.GetFrameInfo();
    EXPECT_EQ(frameInfo.batchCount, 1u);
    EXPECT_EQ(frameInfo.instanceCount, 2u);
    EXPECT_EQ(frameInfo.polyCount, 2u);
    EXPECT_EQ(frameInfo.vertexCount, 6u);
}

TEST(RendererStatsTests, CompositeRendererExposesFinalizedFrameInfoWithoutFeatureRegistration)
{
    auto& driver = EnsureTestDriver();

    StatsOnlyRenderer renderer(driver);
    NLS::Render::Resources::Material material;
    material.SetGPUInstances(3);
    const auto mesh = CreateTriangleMesh();

    NLS::Render::Entities::Drawable drawable;
    drawable.mesh = mesh.get();
    drawable.material = &material;

    renderer.ResetFrameStatistics();
    renderer.DrawEntity(NLS::Render::Data::PipelineState {}, drawable);
    renderer.FinalizeFrameStatistics();

    const auto& frameInfo = renderer.GetFrameInfo();
    EXPECT_EQ(frameInfo.batchCount, 1u);
    EXPECT_EQ(frameInfo.instanceCount, 3u);
    EXPECT_EQ(frameInfo.polyCount, 3u);
    EXPECT_EQ(frameInfo.vertexCount, 9u);
}

TEST(RendererStatsTests, CompositeRendererFinalizedFrameInfoIsZeroWhenNoDrawsWereSubmitted)
{
    auto& driver = EnsureTestDriver();

    StatsOnlyRenderer renderer(driver);
    renderer.ResetFrameStatistics();
    renderer.FinalizeFrameStatistics();

    const auto& frameInfo = renderer.GetFrameInfo();
    EXPECT_EQ(frameInfo.batchCount, 0u);
    EXPECT_EQ(frameInfo.instanceCount, 0u);
    EXPECT_EQ(frameInfo.polyCount, 0u);
    EXPECT_EQ(frameInfo.vertexCount, 0u);
}

TEST(RendererStatsTests, RendererStatsTracksThreadedFrameTelemetryFields)
{
    NLS::Render::Core::RendererStats stats;

    stats.BeginFrame();
    NLS::Render::Context::ThreadedFrameTelemetry telemetry;
    telemetry.inFlightFrameCount = 2u;
    telemetry.blockedPublishCount = 1u;
    telemetry.publishState = NLS::Render::Data::FramePublishState::BackPressured;
    telemetry.stageSummary = NLS::Render::Data::ThreadedFrameStageSummary::Rhi;
    telemetry.retirementState = NLS::Render::Data::FrameRetirementState::Pending;
    stats.SetThreadedFrameTelemetry(telemetry);
    stats.EndFrame();

    const auto& frameInfo = stats.GetFrameInfo();
    EXPECT_EQ(frameInfo.inFlightFrameCount, 2u);
    EXPECT_EQ(frameInfo.blockedFrameCount, 1u);
    EXPECT_EQ(frameInfo.publishState, NLS::Render::Data::FramePublishState::BackPressured);
    EXPECT_EQ(frameInfo.stageSummary, NLS::Render::Data::ThreadedFrameStageSummary::Rhi);
    EXPECT_EQ(frameInfo.retirementState, NLS::Render::Data::FrameRetirementState::Pending);
}

TEST(RendererStatsTests, RendererStatsTracksInfrastructureMainlineDiagnostics)
{
    NLS::Render::Core::RendererStats stats;

    NLS::Render::Context::ThreadedFrameTelemetry telemetry;
    telemetry.descriptorMainlineActive = true;
    telemetry.pipelineMainlineActive = true;
    telemetry.transientLifetimeMainlineActive = true;
    telemetry.retirementMainlineActive = true;
    telemetry.descriptorBypassCount = 0u;
    telemetry.pipelineBypassCount = 0u;
    telemetry.transientLifetimeBypassCount = 0u;
    telemetry.retirementBypassCount = 0u;
    telemetry.transientTextureRegistrationCount = 3u;
    telemetry.transientBufferRegistrationCount = 2u;
    telemetry.retiredTransientTextureCount = 3u;
    telemetry.retiredTransientBufferCount = 2u;
    telemetry.descriptorTransientPeak = 7u;
    telemetry.descriptorAllocationFailures = 1u;
    telemetry.pipelineCacheGraphicsHits = 5u;
    telemetry.pipelineCacheGraphicsMisses = 2u;
    telemetry.pipelineCacheGraphicsStores = 2u;
    telemetry.pipelineCacheGraphicsEntries = 2u;
    telemetry.pipelineCacheComputeHits = 4u;
    telemetry.pipelineCacheComputeMisses = 1u;
    telemetry.pipelineCacheComputeStores = 1u;
    telemetry.pipelineCacheComputeEntries = 1u;

    stats.BeginFrame();
    stats.SetThreadedFrameTelemetry(telemetry);
    stats.EndFrame();

    const auto& frameInfo = stats.GetFrameInfo();
    EXPECT_TRUE(frameInfo.descriptorMainlineActive);
    EXPECT_TRUE(frameInfo.pipelineMainlineActive);
    EXPECT_TRUE(frameInfo.transientLifetimeMainlineActive);
    EXPECT_TRUE(frameInfo.retirementMainlineActive);
    EXPECT_EQ(frameInfo.descriptorBypassCount, 0u);
    EXPECT_EQ(frameInfo.pipelineBypassCount, 0u);
    EXPECT_EQ(frameInfo.transientLifetimeBypassCount, 0u);
    EXPECT_EQ(frameInfo.retirementBypassCount, 0u);
    EXPECT_EQ(frameInfo.transientTextureRegistrationCount, 3u);
    EXPECT_EQ(frameInfo.transientBufferRegistrationCount, 2u);
    EXPECT_EQ(frameInfo.retiredTransientTextureCount, 3u);
    EXPECT_EQ(frameInfo.retiredTransientBufferCount, 2u);
    EXPECT_EQ(frameInfo.descriptorTransientPeak, 7u);
    EXPECT_EQ(frameInfo.descriptorAllocationFailures, 1u);
    EXPECT_EQ(frameInfo.pipelineCacheGraphicsHits, 5u);
    EXPECT_EQ(frameInfo.pipelineCacheGraphicsMisses, 2u);
    EXPECT_EQ(frameInfo.pipelineCacheGraphicsStores, 2u);
    EXPECT_EQ(frameInfo.pipelineCacheGraphicsEntries, 2u);
    EXPECT_EQ(frameInfo.pipelineCacheComputeHits, 4u);
    EXPECT_EQ(frameInfo.pipelineCacheComputeMisses, 1u);
    EXPECT_EQ(frameInfo.pipelineCacheComputeStores, 1u);
    EXPECT_EQ(frameInfo.pipelineCacheComputeEntries, 1u);
}
