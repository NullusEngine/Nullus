// Runtime/Rendering/RHI/Core/RHICommandList.h
#pragma once

#include "Rendering/RHI/Core/RHICommand.h"
#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/RHI/Core/RHIPipeline.h"
#include "Rendering/RHI/Core/RHIBinding.h"
#include "Rendering/RHI/Core/IRHIResource.h"

namespace NLS::Render::RHI
{

// 命令参数结构体
struct DrawArguments
{
    uint32_t vertexCount = 0;
    uint32_t instanceCount = 1;
    uint32_t firstVertex = 0;
    uint32_t firstInstance = 0;
};

struct DrawIndexedArguments
{
    uint32_t indexCount = 0;
    uint32_t instanceCount = 1;
    uint32_t firstIndex = 0;
    int32_t vertexOffset = 0;
    uint32_t firstInstance = 0;
};

struct DrawInstancedArguments
{
    uint32_t vertexCount = 0;
    uint32_t instanceCount = 1;
    uint32_t firstVertex = 0;
    uint32_t firstInstance = 0;
};

struct DrawIndexedInstancedArguments
{
    uint32_t indexCount = 0;
    uint32_t instanceCount = 1;
    uint32_t firstIndex = 0;
    int32_t vertexOffset = 0;
    uint32_t firstInstance = 0;
};

struct DispatchArguments
{
    uint32_t threadGroupX = 0;
    uint32_t threadGroupY = 0;
    uint32_t threadGroupZ = 0;
};

struct CopyBufferArguments
{
    std::shared_ptr<RHIBuffer> source;
    std::shared_ptr<RHIBuffer> destination;
    RHIBufferCopyRegion region;
};

struct CopyBufferToTextureArguments
{
    RHIBufferToTextureCopyDesc desc;
};

struct CopyTextureArguments
{
    RHITextureCopyDesc desc;
};

// 简化的Viewport别名（使用已有的RHIViewport）
using Viewport = RHIViewport;

// 简化的RenderPassDesc别名（使用已有的RHIRenderPassDesc）
using RenderPassDesc = RHIRenderPassDesc;

// Barrier描述符
struct BarrierDesc
{
    enum class Type : uint8_t
    {
        Transition,
        UAV,
        Alias
    };

    Type type = Type::Transition;
    std::vector<RHIBufferBarrier> bufferBarriers;
    std::vector<RHITextureBarrier> textureBarriers;
};

// 命令列表接口
class NLS_RENDER_API RHICommandList
{
public:
    enum class State : uint8_t
    {
        Initial,
        Recording,
        Executing
    };

    virtual ~RHICommandList() = default;

    // 状态查询
    virtual State GetState() const = 0;

    // 渲染通道
    virtual void BeginRenderPass(const RenderPassDesc& desc) = 0;
    virtual void EndRenderPass() = 0;

    // 视口和裁剪
    virtual void SetViewport(const Viewport& viewport) = 0;
    virtual void SetScissor(const RHIRect2D& rect) = 0;

    // 管线绑定
    virtual void BindGraphicsPipeline(const std::shared_ptr<RHIGraphicsPipeline>& pipeline) = 0;
    virtual void BindComputePipeline(const std::shared_ptr<RHIComputePipeline>& pipeline) = 0;
    virtual void BindBindingSet(uint32_t setIndex, const std::shared_ptr<RHIBindingSet>& bindingSet) = 0;
    virtual void PushConstants(ShaderStageMask stageMask, uint32_t offset, uint32_t size, const void* data) = 0;

    // 顶点/索引缓冲区
    virtual void BindVertexBuffer(uint32_t slot, const RHIVertexBufferView& view) = 0;
    virtual void BindIndexBuffer(const RHIIndexBufferView& view) = 0;

    // 绘制命令
    virtual void Draw(const DrawArguments& args) = 0;
    virtual void DrawIndexed(const DrawIndexedArguments& args) = 0;
    virtual void DrawInstanced(const DrawInstancedArguments& args) = 0;
    virtual void DrawIndexedInstanced(const DrawIndexedInstancedArguments& args) = 0;

    // 计算命令
    virtual void Dispatch(const DispatchArguments& args) = 0;
    virtual void DispatchIndirect(RHIBuffer* indirectBuffer, uint64_t offset = 0) = 0;

    // 状态设置
    virtual void SetStencilRef(uint32_t stencilRef) = 0;
    virtual void SetBlendFactor(const float blendFactor[4]) = 0;

    // 同步命令
    virtual void Barrier(const BarrierDesc& barrier) = 0;
    virtual void UAVBarrier(IRHIResource* resource) = 0;
    virtual void AliasBarrier(IRHIResource* before, IRHIResource* after) = 0;

    // 复制命令
    virtual void CopyBuffer(const CopyBufferArguments& args) = 0;
    virtual void CopyBufferToTexture(const CopyBufferToTextureArguments& args) = 0;
    virtual void CopyTexture(const CopyTextureArguments& args) = 0;

protected:
    State state_ = State::Initial;
};

// 命令基类和具体命令类型
class NLS_RENDER_API Command
{
public:
    virtual ~Command() = default;
    enum class Type : uint8_t
    {
        BeginRenderPass,
        EndRenderPass,
        SetViewport,
        SetScissor,
        BindGraphicsPipeline,
        BindComputePipeline,
        BindBindingSet,
        PushConstants,
        BindVertexBuffer,
        BindIndexBuffer,
        Draw,
        DrawIndexed,
        DrawInstanced,
        DrawIndexedInstanced,
        Dispatch,
        DispatchIndirect,
        SetStencilRef,
        SetBlendFactor,
        Barrier,
        UAVBarrier,
        AliasBarrier,
        CopyBuffer,
        CopyBufferToTexture,
        CopyTexture
    };

    explicit Command(Type type) : type(type) {}
    Type type;
};

struct BeginRenderPassCmd : Command { BeginRenderPassCmd(); RenderPassDesc desc; };
struct EndRenderPassCmd : Command { EndRenderPassCmd(); };
struct SetViewportCmd : Command { SetViewportCmd(); Viewport viewport; };
struct SetScissorCmd : Command { SetScissorCmd(); RHIRect2D rect; };
struct BindGraphicsPipelineCmd : Command { BindGraphicsPipelineCmd(); std::shared_ptr<RHIGraphicsPipeline> pipeline; };
struct BindComputePipelineCmd : Command { BindComputePipelineCmd(); std::shared_ptr<RHIComputePipeline> pipeline; };
struct BindBindingSetCmd : Command { BindBindingSetCmd(); uint32_t setIndex = 0; std::shared_ptr<RHIBindingSet> bindingSet; };
struct PushConstantsCmd : Command { PushConstantsCmd(); ShaderStageMask stageMask = ShaderStageMask::None; uint32_t offset = 0; uint32_t size = 0; std::vector<uint8_t> data; };
struct BindVertexBufferCmd : Command { BindVertexBufferCmd(); uint32_t slot = 0; RHIVertexBufferView view; };
struct BindIndexBufferCmd : Command { BindIndexBufferCmd(); RHIIndexBufferView view; };
struct DrawCmd : Command { DrawCmd(); DrawArguments args; };
struct DrawIndexedCmd : Command { DrawIndexedCmd(); DrawIndexedArguments args; };
struct DrawInstancedCmd : Command { DrawInstancedCmd(); DrawInstancedArguments args; };
struct DrawIndexedInstancedCmd : Command { DrawIndexedInstancedCmd(); DrawIndexedInstancedArguments args; };
struct DispatchCmd : Command { DispatchCmd(); DispatchArguments args; };
struct DispatchIndirectCmd : Command { DispatchIndirectCmd(); RHIBuffer* indirectBuffer = nullptr; uint64_t offset = 0; };
struct SetStencilRefCmd : Command { SetStencilRefCmd(); uint32_t stencilRef = 0; };
struct SetBlendFactorCmd : Command { SetBlendFactorCmd(); float blendFactor[4] = {1.0f, 1.0f, 1.0f, 1.0f}; };
struct UAVBarrierCmd : Command { UAVBarrierCmd(); IRHIResource* resource = nullptr; };
struct AliasBarrierCmd : Command { AliasBarrierCmd(); IRHIResource* before = nullptr; IRHIResource* after = nullptr; };
struct CopyBufferCmd : Command { CopyBufferCmd(); CopyBufferArguments args; };
struct CopyBufferToTextureCmd : Command { CopyBufferToTextureCmd(); CopyBufferToTextureArguments args; };
struct CopyTextureCmd : Command { CopyTextureCmd(); CopyTextureArguments args; };
struct BarrierCmd : Command { BarrierCmd(); BarrierDesc barrier; };

// 默认命令列表实现 - 将命令录制到向量中
class NLS_RENDER_API DefaultRHICommandList : public RHICommandList
{
public:
    DefaultRHICommandList() = default;

    // RHICommandList 接口实现
    State GetState() const override { return state_; }

    void BeginRenderPass(const RenderPassDesc& desc) override;
    void EndRenderPass() override;

    void SetViewport(const Viewport& viewport) override;
    void SetScissor(const RHIRect2D& rect) override;

    void BindGraphicsPipeline(const std::shared_ptr<RHIGraphicsPipeline>& pipeline) override;
    void BindComputePipeline(const std::shared_ptr<RHIComputePipeline>& pipeline) override;
    void BindBindingSet(uint32_t setIndex, const std::shared_ptr<RHIBindingSet>& bindingSet) override;
    void PushConstants(ShaderStageMask stageMask, uint32_t offset, uint32_t size, const void* data) override;

    void BindVertexBuffer(uint32_t slot, const RHIVertexBufferView& view) override;
    void BindIndexBuffer(const RHIIndexBufferView& view) override;

    void Draw(const DrawArguments& args) override;
    void DrawIndexed(const DrawIndexedArguments& args) override;
    void DrawInstanced(const DrawInstancedArguments& args) override;
    void DrawIndexedInstanced(const DrawIndexedInstancedArguments& args) override;

    void Dispatch(const DispatchArguments& args) override;
    void DispatchIndirect(RHIBuffer* indirectBuffer, uint64_t offset = 0) override;

    void SetStencilRef(uint32_t stencilRef) override;
    void SetBlendFactor(const float blendFactor[4]) override;

    void Barrier(const BarrierDesc& barrier) override;
    void UAVBarrier(IRHIResource* resource) override;
    void AliasBarrier(IRHIResource* before, IRHIResource* after) override;

    void CopyBuffer(const CopyBufferArguments& args) override;
    void CopyBufferToTexture(const CopyBufferToTextureArguments& args) override;
    void CopyTexture(const CopyTextureArguments& args) override;

    // 访问录制的命令
    const std::vector<std::shared_ptr<Command>>& GetCommands() const { return commands_; }
    void ClearCommands() { commands_.clear(); }

private:
    std::vector<std::shared_ptr<Command>> commands_;
};

} // namespace NLS::Render::RHI