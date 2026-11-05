#include "PluginHost.h"
#include "SDK/NullusPlugin.h"
#include "Platform/Modules/NativeLibrary.h"
#include <bit>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>

namespace NLS::Plugins
{
using namespace Core::Modules;
namespace
{
std::atomic<uint64_t> nextAuthority{1};
std::atomic<PluginHost*> processHost{nullptr};
NlsPluginHeader Header(uint32_t size) { return {size, NLS_PLUGIN_ABI_MAJOR, NLS_PLUGIN_ABI_MINOR, 0}; }
bool Valid(const NlsPluginHeader& h, uint32_t minimum) { return h.struct_size >= minimum && h.abi_major == NLS_PLUGIN_ABI_MAJOR; }
std::string CopyString(NlsPluginString s)
{
    if (s.length > 1024 * 1024 || (s.length && !s.data)) throw std::invalid_argument("invalid ABI string");
    return s.length ? std::string(s.data, static_cast<size_t>(s.length)) : std::string{};
}
}
struct PluginHost::State
{
    struct Owner
    {
        std::string id;
        std::shared_ptr<ModuleScope> scope;
        std::map<uint64_t, RegistrationToken> tokens;
        std::atomic_bool active{true};
        LoadingPhase phase = LoadingPhase::PreEngine;
    };
    PluginHost* host = nullptr;
    std::thread::id thread = std::this_thread::get_id();
    std::map<uint64_t, std::unique_ptr<Owner>> owners;
    std::mutex ownerMutex;
    uint64_t nextOwner = 1, nextToken = 1;
    std::mutex logMutex;
    std::function<void(const std::string&)> log;
    std::function<bool(const std::string&, std::vector<uint8_t>&)> readResource;
    Owner* Find(uint64_t id)
    {
        std::lock_guard lock(ownerMutex);
        auto it = owners.find(id); return it == owners.end() ? nullptr : it->second.get();
    }
    static NlsPluginResult NLS_PLUGIN_CALL Register(void* context, uint64_t ownerId, const NlsPluginExtension* d, uint64_t* token)
    {
        try
        {
            auto& s = *static_cast<State*>(context);
            if (std::this_thread::get_id() != s.thread || !d || !token || !s.host || !Valid(d->header, sizeof(*d)) || !d->invoke) return NLS_PLUGIN_INVALID;
            auto* owner = s.Find(ownerId);
            if (!owner || !owner->active || !owner->scope) return NLS_PLUGIN_UNAVAILABLE;
            // A failed registration leaves context ownership with the caller.
            auto lifetime = std::make_shared<std::pair<void*, NlsPluginDestroy>>(d->context, nullptr);
            auto callback = [lifetime, invoke = d->invoke](ExtensionCall& call) {
                NlsPluginCall native{Header(sizeof(NlsPluginCall)), call.event, 0, call.deltaSeconds, call.value};
                if (invoke(lifetime->first, &native) != NLS_PLUGIN_OK) throw std::runtime_error("plugin callback failed");
                call.value = native.value;
            };
            auto registration = s.host->Extensions().Register(owner->scope,
                {CopyString(d->id), CopyString(d->kind), d->priority}, std::move(callback));
            if (!registration) return NLS_PLUGIN_FAILED;
            owner->scope->TrackCleanup([lifetime] { if (lifetime->second) { auto destroy = lifetime->second; lifetime->second = nullptr; destroy(lifetime->first); } });
            const auto key = s.nextToken++;
            owner->tokens.emplace(key, std::move(registration));
            lifetime->second = d->destroy; *token = key; return NLS_PLUGIN_OK;
        }
        catch (...) { return NLS_PLUGIN_FAILED; }
    }
    static NlsPluginResult NLS_PLUGIN_CALL Unregister(void* context, uint64_t ownerId, uint64_t token)
    {
        try
        {
            auto& s = *static_cast<State*>(context);
            if (std::this_thread::get_id() != s.thread) return NLS_PLUGIN_INVALID;
            auto* owner = s.Find(ownerId); if (!owner) return NLS_PLUGIN_UNAVAILABLE;
            return owner->tokens.erase(token) ? NLS_PLUGIN_OK : NLS_PLUGIN_UNAVAILABLE;
        }
        catch (...) { return NLS_PLUGIN_FAILED; }
    }
    static NlsPluginResult NLS_PLUGIN_CALL RequestUnload(void* context, uint64_t ownerId)
    {
        auto& s = *static_cast<State*>(context);
        if (std::this_thread::get_id() != s.thread || !s.host) return NLS_PLUGIN_UNAVAILABLE;
        auto* owner = s.Find(ownerId); if (!owner || !owner->active) return NLS_PLUGIN_UNAVAILABLE;
        return s.host->Modules().RequestUnload(owner->id) ? NLS_PLUGIN_OK : NLS_PLUGIN_UNAVAILABLE;
    }
    static void NLS_PLUGIN_CALL Log(void* context, uint64_t ownerId, NlsPluginString text)
    {
        try
        {
            auto& s = *static_cast<State*>(context); auto* owner = s.Find(ownerId); if (!owner || !owner->active) return;
            const char* phases[]{"PreEngine","PostEngine","PostEditor","PrePlay"};
            const auto message = owner->id + " phase=" + phases[static_cast<size_t>(owner->phase)] + " " + CopyString(text);
            std::function<void(const std::string&)> callback;
            { std::lock_guard lock(s.logMutex); callback = s.log; }
            if (callback) callback(message);
        }
        catch (...) {}
    }
    static void* NLS_PLUGIN_CALL Alloc(uint64_t bytes)
    { return bytes <= static_cast<uint64_t>((std::numeric_limits<size_t>::max)()) ? std::malloc(static_cast<size_t>(bytes)) : nullptr; }
    static void NLS_PLUGIN_CALL Free(void* memory) { std::free(memory); }
    static NlsPluginResult NLS_PLUGIN_CALL ReadResource(void* context, uint64_t ownerId, NlsPluginString path,
        void* buffer, uint64_t capacity, uint64_t* required)
    {
        try
        {
            auto& s = *static_cast<State*>(context);
            if (std::this_thread::get_id() != s.thread || !required) return NLS_PLUGIN_INVALID;
            auto* owner = s.Find(ownerId);
            if (!s.host || !owner || !owner->active || !s.readResource) return NLS_PLUGIN_UNAVAILABLE;
            std::vector<uint8_t> bytes;
            if (!s.readResource(CopyString(path), bytes)) return NLS_PLUGIN_UNAVAILABLE;
            *required = bytes.size();
            if (capacity < bytes.size() || (!buffer && !bytes.empty())) return NLS_PLUGIN_BUFFER_TOO_SMALL;
            if (!bytes.empty()) std::memcpy(buffer, bytes.data(), bytes.size());
            return NLS_PLUGIN_OK;
        }
        catch (...) { return NLS_PLUGIN_FAILED; }
    }

    class NativeModule final : public IModule
    {
    public:
        NativeModule(std::shared_ptr<State> state, std::shared_ptr<Platform::NativeLibrary> library, NlsPluginApi api)
            : m_state(std::move(state)), m_library(std::move(library)), m_api(api) {}
        ~NativeModule() override
        {
            if (m_instance) m_api.destroy(m_instance);
            if (m_stopped && (m_api.capabilities & NLS_PLUGIN_CAP_SAFE_UNLOAD)) m_library->AllowUnload();
        }
        bool Startup(ModuleContext& context) override
        {
            m_owner = m_state->nextOwner++;
            auto owner = std::make_unique<Owner>(); owner->id = context.scope->Id(); owner->scope = context.scope; owner->phase = context.phase;
            { std::lock_guard lock(m_state->ownerMutex); m_state->owners.emplace(m_owner, std::move(owner)); }
            m_hostApi = {Header(sizeof(NlsPluginHostApi)), 0, m_state.get(), m_owner, m_state->host->AuthorityId(),
                Register, Unregister, RequestUnload, Log, Alloc, Free, ReadResource};
            auto state = m_state; const auto ownerId = m_owner;
            context.scope->TrackRevocation([state, ownerId] { if (auto* owner = state->Find(ownerId)) { owner->active = false; owner->tokens.clear(); } });
            context.scope->TrackCleanup([state, ownerId] { if (auto* owner = state->Find(ownerId)) owner->scope.reset(); });
            if (m_api.create(&m_hostApi, &m_instance) != NLS_PLUGIN_OK || !m_instance) return false;
            context.scope->TrackAsync([] {}, [this] { return !m_instance || m_api.is_stopped(m_instance) == NLS_PLUGIN_OK; });
            return m_api.startup(m_instance) == NLS_PLUGIN_OK;
        }
        void RequestStop() noexcept override { if (m_instance) m_api.request_stop(m_instance); }
        void AbortStartup() noexcept override { if (m_instance) m_api.abort_startup(m_instance); }
        bool IsStopped() const noexcept override { return !m_instance || m_api.is_stopped(m_instance) == NLS_PLUGIN_OK; }
        void Shutdown() noexcept override { if (m_instance) m_api.shutdown(m_instance); m_stopped = true; }
    private:
        std::shared_ptr<State> m_state;
        std::shared_ptr<Platform::NativeLibrary> m_library;
        NlsPluginApi m_api{};
        NlsPluginHostApi m_hostApi{};
        void* m_instance = nullptr;
        uint64_t m_owner = 0;
        bool m_stopped = false;
    };
};
PluginHost::PluginHost(HostKind kind)
    : m_state(std::make_shared<State>()), m_authority(nextAuthority.fetch_add(1)), m_modules(m_extensions, kind)
{
    PluginHost* expected = nullptr;
    if (!processHost.compare_exchange_strong(expected, this))
        throw std::logic_error("a process plugin authority already exists");
    m_state->host = this;
}
PluginHost::~PluginHost()
{
    m_modules.ShutdownAll();
    // Owner tombstones and the function table context remain valid if a plugin
    // cannot drain. They report unavailable instead of dereferencing a dead host.
    m_state->host = nullptr;
    std::lock_guard lock(m_state->logMutex);
    m_state->log = {};
    m_state->readResource = {};
    if (!m_modules.HasPinnedModules()) processHost = nullptr;
}
void PluginHost::SetLog(std::function<void(const std::string&)> callback)
{ std::lock_guard lock(m_state->logMutex); m_state->log = std::move(callback); }
void PluginHost::SetResourceReader(std::function<bool(const std::string&, std::vector<uint8_t>&)> reader)
{ m_state->readResource = std::move(reader); }
bool PluginHost::RegisterBinary(ModuleDescriptor descriptor, const std::filesystem::path& path)
{
    try
    {
        auto library = std::make_shared<Platform::NativeLibrary>(path);
        auto query = reinterpret_cast<NlsPluginQueryApi>(library->Symbol("NullusPlugin_QueryApi"));
        if (!query) throw std::runtime_error("missing NullusPlugin_QueryApi export");
        NlsPluginApi api{}; api.header = Header(sizeof(api));
        if (query(NLS_PLUGIN_ABI_MAJOR, NLS_PLUGIN_ABI_MINOR, &api) != NLS_PLUGIN_OK || !Valid(api.header, sizeof(api)))
            throw std::runtime_error("plugin ABI negotiation failed");
        if (api.os != NLS_PLUGIN_OS || api.architecture != NLS_PLUGIN_ARCH || api.pointer_bits != sizeof(void*) * 8 ||
            api.little_endian != static_cast<uint32_t>(std::endian::native == std::endian::little))
            throw std::runtime_error("plugin target/architecture mismatch");
        if (!api.create || !api.startup || !api.request_stop || !api.abort_startup || !api.shutdown || !api.destroy || !api.is_stopped)
            throw std::runtime_error("incomplete plugin lifecycle table");
        descriptor.supportsUnload = descriptor.supportsUnload && (api.capabilities & NLS_PLUGIN_CAP_SAFE_UNLOAD);
        return m_modules.Register(std::move(descriptor), [state = m_state, library, api] {
            return std::make_unique<State::NativeModule>(state, library, api);
        });
    }
    catch (const std::exception& e) { m_error = e.what(); return false; }
}
}
