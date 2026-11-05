#pragma once

#include "CoreDef.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace NLS::Core::Modules
{
enum class HostKind : uint32_t { Editor = 1, Game = 2, Build = 4 };
enum class ModuleType { Runtime, Editor, Developer, ThirdParty };
enum class LoadingPhase { PreEngine, PostEngine, PostEditor, PrePlay };
enum class ModuleState
{
    Discovered, Starting, Running, Quiescing, Revoking, Draining, Cleaning,
    SafeToUnload, Failed, RestartRequired
};

struct ModuleDescriptor
{
    std::string id;
    ModuleType type = ModuleType::Runtime;
    LoadingPhase phase = LoadingPhase::PostEngine;
    uint32_t hosts = 3;
    std::vector<std::string> dependencies;
    bool supportsUnload = false;
};

class ModuleScope;
class NLS_CORE_API ExecutionGuard
{
public:
    ExecutionGuard() = default;
    ExecutionGuard(ExecutionGuard&& other) noexcept;
    ExecutionGuard& operator=(ExecutionGuard&& other) noexcept;
    ~ExecutionGuard();
    explicit operator bool() const { return m_scope != nullptr; }
    ExecutionGuard(const ExecutionGuard&) = delete;
    ExecutionGuard& operator=(const ExecutionGuard&) = delete;
private:
    friend class ModuleScope;
    explicit ExecutionGuard(std::shared_ptr<ModuleScope> scope);
    std::shared_ptr<ModuleScope> m_scope;
};

// All host work has an explicit owner. A task must retain its ExecutionGuard
// until its final continuation completes; a cancellation request is not completion.
class NLS_CORE_API ModuleScope : public std::enable_shared_from_this<ModuleScope>
{
public:
    explicit ModuleScope(std::string id);
    const std::string& Id() const { return m_id; }
    ExecutionGuard Enter(bool startupWork = false);
    bool CanAcquire() const;
    bool AddLease();
    void ReleaseLease();
    void TrackRevocation(std::function<void()> action);
    void TrackCleanup(std::function<void()> action);
    void TrackAsync(std::function<void()> cancel, std::function<bool()> complete);
    void Commit();
    void Quiesce();
    void Revoke();
    bool IsDrained() const;
    bool Cleanup();
    const std::string& CleanupError() const { return m_cleanupError; }
private:
    friend class ExecutionGuard;
    void Leave();
    std::string m_id;
    mutable std::mutex m_mutex;
    bool m_accepting = true;
    bool m_committed = false;
    bool m_revoked = false;
    bool m_cleaned = false;
    uint64_t m_calls = 0;
    uint64_t m_leases = 0;
    std::vector<std::function<void()>> m_revocations;
    std::vector<std::function<void()>> m_cleanups;
    std::vector<std::pair<std::function<void()>, std::function<bool()>>> m_async;
    std::string m_cleanupError;
};

// The shared invocation vocabulary is deliberately small. Adapters map actual
// Editor and RuntimePlaySession events to these owned callbacks.
struct ExtensionCall
{
    uint32_t event = 0;
    double deltaSeconds = 0;
    uint64_t value = 0;
};
struct ExtensionDescriptor
{
    std::string id;
    std::string kind;
    int32_t priority = 0;
};
namespace Detail { struct ExtensionEntry; struct RegistryState; }

class NLS_CORE_API RegistrationToken
{
public:
    RegistrationToken() = default;
    RegistrationToken(RegistrationToken&&) noexcept;
    RegistrationToken& operator=(RegistrationToken&&) noexcept;
    ~RegistrationToken();
    void Reset();
    explicit operator bool() const { return m_key != 0; }
    RegistrationToken(const RegistrationToken&) = delete;
    RegistrationToken& operator=(const RegistrationToken&) = delete;
private:
    friend class ExtensionRegistry;
    std::weak_ptr<Detail::RegistryState> m_registry;
    uint64_t m_key = 0;
};

class NLS_CORE_API ExtensionLease
{
public:
    ExtensionLease() = default;
    ExtensionLease(ExtensionLease&&) noexcept;
    ExtensionLease& operator=(ExtensionLease&&) noexcept;
    ~ExtensionLease();
    bool Invoke(ExtensionCall& call) const;
    explicit operator bool() const { return m_entry != nullptr; }
    const ExtensionDescriptor* Descriptor() const;
    ExtensionLease(const ExtensionLease&) = delete;
    ExtensionLease& operator=(const ExtensionLease&) = delete;
private:
    friend class ExtensionRegistry;
    explicit ExtensionLease(std::shared_ptr<Detail::ExtensionEntry> entry);
    void Reset();
    std::shared_ptr<Detail::ExtensionEntry> m_entry;
};

class NLS_CORE_API ExtensionRegistry
{
public:
    ExtensionRegistry();
    RegistrationToken Register(const std::shared_ptr<ModuleScope>& owner,
        ExtensionDescriptor descriptor, std::function<void(ExtensionCall&)> callback);
    ExtensionLease Find(const std::string& id) const;
    std::vector<ExtensionLease> FindKind(const std::string& kind) const;
    size_t Count() const;
    uint64_t Revision() const;
private:
    std::shared_ptr<Detail::RegistryState> m_state;
};

struct ModuleContext
{
    std::shared_ptr<ModuleScope> scope;
    ExtensionRegistry& extensions;
    HostKind host;
    LoadingPhase phase;
};

class IModule
{
public:
    virtual ~IModule() = default;
    virtual bool Startup(ModuleContext&) = 0;
    virtual void RequestStop() noexcept {}
    virtual void AbortStartup() noexcept {}
    // Must report completion of private threads and independently executing
    // objects. Returning true for untracked live work violates the module ABI.
    virtual bool IsStopped() const noexcept = 0;
    virtual std::string UnloadDiagnostic() const { return {}; }
    virtual void Shutdown() noexcept = 0;
};

// Process-owned, never a plugin-local singleton. The host creates one manager
// and passes context explicitly to source modules and C ABI adapters.
class NLS_CORE_API ModuleManager
{
public:
    explicit ModuleManager(ExtensionRegistry& extensions, HostKind host);
    ~ModuleManager();
    bool Register(ModuleDescriptor descriptor, std::function<std::unique_ptr<IModule>()> factory);
    bool Validate();
    bool Start(const std::string& id);
    bool StartThrough(LoadingPhase phase);
    bool RequestUnload(const std::string& id);
    void Pump();
    void ShutdownAll();
    ModuleState State(const std::string& id) const;
    const std::string& LastError() const { return m_error; }
    std::vector<std::string> Ids() const;
    bool HasPinnedModules() const;
    void SetDrainTimeout(std::chrono::milliseconds timeout) { m_drainTimeout = timeout; }
private:
    struct Record;
    bool StartRecursive(const std::string& id, std::vector<std::string>& started);
    bool HasUsers(const std::string& id) const;
    bool CheckGraph(const std::string& id, std::vector<std::string>& stack,
        std::vector<std::string>& visited, bool runtimeOnly);
    void Fail(Record& record, const std::string& reason);
    ExtensionRegistry& m_extensions;
    HostKind m_host;
    std::thread::id m_thread;
    std::map<std::string, std::shared_ptr<Record>> m_records;
    std::string m_error;
    std::chrono::milliseconds m_drainTimeout{5000};
};
}
