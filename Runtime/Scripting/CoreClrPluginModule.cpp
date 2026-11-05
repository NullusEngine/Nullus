#include "CoreClrScriptBackend.h"
#include "PluginScriptAbi.h"
#include <Debug/Logger.h>
#include <atomic>
#include <algorithm>
#include <stdexcept>
namespace NLS::Scripting
{
namespace
{
using namespace Core::Modules;
struct ManagedPluginState
{
    ScriptPluginApi api;
    ScriptPluginHostApi host;
    uint64_t token = 0;
    std::atomic_bool accepting{true};
    ModuleContext* startup = nullptr;
    std::string id;
    std::vector<RegistrationToken> registrations;
    std::string Error() const
    {
        if (!token || !api.diagnostic) return "managed plugin startup failed";
        const auto size = api.diagnostic(token,nullptr,0); std::string text((std::min)(size,uint32_t{65536}),0);
        api.diagnostic(token,reinterpret_cast<uint8_t*>(text.data()),static_cast<uint32_t>(text.size())); return text;
    }
};
class ManagedPluginModule final : public IModule
{
    std::filesystem::path m_path;
    std::string m_entry;
    std::shared_ptr<ManagedPluginState> m_state;
public:
    ManagedPluginModule(ScriptPluginApi api,std::filesystem::path path,std::string entry) : m_path(std::move(path)),m_entry(std::move(entry)),m_state(std::make_shared<ManagedPluginState>()) { m_state->api = api; }
    bool Startup(ModuleContext& context) override
    {
        auto state = m_state; state->startup = &context; state->id = context.scope->Id(); state->host.context = state.get();
        state->host.registerExtension = [](void* value,const uint8_t* id,uint32_t idSize,const uint8_t* kind,uint32_t kindSize,int32_t priority,uint64_t callback) -> int32_t {
            auto* state = static_cast<ManagedPluginState*>(value);
            if (!state->accepting || !state->startup || !id || !kind || idSize > 4096 || kindSize > 256) return -1;
            try
            {
                auto owner = state->startup->scope;
                auto token = state->startup->extensions.Register(owner,{std::string(reinterpret_cast<const char*>(id),idSize),std::string(reinterpret_cast<const char*>(kind),kindSize),priority},[state,callback](ExtensionCall& call) {
                    if (state->api.invoke(state->token,callback,call.event,call.deltaSeconds,&call.value) != 0) throw std::runtime_error(state->Error());
                });
                if (!token) return -1; state->registrations.push_back(std::move(token)); return 0;
            }
            catch (...) { return -1; }
        };
        state->host.log = [](void* value,const uint8_t* text,uint32_t size) {
            auto* state = static_cast<ManagedPluginState*>(value);
            if (state->accepting && text && size <= 65536) NLS_LOG_INFO("[C# plugin " + state->id + "] " + std::string(reinterpret_cast<const char*>(text),size));
        };
        context.scope->TrackCleanup([state] { state->registrations.clear(); if (state->token) { state->api.release(state->token); state->token = 0; } });
        const auto utf8 = m_path.u8string();
        const auto result = state->api.load(reinterpret_cast<const uint8_t*>(utf8.c_str()),reinterpret_cast<const uint8_t*>(m_entry.c_str()),&state->host,&state->token);
        state->startup = nullptr;
        if (result != 0) throw std::runtime_error(state->Error());
        return true;
    }
    void RequestStop() noexcept override { m_state->accepting = false; if (m_state->token) m_state->api.stop(m_state->token); }
    void AbortStartup() noexcept override { RequestStop(); }
    bool IsStopped() const noexcept override
    {
        if (!m_state->token) return true;
        const auto result = m_state->api.poll(m_state->token);
        if (result < 0) NLS_LOG_ERROR(m_state->Error());
        return result == 1;
    }
    void Shutdown() noexcept override {}
    std::string UnloadDiagnostic() const override { return m_state->Error(); }
};
}
std::unique_ptr<Core::Modules::IModule> CoreClrScriptBackend::CreatePluginModule(const std::filesystem::path& path,const std::string& entry)
{
    if (!m_initialized || !m_host.IsLoaded()) return {};
    void* function = nullptr;
    const auto status = m_host.GetUnmanagedFunctionPointer(m_assembly.parent_path() / "Nullus.Managed.dll","Nullus.Plugins.PluginExports, Nullus.Managed","GetApiTable",function);
    if (!status.Succeeded() || !function) throw std::runtime_error("CoreCLR plugin Adapter entry is unavailable");
    using GetApi = const ScriptPluginApi* (*)(); const auto* api = reinterpret_cast<GetApi>(function)();
    if (!api || api->size < sizeof(ScriptPluginApi) || api->version != 1 || !api->load || !api->invoke || !api->stop || !api->poll || !api->release || !api->diagnostic)
        throw std::runtime_error("CoreCLR plugin Adapter ABI mismatch");
    return std::make_unique<ManagedPluginModule>(*api,path,entry);
}
}
