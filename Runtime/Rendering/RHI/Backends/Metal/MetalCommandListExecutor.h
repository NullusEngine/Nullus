// Runtime/Rendering/RHI/Backends/Metal/MetalCommandListExecutor.h
#pragma once
#include "Rendering/RHI/Core/RHICommandListExecutor.h"
#include "Rendering/RHI/Core/RHICommandList.h"

#if defined(__APPLE__)
#import <Metal/Metal.h>
#import <Metal/MTLCommandQueue.hpp>
#endif

namespace NLS::Render::RHI::Metal {

class MetalCommandListExecutor : public IRHICommandListExecutor {
public:
    MetalCommandListExecutor();
    virtual ~MetalCommandListExecutor() override;

    // IRHICommandListExecutor
    void Reset(RHICommandList* cmdList) override;
    void BeginRecording(RHICommandList* cmdList) override;
    void EndRecording(RHICommandList* cmdList) override;
    void Execute(RHICommandList* cmdList) override;
    const char* GetBackendName() const override { return "Metal"; }
    bool SupportsExplicitBarriers() const override { return false; } // Metal uses implicit synchronization

private:
    void ExecuteDrawCommands(RHICommandList* cmdList);
    void ExecuteComputeCommands(RHICommandList* cmdList);
    void ExecuteCopyCommands(RHICommandList* cmdList);
    void ExecuteRenderPass(RHICommandList* cmdList);
    // Note: No ExecuteBarriers - Metal doesn't support explicit barriers

    // Record barrier state for potential debugging/logging purposes
    // even though they won't be executed as explicit barriers
    void RecordBarrierState(const BarrierDesc& barrier);

    RHICommandList* currentCommandList_ = nullptr;

    // Track barriers for state debugging (Metal implicit sync makes this useful for diagnostics)
    bool hasRecordedBarriers_ = false;
};

} // namespace NLS::Render::RHI::Metal