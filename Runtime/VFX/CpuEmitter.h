#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "VfxProgram.h"
#include "VfxTypedBytecode.h"
#include "EmissionScheduler.h"
#include "ParticleBounds.h"
#include <Physics/PhysicsWorld.h>
#include "ParticleBounds.h"

namespace NLS::VFX
{
enum class ParticleEventKind : uint8_t { Spawn, Death, Collision };
/**
 * Scheduler that served the most recent particle fan-out.
 *
 * The general Jobs pool is the only supported parallel backend for CPU
 * emitters: it keeps particle work off the rendering TaskGraph and off the
 * named render/RHI lanes. Reporting the choice keeps the fallback to serial
 * assertable instead of inferring it from worker counts.
 */
enum class ParallelScheduler : uint8_t { Serial, Jobs };
struct ParticleEvent
{
    ParticleEventKind kind = ParticleEventKind::Spawn;
    uint64_t particleId = 0;
    Value position {};
    Physics::BodyHandle body;
    // Event payload published to the receiving Event Handler script. Position
    // is kept separately for the existing receiver path; velocity/normal feed
    // the per-event Emitter.Event* parameter set.
    Value velocity {};
    Value normal {};
    uint32_t sourceIndex = 0;
};

struct EmitterStatistics
{
    uint64_t spawned = 0;
    uint64_t died = 0;
    uint64_t dropped = 0;
    uint64_t simulationSteps = 0;
    uint64_t collisions = 0;
};

class NLS_VFX_API CpuEmitter
{
public:
    bool Initialize(const EmitterAsset& asset, Program spawn, Program update, std::string& error, std::span<const ParameterDefinition> parameters = {},
                    Program emitterUpdate = {}, Program emitterCollision = {}, std::span<const Program> emitterEvents = {},
                    std::span<const Program> emitterEventParticles = {}, Program emitterSpawn = {});
    bool Tick(double deltaTime);
    // Exact simulation ticks, shared with the System's fixed/warmup plan.
    bool TickSteps(std::span<const double> steps);
    // Runs one exact sub-step. The System drives cross-emitter event dispatch
    // between sub-steps, matching Niagara's per-tick event ordering.
    bool TickStep(double step);
    // Drops accumulated events. The System clears once per frame and appends
    // across sub-steps so receivers observe all events produced this frame.
    void ClearEvents(bool clear) { if (clear) m_events.clear(); }
    void Reset();
    void Activate() { m_emission.Activate(); }
    void Deactivate(bool immediate = false);
    void SetPaused(bool paused) { m_paused = paused; }
    // Publishes the camera snapshot camera-relative modules read. Setting it
    // also updates the reserved Camera.* parameters so the lowered programs pick
    // the values up through their normal parameter path.
    void SetCameraContext(const CameraContext& camera);
    [[nodiscard]] const CameraContext& Camera() const { return m_camera; }
    bool SetParameter(std::string name, Value value);
    // Deterministic diagnostics and A/B measurements can disable worker fan-out.
    void SetParallelExecutionEnabled(bool enabled) { m_parallelExecutionEnabled = enabled; }
    // Worker tasks used by the most recent advance, 1 when the serial path ran.
    [[nodiscard]] uint32_t LastParallelTaskCount() const { return m_lastParallelTaskCount; }
    [[nodiscard]] ParallelScheduler LastParallelScheduler() const { return m_lastParallelScheduler; }
    bool Seek(double age);
    void SetCollisionWorld(std::shared_ptr<Physics::PhysicsWorld> world) { m_collisionWorld = std::move(world); }
    [[nodiscard]] const ParticleDataSet& Data() const { return m_data; }
    [[nodiscard]] const EmitterAsset& Asset() const { return m_asset; }
    [[nodiscard]] const ParticleBounds& Bounds() const { return m_bounds; }
    [[nodiscard]] const EmitterStatistics& Statistics() const { return m_statistics; }
    [[nodiscard]] size_t EventHandlerCount() const { return m_eventHandlers.size(); }
    [[nodiscard]] const EmitterAsset::EventReceiver& EventReceiver(size_t handler) const;
    [[nodiscard]] const std::vector<ParticleEvent>& Events() const { return m_events; }
    // Event handler reception: spawn particles at the positions of events that
    // another emitter published this frame. Each handler has its own receiver
    // policy and per-event script. Returns false only on a failed spawn program;
    // an empty event list is a no-op.
    bool ReceiveEvents(size_t handler, std::span<const ParticleEvent> events, uint32_t spawnCount, float randomAge, uint64_t& nextId);
    [[nodiscard]] double Age() const { return m_emission.Age(); }
    [[nodiscard]] bool IsComplete() const { return !m_emission.IsSpawning() && m_data.Count() == 0; }

private:
    enum class ParallelAdvanceOutcome
    {
        Completed,
        Unavailable,
        Failed
    };
    struct ParallelRangeState
    {
        std::atomic_bool failed { false };
    };
    bool Step(double deltaTime);
    bool ExecuteTyped(const Typed::BytecodeProgram& program,uint32_t first,uint32_t count,
        float deltaTime,uint32_t spawnIndex,std::span<const int32_t> bindings);
    void Spawn(const SpawnBatch& batch);
    bool AdvanceParticles(uint32_t first, uint32_t count, float deltaTime);
    bool AdvanceParticleRange(uint32_t first, uint32_t count, float deltaTime,
                              ParallelRangeState* parallel = nullptr);
    ParallelAdvanceOutcome AdvanceParticlesParallel(uint32_t count, float deltaTime, uint32_t taskCount);
    void AdvanceCollisions(uint32_t first, uint32_t count);
    void RemoveDead();

    EmitterAsset m_asset;
    Program m_spawn;
    Program m_update;
    Typed::TypeRegistry m_typedTypes;
    std::vector<int32_t> m_spawnBindings, m_updateBindings;
    EmitterScript m_emitterScript;
    struct EventHandlerRuntime
    {
        EmitterScript script;
        EmitterAsset::EventReceiver receiver;
    };
    std::vector<EventHandlerRuntime> m_eventHandlers;
    std::vector<Program> m_eventParticlePrograms;
    std::vector<std::vector<int32_t>> m_eventParticleBindings;
    EmitterScript m_collisionScript;
    uint64_t m_eventGenerationMask = 0;
    bool m_collisionEnabled = false;
    float m_collisionRadius = 0, m_collisionRestitution = 0.5f, m_collisionFriction = 0;
    ParticleDataSet m_data;
    ParticleBounds m_bounds;
    std::unordered_map<std::string,Value> m_parameters;
    std::unordered_map<std::string,ValueType> m_parameterTypes;
    std::vector<ParticleEvent> m_events;
    std::vector<uint8_t> m_alive;
    std::vector<Value> m_previousPositions;
    std::shared_ptr<Physics::PhysicsWorld> m_collisionWorld;
    EmitterStatistics m_statistics;
    EmissionScheduler m_emission;
    uint32_t m_spawnPerFrame = 0;
    double m_spawnPerUnit = 0, m_spawnPerUnitRemainder = 0;
    CameraContext m_camera;
    int32_t m_ageAttribute = -1;
    int32_t m_lifetimeAttribute = -1;
    int32_t m_positionAttribute = -1;
    int32_t m_velocityAttribute = -1;
    int32_t m_eventPositionAttribute = -1, m_eventVelocityAttribute = -1, m_eventNormalAttribute = -1;
    int32_t m_eventKindAttribute = -1, m_eventParticleIdAttribute = -1, m_eventSourceIndexAttribute = -1;
    // Optional collision result columns. They only exist when a Collision
    // module declared them, so emitters without collision keep their previous
    // attribute layout and program identity.
    int32_t m_collisionNormalAttribute = -1;
    int32_t m_collisionStateAttribute = -1;
    bool m_initialized = false;
    bool m_paused = false;
    bool m_parallelExecutionEnabled = true;
    uint32_t m_lastParallelTaskCount = 1u;
    ParallelScheduler m_lastParallelScheduler = ParallelScheduler::Serial;
};
}
