#pragma once

#include <memory>
#include <string>
#include <vector>

#include <Rendering/RHI/Core/RHIDevice.h>
#include "VfxProgram.h"
#include "ParticleRenderData.h"
#include "GpuEventBuffer.h"

namespace NLS::VFX
{
struct GpuSimulationDispatch
{
    uint32_t count = 0;
    float deltaTime = 0;
    uint32_t seed = 0;
    uint64_t tick = 0;
    uint64_t firstId = 0;
    float initialAge = 0;
    float ageStep = 0;
    uint32_t stageIndex = 0, stageIteration = 0;
    // How the dispatch touches the shared GPU event buffer. The role decides
    // the resource state so a publisher's writes are ordered against the
    // reads of the emitter that consumes them.
    enum EventRole : uint32_t { kNoEvents = 0, kPublishEvents = 1, kConsumeEvents = 2 };
    uint32_t eventRole = kNoEvents;
};
struct GpuDispatchResources
{
    std::shared_ptr<Render::RHI::RHIBuffer> constants;
    std::shared_ptr<Render::RHI::RHIBuffer> parameters;
    std::shared_ptr<Render::RHI::RHIBindingSet> bindings;
    std::shared_ptr<Render::RHI::RHIComputePipeline> pipeline;
    std::shared_ptr<Render::RHI::RHIBuffer> attributes;
    std::shared_ptr<Render::RHI::RHIBuffer> ids;
    std::shared_ptr<Render::RHI::RHIBuffer> sourceAttributes;
    std::shared_ptr<Render::RHI::RHIBuffer> sourceIds;
    std::shared_ptr<Render::RHI::RHIBuffer> counters;
    std::shared_ptr<Render::RHI::RHIComputePipeline> finishPipeline;
    std::shared_ptr<Render::RHI::RHIBuffer> events, eventCounts;
    uint64_t storageVersion = 0;
};
struct GpuResizeResources
{
    std::shared_ptr<Render::RHI::RHIBuffer> sourceAttributes, sourceIds, attributes, ids;
    std::shared_ptr<Render::RHI::RHIBuffer> scratchAttributes, scratchIds;
    uint32_t previousCapacity = 0, capacity = 0;
};

// Render-owned storage. Recorded dispatch resources must survive the GPU fence.
class NLS_VFX_API GpuDataSet final
{
public:
    GpuDataSet() = default;
    GpuDataSet(GpuDataSet&&) noexcept = default;
    GpuDataSet& operator=(GpuDataSet&&) noexcept = default;
    bool Initialize(std::shared_ptr<Render::RHI::RHIDevice> device, const ParticleDataSet& initial, std::string& error,
        const std::shared_ptr<GpuEventBuffer>& events = {});
    [[nodiscard]] std::shared_ptr<GpuResizeResources> RecordReserve(Render::RHI::RHICommandBuffer& commands,
        uint32_t capacity, std::string& error);
    [[nodiscard]] std::shared_ptr<Render::RHI::RHIComputePipeline> CreatePipeline(
        const Render::RHI::RHIShaderModuleDesc& shader, std::string& error) const;
    [[nodiscard]] std::shared_ptr<Render::RHI::RHIComputePipeline> CreateCompactionPipeline(
        const Render::RHI::RHIShaderModuleDesc& shader, std::string& error) const;
    [[nodiscard]] std::shared_ptr<GpuDispatchResources> Record(
        Render::RHI::RHICommandBuffer& commands,
        std::shared_ptr<Render::RHI::RHIComputePipeline> pipeline,
        uint32_t count, float deltaTime, uint32_t seed, uint64_t tick,
        std::span<const Value> parameters, std::string& error);
    [[nodiscard]] std::shared_ptr<GpuDispatchResources> RecordCompaction(
        Render::RHI::RHICommandBuffer& commands,
        std::shared_ptr<Render::RHI::RHIComputePipeline> compact,
        std::shared_ptr<Render::RHI::RHIComputePipeline> finish, std::string& error, uint32_t verticesPerParticle = 6);
    [[nodiscard]] std::shared_ptr<GpuDispatchResources> RecordSimulation(
        Render::RHI::RHICommandBuffer& commands,
        std::shared_ptr<Render::RHI::RHIComputePipeline> pipeline,
        const GpuSimulationDispatch& dispatch, std::span<const Value> parameters, std::string& error);
    [[nodiscard]] std::shared_ptr<GpuDispatchResources> RecordReset(
        Render::RHI::RHICommandBuffer& commands,
        std::shared_ptr<Render::RHI::RHIComputePipeline> reset, std::string& error);
    // Call after the owning command list has been accepted for execution.
    bool CommitCompaction(const std::shared_ptr<GpuDispatchResources>& resources);
    [[nodiscard]] std::shared_ptr<const ParticleRenderData> RecordSnapshot(Render::RHI::RHICommandBuffer& commands, std::string& error,
        const std::optional<ParticleBounds>& orderingBounds = {}, uint64_t orderingBoundsFrame = 0) const;
    [[nodiscard]] const std::shared_ptr<Render::RHI::RHIBuffer>& AttributeBuffer() const { return m_attributes; }
    [[nodiscard]] const std::shared_ptr<Render::RHI::RHIBuffer>& IdBuffer() const { return m_ids; }
    [[nodiscard]] const std::shared_ptr<Render::RHI::RHIBuffer>& CounterBuffer() const { return m_counters; }
    [[nodiscard]] const std::shared_ptr<Render::RHI::RHIBuffer>& BoundsBuffer() const { return m_bounds; }
    [[nodiscard]] uint32_t Capacity() const { return m_capacity; }
    [[nodiscard]] uint32_t CountUpperBound() const { return m_count; }
    [[nodiscard]] const std::vector<Attribute>& Attributes() const { return m_schema; }
    [[nodiscard]] size_t ScalarCount() const { return m_scalarCount; }

private:
    friend class GpuEmitter;
    // A recording transaction shares storage but advances its own ping-pong view.
    GpuDataSet(const GpuDataSet&) = default;
    GpuDataSet& operator=(const GpuDataSet&) = default;
    bool EnsureScratch(std::string& error);
    std::shared_ptr<Render::RHI::RHIDevice> m_device;
    std::shared_ptr<Render::RHI::RHIBuffer> m_attributes;
    std::shared_ptr<Render::RHI::RHIBuffer> m_ids;
    std::shared_ptr<Render::RHI::RHIBuffer> m_counters;
    std::shared_ptr<Render::RHI::RHIBuffer> m_bounds;
    std::shared_ptr<Render::RHI::RHIBuffer> m_scratchAttributes;
    std::shared_ptr<Render::RHI::RHIBuffer> m_scratchIds;
    // The shared GPU event buffer. Emitters without one bind a one element
    // placeholder so the binding layout stays identical.
    std::shared_ptr<Render::RHI::RHIBuffer> m_events;
    std::shared_ptr<Render::RHI::RHIBuffer> m_eventCounts;
    std::shared_ptr<Render::RHI::RHIBindingLayout> m_bindingLayout;
    std::shared_ptr<Render::RHI::RHIPipelineLayout> m_pipelineLayout;
    std::shared_ptr<Render::RHI::RHIBindingLayout> m_compactionLayout;
    std::shared_ptr<Render::RHI::RHIPipelineLayout> m_compactionPipelineLayout;
    std::vector<Attribute> m_schema;
    size_t m_scalarCount = 0;
    uint32_t m_capacity = 0;
    uint32_t m_count = 0;
    uint64_t m_storageVersion = 0;
};
}
