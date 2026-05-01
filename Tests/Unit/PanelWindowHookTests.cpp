#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <thread>
#include <vector>

#include "ReflectionTestUtils.h"
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

std::filesystem::path GetRepositoryRoot()
{
    return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
}

std::string ReadRepositorySource(std::string_view relativePath)
{
    return NLS::Tests::Reflection::ReadAllText(GetRepositoryRoot() / relativePath);
}

void ExpectFunctionReturnsLiteral(
    std::string_view source,
    std::string_view signature,
    std::string_view literal)
{
    const auto signatureOffset = source.find(signature);
    ASSERT_NE(signatureOffset, std::string_view::npos) << "Missing function signature: " << signature;

    const auto bodyOffset = source.find("return ", signatureOffset);
    ASSERT_NE(bodyOffset, std::string_view::npos) << "Missing return statement near: " << signature;

    const std::string expectedReturn = std::string("return ") + std::string(literal) + ";";
    EXPECT_NE(source.find(expectedReturn, bodyOffset), std::string_view::npos)
        << "Expected " << signature << " to contain " << expectedReturn;
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

TEST(PanelWindowHookTests, UnifiedEditorViewsConfigureRetiredFrameConsumptionFromSharedViewBase)
{
    const auto aViewSource = ReadRepositorySource("Project/Editor/Panels/AView.cpp");
    const auto sceneViewSource = ReadRepositorySource("Project/Editor/Panels/SceneView.cpp");
    const auto gameViewSource = ReadRepositorySource("Project/Editor/Panels/GameView.cpp");
    const auto assetViewSource = ReadRepositorySource("Project/Editor/Panels/AssetView.cpp");
    const auto applicationSource = ReadRepositorySource("Project/Editor/Core/Application.cpp");

    ExpectFunctionReturnsLiteral(
        aViewSource,
        "bool Editor::Panels::AView::RequiresRetiredFrameConsumption() const",
        "m_requiresRetiredFrameConsumption");
    EXPECT_NE(sceneViewSource.find("SetRequiresRetiredFrameConsumption(true);"), std::string::npos);
    EXPECT_NE(gameViewSource.find("SetRequiresRetiredFrameConsumption(true);"), std::string::npos);
    EXPECT_NE(assetViewSource.find("SetRequiresRetiredFrameConsumption(true);"), std::string::npos);
    EXPECT_EQ(sceneViewSource.find("RequiresRetiredFrameConsumption() const"), std::string::npos);
    EXPECT_EQ(gameViewSource.find("RequiresRetiredFrameConsumption() const"), std::string::npos);
    EXPECT_EQ(assetViewSource.find("RequiresRetiredFrameConsumption() const"), std::string::npos);
    const auto drainOffset = aViewSource.find("DriverRendererAccess::DrainThreadedRendering(*driver)");
    const auto deferOffset = aViewSource.find("ShouldDeferRetirementAwareViewResize(");
    ASSERT_NE(drainOffset, std::string::npos);
    ASSERT_NE(deferOffset, std::string::npos);
    EXPECT_LT(drainOffset, deferOffset);
    const auto endFrameOffset = aViewSource.find("m_renderer->EndFrame();");
    const auto postSubmitDrainOffset =
        aViewSource.find("DriverRendererAccess::DrainThreadedRendering(*driver)", endFrameOffset);
    const auto afterRenderFrameOffset = aViewSource.find("AfterRenderFrame();", endFrameOffset);
    ASSERT_NE(endFrameOffset, std::string::npos);
    ASSERT_NE(postSubmitDrainOffset, std::string::npos);
    ASSERT_NE(afterRenderFrameOffset, std::string::npos);
    EXPECT_LT(endFrameOffset, postSubmitDrainOffset);
    EXPECT_LT(postSubmitDrainOffset, afterRenderFrameOffset);
    EXPECT_NE(applicationSource.find("RunEditorFrame("), std::string::npos);
    EXPECT_NE(applicationSource.find("SyncPlatformSwapchainToFramebufferSize("), std::string::npos);
    EXPECT_EQ(applicationSource.find("MakeCurrentContext("), std::string::npos);
}

TEST(PanelWindowHookTests, EditorPanelsUnsubscribeGlobalEventListenersBeforeDestruction)
{
    const auto sceneViewHeader = ReadRepositorySource("Project/Editor/Panels/SceneView.h");
    const auto sceneViewSource = ReadRepositorySource("Project/Editor/Panels/SceneView.cpp");
    const auto hierarchyHeader = ReadRepositorySource("Project/Editor/Panels/Hierarchy.h");
    const auto hierarchySource = ReadRepositorySource("Project/Editor/Panels/Hierarchy.cpp");
    const auto consoleHeader = ReadRepositorySource("Project/Editor/Panels/Console.h");
    const auto consoleSource = ReadRepositorySource("Project/Editor/Panels/Console.cpp");
    const auto toolbarHeader = ReadRepositorySource("Project/Editor/Panels/Toolbar.h");
    const auto toolbarSource = ReadRepositorySource("Project/Editor/Panels/Toolbar.cpp");
    const auto editorSource = ReadRepositorySource("Project/Editor/Core/Editor.cpp");

    EXPECT_NE(sceneViewHeader.find("~SceneView()"), std::string::npos);
    EXPECT_NE(sceneViewHeader.find("m_destroyedListener"), std::string::npos);
    EXPECT_NE(sceneViewHeader.find("m_highlightedActor = nullptr"), std::string::npos);
    EXPECT_NE(sceneViewSource.find("m_destroyedListener = Engine::GameObject::DestroyedEvent +="), std::string::npos);
    EXPECT_NE(sceneViewSource.find("Engine::GameObject::DestroyedEvent -= m_destroyedListener"), std::string::npos);

    EXPECT_NE(hierarchyHeader.find("~Hierarchy()"), std::string::npos);
    EXPECT_NE(hierarchyHeader.find("m_actorDestroyedListener"), std::string::npos);
    EXPECT_NE(hierarchyHeader.find("m_sceneUnloadListener"), std::string::npos);
    EXPECT_NE(hierarchySource.find("m_actorDestroyedListener = Engine::GameObject::DestroyedEvent +="), std::string::npos);
    EXPECT_NE(hierarchySource.find("Engine::GameObject::DestroyedEvent -= m_actorDestroyedListener"), std::string::npos);
    EXPECT_NE(hierarchySource.find("EDITOR_CONTEXT(sceneManager).SceneUnloadEvent -= m_sceneUnloadListener"), std::string::npos);

    EXPECT_NE(consoleHeader.find("~Console()"), std::string::npos);
    EXPECT_NE(consoleHeader.find("m_logListener"), std::string::npos);
    EXPECT_NE(consoleSource.find("m_logListener = Debug::Logger::LogEvent +="), std::string::npos);
    EXPECT_NE(consoleSource.find("Debug::Logger::LogEvent -= m_logListener"), std::string::npos);

    EXPECT_NE(toolbarHeader.find("~Toolbar()"), std::string::npos);
    EXPECT_NE(toolbarHeader.find("m_editorModeChangedListener"), std::string::npos);
    EXPECT_NE(toolbarSource.find("m_editorModeChangedListener = EDITOR_EVENT(EditorModeChangedEvent) +="), std::string::npos);
    EXPECT_NE(toolbarSource.find("EDITOR_EVENT(EditorModeChangedEvent) -= m_editorModeChangedListener"), std::string::npos);

    const auto destroyPanelsOffset = editorSource.find("m_panelsManager.DestroyPanels();");
    const auto unloadSceneOffset = editorSource.find("m_context.sceneManager.UnloadCurrentScene();");
    ASSERT_NE(destroyPanelsOffset, std::string::npos);
    ASSERT_NE(unloadSceneOffset, std::string::npos);
    EXPECT_LT(destroyPanelsOffset, unloadSceneOffset);
}
