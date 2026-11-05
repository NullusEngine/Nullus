#include <gtest/gtest.h>
#include "Core/EditorGuiInput.h"

TEST(EditorInputFocusTests, TextDeactivationDuringDrawingDoesNotBlockTheRestOfTheFrame)
{
    auto* previous = ImGui::GetCurrentContext();
    auto* context = ImGui::CreateContext();
    struct ContextScope
    {
        ImGuiContext* current;
        ImGuiContext* previous;
        ~ContextScope() { ImGui::DestroyContext(current); ImGui::SetCurrentContext(previous); }
    } scope{context, previous};
    auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.DisplaySize = {400, 300};
    io.Fonts->AddFontDefault();
    unsigned char* pixels = nullptr; int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    char value[32] = "search";
    for (int frame = 0; frame < 3; ++frame)
    {
        ImGui::NewFrame();
        ImGui::Begin("Text owner");
        if (frame == 0) ImGui::SetKeyboardFocusHere();
        ImGui::InputText("Search", value, sizeof(value));
        ImGui::End(); ImGui::EndFrame();
    }
    ASSERT_TRUE(NLS::Editor::Core::IsEditorTextInputActive());
    ASSERT_TRUE(io.WantTextInput);
    ImGui::ClearActiveID();
    EXPECT_TRUE(io.WantTextInput); // NewFrame's snapshot has not changed yet.
    EXPECT_FALSE(NLS::Editor::Core::IsEditorTextInputActive());
}

TEST(EditorInputFocusTests, FreshViewportPressCanLeaveTextButCannotStealAHeldPointer)
{
    auto* previous = ImGui::GetCurrentContext(); auto* context = ImGui::CreateContext();
    struct ContextScope
    {
        ImGuiContext* current; ImGuiContext* previous;
        ~ContextScope() { ImGui::DestroyContext(current); ImGui::SetCurrentContext(previous); }
    } scope{context, previous};
    auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.DisplaySize = {800, 400};
    io.MousePos = {500, 100}; io.Fonts->AddFontDefault();
    unsigned char* pixels = nullptr; int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    char value[32] = "search"; ImGuiWindow* viewport = nullptr;
    constexpr auto flags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;
    for (int frame = 0; frame < 3; ++frame)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize({300, 300});
        ImGui::Begin("Scene", nullptr, flags); viewport = ImGui::GetCurrentWindow(); ImGui::End();
        ImGui::SetNextWindowPos({400, 0}); ImGui::SetNextWindowSize({300, 300});
        ImGui::Begin("Inspector", nullptr, flags);
        if (frame == 0) ImGui::SetKeyboardFocusHere();
        ImGui::InputText("Search", value, sizeof(value));
        ImGui::End(); ImGui::EndFrame();
    }
    ASSERT_TRUE(NLS::Editor::Core::IsEditorTextInputActive());
    EXPECT_FALSE(NLS::Editor::Core::TryFocusViewportForNavigation(viewport, {100, 100}, false, false));
    EXPECT_FALSE(NLS::Editor::Core::TryFocusViewportForNavigation(viewport, {100, 100}, true, true));
    EXPECT_FALSE(NLS::Editor::Core::TryFocusViewportForNavigation(viewport, {500, 100}, true, false));
    EXPECT_TRUE(NLS::Editor::Core::IsEditorTextInputActive());
    ASSERT_TRUE(NLS::Editor::Core::TryFocusViewportForNavigation(viewport, {100, 100}, true, false));
    EXPECT_FALSE(NLS::Editor::Core::IsEditorTextInputActive());
    EXPECT_EQ(context->NavWindow, viewport);
    EXPECT_TRUE(io.WantTextInput);
}
