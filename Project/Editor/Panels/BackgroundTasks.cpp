#include "Panels/BackgroundTasks.h"
#include <imgui.h>

namespace NLS::Editor::Panels
{
BackgroundTasks::BackgroundTasks(const std::string& title, bool opened,
    const UI::PanelWindowSettings& settings, Core::EditorTaskProgressService& tasks)
    : PanelWindow(title, opened, settings), m_tasks(tasks)
{
    minSize = {300.0f, 160.0f};
}

void BackgroundTasks::OnBeforeDrawWidgets()
{
    if (ImGui::Button("Clear completed")) m_tasks.ClearCompleted();
    ImGui::Separator();
    const auto tasks = m_tasks.GetSnapshot();
    if (tasks.empty()) ImGui::TextDisabled("No background tasks");
    for (const auto& task : tasks)
    {
        ImGui::PushID(std::to_string(task.id).c_str());
        ImGui::TextUnformatted(task.title.c_str());
        ImGui::SameLine();
        const auto color = task.state == Core::EditorTaskState::Failed ? ImVec4(1,.35f,.35f,1) : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
        ImGui::TextColored(color, "%s", task.cancellationRequested && task.state == Core::EditorTaskState::Running
            ? "Cancelling..." : Core::EditorTaskStateName(task.state));
        if (!task.detail.empty()) ImGui::TextWrapped("%s", task.detail.c_str());
        if (task.progress) ImGui::ProgressBar(*task.progress, ImVec2(-1, 0));
        else if (task.state == Core::EditorTaskState::Running) ImGui::TextDisabled("Working...");
        if (task.state == Core::EditorTaskState::Running && task.cancellable)
            if (ImGui::Button("Cancel")) m_tasks.RequestCancel(task.id);
        ImGui::Separator();
        ImGui::PopID();
    }
}
}
