#pragma once
#include "Core/Modules/ModuleServices.h"
namespace NLS::Plugins
{
// The scripting subsystem owns the implementation and VM. Plugin metadata can
// be resolved before Context construction; backend access happens at Startup.
class ScriptModuleAdapter final : public Core::Modules::IModule
{
public:
    ScriptModuleAdapter(std::string backend,std::filesystem::path path,std::string entry)
        : m_backend(std::move(backend)),m_path(std::move(path)),m_entry(std::move(entry)) {}
    bool Startup(Core::Modules::ModuleContext& context) override
    {
        if (!Core::ServiceLocator::Contains<Core::Modules::ScriptModuleProvider>()) throw std::runtime_error("script plugin services are unavailable in this host");
        m_module = Core::ServiceLocator::Get<Core::Modules::ScriptModuleProvider>().create(m_backend,m_path,m_entry);
        if (!m_module) throw std::runtime_error("script plugin backend is unavailable: " + m_backend);
        return m_module->Startup(context);
    }
    void RequestStop() noexcept override { if (m_module) m_module->RequestStop(); }
    void AbortStartup() noexcept override { if (m_module) m_module->AbortStartup(); }
    bool IsStopped() const noexcept override { return !m_module || m_module->IsStopped(); }
    std::string UnloadDiagnostic() const override { return m_module ? m_module->UnloadDiagnostic() : std::string{}; }
    void Shutdown() noexcept override { if (m_module) m_module->Shutdown(); }
private:
    std::string m_backend,m_entry;
    std::filesystem::path m_path;
    std::unique_ptr<Core::Modules::IModule> m_module;
};
}
