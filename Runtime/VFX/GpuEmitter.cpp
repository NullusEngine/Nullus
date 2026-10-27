#include "GpuEmitter.h"
#include <cstdio>
#include "VfxParameterValidation.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace NLS::VFX
{
struct GpuEmitter::BoundsReadback
{
    ParticleBounds bounds{};
    Render::RHI::RHIReadbackResult readback;
    uint64_t frameId = 0;
    uint64_t revision = 0;
};

namespace
{
std::mutex& AbandonedBoundsMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::vector<std::shared_ptr<void>>& AbandonedBoundsStorage()
{
    static std::vector<std::shared_ptr<void>> readbacks;
    return readbacks;
}

bool ValidOrderingBounds(const ParticleBounds& bounds)
{
    if (bounds.invalidCount > bounds.particleCount || bounds.spriteRadius < 0 ||
        !std::isfinite(bounds.spriteRadius)) return false;
    for (size_t component = 0; component < 3; ++component)
        if (!std::isfinite(bounds.minimum[component]) || !std::isfinite(bounds.maximum[component]) ||
            bounds.maximum[component] < bounds.minimum[component]) return false;
    return true;
}
}

GpuEmitter::~GpuEmitter()
{
    if (m_pendingBoundsReadback)
    {
        std::lock_guard lock(AbandonedBoundsMutex());
        AbandonedBoundsStorage().push_back(std::move(m_pendingBoundsReadback));
    }
    RetireAbandonedBoundsReadbacks();
}

void GpuEmitter::RetireAbandonedBoundsReadbacks()
{
    std::lock_guard lock(AbandonedBoundsMutex());
    auto& readbacks = AbandonedBoundsStorage();
    std::erase_if(readbacks,[](const std::shared_ptr<void>& storage)
    {
        const auto readback = std::static_pointer_cast<BoundsReadback>(storage);
        if (!readback->readback.completion) return true;
        const auto status = readback->readback.completion->Poll();
        return status.IsComplete();
    });
}

bool GpuEmitter::Initialize(std::shared_ptr<Render::RHI::RHIDevice> device, const EmitterAsset& asset,
    const EmitterPrograms& programs, const GpuEmitterShaders& shaders, std::string& error,std::span<const ParameterDefinition> parameters,
    const std::shared_ptr<GpuEventBuffer>& events)
{
    if (!m_pending.expired() || asset.target != SimulationTarget::GPU || asset.collisionEnabled ||
        programs.spawn.stage != ScriptStage::ParticleSpawn || programs.update.stage != ScriptStage::ParticleUpdate)
    { error = "GPU emitter requires GPU programs, no pending recording and no CPU PhysX collision"; return false; }
    EmissionScheduler emission;
    if (!emission.Initialize(asset,error)) return false;
    Detail::ParameterTypes types;
    if (!Detail::AddParameterTypes(types, parameters, error) || !Detail::AddParameterTypes(types, programs.spawn, error) || !Detail::AddParameterTypes(types, programs.update, error) || !Detail::AddParameterTypes(types, programs.emitterUpdate, error) || !Detail::AddParameterTypes(types, programs.emitterSpawn, error))
        return false;
    for (size_t i = 0; i < programs.stages.size(); ++i)
        if (!Detail::AddParameterTypes(types,programs.stages[i].program,error) ||
            (i < asset.simulationStages.size() && !Detail::AddStageControlTypes(types,asset.simulationStages[i].bindings,error))) return false;
    uint64_t scalars = 0;
    for (const auto& attribute : asset.attributes) scalars += ComponentCount(attribute.type);
    if (uint64_t(asset.capacity)*scalars*sizeof(float) > 512u*1024u*1024u)
    { error = "GPU particle limit exceeds the 512 MiB attribute storage limit"; return false; }
    auto eventStorage = events;
    if (!eventStorage)
    {
        eventStorage = std::make_shared<GpuEventBuffer>();
        if (!eventStorage->Initialize(device,256,error)) return false;
    }
    GpuDataSet data;
    try
    {
        ParticleDataSet initial;
        const auto capacity = asset.initialCapacity ? asset.initialCapacity : (std::min)(asset.capacity,256u);
        initial.Initialize(asset.attributes,capacity);
        const auto age = initial.FindAttribute("Particles.Age"), lifetime = initial.FindAttribute("Particles.Lifetime");
        if (age < 0 || lifetime < 0 || asset.attributes[age].type != ValueType::Float || asset.attributes[lifetime].type != ValueType::Float)
        { error = "GPU emitter requires scalar Age and Lifetime attributes"; return false; }
        if (!data.Initialize(device,initial,error,eventStorage)) return false;
        if (!data.EnsureScratch(error)) return false;
    }
    catch (const std::invalid_argument& exception) { error = exception.what(); return false; }
    const auto create = [&](const Render::RHI::RHIShaderModuleDesc& shader, const char* entry, bool compaction)
    {
        if (shader.entryPoint != entry) { error = std::string("GPU emitter kernel mismatch: ")+entry; return std::shared_ptr<Render::RHI::RHIComputePipeline>{}; }
        return compaction ? data.CreateCompactionPipeline(shader,error) : data.CreatePipeline(shader,error);
    };
    auto begin = create(shaders.beginSpawn,"BeginSpawn",false); if (!begin) return false;
    auto spawn = create(shaders.spawn,"SpawnParticles",false); if (!spawn) return false;
    auto update = create(shaders.update,"UpdateParticles",false); if (!update) return false;
    auto advance = create(shaders.advanceSpawn,"AdvanceSpawn",false); if (!advance) return false;
    auto compact = create(shaders.compact,"Compact",true); if (!compact) return false;
    auto finish = create(shaders.finish,"Finish",true); if (!finish) return false;
    auto reset = create(shaders.reset,"Reset",true); if (!reset) return false;
    auto resetEvents = create(shaders.resetEvents,"ResetEvents",false); if (!resetEvents) return false;
    auto receiveEvents = create(shaders.receiveEvents,"ReceiveEvents",false); if (!receiveEvents) return false;
    auto bounds = std::make_shared<ParticleBoundsCalculator>();
    if (!bounds->Initialize(device,asset.attributes,shaders.bounds,error,asset.renderer == RendererType::Mesh ? &asset.mesh : nullptr)) return false;
    const auto gather = [](const Program& program)
    {
        Parameters result;
        for (const auto& parameter : program.parameters.entries)
        {
            result.names.push_back(parameter.name);
            result.defaults.push_back(parameter.defaultValue.scalar);
        }
        return result;
    };
    if (asset.simulationStages.size() != programs.stages.size() || programs.stages.size() != shaders.stages.size() || programs.stages.size() > 64)
    { error = "GPU Simulation Stage program count mismatch"; return false; }
    std::vector<Stage> stages; uint32_t iterations = 0;
    for (size_t index = 0; index < programs.stages.size(); ++index)
    {
        const auto& settings = asset.simulationStages[index];
        if (settings.iterations > 128 || (iterations += settings.iterations) > 1024 || !ValidateSimulationStageBindings(settings.bindings,error) ||
            programs.stages[index].program.stage != ScriptStage::SimulationStage || shaders.stages[index].entryPoint != "Main")
        { error = "Invalid GPU Simulation Stage settings or kernel"; return false; }
        auto pipeline = data.CreatePipeline(shaders.stages[index],error); if (!pipeline) return false;
        stages.push_back({settings.enabled,settings.iterations,gather(programs.stages[index].program),std::move(pipeline),settings.bindings});
    }
    m_data = std::move(data); m_emission = std::move(emission);
    if (!m_emitterScript.Initialize(asset.emitterAttributes, programs.emitterUpdate, programs.emitterSpawn))
    { error = "GPU emitter script could not be initialized"; return false; }
    m_beginSpawn = std::move(begin); m_spawn = std::move(spawn); m_update = std::move(update); m_advanceSpawn = std::move(advance);
    m_compact = std::move(compact); m_finish = std::move(finish); m_reset = std::move(reset);
    m_bounds = std::move(bounds);
    m_spawnParameters = gather(programs.spawn); m_updateParameters = gather(programs.update);
    m_events = std::move(eventStorage);
    m_resetEvents = std::move(resetEvents); m_receiveEvents = std::move(receiveEvents);
    // The publish flag in the spawn program decides whether the spawn kernel
    // records the emitter's events into the shared event buffer.
    m_publishesEvents = false;
    for (size_t index = 0; index < m_spawnParameters.names.size(); ++index)
        if (m_spawnParameters.names[index] == "Emitter.EventPublish" && m_spawnParameters.defaults[index][0] != 0)
            m_publishesEvents = true;
    m_parameters.clear(); m_owner = std::make_shared<const uint8_t>(0); ++m_generation;
    m_stages = std::move(stages); m_maxCapacity = asset.capacity;
    m_parameterTypes = std::move(types);
    for (const auto& parameter : parameters) m_parameters[parameter.name] = parameter.value;
    m_parameters["Emitter.EventBufferEnabled"] = {1,0,0,0};
    m_seed = asset.seed; m_scriptBursts = asset.bursts.empty(); m_hasFrame = false; m_clearPending = false; m_paused = false;
    m_pendingBoundsReadback.reset(); m_orderingBounds.reset(); m_orderingBoundsFrame = 0; m_boundsRefreshAge = 0; ++m_boundsRevision;
    error.clear(); return true;
}

std::vector<Value> GpuEmitter::Capture(const Parameters& parameters) const
{
    auto values = parameters.defaults;
    for (size_t index = 0; index < parameters.names.size(); ++index)
        if (const auto found = m_parameters.find(parameters.names[index]); found != m_parameters.end()) values[index] = found->second;
    for (size_t index = 0; index < parameters.names.size(); ++index)
        if (parameters.names[index] == "Emitter.EventBufferEnabled") values[index] = {1,0,0,0};
    return values;
}
bool GpuEmitter::CaptureStages(const std::unordered_map<std::string,Value>& parameters,
    std::vector<SimulationStageExecution>& execution,std::string& error) const
{
    std::vector<SimulationStageExecution> candidate; candidate.reserve(m_stages.size()); uint32_t total = 0;
    for (const auto& stage : m_stages)
    {
        SimulationStageExecution value;
        if (!ResolveSimulationStageControl(stage.enabled,stage.iterations,stage.bindings,parameters,value,error)) return false;
        if (value.enabled && (total += value.iterations) > 1024) { error = "Simulation Stages exceed 1024 total iterations"; return false; }
        candidate.push_back(value);
    }
    execution = std::move(candidate); error.clear(); return true;
}

std::shared_ptr<const GpuFramePacket> GpuEmitter::RecordFrame(Render::RHI::RHICommandBuffer& commands,
    uint64_t frameId, double deltaTime, std::string& error)
{
    if (!std::isfinite(deltaTime) || deltaTime < 0 || deltaTime > 1)
    { error = "Invalid GPU frame delta time"; return {}; }
    const auto count = (std::max)(1u,static_cast<uint32_t>(std::ceil(deltaTime*60.0-1e-12)));
    std::array<double,60> steps; std::fill_n(steps.begin(),count,deltaTime/count);
    return RecordSteps(commands,frameId,std::span(steps.data(),deltaTime > 0 ? count : 0),error);
}

std::shared_ptr<const GpuFramePacket> GpuEmitter::RecordSteps(Render::RHI::RHICommandBuffer& commands,
    uint64_t frameId,std::span<const double> steps,std::string& error,
    const std::shared_ptr<const GpuFramePacket>& previous,bool finalize)
{
    if (!m_owner || (previous ? m_pending.lock() != previous || previous->m_frameId != frameId || previous->m_bounds : !m_pending.expired()) || (m_hasFrame && frameId <= m_lastFrame) ||
        !commands.IsRecording() || commands.IsChildCommandBuffer() || steps.size() > kMaxSystemWarmupTicks+kMaxSystemFixedTicks)
    { error = "Invalid GPU frame, duplicate world frame or an uncommitted recording"; return {}; }
    for (const auto step : steps) if (!std::isfinite(step) || step <= 0 || step > 1)
    { error = "Invalid GPU simulation tick"; return {}; }
    // Capture and validate controls before any commands are recorded. A rejected
    // parameter must not leave a partially advanced emission clock or GPU buffer.
    std::vector<SimulationStageExecution> stageExecution;
    if (!CaptureStages(m_parameters,stageExecution,error)) return {};
    auto packet = previous ? std::make_shared<GpuFramePacket>(*previous) : std::make_shared<GpuFramePacket>();
    packet->m_data = std::shared_ptr<GpuDataSet>(new GpuDataSet(previous ? *previous->m_data : m_data));
    if (!previous) packet->m_emission = m_emission;
    packet->m_owner = m_owner; packet->m_generation = m_generation; packet->m_frameId = frameId;
    auto& data = *packet->m_data;
    const auto retain = [&](std::shared_ptr<GpuDispatchResources> resources)
    {
        if (!resources) return false;
        packet->m_resources.push_back(std::move(resources)); return true;
    };
    if (!previous && m_clearPending && !retain(data.RecordReset(commands,m_reset,error))) return {};
    const auto spawnParameters = Capture(m_spawnParameters), updateParameters = Capture(m_updateParameters);
    const auto compact = [&]()
    {
        auto resources = data.RecordCompaction(commands,m_compact,m_finish,error);
        if (!resources || !data.CommitCompaction(resources)) return false;
        return retain(std::move(resources));
    };
    bool firstStep = !previous;
    const auto reserve = [&](uint32_t count)
    {
        const auto required = static_cast<uint32_t>((std::min)(uint64_t(m_maxCapacity),uint64_t(data.CountUpperBound())+count));
        if (required <= data.Capacity()) return true;
        auto growth = data.RecordReserve(commands,(std::min)(m_maxCapacity,(std::max)(required,data.Capacity()*2)),error);
        if (!growth) return false;
        packet->m_growth.push_back(std::move(growth)); return true;
    };
    const auto eventParametersFor = [&](uint32_t handler,EventExecutionMode mode,uint32_t maxEvents,float randomAge)
    {
        auto values = spawnParameters;
        const auto set = [&](std::string_view name,const Value& value)
        { const auto found = std::find(m_spawnParameters.names.begin(),m_spawnParameters.names.end(),name);
          if (found != m_spawnParameters.names.end()) values[found-m_spawnParameters.names.begin()] = value; };
        set("Emitter.EventHandlerIndex",{static_cast<float>(handler),0,0,0});
        set("Emitter.EventExecutionMode",{static_cast<float>(mode),0,0,0});
        set("Emitter.EventMaxEvents",{static_cast<float>(maxEvents),0,0,0});
        set("Emitter.EventRandomAge",{randomAge,0,0,0});
        set("Emitter.EventEmitterCount",{static_cast<float>(m_events->EmitterCount()),0,0,0});
        return values;
    };
    if (!m_paused)
    {
        for (const auto step : steps)
        {
            GpuSimulationDispatch resetDispatch{};
            resetDispatch.stageIteration = firstStep ? 1u : 0u;
            if (!retain(data.RecordSimulation(commands,m_resetEvents,resetDispatch,spawnParameters,error))) return {};
            firstStep = false;
            if (m_emitterScript.HasUpdate())
            {
                ExecutionContext context;
                context.deltaTime = static_cast<float>(step);
                context.seed = m_seed;
                context.tick = packet->m_emission.SimulationSteps();
                context.parameters = &m_parameters;
                auto scriptParameters = m_parameters;
                scriptParameters["Emitter.Age"] = {static_cast<float>(packet->m_emission.Age()),0,0,0};
                context.parameters = &scriptParameters;
                if (!m_emitterScript.Tick(context)) { error = "GPU emitter script failed"; return {}; }
                const auto rate = m_emitterScript.Read("Emitter.SpawnRate",static_cast<float>(packet->m_emission.SpawnRate()));
                if (!packet->m_emission.SetSpawnRate(rate)) { error = "GPU emitter script produced an invalid spawn rate"; return {}; }
                if (m_emitterScript.HasWrite("Emitter.Duration"))
                {
                    if (!packet->m_emission.SetDuration(m_emitterScript.Read("Emitter.Duration",static_cast<float>(packet->m_emission.Duration()))))
                    { error = "GPU emitter script produced an invalid duration"; return {}; }
                }
                if (m_emitterScript.HasWrite("Emitter.Loop"))
                    packet->m_emission.SetLooping(m_emitterScript.Read("Emitter.Loop",packet->m_emission.Looping() ? 1.0f : 0.0f) != 0);
                if (m_emitterScript.HasWrite("Emitter.SpawnCount"))
                {
                    const auto count = static_cast<uint32_t>((std::max)(0.0f,m_emitterScript.Read("Emitter.SpawnCount",0.0f)));
                    if (count && m_scriptBursts) packet->m_emission.QueueBurst(count);
                }
            }
            EmissionStep emission;
            if (!packet->m_emission.Advance(step,emission)) { error = "GPU emission clock overflow"; return {}; }
            GpuSimulationDispatch dispatch{data.CountUpperBound(),emission.deltaTime,m_seed,emission.tick};
            if (!retain(data.RecordSimulation(commands,m_update,dispatch,updateParameters,error)) || !compact()) return {};
            for (const auto& batch : emission.batches)
            {
                const auto required = static_cast<uint32_t>((std::min)(uint64_t(m_maxCapacity),uint64_t(data.CountUpperBound())+batch.count));
                if (required > data.Capacity())
                {
                    const auto capacity = (std::min)(m_maxCapacity,(std::max)(required,data.Capacity()*2));
                    auto growth = data.RecordReserve(commands,capacity,error); if (!growth) return {};
                    packet->m_growth.push_back(std::move(growth));
                }
                dispatch = {batch.count,0,m_seed,emission.tick,batch.firstId,batch.initialAge,batch.ageStep};
                dispatch.eventRole = m_publishesEvents ? GpuSimulationDispatch::kPublishEvents : GpuSimulationDispatch::kNoEvents;
                if (!retain(data.RecordSimulation(commands,m_beginSpawn,dispatch,spawnParameters,error)) ||
                    !retain(data.RecordSimulation(commands,m_spawn,dispatch,spawnParameters,error))) return {};
                if (batch.initialAge > 0 && !retain(data.RecordSimulation(commands,m_advanceSpawn,dispatch,updateParameters,error))) return {};
            }
            // Each CPU event already carries its policy outputs and handler
            // identity. Do not consume the queue until the whole frame commits.
            for (size_t eventIndex = packet->m_cpuEventCount; eventIndex < m_pendingEventSpawns.size(); ++eventIndex)
            {
                const auto& event = m_pendingEventSpawns[eventIndex];
                const auto count = event.executionMode == EventExecutionMode::SpawnedParticles
                    ? (std::min)(event.count,m_maxCapacity) : 0u;
                if (!reserve(count)) return {};
                auto parameters = eventParametersFor(event.handlerIndex,event.executionMode,0,event.randomAge);
                const auto set = [&](std::string_view name,const Value& value)
                { const auto found = std::find(m_spawnParameters.names.begin(),m_spawnParameters.names.end(),name);
                  if (found != m_spawnParameters.names.end()) parameters[found-m_spawnParameters.names.begin()] = value; };
                set("Emitter.EventPosition",event.position); set("Emitter.EventVelocity",event.velocity);
                set("Emitter.EventNormal",event.normal); set("Emitter.EventKind",{event.kind,0,0,0});
                set("Emitter.EventParticleId",{event.particleId,0,0,0}); set("Emitter.EventSourceIndex",{event.sourceIndex,0,0,0});
                set("Emitter.EventOverrideVelocity",{event.overrideVelocity?1.0f:0.0f,0,0,0});
                set("Emitter.EventPerEvent",{static_cast<float>(event.count),0,0,0});
                const auto firstId = packet->m_emission.NextId();
                if (!packet->m_emission.ConsumeNextId(count)) { error = "GPU event spawn id overflow"; return {}; }
                dispatch = {count,0,m_seed,event.targetParticleId,firstId,0,0};
                dispatch.stageIndex = static_cast<uint32_t>(packet->m_emission.SimulationSteps());
                dispatch.stageIteration = static_cast<uint32_t>(packet->m_emission.SimulationSteps()>>32);
                if (!retain(data.RecordSimulation(commands,m_receiveEvents,dispatch,parameters,error))) return {};
            }
            packet->m_cpuEventCount = m_pendingEventSpawns.size();
            for (const auto& receiver : m_eventReceivers)
            {
                const auto count = receiver.executionMode == EventExecutionMode::SpawnedParticles ? m_maxCapacity : 0u;
                if (!reserve(count)) return {};
                auto parameters = eventParametersFor(receiver.handlerIndex,receiver.executionMode,receiver.maxEvents,receiver.randomAge);
                const auto set = [&](std::string_view name,const Value& value)
                { const auto found = std::find(m_spawnParameters.names.begin(),m_spawnParameters.names.end(),name);
                  if (found != m_spawnParameters.names.end()) parameters[found-m_spawnParameters.names.begin()] = value; };
                set("Emitter.EventMode",{1,0,0,0});
                set("Emitter.EventSourceIndex",{static_cast<float>(receiver.sourceIndex),0,0,0});
                set("Emitter.EventPerEvent",{static_cast<float>(receiver.spawnCount),0,0,0});
                set("Emitter.EventKind",{static_cast<float>(receiver.kind),0,0,0});
                const auto firstId = packet->m_emission.NextId();
                if (!packet->m_emission.ConsumeNextId(count)) { error = "GPU event spawn id overflow"; return {}; }
                dispatch = {count,0,m_seed,packet->m_emission.SimulationSteps(),firstId,0,0};
                dispatch.stageIndex = static_cast<uint32_t>(packet->m_emission.SimulationSteps());
                dispatch.stageIteration = static_cast<uint32_t>(packet->m_emission.SimulationSteps()>>32);
                if (!retain(data.RecordSimulation(commands,m_receiveEvents,dispatch,parameters,error))) return {};
            }
            if (!compact()) return {};
            for (uint32_t index = 0; index < m_stages.size(); ++index)
            {
                const auto& stage = m_stages[index]; const auto& execution = stageExecution[index]; if (!execution.enabled) continue;
                const auto parameters = Capture(stage.parameters);
                for (uint32_t iteration = 0; iteration < execution.iterations; ++iteration)
                {
                    dispatch = {data.CountUpperBound(),emission.deltaTime,m_seed,emission.tick};
                    dispatch.stageIndex = index; dispatch.stageIteration = iteration;
                    if (!retain(data.RecordSimulation(commands,stage.pipeline,dispatch,parameters,error))) return {};
                }
            }
            if (!m_stages.empty() && !compact()) return {};
        }

    }
    if (!finalize) { m_pending = packet; error.clear(); return packet; }
    ParticleRenderData renderData;
    renderData.attributes = data.Attributes(); renderData.capacity = data.Capacity(); renderData.countUpperBound = data.CountUpperBound();
    renderData.values = data.AttributeBuffer(); renderData.counters = data.CounterBuffer();
    packet->m_bounds = m_bounds->Prepare(renderData,error);
    if (!packet->m_bounds) return {};
    if (Render::Context::RecordComputeDispatchInputs(commands,packet->m_bounds->dispatches) != packet->m_bounds->dispatches.size())
    { error = "GPU frame bounds recording failed"; return {}; }
    data.m_bounds = packet->m_bounds->bounds;
    m_pending = packet;
    error.clear(); return packet;
}

bool GpuEmitter::CommitFrame(const std::shared_ptr<const GpuFramePacket>& packet)
{
    if (!packet || !packet->m_bounds || packet->m_owner != m_owner || packet->m_generation != m_generation || m_pending.lock() != packet) return false;
    m_pendingEventSpawns.erase(m_pendingEventSpawns.begin(),m_pendingEventSpawns.begin()+
        (std::min)(packet->m_cpuEventCount,m_pendingEventSpawns.size()));
    m_data = *packet->m_data; m_emission = packet->m_emission; m_lastFrame = packet->m_frameId;
    m_hasFrame = true; m_clearPending = false; ++m_generation; m_pending.reset(); return true;
}

bool GpuEmitter::Reset()
{
    if (!m_owner || !m_pending.expired()) return false;
    m_emission.Reset();
    (void)m_emitterScript.Reset({0, m_seed, 0, &m_parameters});
    m_pendingEventSpawns.clear();
    m_clearPending = true;
    ++m_generation;
    ++m_boundsRevision;
    m_orderingBounds.reset(); m_orderingBoundsFrame = 0; m_boundsRefreshAge = 0; return true;
}

bool GpuEmitter::Activate()
{
    if (!m_owner || !m_pending.expired()) return false;
    m_emission.Activate(); return true;
}

bool GpuEmitter::Deactivate(bool immediate)
{
    if (!m_owner || !m_pending.expired()) return false;
    m_emission.Deactivate(); m_clearPending |= immediate;
    if (immediate)
    {
        ++m_boundsRevision; m_orderingBounds.reset(); m_orderingBoundsFrame = 0; m_boundsRefreshAge = 0;
    }
    return true;
}

void GpuEmitter::QueueEvents(std::span<const GpuEventSpawn> events)
{
    // The proxy republishes the complete immutable frame batch on a retry.
    m_pendingEventSpawns.assign(events.begin(),events.end());
}

bool GpuEmitter::SetParameter(std::string name, Value value)
{
    if (!Detail::ValidParameter(m_parameterTypes,name,value)) return false;
    const bool controlsStage = std::any_of(m_stages.begin(),m_stages.end(),[&](const auto& stage) { return stage.bindings.enabled == name || stage.bindings.iterations == name; });
    if (controlsStage)
    {
        auto parameters = m_parameters; parameters.insert_or_assign(name,value);
        std::vector<SimulationStageExecution> execution; std::string error;
        if (!CaptureStages(parameters,execution,error)) return false;
    }
    m_parameters.insert_or_assign(std::move(name),value); return true;
}
bool GpuEmitter::SetParameters(const std::unordered_map<std::string,Value>& parameters)
{
    for (const auto& [name,value] : parameters)
        if (!Detail::ValidParameter(m_parameterTypes,name,value)) return false;
    std::vector<SimulationStageExecution> execution; std::string error;
    if (!CaptureStages(parameters,execution,error)) return false;
    m_parameters = parameters; return true;
}

bool GpuEmitter::BeginOrderingBoundsReadback(const GpuFramePacket& packet, std::string& error)
{
    using namespace Render::RHI;
    RetireAbandonedBoundsReadbacks();
    if (!packet.m_bounds || !packet.m_bounds->device || !packet.m_bounds->bounds)
    { error = "GPU frame does not expose completed bounds storage"; return false; }

    // Ordering tolerates a small temporal lag. Build the first snapshot
    // immediately, then refresh every four submitted frames to keep the
    // asynchronous copy from adding a second readback submission per frame.
    ++m_boundsRefreshAge;
    if (m_orderingBounds && m_boundsRefreshAge < 4)
    {
        error.clear();
        return true;
    }

    if (m_pendingBoundsReadback && !PollOrderingBoundsReadback(error))
        return false;
    if (m_pendingBoundsReadback)
    {
        error.clear();
        return true;
    }

    auto readback = std::make_shared<BoundsReadback>();
    readback->frameId = packet.m_frameId;
    readback->revision = m_boundsRevision;
    RHIBufferReadbackDesc request;
    request.source = packet.m_bounds->bounds;
    request.sourceState = ResourceState::Unknown;
    request.size = sizeof(ParticleBounds);
    request.data = &readback->bounds;
    request.debugName = "VfxGpuOrderingBounds";
    readback->readback = packet.m_bounds->device->BeginReadBuffer(request);
    if (!readback->readback.Succeeded())
    {
        error = readback->readback.message;
        return false;
    }
    m_pendingBoundsReadback = std::move(readback);
    m_boundsRefreshAge = 0;
    error.clear();
    return true;
}

bool GpuEmitter::PollOrderingBoundsReadback(std::string& error)
{
    RetireAbandonedBoundsReadbacks();
    if (!m_pendingBoundsReadback)
    {
        error.clear();
        return true;
    }
    auto readback = m_pendingBoundsReadback;
    Render::RHI::RHICompletionStatus status;
    if (readback->readback.completion)
        status = readback->readback.completion->Poll();
    else
        status = {Render::RHI::RHICompletionStatusCode::Success,{}};
    if (!status.IsComplete())
    {
        error.clear();
        return true;
    }

    m_pendingBoundsReadback.reset();
    if (!status.Succeeded())
    {
        error = status.message.empty() ? "GPU ordering bounds readback failed" : status.message;
        return false;
    }
    if (readback->revision != m_boundsRevision)
    {
        error.clear();
        return true;
    }
    if (!ValidOrderingBounds(readback->bounds))
    {
        error = "GPU ordering bounds readback returned nonfinite values";
        return false;
    }
    m_orderingBounds = readback->bounds;
    m_orderingBoundsFrame = readback->frameId;
    error.clear();
    return true;
}
}
