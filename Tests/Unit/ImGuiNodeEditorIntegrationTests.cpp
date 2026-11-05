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
        ImGui::SetCurrentContext(m_context);
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

TEST_F(NodeEditorIntegration, HostKeyboardRequestsDeleteThroughExistingApiAndKeepPointerGestures)
{
    namespace ed = ax::NodeEditor;
    struct Observer { int draws = 0; int destroys = 0; } observed;
    ed::SetKeyboardInputObserver([](ed::EditorContext*, bool, bool destroyed, bool, void* user)
    { auto& value = *static_cast<Observer*>(user); if (destroyed) ++value.destroys; else ++value.draws; }, &observed);
    struct Restore { ~Restore() { ed::SetKeyboardInputObserver(nullptr); } } restore;
    ed::Config config; config.SettingsFile = nullptr;
    auto* context = ed::CreateEditor(&config); ed::SetCurrentEditor(context);
    EXPECT_FALSE(ed::AreKeyboardShortcutsEnabled()); EXPECT_TRUE(ed::AreShortcutsEnabled());
    int deleted = 0;
    for (int frame = 0; frame < 6; ++frame)
    {
        ImGui::GetIO().AddMousePosEvent(100, 100);
        ImGui::NewFrame(); ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize({700, 500}); ImGui::Begin("Managed Graph"); ImGui::SetWindowFocus();
        ed::Begin("Canvas", {600, 400}); ed::BeginNode(ed::NodeId(1)); ImGui::TextUnformatted("Node"); ed::EndNode();
        if (ed::BeginDelete())
        {
            ed::NodeId node;
            while (ed::QueryDeletedNode(&node)) if (ed::AcceptDeletedItem()) { EXPECT_EQ(node, ed::NodeId(1)); ++deleted; }
            ed::EndDelete();
        }
        ed::End();
        if (frame == 1) { ed::SelectNode(ed::NodeId(1)); ed::RequestKeyboardShortcut(ed::KeyboardShortcut::Delete); }
        ImGui::End(); ImGui::Render();
    }
    EXPECT_EQ(deleted, 1); EXPECT_EQ(observed.draws, 6);
    ed::DestroyEditor(context); EXPECT_EQ(observed.destroys, 1);
}
