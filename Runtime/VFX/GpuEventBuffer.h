#pragma once

// System-wide GPU event buffer. It lets GPU emitters exchange particle events
// without a CPU round trip: a publisher writes its event records into its own
// slice of the record range, and a receiver reads those records inside its
// spawn kernel.
//
// Layout: three float4 slots per event: position/kind, velocity/id-low bits,
// normal/id-high bits. Count slots [0,256) address emitter event slices;
// [256,256+emitters*64) retain per-handler consumption across substeps.
// ID selection never converts the 64-bit identity through a float.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <Rendering/RHI/Core/RHIDevice.h>

namespace NLS::VFX
{
class NLS_VFX_API GpuEventBuffer final
{
public:
    // Events published by one emitter in one simulation substep. A publisher that exceeds
    // the budget drops the surplus; a receiver never reads past the count.
    static constexpr uint32_t kEventsPerEmitter = 256;
    static constexpr uint32_t kRecordFloats = 12;

    bool Initialize(std::shared_ptr<Render::RHI::RHIDevice> device,uint32_t emitterCount,std::string& error)
    {
        using namespace Render::RHI;
        m_records.reset(); m_counts.reset(); m_emitterCount = 0;
        if (!device || !device->IsBackendReady() || emitterCount == 0 || emitterCount > 256)
        { error = "GPU event buffer requires a ready device and a bounded emitter count"; return false; }
        const auto records = static_cast<size_t>(emitterCount)*kEventsPerEmitter*kRecordFloats*sizeof(float);
        const auto counts = static_cast<size_t>(256+emitterCount*64)*sizeof(uint32_t);
        RHIBufferDesc recordDesc;
        recordDesc.size = records; recordDesc.usage = BufferUsageFlags::Storage|BufferUsageFlags::ShaderRead;
        recordDesc.memoryUsage = MemoryUsage::GPUOnly; recordDesc.debugName = "VfxGpuEvents";
        std::vector<uint8_t> zeroRecords(records,0);
        RHIBufferUploadDesc recordUpload; recordUpload.data = zeroRecords.data(); recordUpload.dataSize = records;
        auto eventRecords = device->CreateBuffer(recordDesc,recordUpload);
        RHIBufferDesc countDesc;
        countDesc.size = counts; countDesc.usage = BufferUsageFlags::Storage|BufferUsageFlags::ShaderRead;
        countDesc.memoryUsage = MemoryUsage::GPUOnly; countDesc.debugName = "VfxGpuEventCounts";
        std::vector<uint32_t> zeroCounts(counts/sizeof(uint32_t),0);
        RHIBufferUploadDesc countUpload; countUpload.data = zeroCounts.data(); countUpload.dataSize = counts;
        auto eventCounts = device->CreateBuffer(countDesc,countUpload);
        if (!eventRecords || !eventCounts) { error = "GPU event buffer allocation failed"; return false; }
        m_records = std::move(eventRecords); m_counts = std::move(eventCounts); m_emitterCount = emitterCount;
        error.clear(); return true;
    }
    [[nodiscard]] std::shared_ptr<Render::RHI::RHIBuffer> Records() const { return m_records; }
    [[nodiscard]] std::shared_ptr<Render::RHI::RHIBuffer> Counts() const { return m_counts; }
    [[nodiscard]] uint32_t EmitterCount() const { return m_emitterCount; }
    [[nodiscard]] uint32_t Base(uint32_t emitterIndex) const { return emitterIndex*kEventsPerEmitter; }
    // Bounds the buffer so a stray index can never read outside the range.
    [[nodiscard]] uint32_t RecordCount() const { return m_emitterCount*kEventsPerEmitter; }

private:
    std::shared_ptr<Render::RHI::RHIBuffer> m_records;
    std::shared_ptr<Render::RHI::RHIBuffer> m_counts;
    uint32_t m_emitterCount = 0;
};
}
