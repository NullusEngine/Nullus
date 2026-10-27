#pragma once

#include <mutex>
#include <optional>
#include "CompiledSystem.h"
#include "CpuEmitter.h"
#include "ParticleRenderData.h"

namespace NLS::VFX
{
struct SystemRenderFrame
{
    std::vector<std::shared_ptr<const ParticleRenderData>> emitters;
};

// Immutable game-thread publication. Its per-device render cache never changes simulation inputs.
struct SystemFrame
{
    std::shared_ptr<const CompiledSystem> system;
    std::vector<std::shared_ptr<const ParticleDataSet>> cpu;
    std::vector<ParticleBounds> cpuBounds;
    // Event spawns destined for GPU emitters, indexed like system.emitters.
    std::vector<std::vector<GpuEventSpawn>> gpuEvents;
    std::unordered_map<std::string,Value> systemAttributes;
    std::unordered_map<std::string,Value> parameters;
    uint64_t sequence = 0, resetRevision = 0, clearRevision = 0;
    double deltaTime = 0, age = 0;
    double simulationAge = 0;
    std::vector<double> simulationSteps;
    bool spawning = true, paused = false;
    mutable std::mutex cacheMutex;
    mutable std::unordered_map<uint64_t,std::shared_ptr<const SystemRenderFrame>> renderCache;
};

class NLS_VFX_API SystemInstance final
{
public:
    bool Initialize(std::shared_ptr<const CompiledSystem> system, std::string& error);
    bool Tick(double deltaTime);
    // Initialize/Reset defer warmup until Tick so callers can bind parameters
    // and collision providers. Tick(0) runs warmup without advancing System age.
    void Reset();
    void Activate();
    void Deactivate(bool immediate = false);
    void SetPaused(bool paused);
    bool SetParameter(std::string name, Value value);
    bool SetParameters(const std::unordered_map<std::string,Value>& values);
    bool SetTypedParameter(std::string name, ValueType type, Value value);
    // Atomically replaces a fixed-size binding. Curve records use Float4
    // (time,value,0,0); array storage uses the declared element type.
    bool SetDataInterface(std::string_view name,ValueType storageType,std::span<const Value> values);
    void SetCollisionWorld(std::shared_ptr<Physics::PhysicsWorld> world);
    // Publishes the camera snapshot used by camera-relative modules. Callers
    // that never set it keep the default (invalid) context, and those modules
    // report a compile-time diagnostic instead of silently using stale data.
    void SetCameraContext(const CameraContext& camera) { m_camera = camera; }
    // Publishes the owning component transform. Emitters that never receive one
    // keep the declared neutral defaults.
    void SetSystemTransform(const Value& position, const Value& velocity)
    { m_systemPosition = position; m_systemVelocity = velocity; m_systemTransformValid = true; }
    [[nodiscard]] const Value& SystemPosition() const { return m_systemPosition; }
    [[nodiscard]] const Value& SystemVelocity() const { return m_systemVelocity; }
    [[nodiscard]] const CameraContext& Camera() const { return m_camera; }
    /** True when at least one emitter queries scene collision geometry. */
    [[nodiscard]] bool RequiresCollisionWorld() const;
    [[nodiscard]] std::shared_ptr<const SystemFrame> Capture() const { return m_frame; }
    [[nodiscard]] const std::vector<std::optional<CpuEmitter>>& CpuEmitters() const { return m_cpu; }
    [[nodiscard]] double Age() const { return m_age; }
    // True once the optional Complete If Unused policy has been satisfied.
    [[nodiscard]] bool IsComplete() const { return m_completed; }

private:
    // Spawns receivers from the events their source emitters published this tick.
    bool DispatchEvents();
    void Publish(double deltaTime,std::vector<double> steps = {});
    std::shared_ptr<const CompiledSystem> m_system;
    Program m_systemSpawnProgram, m_systemUpdateProgram;
    ParticleDataSet m_systemData;
    std::vector<std::optional<CpuEmitter>> m_cpu;
    std::unordered_map<std::string,Value> m_parameters;
    std::unordered_map<std::string,ValueType> m_parameterTypes;
    std::shared_ptr<const SystemFrame> m_frame;
    std::shared_ptr<Physics::PhysicsWorld> m_collisionWorld;
    CameraContext m_camera;
    // System-level transform published to emitters. SystemLocation reads the
    // position and InheritVelocity reads the velocity, mirroring how Niagara
    // exposes the owning component transform to its emitters.
    Value m_systemPosition{}, m_systemVelocity{};
    bool m_systemTransformValid = false;
    uint64_t m_sequence = 0, m_resetRevision = 1, m_clearRevision = 0;
    // Event-spawned particles draw from the same id space as scheduled ones so a
    // particle id stays unique within the emitter across both paths.
    uint64_t m_eventIdCursor = 0x8000000000000000ull;
    // Events consumed this System tick, indexed by receiver emitter and handler.
    std::vector<std::vector<uint32_t>> m_eventConsumed;
    // Event payloads accumulated for GPU receivers between publications.
    std::vector<std::vector<GpuEventSpawn>> m_gpuEventSpawns;
    double m_age = 0;
    double m_simulationAge = 0, m_fixedRemainder = 0;
    bool m_warmupPending = false;
    bool m_spawning = true, m_paused = false;
    bool m_completed = false;
    double m_unusedSeconds = 0;
};
}
