#pragma once

#include "GpuEmitter.h"
#include "SystemInstance.h"

namespace NLS::VFX
{
// Render-owned simulation state. Each prepared world frame receives its own GPU copy,
// so later simulation cannot overwrite inputs still referenced by an earlier view/frame.
class NLS_VFX_API SystemRenderProxy final
{
public:
    explicit SystemRenderProxy(std::shared_ptr<const CompiledSystem> system);
    ~SystemRenderProxy();
    SystemRenderProxy(const SystemRenderProxy&) = delete;
    SystemRenderProxy& operator=(const SystemRenderProxy&) = delete;
    [[nodiscard]] std::shared_ptr<const SystemRenderFrame> Prepare(
        std::shared_ptr<Render::RHI::RHIDevice> device, const std::shared_ptr<const SystemFrame>& frame, std::string& error);
    [[nodiscard]] const std::shared_ptr<const CompiledSystem>& Program() const { return m_system; }

private:
    struct DeviceState;
    std::shared_ptr<const CompiledSystem> m_system;
    std::unordered_map<uint64_t,std::unique_ptr<DeviceState>> m_devices;
};
}
