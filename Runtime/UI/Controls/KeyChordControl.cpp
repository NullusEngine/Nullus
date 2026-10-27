#include "UI/Controls/KeyChordControl.h"

namespace NLS::UI::Controls
{
std::string KeyChordLabel(const KeyChord& chord)
{
    if (chord.key == ImGuiKey_None)
        return "Unassigned";
    std::string label;
    if (chord.ctrl)
        label += "Ctrl+";
    if (chord.shift)
        label += "Shift+";
    if (chord.alt)
        label += "Alt+";
    if (chord.super)
        label += "Super+";
    return label + ImGui::GetKeyName(chord.key);
}
ControlResult KeyChordField(const char* id, KeyChord& chord, KeyChordState& state, const ControlOptions& options)
{
    ControlResult result;
    ImGui::PushID(id);
    ImGui::BeginDisabled(options.readOnly);
    const std::string label = state.recording ? "Press shortcut (Esc cancels)" : KeyChordLabel(chord);
    if (ImGui::Button((label + "###Chord").c_str()))
    {
        state.recording = true;
        result.started = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear"))
    {
        chord = {};
        state.recording = false;
        result.changed = result.completed = true;
    }
    if (state.recording && !options.readOnly && !result.started)
    {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            state.recording = false;
            result.canceled = true;
        }
        else
            for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; ++key)
            {
                if ((key >= ImGuiKey_LeftCtrl && key <= ImGuiKey_RightSuper) || key >= ImGuiKey_GamepadStart)
                    continue;
                if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(key), false))
                {
                    const auto& io = ImGui::GetIO();
                    chord = {static_cast<ImGuiKey>(key), io.KeyCtrl, io.KeyShift, io.KeyAlt, io.KeySuper};
                    state.recording = false;
                    result.changed = result.completed = true;
                    break;
                }
            }
    }
    result.active = state.recording;
    result.components = result.changed ? 1 : 0;
    ImGui::EndDisabled();
    ImGui::PopID();
    return result;
}
} // namespace NLS::UI::Controls
