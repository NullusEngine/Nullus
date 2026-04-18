#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "Core/ServiceLocator.h"
#include "Rendering/Core/CompositeRenderer.h"
#include "Rendering/Core/FrameObjectBindingProvider.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Data/FrameDescriptor.h"
#include "Rendering/Entities/Camera.h"
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

    class RecordingBindingProvider final : public NLS::Render::Core::FrameObjectBindingProvider
    {
    public:
        RecordingBindingProvider(
            NLS::Render::Core::CompositeRenderer& renderer,
            std::vector<std::string>& events)
            : FrameObjectBindingProvider(renderer)
            , m_events(events)
        {
        }

    protected:
        void OnBeginFrame(const NLS::Render::Data::FrameDescriptor&) override
        {
            m_events.push_back("begin");
        }

        void OnEndFrame() override
        {
            m_events.push_back("end");
        }

        void OnPrepareDraw(PipelineState&, const NLS::Render::Entities::Drawable&) override
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

    class ProviderAwareRenderer final : public NLS::Render::Core::CompositeRenderer
    {
    public:
        ProviderAwareRenderer(NLS::Render::Context::Driver& driver)
            : CompositeRenderer(driver)
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

        void BindPreparedGraphicsPipeline(const PreparedRecordedDraw&) const override {}
        void BindPreparedMaterialBindingSet(const PreparedRecordedDraw&) const override {}
        void SubmitPreparedDraw(const PreparedRecordedDraw&) const override {}

    private:
        std::shared_ptr<NLS::Render::RHI::RHICommandBuffer> m_commandBuffer;
    };
}

TEST(RendererFrameObjectBindingTests, ProviderTracksFrameLifecycle)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    NLS::Core::ServiceLocator::Provide(*driver);

    std::vector<std::string> events;
    ProviderAwareRenderer renderer(*driver);
    auto provider = std::make_unique<RecordingBindingProvider>(renderer, events);
    auto* providerPtr = provider.get();
    renderer.SetFrameObjectBindingProvider(std::move(provider));

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 64u;
    frameDescriptor.renderHeight = 64u;
    frameDescriptor.camera = &camera;

    providerPtr->BeginFrame(frameDescriptor);
    EXPECT_TRUE(providerPtr->IsFramePrepared());
    EXPECT_FALSE(providerPtr->IsObjectPrepared());

    providerPtr->EndFrame();
    EXPECT_FALSE(providerPtr->IsFramePrepared());
    EXPECT_FALSE(providerPtr->IsObjectPrepared());
    EXPECT_EQ(events, std::vector<std::string>({ "begin", "end" }));
}

TEST(RendererFrameObjectBindingTests, ProviderPreparesObjectStateDuringDrawsWithoutFeatures)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    NLS::Core::ServiceLocator::Provide(*driver);

    std::vector<std::string> events;
    ProviderAwareRenderer renderer(*driver);
    auto provider = std::make_unique<RecordingBindingProvider>(renderer, events);
    auto* providerPtr = provider.get();
    renderer.SetFrameObjectBindingProvider(std::move(provider));

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 64u;
    frameDescriptor.renderHeight = 64u;
    frameDescriptor.camera = &camera;

    providerPtr->BeginFrame(frameDescriptor);

    NLS::Render::Data::PipelineState pipelineState;
    NLS::Render::Entities::Drawable drawable;
    renderer.DrawEntity(pipelineState, drawable);

    EXPECT_TRUE(providerPtr->IsFramePrepared());
    EXPECT_TRUE(providerPtr->IsObjectPrepared());
    EXPECT_EQ(providerPtr->GetPreparedDrawCount(), 1u);
    EXPECT_EQ(events, std::vector<std::string>({ "begin", "before", "prepare" }));

    providerPtr->EndFrame();
}
