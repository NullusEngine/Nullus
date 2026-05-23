# Tasks: 多线程渲染框架

**Input**: Design documents from `D:/VSProject/Nullus/specs/006-multi-thread-rendering/`
**Prerequisites**: `plan.md`, `spec.md`, `research.md`, `data-model.md`, `contracts/threaded-rendering-lifecycle-contract.md`, `quickstart.md`

**Tests**: 本特性在 spec、plan 和 quickstart 中都明确要求验证证据，因此每个用户故事都包含聚焦测试任务以及对应的运行时验证任务。

**Organization**: 任务按用户故事分组，确保每个故事都能独立实现、独立验证，并且可以按优先级增量交付。

## Format: `[ID] [P?] [Story] Description`

- **[P]**: 可并行执行（不同文件、无未完成前置依赖）
- **[Story]**: 任务所属用户故事（`[US1]`, `[US2]`, `[US3]`, `[US4]`）
- 每条任务都带精确文件路径

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: 建立多线程渲染模式的共享入口、配置和测试挂接点。

- [X] T001 在 `Runtime/Rendering/Settings/DriverSettings.h` 和 `Project/Launcher/Core/Launcher.cpp` 中添加多线程渲染模式与帧槽数量配置入口
- [X] T002 [P] 在 `Tests/Unit/CMakeLists.txt` 和 `Tests/Unit/ThreadedRenderingLifecycleTests.cpp` 中添加多线程渲染生命周期测试入口

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: 建立三阶段帧生命周期、公共状态模型和共享诊断，这些内容会阻塞所有用户故事

**⚠️ CRITICAL**: 这一阶段完成前，不应开始任何用户故事实现

- [X] T003 在 `Runtime/Rendering/Context/ThreadedRenderingLifecycle.h` 和 `Runtime/Rendering/Context/ThreadedRenderingLifecycle.cpp` 中定义 `FrameSnapshot`、`RenderScenePackage`、`RhiSubmissionFrame`、`InFlightFrameSlot` 和退休状态枚举
- [X] T004 在 `Runtime/Rendering/Context/Driver.h`、`Runtime/Rendering/Context/Driver.cpp` 和 `Runtime/Rendering/Context/DriverAccess.h` 中建立三阶段 worker 启停、槽位存储和跨阶段移交骨架
- [X] T005 [P] 在 `Tests/Unit/ThreadedRenderingLifecycleTests.cpp` 中添加帧槽复用、退休前不可覆盖和背压可观测性的基础回归测试
- [X] T006 [P] 在 `Runtime/Rendering/Core/RendererStats.h`、`Runtime/Rendering/Core/RendererStats.cpp` 和 `Project/Editor/Panels/FrameInfoRendererStats.cpp` 中接入阶段状态、in-flight 深度和背压诊断字段

**Checkpoint**: 三阶段生命周期骨架、公共状态对象和基础诊断已经具备，用户故事可以开始逐步接入

---

## Phase 3: User Story 1 - 保持逻辑主线程响应性 (Priority: P1) 🎯 MVP

**Goal**: 让逻辑主线程只负责发布不可变帧快照，不再直接承担 backend submit / present 工作。

**Independent Test**: 运行一个代表性场景，确认逻辑线程能够持续推进；当下游滞后时，系统对快照发布施加有界背压并报告状态，而不是在逻辑线程上直接执行提交和呈现。

### Tests for User Story 1

- [X] T007 [P] [US1] 在 `Tests/Unit/ThreadedRenderingLifecycleTests.cpp` 中添加逻辑线程只能发布快照、不能直接提交或呈现的失败用例
- [X] T008 [P] [US1] 在 `Tests/Unit/CompositeRendererExplicitDrawOrderTests.cpp` 中添加 `ABaseRenderer` 改为快照发布路径后的回归覆盖

### Implementation for User Story 1

- [X] T009 [US1] 在 `Runtime/Rendering/Core/ABaseRenderer.h`、`Runtime/Rendering/Core/ABaseRenderer.cpp` 和 `Runtime/Engine/Rendering/BaseSceneRenderer.cpp` 中实现从 `FrameDescriptor` 与场景输入构建不可变 `FrameSnapshot`
- [X] T010a [US1] 在 `Runtime/Rendering/Core/ABaseRenderer.h` 和 `Runtime/Rendering/Core/ABaseRenderer.cpp` 中建立快照发布路径，并保留 runtime 可见帧 direct submit fallback 以恢复画面
- [X] T010b [US1] 在 `Runtime/Rendering/Core/ABaseRenderer.h` 和 `Runtime/Rendering/Core/ABaseRenderer.cpp` 中移除 runtime 可见帧 direct submit fallback，使逻辑主线程只发布快照并等待退休信号
- [X] T010c [US1] 在 `Runtime/Rendering/Settings/DriverSettings.h`、`Runtime/Rendering/Context/DriverAccess.h`、`Runtime/Rendering/Context/Driver.cpp`、`Runtime/Rendering/Core/ABaseRenderer.cpp` 和 `Tests/Unit/ThreadedRenderingLifecycleTests.cpp` 中把 threaded runtime direct submit fallback 显式门控化，允许在测试和产品层显式切换
- [X] T010d [US1] 在 `Runtime/Rendering/Settings/DriverSettings.h`、`Runtime/Rendering/Context/Driver.cpp`、`Project/Launcher/Core/Launcher.cpp` 和 `Tests/Unit/ThreadedRenderingLifecycleTests.cpp` 中将 threaded runtime direct submit fallback 默认切换为关闭，并保留 opt-in 兼容回归覆盖
- [X] T011 [US1] 在 `Runtime/Rendering/Context/Driver.cpp` 和 `Runtime/Rendering/Core/RendererStats.cpp` 中实现有界发布、背压节流和被阻塞帧的诊断记录
- [X] T012 [US1] 在 `Project/Editor/Panels/FrameInfo.h` 和 `Project/Editor/Panels/FrameInfoRendererStats.cpp` 中展示逻辑发布状态、in-flight 深度与被背压帧信息

**Checkpoint**: 逻辑发布骨架、背压诊断和无 fallback 的 runtime 可见帧快照发布路径已经具备；逻辑主线程不再直接承担 runtime backend submit / present

---

## Phase 4: User Story 2 - 在渲染场景线程上准备场景包 (Priority: P1)

**Goal**: 让渲染场景线程只消费不可变快照，并生成完整的 `RenderScenePackage`，不再回读 live scene。

**Independent Test**: 向渲染器输入包含 opaque、transparent、skybox、offscreen 与 editor helper 的场景，确认渲染场景线程能仅基于快照生成完整场景包，并在空场景/无光场景下安全产出最小包。

### Tests for User Story 2

- [X] T013 [P] [US2] 在 `Tests/Unit/ThreadedRenderingLifecycleTests.cpp` 中添加渲染场景线程只能消费快照、不能回读 live state 的失败用例
- [X] T014 [P] [US2] 在 `Tests/Unit/RendererFrameObjectBindingTests.cpp` 和 `Tests/Unit/LightingDataProviderTests.cpp` 中添加场景包准备、frame/object data 和 lighting 准备阶段的回归覆盖

### Implementation for User Story 2

- [X] T015 [US2] 在 `Runtime/Engine/Rendering/BaseSceneRenderer.h` 和 `Runtime/Engine/Rendering/BaseSceneRenderer.cpp` 中实现 `RenderScenePackage` 组装、可见性收集和 pass 计划输出
- [X] T015a [US2] 在 `Runtime/Rendering/Context/ThreadedRenderingLifecycle.h`、`Runtime/Engine/Rendering/BaseSceneRenderer.cpp`、`Runtime/Rendering/Context/Driver.cpp` 以及 `Tests/Unit/RendererFrameObjectBindingTests.cpp`、`Tests/Unit/LightingDataProviderTests.cpp`、`Tests/Unit/ThreadedRenderingLifecycleTests.cpp` 中为 `RenderScenePackage` 增加 `drawCommandCount`、`materialBatchCount`、`renderTargetUseCount` 和 `containsCommandInputs` 中间 surface
- [X] T016 [US2] 在 `Runtime/Engine/Rendering/ForwardSceneRenderer.cpp` 和 `Runtime/Engine/Rendering/DeferredSceneRenderer.cpp` 中重构场景渲染器，使其只消费 snapshot-owned 输入并输出场景包数据
- [X] T017 [US2] 在 `Runtime/Engine/Rendering/EngineFrameObjectBindingProvider.cpp` 和 `Runtime/Engine/Rendering/SceneLightingProvider.cpp` 中把 frame/object data 与 lighting publication 的准备阶段迁移到渲染场景线程
- [X] T018 [US2] 在 `Runtime/Rendering/Context/Driver.cpp` 中接入渲染场景 worker 循环、场景包完成状态和到 RHI 阶段的槽位移交

**Checkpoint**: 场景包摘要、frame/object data、lighting 准备阶段和中间 command-input surface 已接入；真实 draw/pass 输入仍依赖现有 live scene 解析，渲染场景线程完全脱离 live scene 尚未完成

---

## Phase 5: User Story 3 - 在专用 RHI 线程上提交 GPU 工作 (Priority: P2)

**Goal**: 让命令录制、提交、同步、present 和退休都由 RHI 线程独占。

**Independent Test**: 运行受支持后端的 smoke scene，确认命令录制、队列提交、swapchain acquire/present、offscreen-only 帧退休和 shutdown drain 全都发生在 RHI 线程。

### Tests for User Story 3

- [X] T019 [P] [US3] 在 `Tests/Unit/ThreadedRenderingLifecycleTests.cpp` 中添加 RHI 线程独占 submit / present / retire 的失败用例
- [X] T020 [P] [US3] 在 `Tests/Unit/DriverSwapchainResizeTests.cpp` 和 `Tests/Unit/DriverNullDeviceFallbackTests.cpp` 中添加 resize、offscreen-only 和退休路径的回归覆盖

### Implementation for User Story 3

- [X] T021a [US3] 在 `Runtime/Rendering/Context/Driver.h`、`Runtime/Rendering/Context/Driver.cpp` 和 `Runtime/Rendering/RHI/Core/RHISwapchain.h` 中建立 RHI worker 生命周期骨架、mock submit/present 测试和 `RhiSubmissionFrame` 退休状态推进
- [X] T021b [US3] 将真实 `RenderScenePackage` 转换为 RHI 线程可录制的 draw/pass command inputs，并由 RHI worker 录制非空可见帧命令
- [X] T021c [US3] 在 `Runtime/Rendering/Context/ThreadedRenderingLifecycle.h`、`Runtime/Rendering/Context/Driver.cpp` 和 `Tests/Unit/ThreadedRenderingLifecycleTests.cpp` 中为 RHI submission 记录 package-backed visible work / draw count 中间诊断
- [X] T021d [US3] 在 `Runtime/Rendering/Context/ThreadedRenderingLifecycle.h`、`Runtime/Engine/Rendering/BaseSceneRenderer.cpp`、`Runtime/Rendering/Context/Driver.cpp` 和 `Tests/Unit/ThreadedRenderingLifecycleTests.cpp` 中把 pass command inputs 扩展为可录制的 render pass plan，并驱动 mock command buffer 执行 `BeginRenderPass/SetViewport/EndRenderPass`
- [X] T022a [US3] 在 `Runtime/Rendering/Context/DriverAccess.h` 和 `Runtime/Rendering/Context/Driver.cpp` 中建立 threaded RHI worker 访问路径和 direct present 防重入保护
- [X] T022b [US3] 移除运行时可见帧 direct submit fallback，使 command buffer 获取、queue submit、swapchain acquire/present 只由 RHI 线程执行
- [X] T022c [US3] 在 `Tests/Unit/ThreadedRenderingLifecycleTests.cpp` 中验证禁用 direct submit fallback 时，swapchain 可见帧仍可通过 RHI worker 录制 pass、提交并 present
- [X] T023 [US3] 在 `Runtime/Rendering/FrameGraph/FrameGraphExecutionContext.h`、`Runtime/Rendering/RHI/Core/RHISwapchain.h` 和 `Runtime/Rendering/Context/Driver.cpp` 中把退休信号与 per-frame 资源复用绑定起来
- [X] T024 [US3] 在 `Runtime/Rendering/Core/ABaseRenderer.cpp` 和 `Runtime/Rendering/Context/Driver.cpp` 中实现 `OffscreenOnly` 帧的无 present 提交/退休路径

**Checkpoint**: RHI worker 生命周期骨架、mock 后端提交/退休和 offscreen/swapchain 状态推进已验证；runtime 可见帧默认且唯一地走 RHI-thread-only 提交路径

---

## Phase 6: User Story 4 - 保持 Editor / Runtime 可见行为稳定 (Priority: P3)

**Goal**: 在引入三线程生命周期后，保持 Editor、Game、辅助渲染、resize 和 shutdown 路径仍然正确。

**Independent Test**: 启动 Editor 和 Game，验证 scene view、game view、offscreen 面板、grid / gizmo / outline / debug helper、窗口 resize 和退出流程都仍能正确工作。

### Tests for User Story 4

- [X] T025 [P] [US4] 在 `Tests/Unit/PanelWindowHookTests.cpp` 和 `Tests/Unit/DebugDrawPassTests.cpp` 中添加编辑器视图与辅助路径在多线程生命周期下的失败用例
- [X] T026 [P] [US4] 在 `Tests/Unit/ThreadedRenderingLifecycleTests.cpp` 中添加 resize drain 与 shutdown drain 的失败用例

### Implementation for User Story 4

- [X] T027 [US4] 在 `Project/Editor/Panels/GameView.cpp`、`Project/Editor/Panels/SceneView.cpp` 和 `Project/Editor/Panels/AssetView.cpp` 中把视图消费者接入新的帧生命周期与退休模型
- [X] T028 [US4] 在 `Project/Editor/Rendering/DebugSceneRenderer.cpp`、`Project/Editor/Rendering/GridRenderPass.cpp`、`Project/Editor/Rendering/GizmoRenderer.cpp` 和 `Project/Editor/Rendering/OutlineRenderer.cpp` 中适配编辑器辅助渲染到新的生命周期
- [X] T029 [US4] 在 `Runtime/Rendering/Context/Driver.cpp` 和 `Project/Launcher/Core/Launcher.cpp` 中实现 Editor / Game 的 resize drain、shutdown drain 和恢复逻辑
- [X] T030 [US4] 在 `Project/Editor/Panels/FrameInfoRendererStats.cpp` 和 `Project/Editor/Panels/FrameInfo.cpp` 中接入用户可见的阶段诊断、in-flight 深度和帧退休状态展示

**Checkpoint**: Editor、Game、offscreen、辅助渲染、resize 和 shutdown 在当前 fallback-enabled threaded 模式下完成验证；无 fallback 的完整 RHI-thread-only 路径尚未完成

---

## Phase 7: Polish & Cross-Cutting Concerns

**Purpose**: 收口跨故事问题、补齐文档与完整验证路径

- [X] T031 [P] 在 `specs/006-multi-thread-rendering/quickstart.md` 和 `specs/006-multi-thread-rendering/contracts/threaded-rendering-lifecycle-contract.md` 中更新最终实现后的验证步骤与生命周期边界说明
- [X] T032a 在 `Runtime/Rendering/Core/ABaseRenderer.cpp`、`Runtime/Rendering/Context/Driver.cpp` 和 `Project/Launcher/Core/Launcher.cpp` 中审计产品层 submit / present 入口，并阻止 threaded 模式下无 standalone UI frame 的额外 main-thread present
- [X] T032b 在 `Runtime/Rendering/Core/ABaseRenderer.cpp`、`Runtime/Rendering/Context/Driver.cpp` 和 `Project/Launcher/Core/Launcher.cpp` 中移除 runtime direct submit fallback 对同线程 submit / present 的剩余依赖
- [X] T032d 在 `Runtime/Rendering/Settings/DriverSettings.h`、`Runtime/Rendering/Context/Driver.cpp`、`Project/Launcher/Core/Launcher.cpp` 和 `Tests/Unit/ThreadedRenderingLifecycleTests.cpp` 中把 runtime direct submit fallback 的默认产品路径切换为关闭，并验证显式开启时仍可保留兼容 direct 路径
- [X] T032c 在 `Runtime/Rendering/Context/Driver.cpp`、`Runtime/Rendering/Context/DriverAccess.h` 和 `Tests/Unit/ThreadedRenderingLifecycleTests.cpp` 中加入 direct explicit frame ownership guard，禁止线程化帧在途时主线程重新占用 frame context
- [X] T033a 在 `specs/006-multi-thread-rendering/quickstart.md` 指定的命令和手工流程上运行 fallback-enabled threaded 验证并记录黑屏回归修复结果
- [ ] T033b 运行无 direct submit fallback 的完整 Editor/Game/RenderDoc 验证并记录结果

---

## Dependencies & Execution Order

### Phase Dependencies

- **Phase 1: Setup**: 无依赖，可立即开始
- **Phase 2: Foundational**: 依赖 Phase 1，阻塞全部用户故事
- **Phase 3: US1**: 依赖 Phase 2，是 MVP 基线
- **Phase 4: US2**: 依赖 Phase 2，建议在 US1 落稳后接入，因为它消费快照与共享槽位模型
- **Phase 5: US3**: 依赖 Phase 2，建议在 US2 输出场景包后接入，因为它消费完整 `RenderScenePackage`
- **Phase 6: US4**: 依赖 Phase 2，建议在 US1/US2/US3 三段生命周期贯通后收口产品路径
- **Phase 7: Polish**: 依赖所有目标用户故事完成

### User Story Dependencies

- **US1 (P1)**: 无其他用户故事依赖；这是建议 MVP 范围
- **US2 (P1)**: 依赖 Foundational，可在 US1 的快照发布模型稳定后推进
- **US3 (P2)**: 依赖 Foundational，且最好在 US2 能输出完整场景包之后推进
- **US4 (P3)**: 依赖 Foundational，并以 US1/US2/US3 的稳定生命周期为前提做产品收口

### Within Each User Story

- 先写测试并确认失败，再做实现
- 先建立状态对象和阶段边界，再迁移调用方
- 先打通生命周期，再做产品视图和辅助路径接线
- 在退休路径验证通过前，不要复用 per-frame 资源或输出目标

### Parallel Opportunities

- `T002` 可以和 `T001` 并行
- `T005` 和 `T006` 可以在 `T003` / `T004` 完成后并行
- **US1** 中 `T007` 和 `T008` 可以并行
- **US2** 中 `T013` 和 `T014` 可以并行
- **US3** 中 `T019` 和 `T020` 可以并行
- **US4** 中 `T025` 和 `T026` 可以并行
- 在 Foundational 完成且 MVP 基线落稳后，US2 / US3 / US4 可由不同成员并行推进

---

## Parallel Example: User Story 1

```text
T007 [US1] 在 Tests/Unit/ThreadedRenderingLifecycleTests.cpp 中添加逻辑线程发布边界失败用例
T008 [US1] 在 Tests/Unit/CompositeRendererExplicitDrawOrderTests.cpp 中添加快照发布路径回归覆盖
```

## Parallel Example: User Story 2

```text
T013 [US2] 在 Tests/Unit/ThreadedRenderingLifecycleTests.cpp 中添加渲染场景线程消费快照失败用例
T014 [US2] 在 Tests/Unit/RendererFrameObjectBindingTests.cpp 和 Tests/Unit/LightingDataProviderTests.cpp 中添加场景包准备回归覆盖
```

## Parallel Example: User Story 3

```text
T019 [US3] 在 Tests/Unit/ThreadedRenderingLifecycleTests.cpp 中添加 RHI 线程独占提交失败用例
T020 [US3] 在 Tests/Unit/DriverSwapchainResizeTests.cpp 和 Tests/Unit/DriverNullDeviceFallbackTests.cpp 中添加 resize/offscreen 退休回归覆盖
```

## Parallel Example: User Story 4

```text
T025 [US4] 在 Tests/Unit/PanelWindowHookTests.cpp 和 Tests/Unit/DebugDrawPassTests.cpp 中添加编辑器兼容失败用例
T026 [US4] 在 Tests/Unit/ThreadedRenderingLifecycleTests.cpp 中添加 resize/shutdown drain 失败用例
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. 完成 Phase 1 与 Phase 2
2. 完成 US1，让逻辑线程只负责发布快照
3. 按 `specs/006-multi-thread-rendering/quickstart.md` 运行聚焦测试和最小场景验证
4. 在确认逻辑线程不再直接 submit / present 后再进入后续故事

### Incremental Delivery

1. 先建立共享生命周期骨架与诊断
2. 交付 **US1**，获得最小可验证多线程基线
3. 交付 **US2**，让场景准备从 live scene 脱钩
4. 交付 **US3**，把 backend 提交与退休集中到 RHI 线程
5. 交付 **US4**，收口 Editor / Game / offscreen / resize / shutdown 路径
6. 通过 Polish 阶段做完整验证和文档收尾

### Parallel Team Strategy

1. 团队共同完成 Phase 1 和 Phase 2
2. 先由核心成员完成 US1，建立统一生命周期边界
3. US1 落稳后并行分工：
   - 开发者 A：US2 场景包准备
   - 开发者 B：US3 RHI 线程提交与退休
   - 开发者 C：US4 Editor / Runtime 收口与可见验证
4. 最后一起完成 Polish 和完整验证

---

## Notes

- `[P]` 任务代表文件和职责边界可并行推进
- 用户故事任务都带 `[USn]` 标签，便于追踪 spec 覆盖
- 每个故事都必须在进入下一故事前完成一次独立验证
- 不要手改 `Runtime/*/Gen/`
- 不要引入新的产品层旁路去直接操作 backend submit / present
