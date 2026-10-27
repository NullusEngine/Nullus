#include "SystemInstance.h"
#include "VfxParameterValidation.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace NLS::VFX
{
bool SystemInstance::Initialize(std::shared_ptr<const CompiledSystem> system, std::string& error)
{
    if (!system || system->asset.emitters.size() != system->emitters.size() || system->emitters.empty())
    { error = "Particle system requires complete emitter programs"; return false; }
    if (!ValidateSystemTiming(system->asset.timing,error)) return false;
    Detail::ParameterTypes types;
    if (!Detail::AddParameterTypes(types,system->asset.parameters,error) ||
        !Detail::AddParameterTypes(types,system->systemSpawn,error) ||
        !Detail::AddParameterTypes(types,system->systemUpdate,error)) return false;
    for (size_t i = 0; i < system->emitters.size(); ++i)
    {
        const auto& program = system->emitters[i];
        if (!Detail::AddParameterTypes(types,program.spawn,error) || !Detail::AddParameterTypes(types,program.update,error)) return false;
        for (const auto& stage : program.stages) if (!Detail::AddParameterTypes(types,stage.program,error)) return false;
        for (const auto& stage : system->asset.emitters[i].simulationStages) if (!Detail::AddStageControlTypes(types,stage.bindings,error)) return false;
    }
    bool validTypes=true;
    VisitCompiledPrograms(*system,[&](const Program& program){if(validTypes)validTypes=Detail::AddParameterTypes(types,program,error);});
    std::vector<DataInterfaceDefinition> interfaces;
    if(!validTypes || !CollectCompiledDataInterfaces(*system,interfaces,error))return false;
    std::vector<std::optional<CpuEmitter>> cpu(system->emitters.size());
    for (size_t index = 0; index < cpu.size(); ++index)
        if (system->asset.emitters[index].target == SimulationTarget::CPU)
        {
            cpu[index].emplace();
            if (!cpu[index]->Initialize(system->asset.emitters[index], system->emitters[index].spawn, system->emitters[index].update, error, system->asset.parameters, system->emitters[index].emitterUpdate, system->emitters[index].emitterCollision, system->emitters[index].eventHandlers, system->emitters[index].eventParticles, system->emitters[index].emitterSpawn))
                return false;
            cpu[index]->SetCollisionWorld(m_collisionWorld);
        }
    m_system = std::move(system); m_cpu = std::move(cpu); m_parameters.clear(); m_sequence = 0;
    m_parameterTypes = std::move(types);
    for (const auto& parameter : m_system->asset.parameters)
    {
        m_parameters[parameter.name] = parameter.value;
        for (auto& emitter : m_cpu) if (emitter) emitter->SetParameter(parameter.name,parameter.value);
    }
    m_systemSpawnProgram = m_system->systemSpawn;
    m_systemUpdateProgram = m_system->systemUpdate;
    try { m_systemData.Initialize(m_system->asset.systemAttributes,1,1); }
    catch (const std::invalid_argument& exception) { error = exception.what(); return false; }
    m_systemData.Spawn(1);
    if (!m_systemSpawnProgram.instructions.empty())
    {
        ExecutionContext context;
        context.seed = 0;
        context.parameters = &m_parameters;
        if (!Typed::ExecuteParticleProgram(m_systemSpawnProgram, m_systemData, 0, 1, context, error))
        {
            error = "System Spawn bytecode failed: " + error;
            return false;
        }
    }
    m_age = 0; m_spawning = true; m_paused = false; ++m_resetRevision; m_clearRevision = 0;
    m_simulationAge = 0; m_fixedRemainder = 0; m_warmupPending = m_system->asset.timing.warmupTickCount > 0;
    Publish(0); error.clear(); return true;
}

bool SystemInstance::Tick(double deltaTime)
{
    if (!m_system || !std::isfinite(deltaTime) || deltaTime < 0 || deltaTime > 1) return false;
    for (const auto& emitter : m_system->asset.emitters) if (emitter.collisionEnabled && !m_collisionWorld) return false;
    if (m_paused || (deltaTime == 0 && !m_warmupPending)) return true;
    m_eventConsumed.assign(m_cpu.size(),{});
    m_gpuEventSpawns.assign(m_cpu.size(),{});
    for (size_t index = 0; index < m_cpu.size(); ++index)
    {
        if (m_cpu[index]) m_eventConsumed[index].assign(m_cpu[index]->EventHandlerCount(),0u);
        if (index < m_system->asset.emitters.size() &&
            m_system->asset.emitters[index].target == SimulationTarget::GPU)
            m_eventConsumed[index].assign(m_system->asset.emitters[index].eventHandlers.size(),0u);
    }
    if (!m_systemUpdateProgram.instructions.empty())
    {
        ExecutionContext context;
        context.deltaTime = static_cast<float>(deltaTime);
        context.seed = 0;
        context.tick = static_cast<uint64_t>(m_simulationAge*1000000.0);
        context.parameters = &m_parameters;
        std::string error;
        if (!Typed::ExecuteParticleProgram(m_systemUpdateProgram, m_systemData, 0, 1, context, error))
            return false;
    }
    const auto& timing = m_system->asset.timing;
    std::vector<double> steps;
    double remainder = m_fixedRemainder;
    const auto append = [&](double delta)
    {
        const double budget = remainder+delta;
        const auto requested = static_cast<uint32_t>(std::floor(budget/timing.fixedTickDelta+1e-10));
        const auto count = (std::min)(requested,kMaxSystemFixedTicks);
        steps.insert(steps.end(),count,timing.fixedTickDelta);
        // Like Niagara, retain only the fractional tick after the catch-up cap.
        remainder = (std::max)(0.0,budget-requested*timing.fixedTickDelta);
        return count*timing.fixedTickDelta;
    };
    if (m_warmupPending)
        for (uint32_t tick = 0; tick < timing.warmupTickCount; ++tick)
        {
            if (timing.fixedTick) append(timing.warmupTickDelta);
            else steps.push_back(timing.warmupTickDelta);
        }
    double advanced = 0;
    if (deltaTime > 0 && timing.fixedTick) advanced = append(deltaTime);
    else if (deltaTime > 0)
    {
        const auto count = (std::max)(1u,static_cast<uint32_t>(std::ceil(deltaTime*60.0-1e-12)));
        steps.insert(steps.end(),count,deltaTime/count); advanced = deltaTime;
    }
    const auto simulated = std::accumulate(steps.begin(),steps.end(),0.0);
    if (!std::isfinite(m_simulationAge+simulated) || m_simulationAge+simulated > 1e9) return false;
    // Drive exact sub-steps and dispatch events right after each one, so a
    // receiver reacts to the events of the same sub-step instead of waiting for
    // the end of the frame. Event lists are cleared per sub-step by the emitters.
    // Publish the camera snapshot before stepping so camera-relative modules see
    // the same values the render pass will use this frame. The reserved
    // parameters also travel with the world frame, so GPU emitters read the same
    // values without a separate publication path.
    if (m_camera.valid)
    {
        const std::array<std::pair<const char*,Value>,7> published{{
            {"Camera.Position",m_camera.position},{"Camera.Forward",m_camera.forward},
            {"Camera.Right",m_camera.right},{"Camera.Up",m_camera.up},
            {"Camera.FovDegrees",{m_camera.fovDegrees,0,0,0}},
            {"Camera.ViewportWidth",{m_camera.viewportWidth,0,0,0}},
            {"Camera.ViewportHeight",{m_camera.viewportHeight,0,0,0}}}};
        for (const auto& [name,value] : published)
        {
            // Only publish names the program declared; adding an unknown entry
            // would make the GPU parameter binding reject the frame.
            if (m_parameterTypes.contains(name)) m_parameters.insert_or_assign(name,value);
        }
        for (auto& emitter : m_cpu) if (emitter) emitter->SetCameraContext(m_camera);
    }
    // Engine context: frame index, first-frame flag and system age. Publishing
    // through the same parameter map keeps the GPU path identical.
    {
        const std::array<std::pair<const char*,Value>,3> engine{{
            // First frame means the first simulated tick, not the first publication.
            {"Engine.FirstFrame",{m_simulationAge == 0 ? 1.0f : 0.0f,0,0,0}},
            {"Engine.SystemAge",{static_cast<float>(m_simulationAge),0,0,0}},
            {"Engine.FrameIndex",{static_cast<float>(m_sequence),0,0,0}}}};
        for (const auto& [name,value] : engine)
            if (m_parameterTypes.contains(name)) m_parameters.insert_or_assign(name,value);
        for (auto& emitter : m_cpu) if (emitter) for (const auto& [name,value] : engine) (void)emitter->SetParameter(name,value);
    }
    // System transform: the owning component position and velocity, read by
    // SystemLocation and InheritVelocity.
    if (m_systemTransformValid)
    {
        const std::array<std::pair<const char*,Value>,2> transform{{
            {"System.Position",m_systemPosition},{"System.Velocity",m_systemVelocity}}};
        for (const auto& [name,value] : transform)
            if (m_parameterTypes.contains(name)) m_parameters.insert_or_assign(name,value);
        for (auto& emitter : m_cpu) if (emitter) for (const auto& [name,value] : transform) (void)emitter->SetParameter(name,value);
    }
    for (size_t step = 0; step < steps.size(); ++step)
    {
        for (auto& emitter : m_cpu)
        {
            if (!emitter) continue;
            emitter->ClearEvents(step == 0);
            if (!emitter->TickStep(steps[step])) return false;
        }
        if (!DispatchEvents()) return false;
        // Consume what was dispatched: each event is delivered exactly once, so
        // the next sub-step only sees events produced by that sub-step.
        for (auto& emitter : m_cpu) if (emitter) emitter->ClearEvents(true);
    }
    m_fixedRemainder = remainder; m_warmupPending = false;
    // Complete If Unused: once every emitter has been empty for the configured
    // grace period the whole System reports completion. A system that still has
    // live particles keeps the timer cleared.
    if (m_system->asset.completion.enabled)
    {
        const bool anyAlive = std::any_of(m_cpu.begin(),m_cpu.end(),
            [](const std::optional<CpuEmitter>& emitter) { return emitter && emitter->Data().Count() != 0; });
        if (anyAlive) m_unusedSeconds = 0;
        else m_unusedSeconds += simulated;
        if (m_unusedSeconds >= m_system->asset.completion.unusedGracePeriod) m_completed = true;
    }
    if (steps.empty()) return true;
    m_age += advanced;
    m_simulationAge += simulated;
    Publish(advanced,std::move(steps)); return true;
}


bool SystemInstance::DispatchEvents()
{
    if (!m_system) return true;
    // One pass in emitter order. A receiver that spawns publishes its own Spawn
    // events, but dispatch does not re-visit emitters, so a chain cannot loop.
    const auto kindMatches = [](const ParticleEventSelection selection, const ParticleEventKind kind)
    {
        return (selection == ParticleEventSelection::Spawn && kind == ParticleEventKind::Spawn) ||
            (selection == ParticleEventSelection::Death && kind == ParticleEventKind::Death) ||
            (selection == ParticleEventSelection::Collision && kind == ParticleEventKind::Collision);
    };
    for (size_t index = 0; index < m_cpu.size(); ++index)
    {
        auto& receiver = m_cpu[index];
        if (!receiver) continue;
        for (size_t handler = 0; handler < receiver->EventHandlerCount(); ++handler)
        {
            const auto& reception = receiver->EventReceiver(handler);
            if (reception.Empty()) continue;
            const auto& consumed = m_eventConsumed[index][handler];
            if (reception.maxEventsPerFrame != 0 && consumed >= reception.maxEventsPerFrame) continue;
            std::vector<ParticleEvent> payload;
            for (size_t sourceIndex = 0; sourceIndex < m_cpu.size(); ++sourceIndex)
            {
                if (sourceIndex == index || !m_cpu[sourceIndex]) continue;
                if (!reception.sourceEmitter.empty() &&
                    m_system->asset.emitters[sourceIndex].id != reception.sourceEmitter) continue;
                for (const auto& event : m_cpu[sourceIndex]->Events())
                    if (kindMatches(reception.eventKind,event.kind))
                    {
                        ParticleEvent delivered = event;
                        delivered.sourceIndex = static_cast<uint32_t>(sourceIndex);
                        payload.push_back(std::move(delivered));
                    }
            }
            if (payload.empty()) continue;
            if (reception.maxEventsPerFrame != 0)
            {
                const auto remaining = reception.maxEventsPerFrame-consumed;
                if (payload.size() > remaining) payload.resize(remaining);
            }
            if (!receiver->ReceiveEvents(handler,payload,reception.spawnCount,reception.randomAge,m_eventIdCursor)) return false;
            m_eventConsumed[index][handler] += static_cast<uint32_t>(payload.size());
        }
    }
    // GPU receivers consume the CPU-published event stream through the frame
    // packet. The receiver's handler script runs on the CPU, so the payload
    // carries the resolved spawn count, random age and birth overrides.
    // GPU-to-GPU events require a GPU event buffer and are not part of
    // this first receive path.
    const auto eventKindValue = [](const ParticleEventKind kind)
    {
        switch (kind)
        {
        case ParticleEventKind::Spawn: return 1.0f;
        case ParticleEventKind::Death: return 2.0f;
        case ParticleEventKind::Collision: return 3.0f;
        }
        return 0.0f;
    };
    for (size_t index = 0; index < m_system->asset.emitters.size(); ++index)
    {
        const auto& emitter = m_system->asset.emitters[index];
        if (emitter.target != SimulationTarget::GPU) continue;
        for (size_t handler = 0; handler < emitter.eventHandlers.size(); ++handler)
        {
            const auto& reception = emitter.eventHandlers[handler].receiver;
            if (reception.Empty()) continue;
            auto& consumed = m_eventConsumed[index][handler];
            if (reception.maxEventsPerFrame != 0 && consumed >= reception.maxEventsPerFrame) continue;
            EmitterScript script;
            if (handler < m_system->emitters[index].eventHandlers.size() &&
                !m_system->emitters[index].eventHandlers[handler].instructions.empty() &&
                !script.Initialize(emitter.emitterAttributes,m_system->emitters[index].eventHandlers[handler]))
                continue;
            for (size_t sourceIndex = 0; sourceIndex < m_cpu.size(); ++sourceIndex)
            {
                if (!m_cpu[sourceIndex] || sourceIndex == index) continue;
                if (!reception.sourceEmitter.empty() &&
                    m_system->asset.emitters[sourceIndex].id != reception.sourceEmitter) continue;
                for (const auto& event : m_cpu[sourceIndex]->Events())
                {
                    if (!kindMatches(reception.eventKind,event.kind)) continue;
                    if (reception.maxEventsPerFrame != 0 && consumed >= reception.maxEventsPerFrame) break;
                    uint32_t count = reception.spawnCount;
                    float randomAge = reception.randomAge;
                    Value position = event.position;
                    Value velocity = event.velocity;
                    bool overrideVelocity = false;
                    if (script.HasUpdate())
                    {
                        auto parameters = m_parameters;
                        parameters["Emitter.EventKind"] = {eventKindValue(event.kind),0,0,0};
                        parameters["Emitter.EventPosition"] = event.position;
                        parameters["Emitter.EventVelocity"] = event.velocity;
                        parameters["Emitter.EventNormal"] = event.normal;
                        parameters["Emitter.EventParticleId"] = {static_cast<float>(event.particleId),0,0,0};
                        parameters["Emitter.EventSourceIndex"] = {static_cast<float>(sourceIndex),0,0,0};
                        ExecutionContext context;
                        context.seed = emitter.seed;
                        context.tick = event.particleId;
                        context.parameters = &parameters;
                        if (!script.Tick(context)) continue;
                        if (script.HasWrite("Emitter.ReceiveSpawnCount"))
                        {
                            const auto value = script.Read("Emitter.ReceiveSpawnCount",static_cast<float>(count));
                            if (!std::isfinite(value)) continue;
                            count = static_cast<uint32_t>(std::llround(std::clamp(value,0.0f,100000.0f)));
                        }
                        if (script.HasWrite("Emitter.ReceiveRandomAge"))
                        {
                            const auto value = script.Read("Emitter.ReceiveRandomAge",randomAge);
                            if (std::isfinite(value) && value >= 0 && value <= 3600) randomAge = value;
                        }
                        if (script.HasWrite("Emitter.ReceivePosition")) position = script.ReadValue("Emitter.ReceivePosition",position);
                        overrideVelocity = script.HasWrite("Emitter.ReceiveVelocity");
                        if (overrideVelocity) velocity = script.ReadValue("Emitter.ReceiveVelocity",velocity);
                    }
                    GpuEventSpawn payload;
                    payload.position = position;
                    payload.velocity = velocity;
                    payload.normal = event.normal;
                    payload.count = count;
                    payload.randomAge = randomAge;
                    payload.kind = eventKindValue(event.kind);
                    payload.particleId = static_cast<float>(event.particleId);
                    payload.sourceIndex = static_cast<float>(sourceIndex);
                    payload.overrideVelocity = overrideVelocity;
                    payload.handlerIndex = static_cast<uint32_t>(handler);
                    payload.executionMode = reception.executionMode;
                    payload.targetParticleId = event.particleId;
                    m_gpuEventSpawns[index].push_back(payload);
                    ++consumed;
                }
            }
        }
    }
    return true;
}
void SystemInstance::Publish(double deltaTime,std::vector<double> steps)
{
    if (!m_system) return;
    auto frame = std::make_shared<SystemFrame>();
    frame->system = m_system; frame->parameters = m_parameters; frame->sequence = ++m_sequence;
    frame->resetRevision = m_resetRevision; frame->clearRevision = m_clearRevision;
    frame->deltaTime = deltaTime; frame->age = m_age; frame->spawning = m_spawning; frame->paused = m_paused;
    frame->simulationAge = m_simulationAge; frame->simulationSteps = std::move(steps);
    frame->gpuEvents = m_gpuEventSpawns;
    frame->systemAttributes.clear();
    for (const auto& attribute : m_system->asset.systemAttributes)
    {
        const auto index = m_systemData.FindAttribute(attribute.name);
        frame->systemAttributes[attribute.name] = index >= 0 && m_systemData.Count() ?
            m_systemData.Read(static_cast<uint32_t>(index),0) : attribute.defaultValue;
    }
    frame->cpu.resize(m_cpu.size());
    frame->cpuBounds.resize(m_cpu.size());
    for (size_t index = 0; index < m_cpu.size(); ++index)
        if (m_cpu[index])
        {
            frame->cpu[index] = std::make_shared<const ParticleDataSet>(m_cpu[index]->Data());
            frame->cpuBounds[index] = m_cpu[index]->Bounds();
        }
    m_frame = std::move(frame);
}

void SystemInstance::Reset()
{
    for (auto& emitter : m_cpu) if (emitter) emitter->Reset();
    try
    {
        m_systemData.Initialize(m_system->asset.systemAttributes,1,1);
        m_systemData.Spawn(1);
        if (!m_systemSpawnProgram.instructions.empty())
        {
            ExecutionContext context;
            context.parameters = &m_parameters;
            std::string error;
            (void)Typed::ExecuteParticleProgram(m_systemSpawnProgram, m_systemData, 0, 1, context, error);
        }
    }
    catch (const std::invalid_argument&) { m_systemData = {}; }
    ++m_resetRevision; m_age = 0; m_spawning = true;
    m_simulationAge = 0; m_fixedRemainder = 0; m_warmupPending = m_system && m_system->asset.timing.warmupTickCount > 0;
    m_completed = false; m_unusedSeconds = 0;
    Publish(0);
}
void SystemInstance::Activate()
{
    m_spawning = true; for (auto& emitter : m_cpu) if (emitter) emitter->Activate(); Publish(0);
}
void SystemInstance::Deactivate(bool immediate)
{
    m_spawning = false; for (auto& emitter : m_cpu) if (emitter) emitter->Deactivate(immediate);
    if (immediate) ++m_clearRevision;
    Publish(0);
}
void SystemInstance::SetPaused(bool paused)
{
    m_paused = paused; for (auto& emitter : m_cpu) if (emitter) emitter->SetPaused(paused); Publish(0);
}
bool SystemInstance::SetParameter(std::string name, Value value)
{
    return SetParameters({{std::move(name),value}});
}
bool SystemInstance::SetParameters(const std::unordered_map<std::string,Value>& values)
{
    auto parameters = m_parameters;
    for (const auto& [name,value] : values)
    {
        if (!Detail::ValidParameter(m_parameterTypes,name,value)) return false;
        parameters.insert_or_assign(name,value);
    }
    if (m_system)
    {
        std::vector<DataInterfaceDefinition> interfaces; std::string error;
        if(!CollectCompiledDataInterfaces(*m_system,interfaces,error))return false;
        for(const auto& binding:interfaces)
        {
            auto data=binding.data;
            for(size_t index=0;index<data.size();++index)
                if(const auto value=parameters.find(DataInterfaceParameter(binding.name,index));value!=parameters.end())data[index]=value->second;
            if(!ValidateDataInterfaceData(binding,data,error))return false;
        }
        for (const auto& emitter : m_system->asset.emitters)
        {
            std::vector<SimulationStageExecution> execution; std::string error;
            if (!ResolveSimulationStageControls(emitter.simulationStages,parameters,execution,error)) return false;
        }
    }
    m_parameters = std::move(parameters);
    for (auto& emitter : m_cpu) if (emitter) for (const auto& [name,value] : values) emitter->SetParameter(name,value);
    Publish(0); return true;
}
bool SystemInstance::SetTypedParameter(std::string name,ValueType type,Value value)
{
    const auto found = m_parameterTypes.find(name);
    const bool vectorApi = type == ValueType::Float4 && found != m_parameterTypes.end() && found->second >= ValueType::Float2 && found->second <= ValueType::Float4;
    if (!ValidValue(value,type) || (found != m_parameterTypes.end() && found->second != type && !vectorApi) || (found == m_parameterTypes.end() && IsIntegralType(type))) return false;
    return SetParameter(std::move(name),value);
}
bool SystemInstance::SetDataInterface(std::string_view name,ValueType storageType,std::span<const Value> values)
{
    if(!m_system)return false;
    std::vector<DataInterfaceDefinition> interfaces; std::string error;
    if(!CollectCompiledDataInterfaces(*m_system,interfaces,error))return false;
    const auto binding=std::find_if(interfaces.begin(),interfaces.end(),[&](const auto& value){return value.name==name;});
    if(binding==interfaces.end() || storageType!=DataInterfaceStorageType(*binding) || values.size()!=binding->data.size() ||
        !ValidateDataInterfaceData(*binding,values,error))return false;
    std::unordered_map<std::string,Value> parameters;
    for(size_t index=0;index<values.size();++index)parameters.emplace(DataInterfaceParameter(name,index),values[index]);
    return SetParameters(parameters);
}

void SystemInstance::SetCollisionWorld(std::shared_ptr<Physics::PhysicsWorld> world)
{
    m_collisionWorld = std::move(world); for (auto& emitter : m_cpu) if (emitter) emitter->SetCollisionWorld(m_collisionWorld);
}
bool SystemInstance::RequiresCollisionWorld() const
{
    return m_system && std::any_of(m_system->asset.emitters.begin(),m_system->asset.emitters.end(),
        [](const EmitterAsset& emitter) { return emitter.collisionEnabled; });
}
}
