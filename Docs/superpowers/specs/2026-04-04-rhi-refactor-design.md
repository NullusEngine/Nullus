# RHI重构设计方案

**日期**: 2026-04-04
**状态**: 设计完成，待评审
**目标**: 提升跨平台一致性，统一抽象层

---

## 1. 概述

### 1.1 重构背景

当前项目的RHI（Render Hardware Interface）存在以下问题：
- Legacy `IRenderDevice`与Formal `RHIDevice`双重维护
- 不同后端（DX12/Vulkan/DX11/OpenGL）的行为和语义不一致
- 资源管理、绑定模型、同步机制在不同后端实现差异大
- 技术债累积，影响新功能开发

### 1.2 重构目标

参考UE（Unreal Engine）的RHI设计，建立统一的RHI抽象层，确保：
- **资源管理统一**: Texture/Buffer创建、状态追踪、生命周期管理跨平台一致
- **绑定模型统一**: Descriptor/Binding Layout语义在所有后端一致
- **同步机制统一**: Barrier/Fence/Semaphore的语义和执行顺序跨平台一致

### 1.3 重构策略

采用**大爆炸式重构**，一次性完成架构升级：
- 建立完善的RHICommandList统一命令抽象
- 消除Legacy IRenderDevice，仅保留Formal RHI
- 所有后端实现统一的`RHICommandListExecutor`接口

---

## 2. 核心架构设计

### 2.1 RHICommandList 统一命令模型

#### 2.1.1 设计原则

参考UE的`FRHICommandList`，将所有GPU命令统一到单一抽象：
- **Graphics Commands**: Draw, DrawIndexed, DrawInstanced, DrawIndexedInstanced
- **Compute Commands**: Dispatch, DispatchIndirect
- **Copy Commands**: CopyBuffer, CopyBufferToTexture, CopyTexture
- **State Commands**: SetViewport, SetScissor, SetStencilRef, SetBlendFactor
- **Binding Commands**: BindBindingSet, PushConstants
- **Synchronization Commands**: Barrier, UAVBarrier, AliasBarrier

#### 2.1.2 接口定义

```cpp
// Runtime/Rendering/RHI/Core/RHICommandList.h

class RHICommandList {
public:
    // 命令列表状态
    enum class State { Initial, Recording, Executing };

    State GetState() const { return state_; }

    // === Graphics Commands ===
    void Draw(const DrawArguments& args);
    void DrawIndexed(const DrawIndexedArguments& args);
    void DrawInstanced(const DrawInstancedArguments& args);
    void DrawIndexedInstanced(const DrawIndexedInstancedArguments& args);

    // === Compute Commands ===
    void Dispatch(const DispatchArguments& args);
    void DispatchIndirect(BufferHandle indirectBuffer, uint64 offset);

    // === Copy Commands ===
    void CopyBuffer(CopyBufferArguments args);
    void CopyBufferToTexture(CopyBufferToTextureArguments args);
    void CopyTexture(CopyTextureArguments args);

    // === State Commands ===
    void SetViewport(const Viewport& viewport);
    void SetScissor(uint32 left, uint32 top, uint32 right, uint32 bottom);
    void SetStencilRef(uint32 stencilRef);
    void SetBlendFactor(const float blendFactor[4]);

    // === Binding Commands ===
    void BindGraphicsPipeline(RHIGraphicsPipelineHandle pipeline);
    void BindComputePipeline(RHIComputePipelineHandle pipeline);
    void BindBindingSet(uint32 setIndex, RHIBindingSetHandle bindingSet);
    void PushConstants(ShaderStageMask stages, uint32 offset, uint32 size, const void* data);

    // === Vertex/Index Buffer ===
    void BindVertexBuffer(uint32 slot, RHIBufferHandle buffer, uint64 offset);
    void BindIndexBuffer(RHIBufferHandle buffer, uint64 offset, IndexType indexType);

    // === Synchronization Commands ===
    void Barrier(const RHIBarrier& barrier);
    void Barrier(const RHIBarrier* barriers, uint32 count);
    void UAVBarrier(RHIResourceHandle resource);
    void AliasBarrier(RHIResourceHandle before, RHIResourceHandle after);

    // === Render Pass ===
    void BeginRenderPass(const RenderPassDesc& desc);
    void EndRenderPass();

private:
    State state_ = State::Initial;
    CommandListData* data_ = nullptr;
};
```

#### 2.1.3 命令执行器接口

```cpp
// Runtime/Rendering/RHI/Core/RHICommandListExecutor.h

class IRHICommandListExecutor {
public:
    virtual ~IRHICommandListExecutor() = default;

    // 准备执行（重置命令列表）
    virtual void Reset(RHICommandList* cmdList) = 0;

    // 开始录制
    virtual void BeginRecording(RHICommandList* cmdList) = 0;

    // 结束录制
    virtual void EndRecording(RHICommandList* cmdList) = 0;

    // 执行命令列表
    virtual void Execute(RHICommandList* cmdList) = 0;

    // 后端特定信息
    virtual const char* GetBackendName() const = 0;
};
```

### 2.2 统一资源管理

#### 2.2.1 统一资源描述符

```cpp
// Runtime/Rendering/RHI/Core/RHIResource.h

enum class EResourceType { Buffer, Texture1D, Texture2D, Texture3D, TextureCube };

enum class EResourceFlags {
    None              = 0,
    Shared            = 1 << 0,   // 跨Adapter共享
    Sparse            = 1 << 1,   // 稀疏资源
    Transient         = 1 << 2,   // 临时资源（显存可回收）
    RenderTargetable  = 1 << 3,
    DepthStencilable  = 1 << 4,
    UnorderedAccess   = 1 << 5,
};

struct RHIResourceDesc {
    EResourceType type;
    uint64 size;                   // Buffer大小或Texture字节大小
    uint32 width = 1;
    uint32 height = 1;
    uint32 depth = 1;
    uint32 arraySize = 1;
    uint32 mipLevels = 1;
    EResourceFlags flags = EResourceFlags::None;
    ResourceState initialState = ResourceState::Common;
    EPixelFormat format = EPixelFormat::Unknown;
};
```

#### 2.2.2 统一资源接口

```cpp
class IRHIResource : public RHIObject {
public:
    virtual RHIResourceDesc GetDesc() const = 0;
    virtual ResourceState GetState() const = 0;
    virtual void SetState(ResourceState state) = 0;
    virtual NativeHandle GetNativeHandle() const = 0;
    virtual bool IsValid() const = 0;
};

class IRHIBuffer : public IRHIResource {
public:
    virtual uint64 GetSize() const = 0;
    virtual uint64 GetGPUAddress() const = 0;
    virtual void* Map() = 0;
    virtual void Unmap() = 0;
};

class IRHITexture : public IRHIResource {
public:
    virtual uint32 GetWidth() const = 0;
    virtual uint32 GetHeight() const = 0;
    virtual uint32 GetDepth() const = 0;
    virtual uint32 GetMipLevels() const = 0;
    virtual EPixelFormat GetFormat() const = 0;
    virtual IRHITextureView* CreateView(const TextureViewDesc& desc) = 0;
};
```

#### 2.2.3 资源状态追踪

```cpp
// Runtime/Rendering/RHI/Core/ResourceStateTracker.h

enum class ResourceState {
    Common             = 0,
    RenderTarget       = 0x1,
    DepthStencil       = 0x2,
    DepthStencilRead   = 0x4,
    ShaderResource     = 0x8,
    UnorderedAccess    = 0x10,
    CopyDest           = 0x20,
    CopySource         = 0x40,
    ResolveDest        = 0x80,
    ResolveSource      = 0x100,
    Present            = 0x200,
    IndirectArgument   = 0x400,
    Predication        = 0x800,
};

class ResourceStateTracker {
public:
    // 记录状态转换
    void RecordTransition(IRHIResource* resource, ResourceState before, ResourceState after);

    // 批量获取需要插入的barriers
    void GetPendingBarriers(std::vector<RHIBarrier>& outBarriers);

    // 重置追踪状态（新的一帧开始）
    void Reset();

private:
    std::unordered_map<IRHIResource*, ResourceState> resourceStates_;
    std::vector<RHIBarrier> pendingBarriers_;
};
```

### 2.3 统一绑定模型

#### 2.3.1 Binding Layout

```cpp
// Runtime/Rendering/RHI/Core/RHIBinding.h

enum class EDescriptorType {
    Sampler,
    Texture,
    RWTexture,
    UniformBuffer,
    RWBuffer,
    AccelerationStructure,
};

struct DescriptorSlot {
    uint32_t set;
    uint32_t binding;
    EDescriptorType type;
    ShaderStageMask visibility;
};

class IRHIBindingLayout : public RHIObject {
public:
    virtual const std::vector<DescriptorSlot>& GetSlots() const = 0;
    virtual ShaderStageMask GetVisibilityMask() const = 0;
    virtual uint32 GetHash() const = 0;
};

// 固定4-set布局常量
namespace RHIBindingSets {
    constexpr uint32 Frame  = 0;   // Frame/Scene级别资源
    constexpr uint32 Material = 1;   // Material级别资源
    constexpr uint32 Object  = 2;   // Per-object资源
    constexpr uint32 Pass     = 3;   // Pass/Rare资源
}
```

#### 2.3.2 Binding Set

```cpp
class IRHIBindingSet : public RHIObject {
public:
    virtual void SetBuffer(uint32 binding, IRHIBuffer* buffer) = 0;
    virtual void SetTexture(uint32 binding, IRHITexture* texture) = 0;
    virtual void SetRWTexture(uint32 binding, IRHITexture* texture) = 0;
    virtual void SetSampler(uint32 binding, RHISampler* sampler) = 0;
    virtual void SetUniformBuffer(uint32 binding, IRHIBuffer* buffer, uint64 offset, uint64 size) = 0;
    virtual void SetRWBuffer(uint32 binding, IRHIBuffer* buffer) = 0;

    // 标记为dirty，触发重新绑定
    virtual void MarkDirty() = 0;
    virtual bool IsDirty() const = 0;
};
```

### 2.4 统一同步机制

#### 2.4.1 Barrier 描述符

```cpp
// Runtime/Rendering/RHI/Core/RHISync.h

struct RHIBarrier {
    enum class Type { Transition, UAV, Alias };

    Type type;

    // Transition barrier
    IRHIResource* resource = nullptr;
    ResourceState before = ResourceState::Common;
    ResourceState after = ResourceState::Common;

    // UAV barrier
    IRHIResource* uavResource = nullptr;

    // Alias barrier
    IRHIResource* beforeResource = nullptr;
    IRHIResource* afterResource = nullptr;
};

// Barrier 辅助函数
inline RHIBarrier TransitionBarrier(IRHIResource* resource, ResourceState before, ResourceState after) {
    return RHIBarrier{ RHIBarrier::Type::Transition, resource, before, after };
}

inline RHIBarrier UAVBarrier(IRHIResource* resource) {
    return RHIBarrier{ RHIBarrier::Type::UAV, nullptr, ResourceState::Common, ResourceState::Common, resource };
}
```

#### 2.4.2 Fence 和 Semaphore

```cpp
class IRHIFence : public RHIObject {
public:
    virtual bool IsSignaled() const = 0;
    virtual void Reset() = 0;
    virtual void Wait(uint64 timeoutMs = UINT64_MAX) = 0;
};

class IRHISemaphore : public RHIObject {
public:
    virtual bool IsSignaled() const = 0;
    virtual void Reset() = 0;
    // 用于Queue.submit的信号量链
    virtual void* GetNativeSemaphoreHandle() const = 0;
};
```

---

## 3. 后端实现架构

### 3.1 Executor 工厂

```cpp
// Runtime/Rendering/RHI/Backends/RHIExecutorFactory.h

enum class ERHIBackend { DX12, Vulkan, DX11, OpenGL, Metal, Null };

class RHIDevice {
public:
    // 创建指定后端的Executor
    static std::unique_ptr<IRHICommandListExecutor> CreateExecutor(ERHIBackend backend);

    // 原有工厂方法保持兼容
    static std::unique_ptr<RHIDevice> CreateDX12Device();
    static std::unique_ptr<RHIDevice> CreateVulkanDevice();
    // ...
};
```

### 3.2 DX12 Executor 实现

```cpp
// Runtime/Rendering/RHI/Backends/DX12/DX12CommandListExecutor.h

class DX12CommandListExecutor : public IRHICommandListExecutor {
public:
    // IRHICommandListExecutor 接口实现
    void Reset(RHICommandList* cmdList) override;
    void BeginRecording(RHICommandList* cmdList) override;
    void EndRecording(RHICommandList* cmdList) override;
    void Execute(RHICommandList* cmdList) override;
    const char* GetBackendName() const override { return "DX12"; }

private:
    void ExecuteDraw(RHICommandList* cmdList);
    void ExecuteCompute(RHICommandList* cmdList);
    void ExecuteCopy(RHICommandList* cmdList);
    void ExecuteBarriers(RHICommandList* cmdList);

    Microsoft::WRL::ComPtr<ID3D12CommandList> nativeCmdList_;
    std::vector<D3D12_RESOURCE_BARRIER> dx12Barriers_;
};
```

### 3.3 Vulkan Executor 实现

```cpp
// Runtime/Rendering/RHI/Backends/Vulkan/VulkanCommandListExecutor.h

class VulkanCommandListExecutor : public IRHICommandListExecutor {
public:
    // IRHICommandListExecutor 接口实现
    void Reset(RHICommandList* cmdList) override;
    void BeginRecording(RHICommandList* cmdList) override;
    void EndRecording(RHICommandList* cmdList) override;
    void Execute(RHICommandList* cmdList) override;
    const char* GetBackendName() const override { return "Vulkan"; }

private:
    void ExecuteDraw(RHICommandList* cmdList);
    void ExecuteCompute(RHICommandList* cmdList);
    void ExecuteCopy(RHICommandList* cmdList);
    void ExecuteBarriers(RHICommandList* cmdList);

    VkCommandBuffer vkCmdBuffer_;
    std::vector<VkBufferMemoryBarrier> vkBufferBarriers_;
    std::vector<VkImageMemoryBarrier> vkImageBarriers_;
};
```

### 3.4 Tier B 后端适配器

```cpp
// Runtime/Rendering/RHI/Backends/DX11/DX11CommandListExecutor.h

class DX11CommandListExecutor : public IRHICommandListExecutor {
public:
    // DX11/OpenGL不支持显式barrier，通过适配器模拟
    void ExecuteBarriers(RHICommandList* cmdList) override {
        // DX11: 隐式状态同步，barrier调用变为no-op但记录状态转换
        // 用于状态一致性验证
    }
};

// OpenGL类似实现
class OpenGLCommandListExecutor : public IRHICommandListExecutor {
    // GL命令录制和回放
};
```

---

## 4. 架构组件关系

```
                        ┌──────────────────────────────────────┐
                        │            RHIDevice                │
                        │  - CreateBuffer/CreateTexture       │
                        │  - CreateBindingLayout              │
                        │  - CreateSwapchain                  │
                        │  - CreateCommandListExecutor        │
                        └──────────────────┬───────────────────┘
                                           │
                    ┌──────────────────────┼──────────────────────┐
                    │                      │                      │
                    ▼                      ▼                      ▼
        ┌───────────────────┐   ┌───────────────────┐   ┌───────────────────┐
        │   RHISwapchain    │   │  RHICommandList    │   │   IRHIResource    │
        │   (Present)       │   │  (录制接口)         │   │  (Buffer/Texture) │
        └───────────────────┘   └─────────┬─────────┘   └───────────────────┘
                                          │
                               ┌──────────▼──────────┐
                               │ IRHICommandList    │
                               │ Executor           │
                               │ (实际执行)          │
                               └──────────┬──────────┘
                                          │
        ┌─────────────────────────────────┼─────────────────────────────────┐
        │                                 │                                 │
        ▼                                 ▼                                 ▼
┌───────────────┐               ┌─────────────────┐               ┌─────────────────┐
│ DX12 Executor │               │ Vulkan Executor │               │ DX11/GL 适配器  │
│ - D3D12 CmdList│               │ - VkCmdBuffer   │               │ - 模拟barrier   │
│ - D3D12 Barrier│               │ - VkBarrier     │               │ - 状态追踪      │
└───────────────┘               └─────────────────┘               └─────────────────┘
```

---

## 5. 迁移计划

### Phase 1: 核心抽象建立（4周）

**目标**: 建立新的RHICommandList和Executor接口

**任务**:
1. 新增 `RHICommandList.h/cpp` - 命令列表抽象
2. 新增 `RHICommandListExecutor.h` - 执行器接口
3. 新增 `IRHIResource.h` - 统一资源接口
4. 新增 `RHIBarrier.h` - 统一同步描述符
5. 更新 `RHIDevice.h` - 添加CreateExecutor工厂方法

**交付物**: 新的抽象接口，不影响现有功能

### Phase 2: DX12 Executor 实现（3周）

**目标**: 在DX12后端实现完整的Executor

**任务**:
1. 实现 `DX12CommandListExecutor`
2. 实现命令录制和回放逻辑
3. 实现barrier翻译
4. 实现资源状态追踪

**验证**: 使用RenderDoc验证DX12下所有命令正确执行

### Phase 3: Vulkan Executor 实现（3周）

**目标**: 在Vulkan后端实现完整的Executor

**任务**:
1. 实现 `VulkanCommandListExecutor`
2. 命令录制和回放
3. barrier翻译
4. 资源状态追踪

**验证**: 与DX12对比，确保行为一致

### Phase 4: Tier B 后端适配器（2周）

**目标**: 为DX11/OpenGL实现适配器

**任务**:
1. 实现 `DX11CommandListExecutor`
2. 实现 `OpenGLCommandListExecutor`
3. 模拟barrier语义以保持API一致性

### Phase 5: 集成和迁移（4周）

**目标**: 将FrameGraph、Material系统迁移到新RHI

**任务**:
1. 更新 `FrameGraphExecutionContext` 使用RHICommandList
2. 更新 `Driver` 类，移除Legacy设备
3. 迁移 `Material` 系统到新绑定模型
4. 迁移渲染Pass到新命令接口

### Phase 6: 验证和优化（2周）

**目标**: 确保所有后端行为一致，性能达标

**任务**:
1. RenderDoc多后端对比验证
2. 性能测试和优化
3. 文档更新

---

## 6. 风险和缓解

### 风险1: 大爆炸式重构导致回归问题
**缓解**: 每个Phase结束时进行集成测试，确保功能不退步

### 风险2: Tier B后端适配器性能损失
**缓解**: 适配器层尽可能轻量，对于不支持的特性在API层面禁止而非模拟

### 风险3: 迁移周期过长影响其他开发
**缓解**: Phase 1-3期间，其他开发可在Legacy路径继续；Phase 5开始逐步切换

---

## 7. 待明确事项

- [ ] 是否需要保留对Metal后端的完整支持（当前只有scaffold）
- [ ] 具体的性能基准测试指标（当前帧率 vs 目标帧率）
- [ ] 是否需要支持DXR（RayTracing）扩展

---

## 8. 参考资料

- Unreal Engine 5 RHI源码 (`GpuCore.cpp`, `RHICommandList.cpp`)
- Vulkan Specification - Synchronization
- DirectX 12 Specification - Resource Barriers
