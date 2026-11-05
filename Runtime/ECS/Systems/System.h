#pragma once
#include "ECS/Commands/EntityCommandBuffer.h"
#include <array>

namespace NLS::ECS
{
enum class SystemPhase : uint8_t
{
    Initialization, PreFixedUpdate, FixedSimulation, PostPhysics,
    Update, LateUpdate, Presentation, Cleanup, Count
};
NLS_ECS_API const char* SystemPhaseName(SystemPhase phase);

struct SystemTime
{
    double deltaTime = 0, unscaledDeltaTime = 0, time = 0, unscaledTime = 0;
    double fixedDeltaTime = 1.0 / 60.0, timeScale = 1;
    uint64_t frameIndex = 0, fixedFrameIndex = 0;
};
struct SystemDescription
{
    std::string name;
    SystemPhase phase = SystemPhase::Update;
    std::string group;
    std::vector<Access> access;
    std::vector<std::string> before, after;
    bool enabled = true;
    // Safe default for engine/VM/physics/UI adapters. Pure data systems opt in.
    bool mainThread = true;
    template<class T> SystemDescription& Reads(const World& world)
    { access.push_back({world.GetComponentType<T>().id, false, false}); return *this; }
    template<class T> SystemDescription& Writes(const World& world)
    { access.push_back({world.GetComponentType<T>().id, true, false}); return *this; }
    template<class T> SystemDescription& ReadsResource(const World& world)
    { access.push_back({world.GetComponentType<T>().id, false, true}); return *this; }
    template<class T> SystemDescription& WritesResource(const World& world)
    { access.push_back({world.GetComponentType<T>().id, true, true}); return *this; }
    SystemDescription& Uses(const QueryDescription& query)
    { access.insert(access.end(), query.access.begin(), query.access.end()); return *this; }
};
struct SystemContext
{
    World& world;
    const SystemTime& time;
    SystemPhase phase;
    EntityCommandBuffer::Writer commands;
    // The application owns Base::Jobs lifetime. False in serial mode or when
    // that shared service has not been initialized.
    bool jobsEnabled = false;
    // First run uses version zero. Advances only after a successful, joined
    // update; use this cursor for Changed filters inside a system access scope.
    ChangeToken lastUpdate;
};
class System
{
public:
    virtual ~System() = default;
    virtual void OnCreate(World&) {}
    virtual void OnStart(World&) {}
    virtual void OnUpdate(SystemContext&) = 0;
    virtual void OnStop(World&) noexcept {}
    virtual void OnDestroy(World&) noexcept {}
};
struct SystemStatistics
{
    std::string name, group;
    SystemPhase phase;
    bool enabled = false, started = false, mainThread = true;
    bool lastRanOnWorker = false;
    uint64_t updates = 0;
    double lastMilliseconds = 0, totalMilliseconds = 0;
    std::string lastError;
    std::vector<Access> access;
};
struct SystemDependency
{
    std::string from, to, reason;
    ComponentId component = 0;
    bool resource = false;
};
}
