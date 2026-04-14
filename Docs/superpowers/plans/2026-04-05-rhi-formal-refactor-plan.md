# 渲染器 Formal RHI 重构实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将渲染器从 Dual-layer (Formal RHI + Legacy) 重构为 Formal RHI 单一路径，所有后端实现统一接口

**Architecture:** 新增 RHIMesh 接口和适配器，重构 Forward/DeferredSceneRenderer 直接录制 Formal RHI 命令，重写 DX11/OpenGL 后端

**Tech Stack:** C++17, Vulkan, DX12, DX11, OpenGL, CMake

---

## 文件结构映射

| 文件 | 变更类型 | 说明 |
|------|----------|------|
| `Runtime/Rendering/RHI/Core/RHIMesh.h` | 新增 | Formal RHI Mesh 接口 |
| `Runtime/Rendering/RHI/Core/RHIMeshAdapter.h` | 新增 | Mesh → RHIMesh 适配器 |
| `Runtime/Rendering/RHI/Core/RHIMeshAdapter.cpp` | 新增 | 适配器实现 |
| `Runtime/Rendering/Resources/IMesh.h` | 修改 | 添加 GetRHIMesh() |
| `Runtime/Rendering/Resources/Mesh.cpp` | 修改 | 实现 GetRHIMesh() |
| `Runtime/Rendering/Resources/Material.h` | 修改 | Formal RHI 方法改为 public |
| `Runtime/Engine/Rendering/ForwardSceneRenderer.cpp` | 修改 | 直接录制 Formal RHI |
| `Runtime/Engine/Rendering/DeferredSceneRenderer.cpp` | 修改 | 直接录制 Formal RHI |
| `Runtime/Rendering/Core/ABaseRenderer.cpp` | 修改 | 移除 legacy 逻辑 |
| `Runtime/Rendering/Context/Driver.cpp` | 修改 | 简化 SubmitMaterialDraw |

---

## Task 1: 新增 RHIMesh 接口

**Files:**
- Create: `Runtime/Rendering/RHI/Core/RHIMesh.h`

- [ ] **Step 1: 创建 RHIMesh.h**

```cpp
#pragma once

#include <memory>
#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/RHI/Core/RHIEnums.h"
#include "Rendering/Settings/EPrimitiveMode.h"

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
    virtual Settings::EPrimitiveMode GetPrimitiveMode() const = 0;
    virtual uint32_t GetVertexStride() const = 0;
    virtual IndexType GetIndexType() const = 0;
};
}
```

- [ ] **Step 2: Commit**

```bash
git add Runtime/Rendering/RHI/Core/RHIMesh.h
git commit -m "feat(RHI): add RHIMesh interface for formal RHI mesh abstraction"
```

---

## Task 2: 实现 RHIMeshAdapter

**Files:**
- Create: `Runtime/Rendering/RHI/Core/RHIMeshAdapter.h`
- Create: `Runtime/Rendering/RHI/Core/RHIMeshAdapter.cpp`

- [ ] **Step 1: 创建 RHIMeshAdapter.h**

```cpp
#pragma once

#include "Rendering/RHI/Core/RHIMesh.h"
#include <memory>

namespace NLS::Render::Resources
{
class Mesh;
}

namespace NLS::Render::RHI
{
class NLS_RENDER_API RHIMeshAdapter : public RHIMesh
{
public:
    explicit RHIMeshAdapter(const Resources::Mesh& mesh);
    ~RHIMeshAdapter() override;

    std::shared_ptr<RHIBuffer> GetVertexBuffer() const override;
    std::shared_ptr<RHIBuffer> GetIndexBuffer() const override;
    uint32_t GetVertexCount() const override { return m_vertexCount; }
    uint32_t GetIndexCount() const override { return m_indexCount; }
    Settings::EPrimitiveMode GetPrimitiveMode() const override { return Settings::EPrimitiveMode::TRIANGLE_LIST; }
    uint32_t GetVertexStride() const override { return m_vertexStride; }
    IndexType GetIndexType() const override { return IndexType::UInt32; }

private:
    std::shared_ptr<RHIBuffer> m_vertexBuffer;
    std::shared_ptr<RHIBuffer> m_indexBuffer;
    uint32_t m_vertexCount = 0;
    uint32_t m_indexCount = 0;
    uint32_t m_vertexStride = 0;
};
}
```

- [ ] **Step 2: 创建 RHIMeshAdapter.cpp**

```cpp
#include "Rendering/RHI/Core/RHIMeshAdapter.h"
#include "Rendering/Resources/Mesh.h"
#include "Rendering/RHI/Core/RHIDevice.h"

namespace NLS::Render::RHI
{
RHIMeshAdapter::RHIMeshAdapter(const Resources::Mesh& mesh)
{
    m_vertexCount = mesh.GetVertexCount();
    m_indexCount = mesh.GetIndexCount();
    m_vertexStride = sizeof(Geometry::Vertex);

    // Get formal RHI buffers from mesh's VertexBuffer/IndexBuffer
    auto vertexView = mesh.GetVertexBufferView();
    if (vertexView.explicitBuffer)
        m_vertexBuffer = vertexView.explicitBuffer;

    auto indexView = mesh.GetIndexBufferView();
    if (indexView.has_value() && indexView->explicitBuffer)
        m_indexBuffer = indexView->explicitBuffer;
}

RHIMeshAdapter::~RHIMeshAdapter() = default;

std::shared_ptr<RHIBuffer> RHIMeshAdapter::GetVertexBuffer() const
{
    return m_vertexBuffer;
}

std::shared_ptr<RHIBuffer> RHIMeshAdapter::GetIndexBuffer() const
{
    return m_indexBuffer;
}
}
```

- [ ] **Step 3: Commit**

```bash
git add Runtime/Rendering/RHI/Core/RHIMeshAdapter.h Runtime/Rendering/RHI/Core/RHIMeshAdapter.cpp
git commit -m "feat(RHI): add RHIMeshAdapter to bridge Resources::Mesh to RHIMesh"
```

---

## Task 3: 修改 IMesh 接口

**Files:**
- Modify: `Runtime/Rendering/Resources/IMesh.h:26-36`
- Modify: `Runtime/Rendering/Resources/Mesh.cpp`

- [ ] **Step 1: 修改 IMesh.h 添加 GetRHIMesh()**

在 `class IMesh` 的 public 部分添加：

```cpp
virtual std::shared_ptr<RHI::RHIMesh> GetRHIMesh() const = 0;
```

- [ ] **Step 2: 修改 Mesh.h 添加 GetRHIMesh() 实现**

在 `Mesh` 类中添加：

```cpp
virtual std::shared_ptr<NLS::Render::RHI::RHIMesh> GetRHIMesh() const override;
```

- [ ] **Step 3: 修改 Mesh.cpp 实现 GetRHIMesh()**

在文件末尾添加：

```cpp
std::shared_ptr<NLS::Render::RHI::RHIMesh> Mesh::GetRHIMesh() const
{
    static thread_local std::shared_ptr<RHI::RHIMeshAdapter> cachedAdapter;
    cachedAdapter = std::make_shared<RHI::RHIMeshAdapter>(*this);
    return cachedAdapter;
}
```

- [ ] **Step 4: Commit**

```bash
git add Runtime/Rendering/Resources/IMesh.h Runtime/Rendering/Resources/Mesh.cpp
git commit -m "feat(Mesh): add GetRHIMesh() to expose formal RHI mesh"
```

---

## Task 4: 暴露 Material Formal RHI 方法

**Files:**
- Modify: `Runtime/Rendering/Resources/Material.h:233-253`

- [ ] **Step 1: 将 private 方法移到 public**

将以下方法从 `private` 部分移到 `public` 部分：

```cpp
// 从 private 移到 public
std::shared_ptr<RHI::RHIGraphicsPipeline> BuildRecordedGraphicsPipeline(
    const std::shared_ptr<RHI::RHIDevice>& device,
    Settings::EPrimitiveMode primitiveMode,
    Settings::EComparaisonAlgorithm depthCompare,
    MaterialPipelineStateOverrides overrides = {},
    bool* hasPipelineLayout = nullptr,
    bool* hasVertexShader = nullptr,
    bool* hasFragmentShader = nullptr) const;
MaterialRuntimeState& GetRuntimeState() const;
void ResetRuntimeState() const;
void InvalidateExplicitBindingSetCache() const;
std::shared_ptr<RHI::RHIBindingSet> GetRecordedBindingSet(const std::shared_ptr<RHI::RHIDevice>& device) const;
const MaterialResourceSet& GetBindingSet() const;
const std::shared_ptr<RHI::RHIBindingLayout>& GetExplicitBindingLayout(const std::shared_ptr<RHI::RHIDevice>& device) const;
const std::shared_ptr<RHI::RHIBindingSet>& GetExplicitBindingSet(const std::shared_ptr<RHI::RHIDevice>& device) const;
const std::shared_ptr<RHI::RHIPipelineLayout>& GetExplicitPipelineLayout(const std::shared_ptr<RHI::RHIDevice>& device) const;
```

- [ ] **Step 2: Commit**

```bash
git add Runtime/Rendering/Resources/Material.h
git commit -m "feat(Material): expose formal RHI methods as public for renderer direct access"
```

---

## Task 5: 重构 ForwardSceneRenderer 使用 Formal RHI

**Files:**
- Modify: `Runtime/Engine/Rendering/ForwardSceneRenderer.cpp:177-223`

- [ ] **Step 1: 重写 DrawOpaques 方法**

将 `DrawOpaques` 改为直接录制：

```cpp
void ForwardSceneRenderer::DrawOpaques(NLS::Render::Data::PipelineState pso)
{
    const auto& scene = GetDescriptor<ForwardSceneDescriptor>();
    auto commandBuffer = GetActiveExplicitCommandBuffer();
    auto device = GetExplicitDevice();

    for (const auto& [_, drawable] : scene.drawables.opaques)
    {
        if (drawable.material == nullptr || drawable.mesh == nullptr)
            continue;

        auto material = drawable.material;
        auto mesh = drawable.mesh;

        auto pipeline = material->BuildRecordedGraphicsPipeline(
            device, drawable.primitiveMode, pso.depthFunc, {});
        auto bindingSet = material->GetRecordedBindingSet(device);
        auto rhiMesh = mesh->GetRHIMesh();

        commandBuffer->BindGraphicsPipeline(pipeline);
        commandBuffer->BindBindingSet(0, bindingSet);
        commandBuffer->BindVertexBuffer(0, { rhiMesh->GetVertexBuffer(), 0, rhiMesh->GetVertexStride() });
        commandBuffer->BindIndexBuffer({ rhiMesh->GetIndexBuffer(), 0, rhiMesh->GetIndexType() });
        commandBuffer->DrawIndexed(rhiMesh->GetIndexCount(), material->GetGPUInstances(), 0, 0, 0);
    }
}
```

- [ ] **Step 2: 重写 DrawSkyboxes 方法**

```cpp
void ForwardSceneRenderer::DrawSkyboxes(NLS::Render::Data::PipelineState pso)
{
    const auto& scene = GetDescriptor<ForwardSceneDescriptor>();
    auto commandBuffer = GetActiveExplicitCommandBuffer();
    auto device = GetExplicitDevice();

    for (const auto& [_, drawable] : scene.drawables.skyboxes)
    {
        if (drawable.mesh == nullptr || drawable.material == nullptr)
            continue;

        auto material = drawable.material;
        auto mesh = drawable.mesh;

        auto pipeline = material->BuildRecordedGraphicsPipeline(
            device, drawable.primitiveMode, NLS::Render::Settings::EComparaisonAlgorithm::LESS_EQUAL, {});
        auto bindingSet = material->GetRecordedBindingSet(device);
        auto rhiMesh = mesh->GetRHIMesh();

        commandBuffer->BindGraphicsPipeline(pipeline);
        commandBuffer->BindBindingSet(0, bindingSet);
        commandBuffer->BindVertexBuffer(0, { rhiMesh->GetVertexBuffer(), 0, rhiMesh->GetVertexStride() });
        commandBuffer->BindIndexBuffer({ rhiMesh->GetIndexBuffer(), 0, rhiMesh->GetIndexType() });
        commandBuffer->DrawIndexed(rhiMesh->GetIndexCount(), material->GetGPUInstances(), 0, 0, 0);
    }
}
```

- [ ] **Step 3: 重写 DrawTransparents 方法**

```cpp
void ForwardSceneRenderer::DrawTransparents(NLS::Render::Data::PipelineState pso)
{
    const auto& scene = GetDescriptor<ForwardSceneDescriptor>();
    auto commandBuffer = GetActiveExplicitCommandBuffer();
    auto device = GetExplicitDevice();

    NLS::Render::Resources::MaterialPipelineStateOverrides transparentOverrides;
    transparentOverrides.depthWrite = false;

    for (const auto& [_, drawable] : scene.drawables.transparents)
    {
        if (drawable.material == nullptr || drawable.mesh == nullptr)
            continue;

        auto material = drawable.material;
        auto mesh = drawable.mesh;

        auto pipeline = material->BuildRecordedGraphicsPipeline(
            device, drawable.primitiveMode, pso.depthFunc, transparentOverrides);
        auto bindingSet = material->GetRecordedBindingSet(device);
        auto rhiMesh = mesh->GetRHIMesh();

        commandBuffer->BindGraphicsPipeline(pipeline);
        commandBuffer->BindBindingSet(0, bindingSet);
        commandBuffer->BindVertexBuffer(0, { rhiMesh->GetVertexBuffer(), 0, rhiMesh->GetVertexStride() });
        commandBuffer->BindIndexBuffer({ rhiMesh->GetIndexBuffer(), 0, rhiMesh->GetIndexType() });
        commandBuffer->DrawIndexed(rhiMesh->GetIndexCount(), material->GetGPUInstances(), 0, 0, 0);
    }
}
```

- [ ] **Step 4: Commit**

```bash
git add Runtime/Engine/Rendering/ForwardSceneRenderer.cpp
git commit -m "refactor(ForwardSceneRenderer): use formal RHI direct command recording"
```

---

## Task 6: 重构 DeferredSceneRenderer 使用 Formal RHI

**Files:**
- Modify: `Runtime/Engine/Rendering/DeferredSceneRenderer.cpp`

- [ ] **Step 1: 读取 DeferredSceneRenderer.cpp 了解结构**

确认 DeferredSceneRenderer 是否有类似的 DrawOpaques/DrawTransparents 方法需要重构。

- [ ] **Step 2: 按照 Task 5 的模式重构 DeferredSceneRenderer**

模式相同：直接录制 Formal RHI 命令，调用 `BuildRecordedGraphicsPipeline`、`GetRecordedBindingSet`、`GetRHIMesh`。

- [ ] **Step 3: Commit**

```bash
git add Runtime/Engine/Rendering/DeferredSceneRenderer.cpp
git commit -m "refactor(DeferredSceneRenderer): use formal RHI direct command recording"
```

---

## Task 7: 移除 ABaseRenderer legacy 逻辑

**Files:**
- Modify: `Runtime/Rendering/Core/ABaseRenderer.cpp:491-565`

- [ ] **Step 1: 修改 DrawEntity 移除 legacy fallback**

将 `DrawEntity` 方法中的 legacy fallback 移除，改为直接调用 Formal RHI：

```cpp
void ABaseRenderer::DrawEntity(
    PipelineState p_pso,
    const Entities::Drawable& p_drawable)
{
    auto material = p_drawable.material;
    auto mesh = p_drawable.mesh;

    if (material == nullptr || mesh == nullptr)
        return;

    const auto gpuInstances = material->GetGPUInstances();

    if (mesh && gpuInstances > 0)
    {
        auto commandBuffer = GetActiveExplicitCommandBuffer();
        auto device = GetExplicitDevice();

        auto pipelineOverrides = BuildMaterialPipelineStateOverrides(p_pso, *material);
        auto pipeline = material->BuildRecordedGraphicsPipeline(
            device, p_drawable.primitiveMode, p_pso.depthFunc, pipelineOverrides);
        auto bindingSet = material->GetRecordedBindingSet(device);
        auto rhiMesh = mesh->GetRHIMesh();

        commandBuffer->BindGraphicsPipeline(pipeline);
        commandBuffer->BindBindingSet(0, bindingSet);
        commandBuffer->BindVertexBuffer(0, { rhiMesh->GetVertexBuffer(), 0, rhiMesh->GetVertexStride() });
        commandBuffer->BindIndexBuffer({ rhiMesh->GetIndexBuffer(), 0, rhiMesh->GetIndexType() });
        commandBuffer->DrawIndexed(rhiMesh->GetIndexCount(), gpuInstances, 0, 0, 0);
    }
}
```

- [ ] **Step 2: Commit**

```bash
git add Runtime/Rendering/Core/ABaseRenderer.cpp
git commit -m "refactor(ABaseRenderer): remove legacy fallback, use formal RHI direct recording"
```

---

## Task 8: 简化 Driver::SubmitMaterialDraw

**Files:**
- Modify: `Runtime/Rendering/Context/Driver.cpp`

- [ ] **Step 1: 读取 Driver.cpp 中的 SubmitMaterialDraw 方法**

确认 Formal RHI 录制逻辑位置。

- [ ] **Step 2: 移除 SubmitMaterialDraw 中的 Formal RHI 录制逻辑**

该方法现在只处理 legacy fallback（内部使用），Formal RHI 录制完全由渲染器层处理。

- [ ] **Step 3: Commit**

```bash
git add Runtime/Rendering/Context/Driver.cpp
git commit -m "refactor(Driver): simplify SubmitMaterialDraw, formal RHI now handled by renderer"
```

---

## Task 9: 构建验证

- [ ] **Step 1: 运行 cmake 配置**

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
```

- [ ] **Step 2: 运行构建**

```powershell
cmake --build build --config Debug
```

预期：无编译错误。

- [ ] **Step 3: 运行单元测试**

```powershell
ctest --test-dir build -C Debug --output-on-failure
```

预期：所有测试通过。

---

## Task 10: 手动 Editor 验证（可选，用于完整测试）

- [ ] **Step 1: 运行 Editor (Vulkan)**

```powershell
./Build/bin/Debug/Editor.exe --backend vulkan
```

- [ ] **Step 2: RenderDoc 捕获验证**

使用 RenderDoc 捕获帧，检查渲染输出是否正确。

---

## 验证清单

- [ ] Task 1: RHIMesh.h 创建成功
- [ ] Task 2: RHIMeshAdapter 实现完成
- [ ] Task 3: IMesh::GetRHIMesh() 可用
- [ ] Task 4: Material Formal RHI 方法 public
- [ ] Task 5: ForwardSceneRenderer 直接录制
- [ ] Task 6: DeferredSceneRenderer 直接录制
- [ ] Task 7: ABaseRenderer legacy 移除
- [ ] Task 8: Driver::SubmitMaterialDraw 简化
- [ ] Task 9: 构建通过，测试通过
- [ ] Task 10: Editor 运行正常

---

## 风险与注意事项

1. **DX11/OpenGL 后端**: 本计划不包括 DX11/OpenGL 的 Formal RHI 后端重写，它们仍然通过 legacy 路径工作
2. **Shader 兼容性**: 确保所有 shader 在新录制路径下正常工作
3. **Frame Graph 资源屏障**: 检查 Formal RHI 路径下的资源状态转换