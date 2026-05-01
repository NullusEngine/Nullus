#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "Core/ServiceLocator.h"
#include "Panels/FrameInfo.h"
#include "Panels/ViewFrameLifecycle.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Context/ThreadedRenderingLifecycle.h"
#include "Rendering/Core/CompositeRenderer.h"
#include "Rendering/Geometry/Vertex.h"
#include "Rendering/RHI/Core/RHICommand.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/Mesh.h"
#include "Rendering/Settings/DriverSettings.h"
#include "UI/Panels/PanelWindow.h"
#include "UI/Widgets/Texts/Text.h"
#include "ImGui/imgui.h"

namespace
{
class TestCommandBuffer final : public NLS::Render::RHI::RHICommandBuffer
{
public:
    std::string_view GetDebugName() const override { return "PanelStatsTestCommandBuffer"; }
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

std::unique_ptr<NLS::Render::Resources::Mesh> CreateTriangleMesh()
{
    std::vector<NLS::Render::Geometry::Vertex> vertices(3);
    vertices[0].position[0] = 0.0f;
    vertices[1].position[0] = 1.0f;
    vertices[2].position[1] = 1.0f;
    std::vector<uint32_t> indices { 0u, 1u, 2u };
    return std::make_unique<NLS::Render::Resources::Mesh>(vertices, indices, 0u);
}

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

class ProbePanel final : public NLS::UI::PanelWindow
{
public:
    ProbePanel()
        : NLS::UI::PanelWindow("Probe Panel", true, {})
    {
    }

    ImVec2 sizeAtHook { 0.0f, 0.0f };
    ImVec2 contentAtHook { 0.0f, 0.0f };
    bool hookCalled = false;

protected:
    void OnBeforeDrawWidgets() override
    {
        hookCalled = true;
        const auto& size = GetSize();
        sizeAtHook = ImVec2(size.x, size.y);
        contentAtHook = ImGui::GetContentRegionAvail();
    }
};

const NLS::UI::Widgets::Text& TextWidgetAt(NLS::UI::PanelWindow& panel, const size_t index)
{
    const auto& widgets = panel.GetWidgets();
    EXPECT_LT(index, widgets.size());
    auto* text = dynamic_cast<NLS::UI::Widgets::Text*>(widgets[index].first);
    EXPECT_NE(text, nullptr);
    return *text;
}

}

TEST(PanelWindowHookTests, BeforeDrawHookSeesCurrentFrameWindowSize)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    io.Fonts->AddFontDefault();
    unsigned char* pixels = nullptr;
    int width = 0;
    int height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    ProbePanel panel;

    panel.SetSize({ 480.0f, 360.0f });
    ImGui::NewFrame();
    panel.Draw();
    ImGui::EndFrame();

    ImGui::NewFrame();
    panel.Draw();
    ImGui::EndFrame();

    panel.hookCalled = false;
    ImGui::NewFrame();
    panel.Draw();

    EXPECT_TRUE(panel.hookCalled);
    EXPECT_NEAR(panel.sizeAtHook.x, 480.0f, 1.0f);
    EXPECT_NEAR(panel.sizeAtHook.y, 360.0f, 1.0f);
    EXPECT_GT(panel.contentAtHook.x, 0.0f);
    EXPECT_GT(panel.contentAtHook.y, 0.0f);

    ImGui::EndFrame();
    ImGui::DestroyContext();
}

TEST(PanelWindowHookTests, FrameInfoPanelReadsRendererOwnedStats)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    NLS::Core::ServiceLocator::Provide(*driver);

    StatsOnlyRenderer renderer(*driver);
    NLS::Render::Resources::Material material;
    material.SetGPUInstances(2);
    const auto mesh = CreateTriangleMesh();

    NLS::Render::Entities::Drawable drawable;
    drawable.mesh = mesh.get();
    drawable.material = &material;

    renderer.ResetFrameStatistics();
    renderer.DrawEntity(NLS::Render::Data::PipelineState {}, drawable);
    renderer.FinalizeFrameStatistics();

    NLS::Editor::Panels::FrameInfo panel("Frame Info", true, {});
    panel.UpdateForRenderer("Stats View", renderer);

    EXPECT_EQ(TextWidgetAt(panel, 0u).content, "Target View: Stats View");
    EXPECT_EQ(TextWidgetAt(panel, 2u).content, "Batches: 1");
    EXPECT_EQ(TextWidgetAt(panel, 3u).content, "Instances: 2");
    EXPECT_EQ(TextWidgetAt(panel, 4u).content, "Polygons: 2");
    EXPECT_EQ(TextWidgetAt(panel, 5u).content, "Vertices: 6");
    EXPECT_EQ(TextWidgetAt(panel, 9u).content, "Frame Stage: Direct");
    EXPECT_EQ(TextWidgetAt(panel, 10u).content, "Retirement State: Direct");
}

TEST(PanelWindowHookTests, FrameInfoPanelReadsThreadedPublishDiagnostics)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 2u;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    NLS::Core::ServiceLocator::Provide(*driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(*driver);

    StatsOnlyRenderer renderer(*driver);
    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 64u;
    frameDescriptor.renderHeight = 64u;
    frameDescriptor.camera = &camera;

    renderer.BeginFrame(frameDescriptor);
    renderer.EndFrame();

    NLS::Editor::Panels::FrameInfo panel("Frame Info", true, {});
    panel.UpdateForRenderer("Threaded View", renderer);

    EXPECT_EQ(TextWidgetAt(panel, 6u).content, "Frames In Flight: 1");
    EXPECT_EQ(TextWidgetAt(panel, 7u).content, "Blocked Frames: 0");
    EXPECT_EQ(TextWidgetAt(panel, 8u).content, "Publish State: Open");
    EXPECT_EQ(TextWidgetAt(panel, 9u).content, "Frame Stage: Logic");
    EXPECT_EQ(TextWidgetAt(panel, 10u).content, "Retirement State: Pending");
}

TEST(PanelWindowHookTests, FrameInfoPanelRefreshesThreadedDiagnosticsAfterFrameRetirement)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    NLS::Core::ServiceLocator::Provide(*driver);

    StatsOnlyRenderer renderer(*driver);
    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 64u;
    frameDescriptor.renderHeight = 64u;
    frameDescriptor.camera = &camera;

    renderer.BeginFrame(frameDescriptor);
    renderer.EndFrame();

    const auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(*driver);
    ASSERT_NE(lifecycle, nullptr);

    for (int attempt = 0; attempt < 100; ++attempt)
    {
        const auto* slot = lifecycle->PeekSlot(0u);
        if (slot != nullptr && slot->stage == NLS::Render::Context::ThreadedFrameStage::Retired)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    NLS::Editor::Panels::FrameInfo panel("Frame Info", true, {});
    panel.UpdateForRenderer("Threaded View", renderer);

    EXPECT_EQ(TextWidgetAt(panel, 6u).content, "Frames In Flight: 0");
    EXPECT_EQ(TextWidgetAt(panel, 7u).content, "Blocked Frames: 0");
    EXPECT_EQ(TextWidgetAt(panel, 9u).content, "Frame Stage: Retired");
    EXPECT_EQ(TextWidgetAt(panel, 10u).content, "Retirement State: Ready");
}

TEST(PanelWindowHookTests, RetirementAwareResizePolicyDefersViewResizeWhileFramesRemainInFlight)
{
    const std::pair<uint16_t, uint16_t> activeSize { 480u, 360u };
    const std::pair<uint16_t, uint16_t> requestedSize { 640u, 480u };

    NLS::Render::Context::ThreadedFrameTelemetry telemetry {};
    telemetry.inFlightFrameCount = 1u;

    EXPECT_TRUE(NLS::Editor::Panels::ShouldDeferRetirementAwareViewResize(
        requestedSize,
        activeSize,
        true,
        telemetry));

    telemetry.inFlightFrameCount = 0u;
    EXPECT_FALSE(NLS::Editor::Panels::ShouldDeferRetirementAwareViewResize(
        requestedSize,
        activeSize,
        true,
        telemetry));
    EXPECT_FALSE(NLS::Editor::Panels::ShouldDeferRetirementAwareViewResize(
        requestedSize,
        activeSize,
        false,
        telemetry));
}

TEST(PanelWindowHookTests, RetirementAwareResizePolicyRequestsDrainBeforeDeferring)
{
    const std::pair<uint16_t, uint16_t> activeSize { 480u, 360u };
    const std::pair<uint16_t, uint16_t> requestedSize { 640u, 480u };

    NLS::Render::Context::ThreadedFrameTelemetry telemetry {};
    telemetry.inFlightFrameCount = 1u;

    EXPECT_TRUE(NLS::Editor::Panels::ShouldDrainBeforeRetirementAwareViewResize(
        requestedSize,
        activeSize,
        true,
        telemetry));

    telemetry.inFlightFrameCount = 0u;
    EXPECT_FALSE(NLS::Editor::Panels::ShouldDrainBeforeRetirementAwareViewResize(
        requestedSize,
        activeSize,
        true,
        telemetry));
}
