#pragma once
#include "UI/Panels/PanelWindow.h"
#include "Plugins/PluginManager.h"
#include "Packages/PackageManager.h"
#include <atomic>
#include <mutex>

namespace NLS::Editor::PluginSupport
{
class PluginManagerPanel final : public UI::PanelWindow
{
public:
    PluginManagerPanel(NLS::Plugins::PluginManager& manager, NLS::Plugins::PluginHost& host);
    ~PluginManagerPanel() override;
    void CancelPending();
protected:
    void OnAfterDrawWidgets() override;
private:
    struct State;
    void Start(const std::string& title, std::function<NLS::Packages::PackageResult()> operation);
    void Refresh();
    NLS::Plugins::PluginManager& m_manager;
    NLS::Plugins::PluginHost& m_host;
    std::shared_ptr<NLS::Packages::PackageManager> m_packages;
    std::shared_ptr<State> m_state;
    std::string m_selected;
    char m_query[256]{};
    char m_source[2048]{};
    char m_packageId[256]{}, m_version[128]{"*"}, m_revision[65]{};
    int m_sourceKind = 0;
};
}
