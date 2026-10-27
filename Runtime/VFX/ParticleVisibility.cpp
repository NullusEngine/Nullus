#include "ParticleVisibility.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <Rendering/RHI/BindingPointMap.h>

namespace NLS::VFX
{
namespace
{
using namespace Render::RHI;
namespace context = Render::Context;
bool Validate(const ParticleRenderData& data, const std::vector<Attribute>& attributes, bool ids, std::string& error)
{
    if (!data.values || !data.counters || !data.capacity || data.capacity > 10000000 || data.countUpperBound > data.capacity ||
        data.attributes.size() != attributes.size() || (ids && !data.ids))
    { error = "Invalid particle visibility storage"; return false; }
    uint64_t scalars = 0;
    for (size_t index = 0; index < attributes.size(); ++index)
    {
        if (attributes[index].name != data.attributes[index].name || attributes[index].type != data.attributes[index].type)
        { error = "Particle visibility shader layout mismatch"; return false; }
        scalars += ComponentCount(attributes[index].type);
    }
    if (data.values->GetDesc().size < scalars*data.capacity*4 || data.counters->GetDesc().size < 32 ||
        (ids && data.ids->GetDesc().size < uint64_t(data.capacity)*8))
    { error = "Particle visibility input buffers are smaller than the declared capacity"; return false; }
    return true;
}
bool ValidateSchema(const std::vector<Attribute>& attributes, std::string& error, const MeshSettings* mesh = nullptr)
{
    ParticleDataSet data;
    try { data.Initialize(attributes,1); }
    catch (const std::invalid_argument& exception) { error = exception.what(); return false; }
    const auto position = data.FindAttribute("Particles.Position"), size = data.FindAttribute("Particles.Size");
    if (position < 0 || attributes[position].type != ValueType::Float3 ||
        (!mesh && size >= 0 && attributes[size].type != ValueType::Float && attributes[size].type != ValueType::Float2))
    { error = "Particle bounds require float3 Position and scalar/float2 Size"; return false; }
    if (mesh)
    {
        if (!ValidateMeshSettings(*mesh,error)) return false;
        const auto scale = mesh->scaleAttribute.empty() ? -1 : data.FindAttribute(mesh->scaleAttribute);
        const auto orientation = mesh->orientationAttribute.empty() ? -1 : data.FindAttribute(mesh->orientationAttribute);
        const auto meshIndex = mesh->meshIndexAttribute.empty() ? -1 : data.FindAttribute(mesh->meshIndexAttribute);
        if ((scale >= 0 && attributes[scale].type != ValueType::Float && attributes[scale].type != ValueType::Float3) ||
            (orientation >= 0 && attributes[orientation].type != ValueType::Float4) ||
            (meshIndex >= 0 && attributes[meshIndex].type != ValueType::Float))
        { error = "Mesh bounds require scalar/float3 Scale, float4 MeshOrientation and a scalar MeshIndex"; return false; }
    }
    return true;
}
std::shared_ptr<RHIBuffer> Buffer(const std::shared_ptr<RHIDevice>& device, uint64_t size, BufferUsageFlags usage, const char* name,
    const void* initial = nullptr)
{
    RHIBufferDesc desc; desc.size = size; desc.usage = usage;
    desc.memoryUsage = initial ? MemoryUsage::CPUToGPU : MemoryUsage::GPUOnly; desc.debugName = name;
    if (!initial) return device->CreateBuffer(desc);
    RHIBufferUploadDesc upload; upload.data = initial; upload.dataSize = size;
    return device->CreateBuffer(desc,upload);
}
RHIBindingSetEntry Bind(uint32_t binding, BindingType type, const std::shared_ptr<RHIBuffer>& buffer, uint32_t stride)
{
    RHIBindingSetEntry entry; entry.binding = binding; entry.type = type; entry.buffer = buffer;
    entry.bufferRange = buffer->GetDesc().size; entry.elementStride = stride; return entry;
}
RHIBindingLayoutEntry Layout(const char* name, BindingType type, uint32_t binding, uint32_t stride)
{
    return {name,type,BindingPointMap::kPassDescriptorSet,binding,1,ShaderStageMask::Compute,BindingPointMap::kPassBindingSpace,stride};
}
std::shared_ptr<RHIPipelineLayout> PipelineLayout(const std::shared_ptr<RHIDevice>& device, const std::shared_ptr<RHIBindingLayout>& bindings)
{
    if (!bindings) return {};
    RHIPipelineLayoutDesc desc; desc.bindingLayouts.resize(BindingPointMap::kPassDescriptorSet+1);
    desc.bindingLayouts[BindingPointMap::kPassDescriptorSet] = bindings; return device->CreatePipelineLayout(desc);
}
std::shared_ptr<RHIComputePipeline> Pipeline(const std::shared_ptr<RHIDevice>& device,
    const std::shared_ptr<RHIPipelineLayout>& layout, const RHIShaderModuleDesc& shader, const char* entry)
{
    if (!layout || shader.stage != ShaderStage::Compute || shader.entryPoint != entry || shader.bytecode.empty()) return {};
    RHIComputePipelineDesc desc; desc.pipelineLayout = layout; desc.computeShader = device->CreateShaderModule(shader); desc.debugName = entry;
    return desc.computeShader ? device->CreateComputePipeline(desc) : nullptr;
}
void Read(context::RecordedComputeDispatchInput& dispatch, std::initializer_list<std::shared_ptr<RHIBuffer>> buffers)
{
    for (const auto& buffer : buffers)
        if (buffer && buffer->GetDesc().memoryUsage != MemoryUsage::CPUToGPU) dispatch.shaderReadBuffersBefore.push_back(buffer);
}
void Indirect(context::RecordedComputeDispatchInput& dispatch, const std::shared_ptr<RHIBuffer>& buffer, bool written)
{
    if (buffer->GetDesc().memoryUsage != MemoryUsage::CPUToGPU)
        dispatch.exportedBufferVisibilityTransitions.push_back({buffer,ResourceState::Unknown,ResourceState::IndirectArgument,
            PipelineStageMask::ComputeShader,PipelineStageMask::DrawIndirect,written ? AccessMask::ShaderWrite : AccessMask::ShaderRead,AccessMask::IndirectRead});
}
void Groups(context::RecordedComputeDispatchInput& dispatch, uint32_t groups)
{
    dispatch.groupCountX = (std::min)(groups,65535u); dispatch.groupCountY = (groups+65534)/65535;
}
}

bool ParticleBoundsCalculator::Initialize(std::shared_ptr<RHIDevice> device, const std::vector<Attribute>& attributes,
    const GpuBoundsShaders& shaders, std::string& error, const MeshSettings* mesh)
{
    if (!device || !device->IsBackendReady() || !device->GetCapabilities().supportsCompute)
    { error = "Particle bounds require a compute device"; return false; }
    if (!ValidateSchema(attributes,error,mesh)) return false;
    RHIBindingLayoutDesc desc; desc.debugName = "VfxBoundsBindings";
    desc.entries = {Layout("VfxBoundsConstants",BindingType::UniformBuffer,0,0),Layout("VfxAttributes",BindingType::StructuredBuffer,0,4),
        Layout("VfxCounters",BindingType::StructuredBuffer,1,4),Layout("VfxPartialBounds",BindingType::StructuredBuffer,2,sizeof(ParticleBounds)),
        Layout("VfxBoundsOutput",BindingType::StorageBuffer,0,sizeof(ParticleBounds))};
    auto layout = device->CreateBindingLayout(desc); auto pipeline = PipelineLayout(device,layout);
    auto reduce = Pipeline(device,pipeline,shaders.reduce,"BoundsReduce"), finish = Pipeline(device,pipeline,shaders.finish,"BoundsFinish");
    if (!reduce || !finish) { error = "Particle bounds pipeline creation failed"; return false; }
    m_device = std::move(device); m_layout = std::move(layout); m_reduce = std::move(reduce); m_finish = std::move(finish); m_attributes = attributes;
    error.clear(); return true;
}

std::shared_ptr<const GpuBoundsResources> ParticleBoundsCalculator::Prepare(const ParticleRenderData& data, std::string& error,
    const std::vector<Value>& meshBounds) const
{
    if (!m_device) { error = "Particle bounds calculator is not initialized"; return {}; }
    if (!Validate(data,m_attributes,false,error)) return {};
    auto result = std::make_shared<GpuBoundsResources>(); result->device = m_device;
    const uint32_t groups = (data.capacity+255)/256;
    struct Constants { std::array<uint32_t,4> dimensions; std::array<Value,8> meshBounds; uint32_t meshCount; std::array<uint32_t,3> padding; };
    static_assert(sizeof(Constants) == 160);
    Constants constants{}; constants.dimensions = {data.capacity,groups,0,0};
    constants.meshCount = static_cast<uint32_t>((std::min)(meshBounds.size(),kMaxMeshSlots));
    for (std::size_t slot = 0; slot < constants.meshCount; ++slot) constants.meshBounds[slot] = meshBounds[slot];
    result->constants = Buffer(m_device,sizeof(constants),BufferUsageFlags::Uniform,"VfxBoundsConstants",&constants);
    const auto usage = BufferUsageFlags::Storage|BufferUsageFlags::ShaderRead|BufferUsageFlags::CopySrc;
    result->partials = Buffer(m_device,uint64_t(groups)*sizeof(ParticleBounds),usage,"VfxPartialBounds");
    result->bounds = Buffer(m_device,sizeof(ParticleBounds),usage,"VfxFrameBounds");
    if (!result->constants || !result->partials || !result->bounds) { error = "Particle bounds allocation failed"; return {}; }
    for (uint32_t pass = 0; pass < 2; ++pass)
    {
        RHIBindingSetDesc bindings; bindings.layout = m_layout;
        bindings.entries = {Bind(0,BindingType::UniformBuffer,result->constants,0),Bind(0,BindingType::StructuredBuffer,data.values,4),
            Bind(1,BindingType::StructuredBuffer,data.counters,4),Bind(2,BindingType::StructuredBuffer,result->partials,sizeof(ParticleBounds)),
            Bind(0,BindingType::StorageBuffer,pass ? result->bounds : result->partials,sizeof(ParticleBounds))};
        auto set = m_device->CreateBindingSet(bindings);
        if (!set) { error = "Particle bounds binding creation failed"; return {}; }
        context::RecordedComputeDispatchInput dispatch; dispatch.debugName = pass ? "VfxBoundsFinish" : "VfxBoundsReduce";
        dispatch.pipeline = pass ? m_finish : m_reduce; dispatch.bindingSets = {{BindingPointMap::kPassDescriptorSet,std::move(set)}};
        if (!pass) { Groups(dispatch,groups); Read(dispatch,{data.values,data.counters}); }
        dispatch.shaderWriteBuffersBefore = {pass ? result->bounds : result->partials};
        dispatch.shaderReadBuffersAfter = dispatch.shaderWriteBuffersBefore;
        if (pass) Indirect(dispatch,data.counters,false);
        result->dispatches.push_back(std::move(dispatch));
    }
    error.clear(); return result;
}

bool ParticleVisibility::Initialize(std::shared_ptr<RHIDevice> device, const std::vector<Attribute>& attributes,
    const GpuVisibilityShaders& shaders, std::string& error, const MeshSettings* mesh)
{
    if (!device || !device->IsBackendReady() || !device->GetCapabilities().supportsCompute)
    { error = "Particle visibility requires a compute device"; return false; }
    if (!ValidateSchema(attributes,error,mesh)) return false;
    RHIBindingLayoutDesc desc; desc.debugName = "VfxVisibilityBindings";
    desc.entries = {Layout("VfxVisibilityView",BindingType::UniformBuffer,0,0),Layout("VfxAttributes",BindingType::StructuredBuffer,0,4),
        Layout("VfxIds",BindingType::StructuredBuffer,1,8),Layout("VfxCounters",BindingType::StructuredBuffer,2,4),
        Layout("VfxBounds",BindingType::StructuredBuffer,3,sizeof(ParticleBounds)),Layout("VfxVisibleScratch",BindingType::StorageBuffer,0,16),
        Layout("VfxVisibilityGroupsData",BindingType::StorageBuffer,1,16),Layout("VfxVisibleKeys",BindingType::StorageBuffer,2,16),
        Layout("VfxViewCounters",BindingType::StorageBuffer,3,4)};
    auto layout = device->CreateBindingLayout(desc); auto pipeline = PipelineLayout(device,layout);
    auto cull = Pipeline(device,pipeline,shaders.cull,"CullSprites"), scan = Pipeline(device,pipeline,shaders.scan,"ScanVisibility"),
        scatter = Pipeline(device,pipeline,shaders.scatter,"ScatterVisibility");
    if (!cull || !scan || !scatter) { error = "Particle visibility pipeline creation failed"; return false; }
    m_device = std::move(device); m_layout = std::move(layout); m_cull = std::move(cull); m_scan = std::move(scan); m_scatter = std::move(scatter);
    m_attributes = attributes; error.clear(); return true;
}

std::shared_ptr<const ParticleVisibilityResources> ParticleVisibility::Prepare(const ParticleRenderData& data,
    const SpriteView& view, std::string& error, const MeshDrawInfo* mesh) const
{
    if (!m_device || !data.bounds || data.bounds->GetDesc().size < sizeof(ParticleBounds))
    { error = "Particle visibility requires frame bounds"; return {}; }
    if (!Validate(data,m_attributes,true,error)) return {};
    const auto finite = [](const auto& values) { return std::all_of(values.begin(),values.end(),[](float value) { return std::isfinite(value); }); };
    if (!finite(view.localToWorld) || !finite(view.viewProjection) || !finite(view.cameraRight) || !finite(view.cameraUp) || !finite(view.cameraForward))
    { error = "Particle visibility view must be finite"; return {}; }
    const uint32_t count = std::bit_ceil((std::max)(256u,data.capacity)), groups = count/256;
    struct Constants
    {
        std::array<float,16> viewProjection, localToWorld;
        Value right, up, forward;
        uint32_t capacity, count, groups, cull;
        std::array<Value,8> meshBounds;
        // uint4[2] in HLSL; scalar arrays would require 16-byte element strides.
        std::array<std::array<uint32_t,4>,2> meshIndexCounts;
        uint32_t indexCount, meshCount;
        std::array<uint32_t,2> padding;
    };
    static_assert(sizeof(Constants) == 368);
    static_assert(offsetof(Constants,meshIndexCounts) == 320 && offsetof(Constants,meshCount) == 356);
    Constants constants{};
    constants.viewProjection = view.viewProjection; constants.localToWorld = view.localToWorld;
    constants.right = view.cameraRight; constants.up = view.cameraUp; constants.forward = view.cameraForward;
    constants.capacity = data.capacity; constants.count = count; constants.groups = groups;
    constants.cull = view.frustumCulling ? 1u : 0u;
    if (mesh && !mesh->Empty())
    {
        if (mesh->bounds.size() > kMaxMeshSlots || mesh->indexCounts.size() != mesh->bounds.size() ||
            std::any_of(mesh->bounds.begin(),mesh->bounds.end(),[&](const auto& value) { return !finite(value) || value[3] < 0; }) ||
            std::any_of(mesh->indexCounts.begin(),mesh->indexCounts.end(),[](uint32_t value) { return value == 0 || value%3 != 0; }))
        { error = "Invalid mesh slot bounds or index counts"; return {}; }
        constants.meshCount = static_cast<uint32_t>((std::min)(mesh->bounds.size(),kMaxMeshSlots));
        for (std::size_t slot = 0; slot < constants.meshCount; ++slot)
        {
            constants.meshBounds[slot] = mesh->bounds[slot];
            constants.meshIndexCounts[slot/4][slot%4] = slot < mesh->indexCounts.size() ? mesh->indexCounts[slot] : 0u;
        }
    }
    auto result = std::make_shared<ParticleVisibilityResources>(); result->device = m_device; result->bounds = data.bounds;
    result->constants = Buffer(m_device,sizeof(constants),BufferUsageFlags::Uniform,"VfxVisibilityView",&constants);
    const auto usage = BufferUsageFlags::Storage|BufferUsageFlags::ShaderRead|BufferUsageFlags::CopySrc;
    result->keys = Buffer(m_device,uint64_t(count)*16,usage,"VfxVisibleKeys");
    result->scratch = Buffer(m_device,uint64_t(count)*16,usage,"VfxVisibleScratch");
    result->groups = Buffer(m_device,uint64_t(groups)*16,usage,"VfxVisibilityGroups");
    // The first 16 bytes hold counts, followed by the primary 20-byte indexed
    // command. Per-slot commands start at byte 48, after the aligned header.
    const uint64_t argumentBytes = 48u+uint64_t(constants.meshCount)*20u;
    result->arguments = Buffer(m_device,argumentBytes,usage|BufferUsageFlags::Indirect,"VfxViewCounters");
    if (!result->constants || !result->keys || !result->scratch || !result->groups || !result->arguments)
    { error = "Particle visibility allocation failed"; return {}; }
    RHIBindingSetDesc bindings; bindings.layout = m_layout;
    bindings.entries = {Bind(0,BindingType::UniformBuffer,result->constants,0),Bind(0,BindingType::StructuredBuffer,data.values,4),
        Bind(1,BindingType::StructuredBuffer,data.ids,8),Bind(2,BindingType::StructuredBuffer,data.counters,4),
        Bind(3,BindingType::StructuredBuffer,data.bounds,sizeof(ParticleBounds)),Bind(0,BindingType::StorageBuffer,result->scratch,16),
        Bind(1,BindingType::StorageBuffer,result->groups,16),Bind(2,BindingType::StorageBuffer,result->keys,16),Bind(3,BindingType::StorageBuffer,result->arguments,4)};
    auto set = m_device->CreateBindingSet(bindings);
    if (!set) { error = "Particle visibility binding creation failed"; return {}; }
    for (uint32_t pass = 0; pass < 3; ++pass)
    {
        context::RecordedComputeDispatchInput dispatch;
        dispatch.debugName = pass == 0 ? "VfxCullSprites" : pass == 1 ? "VfxScanVisibility" : "VfxScatterVisibility";
        dispatch.pipeline = pass == 0 ? m_cull : pass == 1 ? m_scan : m_scatter;
        dispatch.bindingSets = {{BindingPointMap::kPassDescriptorSet,set}};
        if (pass != 1) Groups(dispatch,groups);
        if (!pass)
        {
            Read(dispatch,{data.values,data.ids,data.counters,data.bounds});
            dispatch.shaderWriteBuffersBefore = {result->scratch,result->groups,result->keys,result->arguments};
        }
        else dispatch.uavBarrierBuffersBefore = {result->scratch,result->groups,result->keys,result->arguments};
        if (pass == 2)
        {
            dispatch.shaderReadBuffersAfter = {result->keys};
            Indirect(dispatch,result->arguments,true); Indirect(dispatch,data.counters,false);
        }
        result->dispatches.push_back(std::move(dispatch));
    }
    error.clear(); return result;
}
}
