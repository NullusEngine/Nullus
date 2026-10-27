#pragma once
#include <imgui.h>
#include <VFX/Authoring/VfxAuthoring.h>

namespace NLS::Editor::VFXTools
{
// Scoped candidate theme. Stack accent colors come from UE 5.7.4's
// NiagaraEditorWidgetsStyle; full font/icon/layout screenshot parity is gated
// separately in NiagaraParity.json.
class VfxStyle final
{
public:
    VfxStyle()
    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{8,8});
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,{6,4});
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,{6,4});
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,2);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,0);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding,0);
        ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding,2);
        ImGui::PushStyleColor(ImGuiCol_WindowBg,ImVec4{0.035f,0.035f,0.035f,1});
        ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4{0.055f,0.055f,0.055f,1});
        ImGui::PushStyleColor(ImGuiCol_FrameBg,ImVec4{0.025f,0.025f,0.025f,1});
        ImGui::PushStyleColor(ImGuiCol_Header,ImVec4{0.12f,0.12f,0.12f,1});
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered,ImVec4{0.18f,0.18f,0.18f,1});
        ImGui::PushStyleColor(ImGuiCol_HeaderActive,ImVec4{0.08f,0.29f,0.55f,1});
        ImGui::PushStyleColor(ImGuiCol_Button,ImVec4{0.12f,0.12f,0.12f,1});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4{0.22f,0.22f,0.22f,1});
        ImGui::PushStyleColor(ImGuiCol_Tab,ImVec4{0.075f,0.075f,0.075f,1});
        ImGui::PushStyleColor(ImGuiCol_TabSelected,ImVec4{0.15f,0.15f,0.15f,1});
        ImGui::PushStyleColor(ImGuiCol_Border,ImVec4{0.015f,0.015f,0.015f,1});
    }
    ~VfxStyle() { ImGui::PopStyleColor(11); ImGui::PopStyleVar(7); }
    static ImU32 StageColor(NLS::VFX::Authoring::Stage stage)
    {
        using NLS::VFX::Authoring::Stage;
        switch (stage)
        {
        case Stage::SystemSpawn: case Stage::SystemUpdate: return IM_COL32(67,105,124,255);
        case Stage::EmitterSpawn: case Stage::EmitterUpdate: return IM_COL32(126,87,67,255);
        case Stage::ParticleSpawn: case Stage::ParticleUpdate: return IM_COL32(87,107,61,255);
        default: return IM_COL32(87,107,61,255);
        }
    }
    static ImU32 StageIconColor(NLS::VFX::Authoring::Stage stage)
    {
        using NLS::VFX::Authoring::Stage;
        if (stage == Stage::SystemSpawn || stage == Stage::SystemUpdate) return IM_COL32(1,202,252,255);
        if (stage == Stage::EmitterSpawn || stage == Stage::EmitterUpdate) return IM_COL32(241,99,6,255);
        return IM_COL32(131,218,9,255);
    }
};
}
