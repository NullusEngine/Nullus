#include "SystemScheduler.h"
#include "ECS/Jobs/JobBatch.h"
#include <Base/Jobs/JobSystem.h>
#include <algorithm>
#include <chrono>
#include <queue>

namespace NLS::ECS
{
namespace
{
struct FlagScope
{
    explicit FlagScope(bool& value) : flag(value) { flag = true; }
    ~FlagScope() { flag = false; }
    bool& flag;
};
size_t PhaseIndex(SystemPhase phase)
{
    const auto index = static_cast<size_t>(phase);
    if (index >= static_cast<size_t>(SystemPhase::Count)) throw std::invalid_argument("Invalid ECS system phase");
    return index;
}
}
const char* SystemPhaseName(SystemPhase phase)
{
    constexpr const char* names[]{"Initialization", "PreFixedUpdate", "FixedSimulation", "PostPhysics", "Update", "LateUpdate", "Presentation", "Cleanup"};
    return names[PhaseIndex(phase)];
}
SystemScheduler::SystemScheduler(World& world) : m_world(world), m_commands(world) {}
SystemScheduler::~SystemScheduler()
{
    Shutdown();
}
void SystemScheduler::Shutdown()
{
    if (m_destroyed) return;
    Stop();
    m_destroyed = true;
    FlagScope lifecycle(m_lifecycleRunning);
    for (auto iterator = m_systems.rbegin(); iterator != m_systems.rend(); ++iterator) (*iterator)->system->OnDestroy(m_world);
    // Callbacks can look up their peers while all objects still exist. During
    // subsequent C++ destructors the public collection is already empty.
    auto retired = std::move(m_systems);
    m_systems.clear();
}
void SystemScheduler::CheckEditable() const
{
    m_world.CheckStructural();
    if (m_destroyed || m_phaseRunning || m_lifecycleRunning) throw std::logic_error("ECS system registration changes require a lifecycle safe point");
}
System& SystemScheduler::RegisterSystem(SystemDescription description, std::unique_ptr<System> system)
{
    CheckEditable(); PhaseIndex(description.phase);
    if (!system || description.name.empty() || Find(description.name)) throw std::invalid_argument("ECS systems require a unique nonempty name and implementation");
    for (const auto& access : description.access)
    {
        const auto& type = m_world.GetRegistry().Get(access.type);
        if (access.write && !access.resource && !type.HasColumn()) throw std::invalid_argument("Systems cannot declare mutable shared/tag column access");
    }
    auto entry = std::make_unique<Entry>();
    entry->description = std::move(description); entry->system = std::move(system);
    m_systems.reserve(m_systems.size() + 1);
    {
        FlagScope lifecycle(m_lifecycleRunning);
        try
        {
            entry->system->OnCreate(m_world);
            if (m_running && entry->description.enabled) { entry->system->OnStart(m_world); entry->started = true; }
        }
        catch (...) { entry->system->OnDestroy(m_world); throw; }
    }
    auto& result = *entry->system;
    m_systems.push_back(std::move(entry)); m_dirty = true;
    return result;
}
System* SystemScheduler::Find(std::string_view name) const
{
    for (const auto& entry : m_systems) if (entry->description.name == name) return entry->system.get();
    return nullptr;
}
bool SystemScheduler::Remove(std::string_view name)
{
    CheckEditable();
    const auto iterator = std::find_if(m_systems.begin(), m_systems.end(), [&](const auto& entry) { return entry->description.name == name; });
    if (iterator == m_systems.end()) return false;
    FlagScope lifecycle(m_lifecycleRunning);
    if ((*iterator)->started) (*iterator)->system->OnStop(m_world);
    (*iterator)->system->OnDestroy(m_world);
    m_systems.erase(iterator); m_dirty = true; return true;
}
bool SystemScheduler::SetEnabled(std::string_view name, bool enabled)
{
    CheckEditable();
    for (auto& entry : m_systems)
        if (entry->description.name == name)
        {
            if (entry->description.enabled == enabled) return true;
            FlagScope lifecycle(m_lifecycleRunning);
            if (!enabled && entry->started) { entry->system->OnStop(m_world); entry->started = false; }
            if (enabled && m_running) { entry->system->OnStart(m_world); entry->started = true; entry->lastUpdateVersion = 0; }
            entry->description.enabled = enabled; m_dirty = true; return true;
        }
    return false;
}
void SystemScheduler::Build()
{
    if (!m_dirty) return;
    const auto count = m_systems.size();
    m_edges.assign(count, std::vector<bool>(count, false)); m_dependencies.clear();
    for (auto& order : m_orders) order.clear();
    std::map<std::string, size_t, std::less<>> names;
    for (size_t index = 0; index < count; ++index) names.emplace(m_systems[index]->description.name, index);
    const auto edge = [&](size_t from, size_t to, std::string reason, ComponentId component = 0, bool resource = false)
    {
        const auto& a = m_systems[from]->description; const auto& b = m_systems[to]->description;
        if (!a.enabled || !b.enabled) return;
        if (a.phase > b.phase) throw std::logic_error("ECS dependency contradicts phase order: " + a.name + " -> " + b.name);
        if (!m_edges[from][to])
        { m_edges[from][to] = true; m_dependencies.push_back({a.name, b.name, std::move(reason), component, resource}); }
    };
    for (size_t index = 0; index < count; ++index)
    {
        const auto& description = m_systems[index]->description;
        if (!description.enabled) continue;
        for (const auto& name : description.before)
        {
            const auto found = names.find(name);
            if (found == names.end()) throw std::invalid_argument("Unknown ECS Before system: " + name);
            edge(index, found->second, "Before");
        }
        for (const auto& name : description.after)
        {
            const auto found = names.find(name);
            if (found == names.end()) throw std::invalid_argument("Unknown ECS After system: " + name);
            edge(found->second, index, "After");
        }
    }
    for (size_t phase = 0; phase < m_orders.size(); ++phase)
    {
        std::vector<size_t> incoming(count, 0);
        size_t expected = 0;
        for (size_t to = 0; to < count; ++to)
            if (m_systems[to]->description.enabled && PhaseIndex(m_systems[to]->description.phase) == phase)
            {
                ++expected;
                for (size_t from = 0; from < count; ++from)
                    if (m_edges[from][to] && PhaseIndex(m_systems[from]->description.phase) == phase) ++incoming[to];
            }
        std::priority_queue<size_t, std::vector<size_t>, std::greater<>> ready;
        for (size_t index = 0; index < count; ++index)
            if (m_systems[index]->description.enabled && PhaseIndex(m_systems[index]->description.phase) == phase && !incoming[index]) ready.push(index);
        auto& order = m_orders[phase];
        while (!ready.empty())
        {
            const auto from = ready.top(); ready.pop(); order.push_back(from);
            for (size_t to = 0; to < count; ++to)
                if (m_edges[from][to] && PhaseIndex(m_systems[to]->description.phase) == phase && --incoming[to] == 0) ready.push(to);
        }
        if (order.size() != expected) throw std::logic_error("Cycle in ECS system dependencies for " + std::string(SystemPhaseName(static_cast<SystemPhase>(phase))));
        // Explicit topology wins. Orient otherwise unordered hazards along that
        // stable order, so a valid reverse explicit ordering creates no false cycle.
        for (size_t earlier = 0; earlier < order.size(); ++earlier)
            for (size_t later = earlier + 1; later < order.size(); ++later)
            {
                const auto from = order[earlier], to = order[later];
                for (const auto& a : m_systems[from]->description.access)
                    for (const auto& b : m_systems[to]->description.access)
                        if (a.type == b.type && a.resource == b.resource && (a.write || b.write))
                            edge(from, to, "Read/write conflict", a.type, a.resource);
            }
    }
    m_dirty = false;
}
void SystemScheduler::ValidateDependencies() { CheckEditable(); Build(); }
void SystemScheduler::Start()
{
    CheckEditable(); if (m_running) return;
    Build(); m_world.ClearCancellation();
    FlagScope lifecycle(m_lifecycleRunning);
    try
    {
        for (auto& entry : m_systems)
            if (entry->description.enabled) { entry->system->OnStart(m_world); entry->started = true; entry->lastUpdateVersion = 0; }
    }
    catch (...)
    {
        for (auto iterator = m_systems.rbegin(); iterator != m_systems.rend(); ++iterator)
            if ((*iterator)->started) { (*iterator)->system->OnStop(m_world); (*iterator)->started = false; }
        throw;
    }
    m_running = true;
}
void SystemScheduler::Stop()
{
    CheckEditable(); if (!m_running) return;
    FlagScope lifecycle(m_lifecycleRunning);
    for (auto iterator = m_systems.rbegin(); iterator != m_systems.rend(); ++iterator)
        if ((*iterator)->started) { (*iterator)->system->OnStop(m_world); (*iterator)->started = false; }
    m_running = false; m_commands.Reset();
}
void SystemScheduler::Update(size_t index, SystemPhase phase, const SystemTime& time, bool jobsEnabled)
{
    auto& entry = *m_systems[index];
    if (m_world.IsCancellationRequested()) throw std::runtime_error("ECS World execution cancelled");
    auto lease = m_world.AcquireAccess(entry.description.access, true);
    World::AccessScope scope(lease);
    SystemContext context{m_world, time, phase, m_commands.GetWriter(uint64_t(index) << 32), jobsEnabled, {m_world.GetId(), entry.lastUpdateVersion}};
    entry.statistics.lastRanOnWorker = std::this_thread::get_id() != m_world.m_owner;
    const auto started = m_world.GetConfig().enableStatistics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    try { entry.system->OnUpdate(context); entry.statistics.lastError.clear(); }
    catch (const std::exception& exception) { entry.statistics.lastError = exception.what(); throw; }
    entry.lastUpdateVersion = m_world.m_version.load(std::memory_order_acquire);
    if (m_world.GetConfig().enableStatistics)
    {
        ++entry.statistics.updates;
        entry.statistics.lastMilliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        entry.statistics.totalMilliseconds += entry.statistics.lastMilliseconds;
    }
}
void SystemScheduler::RunPhase(SystemPhase phase, const SystemTime& time)
{
    CheckEditable(); const auto phaseIndex = PhaseIndex(phase);
    const bool jobsEnabled = Base::Jobs::IsJobSystemInitialized() && !m_world.GetConfig().singleThreaded;
    if (!m_running) Start();
    Build(); m_commands.Reset();
    FlagScope execution(m_phaseRunning);
    auto freeze = m_world.AcquireAccess({}, true);
    try
    {
        const auto& order = m_orders[phaseIndex];
        if (!jobsEnabled)
        { for (const auto index : order) Update(index, phase, time, false); }
        else
        {
            // Execute dependency frontiers. Main-thread work runs on the caller
            // while independent pure data systems run on the existing pool.
            // One join per frontier bounds task overhead and command sync points.
            std::vector<bool> done(m_systems.size(), false);
            size_t completed = 0;
            while (completed < order.size())
            {
                std::vector<size_t> ready;
                for (const auto index : order)
                    if (!done[index])
                    {
                        bool blocked = false;
                        for (const auto predecessor : order) if (!done[predecessor] && m_edges[predecessor][index]) { blocked = true; break; }
                        if (!blocked) ready.push_back(index);
                    }
                if (ready.empty()) throw std::logic_error("ECS dependency graph has no runnable frontier");
                Detail::JobBatch jobs;
                std::exception_ptr error;
                try
                {
                    for (const auto index : ready)
                        if (!m_systems[index]->description.mainThread)
                        {
                            jobs.Add([&, index] { Update(index, phase, time, true); }, "ECS system");
                        }
                    for (const auto index : ready)
                        if (m_systems[index]->description.mainThread) Update(index, phase, time, true);
                }
                catch (...) { error = std::current_exception(); }
                const auto waiting = m_world.GetConfig().enableStatistics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                try { jobs.Wait(); } catch (...) { if (!error) error = std::current_exception(); }
                if (m_world.GetConfig().enableStatistics)
                {
                    m_world.m_jobs.fetch_add(jobs.GetJobCount());
                    m_world.m_jobWaitMilliseconds.fetch_add(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waiting).count());
                }
                if (error) std::rethrow_exception(error);
                for (const auto index : ready) { done[index] = true; ++completed; }
            }
        }
    }
    catch (...) { m_commands.Reset(); throw; }
    freeze.reset();
    m_lastPlayback = m_commands.Playback();
    if (!m_lastPlayback.Succeeded()) throw std::runtime_error("ECS command playback: " + m_lastPlayback.errors.front().message);
    if (phase == SystemPhase::Cleanup) m_world.TrimEmptyChunks();
}
std::vector<SystemStatistics> SystemScheduler::GetStatistics() const
{
    CheckEditable(); std::vector<SystemStatistics> result;
    for (const auto& entry : m_systems)
    {
        auto statistics = entry->statistics;
        const auto& description = entry->description;
        statistics.name = description.name; statistics.phase = description.phase;
        statistics.group = description.group.empty() ? SystemPhaseName(description.phase) : description.group;
        statistics.enabled = description.enabled; statistics.started = entry->started;
        statistics.mainThread = description.mainThread; statistics.access = description.access;
        result.push_back(std::move(statistics));
    }
    return result;
}
std::vector<SystemDependency> SystemScheduler::GetDependencies() { CheckEditable(); Build(); return m_dependencies; }
std::vector<std::string> SystemScheduler::GetExecutionOrder(SystemPhase phase)
{
    CheckEditable(); Build(); std::vector<std::string> result;
    for (const auto index : m_orders[PhaseIndex(phase)]) result.push_back(m_systems[index]->description.name);
    return result;
}
}
