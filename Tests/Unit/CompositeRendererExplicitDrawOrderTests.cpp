#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "Core/ServiceLocator.h"
#include "Rendering/Core/CompositeRenderer.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Features/ARenderFeature.h"
#include "Rendering/RHI/Core/RHICommand.h"
#include "Rendering/Settings/DriverSettings.h"

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
        void* GetNativeCommandBuffer() const override { return nullptr; }
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

    class OrderRecordingFeature final : public NLS::Render::Features::ARenderFeature
    {
    public:
        OrderRecordingFeature(NLS::Render::Core::CompositeRenderer& renderer, std::vector<std::string>& events)
            : ARenderFeature(renderer)
            , m_events(events)
        {
        }

    protected:
        void OnBeforeDraw(PipelineState&, const NLS::Render::Entities::Drawable&) override
        {
            m_events.push_back("before");
        }

        void OnPrepareExplicitDraw(
            NLS::Render::RHI::RHICommandBuffer&,
            PipelineState&,
            const NLS::Render::Entities::Drawable&) override
        {
            m_events.push_back("prepare");
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
}

TEST(CompositeRendererExplicitDrawOrderTests, BindsPipelineBeforePreparingExplicitFeatureBindings)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    NLS::Core::ServiceLocator::Provide(*driver);
    std::vector<std::string> events;
    OrderRecordingRenderer renderer(*driver, events);
    renderer.AddFeature<OrderRecordingFeature>(events);

    NLS::Render::Data::PipelineState pipelineState;
    NLS::Render::Entities::Drawable drawable;
    renderer.DrawEntity(pipelineState, drawable);

    const std::vector<std::string> expected = { "before", "pipeline", "prepare", "material", "draw" };
    EXPECT_EQ(events, expected);
}
