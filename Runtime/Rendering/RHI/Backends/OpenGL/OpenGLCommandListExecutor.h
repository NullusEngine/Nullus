// Runtime/Rendering/RHI/Backends/OpenGL/OpenGLCommandListExecutor.h
#pragma once
#include "Rendering/RHI/Core/RHICommandListExecutor.h"
#include "Rendering/RHI/Core/RHICommandList.h"

namespace NLS::Render::RHI::OpenGL {

class OpenGLCommandListExecutor : public IRHICommandListExecutor {
public:
    OpenGLCommandListExecutor();
    virtual ~OpenGLCommandListExecutor();

    // IRHICommandListExecutor
    void Reset(RHICommandList* cmdList) override;
    void BeginRecording(RHICommandList* cmdList) override;
    void EndRecording(RHICommandList* cmdList) override;
    void Execute(RHICommandList* cmdList) override;
    const char* GetBackendName() const override { return "OpenGL"; }
    bool SupportsExplicitBarriers() const override { return false; } // OpenGL uses implicit synchronization

private:
    void ExecuteDrawCommands(RHICommandList* cmdList);
    void ExecuteComputeCommands(RHICommandList* cmdList);
    void ExecuteCopyCommands(RHICommandList* cmdList);
    void ExecuteRenderPass(RHICommandList* cmdList);
    // Note: No ExecuteBarriers - OpenGL doesn't support explicit barriers

    // Record barrier state for potential debugging/logging purposes
    // even though they won't be executed as explicit barriers
    void RecordBarrierState(const BarrierDesc& barrier);

    RHICommandList* currentCommandList_ = nullptr;

    // Track barriers for state debugging (OpenGL implicit sync makes this useful for diagnostics)
    bool hasRecordedBarriers_ = false;
};

} // namespace NLS::Render::RHI::OpenGL