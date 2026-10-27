#pragma once

#include <optional>
#include <unordered_map>
#include "CompiledSystem.h"
#include "EmissionScheduler.h"
#include "GpuDataSet.h"
#include "VfxShaders.h"
#include "ParticleVisibility.h"

namespace NLS::VFX
{
class NLS_VFX_API GpuFramePacket final
{
public:
    [[nodiscard]] const GpuDataSet& Data() const { return *m_data; }
    [[nodiscard]] double Age() const { return m_emission.Age(); }
    [[nodiscard]] uint64_t FrameId() const { return m_frameId; }
    [[nodiscard]] size_t DispatchResourceCount() const { return m_resources.size(); }

private:
    friend class GpuEmitter;
    std::shared_ptr<GpuDataSet> m_data;
    EmissionScheduler m_emission;
    std::vector<std::shared_ptr<GpuDispatchResources>> m_resources;
    std::shared_ptr<const GpuBoundsResources> m_bounds;
    std::vector<std::shared_ptr<GpuResizeResources>> m_growth;
    std::shared_ptr<const uint8_t> m_owner;
    uint64_t m_generation = 0;
    uint64_t m_frameId = 0;
    size_t m_cpuEventCount = 0;
};

// Render-thread owned. All frames for an instance execute on one ordered queue.
// Retain each immutable packet through that frame's GPU fence, including after destruction/reset.
class NLS_VFX_API GpuEmitter final
{
public:
    ~GpuEmitter();
    bool Initialize(std::shared_ptr<Render::RHI::RHIDevice> device, const EmitterAsset& asset,
        const EmitterPrograms& programs, const GpuEmitterShaders& shaders, std::string& error,std::span<const ParameterDefinition> parameters = {},
        const std::shared_ptr<GpuEventBuffer>& events = {});
    // GPU event receivers. The source index addresses the shared event buffer
    // slice of the publishing emitter; the spawn count is the number of
    // particles created per received event.
    struct EventReceiver
    {
        int32_t sourceIndex = -1;
        uint32_t spawnCount = 1;
        uint32_t handlerIndex = 0;
        EventExecutionMode executionMode = EventExecutionMode::SpawnedParticles;
        uint32_t maxEvents = 0;
        float randomAge = 0;
        ParticleEventSelection kind = ParticleEventSelection::Spawn;
    };
    void SetEventReceivers(std::span<const EventReceiver> receivers) { m_eventReceivers.assign(receivers.begin(),receivers.end()); }
    [[nodiscard]] std::shared_ptr<const GpuFramePacket> RecordFrame(
        Render::RHI::RHICommandBuffer& commands, uint64_t frameId, double deltaTime, std::string& error);
    [[nodiscard]] std::shared_ptr<const GpuFramePacket> RecordSteps(
        Render::RHI::RHICommandBuffer& commands,uint64_t frameId,std::span<const double> steps,std::string& error,
        const std::shared_ptr<const GpuFramePacket>& previous = {},bool finalize = true);
    // A recording failure requires discarding the whole command list. Never submit partial frames.
    // Commit only when the entire command list has been accepted for ordered execution.
    bool CommitFrame(const std::shared_ptr<const GpuFramePacket>& packet);
    bool Reset();
    bool Activate();
    bool Deactivate(bool immediate = false);
    void SetPaused(bool paused) { m_paused = paused; }
    bool SetParameter(std::string name, Value value);
    bool SetParameters(const std::unordered_map<std::string,Value>& parameters);
    // Queues CPU-published event payloads for the next recorded frame. Events
    // are recorded as extra spawn batches with per-particle position/velocity
    // overrides.
    void QueueEvents(std::span<const GpuEventSpawn> events);
    // Starts an asynchronous CPU-visible bounds copy after the frame's command
    // list has been accepted. At most one readback is in flight per emitter.
    bool BeginOrderingBoundsReadback(const GpuFramePacket& packet, std::string& error);
    // Polls the in-flight copy. Pending is success with no new publication.
    bool PollOrderingBoundsReadback(std::string& error);
    [[nodiscard]] const std::optional<ParticleBounds>& OrderingBounds() const { return m_orderingBounds; }
    [[nodiscard]] uint64_t OrderingBoundsFrame() const { return m_orderingBoundsFrame; }
    [[nodiscard]] const GpuDataSet& Data() const { return m_data; }
    [[nodiscard]] double Age() const { return m_emission.Age(); }

private:
    static void RetireAbandonedBoundsReadbacks();
    struct Parameters { std::vector<std::string> names; std::vector<Value> defaults; };
    std::vector<Value> Capture(const Parameters& parameters) const;
    bool CaptureStages(const std::unordered_map<std::string,Value>& parameters,
        std::vector<SimulationStageExecution>& execution, std::string& error) const;
    GpuDataSet m_data;
    EmissionScheduler m_emission;
    EmitterScript m_emitterScript;
    std::shared_ptr<Render::RHI::RHIComputePipeline> m_beginSpawn, m_spawn, m_update, m_advanceSpawn, m_compact, m_finish, m_reset;
    std::shared_ptr<Render::RHI::RHIComputePipeline> m_resetEvents, m_receiveEvents;
    std::shared_ptr<ParticleBoundsCalculator> m_bounds;
    Parameters m_spawnParameters, m_updateParameters;
    struct Stage
    {
        bool enabled = true;
        uint32_t iterations = 1;
        Parameters parameters;
        std::shared_ptr<Render::RHI::RHIComputePipeline> pipeline;
        SimulationStageBindings bindings;
    };
    std::vector<Stage> m_stages;
    uint32_t m_maxCapacity = 0;
    std::unordered_map<std::string,Value> m_parameters;
    std::unordered_map<std::string,ValueType> m_parameterTypes;
    std::vector<GpuEventSpawn> m_pendingEventSpawns;
    std::vector<EventReceiver> m_eventReceivers;
    std::shared_ptr<GpuEventBuffer> m_events;
    bool m_publishesEvents = false;
    struct BoundsReadback;
    std::shared_ptr<BoundsReadback> m_pendingBoundsReadback;
    std::optional<ParticleBounds> m_orderingBounds;
    std::shared_ptr<const uint8_t> m_owner;
    std::weak_ptr<const GpuFramePacket> m_pending;
    uint64_t m_generation = 0;
    uint64_t m_lastFrame = 0;
    uint64_t m_orderingBoundsFrame = 0;
    uint64_t m_boundsRevision = 0;
    uint32_t m_boundsRefreshAge = 0;
    uint32_t m_seed = 0;
    bool m_scriptBursts = true;
    bool m_hasFrame = false;
    bool m_clearPending = false;
    bool m_paused = false;
};
}
