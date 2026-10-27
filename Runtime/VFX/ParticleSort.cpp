#include "ParticleSort.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <Rendering/RHI/BindingPointMap.h>

namespace NLS::VFX
{
namespace
{
using namespace Render::RHI;
struct Constants
{
    std::array<float,16> localToWorld;
    Value cameraForward;
    uint32_t capacity, count, sequence, distance;
};
static_assert(sizeof(Constants) == 96);
constexpr uint32_t kConstantStride = 256;
RHIBindingSetEntry Bind(uint32_t binding, BindingType type, const std::shared_ptr<RHIBuffer>& buffer, uint32_t stride)
{
    RHIBindingSetEntry entry;
    entry.binding = binding; entry.type = type; entry.buffer = buffer;
    entry.bufferRange = buffer->GetDesc().size; entry.elementStride = stride;
    return entry;
}
}

bool ParticleSorter::Initialize(std::shared_ptr<Render::RHI::RHIDevice> device, const std::vector<Attribute>& attributes,
    const GpuSortShaders& shaders, std::string& error)
{
    if (!device || !device->IsBackendReady() || !device->GetCapabilities().supportsCompute ||
        shaders.initialize.stage != ShaderStage::Compute || shaders.step.stage != ShaderStage::Compute)
    { error = "Particle sorting requires a compute device and sort kernels"; return false; }
    ParticleDataSet schema;
    try { schema.Initialize(attributes,1); }
    catch (const std::invalid_argument& exception) { error = exception.what(); return false; }
    const auto position = schema.FindAttribute("Particles.Position");
    if (position < 0 || attributes[position].type != ValueType::Float3)
    { error = "Particle sorting requires float3 Position"; return false; }
    RHIBindingLayoutDesc binding;
    binding.debugName = "VfxSortBindings";
    binding.entries = {
        {"VfxSortView",BindingType::UniformBuffer,BindingPointMap::kPassDescriptorSet,0,1,ShaderStageMask::Compute,BindingPointMap::kPassBindingSpace,0},
        {"VfxAttributes",BindingType::StructuredBuffer,BindingPointMap::kPassDescriptorSet,0,1,ShaderStageMask::Compute,BindingPointMap::kPassBindingSpace,4},
        {"VfxIds",BindingType::StructuredBuffer,BindingPointMap::kPassDescriptorSet,1,1,ShaderStageMask::Compute,BindingPointMap::kPassBindingSpace,8},
        {"VfxCounters",BindingType::StructuredBuffer,BindingPointMap::kPassDescriptorSet,2,1,ShaderStageMask::Compute,BindingPointMap::kPassBindingSpace,4},
        {"VfxSortKeys",BindingType::StorageBuffer,BindingPointMap::kPassDescriptorSet,0,1,ShaderStageMask::Compute,BindingPointMap::kPassBindingSpace,16}};
    auto layout = device->CreateBindingLayout(binding);
    if (!layout) { error = "Particle sort binding layout creation failed"; return false; }
    RHIPipelineLayoutDesc pipelineLayout;
    pipelineLayout.bindingLayouts.resize(BindingPointMap::kPassDescriptorSet+1);
    pipelineLayout.bindingLayouts[BindingPointMap::kPassDescriptorSet] = layout;
    auto sharedLayout = device->CreatePipelineLayout(pipelineLayout);
    if (!sharedLayout) { error = "Particle sort pipeline layout creation failed"; return false; }
    RHIComputePipelineDesc pipeline;
    pipeline.pipelineLayout = sharedLayout; pipeline.debugName = "VfxSortInitialize";
    pipeline.computeShader = device->CreateShaderModule(shaders.initialize);
    auto initialize = pipeline.computeShader ? device->CreateComputePipeline(pipeline) : nullptr;
    pipeline.computeShader = device->CreateShaderModule(shaders.step); pipeline.debugName = "VfxSortStep";
    auto step = pipeline.computeShader ? device->CreateComputePipeline(pipeline) : nullptr;
    if (!initialize || !step) { error = "Particle sort pipeline creation failed"; return false; }
    m_device = std::move(device); m_layout = std::move(layout); m_initialize = std::move(initialize); m_step = std::move(step);
    m_attributes = attributes; m_scalars = 0;
    for (const auto& attribute : attributes) m_scalars += ComponentCount(attribute.type);
    error.clear(); return true;
}

std::shared_ptr<const ParticleSortResources> ParticleSorter::Prepare(const ParticleRenderData& data,
    const std::array<float,16>& localToWorld, const Value& cameraForward, std::string& error, std::shared_ptr<RHIBuffer> initializedKeys) const
{
    const auto finite = [](const auto& values) { return std::all_of(values.begin(),values.end(),[](float value) { return std::isfinite(value); }); };
    if (!m_device || !data.values || !data.ids || !data.counters || !data.capacity || data.capacity > 10000000 || data.countUpperBound > data.capacity ||
        data.attributes.size() != m_attributes.size() ||
        !finite(localToWorld) || !finite(cameraForward) ||
        std::hypot(cameraForward[0],cameraForward[1],cameraForward[2]) < 1e-6f)
    { error = "Invalid particle sort storage or view"; return {}; }
    for (size_t index = 0; index < m_attributes.size(); ++index)
        if (data.attributes[index].name != m_attributes[index].name || data.attributes[index].type != m_attributes[index].type)
        { error = "Particle sort shader attribute layout mismatch"; return {}; }
    if (data.values->GetDesc().size < uint64_t(data.capacity)*m_scalars*sizeof(float) ||
        data.ids->GetDesc().size < uint64_t(data.capacity)*8 || data.counters->GetDesc().size < 8*sizeof(uint32_t))
    { error = "Particle sort input buffers are smaller than the declared capacity"; return {}; }
    const uint32_t count = std::bit_ceil((std::max)(256u,data.capacity));
    if (initializedKeys && initializedKeys->GetDesc().size < uint64_t(count)*16)
    { error = "Particle sort keys are smaller than the padded capacity"; return {}; }
    std::vector<Constants> passes{{localToWorld,cameraForward,data.capacity,count,initializedKeys ? 1u : 0u,0}};
    for (uint32_t sequence = 512; sequence <= count; sequence <<= 1)
        for (uint32_t distance = sequence >> 1; distance != 0; distance = distance >= 256 ? distance >> 1 : 0)
            passes.push_back({localToWorld,cameraForward,data.capacity,count,sequence,distance});
    std::vector<uint8_t> constants(passes.size()*kConstantStride);
    for (size_t index = 0; index < passes.size(); ++index)
        std::memcpy(constants.data()+index*kConstantStride,&passes[index],sizeof(Constants));
    auto resources = std::make_shared<ParticleSortResources>();
    resources->device = m_device;
    RHIBufferDesc desc;
    desc.size = constants.size(); desc.usage = BufferUsageFlags::Uniform; desc.memoryUsage = MemoryUsage::CPUToGPU; desc.debugName = "VfxSortConstants";
    RHIBufferUploadDesc upload; upload.data = constants.data(); upload.dataSize = constants.size();
    resources->constants = m_device->CreateBuffer(desc,upload);
    desc.size = uint64_t(count)*16; desc.usage = BufferUsageFlags::Storage|BufferUsageFlags::ShaderRead|BufferUsageFlags::CopySrc;
    desc.memoryUsage = MemoryUsage::GPUOnly; desc.debugName = "VfxViewSortKeys";
    resources->keys = initializedKeys ? initializedKeys : m_device->CreateBuffer(desc);
    if (!resources->constants || !resources->keys) { error = "Particle sort allocation failed"; return {}; }
    for (size_t index = 0; index < passes.size(); ++index)
    {
        RHIBindingSetDesc binding; binding.layout = m_layout; binding.debugName = "VfxSortPass";
        auto uniform = Bind(0,BindingType::UniformBuffer,resources->constants,0);
        uniform.bufferOffset = index*kConstantStride; uniform.bufferRange = sizeof(Constants);
        binding.entries = {uniform,Bind(0,BindingType::StructuredBuffer,data.values,4),Bind(1,BindingType::StructuredBuffer,data.ids,8),
            Bind(2,BindingType::StructuredBuffer,data.counters,4),Bind(0,BindingType::StorageBuffer,resources->keys,16)};
        auto set = m_device->CreateBindingSet(binding);
        if (!set) { error = "Particle sort binding creation failed"; return {}; }
        Render::Context::RecordedComputeDispatchInput dispatch;
        dispatch.debugName = index ? "VfxSortStep" : "VfxSortInitialize";
        dispatch.pipeline = index ? m_step : m_initialize;
        dispatch.bindingSets = {{BindingPointMap::kPassDescriptorSet,std::move(set)}};
        const uint32_t groups = count/256;
        dispatch.groupCountX = (std::min)(groups,65535u); dispatch.groupCountY = (groups+65534)/65535;
        if (!index)
        {
            for (const auto& buffer : {data.values,data.ids,data.counters})
                if (buffer->GetDesc().memoryUsage != MemoryUsage::CPUToGPU) dispatch.shaderReadBuffersBefore.push_back(buffer);
            dispatch.shaderWriteBuffersBefore = {resources->keys};
            if (initializedKeys) dispatch.uavBarrierBuffersBefore = {resources->keys};
        }
        else dispatch.uavBarrierBuffersBefore = {resources->keys};
        resources->dispatches.push_back(std::move(dispatch));
    }
    resources->dispatches.back().shaderReadBuffersAfter = {resources->keys};
    if (data.counters->GetDesc().memoryUsage != MemoryUsage::CPUToGPU)
        resources->dispatches.back().exportedBufferVisibilityTransitions = {{data.counters,ResourceState::ShaderRead,ResourceState::IndirectArgument,
            PipelineStageMask::ComputeShader,PipelineStageMask::DrawIndirect,AccessMask::ShaderRead,AccessMask::IndirectRead}};
    error.clear(); return resources;
}

bool ParticleSorter::Record(Render::RHI::RHICommandBuffer& commands, const ParticleSortResources& resources, std::string& error)
{
    if (!commands.IsRecording() || commands.IsChildCommandBuffer())
    { error = "Particle sorting requires a recording primary command buffer"; return false; }
    try
    {
        if (Render::Context::RecordComputeDispatchInputs(commands,resources.dispatches) != resources.dispatches.size())
        { error = "Particle sort dispatch recording failed"; return false; }
    }
    catch (const std::exception& exception) { error = exception.what(); return false; }
    error.clear(); return true;
}
}
