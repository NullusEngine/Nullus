# Contract: 多线程渲染生命周期边界

## Purpose

定义逻辑主线程、渲染场景线程和 RHI 线程之间的帧生命周期、所有权边界、背压行为以及 resize / shutdown / offscreen 特殊路径的统一规则。

## Scope

本 contract 约束以下范围：

- 逻辑帧到渲染帧的发布边界
- 场景准备与 draw/package 组装
- backend-facing 命令录制、提交、同步与 present
- per-frame 资源、render target、临时上传资源的退休与复用
- Editor / Game / offscreen 渲染路径的统一生命周期

## Implementation Status

当前实现状态：

- 已实现：三阶段 slot 生命周期、基础 telemetry、render scene / RHI worker 骨架、resize/shutdown drain 测试覆盖。
- 已实现：DX12 Game/Editor 可见性 fallback，防止线程化迁移期间的黑屏回归。
- 已实现：threaded 模式下无 standalone UI frame 的主线程额外 present 防重入，避免和 worker swapchain 生命周期冲突。
- 未完成：真实场景 draw/pass command buffer 由 RHI 线程录制并提交。
- 未完成：移除运行时可见帧 direct submit fallback。
- 未完成：`Driver` frame context / swapchain / `currentFrameIndex` 的跨线程所有权收敛。
- 未完成：渲染场景线程完全停止回读 live gameplay/editor scene state。

在未完成项落地前，Validation Acceptance Rules 中的 RHI-thread-only、render-scene-snapshot-only 和 logic-thread-no-submit 条款不得视为完全满足。

## Thread Ownership Rules

### 1. 逻辑主线程

逻辑主线程负责：

- 更新 gameplay 与 editor 活状态
- 生成当前帧的 `Frame Snapshot`
- 在允许发布时把快照移交给渲染场景线程

逻辑主线程不得：

- 直接执行 backend submit
- 直接执行 swapchain present
- 在快照发布后继续修改该快照内容

### 2. 渲染场景线程

渲染场景线程负责：

- 基于不可变快照做场景解析、可见性收集和 draw 排序
- 组装 pass 顺序、目标使用意图和 `Render Scene Package`
- 准备 renderer-owned frame/object data、lighting、stats 所需的每帧输入

渲染场景线程不得：

- 回读 live gameplay/editor 状态补齐包内容
- 执行 swapchain acquire/present
- 释放仍由 RHI 线程拥有的 per-frame 资源

### 3. RHI 线程

RHI 线程负责：

- 命令录制
- 队列提交与同步
- swapchain acquire / present
- frame retirement
- resize drain 与 shutdown drain

RHI 线程不得：

- 回写 gameplay/editor 活状态
- 在未退休前允许外部复用对应帧槽资源

## Frame Handoff Rules

单帧必须按如下顺序推进：

1. 逻辑主线程构建 `Frame Snapshot`
2. 快照发布到可用 `In-Flight Frame Slot`
3. 渲染场景线程消费快照并生成 `Render Scene Package`
4. RHI 线程消费场景包并生成 `RHI Submission Frame`
5. RHI 线程完成提交、必要时 present
6. RHI 线程发出 `Frame Retirement Signal`
7. 帧槽与关联数据才允许被复用

任何阶段都不得跳过退休直接复用下游仍持有的数据。

### 生命周期摘要暴露

实现可以在内部使用更细粒度的 slot 状态，但对产品层和诊断层至少要暴露以下稳定摘要：

- `Publish State`: `Direct` / `Open` / `BackPressured`
- `Frame Stage`: `Direct` / `Logic` / `RenderScene` / `RHI` / `Retired`
- `Retirement State`: `Direct` / `Pending` / `Ready` / `Consumed`

这些摘要必须能让用户区分：

- 当前帧主要停留在哪个线程所有权阶段
- 该帧是否还在等待退休
- 该帧是否已经进入可复用状态

## Back-Pressure Rules

- 帧槽数量必须有上限。
- 当没有可发布槽位时，默认行为是对逻辑发布边界施加节流。
- 节流必须可观测，至少能够区分“正常推进”“被背压阻塞”“进入 drain”。
- 不允许静默丢弃需要渲染的帧来掩盖下游滞后。
- 当存在 pending swapchain resize 且旧帧尚未 drain 时，新的 swapchain frame snapshot 必须被阻止发布。
- drain 完成后，只有在 pending resize 已真正应用并释放旧输出资源之后，才允许恢复 swapchain 帧发布。

## Resource Lifetime Rules

- `Frame Snapshot` 在退休前不得释放或覆盖。
- `Render Scene Package` 在 RHI 线程开始消费后不得被上游线程修改。
- per-frame descriptor、upload、render target、frame context 资源只能在对应帧退休后复用。
- resize 不能发生在仍有帧持有旧 swapchain/backbuffer 资源时。

## Output Mode Rules

### SwapchainPresent

- 必须由 RHI 线程负责 acquire、submit、present。
- present 完成前，该帧的 swapchain 相关资源不得复用。

### OffscreenOnly

- 不需要 present。
- 仍必须经过 submit 与 retirement，之后才能复用目标资源。

## Resize And Shutdown Rules

### Resize

1. 停止向受影响输出继续发布新帧
2. drain 在途帧
3. 回收旧输出相关资源
4. 以新尺寸恢复发布

当前实现约束：

- `Driver` 在 threaded 模式下会阻止新的 swapchain snapshot 发布，直到 in-flight 深度归零
- 恢复发布前必须先执行 pending resize，并释放旧 `swapchainBackbufferView`
- offscreen-only 输出不受 swapchain resize drain 阻塞

### Shutdown

1. 停止接受新快照
2. 允许渲染场景线程结束当前工作或取消未提交工作
3. 由 RHI 线程完成提交中帧的退休
4. 统一释放剩余资源

当前实现约束：

- `StopThreadedRenderingWorkers()` 在 join worker 后会同步 drain 剩余生命周期
- shutdown drain 必须覆盖已发布但尚未 submit / present / retire 的帧
- 在 drain 结束前，不允许销毁 device、swapchain 或 frame slot 关联资源

不允许在未 drain 的情况下直接销毁 device、swapchain 或 frame slot 资源。

## Validation Acceptance Rules

只有同时满足以下条件，本 contract 才算被实现满足：

- 逻辑线程不再直接承担 backend submit / present
- 渲染场景线程只消费不可变快照
- RHI 线程独占 acquire / submit / present / retire
- 背压行为可被观察和记录
- offscreen 路径与 swapchain 路径共用同一帧退休模型
- resize 与 shutdown 都经过明确 drain
- Editor、Game 与编辑器辅助路径仍可运行
- 用户可通过 `FrameInfo` 面板读取 `Publish State`、`Frame Stage`、`Retirement State`、`in-flight depth`

当前 fallback-enabled 验证只能证明可见行为和生命周期骨架稳定，不能作为完整 RHI-thread-only 交付证据。

## Out Of Scope

本 contract 不负责重新设计：

- 异步资源流送系统
- 全局 job system
- shader variant 体系
- backend capability policy 本身
