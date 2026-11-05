#include "EditorExtensionAdapter.h"
#include "Core/PanelsManager.h"
#include "Panels/EditorTopBar.h"
#include "UI/Panels/PanelWindow.h"
#include "Plugins/SDK/NullusPlugin.h"
#include "Core/ServiceLocator.h"
#include "Shortcuts/EditorShortcutService.h"

namespace NLS::Editor::PluginSupport
{
namespace
{
class ExtensionPanel final : public UI::PanelWindow
{
public:
    ExtensionPanel(const std::string& id, NLS::Core::Modules::ExtensionLease lease)
        : PanelWindow(id, false), m_lease(std::move(lease)) {}
protected:
    void OnAfterDrawWidgets() override
    {
        NLS::Core::Modules::ExtensionCall call{NLS_PLUGIN_EVENT_EDITOR_DRAW};
        m_lease.Invoke(call);
    }
private:
    NLS::Core::Modules::ExtensionLease m_lease;
};
}
EditorExtensionAdapter::EditorExtensionAdapter(Core::PanelsManager& panels, NLS::Plugins::PluginManager& manager, NLS::Plugins::PluginHost& host)
    : m_panels(panels), m_manager(manager), m_host(host) { Sync(); }
EditorExtensionAdapter::~EditorExtensionAdapter() { Clear(); }
void EditorExtensionAdapter::Clear()
{
    if (m_panels.HasPanel("Editor Top Bar"))
    {
        auto& top = m_panels.GetPanelAs<Panels::EditorTopBar>("Editor Top Bar");
        for (const auto& path : m_commandPaths) top.UnregisterCommand(path);
    }
    for (const auto& id : m_panelIds) m_panels.RemovePanel(id);
    if (NLS::Core::ServiceLocator::Contains<Shortcuts::EditorShortcutService>())
        for (const auto& id : m_shortcutIds) NLS::Core::ServiceLocator::Get<Shortcuts::EditorShortcutService>().GetRegistry().UnregisterCommand(id);
    m_shortcutIds.clear();
    m_panelIds.clear(); m_commandPaths.clear();
}
void EditorExtensionAdapter::Sync()
{
    const auto revision = m_host.Extensions().Revision();
    if (m_revision == revision || !m_panels.HasPanel("Editor Top Bar")) return;
    Clear(); m_revision = revision;
    auto& top = m_panels.GetPanelAs<Panels::EditorTopBar>("Editor Top Bar");
    for (auto& lease : m_host.Extensions().FindKind("editor.panel"))
    {
        const auto id = lease.Descriptor()->id;
        if (m_panels.HasPanel(id)) continue;
        m_panels.AddPanel(id, std::make_unique<ExtensionPanel>(id, std::move(lease)));
        m_panelIds.push_back(id);
        const auto path = "Window/" + id;
        if (top.RegisterCommand(path, [this, id] {
            if (m_panels.HasPanel(id)) { auto& panel = m_panels.GetPanelAs<UI::PanelWindow>(id); panel.Open(); panel.Focus(); }
        })) m_commandPaths.push_back(path);
    }
    for (auto& lease : m_host.Extensions().FindKind("editor.command"))
    {
        const auto id = lease.Descriptor()->id;
        const auto path = "Tools/" + id;
        if (NLS::Core::ServiceLocator::Contains<Shortcuts::EditorShortcutService>())
        {
            Shortcuts::ShortcutCommand command;
            command.id = "plugin.extension." + id; command.displayName = id; command.category = "Plugins";
            command.execute = [this,id] { auto current = m_host.Extensions().Find(id); NLS::Core::Modules::ExtensionCall call{NLS_PLUGIN_EVENT_COMMAND}; current.Invoke(call); };
            if (NLS::Core::ServiceLocator::Get<Shortcuts::EditorShortcutService>().RegisterCommand(command)) m_shortcutIds.push_back(command.id);
        }
        if (top.RegisterCommand(path, [this, id] {
            auto current = m_host.Extensions().Find(id);
            NLS::Core::Modules::ExtensionCall call{NLS_PLUGIN_EVENT_COMMAND}; current.Invoke(call);
        })) m_commandPaths.push_back(path);
    }
}
}
