#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "Core/ServiceLocator.h"
#include "Panels/FrameInfo.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Core/CompositeRenderer.h"
#include "Rendering/Geometry/Vertex.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/Mesh.h"
#include "Rendering/Settings/DriverSettings.h"
#include "UI/Panels/PanelWindow.h"
#include "UI/Widgets/Texts/Text.h"
#include "ImGui/imgui.h"

namespace
{
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
}
