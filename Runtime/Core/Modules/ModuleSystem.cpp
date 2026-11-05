#include "ModuleSystem.h"
#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>
#include <cstdio>

namespace NLS::Core::Modules
{
ExecutionGuard::ExecutionGuard(std::shared_ptr<ModuleScope> scope) : m_scope(std::move(scope)) {}
ExecutionGuard::ExecutionGuard(ExecutionGuard&& other) noexcept : m_scope(std::move(other.m_scope)) {}
ExecutionGuard& ExecutionGuard::operator=(ExecutionGuard&& other) noexcept
{
    if (this != &other) { if (m_scope) m_scope->Leave(); m_scope = std::move(other.m_scope); }
    return *this;
}
ExecutionGuard::~ExecutionGuard() { if (m_scope) m_scope->Leave(); }
ModuleScope::ModuleScope(std::string id) : m_id(std::move(id)) {}
ExecutionGuard ModuleScope::Enter(bool startupWork)
{
    std::lock_guard lock(m_mutex);
    if (!m_accepting || (!m_committed && !startupWork)) return {};
    ++m_calls;
    return ExecutionGuard(shared_from_this());
}
void ModuleScope::Leave() { std::lock_guard lock(m_mutex); --m_calls; }
bool ModuleScope::CanAcquire() const { std::lock_guard lock(m_mutex); return m_accepting && m_committed; }
bool ModuleScope::AddLease()
{
    std::lock_guard lock(m_mutex);
    if (!m_accepting || !m_committed) return false;
    ++m_leases; return true;
}
void ModuleScope::ReleaseLease() { std::lock_guard lock(m_mutex); --m_leases; }
void ModuleScope::TrackRevocation(std::function<void()> action)
{
    std::lock_guard lock(m_mutex);
    if (!m_accepting) throw std::logic_error("module is quiescing");
    m_revocations.push_back(std::move(action));
}
void ModuleScope::TrackCleanup(std::function<void()> action)
{
    std::lock_guard lock(m_mutex);
    if (!m_accepting) throw std::logic_error("module is quiescing");
    m_cleanups.push_back(std::move(action));
}
void ModuleScope::TrackAsync(std::function<void()> cancel, std::function<bool()> complete)
{
    std::lock_guard lock(m_mutex);
    if (!m_accepting) throw std::logic_error("module is quiescing");
    m_async.emplace_back(std::move(cancel), std::move(complete));
}
void ModuleScope::Commit() { std::lock_guard lock(m_mutex); m_committed = true; }
void ModuleScope::Quiesce()
{
    std::vector<std::function<void()>> cancels;
    {
        std::lock_guard lock(m_mutex);
        if (!m_accepting) return;
        m_accepting = false;
        for (const auto& work : m_async) cancels.push_back(work.first);
    }
    for (auto& cancel : cancels) try { if (cancel) cancel(); } catch (...) { m_cleanupError = "task cancellation threw"; }
}
void ModuleScope::Revoke()
{
    std::vector<std::function<void()>> actions;
    {
        std::lock_guard lock(m_mutex);
        if (m_revoked) return;
        m_revoked = true; actions.swap(m_revocations);
    }
    for (auto it = actions.rbegin(); it != actions.rend(); ++it)
        try { (*it)(); }
        catch (const std::exception& error) { m_cleanupError = "registration revocation failed: " + std::string(error.what()); std::lock_guard lock(m_mutex); m_revocations.push_back(std::move(*it)); }
        catch (...) { m_cleanupError = "registration revocation threw"; std::lock_guard lock(m_mutex); m_revocations.push_back(std::move(*it)); }
}
bool ModuleScope::IsDrained() const
{
    std::vector<std::function<bool()>> checks;
    {
        std::lock_guard lock(m_mutex);
        if (m_calls != 0 || m_leases != 0) return false;
        for (const auto& work : m_async) checks.push_back(work.second);
    }
    for (auto& check : checks) try { if (!check || !check()) return false; } catch (...) { return false; }
    return true;
}
bool ModuleScope::Cleanup()
{
    std::vector<std::function<void()>> actions;
    {
        std::lock_guard lock(m_mutex);
        if (!m_cleaned) { m_cleaned = true; actions.swap(m_cleanups); }
    }
    for (auto it = actions.rbegin(); it != actions.rend(); ++it)
        try { (*it)(); }
        catch (const std::exception& error) { m_cleanupError = "resource cleanup failed: " + std::string(error.what()); std::lock_guard lock(m_mutex); m_cleanups.push_back(std::move(*it)); }
        catch (...) { m_cleanupError = "resource cleanup threw"; std::lock_guard lock(m_mutex); m_cleanups.push_back(std::move(*it)); }
    return m_cleanupError.empty() && IsDrained();
}

namespace Detail
{
struct ExtensionEntry
{
    ExtensionDescriptor descriptor;
    std::shared_ptr<ModuleScope> owner;
    std::function<void(ExtensionCall&)> callback;
    std::atomic<bool> visible{true};
    std::atomic_bool reportedError{false};
};
struct RegistryState
{
    std::mutex mutex;
    uint64_t next = 1;
    std::atomic<uint64_t> revision{0};
    std::map<uint64_t, std::shared_ptr<ExtensionEntry>> entries;
    void Remove(uint64_t key)
    {
        std::shared_ptr<ExtensionEntry> removed;
        {
            std::lock_guard lock(mutex);
            auto it = entries.find(key);
            if (it == entries.end()) return;
            removed = std::move(it->second); entries.erase(it);
            removed->visible = false;
            ++revision;
        }
        // Destructors can call back into the host; never run them under its lock.
    }
};
}
RegistrationToken::RegistrationToken(RegistrationToken&& other) noexcept
    : m_registry(std::move(other.m_registry)), m_key(std::exchange(other.m_key, 0)) {}
RegistrationToken& RegistrationToken::operator=(RegistrationToken&& other) noexcept
{
    if (this != &other) { Reset(); m_registry = std::move(other.m_registry); m_key = std::exchange(other.m_key, 0); }
    return *this;
}
RegistrationToken::~RegistrationToken() { Reset(); }
void RegistrationToken::Reset()
{
    const auto key = std::exchange(m_key, 0);
    if (auto registry = m_registry.lock(); registry && key) registry->Remove(key);
}
ExtensionLease::ExtensionLease(std::shared_ptr<Detail::ExtensionEntry> entry) : m_entry(std::move(entry)) {}
ExtensionLease::ExtensionLease(ExtensionLease&& other) noexcept : m_entry(std::move(other.m_entry)) {}
ExtensionLease& ExtensionLease::operator=(ExtensionLease&& other) noexcept
{
    if (this != &other) { Reset(); m_entry = std::move(other.m_entry); } return *this;
}
ExtensionLease::~ExtensionLease() { Reset(); }
void ExtensionLease::Reset() { if (m_entry) { m_entry->owner->ReleaseLease(); m_entry.reset(); } }
const ExtensionDescriptor* ExtensionLease::Descriptor() const { return m_entry ? &m_entry->descriptor : nullptr; }
bool ExtensionLease::Invoke(ExtensionCall& call) const
{
    if (!m_entry || !m_entry->visible) return false;
    auto guard = m_entry->owner->Enter();
    if (!guard || !m_entry->visible) return false;
    try { m_entry->callback(call); return true; }
    catch (const std::exception& error)
    {
        if (!m_entry->reportedError.exchange(true)) std::fprintf(stderr,"[Extension] module=%s extension=%s: %s\n",m_entry->owner->Id().c_str(),m_entry->descriptor.id.c_str(),error.what());
        return false;
    }
    catch (...) { if (!m_entry->reportedError.exchange(true)) std::fprintf(stderr,"[Extension] module=%s extension=%s: unknown callback exception\n",m_entry->owner->Id().c_str(),m_entry->descriptor.id.c_str()); return false; }
}
ExtensionRegistry::ExtensionRegistry() : m_state(std::make_shared<Detail::RegistryState>()) {}
RegistrationToken ExtensionRegistry::Register(const std::shared_ptr<ModuleScope>& owner,
    ExtensionDescriptor descriptor, std::function<void(ExtensionCall&)> callback)
{
    if (!owner || descriptor.id.empty() || descriptor.kind.empty() || !callback)
        throw std::invalid_argument("invalid extension registration");
    RegistrationToken token;
    auto entry = std::make_shared<Detail::ExtensionEntry>();
    entry->owner = owner; entry->descriptor = std::move(descriptor); entry->callback = std::move(callback);
    {
        std::lock_guard lock(m_state->mutex);
        for (const auto& [key, existing] : m_state->entries)
            if (existing->descriptor.id == entry->descriptor.id) throw std::logic_error("duplicate extension: " + entry->descriptor.id);
        token.m_registry = m_state; token.m_key = m_state->next++;
        m_state->entries.emplace(token.m_key, entry);
        ++m_state->revision;
    }
    const auto weak = token.m_registry;
    const auto key = token.m_key;
    owner->TrackRevocation([weak, key] { if (auto state = weak.lock()) state->Remove(key); });
    // Keep plugin callback storage alive through Draining, even after its
    // public registration token has been revoked.
    owner->TrackCleanup([entry] {});
    return token;
}
ExtensionLease ExtensionRegistry::Find(const std::string& id) const
{
    std::lock_guard lock(m_state->mutex);
    for (const auto& [key, entry] : m_state->entries)
        if (entry->descriptor.id == id && entry->visible && entry->owner->AddLease()) return ExtensionLease(entry);
    return {};
}
std::vector<ExtensionLease> ExtensionRegistry::FindKind(const std::string& kind) const
{
    std::vector<std::shared_ptr<Detail::ExtensionEntry>> entries;
    {
        std::lock_guard lock(m_state->mutex);
        for (const auto& [key, entry] : m_state->entries)
            if (entry->descriptor.kind == kind && entry->visible) entries.push_back(entry);
    }
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        return a->descriptor.priority != b->descriptor.priority ? a->descriptor.priority > b->descriptor.priority : a->descriptor.id < b->descriptor.id;
    });
    std::vector<ExtensionLease> leases;
    for (auto& entry : entries) if (entry->visible && entry->owner->AddLease()) leases.push_back(ExtensionLease(entry));
    return leases;
}
size_t ExtensionRegistry::Count() const { std::lock_guard lock(m_state->mutex); return m_state->entries.size(); }
uint64_t ExtensionRegistry::Revision() const { return m_state->revision.load(); }

struct ModuleManager::Record
{
    ModuleDescriptor descriptor;
    std::function<std::unique_ptr<IModule>()> factory;
    std::unique_ptr<IModule> instance;
    std::shared_ptr<ModuleScope> scope;
    ModuleState state = ModuleState::Discovered;
    bool root = false, requested = false, failed = false, stopped = false;
    bool blocked = false, explicitUnload = false;
    std::chrono::steady_clock::time_point deadline{};
};
ModuleManager::ModuleManager(ExtensionRegistry& extensions, HostKind host)
    : m_extensions(extensions), m_host(host), m_thread(std::this_thread::get_id()) {}
ModuleManager::~ModuleManager()
{
    ShutdownAll();
    // Unsafe code must stay mapped even when its application/session goes away.
    // Intentionally process-lifetime storage, never a static destructor race.
    static auto* pinned = new std::vector<std::shared_ptr<Record>>;
    for (auto& [id, record] : m_records) if (record->scope) pinned->push_back(std::move(record));
}
bool ModuleManager::Register(ModuleDescriptor descriptor, std::function<std::unique_ptr<IModule>()> factory)
{
    if (std::this_thread::get_id() != m_thread || descriptor.id.empty() || !factory || m_records.count(descriptor.id))
    { m_error = "invalid or duplicate module: " + descriptor.id; return false; }
    auto record = std::make_shared<Record>(); record->descriptor = std::move(descriptor); record->factory = std::move(factory);
    m_records.emplace(record->descriptor.id, std::move(record)); return true;
}
bool ModuleManager::CheckGraph(const std::string& id, std::vector<std::string>& stack,
    std::vector<std::string>& visited, bool runtimeOnly)
{
    if (std::find(stack.begin(), stack.end(), id) != stack.end())
    {
        m_error = "module dependency cycle: "; for (auto& node : stack) m_error += node + " -> "; m_error += id; return false;
    }
    auto found = m_records.find(id);
    if (found == m_records.end()) { m_error = "missing module: " + id; return false; }
    const auto& d = found->second->descriptor;
    if (!(d.hosts & static_cast<uint32_t>(m_host)) ||
        (m_host != HostKind::Editor && (d.type == ModuleType::Editor || d.phase == LoadingPhase::PostEditor)) ||
        (m_host == HostKind::Build && d.phase == LoadingPhase::PrePlay) ||
        (runtimeOnly && (d.type == ModuleType::Editor || d.type == ModuleType::Developer)))
    { m_error = "module type/host mismatch: " + id; return false; }
    const auto visitKey = id + (runtimeOnly ? ":runtime" : ":any");
    if (std::find(visited.begin(), visited.end(), visitKey) != visited.end()) return true;
    stack.push_back(id);
    for (const auto& dep : d.dependencies)
    {
        auto it = m_records.find(dep);
        if (it != m_records.end() && it->second->descriptor.phase > d.phase)
        { m_error = "loading phase conflict: " + id + " -> " + dep; return false; }
        if (!CheckGraph(dep, stack, visited, runtimeOnly || d.type == ModuleType::Runtime)) return false;
    }
    stack.pop_back(); visited.push_back(visitKey); return true;
}
bool ModuleManager::Validate()
{
    for (const auto& [id, record] : m_records)
    {
        std::vector<std::string> stack, visited;
        if (!CheckGraph(id, stack, visited, record->descriptor.type == ModuleType::Runtime)) return false;
    }
    return true;
}
bool ModuleManager::StartRecursive(const std::string& id, std::vector<std::string>& started)
{
    auto& r = *m_records.at(id);
    if (r.state == ModuleState::Running) return true;
    if (r.state != ModuleState::Discovered && r.state != ModuleState::SafeToUnload)
    { m_error = "module cannot start in current state: " + id; return false; }
    for (const auto& dep : r.descriptor.dependencies) if (!StartRecursive(dep, started)) return false;
    r.state = ModuleState::Starting; r.scope = std::make_shared<ModuleScope>(id);
    r.failed = r.requested = r.stopped = r.blocked = r.explicitUnload = false;
    try
    {
        r.instance = r.factory();
        if (!r.instance) throw std::runtime_error("module factory returned null");
        ModuleContext context{r.scope, m_extensions, m_host, r.descriptor.phase};
        if (!r.instance->Startup(context)) throw std::runtime_error("Startup returned false");
        r.scope->Commit(); r.state = ModuleState::Running; started.push_back(id); return true;
    }
    catch (const std::exception& e) { Fail(r, id + ": " + e.what()); }
    catch (...) { Fail(r, id + ": Startup threw an unknown exception"); }
    return false;
}
void ModuleManager::Fail(Record& r, const std::string& reason)
{
    m_error = reason; r.failed = true; r.requested = true;
    r.deadline = std::chrono::steady_clock::now() + m_drainTimeout;
    r.scope->Quiesce();
    if (r.instance) { r.instance->RequestStop(); r.instance->AbortStartup(); }
    r.scope->Revoke(); r.state = ModuleState::Draining;
}
bool ModuleManager::Start(const std::string& id)
{
    if (std::this_thread::get_id() != m_thread || !m_records.count(id) || !Validate()) return false;
    std::vector<std::string> started;
    if (StartRecursive(id, started)) { m_records.at(id)->root = true; m_records.at(id)->requested = false; return true; }
    Pump();
    for (auto it = started.rbegin(); it != started.rend(); ++it)
    {
        auto& r = *m_records.at(*it);
        if (!r.root) { r.requested = true; Pump(); }
    }
    return false;
}
bool ModuleManager::StartThrough(LoadingPhase phase)
{
    for (int p = 0; p <= static_cast<int>(phase); ++p)
        for (const auto& [id, r] : m_records)
            if (r->descriptor.phase == static_cast<LoadingPhase>(p) && r->state == ModuleState::Discovered && !Start(id)) return false;
    return true;
}
bool ModuleManager::HasUsers(const std::string& id) const
{
    for (const auto& [other, r] : m_records)
        if (r->scope && std::find(r->descriptor.dependencies.begin(), r->descriptor.dependencies.end(), id) != r->descriptor.dependencies.end()) return true;
    return false;
}
bool ModuleManager::RequestUnload(const std::string& id)
{
    if (std::this_thread::get_id() != m_thread) return false;
    auto it = m_records.find(id); if (it == m_records.end() || !it->second->scope) return false;
    it->second->requested = true; it->second->explicitUnload = true;
    it->second->deadline = std::chrono::steady_clock::now() + m_drainTimeout;
    return true;
}
void ModuleManager::Pump()
{
    if (std::this_thread::get_id() != m_thread) return;
    for (auto& [id, ptr] : m_records)
    {
        auto& r = *ptr;
        if (!r.requested || !r.scope || r.blocked) continue;
        if (r.explicitUnload && (!r.descriptor.supportsUnload || (HasUsers(id) && std::chrono::steady_clock::now() >= r.deadline)))
        {
            r.state = ModuleState::RestartRequired; r.requested = r.explicitUnload = false;
            m_error = "module lacks safe unload capability or has active dependents; restart required: " + id;
            continue;
        }
        if (HasUsers(id)) continue;
        if (r.state == ModuleState::Running || r.state == ModuleState::RestartRequired)
        {
            r.state = ModuleState::Quiescing; r.scope->Quiesce();
            r.deadline = std::chrono::steady_clock::now() + m_drainTimeout;
            if (r.instance) r.instance->RequestStop();
            r.state = ModuleState::Revoking; r.scope->Revoke(); r.state = ModuleState::Draining;
        }
        if (r.state == ModuleState::Draining && r.scope->IsDrained() && (!r.instance || r.instance->IsStopped())) r.state = ModuleState::Cleaning;
        if (r.state == ModuleState::Cleaning)
        {
            if (!r.stopped) { r.stopped = true; if (r.instance && !r.failed) r.instance->Shutdown(); }
            if (r.scope->Cleanup())
            {
                r.instance.reset(); r.scope.reset(); r.root = false; r.requested = false;
                r.state = r.failed ? ModuleState::Failed : ModuleState::SafeToUnload;
            }
        }
        if (r.scope && std::chrono::steady_clock::now() >= r.deadline)
        {
            r.blocked = true; r.state = ModuleState::RestartRequired;
            m_error = "module could not drain safely; restart required: " + id;
            if (r.instance) m_error += ": " + r.instance->UnloadDiagnostic();
            if (!r.scope->CleanupError().empty()) m_error += ": " + r.scope->CleanupError();
        }
    }
}
void ModuleManager::ShutdownAll()
{
    for (auto& [id, r] : m_records) if (r->scope && !r->blocked)
    {
        r->requested = true; r->scope->Quiesce();
        r->explicitUnload = false;
        if (r->instance) r->instance->RequestStop();
        r->scope->Revoke(); r->state = ModuleState::Draining;
        r->deadline = std::chrono::steady_clock::now() + m_drainTimeout;
    }
    for (size_t pass = 0; pass <= m_records.size(); ++pass) Pump();
    for (auto& [id, r] : m_records) if (r->scope) { r->state = ModuleState::RestartRequired; r->blocked = true; }
}
ModuleState ModuleManager::State(const std::string& id) const
{
    const auto it = m_records.find(id); return it == m_records.end() ? ModuleState::Failed : it->second->state;
}
std::vector<std::string> ModuleManager::Ids() const
{
    std::vector<std::string> ids; for (auto& [id, record] : m_records) ids.push_back(id); return ids;
}
bool ModuleManager::HasPinnedModules() const
{
    for (const auto& [id, r] : m_records) if (r->scope) return true; return false;
}
}
