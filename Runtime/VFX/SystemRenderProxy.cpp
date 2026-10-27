#include "SystemRenderProxy.h"
#include "GpuEventSchedule.h"

#include <cmath>
#include <numeric>
#include <Rendering/Context/RhiCommandListExecutor.h>

namespace NLS::VFX
{
struct SystemRenderProxy::DeviceState
{
    std::shared_ptr<Render::RHI::RHIDevice> device;
    std::vector<std::unique_ptr<GpuEmitter>> emitters;
    // The shared event buffer lets GPU emitters publish and consume particle
    // events without a CPU round trip.
    std::shared_ptr<GpuEventBuffer> events;
    uint64_t sequence = 0, resetRevision = 0, clearRevision = 0;
    double age = 0;
    bool spawning = false;
};

SystemRenderProxy::SystemRenderProxy(std::shared_ptr<const CompiledSystem> system) : m_system(std::move(system)) {}
SystemRenderProxy::~SystemRenderProxy() = default;

std::shared_ptr<const SystemRenderFrame> SystemRenderProxy::Prepare(std::shared_ptr<Render::RHI::RHIDevice> device,
    const std::shared_ptr<const SystemFrame>& frame, std::string& error)
{
    using namespace Render::RHI;
    if (!device || !frame || !m_system || frame->system != m_system || frame->cpu.size() != m_system->emitters.size() || frame->cpuBounds.size() != frame->cpu.size())
    { error = "Particle render publication does not match its proxy"; return {}; }
    const auto identity = device->GetCacheIdentity();
    { std::lock_guard lock(frame->cacheMutex); if (const auto found = frame->renderCache.find(identity); found != frame->renderCache.end()) return found->second; }
    if (!ValidatePlatformPrograms(*m_system,device->GetAdapter()->GetBackendType(),error)) return {};
    auto& state = m_devices[identity];
    if (!state)
    {
        auto candidate = std::make_unique<DeviceState>(); candidate->device = device; candidate->emitters.resize(m_system->emitters.size());
        auto events = std::make_shared<GpuEventBuffer>();
        if (!events->Initialize(device,static_cast<uint32_t>(m_system->asset.emitters.size()),error)) return {};
        candidate->events = events;
        const auto& shaders = m_system->FindPlatform(device->GetAdapter()->GetBackendType())->emitters;
        for (size_t index = 0; index < candidate->emitters.size(); ++index)
            if (m_system->asset.emitters[index].target == SimulationTarget::GPU)
            {
                candidate->emitters[index] = std::make_unique<GpuEmitter>();
                if (!candidate->emitters[index]->Initialize(device,m_system->asset.emitters[index],m_system->emitters[index],
                    shaders[index].simulation,error,m_system->asset.parameters,events)) return {};
            }
        for (size_t index = 0; index < candidate->emitters.size(); ++index)
        {
            if (!candidate->emitters[index]) continue;
            std::vector<GpuEmitter::EventReceiver> receivers;
            for (size_t h = 0; h < m_system->asset.emitters[index].eventHandlers.size(); ++h)
            {
                const auto& reception = m_system->asset.emitters[index].eventHandlers[h].receiver;
                if (reception.Empty()) continue;
                int32_t source = -1;
                if (!reception.sourceEmitter.empty())
                {
                    for (size_t s = 0; s < m_system->asset.emitters.size(); ++s)
                        if (m_system->asset.emitters[s].id == reception.sourceEmitter) source = static_cast<int32_t>(s);
                    if (source < 0 || m_system->asset.emitters[source].target != SimulationTarget::GPU) continue;
                }
                receivers.push_back({source,reception.spawnCount,static_cast<uint32_t>(h),reception.executionMode,
                    reception.maxEventsPerFrame,reception.randomAge,reception.eventKind});
            }
            candidate->emitters[index]->SetEventReceivers(receivers);
        }
        state = std::move(candidate);
    }
    if (frame->sequence <= state->sequence)
    { error = "Particle world frame was already advanced without retaining this snapshot"; return {}; }
    const bool reset = frame->resetRevision != state->resetRevision;
    const auto baseAge = reset ? 0 : state->age;
    const auto advanced = std::accumulate(frame->simulationSteps.begin(),frame->simulationSteps.end(),0.0);
    if (!std::isfinite(frame->simulationAge) || std::abs(baseAge+advanced-frame->simulationAge) > 1e-8)
    { error = "Particle GPU updates were skipped; enqueue every published world frame"; return {}; }
    struct Resources
    {
        std::shared_ptr<RHIDevice> device;
        std::shared_ptr<RHICommandPool> pool;
        std::shared_ptr<RHICommandBuffer> commands;
        std::vector<std::shared_ptr<const GpuFramePacket>> packets;
        std::shared_ptr<const SystemRenderFrame> output;
    };
    auto result = std::make_shared<SystemRenderFrame>(); result->emitters.resize(m_system->emitters.size());
    auto resources = std::make_shared<Resources>(); resources->device = device; resources->output = result;
    bool gpu = false;
    for (const auto& emitter : state->emitters) gpu |= emitter != nullptr;
    if (gpu)
    {
        resources->pool = device->CreateCommandPool(QueueType::Graphics,"VfxWorldSimulation");
        if (resources->pool) resources->commands = resources->pool->CreateCommandBuffer("VfxWorldSimulation");
        if (!resources->commands) { error = "Particle world simulation command allocation failed"; return {}; }
        resources->commands->Begin();
    }
    resources->packets.resize(state->emitters.size());
    for (size_t index = 0; index < state->emitters.size(); ++index)
    {
        if (auto& emitter = state->emitters[index])
        {
            if (reset && !emitter->Reset()) { error = "Particle reset was refused by a pending GPU frame"; return {}; }
            if (reset || frame->spawning != state->spawning || frame->clearRevision != state->clearRevision)
            { if (frame->spawning) emitter->Activate(); else emitter->Deactivate(frame->clearRevision != state->clearRevision); }
            emitter->SetPaused(frame->paused);
            if (!emitter->SetParameters(frame->parameters)) { error = "Invalid Simulation Stage controls in particle world frame"; return {}; }
            if (index < frame->gpuEvents.size()) emitter->QueueEvents(frame->gpuEvents[index]);
            std::string boundsError;
            if (!emitter->PollOrderingBoundsReadback(boundsError)) boundsError.clear();
        }
        else
        {
            if (!frame->cpu[index]) { error = "CPU particle frame is missing its immutable data"; return {}; }
            result->emitters[index] = UploadCpuRenderData(device,*frame->cpu[index],error,&frame->cpuBounds[index]);
            if (!result->emitters[index]) return {};
        }
    }
    std::vector<size_t> order;
    if (!BuildGpuEventOrder(m_system->asset,order,error)) return {};
    // Interleave emitters per substep. An emitter cannot consume just the last
    // substep's events when a world frame contains warmup/fixed-tick catch-up.
    for (size_t step = 0; step < (std::max)(size_t(1),frame->simulationSteps.size()); ++step)
    {
        const bool last = step+1 >= frame->simulationSteps.size();
        const auto steps = frame->simulationSteps.empty() ? std::span<const double>{} : std::span(&frame->simulationSteps[step],1);
        for (const auto index : order)
        {
            auto& packet = resources->packets[index];
            packet = state->emitters[index]->RecordSteps(*resources->commands,frame->sequence,steps,error,packet,last);
            if (!packet) return {};
            if (last)
            {
                result->emitters[index] = packet->Data().RecordSnapshot(*resources->commands,error,
                    state->emitters[index]->OrderingBounds(),state->emitters[index]->OrderingBoundsFrame());
                if (!result->emitters[index]) return {};
            }
        }
    }
    if (gpu)
    {
        resources->commands->End();
        RHISubmitDesc submit; submit.commandBuffers = {resources->commands};
        const auto accepted = Render::Context::RhiCommandListExecutor::Get().RunBackendOperation("VfxGpuSimulationSubmit",[&] { return device->GetQueue(QueueType::Graphics)->SubmitChecked(submit); });
        if (accepted.Succeeded() || accepted.mayHaveQueuedGpuWork)
            Render::Context::RhiCommandListExecutor::Get().DeferRelease({resources});
        if (!accepted.Succeeded()) { error = accepted.message; return {}; }
        for (size_t index = 0; index < state->emitters.size(); ++index)
            if (auto& emitter = state->emitters[index])
            {
                const auto& packetData = resources->packets[index];
                if (!emitter->CommitFrame(packetData)) { error = "Particle GPU world frame commit failed"; return {}; }
                std::string boundsError;
                // Ordering is a render enhancement; a failed readback falls
                // back to the component origin until a later frame succeeds.
                if (!emitter->BeginOrderingBoundsReadback(*packetData,boundsError))
                    boundsError.clear();
                if (!emitter->PollOrderingBoundsReadback(boundsError))
                    boundsError.clear();
            }
    }
    state->sequence = frame->sequence; state->resetRevision = frame->resetRevision; state->clearRevision = frame->clearRevision; state->age = frame->simulationAge;
    state->spawning = frame->spawning;
    { std::lock_guard lock(frame->cacheMutex); frame->renderCache.emplace(identity,result); }
    error.clear(); return result;
}
}
