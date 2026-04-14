// Runtime/Rendering/RHI/Backends/DX12/DX12CommandListExecutor.h
#pragma once
#include "Rendering/RHI/Core/RHICommandListExecutor.h"
#include "Rendering/RHI/Core/RHICommandList.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace NLS::Render::RHI::DX12 {

class DX12CommandListExecutor : public IRHICommandListExecutor {
public:
    DX12CommandListExecutor(Microsoft::WRL::ComPtr<ID3D12Device> device,
                            Microsoft::WRL::ComPtr<ID3D12CommandQueue> commandQueue);
    virtual ~DX12CommandListExecutor();

    // IRHICommandListExecutor
    void Reset(RHICommandList* cmdList) override;
    void BeginRecording(RHICommandList* cmdList) override;
    void EndRecording(RHICommandList* cmdList) override;
    void Execute(RHICommandList* cmdList) override;
    const char* GetBackendName() const override { return "DX12"; }
    bool SupportsExplicitBarriers() const override { return true; }

private:
    void ExecuteDrawCommands(RHICommandList* cmdList);
    void ExecuteComputeCommands(RHICommandList* cmdList);
    void ExecuteCopyCommands(RHICommandList* cmdList);
    void ExecuteBarriers(RHICommandList* cmdList);
    void ExecuteRenderPass(RHICommandList* cmdList);
    void TranslateBarrier(const BarrierDesc& barrier);

    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> commandQueue_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> currentCmdList_;
    std::vector<D3D12_RESOURCE_BARRIER> pendingBarriers_;
    RHICommandList* currentCommandList_ = nullptr;
};

} // namespace NLS::Render::RHI::DX12