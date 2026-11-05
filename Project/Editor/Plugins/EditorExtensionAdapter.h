#pragma once
#include "Plugins/PluginManager.h"
#include <map>

namespace NLS::Editor::Core { class PanelsManager; }
namespace NLS::Editor::PluginSupport
{
class EditorExtensionAdapter
{
public:
    EditorExtensionAdapter(Core::PanelsManager& panels, NLS::Plugins::PluginManager& manager, NLS::Plugins::PluginHost& host);
    ~EditorExtensionAdapter();
    void Sync();
private:
    void Clear();
    Core::PanelsManager& m_panels;
    NLS::Plugins::PluginManager& m_manager;
    NLS::Plugins::PluginHost& m_host;
    std::vector<std::string> m_panelIds, m_commandPaths;
    std::vector<std::string> m_shortcutIds;
    uint64_t m_revision = UINT64_MAX;
};
}
