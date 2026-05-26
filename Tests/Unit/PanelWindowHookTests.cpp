#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <future>
#include <fstream>
#include <iterator>
#include <memory>
#include <thread>
#include <string>
#include <utility>
#include <vector>

#include "Core/ServiceLocator.h"
#include "Panels/FrameInfo.h"
#include "Panels/ProfilerPanel.h"
#include "Panels/Console.h"
#include "Panels/ViewFrameLifecycle.h"
#include "Panels/SceneViewPickingPolicy.h"
#include "Profiling/Profiler.h"
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
#include "UI/Widgets/AWidget.h"
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

class NoopProfiledPanel final : public NLS::UI::PanelWindow
{
public:
    NoopProfiledPanel()
        : NLS::UI::PanelWindow("Probe Panel", true, {})
    {
    }

protected:
    void _Draw_Impl() override
    {
    }
};

class RecordingProfilerDestination final : public NLS::Base::Profiling::IProfilerDestination
{
public:
    void BeginScope(const NLS::Base::Profiling::ProfilerScopeEvent& event) override
    {
        events.push_back({ "begin", event.name });
    }

    void EndScope(const NLS::Base::Profiling::ProfilerScopeEvent& event) override
    {
        events.push_back({ "end", event.name });
    }

    NLS::Base::Profiling::ProfilerDestinationState GetState() const override
    {
        return {
            NLS::Base::Profiling::ProfilerDestinationId::Test,
            true,
            NLS::Base::Profiling::ProfilerAvailability::Available,
            NLS::Base::Profiling::ProfilerCapability_CPUScopes,
            ""
        };
    }

    std::vector<std::pair<std::string, std::string>> events;
};

class GarbageProbeWidget final : public NLS::UI::Widgets::AWidget
{
public:
    void _Draw_Impl() override
    {
        ++drawCount;
    }

    int drawCount = 0;
};

class SelfDestroyingProbeWidget final : public NLS::UI::Widgets::AWidget
{
public:
    SelfDestroyingProbeWidget()
    {
        Destroy();
    }

    void _Draw_Impl() override
    {
        ++drawCount;
    }

    int drawCount = 0;
};

class DestructionProbeWidget final : public NLS::UI::Widgets::AWidget
{
public:
    ~DestructionProbeWidget() override
    {
        ++destroyedCount;
    }

    void _Draw_Impl() override
    {
    }

    inline static int destroyedCount = 0;
};

const NLS::UI::Widgets::Text& TextWidgetAt(NLS::UI::PanelWindow& panel, const size_t index)
{
    const auto& widgets = panel.GetWidgets();
    EXPECT_LT(index, widgets.size());
    auto* text = dynamic_cast<NLS::UI::Widgets::Text*>(widgets[index].first);
    EXPECT_NE(text, nullptr);
    return *text;
}

void ExpectTextWidgetContent(NLS::UI::PanelWindow& panel, const std::string& expectedContent)
{
    const auto& widgets = panel.GetWidgets();
    const auto found = std::any_of(
        widgets.begin(),
        widgets.end(),
        [&expectedContent](const auto& widget)
        {
            const auto* text = dynamic_cast<NLS::UI::Widgets::Text*>(widget.first);
            return text != nullptr && text->content == expectedContent;
        });

    EXPECT_TRUE(found) << "Missing FrameInfo text: " << expectedContent;
}

std::string ReadSourceFile(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    EXPECT_TRUE(stream.is_open()) << path.string();
    return {
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()
    };
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

TEST(PanelWindowHookTests, PanelDrawProfilerScopeUsesVisiblePanelName)
{
#if defined(NLS_ENABLE_PROFILING)
    NLS::Base::Profiling::Profiler::ResetForTesting();
    RecordingProfilerDestination destination;
    NLS::Base::Profiling::Profiler::RegisterDestination(destination);
    NLS::Base::Profiling::Profiler::SetEnabled(true);

    NoopProfiledPanel panel;
    panel.Draw();

    NLS::Base::Profiling::Profiler::ResetForTesting();

    ASSERT_EQ(destination.events.size(), 2u);
    EXPECT_EQ(destination.events.front().first, "begin");
    EXPECT_EQ(destination.events.front().second, "Panel::Draw:Probe Panel");
    EXPECT_EQ(destination.events.back().first, "end");
    EXPECT_EQ(destination.events.back().second, "Panel::Draw:Probe Panel");
#else
    SUCCEED() << "Profiling macros compile to no-ops when NLS_ENABLE_PROFILING is disabled.";
#endif
}

TEST(PanelWindowHookTests, PanelDrawRecordsLastDrawDurationForFrameInfo)
{
    NoopProfiledPanel panel;

    EXPECT_EQ(panel.GetLastDrawDurationUs(), 0u);
    panel.Draw();

    EXPECT_GT(panel.GetLastDrawDurationUs(), 0u);
}

TEST(PanelWindowHookTests, WidgetContainerGarbageCollectionRunsOnlyAfterDestroyMarksDirty)
{
    NLS::UI::Internal::WidgetContainer container;
    GarbageProbeWidget first;
    container.ConsiderWidget(first, false);
    auto& second = container.CreateWidget<GarbageProbeWidget>();
    auto& third = container.CreateWidget<GarbageProbeWidget>();

    EXPECT_EQ(container.GetWidgets().size(), 3u);
    container.CollectGarbages();
    EXPECT_EQ(container.GetWidgets().size(), 3u);

    second.Destroy();
    container.UnconsiderWidget(first);
    container.CollectGarbages();

    ASSERT_EQ(container.GetWidgets().size(), 1u);
    EXPECT_EQ(container.GetWidgets().front().first, &third);
}

TEST(PanelWindowHookTests, DestroyedWidgetMarksNewContainerDirtyWhenMovedBeforeCollection)
{
    NLS::UI::Internal::WidgetContainer firstContainer;
    NLS::UI::Internal::WidgetContainer secondContainer;
    GarbageProbeWidget widget;

    firstContainer.ConsiderWidget(widget, false);
    widget.Destroy();
    firstContainer.UnconsiderWidget(widget);

    secondContainer.ConsiderWidget(widget, false);
    secondContainer.CollectGarbages();

    EXPECT_TRUE(secondContainer.GetWidgets().empty());
}

TEST(PanelWindowHookTests, CreateWidgetCollectsWidgetsDestroyedDuringConstruction)
{
    NLS::UI::Internal::WidgetContainer container;

    container.CreateWidget<SelfDestroyingProbeWidget>();
    container.CollectGarbages();

    EXPECT_TRUE(container.GetWidgets().empty());
}

TEST(PanelWindowHookTests, ConsideringWidgetInNewContainerDetachesItFromPreviousContainer)
{
    NLS::UI::Internal::WidgetContainer firstContainer;
    NLS::UI::Internal::WidgetContainer secondContainer;
    GarbageProbeWidget widget;

    firstContainer.ConsiderWidget(widget, false);
    secondContainer.ConsiderWidget(widget, false);
    widget.Destroy();

    firstContainer.CollectGarbages();
    secondContainer.CollectGarbages();

    EXPECT_TRUE(firstContainer.GetWidgets().empty());
    EXPECT_TRUE(secondContainer.GetWidgets().empty());
    EXPECT_FALSE(widget.HasParent());
}

TEST(PanelWindowHookTests, ConsideringWidgetAlreadyInContainerDoesNotDuplicateOwnership)
{
    NLS::UI::Internal::WidgetContainer container;
    GarbageProbeWidget widget;

    container.ConsiderWidget(widget, false);
    container.ConsiderWidget(widget, false);

    ASSERT_EQ(container.GetWidgets().size(), 1u);
    EXPECT_EQ(container.GetWidgets().front().first, &widget);
}

TEST(PanelWindowHookTests, AutomaticReparentPreservesInternalWidgetOwnership)
{
    DestructionProbeWidget::destroyedCount = 0;
    NLS::UI::Internal::WidgetContainer firstContainer;
    NLS::UI::Internal::WidgetContainer secondContainer;

    auto* widget = &firstContainer.CreateWidget<DestructionProbeWidget>();
    secondContainer.ConsiderWidget(*widget, false);
    secondContainer.RemoveAllWidgets();

    EXPECT_EQ(DestructionProbeWidget::destroyedCount, 1);
    if (DestructionProbeWidget::destroyedCount == 0)
        delete widget;
}

TEST(PanelWindowHookTests, AutomaticReparentPreservesExternalWidgetOwnership)
{
    DestructionProbeWidget::destroyedCount = 0;
    auto* widget = new DestructionProbeWidget();
    NLS::UI::Internal::WidgetContainer firstContainer;
    NLS::UI::Internal::WidgetContainer secondContainer;

    firstContainer.ConsiderWidget(*widget, false);
    secondContainer.ConsiderWidget(*widget);
    secondContainer.RemoveAllWidgets();

    EXPECT_EQ(DestructionProbeWidget::destroyedCount, 0);
    if (DestructionProbeWidget::destroyedCount == 0)
        delete widget;
}

TEST(PanelWindowHookTests, ExternalWidgetsClearParentWhenRemovedFromContainer)
{
    NLS::UI::Internal::WidgetContainer container;
    GarbageProbeWidget removedWidget;
    GarbageProbeWidget clearedWidget;
    GarbageProbeWidget collectedWidget;

    container.ConsiderWidget(removedWidget, false);
    container.RemoveWidget(removedWidget);
    EXPECT_FALSE(removedWidget.HasParent());

    container.ConsiderWidget(clearedWidget, false);
    container.RemoveAllWidgets();
    EXPECT_FALSE(clearedWidget.HasParent());

    container.ConsiderWidget(collectedWidget, false);
    collectedWidget.Destroy();
    container.CollectGarbages();
    EXPECT_FALSE(collectedWidget.HasParent());
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
    ExpectTextWidgetContent(panel, "ParseScene Calls: 0");
    ExpectTextWidgetContent(panel, "Drawables O/T/S: 0/0/0");
    ExpectTextWidgetContent(panel, "GBuffer Material Syncs: 0");
    ExpectTextWidgetContent(panel, "Binding Sets Created: 0");
    ExpectTextWidgetContent(panel, "Snapshot Buffers Created: 0");
    ExpectTextWidgetContent(panel, "Target Panel Draw: 0 us");
    ExpectTextWidgetContent(panel, "Frame Stage: Direct");
    ExpectTextWidgetContent(panel, "Retirement State: Direct");
}

TEST(PanelWindowHookTests, FrameInfoPanelRefreshesDuringDrawWithoutLiveTelemetryRead)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto editorSource = ReadSourceFile(root / "Project/Editor/Core/Editor.cpp");
    const auto frameInfoHeader = ReadSourceFile(root / "Project/Editor/Panels/FrameInfo.h");
    const auto frameInfoSource = ReadSourceFile(root / "Project/Editor/Panels/FrameInfo.cpp");
    const auto compositeRendererSource = ReadSourceFile(root / "Runtime/Rendering/Core/CompositeRenderer.cpp");
    const auto driverSource = ReadSourceFile(root / "Runtime/Rendering/Context/Driver.cpp");
    const auto renderThreadCoordinatorSource =
        ReadSourceFile(root / "Runtime/Rendering/Context/RenderThreadCoordinator.cpp");

    EXPECT_NE(frameInfoHeader.find("void SetTargetView(AView* p_targetView);"), std::string::npos);
    EXPECT_NE(frameInfoHeader.find("AView* GetTargetView() const;"), std::string::npos);
    EXPECT_NE(frameInfoHeader.find("void OnBeforeDrawWidgets() override;"), std::string::npos);
    EXPECT_NE(frameInfoHeader.find("AView* m_targetView = nullptr;"), std::string::npos);

    EXPECT_NE(
        frameInfoSource.find("void Editor::Panels::FrameInfo::OnBeforeDrawWidgets()"),
        std::string::npos);
    EXPECT_NE(frameInfoSource.find("RefreshForView(m_targetView);"), std::string::npos);

    EXPECT_NE(editorSource.find("if (sceneView.IsOpened() && sceneView.IsFocused())"), std::string::npos);
    EXPECT_NE(editorSource.find("frameInfo.SetTargetView(&sceneView);"), std::string::npos);
    EXPECT_NE(editorSource.find("else if (gameView.IsOpened() && gameView.IsFocused())"), std::string::npos);
    EXPECT_NE(editorSource.find("frameInfo.SetTargetView(&gameView);"), std::string::npos);
    EXPECT_NE(editorSource.find("else if (assetView.IsOpened() && assetView.IsFocused())"), std::string::npos);
    EXPECT_NE(editorSource.find("frameInfo.SetTargetView(&assetView);"), std::string::npos);
    EXPECT_NE(editorSource.find("frameInfo.GetTargetView() == nullptr || !frameInfo.GetTargetView()->IsOpened()"), std::string::npos);
    EXPECT_NE(editorSource.find("if (sceneView.IsOpened())"), std::string::npos);
    EXPECT_NE(editorSource.find("else if (gameView.IsOpened())"), std::string::npos);
    EXPECT_NE(editorSource.find("else if (assetView.IsOpened())"), std::string::npos);
    EXPECT_NE(frameInfoSource.find("p_targetView == nullptr || !p_targetView->IsOpened()"), std::string::npos);

    const auto frameInfoCreation = editorSource.find("CreatePanel<Panels::FrameInfo>");
    const auto assetViewCreation = editorSource.find("CreatePanel<Panels::AssetView>");
    const auto sceneViewCreation = editorSource.find("CreatePanel<Panels::SceneView>");
    const auto gameViewCreation = editorSource.find("CreatePanel<Panels::GameView>");
    ASSERT_NE(frameInfoCreation, std::string::npos);
    ASSERT_NE(assetViewCreation, std::string::npos);
    ASSERT_NE(sceneViewCreation, std::string::npos);
    ASSERT_NE(gameViewCreation, std::string::npos);
    EXPECT_GT(frameInfoCreation, assetViewCreation);
    EXPECT_GT(frameInfoCreation, sceneViewCreation);
    EXPECT_GT(frameInfoCreation, gameViewCreation);

    const auto getFrameInfo = compositeRendererSource.find("const Data::FrameInfo& CompositeRenderer::GetFrameInfo() const");
    ASSERT_NE(getFrameInfo, std::string::npos);
    const auto isFrameInfoValid = compositeRendererSource.find("bool CompositeRenderer::IsFrameInfoValid() const");
    ASSERT_NE(isFrameInfoValid, std::string::npos);
    const auto getFrameInfoBody = compositeRendererSource.substr(getFrameInfo, isFrameInfoValid - getFrameInfo);
    EXPECT_NE(getFrameInfoBody.find("TryGetThreadedFrameTelemetry"), std::string::npos);
    EXPECT_EQ(getFrameInfoBody.find("DriverRendererAccess::GetThreadedFrameTelemetry"), std::string::npos);

    const auto tryTelemetry = driverSource.find("DriverRendererAccess::TryGetThreadedFrameTelemetry");
    ASSERT_NE(tryTelemetry, std::string::npos);
    const auto setViewport = driverSource.find("void DriverRendererAccess::SetViewport", tryTelemetry);
    ASSERT_NE(setViewport, std::string::npos);
    const auto tryTelemetryBody = driverSource.substr(tryTelemetry, setViewport - tryTelemetry);
    EXPECT_NE(tryTelemetryBody.find("AppendDriverTelemetry"), std::string::npos);
    EXPECT_NE(tryTelemetryBody.find("return std::nullopt"), std::string::npos);

    const auto coordinatorTryTelemetry =
        renderThreadCoordinatorSource.find("RenderThreadCoordinator::TryGetThreadedFrameTelemetry");
    ASSERT_NE(coordinatorTryTelemetry, std::string::npos);
    const auto coordinatorTryTelemetryBody = renderThreadCoordinatorSource.substr(coordinatorTryTelemetry);
    EXPECT_NE(coordinatorTryTelemetryBody.find("threadedLifecycle == nullptr"), std::string::npos);
    EXPECT_NE(coordinatorTryTelemetryBody.find("return std::nullopt"), std::string::npos);
}

TEST(PanelWindowHookTests, FrameInfoTelemetryReadReturnsImmediatelyWhenLifecycleTelemetryIsBusy)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    NLS::Core::ServiceLocator::Provide(*driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(*driver);

    auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(*driver);
    ASSERT_NE(lifecycle, nullptr);
    ASSERT_TRUE(NLS::Render::Context::ThreadedRenderingLifecycleTestAccess::TryLockTelemetry(*lifecycle));

    auto telemetryFuture = std::async(
        std::launch::async,
        [&]
        {
            return NLS::Render::Context::DriverRendererAccess::TryGetThreadedFrameTelemetry(*driver);
        });

    const auto status = telemetryFuture.wait_for(std::chrono::milliseconds(50));

    NLS::Render::Context::ThreadedRenderingLifecycleTestAccess::UnlockTelemetry(*lifecycle);

    ASSERT_EQ(status, std::future_status::ready);
    EXPECT_FALSE(telemetryFuture.get().has_value());
#else
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to lock lifecycle telemetry in this test.";
#endif
}

TEST(PanelWindowHookTests, ProfilerPanelReportsTimelineDisabledByDefault)
{
    NLS::Editor::Panels::ProfilerPanel panel("Profiler", false, {});
    const auto& status = TextWidgetAt(panel, 0u);
    const auto& detail = TextWidgetAt(panel, 1u);

#if defined(NLS_ENABLE_TIMELINE_PROFILER)
    EXPECT_EQ(status.content, "TimelineProfiler: Disabled");
    EXPECT_NE(detail.content.find("Profiler panel is closed"), std::string::npos);
#else
    EXPECT_EQ(status.content, "TimelineProfiler: Disabled");
    EXPECT_NE(detail.content.find("NLS_ENABLE_TIMELINE_PROFILER"), std::string::npos);
#endif
}

TEST(PanelWindowHookTests, ConsoleDefersBackgroundThreadLogsUntilUiFlush)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    NLS::Editor::Panels::Console panel("Console", true, {});

    NLS::Debug::LogData logData;
    logData.date = "2026-05-05_17-30-00";
    logData.message = "Background render thread log";
    logData.logLevel = NLS::Debug::ELogLevel::LOG_INFO;

    auto& widgets = panel.GetWidgets();
    ASSERT_FALSE(widgets.empty());
    auto* logGroup = dynamic_cast<NLS::UI::Widgets::Group*>(widgets.back().first);
    ASSERT_NE(logGroup, nullptr);
    EXPECT_TRUE(logGroup->GetWidgets().empty());

    std::thread worker([&panel, &logData]
    {
        panel.OnLogIntercepted(logData);
    });
    worker.join();

    EXPECT_TRUE(logGroup->GetWidgets().empty());

    panel.FlushPendingLogs();

    EXPECT_EQ(logGroup->GetWidgets().size(), 1u);

    ImGui::DestroyContext();
}

TEST(PanelWindowHookTests, ProfilerPanelDrawDoesNotAdvanceTimelineFrameInsideActiveScope)
{
#if defined(NLS_ENABLE_TIMELINE_PROFILER)
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 720.0f);

    NLS::Base::Profiling::Profiler::ResetForTesting();
    NLS::Editor::Panels::ProfilerPanel panel("Profiler", true, {});

    unsigned char* pixels = nullptr;
    int width = 0;
    int height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    ASSERT_NE(pixels, nullptr);

    NLS::Base::Profiling::Profiler::SetEnabled(true);
    NLS::Base::Profiling::Profiler::RegisterDestination(panel.GetTimelineSink());

    for (int i = 0; i < 16; ++i)
    {
        const auto warmupScope = NLS::Base::Profiling::Profiler::BeginScope("Warmup Scope", __FUNCTION__);
        NLS::Base::Profiling::Profiler::EndScope(warmupScope);
    }

    const auto scope = NLS::Base::Profiling::Profiler::BeginScope("Outer Editor Scope", __FUNCTION__);

    ImGui::NewFrame();
    panel.Draw();
    ImGui::EndFrame();

    EXPECT_EQ(panel.GetTimelineSink().GetTickFrameCountForTesting(), 0u);
    EXPECT_NO_FATAL_FAILURE(NLS::Base::Profiling::Profiler::EndScope(scope));

    NLS::Base::Profiling::Profiler::ResetForTesting();
    ImGui::DestroyContext();
#else
    GTEST_SKIP() << "TimelineProfiler is not enabled in this build.";
#endif
}

TEST(PanelWindowHookTests, ProfilerPanelTimelineDrawHasDedicatedTraceAttributionScope)
{
    const auto profilerPanelPath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Panels/ProfilerPanel.cpp";

    std::ifstream stream(profilerPanelPath, std::ios::binary);
    const std::string source{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(source.empty());
    EXPECT_NE(source.find("#include \"Profiling/Profiler.h\""), std::string::npos);
    EXPECT_NE(source.find("NLS_PROFILE_NAMED_SCOPE(\"ProfilerPanel::DrawTimeline\")"), std::string::npos);
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

    ExpectTextWidgetContent(panel, "Frames In Flight: 1");
    ExpectTextWidgetContent(panel, "Blocked Frames: 0");
    ExpectTextWidgetContent(panel, "Publish State: Open");
    ExpectTextWidgetContent(panel, "Frame Stage: Logic");
    ExpectTextWidgetContent(panel, "Retirement State: Pending");
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

    ExpectTextWidgetContent(panel, "Frames In Flight: 0");
    ExpectTextWidgetContent(panel, "Blocked Frames: 0");
    ExpectTextWidgetContent(panel, "Frame Stage: Retired");
    ExpectTextWidgetContent(panel, "Retirement State: Ready");
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

TEST(PanelWindowHookTests, RetirementAwareRenderPolicyDrainsOnlyForImmediateReadback)
{
    EXPECT_TRUE(NLS::Editor::Panels::ShouldDrainAfterRetirementAwareViewRender(
        true,
        true));

    EXPECT_FALSE(NLS::Editor::Panels::ShouldDrainAfterRetirementAwareViewRender(
        true,
        false));

    EXPECT_FALSE(NLS::Editor::Panels::ShouldDrainAfterRetirementAwareViewRender(
        false,
        true));
}

TEST(PanelWindowHookTests, RetirementAwareRenderPolicyDrainsAfterViewResize)
{
    EXPECT_TRUE(NLS::Editor::Panels::ShouldDrainAfterRetirementAwareViewRender(
        true,
        false,
        true));

    EXPECT_FALSE(NLS::Editor::Panels::ShouldDrainAfterRetirementAwareViewRender(
        false,
        false,
        true));
}

TEST(PanelWindowHookTests, RetirementAwareRenderPolicyDrainsOnlyForExplicitSynchronizationNeeds)
{
    EXPECT_TRUE(NLS::Editor::Panels::ShouldDrainAfterRetirementAwareViewRender(
        true,
        false,
        false,
        true));

    EXPECT_FALSE(NLS::Editor::Panels::ShouldDrainAfterRetirementAwareViewRender(
        false,
        false,
        false,
        true));
}

TEST(PanelWindowHookTests, SceneViewCameraMotionIsTrackedWithoutForcingSynchronizedPresentation)
{
    const NLS::Maths::Vector3 previousPosition { -10.0f, 3.0f, 10.0f };
    const NLS::Maths::Quaternion previousRotation({ 0.0f, 135.0f, 0.0f });

    EXPECT_FALSE(NLS::Editor::Panels::HasSceneViewCameraMotionForPresentation(
        previousPosition,
        previousRotation,
        previousPosition,
        previousRotation));

    EXPECT_TRUE(NLS::Editor::Panels::HasSceneViewCameraMotionForPresentation(
        previousPosition,
        previousRotation,
        previousPosition + NLS::Maths::Vector3 { 0.0f, 0.0f, 0.01f },
        previousRotation));

    EXPECT_TRUE(NLS::Editor::Panels::HasSceneViewCameraMotionForPresentation(
        previousPosition,
        previousRotation,
        previousPosition,
        NLS::Maths::Quaternion({ 1.0f, 135.0f, 0.0f })));
}

TEST(PanelWindowHookTests, SceneViewPickingUsesDelayedReadbackByDefault)
{
    EXPECT_FALSE(NLS::Editor::Panels::ShouldSceneViewRequestImmediatePickingReadback());
}

TEST(PanelWindowHookTests, SceneViewDoesNotSynchronizeRetiredFramePresentationByDefault)
{
    EXPECT_FALSE(NLS::Editor::Panels::ShouldSceneViewSynchronizeRetiredFramePresentation());
}

TEST(PanelWindowHookTests, SceneViewInteractionHintsDoNotForceThreadedRenderingDrain)
{
    constexpr bool requiresRetiredFrameConsumption = true;
    constexpr bool requiresImmediateReadback = false;
    constexpr bool resizedViewThisFrame = false;

    EXPECT_FALSE(NLS::Editor::Panels::ShouldDrainAfterRetirementAwareViewRender(
        requiresRetiredFrameConsumption,
        requiresImmediateReadback,
        resizedViewThisFrame,
        NLS::Editor::Panels::ShouldSceneViewSynchronizeRetiredFramePresentation()));

    const bool cameraMovedForPresentation = true;
    const bool cameraControlActive = true;
    const bool transformGizmoActive = true;
    const bool pendingDelayedPick = true;
    const bool ordinarySceneViewPresentationNeedsSync =
        NLS::Editor::Panels::ShouldSceneViewSynchronizeRetiredFramePresentation() &&
        (cameraMovedForPresentation ||
            cameraControlActive ||
            transformGizmoActive ||
            pendingDelayedPick);

    EXPECT_FALSE(ordinarySceneViewPresentationNeedsSync);
    EXPECT_FALSE(NLS::Editor::Panels::ShouldDrainAfterRetirementAwareViewRender(
        requiresRetiredFrameConsumption,
        requiresImmediateReadback,
        resizedViewThisFrame,
        ordinarySceneViewPresentationNeedsSync));
}

TEST(PanelWindowHookTests, SceneViewRetainsExplicitReadbackAndResizeDrains)
{
    EXPECT_TRUE(NLS::Editor::Panels::ShouldDrainAfterRetirementAwareViewRender(
        true,
        true,
        false,
        false));

    EXPECT_TRUE(NLS::Editor::Panels::ShouldDrainAfterRetirementAwareViewRender(
        true,
        false,
        true,
        false));
}

TEST(PanelWindowHookTests, StartupValidationFocusPersistsForDockingWarmupFrames)
{
    EXPECT_TRUE(NLS::Editor::Panels::ShouldKeepStartupValidationFocusActive(
        "scene",
        0u,
        3u));
    EXPECT_TRUE(NLS::Editor::Panels::ShouldKeepStartupValidationFocusActive(
        "scene",
        2u,
        3u));
    EXPECT_FALSE(NLS::Editor::Panels::ShouldKeepStartupValidationFocusActive(
        "scene",
        3u,
        3u));
    EXPECT_FALSE(NLS::Editor::Panels::ShouldKeepStartupValidationFocusActive(
        "",
        0u,
        3u));
}

TEST(PanelWindowHookTests, SceneViewPickingRendersOnlyWhenSampleIsNeeded)
{
    EXPECT_FALSE(NLS::Editor::Panels::ShouldRenderScenePickingFrame(
        true,
        false,
        false,
        false,
        false,
        false,
        false,
        true,
        true));

    EXPECT_TRUE(NLS::Editor::Panels::ShouldRenderScenePickingFrame(
        true,
        false,
        false,
        false,
        false,
        false,
        true,
        true,
        true));

    EXPECT_TRUE(NLS::Editor::Panels::ShouldRenderScenePickingFrame(
        true,
        false,
        false,
        false,
        true,
        true,
        false,
        false,
        true));

    EXPECT_FALSE(NLS::Editor::Panels::ShouldRenderScenePickingFrame(
        true,
        false,
        true,
        true,
        true,
        false,
        true,
        true,
        false));

    EXPECT_TRUE(NLS::Editor::Panels::ShouldRenderScenePickingFrame(
        true,
        false,
        false,
        true,
        false,
        false,
        false,
        false,
        true));
}

TEST(PanelWindowHookTests, RetirementAwareViewsUseCurrentOverlayMatrices)
{
    EXPECT_FALSE(NLS::Editor::Panels::ShouldDelayRetirementAwareViewOverlayMatrices(
        true,
        false,
        true));

    EXPECT_FALSE(NLS::Editor::Panels::ShouldDelayRetirementAwareViewOverlayMatrices(
        true,
        true,
        true));

    EXPECT_FALSE(NLS::Editor::Panels::ShouldDelayRetirementAwareViewOverlayMatrices(
        true,
        false,
        false));
}

TEST(PanelWindowHookTests, SceneMutatingViewportOverlayRunsBeforeViewRender)
{
    using NLS::Editor::Panels::ViewportOverlayLifecyclePhase;

    EXPECT_TRUE(NLS::Editor::Panels::ShouldApplySceneMutationFromViewportOverlay(
        ViewportOverlayLifecyclePhase::BeforeViewRender));
    EXPECT_FALSE(NLS::Editor::Panels::ShouldApplySceneMutationFromViewportOverlay(
        ViewportOverlayLifecyclePhase::AfterWidgetDraw));
    EXPECT_FALSE(NLS::Editor::Panels::ShouldResolveViewportPicking(
        ViewportOverlayLifecyclePhase::BeforeViewRender));
    EXPECT_TRUE(NLS::Editor::Panels::ShouldResolveViewportPicking(
        ViewportOverlayLifecyclePhase::AfterWidgetDraw));
}

TEST(PanelWindowHookTests, PendingSceneClickPickWaitsForRequestedPickingFrame)
{
    EXPECT_TRUE(NLS::Editor::Panels::ShouldResolvePendingSceneClickPick(
        true,
        true,
        false,
        8u,
        8u));

    EXPECT_FALSE(NLS::Editor::Panels::ShouldResolvePendingSceneClickPick(
        true,
        false,
        false,
        0u,
        8u));

    EXPECT_FALSE(NLS::Editor::Panels::ShouldResolvePendingSceneClickPick(
        true,
        false,
        false,
        8u,
        7u));

    EXPECT_FALSE(NLS::Editor::Panels::ShouldResolvePendingSceneClickPick(
        false,
        false,
        false,
        8u,
        7u));

    EXPECT_TRUE(NLS::Editor::Panels::ShouldResolvePendingSceneClickPick(
        true,
        false,
        false,
        8u,
        8u));

    EXPECT_FALSE(NLS::Editor::Panels::ShouldResolvePendingSceneClickPick(
        true,
        false,
        true,
        8u,
        8u));
}
