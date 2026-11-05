#include "PluginManagerPanel.h"
#include "Packages/RemotePackageSources.h"
#include "Core/EditorTaskProgressService.h"
#include "Jobs/BackgroundJobQueue.h"
#include "ImGui/imgui.h"
#include "Platform/Utils/SystemCalls.h"
#include <algorithm>
#include <cstring>

namespace NLS::Editor::PluginSupport
{
struct PluginManagerPanel::State
{
    std::atomic_bool busy{false}, cancel{false};
    std::mutex mutex;
    std::vector<NLS::Plugins::PluginManifest> rows;
    std::vector<NLS::Plugins::PluginManifest> remote;
    NLS::Plugins::ProjectPluginConfiguration config;
    std::string message;
};
PluginManagerPanel::PluginManagerPanel(NLS::Plugins::PluginManager& manager, NLS::Plugins::PluginHost& host)
    : PanelWindow("Plugin Manager",false), m_manager(manager), m_host(host), m_state(std::make_shared<State>())
{
    try { m_packages = std::make_shared<NLS::Packages::PackageManager>(manager.ProjectRoot()); NLS::Packages::ConfigureRemotePackageSources(*m_packages); m_packages->AddSearchRoot(manager.EnginePluginRoot()); Refresh(); }
    catch (const std::exception& e) { m_state->message = e.what(); }
}
PluginManagerPanel::~PluginManagerPanel() { CancelPending(); }
void PluginManagerPanel::CancelPending() { m_state->cancel = true; }
void PluginManagerPanel::Start(const std::string& title, std::function<NLS::Packages::PackageResult()> operation)
{
    if (!m_packages || m_state->busy.exchange(true)) return;
    m_state->cancel = false;
    struct Work
    {
        std::shared_ptr<State> state;
        std::shared_ptr<NLS::Packages::PackageManager> packages;
        std::function<NLS::Packages::PackageResult()> operation;
        Core::EditorTaskHandle task;
    };
    auto state = m_state;
    auto* work = new Work{state,m_packages,std::move(operation),
        Core::TryBeginEditorTask(title,"Working",std::nullopt,[state] { state->cancel = true; })};
    NLS::Base::Jobs::BackgroundJobDesc job;
    job.debugName = "Plugin package operation"; job.userData = work; job.cancelUserData = work;
    job.function = [](void* data) {
        std::unique_ptr<Work> work(static_cast<Work*>(data));
        try
        {
            auto result = work->operation();
            auto rows = work->packages->List();
            auto config = NLS::Plugins::PluginManager::ReadConfiguration(work->packages->Project());
            { std::lock_guard lock(work->state->mutex); work->state->rows = std::move(rows); work->state->config = std::move(config);
              work->state->message = result.succeeded ? "Completed. Code plugin changes take effect after restarting the editor." : result.error; }
            work->task.Finish(result.succeeded ? Core::EditorTaskState::Succeeded : Core::EditorTaskState::Failed,result.error);
        }
        catch (const std::exception& e)
        { std::lock_guard lock(work->state->mutex); work->state->message = e.what(); work->task.Finish(Core::EditorTaskState::Failed,e.what()); }
        work->state->busy = false;
    };
    job.cancelFunction = [](void* data) {
        std::unique_ptr<Work> work(static_cast<Work*>(data)); work->state->cancel = true;
        work->task.Finish(Core::EditorTaskState::Cancelled,"Cancelled"); work->state->busy = false;
    };
    const auto handle = NLS::Base::Jobs::ScheduleBackgroundJob(job);
    if (!handle.id)
    {
        work->task.Finish(Core::EditorTaskState::Failed,"Background scheduler rejected the operation");
        delete work; m_state->busy = false;
        std::lock_guard lock(m_state->mutex); m_state->message = "Background scheduler is unavailable.";
    }
}
void PluginManagerPanel::Refresh()
{
    Start("Scan plugins",[] { NLS::Packages::PackageResult result; result.succeeded = true; return result; });
}
void PluginManagerPanel::OnAfterDrawWidgets()
{
    std::vector<NLS::Plugins::PluginManifest> rows;
    NLS::Plugins::ProjectPluginConfiguration config;
    std::string message;
    { std::lock_guard lock(m_state->mutex); rows = m_state->rows; config = m_state->config; message = m_state->message;
      for (const auto& remote : m_state->remote)
          if (std::none_of(rows.begin(),rows.end(),[&](const auto& local) { return local.id == remote.id && local.version == remote.version && local.source == remote.source; })) rows.push_back(remote); }
    ImGui::InputText("Search",m_query,sizeof(m_query));
    const bool busy = m_state->busy.load();
    ImGui::BeginDisabled(busy || !m_packages);
    if (ImGui::Button("Rescan")) Refresh(); ImGui::SameLine();
    if (ImGui::Button("Validate")) { auto p = m_packages; Start("Validate plugins",[p] { return p->Validate(); }); } ImGui::SameLine();
    if (ImGui::Button("Restore offline")) { auto p = m_packages; Start("Restore plugins",[p] { return p->RestoreOffline(); }); } ImGui::SameLine();
    if (ImGui::Button("Repair")) { auto p = m_packages; Start("Repair plugins",[p] { return p->Repair(); }); }
    ImGui::Combo("Source",&m_sourceKind,"Directory\0ZIP\0Git HTTPS\0Nullus HTTPS Registry\0");
    ImGui::InputText("Package location",m_source,sizeof(m_source));
    if (m_sourceKind == 2) ImGui::InputText("Full commit",m_revision,sizeof(m_revision));
    if (m_sourceKind == 3) { ImGui::InputText("Package ID",m_packageId,sizeof(m_packageId)); ImGui::InputText("Version range",m_version,sizeof(m_version)); }
    if (m_sourceKind == 3 && ImGui::Button("Search Registry") && m_source[0])
    {
        auto packages = m_packages; auto state = m_state; const std::string source = m_source, query = m_query;
        Start("Search Registry",[packages,state,source,query] {
            auto results = packages->SearchRegistry(source,query,&state->cancel);
            { std::lock_guard lock(state->mutex); state->remote = std::move(results); }
            NLS::Packages::PackageResult result; result.succeeded = true; return result;
        });
    }
    if (ImGui::Button("Install / Update") && m_source[0]) ImGui::OpenPopup("Confirm package installation");
    if (ImGui::BeginPopupModal("Confirm package installation",nullptr,ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextWrapped("Install package from %s? Installation keeps new plugins disabled.",m_source);
        if (ImGui::Button("Install"))
        {
            NLS::Packages::PackageRequest request{static_cast<NLS::Packages::PackageSourceKind>(m_sourceKind),m_source};
            if (m_sourceKind == 2) request.revision = m_revision;
            if (m_sourceKind == 3) { request.id = m_packageId; request.version = m_version; }
            request.cancel = &m_state->cancel;
            auto p = m_packages; auto state = m_state; Start("Install plugin",[p,request,state] { return p->Install(request); }); ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine(); if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup(); ImGui::EndPopup();
    }
    ImGui::Separator();
    for (const auto& plugin : rows)
    {
        if (m_query[0] && plugin.id.find(m_query) == std::string::npos && plugin.name.find(m_query) == std::string::npos) continue;
        const auto key = plugin.id + "@" + plugin.version.String() + "|" + plugin.source;
        if (ImGui::Selectable((plugin.name + " " + plugin.version.String() + "##" + key).c_str(),m_selected == key))
        { m_selected = key; std::strncpy(m_source,plugin.source.c_str(),sizeof(m_source)-1); m_source[sizeof(m_source)-1] = 0; }
        if (m_selected != key) continue;
        ImGui::Indent(); ImGui::TextUnformatted(plugin.id.c_str()); ImGui::TextWrapped("%s",plugin.description.c_str());
        ImGui::TextWrapped("Author: %s\nSource: %s\nEngine: %s",plugin.author.c_str(),plugin.source.c_str(),plugin.engineVersion.c_str());
        for (const auto& warning : plugin.warnings) ImGui::TextWrapped("%s",warning.c_str());
        if (!std::filesystem::is_regular_file(plugin.root / "plugin.json"))
        {
            ImGui::TextUnformatted("Available in Registry. TLS and SHA-256 verify transport and content; publisher signatures are not available.");
            if (ImGui::Button("Use this version"))
            { std::strncpy(m_packageId,plugin.id.c_str(),sizeof(m_packageId)-1); const auto version = plugin.version.String(); std::strncpy(m_version,version.c_str(),sizeof(m_version)-1); m_sourceKind = 3; }
            ImGui::Unindent(); continue;
        }
        const bool desired = config.enabled.count(plugin.id) && config.enabled.at(plugin.id);
        const auto active = std::find_if(m_manager.Active().begin(),m_manager.Active().end(),[&](const auto& p) { return p.id == plugin.id; });
        ImGui::Text("Target: %s | Running: %s",desired ? "Enabled" : "Disabled",active != m_manager.Active().end() ? active->version.String().c_str() : "No");
        if (desired != (active != m_manager.Active().end()) || (active != m_manager.Active().end() && active->version != plugin.version)) ImGui::TextUnformatted("Restart required");
        const auto id = plugin.id; auto p = m_packages;
        if (ImGui::Button(desired ? "Disable" : "Enable")) Start(desired ? "Disable plugin" : "Enable plugin",[p,id,desired] { return p->SetEnabled(id,!desired); });
        ImGui::SameLine(); if (ImGui::Button("Uninstall")) Start("Uninstall plugin",[p,id] { return p->Uninstall(id); });
        ImGui::SameLine(); if (ImGui::Button("Open directory")) NLS::Platform::SystemCalls::OpenFile(plugin.root.string());
        if (std::any_of(plugin.modules.begin(),plugin.modules.end(),[](const auto& m) { return !m.sourceTarget.empty(); }))
        {
            if (ImGui::Button("Review / approve source build")) ImGui::OpenPopup("Approve source build");
            if (ImGui::BeginPopupModal("Approve source build",nullptr,ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::TextWrapped("Allow build scripts and tools shipped in this package to run? Package content changes require approval again. Build outputs must be outside the package.\n%s",plugin.source.c_str());
                if (ImGui::Button("Approve")) { Start("Approve source build",[p,id] { return p->ApproveSourceBuild(id); }); ImGui::CloseCurrentPopup(); }
                ImGui::SameLine(); if (ImGui::Button("Cancel approval")) ImGui::CloseCurrentPopup(); ImGui::EndPopup();
            }
        }
        for (const auto& module : plugin.modules) ImGui::BulletText("Module: %s (state %d)",module.name.c_str(),static_cast<int>(m_host.Modules().State(module.descriptor.id)));
        for (const auto& dependency : plugin.dependencies) ImGui::BulletText("Requires: %s %s",dependency.id.c_str(),dependency.version.c_str());
        for (const auto& other : rows) for (const auto& dependency : other.dependencies) if (dependency.id == plugin.id) ImGui::BulletText("Required by: %s",other.id.c_str());
        ImGui::Unindent();
    }
    ImGui::EndDisabled();
    if (busy) ImGui::TextUnformatted("Working...");
    if (!message.empty()) ImGui::TextWrapped("%s",message.c_str());
    for (const auto& diagnostic : m_manager.Catalog().Diagnostics()) ImGui::TextWrapped("%s",diagnostic.c_str());
}
}
