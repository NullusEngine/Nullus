#include "ParticleRenderData.h"

#include <algorithm>

namespace NLS::VFX
{
std::shared_ptr<const ParticleRenderData> UploadCpuRenderData(
    const std::shared_ptr<Render::RHI::RHIDevice>& device, const ParticleDataSet& data, std::string& error, const ParticleBounds* bounds)
{
    using namespace Render::RHI;
    if (!device || !device->IsBackendReady() || data.Attributes().empty())
    { error = "CPU particle rendering requires a device and attribute schema"; return {}; }
    auto result = std::make_shared<ParticleRenderData>();
    result->device = device;
    result->attributes = data.Attributes(); result->capacity = (std::max)(1u,data.Count()); result->countUpperBound = data.Count();
    result->cpuBounds = bounds ? *bounds : CalculateParticleBounds(data);
    result->orderingBounds = result->cpuBounds;
    size_t scalars = 0;
    for (const auto& attribute : result->attributes) scalars += ComponentCount(attribute.type);
    if (scalars*result->capacity > 128u*1024u*1024u) { error = "CPU particle render snapshot exceeds 512 MiB"; return {}; }
    std::vector<float> values(scalars*result->capacity);
    size_t scalar = 0;
    for (uint32_t attribute = 0; attribute < result->attributes.size(); ++attribute)
        for (uint32_t component = 0; component < ComponentCount(result->attributes[attribute].type); ++component,++scalar)
            for (uint32_t particle = 0; particle < data.Count(); ++particle)
                values[scalar*result->capacity+particle] = data.Read(attribute,particle)[component];
    std::vector<std::array<uint32_t,2>> ids(result->capacity);
    for (uint32_t particle = 0; particle < data.Count(); ++particle)
        ids[particle] = {static_cast<uint32_t>(data.Id(particle)),static_cast<uint32_t>(data.Id(particle)>>32)};
    const std::array<uint32_t,8> counters{data.Count(),0,0,0,6,data.Count(),0,0};
    const auto upload = [&](const void* bytes, size_t size, BufferUsageFlags usage, const char* name)
    {
        RHIBufferDesc desc; desc.size = size; desc.memoryUsage = MemoryUsage::CPUToGPU; desc.usage = usage|BufferUsageFlags::CopySrc; desc.debugName = name;
        RHIBufferUploadDesc initial; initial.data = bytes; initial.dataSize = size;
        return device->CreateBuffer(desc,initial);
    };
    result->values = upload(values.data(),values.size()*sizeof(float),BufferUsageFlags::ShaderRead,"VfxCpuFrameAttributes");
    result->ids = upload(ids.data(),ids.size()*sizeof(ids[0]),BufferUsageFlags::ShaderRead,"VfxCpuFrameIds");
    result->counters = upload(counters.data(),sizeof(counters),BufferUsageFlags::Indirect|BufferUsageFlags::ShaderRead,"VfxCpuFrameCounters");
    result->bounds = upload(&*result->cpuBounds,sizeof(ParticleBounds),BufferUsageFlags::ShaderRead,"VfxCpuFrameBounds");
    if (!result->values || !result->ids || !result->counters || !result->bounds) { error = "CPU particle render snapshot allocation failed"; return {}; }
    error.clear(); return result;
}
}
