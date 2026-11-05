#pragma once
#include "ModuleSystem.h"
#include "Core/ServiceLocator.h"
#include <stdexcept>
#include <filesystem>

namespace NLS::Core::Modules
{
struct ScriptModuleProvider
{
    std::function<std::unique_ptr<IModule>(const std::string&,const std::filesystem::path&,const std::string&)> create;
};
// Injected by the executable. Engine/Scripting depend on this small Core
// capability, not on a concrete PluginManager or package downloader.
struct HostLifecycle
{
    std::function<bool(LoadingPhase)> advance;
    std::function<void()> close;
};
inline void AdvanceHostPhase(LoadingPhase phase)
{
    if (ServiceLocator::Contains<HostLifecycle>() && !ServiceLocator::Get<HostLifecycle>().advance(phase))
        throw std::runtime_error("plugin loading phase failed");
}
inline void CloseHostModules()
{
    if (ServiceLocator::Contains<HostLifecycle>()) ServiceLocator::Get<HostLifecycle>().close();
}
class RuntimeExtensionService
{
public:
    explicit RuntimeExtensionService(ExtensionRegistry& registry) : m_registry(registry) {}
    bool Prepare()
    {
        return !ServiceLocator::Contains<HostLifecycle>() || ServiceLocator::Get<HostLifecycle>().advance(LoadingPhase::PrePlay);
    }
    void Begin() { m_active = true; Dispatch(1, 0); }
    void Tick(double delta) { if (m_active) Dispatch(2, delta); }
    void End() { if (m_active) { Dispatch(3, 0); m_active = false; } }
private:
    void Dispatch(uint32_t event, double delta)
    {
        const auto revision=m_registry.Revision();
        if (revision!=m_revision)
        {
            m_ids.clear();
            for (auto& lease : m_registry.FindKind("runtime.play")) m_ids.push_back(lease.Descriptor()->id);
            m_revision=revision;
        }
        // Cache identities, never execution leases across frames. Each lookup
        // still checks the current owner gate and cannot pin a quiescing DLL.
        for (const auto& id : m_ids)
        { auto lease=m_registry.Find(id); ExtensionCall call{event,delta}; lease.Invoke(call); }
    }
    ExtensionRegistry& m_registry;
    bool m_active = false;
    uint64_t m_revision=UINT64_MAX;
    std::vector<std::string> m_ids;
};
}
