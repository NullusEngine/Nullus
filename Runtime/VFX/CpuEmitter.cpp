#include "CpuEmitter.h"
#include "VfxParameterValidation.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <stdexcept>

#include <Jobs/JobSystem.h>

namespace NLS::VFX
{
namespace
{
constexpr uint32_t kMinimumParticlesPerParallelTask = 2048u;
constexpr uint32_t kMaximumParallelParticleTasks = 32u;

bool ParallelCpuUpdateEnvironmentEnabled()
{
    static const bool enabled = []
    {
        const char* const configured = std::getenv("NLS_VFX_CPU_PARALLEL");
        if (configured == nullptr || *configured == '\0') return true;
        const std::string text { configured };
        return !(text == "0" || text == "false" || text == "FALSE" || text == "off" || text == "OFF");
    }();
    return enabled;
}

uint32_t ResolveParallelTaskCount(const uint32_t count, const uint32_t laneCount)
{
    if (count < kMinimumParticlesPerParallelTask || laneCount < 2u) return 1u;
    const auto byParticles = count / kMinimumParticlesPerParallelTask;
    return (std::min)((std::min)(laneCount,kMaximumParallelParticleTasks),(std::max)(2u,byParticles));
}
}
bool CpuEmitter::Initialize(const EmitterAsset& asset, Program spawn, Program update, std::string& error, std::span<const ParameterDefinition> parameters,
                            Program emitterUpdate, Program emitterCollision, std::span<const Program> emitterEvents, std::span<const Program> emitterEventParticles, Program emitterSpawn)
{
    bool eventStagesValid = emitterEvents.size() == asset.eventHandlers.size() &&
        emitterEventParticles.size() == asset.eventHandlers.size();
    for (const auto& event : emitterEvents)
        eventStagesValid &= event.instructions.empty() || event.stage == ScriptStage::Event;
    for (const auto& event : emitterEventParticles)
        eventStagesValid &= event.instructions.empty() || event.stage == ScriptStage::Event;
    if (asset.target != SimulationTarget::CPU || !asset.simulationStages.empty() || asset.capacity == 0 || asset.capacity > 10000000 || asset.initialCapacity > asset.capacity || !std::isfinite(asset.spawnRate) || asset.spawnRate < 0 || asset.spawnRate > 10000000 || !std::isfinite(asset.duration) || asset.duration < 0.0001 || !std::isfinite(asset.collisionRadius) || asset.collisionRadius < 0 || !std::isfinite(asset.restitution) || asset.restitution < 0 || asset.restitution > 1 || !std::isfinite(asset.friction) || asset.friction < 0 || asset.friction > 1 || spawn.stage != ScriptStage::ParticleSpawn || update.stage != ScriptStage::ParticleUpdate || (!emitterUpdate.instructions.empty() && emitterUpdate.stage != ScriptStage::EmitterUpdate) || (!emitterSpawn.instructions.empty() && emitterSpawn.stage != ScriptStage::EmitterSpawn) || (!emitterCollision.instructions.empty() && emitterCollision.stage != ScriptStage::ParticleUpdate) || !eventStagesValid)
    { error = "Invalid CPU emitter configuration or program stage"; return false; }
    ParticleDataSet candidate;
    try { candidate.Initialize(asset.attributes,asset.capacity,asset.initialCapacity); }
    catch (const std::invalid_argument& exception) { error = exception.what(); return false; }
    const auto age = candidate.FindAttribute("Particles.Age"), lifetime = candidate.FindAttribute("Particles.Lifetime");
    if (age < 0 || lifetime < 0 || asset.attributes[age].type != ValueType::Float || asset.attributes[lifetime].type != ValueType::Float)
    { error = "CPU emitter requires scalar Age and Lifetime attributes"; return false; }
    EmissionScheduler emission;
    if (!emission.Initialize(asset,error)) return false;
    Detail::ParameterTypes types;
    if (!Detail::AddParameterTypes(types, parameters, error) || !Detail::AddParameterTypes(types, spawn, error) || !Detail::AddParameterTypes(types, update, error) || !Detail::AddParameterTypes(types, emitterUpdate, error) || !Detail::AddParameterTypes(types, emitterCollision, error) || !Detail::AddParameterTypes(types, emitterSpawn, error))
        return false;
    for (const auto& event : emitterEvents)
        if (!Detail::AddParameterTypes(types,event,error)) return false;
    for (const auto& event : emitterEventParticles)
        if (!Detail::AddParameterTypes(types,event,error)) return false;
    m_parameterTypes = std::move(types); m_parameters.clear();
    for (const auto& parameter : parameters) m_parameters[parameter.name] = parameter.value;
    if (!m_emitterScript.Initialize(asset.emitterAttributes, emitterUpdate, emitterSpawn))
    { error = "CPU emitter script could not be initialized"; return false; }
    m_eventHandlers.clear();
    m_eventHandlers.reserve(asset.eventHandlers.size());
    for (size_t index = 0; index < asset.eventHandlers.size(); ++index)
    {
        EventHandlerRuntime runtime;
        runtime.receiver = asset.eventHandlers[index].receiver;
        if (!runtime.script.Initialize(asset.emitterAttributes,emitterEvents[index]))
        { error = "CPU event script could not be initialized"; return false; }
        m_eventHandlers.push_back(std::move(runtime));
    }
    m_eventParticlePrograms.assign(emitterEventParticles.begin(),emitterEventParticles.end());
    if (!m_collisionScript.Initialize(asset.emitterAttributes,emitterCollision))
    { error = "CPU collision script could not be initialized"; return false; }
    m_eventGenerationMask = asset.eventGenerationMask;
    m_collisionEnabled = asset.collisionEnabled;
    m_collisionRadius = asset.collisionRadius;
    m_collisionRestitution = asset.restitution;
    m_collisionFriction = asset.friction;
    m_asset = asset;
    m_spawn = std::move(spawn);
    m_update = std::move(update);
    m_data = std::move(candidate);
    m_emission = std::move(emission);
    if (!Typed::ValidateBytecodeProgram(m_spawn, m_typedTypes, error) || !Typed::ValidateBytecodeProgram(m_update, m_typedTypes, error) || !Typed::BindBytecodeAttributes(m_spawn, m_data, m_spawnBindings, error) || !Typed::BindBytecodeAttributes(m_update, m_data, m_updateBindings, error))
        return false;
    m_eventParticleBindings.assign(m_eventParticlePrograms.size(), {});
    for (size_t index = 0; index < m_eventParticlePrograms.size(); ++index)
        if (!Typed::ValidateBytecodeProgram(m_eventParticlePrograms[index], m_typedTypes, error) || !Typed::BindBytecodeAttributes(m_eventParticlePrograms[index], m_data, m_eventParticleBindings[index], error))
            return false;

    m_ageAttribute = age;
    m_lifetimeAttribute = lifetime;
    m_positionAttribute = m_data.FindAttribute("Particles.Position");
    m_collisionNormalAttribute = m_data.FindAttribute("Particles.CollisionNormal");
    m_velocityAttribute = m_data.FindAttribute("Particles.Velocity");
    m_eventPositionAttribute = m_data.FindAttribute("Particles.EventPosition");
    m_eventVelocityAttribute = m_data.FindAttribute("Particles.EventVelocity");
    m_eventNormalAttribute = m_data.FindAttribute("Particles.EventNormal");
    m_eventKindAttribute = m_data.FindAttribute("Particles.EventKind");
    m_eventParticleIdAttribute = m_data.FindAttribute("Particles.EventParticleId");
    m_eventSourceIndexAttribute = m_data.FindAttribute("Particles.EventSourceIndex");
    m_collisionStateAttribute = m_data.FindAttribute("Particles.Colliding");
    m_spawnPerFrame = asset.spawnPerFrame;
    m_spawnPerUnit = asset.spawnPerUnit;
    m_initialized = true;
    Reset();
    error.clear();
    return true;
}

void CpuEmitter::Reset()
{
    m_data.Clear();
    m_emission.Reset();
    (void)m_emitterScript.Reset({0, m_asset.seed, 0, &m_parameters});
    m_eventGenerationMask = m_asset.eventGenerationMask;
    for (size_t index = 0; index < m_eventHandlers.size(); ++index)
    {
        m_eventHandlers[index].receiver = m_asset.eventHandlers[index].receiver;
        (void)m_eventHandlers[index].script.Reset();
    }
    m_collisionEnabled = m_asset.collisionEnabled;
    m_collisionRadius = m_asset.collisionRadius;
    m_collisionRestitution = m_asset.restitution;
    m_collisionFriction = m_asset.friction;
    (void)m_collisionScript.Reset();
    m_events.clear();
    m_statistics = {};
    m_bounds = {};
}

void CpuEmitter::Deactivate(bool immediate)
{
    m_emission.Deactivate();
    if (immediate) { m_data.Clear(); m_events.clear(); m_bounds = {}; }
}

void CpuEmitter::SetCameraContext(const CameraContext& camera)
{
    m_camera = camera;
    if (!camera.valid) return;
    // Feed the reserved parameters the lowered programs reference. Values are
    // plain data, so a program without a Camera.* parameter simply ignores them.
    (void)SetParameter("Camera.Position",camera.position);
    (void)SetParameter("Camera.Forward",camera.forward);
    (void)SetParameter("Camera.Right",camera.right);
    (void)SetParameter("Camera.Up",camera.up);
    (void)SetParameter("Camera.FovDegrees",{camera.fovDegrees,0,0,0});
    (void)SetParameter("Camera.ViewportWidth",{camera.viewportWidth,0,0,0});
    (void)SetParameter("Camera.ViewportHeight",{camera.viewportHeight,0,0,0});
}

bool CpuEmitter::SetParameter(std::string name, Value value)
{
    if (!Detail::ValidParameter(m_parameterTypes,name,value)) return false;
    m_parameters.insert_or_assign(std::move(name),value);
    return true;
}

bool CpuEmitter::Tick(double deltaTime)
{
    if (!std::isfinite(deltaTime) || deltaTime < 0 || deltaTime > 1) return false;
    const auto count = (std::max)(1u,static_cast<uint32_t>(std::ceil(deltaTime*60.0-1e-12)));
    std::array<double,60> steps; std::fill_n(steps.begin(),count,deltaTime/count);
    return TickSteps(std::span(steps.data(),deltaTime > 0 ? count : 0));
}

bool CpuEmitter::TickSteps(std::span<const double> steps)
{
    if (!m_initialized || steps.size() > kMaxSystemWarmupTicks+kMaxSystemFixedTicks ||
        (m_collisionEnabled && !m_collisionWorld)) return false;
    for (const auto step : steps) if (!std::isfinite(step) || step <= 0 || step > 1) return false;
    m_events.clear();
    if (m_paused || steps.empty()) return true;
    for (const auto step : steps) if (!Step(step)) return false;
    m_bounds = CalculateParticleBounds(m_data);
    return true;
}

bool CpuEmitter::TickStep(const double step)
{
    if (!m_initialized || !std::isfinite(step) || step <= 0 || step > 1) return false;
    if (m_collisionEnabled && !m_collisionWorld) return false;
    if (m_paused) return true;
    if (!Step(step)) return false;
    m_bounds = CalculateParticleBounds(m_data);
    return true;
}

bool CpuEmitter::AdvanceParticles(uint32_t first, uint32_t count, float deltaTime)
{
    m_lastParallelTaskCount = 1u;
    m_lastParallelScheduler = ParallelScheduler::Serial;
    if (!count || deltaTime == 0) return true;
    if (m_collisionEnabled && m_positionAttribute >= 0) m_previousPositions.resize(m_data.Count());
    if (m_parallelExecutionEnabled && ParallelCpuUpdateEnvironmentEnabled() && first == 0 && count == m_data.Count())
    {
        uint32_t lanes = 1u;
        // Only the general Jobs pool may serve CPU particle fan-out: reusing the
        // rendering TaskGraph would put simulation work on the render lanes.
        if (NLS::Base::Jobs::IsJobSystemInitialized())
            lanes = (std::max)(1u,NLS::Base::Jobs::GetJobWorkerCount()+1u);
        const auto tasks = ResolveParallelTaskCount(count,lanes);
        const auto outcome = tasks > 1u ? AdvanceParticlesParallel(count,deltaTime,tasks) : ParallelAdvanceOutcome::Unavailable;
        if (outcome == ParallelAdvanceOutcome::Completed)
        {
            AdvanceCollisions(0u,count);
            return true;
        }
        if (outcome == ParallelAdvanceOutcome::Failed) return false;
    }
    if (!AdvanceParticleRange(first, count, deltaTime))
        return false;
    AdvanceCollisions(first,count);
    return true;
}

bool CpuEmitter::ExecuteTyped(const Typed::BytecodeProgram& program,uint32_t first,uint32_t count,
    float deltaTime,uint32_t spawnIndex,std::span<const int32_t> bindings)
{
    Typed::BytecodeExecutionContext context;
    context.types = &m_typedTypes;
    context.boundParameters = &m_parameters;
    context.attributeBindings.assign(bindings.begin(),bindings.end());
    context.deltaTime = deltaTime;
    context.seed = m_asset.seed;
    context.tick = m_statistics.simulationSteps;
    context.spawnIndex = spawnIndex;
    std::string error;
    return Typed::ExecuteBytecodeParticles(program,m_data,first,count,context,error);
}

bool CpuEmitter::AdvanceParticleRange(uint32_t first, uint32_t count, float deltaTime,
                                      ParallelRangeState* parallel)
{
    if (!count || deltaTime == 0) return true;
    if (m_collisionEnabled && m_positionAttribute >= 0)
    {
        for (uint32_t particle = 0; particle < count; ++particle)
            m_previousPositions[first+particle] = m_data.Read(m_positionAttribute,first+particle);
    }
    for (uint32_t particle = first; particle < first + count; ++particle)
    {
        auto age = m_data.Read(m_ageAttribute,particle);
        age[0] += deltaTime;
        m_data.Write(m_ageAttribute,particle,age);
    }
    const auto executed = ExecuteTyped(m_update, first, count, deltaTime, 0, m_updateBindings);
    if (!executed)
    {
        if (parallel) parallel->failed.store(true,std::memory_order_release);
        return false;
    }
    return true;
}

CpuEmitter::ParallelAdvanceOutcome CpuEmitter::AdvanceParticlesParallel(uint32_t count, float deltaTime, uint32_t taskCount)
{
    ParallelRangeState state;
    const auto rangeSize = (count+taskCount-1u)/taskCount;
    struct RangeJob final : NLS::Base::Jobs::IJobParallelFor
    {
        CpuEmitter* emitter = nullptr;
        uint32_t count = 0u, rangeSize = 0u;
        float deltaTime = 0.0f;
        ParallelRangeState* state = nullptr;
        void Execute(uint32_t taskIndex)
        {
            const auto begin = taskIndex*rangeSize;
            if (begin >= count) return;
            const auto end = (std::min)(count,begin+rangeSize);
            (void)emitter->AdvanceParticleRange(begin, end - begin, deltaTime, state);
        }
    };
    if (!NLS::Base::Jobs::IsJobSystemInitialized() || NLS::Base::Jobs::GetJobWorkerCount() == 0u)
        return ParallelAdvanceOutcome::Unavailable;

    RangeJob job;
    job.emitter = this; job.count = count; job.rangeSize = rangeSize; job.deltaTime = deltaTime; job.state = &state;
    NLS::Base::Jobs::JobParallelForScheduleOptions options;
    options.batchSize = 1u; options.debugName = "VfxCpuParticleUpdate";
    auto handle = NLS::Base::Jobs::ScheduleParallelFor(job,taskCount,options);
    if (handle.id == 0u)
        return ParallelAdvanceOutcome::Unavailable;

    NLS::Base::Jobs::Complete(handle);
    m_lastParallelScheduler = ParallelScheduler::Jobs;
    m_lastParallelTaskCount = taskCount;
    return state.failed.load(std::memory_order_acquire) ? ParallelAdvanceOutcome::Failed : ParallelAdvanceOutcome::Completed;
}

void CpuEmitter::AdvanceCollisions(uint32_t first, uint32_t count)
{
    if (m_collisionEnabled && m_collisionWorld && m_positionAttribute >= 0)
    {
        Physics::QueryFilter filter;
        filter.layers = m_asset.collisionLayers;
        const auto world = m_collisionWorld->Read(filter);
        const auto velocityAttribute = m_data.FindAttribute("Particles.Velocity");
        for (uint32_t offset = 0; offset < count; ++offset)
        {
            const auto particle = first+offset;
            auto position = m_data.Read(m_positionAttribute,particle);
            const auto previous = m_previousPositions[first+offset];
            const Maths::Vector3 start{previous[0],previous[1],previous[2]};
            const Maths::Vector3 displacement{position[0]-previous[0],position[1]-previous[1],position[2]-previous[2]};
            const auto distance = displacement.Length();
            if (!std::isfinite(distance) || distance < 1e-7f) continue;
            Physics::QueryHit hit;
            const auto direction = displacement/distance;
            const bool collided = m_collisionRadius > 0
                ? world->SweepSphere(m_collisionRadius,start,direction,distance,hit)
                : world->Raycast(start,direction,distance,hit);
            if (!collided)
            {
                // Clear the previous frame's result so a stale normal cannot be
                // mistaken for a current contact.
                if (m_collisionNormalAttribute >= 0) m_data.Write(m_collisionNormalAttribute,particle,Value{});
                if (m_collisionStateAttribute >= 0) m_data.Write(m_collisionStateAttribute,particle,Value{});
                continue;
            }
            const auto center = start+direction*hit.distance+hit.normal*0.0001f;
            position[0] = center.x; position[1] = center.y; position[2] = center.z;
            m_data.Write(m_positionAttribute,particle,position);
            if (m_collisionNormalAttribute >= 0)
                m_data.Write(m_collisionNormalAttribute,particle,
                    Value{hit.normal.x,hit.normal.y,hit.normal.z,0});
            if (m_collisionStateAttribute >= 0)
                m_data.Write(m_collisionStateAttribute,particle,Value{1,0,0,0});
            if (velocityAttribute >= 0)
            {
                auto velocity = m_data.Read(velocityAttribute,particle);
                Maths::Vector3 value{velocity[0],velocity[1],velocity[2]};
                const auto normalVelocity = value.x*hit.normal.x+value.y*hit.normal.y+value.z*hit.normal.z;
                if (normalVelocity < 0)
                {
                    value = (value-hit.normal*normalVelocity)*(1-m_collisionFriction)-hit.normal*normalVelocity*m_collisionRestitution;
                    velocity[0] = value.x; velocity[1] = value.y; velocity[2] = value.z;
                    m_data.Write(velocityAttribute,particle,velocity);
                }
            }
            ++m_statistics.collisions;
            if ((m_eventGenerationMask & EventBit(ParticleEventSelection::Collision)) != 0)
            {
                ParticleEvent event{ParticleEventKind::Collision,m_data.Id(particle),position,hit.body};
                if (m_velocityAttribute >= 0) event.velocity = m_data.Read(m_velocityAttribute,particle);
                event.normal = {hit.normal.x,hit.normal.y,hit.normal.z,0};
                m_events.push_back(std::move(event));
            }
        }
    }
}

void CpuEmitter::Spawn(const SpawnBatch& batch)
{
    const auto available = static_cast<uint64_t>(m_asset.capacity - m_data.Count());
    const auto accepted = (std::min)(static_cast<uint64_t>(batch.count),available);
    const auto first = m_data.Count();
    const auto required = first+static_cast<uint32_t>(accepted);
    if (required > m_data.AllocatedCapacity() && !m_data.Reserve((std::min)(m_asset.capacity,(std::max)(required,m_data.AllocatedCapacity()*2))))
    { m_statistics.dropped += batch.count; return; }
    for (uint64_t index = 0; index < accepted; ++index)
    {
        const auto id = batch.firstId + index;
        (void)m_data.Spawn(id);
        const auto particle = first + static_cast<uint32_t>(index);
        const auto spawned = ExecuteTyped(m_spawn, particle, 1, 0, static_cast<uint32_t>(index), m_spawnBindings);
        if (!spawned) { m_statistics.dropped += batch.count-accepted; return; }
        if ((m_eventGenerationMask & EventBit(ParticleEventSelection::Spawn)) != 0)
        {
            ParticleEvent event{ParticleEventKind::Spawn,id,m_positionAttribute >= 0 ? m_data.Read(m_positionAttribute,particle) : Value{}};
            if (m_velocityAttribute >= 0) event.velocity = m_data.Read(m_velocityAttribute,particle);
            m_events.push_back(std::move(event));
        }
        if (m_asset.interpolatedSpawning && batch.ageStep > 0)
            if (!AdvanceParticles(particle,1,(std::max)(0.0f,batch.initialAge-static_cast<float>(index)*batch.ageStep))) { m_statistics.dropped += batch.count-accepted; return; }
    }
    m_statistics.spawned += accepted;
    m_statistics.dropped += batch.count - accepted;
    if (m_asset.interpolatedSpawning && batch.ageStep == 0)
        if (!AdvanceParticles(first,static_cast<uint32_t>(accepted),batch.initialAge)) { m_statistics.dropped += batch.count-accepted; return; }
}

const EmitterAsset::EventReceiver& CpuEmitter::EventReceiver(size_t handler) const
{
    return m_eventHandlers.at(handler).receiver;
}

bool CpuEmitter::ReceiveEvents(size_t handler, std::span<const ParticleEvent> events, const uint32_t spawnCount, const float randomAge, uint64_t& nextId)
{
    if (!m_initialized || events.empty() || !std::isfinite(randomAge) || randomAge < 0) return true;
    auto& eventScript = m_eventHandlers.at(handler).script;
    const auto eventKind = [](ParticleEventKind kind)
    {
        switch (kind)
        {
        case ParticleEventKind::Spawn: return 1.0f;
        case ParticleEventKind::Death: return 2.0f;
        case ParticleEventKind::Collision: return 3.0f;
        }
        return 0.0f;
    };
    // A receiving Event Handler runs once per published event, with the event
    // payload exposed as Emitter.Event* parameters. The script may override the
    // receiver's configured spawn count and random age for that event.
    const auto makeEventParameters = [&](const ParticleEvent& event)
    {
        auto parameters = m_parameters;
        parameters["Emitter.EventKind"] = {eventKind(event.kind),0,0,0};
        parameters["Emitter.EventPosition"] = event.position;
        parameters["Emitter.EventVelocity"] = event.velocity;
        parameters["Emitter.EventNormal"] = event.normal;
        parameters["Emitter.EventParticleId"] = {static_cast<float>(event.particleId),0,0,0};
        parameters["Emitter.EventSourceIndex"] = {static_cast<float>(event.sourceIndex),0,0,0};
        parameters["Emitter.Age"] = {static_cast<float>(m_emission.Age()),0,0,0};
        return parameters;
    };
    const auto executeParticleProgram = [&](const ParticleEvent& event,uint32_t particle) -> bool
    {
        const auto& program = m_eventParticlePrograms.at(handler);
        if (program.instructions.empty()) return true;
        auto parameters = makeEventParameters(event);
        ExecutionContext context;
        context.seed = m_asset.seed;
        context.tick = event.particleId;
        context.parameters = &parameters;
        std::string error;
        return Typed::ExecuteParticleProgram(program, m_data, particle, 1, context, error, m_eventParticleBindings[handler]);
    };
    const auto evaluateHandler = [&](const ParticleEvent& event, uint32_t& count, float& age, Value& position, Value& velocity) -> bool
    {
        if (!eventScript.HasUpdate()) return true;
        auto parameters = makeEventParameters(event);
        ExecutionContext context;
        context.seed = m_asset.seed;
        context.tick = event.particleId;
        context.parameters = &parameters;
        if (!eventScript.Tick(context)) return false;
        if (eventScript.HasWrite("Emitter.ReceiveSpawnCount"))
        {
            const auto value = eventScript.Read("Emitter.ReceiveSpawnCount",static_cast<float>(count));
            if (!std::isfinite(value)) return false;
            count = static_cast<uint32_t>(std::llround(std::clamp(value,0.0f,100000.0f)));
        }
        if (eventScript.HasWrite("Emitter.ReceiveRandomAge"))
        {
            const auto value = eventScript.Read("Emitter.ReceiveRandomAge",age);
            if (std::isfinite(value) && value >= 0 && value <= 3600) age = value;
        }
        if (eventScript.HasWrite("Emitter.ReceivePosition"))
            position = eventScript.ReadValue("Emitter.ReceivePosition",position);
        if (eventScript.HasWrite("Emitter.ReceiveVelocity"))
            velocity = eventScript.ReadValue("Emitter.ReceiveVelocity",velocity);
        return true;
    };
    const auto applyEventPayload = [&](uint32_t particle,const ParticleEvent& event,const Value& position,const Value& velocity)
    {
        if (m_eventPositionAttribute >= 0) m_data.Write(m_eventPositionAttribute,particle,position);
        if (m_eventVelocityAttribute >= 0) m_data.Write(m_eventVelocityAttribute,particle,velocity);
        if (m_eventNormalAttribute >= 0) m_data.Write(m_eventNormalAttribute,particle,event.normal);
        if (m_eventKindAttribute >= 0) m_data.Write(m_eventKindAttribute,particle,{eventKind(event.kind),0,0,0});
        if (m_eventParticleIdAttribute >= 0)
            m_data.Write(m_eventParticleIdAttribute,particle,{static_cast<float>(event.particleId),0,0,0});
        if (m_eventSourceIndexAttribute >= 0)
            m_data.Write(m_eventSourceIndexAttribute,particle,{static_cast<float>(event.sourceIndex),0,0,0});
    };
    const auto mode = m_eventHandlers.at(handler).receiver.executionMode;
    for (const auto& event : events)
    {
        uint32_t eventSpawnCount = spawnCount;
        float eventRandomAge = randomAge;
        Value spawnPosition = event.position;
        Value spawnVelocity = event.velocity;
        if (!evaluateHandler(event,eventSpawnCount,eventRandomAge,spawnPosition,spawnVelocity)) return false;
        if (mode != EventExecutionMode::SpawnedParticles)
        {
            const auto count = m_data.Count();
            if (count == 0u) continue;
            if (mode == EventExecutionMode::EveryParticle)
            {
                for (uint32_t particle = 0; particle < count; ++particle)
                {
                    applyEventPayload(particle,event,event.position,event.velocity);
                    if (!executeParticleProgram(event,particle)) return false;
                }
            }
            else
            {
                // Compaction and non-contiguous event births change indices.
                // A stable particle ID must never be treated as an array index.
                for (uint32_t particle = 0; particle < count; ++particle)
                    if (m_data.Id(particle) == event.particleId)
                    {
                        applyEventPayload(particle,event,event.position,event.velocity);
                        if (!executeParticleProgram(event,particle)) return false;
                        break;
                    }
            }
            continue;
        }
        if (eventSpawnCount == 0u) continue;
        const auto available = static_cast<uint64_t>(m_asset.capacity-m_data.Count());
        const auto accepted = (std::min)(static_cast<uint64_t>(eventSpawnCount),available);
        if (!accepted) { m_statistics.dropped += eventSpawnCount; continue; }
        const auto first = m_data.Count();
        const auto required = first+static_cast<uint32_t>(accepted);
        if (required > m_data.AllocatedCapacity() &&
            !m_data.Reserve((std::min)(m_asset.capacity,(std::max)(required,m_data.AllocatedCapacity()*2))))
        { m_statistics.dropped += eventSpawnCount; continue; }
        for (uint64_t index = 0; index < accepted; ++index)
        {
            const auto id = nextId++;
            (void)m_data.Spawn(id);
            const auto particle = first+static_cast<uint32_t>(index);
            const auto spawned = ExecuteTyped(m_spawn, particle, 1, 0, static_cast<uint32_t>(index), m_spawnBindings);
            if (!spawned)
            { m_statistics.dropped += eventSpawnCount-accepted; return false; }
            // The event payload supplies the birth position and may be overridden
            // by the handler's Emitter.ReceivePosition/ReceiveVelocity writes.
            if (m_positionAttribute >= 0) m_data.Write(m_positionAttribute,particle,spawnPosition);
            if (m_velocityAttribute >= 0 && eventScript.HasWrite("Emitter.ReceiveVelocity"))
                m_data.Write(m_velocityAttribute,particle,spawnVelocity);
            applyEventPayload(particle,event,spawnPosition,spawnVelocity);
            if (!executeParticleProgram(event,particle)) return false;
            if ((m_eventGenerationMask & EventBit(ParticleEventSelection::Spawn)) != 0)
            {
                ParticleEvent spawned{ParticleEventKind::Spawn,id,spawnPosition};
                spawned.velocity = spawnVelocity;
                spawned.normal = event.normal;
                m_events.push_back(std::move(spawned));
            }
            // Randomised age lets a receiver spread incoming events over time
            // instead of producing a single synchronous pulse.
            const auto initialAge = eventRandomAge > 0 ? RandomFloat(id,m_asset.seed,0x9e3779b9u,0,0)*eventRandomAge : 0.0f;
            if (initialAge > 0 && !AdvanceParticles(particle,1,initialAge))
            { m_statistics.dropped += eventSpawnCount-accepted; return false; }
        }
        m_statistics.spawned += accepted;
        m_statistics.dropped += eventSpawnCount-accepted;
    }
    // Received events bypass the emission scheduler, so the age bookkeeping that
    // AddEvent normally owns is advanced here to keep statistics consistent.
    ++m_statistics.simulationSteps;
    m_bounds = CalculateParticleBounds(m_data);
    return true;
}
void CpuEmitter::RemoveDead()
{
    m_alive.resize(m_data.Count());
    for (uint32_t particle = 0; particle < m_data.Count(); ++particle)
    {
        const auto age = m_data.Read(m_ageAttribute,particle)[0], lifetime = m_data.Read(m_lifetimeAttribute,particle)[0];
        const bool alive = std::isfinite(age) && std::isfinite(lifetime) && age < lifetime && lifetime > 0;
        m_alive[particle] = static_cast<uint8_t>(alive);
        if (!alive)
        {
            ++m_statistics.died;
            if ((m_eventGenerationMask & EventBit(ParticleEventSelection::Death)) != 0)
            {
                ParticleEvent event{ParticleEventKind::Death,m_data.Id(particle),
                    m_positionAttribute >= 0 ? m_data.Read(m_positionAttribute,particle) : Value{}};
                if (m_velocityAttribute >= 0) event.velocity = m_data.Read(m_velocityAttribute,particle);
                m_events.push_back(std::move(event));
            }
        }
    }
    m_data.Compact(m_alive);
}

bool CpuEmitter::Step(double deltaTime)
{
    // Emitter scripts run once per scheduler tick before emission is computed.
    if (m_emitterScript.HasUpdate())
    {
        ExecutionContext context;
        context.deltaTime = static_cast<float>(deltaTime);
        context.seed = m_asset.seed;
        context.tick = m_emission.SimulationSteps();
        context.parameters = &m_parameters;
        m_parameters["Emitter.Age"] = {static_cast<float>(m_emission.Age()),0,0,0};
        if (!m_emitterScript.Tick(context)) return false;
        const auto rate = m_emitterScript.Read("Emitter.SpawnRate",static_cast<float>(m_emission.SpawnRate()));
        if (!m_emission.SetSpawnRate(rate)) return false;
        m_spawnPerFrame = static_cast<uint32_t>((std::max)(0.0f,m_emitterScript.Read("Emitter.SpawnPerFrame",static_cast<float>(m_spawnPerFrame))));
        m_spawnPerUnit = (std::max)(0.0f,m_emitterScript.Read("Emitter.SpawnPerUnit",static_cast<float>(m_spawnPerUnit)));
        if (m_emitterScript.HasWrite("Emitter.Duration"))
        {
            if (!m_emission.SetDuration(m_emitterScript.Read("Emitter.Duration",static_cast<float>(m_asset.duration)))) return false;
        }
        if (m_emitterScript.HasWrite("Emitter.Loop"))
            m_emission.SetLooping(m_emitterScript.Read("Emitter.Loop",m_asset.looping ? 1.0f : 0.0f) != 0);
        if (m_emitterScript.HasWrite("Emitter.SpawnCount"))
            m_emission.QueueBurst(static_cast<uint32_t>((std::max)(0.0f,m_emitterScript.Read("Emitter.SpawnCount",0.0f))));
    }
    // Event handler scripts update generation masks and reception settings for
    // this tick before emission publishes any events. Multiple handlers share
    // the emitter-level generation policy but keep independent receivers.
    m_parameters["Emitter.Age"] = {static_cast<float>(m_emission.Age()),0,0,0};
    uint64_t generationMask = 0;
    bool hasGenerationPolicy = false;
    for (auto& handler : m_eventHandlers)
    {
        if (!handler.script.HasUpdate()) continue;
        ExecutionContext context;
        context.deltaTime = static_cast<float>(deltaTime);
        context.seed = m_asset.seed;
        context.tick = m_emission.SimulationSteps();
        context.parameters = &m_parameters;
        if (!handler.script.Tick(context)) return false;
        const auto flag = [&](const char* name)
        { return handler.script.HasWrite(name) && handler.script.Read(name,0.0f) > 0.5f; };
        if (handler.script.HasWrite("Emitter.GenerateSpawn") || handler.script.HasWrite("Emitter.GenerateDeath") ||
            handler.script.HasWrite("Emitter.GenerateCollision"))
        {
            hasGenerationPolicy = true;
            if (flag("Emitter.GenerateSpawn")) generationMask |= EventBit(ParticleEventSelection::Spawn);
            if (flag("Emitter.GenerateDeath")) generationMask |= EventBit(ParticleEventSelection::Death);
            if (flag("Emitter.GenerateCollision")) generationMask |= EventBit(ParticleEventSelection::Collision);
        }
        if (handler.script.HasWrite("Emitter.ReceiveKind"))
        {
            const auto kind = static_cast<int32_t>(std::llround(handler.script.Read("Emitter.ReceiveKind",0.0f)));
            if (kind <= 0) handler.receiver.eventKind = ParticleEventSelection::None;
            else if (kind <= static_cast<int32_t>(ParticleEventSelection::Collision))
                handler.receiver.eventKind = static_cast<ParticleEventSelection>(kind);
        }
        if (handler.script.HasWrite("Emitter.ReceiveSpawnCount"))
        {
            const auto count = static_cast<uint32_t>((std::max)(0.0f,handler.script.Read("Emitter.ReceiveSpawnCount",1.0f)));
            if (count >= 1 && count <= 100000) handler.receiver.spawnCount = count;
        }
        if (handler.script.HasWrite("Emitter.ReceiveRandomAge"))
        {
            const auto age = handler.script.Read("Emitter.ReceiveRandomAge",0.0f);
            if (std::isfinite(age) && age >= 0 && age <= 3600) handler.receiver.randomAge = age;
        }
        if (handler.script.HasWrite("Emitter.ReceiveMaxEvents"))
        {
            const auto count = handler.script.Read("Emitter.ReceiveMaxEvents",0.0f);
            if (std::isfinite(count) && count >= 0 && count <= 1000000)
                handler.receiver.maxEventsPerFrame = static_cast<uint32_t>(std::llround(count));
        }
        if (handler.script.HasWrite("Emitter.ReceiveExecutionMode"))
        {
            const auto mode = static_cast<int32_t>(std::llround(handler.script.Read("Emitter.ReceiveExecutionMode",0.0f)));
            if (mode >= 0 && mode <= static_cast<int32_t>(EventExecutionMode::SingleParticle))
                handler.receiver.executionMode = static_cast<EventExecutionMode>(mode);
        }
    }
    if (hasGenerationPolicy) m_eventGenerationMask = generationMask;
    // Collision scripts update the CPU PhysX pass settings for this tick.
    if (m_collisionScript.HasUpdate())
    {
        ExecutionContext context;
        context.deltaTime = static_cast<float>(deltaTime);
        context.seed = m_asset.seed;
        context.tick = m_emission.SimulationSteps();
        context.parameters = &m_parameters;
        if (!m_collisionScript.Tick(context)) return false;
        if (m_collisionScript.HasWrite("Emitter.CollisionEnabled"))
            m_collisionEnabled = m_collisionScript.Read("Emitter.CollisionEnabled",0.0f) > 0.5f;
        if (m_collisionScript.HasWrite("Emitter.CollisionRadius"))
        {
            const auto radius = m_collisionScript.Read("Emitter.CollisionRadius",m_collisionRadius);
            if (std::isfinite(radius) && radius >= 0 && radius <= 10000) m_collisionRadius = radius;
        }
        if (m_collisionScript.HasWrite("Emitter.CollisionRestitution"))
        {
            const auto restitution = m_collisionScript.Read("Emitter.CollisionRestitution",m_collisionRestitution);
            if (std::isfinite(restitution) && restitution >= 0 && restitution <= 1) m_collisionRestitution = restitution;
        }
        if (m_collisionScript.HasWrite("Emitter.CollisionFriction"))
        {
            const auto friction = m_collisionScript.Read("Emitter.CollisionFriction",m_collisionFriction);
            if (std::isfinite(friction) && friction >= 0 && friction <= 1) m_collisionFriction = friction;
        }
    }
    EmissionStep emission;
    if (!m_emission.Advance(deltaTime,emission)) return false;
    // Spawn Per Frame adds an exact count per sub-step on top of the schedule.
    // The ids come from the schedule's cursor so both paths stay unique.
    // Spawn Per Unit: convert the distance the system travelled during this step
    // into births, carrying the fractional particle across steps.
    if (m_spawnPerUnit > 0 && m_emission.IsSpawning())
    {
        // The unit distance comes from the paired System.Velocity and the step
        // duration; the host publishes the velocity through the parameter map.
        const auto found = m_parameters.find("System.Velocity");
        if (found != m_parameters.end())
        {
            const auto& velocity = found->second;
            const auto travelled = std::sqrt(velocity[0]*velocity[0]+velocity[1]*velocity[1]+velocity[2]*velocity[2])*deltaTime;
            const auto budget = m_spawnPerUnitRemainder+travelled*m_spawnPerUnit;
            const auto births = static_cast<uint64_t>(std::floor(budget+1e-9));
            m_spawnPerUnitRemainder = (std::max)(0.0,budget-static_cast<double>(births));
            if (births)
            {
                SpawnBatch batch;
                batch.firstId = m_emission.NextId();
                batch.count = static_cast<uint32_t>((std::min)(births,uint64_t{10000000}));
                emission.batches.push_back(batch);
                emission.requested += batch.count;
                if (!m_emission.ConsumeNextId(batch.count)) return false;
            }
        }
    }
    if (m_spawnPerFrame && m_emission.IsSpawning())
    {
        SpawnBatch batch;
        batch.firstId = m_emission.NextId();
        batch.count = m_spawnPerFrame;
        emission.batches.push_back(batch);
        emission.requested += m_spawnPerFrame;
        if (!m_emission.ConsumeNextId(m_spawnPerFrame)) return false;
    }
    if (!AdvanceParticles(0,m_data.Count(),static_cast<float>(deltaTime))) return false;
    RemoveDead();
    uint64_t represented = 0;
    for (const auto& batch : emission.batches) { Spawn(batch); represented += batch.count; }
    m_statistics.dropped += emission.requested-represented;
    RemoveDead();
    ++m_statistics.simulationSteps;
    return true;
}

bool CpuEmitter::Seek(double age)
{
    if (!m_initialized || !std::isfinite(age) || age < 0 || age > 3600) return false;
    const bool wasPaused = m_paused;
    m_paused = false;
    Reset();
    while (m_emission.Age() + 1e-12 < age)
        if (!Tick((std::min)(1.0,age-m_emission.Age()))) { m_paused = wasPaused; return false; }
    m_paused = wasPaused;
    return true;
}
}
