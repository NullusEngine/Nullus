# RHI重构实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 建立统一的RHICommandList抽象层，消除跨平台差异，提升一致性

**Architecture:** 参考UE的FRHICommandList模式，建立RHICommandList作为统一命令抽象，IRHICommandListExecutor作为后端执行接口。Phase 1建立核心抽象，Phase 2-4实现各后端，Phase 5完成集成迁移。

**Tech Stack:** C++17, DirectX 12, Vulkan, DX11, OpenGL, CMake

---

## 文件结构概览

```
Runtime/Rendering/RHI/Core/
├── RHICommandList.h        [新建] 统一命令列表接口
├── RHICommandList.cpp      [新建] 默认实现
├── RHICommandListExecutor.h [新建] 执行器接口
├── IRHIResource.h          [新建] 统一资源基类
├── RHIBarrier.h            [新建] 统一barrier描述符
├── RHIResource.h           [修改] 添加IRHIResource继承
├── RHIDevice.h             [修改] 添加CreateExecutor工厂
├── RHISync.h               [修改] 添加RHIBarrier结构

Runtime/Rendering/RHI/Backends/DX12/
├── DX12CommandListExecutor.h [新建]
├── DX12CommandListExecutor.cpp [新建]

Runtime/Rendering/RHI/Backends/Vulkan/
├── VulkanCommandListExecutor.h [新建]
├── VulkanCommandListExecutor.cpp [新建]

Runtime/Rendering/RHI/Backends/DX11/
├── DX11CommandListExecutor.h [新建]
├── DX11CommandListExecutor.cpp [新建]

Runtime/Rendering/RHI/Backends/OpenGL/
├── OpenGLCommandListExecutor.h [新建]
├── OpenGLCommandListExecutor.cpp [新建]

Runtime/Rendering/Context/
├── Driver.h [修改] 集成RHICommandList
├── Driver.cpp [修改] 集成RHICommandList
```

---

## Phase 1: 核心抽象建立

### Task 1: 创建RHICommandListExecutor执行器接口

**Files:**
- Create: `Runtime/Rendering/RHI/Core/RHICommandListExecutor.h`

- [ ] **Step 1: 创建RHICommandListExecutor.h头文件**

```cpp
// Runtime/Rendering/RHI/Core/RHICommandListExecutor.h
#pragma once
#include "RHI/RHICommon.h"

namespace RHI {

// 前向声明
class RHICommandList;

// 执行器接口 - 所有后端实现此接口
class IRHICommandListExecutor {
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

RHI_API std::unique_ptr<IRHICommandListExecutor> CreateCommandListExecutor(ERHIBackend backend);

} // namespace RHI
```

- [ ] **Step 2: 验证头文件语法**

检查Runtime/Rendering/RHI/Core/目录是否存在

- [ ] **Step 3: 提交**

```bash
git add Runtime/Rendering/RHI/Core/RHICommandListExecutor.h
git commit -m "feat(RHI): add IRHICommandListExecutor interface

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

### Task 2: 创建RHICommandList统一命令接口

**Files:**
- Create: `Runtime/Rendering/RHI/Core/RHICommandList.h`
- Create: `Runtime/Rendering/RHI/Core/RHICommandList.cpp`

- [ ] **Step 1: 创建RHICommandList.h**

```cpp
// Runtime/Rendering/RHI/Core/RHICommandList.h
#pragma once
#include "RHI/RHICommon.h"
#include "RHI/RHIResource.h"
#include "RHI/RHISync.h"
#include "RHI/RHIPipeline.h"
#include "RHI/RHIBinding.h"

namespace RHI {

// 命令参数结构体
struct DrawArguments {
    uint32 vertexCount;
    uint32 instanceCount = 1;
    uint32 firstVertex = 0;
    uint32 firstInstance = 0;
};

struct DrawIndexedArguments {
    uint32 indexCount;
    uint32 instanceCount = 1;
    uint32 firstIndex = 0;
    int32 vertexOffset = 0;
    uint32 firstInstance = 0;
};

struct DrawInstancedArguments {
    uint32 vertexCount;
    uint32 instanceCount;
    uint32 firstVertex;
    uint32 firstInstance;
};

struct DrawIndexedInstancedArguments {
    uint32 indexCountPerInstance;
    uint32 instanceCount;
    uint32 startIndex;
    int32 baseVertex;
    uint32 startInstance;
};

struct DispatchArguments {
    uint32 threadGroupX;
    uint32 threadGroupY;
    uint32 threadGroupZ;
};

struct CopyBufferArguments {
    RHIBuffer* src;
    RHIBuffer* dst;
    uint64 srcOffset = 0;
    uint64 dstOffset = 0;
    uint64 size = 0; // 0 = entire buffer
};

struct CopyBufferToTextureArguments {
    RHIBuffer* src;
    RHITexture* dst;
    uint32 arraySlice = 0;
    uint32 mipLevel = 0;
};

struct CopyTextureArguments {
    RHITexture* src;
    RHITexture* dst;
    uint32 srcArraySlice = 0;
    uint32 dstArraySlice = 0;
    uint32 srcMipLevel = 0;
    uint32 dstMipLevel = 0;
};

struct Viewport {
    float x, y, width, height;
    float minDepth = 0.0f;
    float maxDepth = 1.0f;
};

struct RenderPassDesc {
    RHITexture* renderTarget = nullptr;
    RHITexture* depthStencil = nullptr;
    Color32f clearColor = {0, 0, 0, 1};
    float clearDepth = 1.0f;
    uint8 clearStencil = 0;
    bool hasDepthStencil = false;
};

// Barrier描述符
struct BarrierDesc {
    enum class Type { Transition, UAV, Alias };
    Type type;

    // Transition
    IRHIResource* resource = nullptr;
    ResourceState before = ResourceState::Common;
    ResourceState after = ResourceState::Common;

    // UAV
    IRHIResource* uavResource = nullptr;

    // Alias
    IRHIResource* beforeResource = nullptr;
    IRHIResource* afterResource = nullptr;
};

// 核心命令列表接口
class RHICommandList {
public:
    enum class State { Initial, Recording, Executing };

    virtual ~RHICommandList() = default;

    State GetState() const { return state_; }

    // === Render Pass ===
    virtual void BeginRenderPass(const RenderPassDesc& desc) = 0;
    virtual void EndRenderPass() = 0;

    // === Graphics Commands ===
    virtual void Draw(const DrawArguments& args) = 0;
    virtual void DrawIndexed(const DrawIndexedArguments& args) = 0;
    virtual void DrawInstanced(const DrawInstancedArguments& args) = 0;
    virtual void DrawIndexedInstanced(const DrawIndexedInstancedArguments& args) = 0;

    // === Compute Commands ===
    virtual void Dispatch(const DispatchArguments& args) = 0;
    virtual void DispatchIndirect(RHIBuffer* indirectBuffer, uint64 offset = 0) = 0;

    // === Copy Commands ===
    virtual void CopyBuffer(const CopyBufferArguments& args) = 0;
    virtual void CopyBufferToTexture(const CopyBufferToTextureArguments& args) = 0;
    virtual void CopyTexture(const CopyTextureArguments& args) = 0;

    // === State Commands ===
    virtual void SetViewport(const Viewport& viewport) = 0;
    virtual void SetScissor(uint32 left, uint32 top, uint32 right, uint32 bottom) = 0;
    virtual void SetStencilRef(uint32 stencilRef) = 0;
    virtual void SetBlendFactor(const float blendFactor[4]) = 0;

    // === Binding Commands ===
    virtual void BindGraphicsPipeline(RHIGraphicsPipeline* pipeline) = 0;
    virtual void BindComputePipeline(RHIComputePipeline* pipeline) = 0;
    virtual void BindBindingSet(uint32 setIndex, IRHIBindingSet* bindingSet) = 0;
    virtual void PushConstants(ShaderStageMask stages, uint32 offset, uint32 size, const void* data) = 0;

    // === Vertex/Index Buffer ===
    virtual void BindVertexBuffer(uint32 slot, IRHIBuffer* buffer, uint64 offset = 0) = 0;
    virtual void BindIndexBuffer(IRHIBuffer* buffer, uint64 offset = 0, IndexType indexType = IndexType::Uint16) = 0;

    // === Synchronization Commands ===
    virtual void Barrier(const BarrierDesc& barrier) = 0;
    virtual void UAVBarrier(IRHIResource* resource) = 0;
    virtual void AliasBarrier(IRHIResource* before, IRHIResource* after) = 0;

protected:
    State state_ = State::Initial;
};

// 默认命令列表实现 - 仅记录命令，实际执行由Executor完成
class DefaultRHICommandList : public RHICommandList {
public:
    DefaultRHICommandList();
    virtual ~DefaultRHICommandList();

    // RHICommandList 接口
    void BeginRenderPass(const RenderPassDesc& desc) override;
    void EndRenderPass() override;
    void Draw(const DrawArguments& args) override;
    void DrawIndexed(const DrawIndexedArguments& args) override;
    void DrawInstanced(const DrawInstancedArguments& args) override;
    void DrawIndexedInstanced(const DrawIndexedInstancedArguments& args) override;
    void Dispatch(const DispatchArguments& args) override;
    void DispatchIndirect(RHIBuffer* indirectBuffer, uint64 offset) override;
    void CopyBuffer(const CopyBufferArguments& args) override;
    void CopyBufferToTexture(const CopyBufferToTextureArguments& args) override;
    void CopyTexture(const CopyTextureArguments& args) override;
    void SetViewport(const Viewport& viewport) override;
    void SetScissor(uint32 left, uint32 top, uint32 right, uint32 bottom) override;
    void SetStencilRef(uint32 stencilRef) override;
    void SetBlendFactor(const float blendFactor[4]) override;
    void BindGraphicsPipeline(RHIGraphicsPipeline* pipeline) override;
    void BindComputePipeline(RHIComputePipeline* pipeline) override;
    void BindBindingSet(uint32 setIndex, IRHIBindingSet* bindingSet) override;
    void PushConstants(ShaderStageMask stages, uint32 offset, uint32 size, const void* data) override;
    void BindVertexBuffer(uint32 slot, IRHIBuffer* buffer, uint64 offset) override;
    void BindIndexBuffer(IRHIBuffer* buffer, uint64 offset, IndexType indexType) override;
    void Barrier(const BarrierDesc& barrier) override;
    void UAVBarrier(IRHIResource* resource) override;
    void AliasBarrier(IRHIResource* before, IRHIResource* after) override;

    // 访问录制的命令（供Executor执行）
    const std::vector<Command>& GetCommands() const { return commands_; }
    void ClearCommands();

private:
    std::vector<Command> commands_;
};

} // namespace RHI
```

- [ ] **Step 2: 创建RHICommandList.cpp**

```cpp
// Runtime/Rendering/RHI/Core/RHICommandList.cpp
#include "RHICommandList.h"

namespace RHI {

DefaultRHICommandList::DefaultRHICommandList() = default;
DefaultRHICommandList::~DefaultRHICommandList() = default;

void DefaultRHICommandList::BeginRenderPass(const RenderPassDesc& desc) {
    commands_.push_back(Command{ Command::Type::BeginRenderPass, .renderPass = desc });
}

void DefaultRHICommandList::EndRenderPass() {
    commands_.push_back(Command{ Command::Type::EndRenderPass });
}

void DefaultRHICommandList::Draw(const DrawArguments& args) {
    commands_.push_back(Command{ Command::Type::Draw, .draw = { Draw, args } });
}

void DefaultRHICommandList::DrawIndexed(const DrawIndexedArguments& args) {
    commands_.push_back(Command{ Command::Type::DrawIndexed, .drawIndexed = args });
}

void DefaultRHICommandList::DrawInstanced(const DrawInstancedArguments& args) {
    commands_.push_back(Command{ Command::Type::DrawInstanced, .drawInstanced = args });
}

void DefaultRHICommandList::DrawIndexedInstanced(const DrawIndexedInstancedArguments& args) {
    commands_.push_back(Command{ Command::Type::DrawIndexedInstanced, .drawIndexedInstanced = args });
}

void DefaultRHICommandList::Dispatch(const DispatchArguments& args) {
    commands_.push_back(Command{ Command::Type::Dispatch, .dispatch = args });
}

void DefaultRHICommandList::DispatchIndirect(RHIBuffer* indirectBuffer, uint64 offset) {
    commands_.push_back(Command{ Command::Type::DispatchIndirect, .dispatchIndirect = { indirectBuffer, offset } });
}

void DefaultRHICommandList::CopyBuffer(const CopyBufferArguments& args) {
    commands_.push_back(Command{ Command::Type::CopyBuffer, .copyBuffer = args });
}

void DefaultRHICommandList::CopyBufferToTexture(const CopyBufferToTextureArguments& args) {
    commands_.push_back(Command{ Command::Type::CopyBufferToTexture, .copyBufferToTexture = args });
}

void DefaultRHICommandList::CopyTexture(const CopyTextureArguments& args) {
    commands_.push_back(Command{ Command::Type::CopyTexture, .copyTexture = args });
}

void DefaultRHICommandList::SetViewport(const Viewport& viewport) {
    commands_.push_back(Command{ Command::Type::SetViewport, .viewport = viewport });
}

void DefaultRHICommandList::SetScissor(uint32 left, uint32 top, uint32 right, uint32 bottom) {
    commands_.push_back(Command{ Command::Type::SetScissor, .scissor = { left, top, right, bottom } });
}

void DefaultRHICommandList::SetStencilRef(uint32 stencilRef) {
    commands_.push_back(Command{ Command::Type::SetStencilRef, .stencilRef = stencilRef });
}

void DefaultRHICommandList::SetBlendFactor(const float blendFactor[4]) {
    auto& bf = commands_.push_back(Command{ Command::Type::SetBlendFactor, .blendFactor = {} });
    std::memcpy(bf.blendFactor.value, blendFactor, sizeof(float) * 4);
}

void DefaultRHICommandList::BindGraphicsPipeline(RHIGraphicsPipeline* pipeline) {
    commands_.push_back(Command{ Command::Type::BindGraphicsPipeline, .pipeline = pipeline });
}

void DefaultRHICommandList::BindComputePipeline(RHIComputePipeline* pipeline) {
    commands_.push_back(Command{ Command::Type::BindComputePipeline, .computePipeline = pipeline });
}

void DefaultRHICommandList::BindBindingSet(uint32 setIndex, IRHIBindingSet* bindingSet) {
    commands_.push_back(Command{ Command::Type::BindBindingSet, .bindingSet = { setIndex, bindingSet } });
}

void DefaultRHICommandList::PushConstants(ShaderStageMask stages, uint32 offset, uint32 size, const void* data) {
    auto& pc = commands_.push_back(Command{ Command::Type::PushConstants });
    pc.pushConstants.stages = stages;
    pc.pushConstants.offset = offset;
    pc.pushConstants.size = size;
    pc.pushConstants.data = malloc(size);
    std::memcpy(pc.pushConstants.data, data, size);
}

void DefaultRHICommandList::BindVertexBuffer(uint32 slot, IRHIBuffer* buffer, uint64 offset) {
    commands_.push_back(Command{ Command::Type::BindVertexBuffer, .vertexBuffer = { slot, buffer, offset } });
}

void DefaultRHICommandList::BindIndexBuffer(IRHIBuffer* buffer, uint64 offset, IndexType indexType) {
    commands_.push_back(Command{ Command::Type::BindIndexBuffer, .indexBuffer = { buffer, offset, indexType } });
}

void DefaultRHICommandList::Barrier(const BarrierDesc& barrier) {
    auto& b = commands_.push_back(Command{ Command::Type::Barrier });
    b.barrier = barrier;
}

void DefaultRHICommandList::UAVBarrier(IRHIResource* resource) {
    commands_.push_back(Command{ Command::Type::UAVBarrier, .uavResource = resource });
}

void DefaultRHICommandList::AliasBarrier(IRHIResource* before, IRHIResource* after) {
    commands_.push_back(Command{ Command::Type::AliasBarrier, .aliasBarrier = { before, after } });
}

void DefaultRHICommandList::ClearCommands() {
    // 清理push constant分配的内存
    for (auto& cmd : commands_) {
        if (cmd.type == Command::Type::PushConstants && cmd.pushConstants.data) {
            free(cmd.pushConstants.data);
        }
    }
    commands_.clear();
}

} // namespace RHI
```

注：上述cpp需要完善Command结构体定义和union，实际实现需要更完整的结构。

- [ ] **Step 3: 验证CMakeLists.txt是否需要更新**

检查Runtime/Rendering/RHI/Core/CMakeLists.txt是否存在

- [ ] **Step 4: 提交**

```bash
git add Runtime/Rendering/RHI/Core/RHICommandList.h
git add Runtime/Rendering/RHI/Core/RHICommandList.cpp
git commit -m "feat(RHI): add RHICommandList unified command interface

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

### Task 3: 创建统一资源接口IRHIResource

**Files:**
- Create: `Runtime/Rendering/RHI/Core/IRHIResource.h`

- [ ] **Step 1: 创建IRHIResource.h**

```cpp
// Runtime/Rendering/RHI/Core/IRHIResource.h
#pragma once
#include "RHI/RHICommon.h"
#include "RHI/RHIEnums.h"

namespace RHI {

// 统一资源描述符
struct ResourceDesc {
    EResourceType type = EResourceType::Buffer;
    uint64 size = 0; // Buffer大小或Texture字节大小

    // Texture专用
    uint32 width = 1;
    uint32 height = 1;
    uint32 depth = 1;
    uint32 arraySize = 1;
    uint32 mipLevels = 1;
    EPixelFormat format = EPixelFormat::Unknown;

    // 标志
    enum Flags {
        None = 0,
        Shared = 1 << 0,        // 跨Adapter共享
        Sparse = 1 << 1,        // 稀疏资源
        Transient = 1 << 2,     // 临时资源
    };
    uint32 flags = None;

    ResourceState initialState = ResourceState::Common;
};

// 统一资源基类
class IRHIResource : public RHIObject {
public:
    virtual ~IRHIResource() = default;

    virtual ResourceDesc GetDesc() const = 0;
    virtual ResourceState GetState() const = 0;
    virtual void SetState(ResourceState state) = 0;
    virtual NativeHandle GetNativeHandle() const = 0;
    virtual bool IsValid() const = 0;
};

// 统一Buffer接口
class IRHIBuffer : public IRHIResource {
public:
    virtual uint64 GetSize() const = 0;
    virtual uint64 GetGPUAddress() const = 0;
    virtual void* Map() = 0;
    virtual void Unmap() = 0;
};

// 统一Texture接口
class IRHITexture : public IRHIResource {
public:
    virtual uint32 GetWidth() const = 0;
    virtual uint32 GetHeight() const = 0;
    virtual uint32 GetDepth() const = 0;
    virtual uint32 GetMipLevels() const = 0;
    virtual EPixelFormat GetFormat() const = 0;
};

// 工厂函数 - 从现有RHIBuffer/RHITexture获取IRHIResource
inline IRHIResource* AsIRHIResource(RHIBuffer* buffer) { return dynamic_cast<IRHIResource*>(buffer); }
inline IRHIResource* AsIRHIResource(RHITexture* texture) { return dynamic_cast<IRHIResource*>(texture); }

} // namespace RHI
```

- [ ] **Step 2: 提交**

```bash
git add Runtime/Rendering/RHI/Core/IRHIResource.h
git commit -m "feat(RHI): add IRHIResource unified resource interface

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

### Task 4: 更新RHIDevice添加CreateExecutor工厂方法

**Files:**
- Modify: `Runtime/Rendering/RHI/Core/RHIDevice.h`

- [ ] **Step 1: 读取当前RHIDevice.h**

Read `Runtime/Rendering/RHI/Core/RHIDevice.h`

- [ ] **Step 2: 添加CreateExecutor声明**

在RHIDevice类中添加:
```cpp
// 创建命令列表Executor
virtual std::unique_ptr<IRHICommandListExecutor> CreateCommandListExecutor() = 0;
```

- [ ] **Step 3: 提交**

```bash
git add Runtime/Rendering/RHI/Core/RHIDevice.h
git commit -m "feat(RHI): add CreateCommandListExecutor to RHIDevice interface

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Phase 2: DX12 Executor 实现

### Task 5: 实现DX12CommandListExecutor

**Files:**
- Create: `Runtime/Rendering/RHI/Backends/DX12/DX12CommandListExecutor.h`
- Create: `Runtime/Rendering/RHI/Backends/DX12/DX12CommandListExecutor.cpp`

- [ ] **Step 1: 创建DX12CommandListExecutor.h**

```cpp
// Runtime/Rendering/RHI/Backends/DX12/DX12CommandListExecutor.h
#pragma once
#include "RHI/Core/RHICommandListExecutor.h"
#include "RHI/Core/RHICommandList.h"

namespace RHI::DX12 {

class DX12CommandListExecutor : public IRHICommandListExecutor {
public:
    DX12CommandListExecutor(ID3D12Device* device, ID3D12CommandQueue* commandQueue);
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

    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> commandQueue_;
    Microsoft::WRL::ComPtr<ID3D12CommandList> currentCmdList_;
    std::vector<D3D12_RESOURCE_BARRIER> dx12Barriers_;
    std::vector<Microsoft::WRL::ComPtr<ID3D12CommandList>> pendingCmdLists_;
};

} // namespace RHI::DX12
```

- [ ] **Step 2: 创建DX12CommandListExecutor.cpp框架**

```cpp
// Runtime/Rendering/RHI/Backends/DX12/DX12CommandListExecutor.cpp
#include "DX12CommandListExecutor.h"

namespace RHI::DX12 {

DX12CommandListExecutor::DX12CommandListExecutor(ID3D12Device* device, ID3D12CommandQueue* commandQueue)
    : device_(device)
    , commandQueue_(commandQueue) {
}

DX12CommandListExecutor::~DX12CommandListExecutor() = default;

void DX12CommandListExecutor::Reset(RHICommandList* cmdList) {
    // 重置录制状态
}

void DX12CommandListExecutor::BeginRecording(RHICommandList* cmdList) {
    // 开始录制
}

void DX12CommandListExecutor::EndRecording(RHICommandList* cmdList) {
    // 结束录制
}

void DX12CommandListExecutor::Execute(RHICommandList* cmdList) {
    // 遍历命令并执行
}

void DX12CommandListExecutor::ExecuteDrawCommands(RHICommandList* cmdList) {
    // 执行图形命令
}

void DX12CommandListExecutor::ExecuteComputeCommands(RHICommandList* cmdList) {
    // 执行计算命令
}

void DX12CommandListExecutor::ExecuteCopyCommands(RHICommandList* cmdList) {
    // 执行拷贝命令
}

void DX12CommandListExecutor::ExecuteBarriers(RHICommandList* cmdList) {
    // 执行barrier命令
}

void DX12CommandListExecutor::ExecuteRenderPass(RHICommandList* cmdList) {
    // 执行渲染Pass
}

} // namespace RHI::DX12
```

- [ ] **Step 3: 实现完整的Execute逻辑（待填充）**

- [ ] **Step 4: 提交**

```bash
git add Runtime/Rendering/RHI/Backends/DX12/DX12CommandListExecutor.h
git add Runtime/Rendering/RHI/Backends/DX12/DX12CommandListExecutor.cpp
git commit -m "feat(DX12): initial DX12CommandListExecutor implementation

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Phase 3: Vulkan Executor 实现

### Task 6: 实现VulkanCommandListExecutor

**Files:**
- Create: `Runtime/Rendering/RHI/Backends/Vulkan/VulkanCommandListExecutor.h`
- Create: `Runtime/Rendering/RHI/Backends/Vulkan/VulkanCommandListExecutor.cpp`

- [ ] **Step 1: 创建VulkanCommandListExecutor.h**

```cpp
// Runtime/Rendering/RHI/Backends/Vulkan/VulkanCommandListExecutor.h
#pragma once
#include "RHI/Core/RHICommandListExecutor.h"
#include "RHI/Core/RHICommandList.h"

namespace RHI::Vulkan {

class VulkanCommandListExecutor : public IRHICommandListExecutor {
public:
    VulkanCommandListExecutor(VkDevice device, VkCommandPool commandPool, VkQueue graphicsQueue);
    virtual ~VulkanCommandListExecutor();

    // IRHICommandListExecutor
    void Reset(RHICommandList* cmdList) override;
    void BeginRecording(RHICommandList* cmdList) override;
    void EndRecording(RHICommandList* cmdList) override;
    void Execute(RHICommandList* cmdList) override;
    const char* GetBackendName() const override { return "Vulkan"; }
    bool SupportsExplicitBarriers() const override { return true; }

private:
    void ExecuteDrawCommands(RHICommandList* cmdList);
    void ExecuteComputeCommands(RHICommandList* cmdList);
    void ExecuteCopyCommands(RHICommandList* cmdList);
    void ExecuteBarriers(RHICommandList* cmdList);

    VkDevice device_;
    VkCommandPool commandPool_;
    VkQueue graphicsQueue_;
    VkCommandBuffer currentCmdBuffer_;
    std::vector<VkBufferMemoryBarrier> bufferBarriers_;
    std::vector<VkImageMemoryBarrier> imageBarriers_;
};

} // namespace RHI::Vulkan
```

- [ ] **Step 2: 创建VulkanCommandListExecutor.cpp框架**

- [ ] **Step 3: 提交**

---

## Phase 4: Tier B 后端适配器

### Task 7: 实现DX11CommandListExecutor

**Files:**
- Create: `Runtime/Rendering/RHI/Backends/DX11/DX11CommandListExecutor.h`
- Create: `Runtime/Rendering/RHI/Backends/DX11/DX11CommandListExecutor.cpp`

- [ ] **Step 1: 创建DX11CommandListExecutor.h**

```cpp
// Runtime/Rendering/RHI/Backends/DX11/DX11CommandListExecutor.h
#pragma once
#include "RHI/Core/RHICommandListExecutor.h"
#include "RHI/Core/RHICommandList.h"

namespace RHI::DX11 {

// DX11不支持显式barrier，通过适配器模拟
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
    bool SupportsExplicitBarriers() const override { return false; } // DX11隐式同步

private:
    ID3D11Device* device_;
    ID3D11DeviceContext* context_;
};

} // namespace RHI::DX11
```

- [ ] **Step 2: 提交**

---

### Task 8: 实现OpenGLCommandListExecutor

**Files:**
- Create: `Runtime/Rendering/RHI/Backends/OpenGL/OpenGLCommandListExecutor.h`
- Create: `Runtime/Rendering/RHI/Backends/OpenGL/OpenGLCommandListExecutor.cpp`

- [ ] **Step 1: 创建OpenGLCommandListExecutor.h/cpp**

- [ ] **Step 2: 提交**

---

## Phase 5: 集成和迁移

### Task 9: 更新Driver集成RHICommandList

**Files:**
- Modify: `Runtime/Rendering/Context/Driver.h`
- Modify: `Runtime/Rendering/Context/Driver.cpp`

- [ ] **Step 1: 读取Driver.h和Driver.cpp**

- [ ] **Step 2: 在DriverImpl中添加Executor成员**

```cpp
std::unique_ptr<IRHICommandListExecutor> commandListExecutor_;
```

- [ ] **Step 3: 修改SubmitMaterialDraw使用RHICommandList**

- [ ] **Step 4: 提交**

---

### Task 10: 迁移FrameGraphExecutionContext使用RHICommandList

**Files:**
- Modify: `Runtime/Rendering/FrameGraph/FrameGraphExecutionContext.h`
- Modify: `Runtime/Rendering/FrameGraph/FrameGraphExecutionContext.cpp`

- [ ] **Step 1: 读取FrameGraphExecutionContext.h/cpp**

- [ ] **Step 2: 更新RecordResourceBarriers使用RHICommandList::Barrier**

- [ ] **Step 3: 提交**

---

### Task 11: 迁移Material系统

**Files:**
- Modify: `Runtime/Rendering/Resources/Material.h`
- Modify: `Runtime/Rendering/Resources/Material.cpp`

- [ ] **Step 1: 读取Material.h/cpp**

- [ ] **Step 2: 更新Material::BuildRecordedGraphicsPipeline签名**

- [ ] **Step 3: 提交**

---

## Phase 6: 验证和优化

### Task 12: RenderDoc多后端验证

- [ ] **Step 1: 使用RenderDoc验证DX12后端正确性**

- [ ] **Step 2: 使用RenderDoc验证Vulkan后端正确性**

- [ ] **Step 3: 对比两个后端的frame capture确保行为一致**

- [ ] **Step 4: 提交验证结果**

---

## 验证清单

每个Task完成后需确认：
- [ ] 代码编译通过
- [ ] 相关单元测试通过（如果有）
- [ ] RenderDoc验证（对于渲染相关任务）

---

## 风险缓解

1. **Phase分离**: 每个Phase有明确的交付物，可独立验证
2. **向后兼容**: Phase 1-4期间Legacy路径仍可用
3. **测试驱动**: 优先编写测试，再实现功能

---

## 参考资料

- `Runtime/Rendering/RHI/Core/` - 现有RHI核心接口
- `Runtime/Rendering/RHI/Backends/DX12/` - DX12后端
- `Runtime/Rendering/RHI/Backends/Vulkan/` - Vulkan后端
- `Runtime/Rendering/Context/Driver.cpp` - 调用入口
- `Docs/Rendering/RHIMultiBackendArchitecture.md` - 架构文档
