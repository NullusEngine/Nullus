#pragma once
#include "PluginManager.h"
#include "Core/Modules/ModuleServices.h"
#include "Core/Assets/ContentSource.h"

namespace NLS::Plugins
{
// Declare before Application so bootstrap precedes Context member construction.
// Its services are removed only after Application and its project objects die.
class NLS_PLUGINS_API ApplicationPlugins
{
public:
    ApplicationPlugins(Core::Modules::HostKind kind, const std::filesystem::path& project,
        const std::filesystem::path& enginePlugins = {}, bool player = false,
        std::function<void(PluginManager&)> registerSources = {});
    ~ApplicationPlugins();
    PluginHost& Host() { return m_host; }
    PluginManager& Manager() { return m_manager; }
private:
    PluginHost m_host;
    PluginManager m_manager;
    Core::Modules::RuntimeExtensionService m_runtime;
    Core::Modules::HostLifecycle m_lifecycle;
    std::vector<std::pair<std::filesystem::path, std::shared_ptr<const Core::Assets::IContentSource>>> m_mounts;
};
}
