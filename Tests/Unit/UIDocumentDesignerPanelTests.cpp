#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <imgui_internal.h>

#include "Panels/UIDocumentDesigner.h"
#include "UI/UIDocumentDesignerModel.h"
#include "UIDocumentDesignerTestSupport.h"

namespace
{
    using namespace NLS::Editor::UIToolsTest;

    void RenderPanel(NLS::Editor::Panels::UIDocumentDesigner& panel)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({ 0.0f, 0.0f });
        ImGui::SetNextWindowSize({ 1480.0f, 960.0f });
        panel.Draw();
        ImGui::Render();
    }
}

TEST(UIDocumentDesignerPanel, OpensDocumentExecutesCommandsAndSaves)
{
    using NLS::Editor::Panels::UIDocumentDesigner;
    using NLS::Editor::UIDocumentTools::UIDocumentDesignerModel;
    using Json = UIDocumentDesignerModel::Json;

    ScopedDocumentDirectory directory;
    ScopedUndoService undo;
    ScopedImGuiContext imgui;

    const auto path = directory.Write(
        "Assets/UI/Hud.ui",
        UIDocumentDesignerModel::CreateDocumentSource("Hud"));
    NLS::UI::PanelWindowSettings settings;
    UIDocumentDesigner panel("UI Designer", true, settings);

    ASSERT_TRUE(panel.OpenDocument(path));
    EXPECT_TRUE(panel.HasDocument());
    EXPECT_FALSE(panel.IsCommandEnabled("save"));

    // Copy the root and paste it as a child, then persist through the same
    // command entry point the toolbar uses.
    ASSERT_TRUE(panel.ExecuteCommand("copy"));
    ASSERT_TRUE(panel.ExecuteCommand("paste"));
    EXPECT_TRUE(panel.IsCommandEnabled("save"));
    ASSERT_TRUE(panel.ExecuteCommand("save"));

    auto document = Json::parse(ReadFile(path));
    ASSERT_TRUE(document["root"].contains("children"));
    ASSERT_EQ(document["root"]["children"].size(), 1u);

    // The pasted node is still selected; duplicate and delete it again.
    ASSERT_TRUE(panel.ExecuteCommand("duplicate"));
    ASSERT_TRUE(panel.ExecuteCommand("save"));
    document = Json::parse(ReadFile(path));
    ASSERT_EQ(document["root"]["children"].size(), 2u);

    ASSERT_TRUE(panel.ExecuteCommand("delete"));
    ASSERT_TRUE(panel.ExecuteCommand("save"));
    document = Json::parse(ReadFile(path));
    ASSERT_EQ(document["root"]["children"].size(), 1u);

    EXPECT_TRUE(panel.ExecuteCommand("frame-all"));
    EXPECT_TRUE(panel.ExecuteCommand("zoom-100"));
    EXPECT_FALSE(panel.OwnsShortcuts());

    RenderPanel(panel);
    RenderPanel(panel);
    EXPECT_TRUE(panel.HasDocument());
}

TEST(UIDocumentDesignerPanel, DirtyDocumentDefersOpenUntilSaved)
{
    using NLS::Editor::Panels::UIDocumentDesigner;
    using NLS::Editor::UIDocumentTools::UIDocumentDesignerModel;

    ScopedDocumentDirectory directory;
    ScopedUndoService undo;
    ScopedImGuiContext imgui;

    const auto first = directory.Write(
        "Assets/UI/First.ui",
        UIDocumentDesignerModel::CreateDocumentSource("First"));
    const auto second = directory.Write(
        "Assets/UI/Second.ui",
        UIDocumentDesignerModel::CreateDocumentSource("Second"));

    NLS::UI::PanelWindowSettings settings;
    UIDocumentDesigner panel("UI Designer", true, settings);
    ASSERT_TRUE(panel.OpenDocument(first));
    ASSERT_EQ(panel.GetDocumentPath(), first);

    // Dirty the document without saving.
    ASSERT_TRUE(panel.ExecuteCommand("copy"));
    ASSERT_TRUE(panel.ExecuteCommand("paste"));

    ASSERT_TRUE(panel.OpenDocument(second));
    EXPECT_EQ(panel.GetDocumentPath(), first) << "dirty open must be deferred";

    // The deferred dialog is presented on the next panel frame.
    RenderPanel(panel);

    ASSERT_TRUE(panel.ExecuteCommand("save"));
    ASSERT_TRUE(panel.OpenDocument(second));
    EXPECT_EQ(panel.GetDocumentPath(), second);
}

TEST(UIDocumentDesignerPanel, TitleBarCloseAllowsReopeningTheDocument)
{
    using NLS::Editor::Panels::UIDocumentDesigner;
    using NLS::Editor::UIDocumentTools::UIDocumentDesignerModel;

    ScopedDocumentDirectory directory;
    ScopedUndoService undo;
    ScopedImGuiContext imgui;
    ImGui::GetIO().DisplaySize = { 1600.0f, 1000.0f };
    const auto path = directory.Write(
        "Assets/UI/Hud.ui",
        UIDocumentDesignerModel::CreateDocumentSource("Hud"));
    NLS::UI::PanelWindowSettings settings;
    settings.closable = true;
    UIDocumentDesigner panel("UI Designer", true, settings);
    ASSERT_TRUE(panel.OpenDocument(path));
    RenderPanel(panel);
    RenderPanel(panel);

    const auto* window = ImGui::FindWindowByName("UI Designer##UI Designer");
    ASSERT_NE(window, nullptr);
    const auto& style = ImGui::GetStyle();
    const ImVec2 closeButton {
        window->Pos.x + window->Size.x - style.FramePadding.x - ImGui::GetFontSize() * 0.5f,
        window->Pos.y + style.FramePadding.y + ImGui::GetFontSize() * 0.5f
    };
    auto& io = ImGui::GetIO();
    io.AddMousePosEvent(closeButton.x, closeButton.y);
    RenderPanel(panel);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    RenderPanel(panel);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    RenderPanel(panel);
    EXPECT_FALSE(panel.IsOpened());
    EXPECT_FALSE(panel.OwnsShortcuts());
    RenderPanel(panel);

    ASSERT_TRUE(panel.OpenDocument(path));
    RenderPanel(panel);
    EXPECT_TRUE(panel.IsOpened());
    EXPECT_EQ(panel.GetDocumentPath(), path);
}
