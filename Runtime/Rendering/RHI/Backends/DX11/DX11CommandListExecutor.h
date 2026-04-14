// Runtime/Rendering/RHI/Backends/DX11/DX11CommandListExecutor.h
#pragma once
#include "Rendering/RHI/Core/RHICommandListExecutor.h"
#include "Rendering/RHI/Core/RHICommandList.h"
#include <d3d11.h>
#include <wrl/client.h>

namespace NLS::Render::RHI::DX11 {

class DX11CommandListExecutor : public IRHICommandListExecutor {
public:
    DX11CommandListExecutor(ID3D11Device* device, ID3D11DeviceContext* context);
    virtual ~DX11CommandListExecutor();

    // IRHICommandListExecutor
    void Reset(RHICommandList* cmdList) override;
    void BeginRecording(RHICommandList* cmdList) override;
    void EndRecording(RHICommandList* cmdList) override;
    void Execute(RHICommandList* cmdList) override;
    const char* GetBackendName() const override { return "DX11"; }
    bool SupportsExplicitBarriers() const override { return false; } // DX11 uses implicit synchronization

private:
    void ExecuteDrawCommands(RHICommandList* cmdList);
    void ExecuteComputeCommands(RHICommandList* cmdList);
    void ExecuteCopyCommands(RHICommandList* cmdList);
    void ExecuteRenderPass(RHICommandList* cmdList);
    // Note: No ExecuteBarriers - DX11 doesn't support explicit barriers

    // Record barrier state for potential debugging/logging purposes
    // even though they won't be executed as explicit barriers
    void RecordBarrierState(const BarrierDesc& barrier);

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    RHICommandList* currentCommandList_ = nullptr;

    // Track barriers for state debugging (DX11 implicit sync makes this useful for diagnostics)
    bool hasRecordedBarriers_ = false;
};

} // namespace NLS::Render::RHI::DX11