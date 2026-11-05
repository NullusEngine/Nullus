#pragma once
#include "PluginsDef.h"
#include "Core/Modules/ModuleSystem.h"
#include <filesystem>
#include <functional>

namespace NLS::Plugins
{
class NLS_PLUGINS_API PluginHost
{
public:
    explicit PluginHost(Core::Modules::HostKind kind);
    ~PluginHost();
    PluginHost(const PluginHost&) = delete;
    PluginHost& operator=(const PluginHost&) = delete;
    bool RegisterBinary(Core::Modules::ModuleDescriptor descriptor, const std::filesystem::path& path);
    Core::Modules::ExtensionRegistry& Extensions() { return m_extensions; }
    Core::Modules::ModuleManager& Modules() { return m_modules; }
    uint64_t AuthorityId() const { return m_authority; }
    void SetLog(std::function<void(const std::string&)> callback);
    void SetResourceReader(std::function<bool(const std::string&, std::vector<uint8_t>&)> reader);
    const std::string& LastError() const { return m_error; }
private:
    struct State;
    std::shared_ptr<State> m_state;
    uint64_t m_authority;
    Core::Modules::ExtensionRegistry m_extensions;
    Core::Modules::ModuleManager m_modules;
    std::string m_error;
};
}
