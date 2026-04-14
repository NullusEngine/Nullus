# 渲染器 Formal RHI 重构设计文档

> **日期**: 2026-04-05
> **状态**: 已批准

## 目标

将渲染器从 **Dual-layer（Formal RHI + Legacy IRenderDevice 并存）** 重构为 **Formal RHI 单一路径**，所有后端（Vulkan, DX12, DX11, OpenGL）实现统一的 Formal RHI 接口。

## 约束条件

1. **保持 Tier B 后端兼容** - DX11/OpenGL 需要接入 Formal RHI
2. **完整重构** - Forward/Deferred SceneRenderer 都走 Formal RHI，无 legacy fallback
3. **不需要旧数据兼容** - 可接受破坏性变更
4. **验证方案** - 编译 + 单元测试 + 手动运行 Editor (RenderDoc)

## 架构总览

```
                        ┌─────────────────────────────────────────┐
                        │          ForwardSceneRenderer            │
                        │    (只调用 Formal RHI API，无 legacy)    │
                        └──────────────────┬──────────────────────┘
                                           │
                        ┌──────────────────▼──────────────────────┐
                        │            RHIDevice (抽象接口)           │
                        │  BindVertexBuffer / BindIndexBuffer    │
                        │  DrawIndexed / BeginRenderPass / ...   │
                        └──────────────────┬──────────────────────┘
                                           │
          ┌──────────────┬─────────────────┼─────────────────┬──────────────┐
          ▼              ▼                 ▼                 ▼              ▼
   ┌────────────┐ ┌────────────┐   ┌────────────┐   ┌────────────┐ ┌────────────┐
   │ VulkanRHI  │ │ DX12RHI    │   │ DX11RHI    │   │ OpenGLRHI  │ │ NullRHI    │
   │ (Tier A)   │ │ (Tier A)   │   │ (Tier B→A) │   │ (Tier B→A) │ │ (Testing)  │
   └────────────┘ └────────────┘   └────────────┘   └────────────┘ └────────────┘
```

## 实现任务

### Task 1: 新增 RHIMesh 接口

**文件**:
- 创建: `Runtime/Rendering/RHI/Core/RHIMesh.h`

```cpp
namespace NLS::Render::RHI
{
class NLS_RENDER_API RHIMesh
{
public:
    virtual ~RHIMesh() = default;
    virtual std::shared_ptr<RHIBuffer> GetVertexBuffer() const = 0;
    virtual std::shared_ptr<RHIBuffer> GetIndexBuffer() const = 0;
    virtual uint32_t GetVertexCount() const = 0;
    virtual uint32_t GetIndexCount() const = 0;
    virtual EPrimitiveMode GetPrimitiveMode() const = 0;
};
}
```

### Task 2: 实现 RHIMeshAdapter

**文件**:
- 创建: `Runtime/Rendering/RHI/Core/RHIMeshAdapter.h`
- 创建: `Runtime/Rendering/RHI/Core/RHIMeshAdapter.cpp`

将 `Resources::Mesh` 适配为 `RHI::RHIMesh` 接口。

### Task 3: 修改 IMesh 接口

**文件**:
- 修改: `Runtime/Rendering/Resources/IMesh.h` - 添加 `GetRHIMesh()`
- 修改: `Runtime/Rendering/Resources/Mesh.cpp` - 实现 `GetRHIMesh()`

```cpp
class NLS_RENDER_API IMesh
{
public:
    virtual std::shared_ptr<RHI::RHIMesh> GetRHIMesh() const = 0;
};
```

### Task 4: 暴露 Material Formal RHI 方法

**文件**:
- 修改: `Runtime/Rendering/Resources/Material.h` - 将 `BuildRecordedGraphicsPipeline()`, `GetRecordedBindingSet()`, `GetExplicitPipelineLayout()` 从 private 移到 public

### Task 5: 重构 ForwardSceneRenderer

**文件**:
- 修改: `Runtime/Engine/Rendering/ForwardSceneRenderer.cpp`

移除 `DrawEntity()` 调用，改为直接录制 Formal RHI 命令：

```cpp
void ForwardSceneRenderer::DrawOpaques(FrameGraph& fg, ...)
{
    auto cmd = GetActiveExplicitCommandBuffer();
    auto device = GetExplicitDevice();

    fg.AddCallbackPass("ForwardOpaque",
        [this, cmd, device](FrameGraphPassResources& resources, void* ctx) {
            cmd->BeginRenderPass(renderPassDesc);

            for (auto& drawable : GetOpaqueDrawables())
            {
                auto material = drawable.material;
                auto mesh = drawable.mesh;

                auto pipeline = material->BuildRecordedGraphicsPipeline(
                    device, drawable.primitiveMode, depthFunc, {});
                auto bindingSet = material->GetRecordedBindingSet(device);
                auto rhiMesh = mesh->GetRHIMesh();

                cmd->BindGraphicsPipeline(pipeline);
                cmd->BindBindingSet(0, bindingSet);
                cmd->BindVertexBuffer(0, rhiMesh->GetVertexBuffer(), 0);
                cmd->BindIndexBuffer(rhiMesh->GetIndexBuffer(), 0);
                cmd->DrawIndexed(rhiMesh->GetIndexCount(),
                                material->GetGPUInstances(), 0, 0, 0);
            }

            cmd->EndRenderPass();
        });
}
```

### Task 6: 重构 DeferredSceneRenderer

**文件**:
- 修改: `Runtime/Engine/Rendering/DeferredSceneRenderer.cpp`

同 Task 5 模式。

### Task 7: 重写 DX11 后端为 Formal RHI

**文件**:
- 创建/修改: `Runtime/Rendering/RHI/Backends/DX11/`

实现完整的 Formal RHI 接口：
- `RHIDevice` - D3D11 设备、命令列表
- `RHICommandBuffer` - 录制 D3D11 命令
- `RHIGraphicsPipeline` - D3D11 管线状态
- `RHIBindingSet` / `RHIBindingLayout` - D3D11 资源绑定

### Task 8: 重写 OpenGL 后端为 Formal RHI

**文件**:
- 创建/修改: `Runtime/Rendering/RHI/Backends/OpenGL/`

实现完整的 Formal RHI 接口。

### Task 9: 移除 ABaseRenderer legacy 逻辑

**文件**:
- 修改: `Runtime/Rendering/Core/ABaseRenderer.cpp` - 移除 `DrawEntity` legacy fallback

### Task 10: 简化 Driver::SubmitMaterialDraw

**文件**:
- 修改: `Runtime/Rendering/Context/Driver.cpp` - 移除 Formal RHI 录制逻辑，仅保留 legacy（供内部使用）

## 验证方案

1. **编译**: `cmake --build build --config Debug`
2. **单元测试**: `ctest --test-dir build -C Debug --output-on-failure`
3. **手动测试**: 运行 `Editor.exe --backend vulkan` 并用 RenderDoc 捕获帧

## 风险与注意事项

1. DX11/OpenGL Formal RHI 后端工作量较大
2. 需要确保 shader 跨后端兼容性
3. Frame graph 的资源屏障需要正确处理