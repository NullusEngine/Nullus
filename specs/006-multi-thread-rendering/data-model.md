# Data Model: 多线程渲染框架

## Overview

该特性不引入新的持久化项目数据。数据模型描述的是运行时帧生命周期、跨线程所有权和验证可见的状态边界。

## Entities

### 1. Frame Snapshot

**Purpose**: 作为逻辑主线程发布给渲染场景线程的不可变帧输入。

**Fields**:

- `frameId` (uint64): 逻辑帧唯一编号。
- `sceneRevision` (uint64): 生成快照时对应的场景修订号。
- `cameraState` (object): 当前帧 camera 的视图、投影、清屏和输出描述。
- `rendererMode` (enum): 当前使用的渲染路径，例如 forward、deferred、editor overlay 组合。
- `targetDescriptor` (object): swapchain 或 offscreen 输出目标描述。
- `debugSubmissionState` (object): 当前帧需要带入的 debug / editor helper 提交状态。
- `publishState` (enum: `Building`, `Published`, `Consumed`, `Retired`): 快照所处生命周期。

**Validation rules**:

- 进入 `Published` 后，快照内容必须不可再被逻辑线程修改。
- `cameraState` 与 `targetDescriptor` 必须来自同一逻辑帧。
- `Retired` 之前，快照不得被覆盖或复用。

**Relationships**:

- 由逻辑主线程创建。
- 被渲染场景线程消费，生成 `Render Scene Package`。

**State transitions**:

- `Building` -> `Published`：逻辑线程完成快照组装。
- `Published` -> `Consumed`：渲染场景线程开始处理。
- `Consumed` -> `Retired`：对应的 RHI 提交帧已经退休。

### 2. Render Scene Package

**Purpose**: 渲染场景线程基于快照生成的可提交渲染包。

**Fields**:

- `frameId` (uint64): 对应的逻辑帧编号。
- `visibleDrawSets` (object): opaque / transparent / skybox / helper 等可见 draw 集合。
- `passPlan` (ordered list): 本帧 pass 顺序与依赖意图。
- `frameDataState` (enum): frame-level data 是否准备完成。
- `objectDataState` (enum): object-level data 是否可在 draw 时被消费。
- `lightingState` (object): 本帧 lighting publication 结果。
- `statsSeed` (object): 统计收集需要的初始状态或预估信息。
- `packageState` (enum: `Preparing`, `ReadyForSubmit`, `Submitting`, `Retired`): 包状态。

**Validation rules**:

- `ReadyForSubmit` 前必须完成 draw 集合和 pass 计划。
- 不得依赖 live gameplay/editor 对象完成 `ReadyForSubmit` 之后的任何准备。
- 空场景或无光场景也必须产生合法包，而不是空指针状态。

**Relationships**:

- 来源于 `Frame Snapshot`。
- 被 RHI 线程消费，生成 `RHI Submission Frame`。

**State transitions**:

- `Preparing` -> `ReadyForSubmit`：渲染场景线程完成场景准备。
- `ReadyForSubmit` -> `Submitting`：RHI 线程开始消费。
- `Submitting` -> `Retired`：对应 GPU 工作退休。

### 3. RHI Submission Frame

**Purpose**: RHI 线程独占的后端提交单元。

**Fields**:

- `frameId` (uint64): 对应逻辑帧编号。
- `slotIndex` (uint32): 所属 in-flight frame slot。
- `outputMode` (enum: `SwapchainPresent`, `OffscreenOnly`): 帧输出模式。
- `recordingState` (enum: `Idle`, `Recording`, `Recorded`, `Submitted`, `Presented`, `Retired`): 后端生命周期。
- `syncState` (object): fence / semaphore / acquire / present 等同步状态摘要。
- `resourceUsage` (object): 本帧拥有的临时上传、render target、descriptor、frame context 资源。
- `retirementSignal` (token): 标识该帧何时允许复用槽位与资源。

**Validation rules**:

- 只有 RHI 线程能推进 `recordingState`。
- `Submitted` 之前不得释放本帧拥有的 GPU 相关资源。
- `OffscreenOnly` 帧不得触发 swapchain present。
- `Presented` 不是必选状态，`OffscreenOnly` 帧可由 `Submitted` 直接进入 `Retired`。

**Relationships**:

- 来自 `Render Scene Package`。
- 消耗 `In-Flight Frame Slot` 中的 per-frame 资源。

**State transitions**:

- `Idle` -> `Recording` -> `Recorded` -> `Submitted`
- `Submitted` -> `Presented`（仅 swapchain）
- `Submitted` / `Presented` -> `Retired`

### 4. In-Flight Frame Slot

**Purpose**: 作为三阶段共享的有界槽位，统一跟踪单帧从发布到退休的所有权。

**Fields**:

- `slotIndex` (uint32)
- `ownerStage` (enum: `Logic`, `RenderScene`, `RHI`, `Retired`)
- `snapshotRef` (optional): 当前槽位持有的 `Frame Snapshot`
- `scenePackageRef` (optional): 当前槽位持有的 `Render Scene Package`
- `submissionFrameRef` (optional): 当前槽位持有的 `RHI Submission Frame`
- `throttleState` (enum: `Open`, `BackPressured`, `Draining`)
- `lastTransitionFrameId` (uint64)

**Validation rules**:

- 同一时刻只能有一个 owner stage。
- 未退休槽位不得被逻辑线程重新占用。
- `BackPressured` 必须有可观察诊断，而不是静默阻塞。

**Relationships**:

- 挂接三阶段所有核心实体。
- 与现有 per-frame `RHIFrameContext` 一一对应或受其驱动。

**State transitions**:

- `Logic` -> `RenderScene` -> `RHI` -> `Retired`
- 任一阶段可进入 `Draining` 以支持 resize/shutdown

### 5. Frame Retirement Signal

**Purpose**: 表示某帧已经完全退出 GPU / swapchain / transient resource 占用，可安全复用。

**Fields**:

- `frameId` (uint64)
- `slotIndex` (uint32)
- `gpuComplete` (bool)
- `presentComplete` (bool)
- `transientReleased` (bool)
- `retireState` (enum: `Pending`, `Ready`, `Consumed`)

**Validation rules**:

- 只有在 `gpuComplete` 且需要时 `presentComplete` 后，才能进入 `Ready`。
- `Ready` 之前不得复用对应 slot 的临时资源。
- `Consumed` 之后该信号不可再次作为有效退休依据使用。

**Relationships**:

- 由 RHI 线程根据后端同步状态生成。
- 驱动 `Frame Snapshot`、`Render Scene Package` 和 `In-Flight Frame Slot` 的复用。

**State transitions**:

- `Pending` -> `Ready`：RHI 线程确认帧退休条件满足。
- `Ready` -> `Consumed`：槽位和关联数据被复用或释放。

### 6. Thread Ownership Contract

**Purpose**: 将可读、可写、可提交、可回收的线程权限显式化。

**Fields**:

- `logicThreadRights` (set): 允许逻辑线程执行的操作集合。
- `renderSceneThreadRights` (set): 允许渲染场景线程执行的操作集合。
- `rhiThreadRights` (set): 允许 RHI 线程执行的操作集合。
- `forbiddenCrossThreadActions` (set): 明确禁止跨线程执行的动作。
- `drainPolicy` (enum: `BlockPublish`, `DrainThenResize`, `DrainThenShutdown`)

**Validation rules**:

- 逻辑线程不得执行 backend submit / present。
- 渲染场景线程不得复用或销毁仍由 RHI 持有的 per-frame 资源。
- RHI 线程不得回写 gameplay/editor 活状态。

**Relationships**:

- 约束全部其他实体。
- 体现在 contract 文档和后续任务拆分中。

## Entity Relationships Summary

- `Frame Snapshot` 是逻辑线程对下游的唯一稳定输入。
- `Render Scene Package` 是场景准备结果，不能再回读 live scene。
- `RHI Submission Frame` 是后端执行与退休的唯一宿主。
- `In-Flight Frame Slot` 把三阶段所有权压缩到固定数量的槽位中。
- `Frame Retirement Signal` 决定何时可以 resize、shutdown、复用或释放资源。
- `Thread Ownership Contract` 为所有跨线程行为提供统一约束。
