#include "ECSBakingPanel.h"
#include <ImGui/imgui.h>
#include <algorithm>

namespace NLS::Editor::Panels
{
ECSBakingPanel::ECSBakingPanel(const std::string& name, bool opened, const UI::PanelWindowSettings& settings,
    ECSAuthoring::SceneBakingService& service, std::filesystem::path assetsRoot,
    std::function<bool(const std::filesystem::path&)> import)
    : UI::PanelWindow(name, opened, settings), m_service(service), m_assetsRoot(std::move(assetsRoot)), m_import(std::move(import))
{
    const std::string output = "BakedScene.ecsscene";
    std::copy(output.begin(), output.end(), m_output.begin());
}

void ECSBakingPanel::OnBeforeDrawWidgets()
{
    using Engine::ECSIntegration::BakingMode;
    bool live = m_service.IsLive();
    if (ImGui::Checkbox("Live Baking", &live)) m_service.SetLive(live);
    const auto unavailable = m_service.GetUnavailableReason();
    ImGui::BeginDisabled(!unavailable.empty());
    if (ImGui::Button("Bake Incremental")) m_service.RequestBake(BakingMode::Incremental);
    ImGui::SameLine();
    if (ImGui::Button("Bake Full")) m_service.RequestBake(BakingMode::Full);
    ImGui::InputText("Output in Assets", m_output.data(), m_output.size());
    if (ImGui::Button("Export ECS Scene"))
    {
        try
        {
            const auto relative = std::filesystem::u8path(m_output.data()).lexically_normal();
            const auto root = std::filesystem::weakly_canonical(m_assetsRoot);
            const auto destination = std::filesystem::weakly_canonical(root / relative);
            const auto within = destination.lexically_relative(root);
            if (relative.is_absolute() || within.empty() || *within.begin() == "..")
                m_exportMessage = "Choose an output inside the project Assets folder.";
            else if (m_service.ExportScene(destination, m_import))
                m_exportMessage = "ECS Scene exported and imported: " + relative.generic_string();
            else m_exportMessage = m_service.GetLastResult().diagnostics.empty()
                ? "ECS Scene export failed." : m_service.GetLastResult().diagnostics.front();
        }
        catch (const std::exception& exception) { m_exportMessage = exception.what(); }
    }
    ImGui::EndDisabled();
    if (!unavailable.empty()) ImGui::TextWrapped("%s", unavailable.c_str());
    if (!m_exportMessage.empty()) ImGui::TextWrapped("%s", m_exportMessage.c_str());
    const auto& result = m_service.GetLastResult();
    ImGui::Separator();
    ImGui::Text("CPU Preview: %zu entities", m_service.GetPreview().GetWorld().GetEntityCount());
    ImGui::Text("Bakers rebuilt: %zu   Reused: %zu   Removed: %zu",
        result.statistics.rebuilt, result.statistics.reused, result.statistics.removed);
    for (const auto& diagnostic : result.diagnostics) ImGui::TextWrapped("%s", diagnostic.c_str());
    if (!result.artifact) return;
    if (ImGui::BeginChild("Entities", ImVec2(0, 220)))
    {
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(result.artifact->entities.size()));
        while (clipper.Step())
        {
            for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index)
            {
                const auto& entity = result.artifact->entities[index];
                const auto label = entity.id + " (" + std::to_string(entity.components.size()) + " components)";
                if (ImGui::Selectable(label.c_str(), m_selected == entity.id)) m_selected = entity.id;
            }
        }
    }
    ImGui::EndChild();
    const auto selected = std::find_if(result.artifact->entities.begin(), result.artifact->entities.end(),
        [this](const auto& entity) { return entity.id == m_selected; });
    if (selected == result.artifact->entities.end()) return;
    ImGui::TextWrapped("Authoring ID: %s", selected->authoringId.c_str());
    for (const auto& component : selected->components)
    {
        if (!ImGui::TreeNode(component.name.c_str())) continue;
        const auto json = component.value.dump(2);
        ImGui::TextWrapped("%s", json.c_str());
        ImGui::TreePop();
    }
}
}
