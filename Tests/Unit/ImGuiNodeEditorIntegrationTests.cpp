#include <gtest/gtest.h>

#include <imgui.h>
#include <imgui_node_editor.h>

namespace
{
class NodeEditorIntegration : public testing::Test
{
protected:
    void SetUp() override
    {
        m_previous = ImGui::GetCurrentContext();
        m_context = ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = {800,600};
        io.DeltaTime = 1.0f/60;
        unsigned char* pixels;
        int width,height;
        io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
    }
    void TearDown() override
    {
        ax::NodeEditor::SetCurrentEditor(nullptr);
        ImGui::DestroyContext(m_context);
        ImGui::SetCurrentContext(m_previous);
    }
    ImGuiContext* m_previous = nullptr;
    ImGuiContext* m_context = nullptr;
};
}

TEST_F(NodeEditorIntegration, ExportedEngineApiSupportsIndependentContextsAndDrawing)
{
    namespace ed = ax::NodeEditor;
    ed::Config config;
    config.SettingsFile = nullptr;
    auto* first = ed::CreateEditor(&config);
    auto* second = ed::CreateEditor(&config);
    ASSERT_NE(first,nullptr);
    ASSERT_NE(second,nullptr);
    for (int frame = 0; frame < 3; ++frame)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowSize({700,500});
        ImGui::Begin("Graph");
        ed::SetCurrentEditor(first);
        ed::Begin("Canvas",{600,400});
        ed::BeginNode(ed::NodeId(1));
        ImGui::TextUnformatted("Constant");
        ed::BeginPin(ed::PinId(2),ed::PinKind::Output);
        ImGui::TextUnformatted("Value");
        ed::EndPin();
        ed::EndNode();
        ed::End();
        ed::SetCurrentEditor(nullptr);
        ImGui::End();
        ImGui::Render();
        if (frame > 0) EXPECT_GT(ImGui::GetDrawData()->TotalVtxCount,0);
    }
    ed::SetCurrentEditor(first);
    ed::SetNodePosition(ed::NodeId(1),{40,70});
    ed::SetCurrentEditor(second);
    ed::SetNodePosition(ed::NodeId(1),{100,200});
    ed::SetCurrentEditor(first);
    EXPECT_FLOAT_EQ(ed::GetNodePosition(ed::NodeId(1)).x,40);
    ed::DestroyEditor(second);
    ed::DestroyEditor(first);
}
