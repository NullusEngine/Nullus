#pragma once
#include "System.h"

namespace NLS::ECS
{
class NLS_ECS_API SystemScheduler final
{
public:
    explicit SystemScheduler(World& world);
    ~SystemScheduler();
    SystemScheduler(const SystemScheduler&) = delete;
    SystemScheduler& operator=(const SystemScheduler&) = delete;
    System& RegisterSystem(SystemDescription description, std::unique_ptr<System> system);
    template<class T, class... Args> T& RegisterSystem(SystemDescription description, Args&&... args)
    {
        static_assert(std::derived_from<T, System>);
        return static_cast<T&>(RegisterSystem(std::move(description), std::make_unique<T>(std::forward<Args>(args)...)));
    }
    System* Find(std::string_view name) const;
    template<class T> T* Find() const
    { for (const auto& entry : m_systems) if (auto* result = dynamic_cast<T*>(entry->system.get())) return result; return nullptr; }
    bool Remove(std::string_view name);
    bool SetEnabled(std::string_view name, bool enabled);
    void Start();
    void Stop();
    bool IsRunning() const { return m_running; }
    void RunPhase(SystemPhase phase, const SystemTime& time);
    void ValidateDependencies();
    std::vector<SystemStatistics> GetStatistics() const;
    std::vector<SystemDependency> GetDependencies();
    std::vector<std::string> GetExecutionOrder(SystemPhase phase);
    const CommandPlaybackResult& GetLastPlayback() const { return m_lastPlayback; }

private:
    friend class World;
    void Shutdown();
    struct Entry
    {
        SystemDescription description;
        std::unique_ptr<System> system;
        bool started = false;
        uint64_t lastUpdateVersion = 0;
        SystemStatistics statistics;
    };
    void CheckEditable() const;
    void Build();
    void Update(size_t index, SystemPhase phase, const SystemTime& time, bool jobsEnabled);
    World& m_world;
    std::vector<std::unique_ptr<Entry>> m_systems;
    std::vector<std::vector<bool>> m_edges;
    std::array<std::vector<size_t>, static_cast<size_t>(SystemPhase::Count)> m_orders;
    std::vector<SystemDependency> m_dependencies;
    bool m_dirty = true, m_running = false, m_phaseRunning = false, m_lifecycleRunning = false, m_destroyed = false;
    EntityCommandBuffer m_commands;
    CommandPlaybackResult m_lastPlayback;
};
}
