#include <gtest/gtest.h>

#include "UI/Modules/Canvas.h"
#include "UI/Panels/APanel.h"
#include "ImGui/imgui.h"

namespace
{
class DummyPanel final : public NLS::UI::APanel
{
protected:
    void _Draw_Impl() override {}
};
}

TEST(CanvasDockspaceBackgroundTests, DockspaceDrawAddsViewportBackgroundFill)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(640.0f, 360.0f);
    io.Fonts->AddFontDefault();
    unsigned char* pixels = nullptr;
    int width = 0;
    int height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    ImGui::GetStyle().Colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.12f, 0.23f, 0.34f, 1.0f);

    DummyPanel panel;
    NLS::UI::Canvas canvas;
    canvas.MakeDockspace(true);
    canvas.AddPanel(panel);

    ImGui::NewFrame();

    ImDrawList* backgroundDrawList = ImGui::GetBackgroundDrawList(ImGui::GetMainViewport());
    const int verticesBeforeDraw = backgroundDrawList->VtxBuffer.Size;

    canvas.Draw();

    ASSERT_GT(backgroundDrawList->VtxBuffer.Size, verticesBeforeDraw);
    EXPECT_EQ(backgroundDrawList->VtxBuffer[verticesBeforeDraw].col, ImGui::GetColorU32(ImGuiCol_DockingEmptyBg));

    ImGui::EndFrame();
    ImGui::DestroyContext();
}
