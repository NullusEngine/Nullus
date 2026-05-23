# Implementation Plan: 多线程渲染框架

**Branch**: `006-multi-thread-rendering` | **Date**: 2026-04-18 | **Spec**: [spec.md](./spec.md)
**Input**: Feature specification from `D:/VSProject/Nullus/specs/006-multi-thread-rendering/spec.md`

**Note**: This template is filled in by the `/speckit.plan` command. See `.specify/templates/plan-template.md` for the execution workflow.

## Summary

在不绕开当前 `Driver`、显式 RHI、场景渲染器与编辑器渲染入口的前提下，引入三阶段渲染编排框架：逻辑主线程负责生成不可变帧快照，渲染场景线程负责从快照生成可提交的渲染场景包，RHI 线程负责命令录制、队列提交、同步、呈现与帧退休。整体方案使用有界 in-flight frame slots 管理跨线程所有权，并把 resize、shutdown、offscreen 输出、编辑器辅助通路都纳入同一帧生命周期。

## Technical Context

**Language/Version**: C++20
**Primary Dependencies**: Nullus `Runtime/Rendering` 与 `Runtime/Engine/Rendering` 模块、Formal RHI、FrameGraph、编辑器渲染辅助路径、GoogleTest/CTest
**Storage**: 不涉及持久化存储；本特性只引入运行时瞬态帧数据、线程间移交状态和 GPU 同步生命周期
**Testing**: `NullusUnitTests` + `ctest`、聚焦渲染单测、Editor/Game 冒烟验证、对可视正确性声明使用 RenderDoc 证据
**Target Platform**: 桌面 Editor/Game 运行时；当前验证重点仍是 Windows 上的 DX12 / Vulkan，同时计划不得把一个后端的结论外推到其他后端
**Project Type**: 多后端桌面游戏引擎与编辑器
**Performance Goals**: 逻辑主线程不再直接承担后端提交与 present；帧移交必须为有界队列；正常交互场景下应保持现有运行流畅性，不引入无界积压
**Constraints**: `Driver` 继续作为图形入口；`Runtime/*/Gen/` 禁止手改；不得把后端特例泄漏回 renderer/editor/game 主线；Editor 与 Game 在分阶段迁移中必须保持可运行；resize 与 shutdown 必须先 drain 再回收
**Scale/Scope**: 覆盖 `Runtime/Rendering/Context`、`Runtime/Rendering/Core`、`Runtime/Rendering/FrameGraph`、`Runtime/Rendering/RHI`、`Runtime/Engine/Rendering`、`Project/Editor/Rendering` 与聚焦测试/验证文档

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- **Spec-first major change**: PASS。该特性属于渲染架构变更，且已在 `D:/VSProject/Nullus/specs/006-multi-thread-rendering/` 下拥有完整 spec bundle。
- **Validation matches subsystem**: PASS。计划中的验证路径明确包含渲染单测、Editor/Game 冒烟验证，以及对图形正确性声明使用 RenderDoc。
- **Generated code and backend boundaries**: PASS。设计明确排除 `Runtime/*/Gen/` 手改，并保持后端差异留在 RHI/backend 内部。
- **Incremental, verified delivery**: PASS。方案被拆分为线程职责、帧移交、资源退休、resize/shutdown、编辑器兼容几个可独立验证的子问题。
- **Product runtime preservation**: PASS。Editor 与 Game 必须在每个迁移切片后仍然可运行，且不能靠隐式回退掩盖问题。

## Project Structure

### Documentation (this feature)

```text
D:/VSProject/Nullus/specs/006-multi-thread-rendering/
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── contracts/
│   └── threaded-rendering-lifecycle-contract.md
└── tasks.md
```

### Source Code (repository root)

```text
D:/VSProject/Nullus/Runtime/Rendering/Context/
├── Driver.h
├── Driver.cpp
└── DriverAccess.h

D:/VSProject/Nullus/Runtime/Rendering/Core/
├── ABaseRenderer.h
├── ABaseRenderer.cpp
├── CompositeRenderer.h
├── CompositeRenderer.cpp
├── FrameObjectBindingProvider.h
└── FrameObjectBindingProvider.cpp

D:/VSProject/Nullus/Runtime/Rendering/FrameGraph/
├── FrameGraphExecutionContext.h
├── FrameGraphTexture.h
└── FrameGraphBuffer.h

D:/VSProject/Nullus/Runtime/Rendering/RHI/
├── RHI.h
├── RHITypes.h
└── Core/
   ├── RHISwapchain.h
   ├── RHICommandList.h
   └── RHICommandListExecutor.h

D:/VSProject/Nullus/Runtime/Engine/Rendering/
├── BaseSceneRenderer.h
├── BaseSceneRenderer.cpp
├── ForwardSceneRenderer.h
├── ForwardSceneRenderer.cpp
├── DeferredSceneRenderer.h
├── DeferredSceneRenderer.cpp
├── EngineFrameObjectBindingProvider.h
├── EngineFrameObjectBindingProvider.cpp
├── SceneLightingProvider.h
└── SceneLightingProvider.cpp

D:/VSProject/Nullus/Project/Editor/Rendering/
├── DebugSceneRenderer.h
├── DebugSceneRenderer.cpp
├── GridRenderPass.h
├── GridRenderPass.cpp
├── OutlineRenderer.h
├── OutlineRenderer.cpp
├── GizmoRenderer.h
└── GizmoRenderer.cpp

D:/VSProject/Nullus/Tests/Unit/
├── DriverSwapchainResizeTests.cpp
├── DriverNullDeviceFallbackTests.cpp
├── CompositeRendererExplicitDrawOrderTests.cpp
├── RendererFrameObjectBindingTests.cpp
├── RendererStatsTests.cpp
├── LightingDataProviderTests.cpp
└── DebugDrawPassTests.cpp
```

**Structure Decision**: 本次变更保持在现有渲染分层内完成，不引入新的并行工作流或旁路入口。线程编排与帧生命周期优先落在 `Runtime/Rendering/Context` 和 `Runtime/Rendering/Core`，场景准备逻辑留在 `Runtime/Engine/Rendering`，编辑器兼容约束在 `Project/Editor/Rendering` 中按现有消费者逐步接入。

## Complexity Tracking

No constitution violations require justification.

## Phase 0: Research Output

Phase 0 研究结论记录在 `D:/VSProject/Nullus/specs/006-multi-thread-rendering/research.md`，已覆盖以下关键未知项：

- 三线程之间的帧移交边界
- 有界 in-flight 帧槽与背压策略
- RHI 线程对资源退休、resize、shutdown 的所有权
- Editor/Game/offscreen 输出在同一生命周期中的兼容方式
- 验证策略与当前仓库测试入口的对接方式

## Phase 1: Design Output

Phase 1 设计产物记录在：

- `D:/VSProject/Nullus/specs/006-multi-thread-rendering/data-model.md`
- `D:/VSProject/Nullus/specs/006-multi-thread-rendering/contracts/threaded-rendering-lifecycle-contract.md`
- `D:/VSProject/Nullus/specs/006-multi-thread-rendering/quickstart.md`

## Phase 2: Implementation Strategy

后续 `/speckit.tasks` 应按以下顺序拆任务，确保每一步都可回归验证：

1. 建立三阶段生命周期与最小可运行的帧槽模型，但先不扩大功能面。
2. 把逻辑线程到渲染场景线程的快照边界落稳，切断对 live scene 读写的直接依赖。
3. 把 RHI 线程的命令录制、提交、present、retire 所有权集中起来。
4. 处理 offscreen、swapchain、resize、shutdown 的特殊路径。
5. 接入 Editor/Game 消费者与现有 renderer-owned 绑定、lighting、stats。
6. 用单测、冒烟与 RenderDoc 证据收口，并清理临时兼容状态。

## Post-Design Constitution Check

- **Spec-first major change**: PASS。plan、research、data model、contract、quickstart 全部位于同一 spec bundle。
- **Validation matches subsystem**: PASS。quickstart 明确要求 `ctest`、聚焦渲染单测、Editor/Game 冒烟，以及仅在需要可视证据时使用 RenderDoc。
- **Generated code and backend boundaries**: PASS。设计要求后端提交与同步继续留在 RHI 线程和 backend 内部，不通过 renderer 层扩散后端特例。
- **Incremental, verified delivery**: PASS。数据模型和 contract 明确了可独立完成的帧生命周期切片与完成判定。
- **Product runtime preservation**: PASS。设计要求 Editor、Game、offscreen 与 resize/shutdown 都走同一生命周期，并在阶段迁移期间保持产品路径可运行。
