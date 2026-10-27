#include "GpuDataSet.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <Rendering/RHI/BindingPointMap.h>

namespace NLS::VFX
{
namespace
{
using namespace Render::RHI;
constexpr uint64_t kMaxAttributeBytes = 512u*1024u*1024u;

void DispatchParticles(RHICommandBuffer& commands, uint32_t count)
{
    if (!count) return;
    const auto groups = (count+63)/64;
    commands.Dispatch((std::min)(groups,65535u),(groups+65534)/65535,1);
}

struct Constants
{
    uint32_t count;
    uint32_t capacity;
    float deltaTime;
    uint32_t seed;
    uint32_t tickLow;
    uint32_t tickHigh;
    uint32_t padding[2];
    uint32_t firstIdLow = 0;
    uint32_t firstIdHigh = 0;
    float initialAge = 0;
    float ageStep = 0;
};
static_assert(sizeof(Constants) == 48);

std::shared_ptr<RHIBuffer> Upload(const std::shared_ptr<RHIDevice>& device, const void* data, size_t bytes,
    BufferUsageFlags usage, MemoryUsage memory, const char* name)
{
    RHIBufferDesc desc;
    desc.size = bytes; desc.usage = usage; desc.memoryUsage = memory; desc.debugName = name;
    RHIBufferUploadDesc upload;
    upload.data = data; upload.dataSize = bytes;
    return device->CreateBuffer(desc,upload);
}

RHIBindingSetEntry Bind(uint32_t binding, BindingType type, std::shared_ptr<RHIBuffer> buffer, uint32_t stride)
{
    RHIBindingSetEntry entry;
    entry.binding = binding; entry.type = type; entry.buffer = std::move(buffer);
    entry.bufferRange = entry.buffer->GetDesc().size; entry.elementStride = stride;
    return entry;
}
}

bool GpuDataSet::Initialize(std::shared_ptr<Render::RHI::RHIDevice> device, const ParticleDataSet& initial, std::string& error,
    const std::shared_ptr<GpuEventBuffer>& events)
{
    using namespace Render::RHI;
    if (!device || !device->IsBackendReady() || !device->GetCapabilities().supportsCompute || !initial.Capacity() || initial.Attributes().empty())
    { error = "GPU particle storage requires a compute device and a nonempty attribute schema"; return false; }
    uint64_t scalars = 0;
    for (const auto& attribute : initial.Attributes()) scalars += ComponentCount(attribute.type);
    const auto bytes = scalars*initial.Capacity()*sizeof(float);
    if (bytes > kMaxAttributeBytes) { error = "GPU particle attribute allocation exceeds 512 MiB"; return false; }
    std::vector<float> values(static_cast<size_t>(scalars*initial.Capacity()));
    size_t offset = 0;
    for (uint32_t attribute = 0; attribute < initial.Attributes().size(); ++attribute)
    {
        for (uint32_t component = 0; component < ComponentCount(initial.Attributes()[attribute].type); ++component)
        {
            for (uint32_t particle = 0; particle < initial.Count(); ++particle)
                values[offset+particle] = initial.Read(attribute,particle)[component];
            offset += initial.Capacity();
        }
    }
    std::vector<std::array<uint32_t,2>> ids(initial.Capacity());
    for (uint32_t index = 0; index < initial.Count(); ++index)
        ids[index] = {static_cast<uint32_t>(initial.Id(index)),static_cast<uint32_t>(initial.Id(index)>>32)};
    auto attributes = Upload(device,values.data(),static_cast<size_t>(bytes),
        BufferUsageFlags::Storage|BufferUsageFlags::ShaderRead|BufferUsageFlags::CopySrc|BufferUsageFlags::CopyDst,MemoryUsage::GPUOnly,"VfxAttributes");
    auto identity = Upload(device,ids.data(),ids.size()*sizeof(ids[0]),
        BufferUsageFlags::Storage|BufferUsageFlags::ShaderRead|BufferUsageFlags::CopySrc|BufferUsageFlags::CopyDst,MemoryUsage::GPUOnly,"VfxIds");
    const std::array<uint32_t,8> initialCounters{initial.Count(),0,0,0,6,initial.Count(),0,0};
    auto counters = Upload(device,initialCounters.data(),sizeof(initialCounters),BufferUsageFlags::Storage|BufferUsageFlags::ShaderRead|BufferUsageFlags::Indirect|
        BufferUsageFlags::CopySrc|BufferUsageFlags::CopyDst,MemoryUsage::GPUOnly,"VfxCounters");
    if (!attributes || !identity || !counters) { error = "GPU particle buffer allocation failed"; return false; }
    // The event buffers are shared by every emitter of a system. Emitters that
    // do not exchange events bind a one element placeholder instead.
    std::shared_ptr<RHIBuffer> eventRecords;
    std::shared_ptr<RHIBuffer> eventCounts;
    if (events && events->Records() && events->Counts())
    {
        eventRecords = events->Records();
        eventCounts = events->Counts();
    }
    else
    {
        const std::array<float,4> placeholder{};
        eventRecords = Upload(device,placeholder.data(),sizeof(placeholder),
            BufferUsageFlags::Storage|BufferUsageFlags::ShaderRead,MemoryUsage::GPUOnly,"VfxEventPlaceholder");
        eventCounts = Upload(device,placeholder.data(),sizeof(uint32_t),
            BufferUsageFlags::Storage|BufferUsageFlags::ShaderRead,MemoryUsage::GPUOnly,"VfxEventCountPlaceholder");
    }
    if (!eventRecords || !eventCounts) { error = "GPU particle event buffer allocation failed"; return false; }
    RHIBindingLayoutDesc bindings;
    bindings.debugName = "VfxSimulationBindings";
    bindings.entries = {
        {"VfxConstants",BindingType::UniformBuffer,BindingPointMap::kPassDescriptorSet,0,1,ShaderStageMask::Compute,BindingPointMap::kPassBindingSpace,0},
        {"VfxParameters",BindingType::StructuredBuffer,BindingPointMap::kPassDescriptorSet,0,1,ShaderStageMask::Compute,BindingPointMap::kPassBindingSpace,sizeof(Value)},
        {"VfxAttributes",BindingType::StorageBuffer,BindingPointMap::kPassDescriptorSet,0,1,ShaderStageMask::Compute,BindingPointMap::kPassBindingSpace,sizeof(float)},
        {"VfxIds",BindingType::StorageBuffer,BindingPointMap::kPassDescriptorSet,1,1,ShaderStageMask::Compute,BindingPointMap::kPassBindingSpace,sizeof(ids[0])},
        {"VfxCounters",BindingType::StorageBuffer,BindingPointMap::kPassDescriptorSet,2,1,ShaderStageMask::Compute,BindingPointMap::kPassBindingSpace,sizeof(uint32_t)},
        {"VfxEvents",BindingType::StorageBuffer,BindingPointMap::kPassDescriptorSet,3,1,ShaderStageMask::Compute,BindingPointMap::kPassBindingSpace,sizeof(float)*4},
        {"VfxEventCounts",BindingType::StorageBuffer,BindingPointMap::kPassDescriptorSet,4,1,ShaderStageMask::Compute,BindingPointMap::kPassBindingSpace,sizeof(uint32_t)}
    };
    auto layout = device->CreateBindingLayout(bindings);
    if (!layout) { error = "GPU particle binding layout creation failed"; return false; }
    RHIPipelineLayoutDesc pipeline;
    pipeline.bindingLayouts.resize(BindingPointMap::kPassDescriptorSet+1);
    pipeline.bindingLayouts[BindingPointMap::kPassDescriptorSet] = layout;
    pipeline.debugName = "VfxSimulationLayout";
    auto pipelineLayout = device->CreatePipelineLayout(pipeline);
    if (!pipelineLayout) { error = "GPU particle pipeline layout creation failed"; return false; }
    auto compactBindings = bindings;
    compactBindings.debugName = "VfxCompactionBindings";
    compactBindings.entries[1].name = "VfxSourceAttributes";
    compactBindings.entries[1].elementStride = sizeof(float);
    compactBindings.entries.push_back({"VfxSourceIds",BindingType::StructuredBuffer,BindingPointMap::kPassDescriptorSet,1,1,
        ShaderStageMask::Compute,BindingPointMap::kPassBindingSpace,sizeof(ids[0])});
    auto compactLayout = device->CreateBindingLayout(compactBindings);
    pipeline.debugName = "VfxCompactionLayout";
    pipeline.bindingLayouts[BindingPointMap::kPassDescriptorSet] = compactLayout;
    auto compactPipelineLayout = device->CreatePipelineLayout(pipeline);
    if (!compactLayout || !compactPipelineLayout) { error = "GPU particle compaction layout creation failed"; return false; }
    m_device = std::move(device); m_attributes = std::move(attributes); m_ids = std::move(identity);
    m_events = std::move(eventRecords); m_eventCounts = std::move(eventCounts);
    m_bindingLayout = std::move(layout); m_pipelineLayout = std::move(pipelineLayout);
    m_counters = std::move(counters); m_compactionLayout = std::move(compactLayout); m_compactionPipelineLayout = std::move(compactPipelineLayout);
    m_scratchAttributes.reset(); m_scratchIds.reset();
    m_bounds.reset();
    m_schema = initial.Attributes(); m_scalarCount = static_cast<size_t>(scalars);
    m_capacity = initial.Capacity(); m_count = initial.Count();
    ++m_storageVersion;
    error.clear(); return true;
}

std::shared_ptr<Render::RHI::RHIComputePipeline> GpuDataSet::CreatePipeline(
    const Render::RHI::RHIShaderModuleDesc& shader, std::string& error) const
{
    if (!m_device || shader.stage != Render::RHI::ShaderStage::Compute || shader.bytecode.empty())
    { error = "GPU particle pipeline requires compute shader bytecode"; return {}; }
    auto module = m_device->CreateShaderModule(shader);
    if (!module) { error = "GPU particle compute shader module creation failed"; return {}; }
    Render::RHI::RHIComputePipelineDesc desc;
    desc.pipelineLayout = m_pipelineLayout; desc.computeShader = std::move(module); desc.debugName = "VfxSimulation";
    auto pipeline = m_device->CreateComputePipeline(desc);
    if (!pipeline) error = "GPU particle compute pipeline creation failed";
    else error.clear();
    return pipeline;
}

std::shared_ptr<GpuResizeResources> GpuDataSet::RecordReserve(Render::RHI::RHICommandBuffer& commands, uint32_t capacity, std::string& error)
{
    using namespace Render::RHI;
    if (!m_device || !commands.IsRecording() || commands.IsChildCommandBuffer() || capacity <= m_capacity || capacity > 10000000 ||
        uint64_t(capacity)*m_scalarCount*sizeof(float) > kMaxAttributeBytes)
    { error = "GPU growth requires a larger capacity within the data set limits and a recording primary command buffer"; return {}; }
    auto resources = std::make_shared<GpuResizeResources>();
    resources->sourceAttributes = m_attributes; resources->sourceIds = m_ids;
    resources->previousCapacity = m_capacity; resources->capacity = capacity;
    auto desc = m_attributes->GetDesc(); desc.size = uint64_t(capacity)*m_scalarCount*sizeof(float); desc.debugName = "VfxGrownAttributes";
    resources->attributes = m_device->CreateBuffer(desc); desc.debugName = "VfxGrownScratchAttributes";
    resources->scratchAttributes = m_device->CreateBuffer(desc);
    desc = m_ids->GetDesc(); desc.size = uint64_t(capacity)*8; desc.debugName = "VfxGrownIds";
    resources->ids = m_device->CreateBuffer(desc); desc.debugName = "VfxGrownScratchIds";
    resources->scratchIds = m_device->CreateBuffer(desc);
    if (!resources->attributes || !resources->ids || !resources->scratchAttributes || !resources->scratchIds)
    { error = "GPU particle growth allocation failed"; return {}; }
    RHIBarrierDesc before,after;
    for (const auto& buffer : {resources->sourceAttributes,resources->sourceIds})
    {
        before.bufferBarriers.push_back({buffer,ResourceState::Unknown,ResourceState::CopySrc,PipelineStageMask::AllCommands,
            PipelineStageMask::Copy,AccessMask::MemoryRead|AccessMask::MemoryWrite,AccessMask::CopyRead});
        after.bufferBarriers.push_back({buffer,ResourceState::CopySrc,ResourceState::ShaderRead,PipelineStageMask::Copy,
            PipelineStageMask::ComputeShader,AccessMask::CopyRead,AccessMask::ShaderRead});
    }
    for (const auto& buffer : {resources->attributes,resources->ids})
    {
        before.bufferBarriers.push_back({buffer,ResourceState::Unknown,ResourceState::CopyDst,PipelineStageMask::AllCommands,
            PipelineStageMask::Copy,AccessMask::MemoryRead|AccessMask::MemoryWrite,AccessMask::CopyWrite});
        after.bufferBarriers.push_back({buffer,ResourceState::CopyDst,ResourceState::ShaderWrite,PipelineStageMask::Copy,
            PipelineStageMask::ComputeShader,AccessMask::CopyWrite,AccessMask::ShaderRead|AccessMask::ShaderWrite});
    }
    auto recorded = commands.BarrierChecked(before);
    if (!recorded.Succeeded()) { error = recorded.message; return {}; }
    for (size_t scalar = 0; scalar < m_scalarCount; ++scalar)
        commands.CopyBuffer(m_attributes,resources->attributes,{scalar*m_capacity*sizeof(float),scalar*capacity*sizeof(float),uint64_t(m_capacity)*sizeof(float)});
    commands.CopyBuffer(m_ids,resources->ids,{0,0,uint64_t(m_capacity)*8});
    recorded = commands.BarrierChecked(after);
    if (!recorded.Succeeded()) { error = recorded.message; return {}; }
    m_attributes = resources->attributes; m_ids = resources->ids;
    m_scratchAttributes = resources->scratchAttributes; m_scratchIds = resources->scratchIds;
    m_capacity = capacity; m_bounds.reset(); ++m_storageVersion;
    error.clear(); return resources;
}

std::shared_ptr<Render::RHI::RHIComputePipeline> GpuDataSet::CreateCompactionPipeline(
    const Render::RHI::RHIShaderModuleDesc& shader, std::string& error) const
{
    using namespace Render::RHI;
    if (!m_device || shader.stage != ShaderStage::Compute || shader.bytecode.empty() ||
        (shader.entryPoint != "Compact" && shader.entryPoint != "Finish" && shader.entryPoint != "Reset"))
    { error = "GPU compaction requires a Compact, Finish or Reset compute kernel"; return {}; }
    auto module = m_device->CreateShaderModule(shader);
    if (!module) { error = "GPU compaction shader module creation failed"; return {}; }
    RHIComputePipelineDesc desc;
    desc.pipelineLayout = m_compactionPipelineLayout; desc.computeShader = std::move(module); desc.debugName = "Vfx"+shader.entryPoint;
    auto pipeline = m_device->CreateComputePipeline(desc);
    if (!pipeline) error = "GPU compaction pipeline creation failed";
    else error.clear();
    return pipeline;
}

std::shared_ptr<GpuDispatchResources> GpuDataSet::RecordCompaction(Render::RHI::RHICommandBuffer& commands,
    std::shared_ptr<Render::RHI::RHIComputePipeline> compact,
    std::shared_ptr<Render::RHI::RHIComputePipeline> finish, std::string& error, uint32_t verticesPerParticle)
{
    using namespace Render::RHI;
    if (!m_device || !compact || !finish || !commands.IsRecording() || commands.IsChildCommandBuffer() ||
        compact->GetDesc().pipelineLayout != m_compactionPipelineLayout || finish->GetDesc().pipelineLayout != m_compactionPipelineLayout ||
        !compact->GetDesc().computeShader || !finish->GetDesc().computeShader ||
        compact->GetDesc().computeShader->GetDesc().entryPoint != "Compact" || finish->GetDesc().computeShader->GetDesc().entryPoint != "Finish")
    { error = "Invalid GPU compaction dispatch"; return {}; }
    if (!EnsureScratch(error)) return {};
    auto resources = std::make_shared<GpuDispatchResources>();
    resources->storageVersion = m_storageVersion;
    resources->sourceAttributes = m_attributes; resources->sourceIds = m_ids;
    resources->attributes = m_scratchAttributes; resources->ids = m_scratchIds; resources->counters = m_counters;
    resources->pipeline = std::move(compact); resources->finishPipeline = std::move(finish);
    const Constants constants{m_count,m_capacity,0,0,0,0,{verticesPerParticle,0}};
    resources->constants = Upload(m_device,&constants,sizeof(constants),BufferUsageFlags::Uniform,MemoryUsage::CPUToGPU,"VfxCompactionConstants");
    if (!resources->constants) { error = "GPU compaction constants allocation failed"; return {}; }
    RHIBindingSetDesc binding;
    binding.layout = m_compactionLayout; binding.debugName = "VfxCompactionBindings";
    binding.entries = {Bind(0,BindingType::UniformBuffer,resources->constants,0),
        Bind(0,BindingType::StructuredBuffer,resources->sourceAttributes,sizeof(float)),
        Bind(1,BindingType::StructuredBuffer,resources->sourceIds,sizeof(uint32_t)*2),
        Bind(0,BindingType::StorageBuffer,resources->attributes,sizeof(float)),
        Bind(1,BindingType::StorageBuffer,resources->ids,sizeof(uint32_t)*2),Bind(2,BindingType::StorageBuffer,m_counters,sizeof(uint32_t)),
        Bind(3,BindingType::StorageBuffer,m_events,sizeof(float)*4),Bind(4,BindingType::StorageBuffer,m_eventCounts,sizeof(uint32_t))};
    resources->bindings = m_device->CreateBindingSet(binding);
    if (!resources->bindings) { error = "GPU compaction binding set creation failed"; return {}; }
    RHIBarrierDesc transitions;
    for (const auto& buffer : {resources->sourceAttributes,resources->sourceIds})
        transitions.bufferBarriers.push_back({buffer,buffer->GetState(),ResourceState::ShaderRead,PipelineStageMask::AllCommands,
            PipelineStageMask::ComputeShader,AccessMask::MemoryRead|AccessMask::MemoryWrite,AccessMask::ShaderRead});
    for (const auto& buffer : {resources->attributes,resources->ids,m_counters})
        transitions.bufferBarriers.push_back({buffer,buffer->GetState(),ResourceState::ShaderWrite,PipelineStageMask::AllCommands,
            PipelineStageMask::ComputeShader,AccessMask::MemoryRead|AccessMask::MemoryWrite,AccessMask::ShaderRead|AccessMask::ShaderWrite});
    auto recorded = commands.BarrierChecked(transitions);
    if (!recorded.Succeeded()) { error = recorded.message; return {}; }
    commands.BindComputePipeline(resources->pipeline);
    commands.BindBindingSet(BindingPointMap::kPassDescriptorSet,resources->bindings);
    DispatchParticles(commands,m_count);
    RHIBarrierDesc counterBarrier;
    counterBarrier.bufferBarriers = {{m_counters,ResourceState::ShaderWrite,ResourceState::ShaderWrite,
        PipelineStageMask::ComputeShader,PipelineStageMask::ComputeShader,AccessMask::ShaderWrite,AccessMask::ShaderRead|AccessMask::ShaderWrite}};
    recorded = commands.BarrierChecked(counterBarrier);
    if (!recorded.Succeeded()) { error = recorded.message; return {}; }
    commands.BindComputePipeline(resources->finishPipeline);
    commands.BindBindingSet(BindingPointMap::kPassDescriptorSet,resources->bindings);
    commands.Dispatch(1,1,1);
    error.clear(); return resources;
}

bool GpuDataSet::EnsureScratch(std::string& error)
{
    if (m_scratchAttributes && m_scratchIds) return true;
    if (!m_device || !m_attributes || !m_ids) { error = "GPU particle storage is not initialized"; return false; }
    auto desc = m_attributes->GetDesc(); desc.debugName = "VfxCompactedAttributes";
    auto attributes = m_device->CreateBuffer(desc);
    desc = m_ids->GetDesc(); desc.debugName = "VfxCompactedIds";
    auto ids = m_device->CreateBuffer(desc);
    if (!attributes || !ids) { error = "GPU compaction scratch allocation failed"; return false; }
    m_scratchAttributes = std::move(attributes); m_scratchIds = std::move(ids); return true;
}

bool GpuDataSet::CommitCompaction(const std::shared_ptr<GpuDispatchResources>& resources)
{
    if (!m_device || !m_attributes || !m_scratchAttributes || !resources || resources->storageVersion != m_storageVersion ||
        resources->sourceAttributes != m_attributes || resources->sourceIds != m_ids ||
        resources->attributes != m_scratchAttributes || resources->ids != m_scratchIds || resources->counters != m_counters) return false;
    std::swap(m_attributes,m_scratchAttributes); std::swap(m_ids,m_scratchIds);
    ++m_storageVersion;
    return true;
}

std::shared_ptr<const ParticleRenderData> GpuDataSet::RecordSnapshot(Render::RHI::RHICommandBuffer& commands, std::string& error,
    const std::optional<ParticleBounds>& orderingBounds, const uint64_t orderingBoundsFrame) const
{
    using namespace Render::RHI;
    if (!m_device || !m_attributes || !commands.IsRecording() || commands.IsChildCommandBuffer())
    { error = "GPU particle snapshots require initialized storage and a recording primary list"; return {}; }
    auto result = std::make_shared<ParticleRenderData>();
    result->device = m_device;
    result->attributes = m_schema; result->capacity = m_capacity; result->countUpperBound = m_count;
    result->bounds = m_bounds;
    result->orderingBounds = orderingBounds;
    result->orderingBoundsFrame = orderingBoundsFrame;
    const auto allocate = [&](const std::shared_ptr<RHIBuffer>& source, BufferUsageFlags usage, const char* name)
    {
        auto desc = source->GetDesc(); desc.usage = usage|BufferUsageFlags::CopyDst|BufferUsageFlags::CopySrc; desc.debugName = name;
        return m_device->CreateBuffer(desc);
    };
    result->values = allocate(m_attributes,BufferUsageFlags::ShaderRead,"VfxGpuFrameAttributes");
    result->ids = allocate(m_ids,BufferUsageFlags::ShaderRead,"VfxGpuFrameIds");
    result->counters = allocate(m_counters,BufferUsageFlags::Indirect|BufferUsageFlags::ShaderRead,"VfxGpuFrameCounters");
    if (!result->values || !result->ids || !result->counters) { error = "GPU particle snapshot allocation failed"; return {}; }
    const std::array pairs{std::pair{m_attributes,result->values},std::pair{m_ids,result->ids},std::pair{m_counters,result->counters}};
    RHIBarrierDesc before,after;
    for (const auto& [source,destination] : pairs)
    {
        before.bufferBarriers.push_back({source,source->GetState(),ResourceState::CopySrc,PipelineStageMask::AllCommands,
            PipelineStageMask::Copy,AccessMask::MemoryRead|AccessMask::MemoryWrite,AccessMask::CopyRead});
        before.bufferBarriers.push_back({destination,destination->GetState(),ResourceState::CopyDst,PipelineStageMask::AllCommands,
            PipelineStageMask::Copy,AccessMask::MemoryRead|AccessMask::MemoryWrite,AccessMask::CopyWrite});
        after.bufferBarriers.push_back({source,ResourceState::CopySrc,ResourceState::ShaderWrite,PipelineStageMask::Copy,
            PipelineStageMask::ComputeShader,AccessMask::CopyRead,AccessMask::ShaderRead|AccessMask::ShaderWrite});
        const bool arguments = destination == result->counters;
        after.bufferBarriers.push_back({destination,ResourceState::CopyDst,arguments ? ResourceState::IndirectArgument : ResourceState::ShaderRead,
            PipelineStageMask::Copy,arguments ? PipelineStageMask::DrawIndirect : PipelineStageMask::VertexShader,
            AccessMask::CopyWrite,arguments ? AccessMask::IndirectRead : AccessMask::ShaderRead});
    }
    auto recorded = commands.BarrierChecked(before);
    if (!recorded.Succeeded()) { error = recorded.message; return {}; }
    for (const auto& [source,destination] : pairs) commands.CopyBuffer(source,destination,{0,0,source->GetDesc().size});
    recorded = commands.BarrierChecked(after);
    if (!recorded.Succeeded()) { error = recorded.message; return {}; }
    error.clear(); return result;
}

std::shared_ptr<GpuDispatchResources> GpuDataSet::Record(Render::RHI::RHICommandBuffer& commands,
    std::shared_ptr<Render::RHI::RHIComputePipeline> pipeline, uint32_t count, float deltaTime, uint32_t seed,
    uint64_t tick, std::span<const Value> parameters, std::string& error)
{
    if (count > m_count) { error = "GPU particle dispatch exceeds its count upper bound"; return {}; }
    return RecordSimulation(commands,std::move(pipeline),{count,deltaTime,seed,tick},parameters,error);
}

std::shared_ptr<GpuDispatchResources> GpuDataSet::RecordSimulation(Render::RHI::RHICommandBuffer& commands,
    std::shared_ptr<Render::RHI::RHIComputePipeline> pipeline, const GpuSimulationDispatch& dispatch,
    std::span<const Value> parameters, std::string& error)
{
    using namespace Render::RHI;
    if (!m_device || !pipeline || pipeline->GetDesc().pipelineLayout != m_pipelineLayout ||
        !commands.IsRecording() || commands.IsChildCommandBuffer() || dispatch.count > m_capacity ||
        !std::isfinite(dispatch.deltaTime) || dispatch.deltaTime < 0 || !std::isfinite(dispatch.initialAge) ||
        dispatch.initialAge < 0 || !std::isfinite(dispatch.ageStep) || dispatch.ageStep < 0)
    { error = "Invalid GPU particle dispatch"; return {}; }
    auto resources = std::make_shared<GpuDispatchResources>();
    const Constants constants{dispatch.count,m_capacity,dispatch.deltaTime,dispatch.seed,static_cast<uint32_t>(dispatch.tick),
        static_cast<uint32_t>(dispatch.tick>>32),{dispatch.stageIndex,dispatch.stageIteration},static_cast<uint32_t>(dispatch.firstId),
        static_cast<uint32_t>(dispatch.firstId>>32),dispatch.initialAge,dispatch.ageStep};
    resources->constants = Upload(m_device,&constants,sizeof(constants),BufferUsageFlags::Uniform,MemoryUsage::CPUToGPU,"VfxConstants");
    const Value fallback{};
    resources->parameters = Upload(m_device,parameters.empty() ? &fallback : parameters.data(),
        parameters.empty() ? sizeof(Value) : parameters.size_bytes(),BufferUsageFlags::ShaderRead,MemoryUsage::CPUToGPU,"VfxParameters");
    if (!resources->constants || !resources->parameters) { error = "GPU particle dispatch parameter allocation failed"; return {}; }
    resources->attributes = m_attributes; resources->ids = m_ids; resources->pipeline = std::move(pipeline); resources->counters = m_counters;
    resources->events = m_events; resources->eventCounts = m_eventCounts;
    RHIBindingSetDesc bindings;
    bindings.layout = m_bindingLayout; bindings.debugName = "VfxDispatchBindings";
    bindings.entries = {Bind(0,BindingType::UniformBuffer,resources->constants,0),
        Bind(0,BindingType::StructuredBuffer,resources->parameters,sizeof(Value)),
        Bind(0,BindingType::StorageBuffer,m_attributes,sizeof(float)),Bind(1,BindingType::StorageBuffer,m_ids,sizeof(uint32_t)*2),
        Bind(2,BindingType::StorageBuffer,m_counters,sizeof(uint32_t)),
        Bind(3,BindingType::StorageBuffer,m_events,sizeof(float)*4),Bind(4,BindingType::StorageBuffer,m_eventCounts,sizeof(uint32_t))};
    resources->bindings = m_device->CreateBindingSet(bindings);
    if (!resources->bindings) { error = "GPU particle binding set creation failed"; return {}; }
    RHIBarrierDesc transitions;
    transitions.bufferBarriers = {
        {m_attributes,m_attributes->GetState(),ResourceState::ShaderWrite,PipelineStageMask::AllCommands,PipelineStageMask::ComputeShader,
            AccessMask::MemoryRead|AccessMask::MemoryWrite,AccessMask::ShaderRead|AccessMask::ShaderWrite},
        {m_ids,m_ids->GetState(),ResourceState::ShaderWrite,PipelineStageMask::AllCommands,PipelineStageMask::ComputeShader,
            AccessMask::MemoryRead|AccessMask::MemoryWrite,AccessMask::ShaderRead|AccessMask::ShaderWrite},
        {m_counters,m_counters->GetState(),ResourceState::ShaderWrite,PipelineStageMask::AllCommands,PipelineStageMask::ComputeShader,
            AccessMask::MemoryRead|AccessMask::MemoryWrite,AccessMask::ShaderRead|AccessMask::ShaderWrite},
        // RWStructuredBuffer bindings require UAV state for both readers and
        // writers. Same-state barriers order every publisher/consumer dispatch.
        {m_events,m_events->GetState(),ResourceState::ShaderWrite,PipelineStageMask::AllCommands,PipelineStageMask::ComputeShader,
            AccessMask::MemoryRead|AccessMask::MemoryWrite,AccessMask::ShaderRead|AccessMask::ShaderWrite},
        {m_eventCounts,m_eventCounts->GetState(),ResourceState::ShaderWrite,PipelineStageMask::AllCommands,PipelineStageMask::ComputeShader,
            AccessMask::MemoryRead|AccessMask::MemoryWrite,AccessMask::ShaderRead|AccessMask::ShaderWrite}
    };
    const auto barrier = commands.BarrierChecked(transitions);
    if (!barrier.Succeeded()) { error = barrier.message; return {}; }
    commands.BindComputePipeline(resources->pipeline);
    commands.BindBindingSet(BindingPointMap::kPassDescriptorSet,resources->bindings);
    const auto& entry = resources->pipeline->GetDesc().computeShader->GetDesc().entryPoint;
    if (entry == "BeginSpawn")
    {
        commands.Dispatch(1,1,1);
        m_count = (std::min)(m_capacity,m_count+dispatch.count);
    }
    else if (entry == "ResetEvents" || entry == "ReceiveEvents")
    {
        commands.Dispatch(1,1,1);
        if (entry == "ReceiveEvents") m_count = (std::min)(m_capacity,m_count+dispatch.count);
    }
    else DispatchParticles(commands,dispatch.count);
    error.clear(); return resources;
}

std::shared_ptr<GpuDispatchResources> GpuDataSet::RecordReset(Render::RHI::RHICommandBuffer& commands,
    std::shared_ptr<Render::RHI::RHIComputePipeline> reset, std::string& error)
{
    using namespace Render::RHI;
    if (!m_device || !reset || !commands.IsRecording() || commands.IsChildCommandBuffer() ||
        reset->GetDesc().pipelineLayout != m_compactionPipelineLayout || !reset->GetDesc().computeShader ||
        reset->GetDesc().computeShader->GetDesc().entryPoint != "Reset")
    { error = "Invalid GPU particle reset dispatch"; return {}; }
    // Reset only consumes counters; the common layout still retains valid storage bindings.
    auto resources = std::make_shared<GpuDispatchResources>();
    resources->attributes = m_attributes; resources->ids = m_ids; resources->counters = m_counters;
    resources->pipeline = std::move(reset);
    const Constants constants{0,m_capacity,0,0,0,0,{6,0}};
    resources->constants = Upload(m_device,&constants,sizeof(constants),BufferUsageFlags::Uniform,MemoryUsage::CPUToGPU,"VfxResetConstants");
    if (!resources->constants) { error = "GPU reset constants allocation failed"; return {}; }
    RHIBindingSetDesc binding;
    binding.layout = m_compactionLayout; binding.debugName = "VfxResetBindings";
    binding.entries = {Bind(0,BindingType::UniformBuffer,resources->constants,0),
        Bind(0,BindingType::StructuredBuffer,m_attributes,sizeof(float)),Bind(1,BindingType::StructuredBuffer,m_ids,sizeof(uint32_t)*2),
        Bind(0,BindingType::StorageBuffer,m_attributes,sizeof(float)),Bind(1,BindingType::StorageBuffer,m_ids,sizeof(uint32_t)*2),
        Bind(2,BindingType::StorageBuffer,m_counters,sizeof(uint32_t))};
    resources->bindings = m_device->CreateBindingSet(binding);
    if (!resources->bindings) { error = "GPU reset binding set creation failed"; return {}; }
    RHIBarrierDesc barrier;
    barrier.bufferBarriers = {{m_counters,m_counters->GetState(),ResourceState::ShaderWrite,
        PipelineStageMask::AllCommands,PipelineStageMask::ComputeShader,AccessMask::MemoryRead|AccessMask::MemoryWrite,
        AccessMask::ShaderRead|AccessMask::ShaderWrite}};
    const auto recorded = commands.BarrierChecked(barrier);
    if (!recorded.Succeeded()) { error = recorded.message; return {}; }
    commands.BindComputePipeline(resources->pipeline);
    commands.BindBindingSet(BindingPointMap::kPassDescriptorSet,resources->bindings);
    commands.Dispatch(1,1,1);
    m_count = 0;
    error.clear(); return resources;
}
}
