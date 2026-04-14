// Runtime/Rendering/RHI/Core/RHICommandListExecutor.h
#pragma once
#include "Rendering/RHI/Core/RHICommon.h"

namespace NLS::Render::RHI {

// 前向声明
class RHICommandList;

// 执行器接口 - 所有后端实现此接口
class NLS_RENDER_API IRHICommandListExecutor {
public:
    virtual ~IRHICommandListExecutor() = default;

    // 重置命令列表
    virtual void Reset(RHICommandList* cmdList) = 0;

    // 开始录制
    virtual void BeginRecording(RHICommandList* cmdList) = 0;

    // 结束录制
    virtual void EndRecording(RHICommandList* cmdList) = 0;

    // 执行命令列表
    virtual void Execute(RHICommandList* cmdList) = 0;

    // 后端信息
    virtual const char* GetBackendName() const = 0;

    // 检查是否支持显式barrier
    virtual bool SupportsExplicitBarriers() const = 0;
};

// Executor工厂
enum class ERHIBackend { DX12, Vulkan, DX11, OpenGL, Metal, Null };

NLS_RENDER_API std::unique_ptr<IRHICommandListExecutor> CreateCommandListExecutor(ERHIBackend backend, const NativeRenderDeviceInfo& nativeInfo);

} // namespace NLS::Render::RHI