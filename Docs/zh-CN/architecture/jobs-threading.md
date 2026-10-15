# 任务系统与线程模型

Nullus 有两套并行的调度设施：面向通用 CPU 工作的 **JobSystem**，和面向渲染管线与引擎阶段任务的 **TaskGraph**。前者解决“把工作分出去”，后者解决“阶段之间怎么交接”。

## JobSystem

`Runtime/Base/Jobs` 提供原生的调度接口。

### 初始化

```cpp
NLS::Base::Jobs::JobSystemConfig config;
config.workerCount = 4;                // kAutoJobWorkerCount 按硬件推导默认值
config.backgroundWorkerCount = 1;
NLS::Base::Jobs::InitializeJobSystem(config);
```

`TryInitializeJobSystem` 是所有权感知的变体：只有本次调用真正创建了运行时才返回 `true`。使用它时，只有在返回 `true` 的分支才应调用 `ShutdownJobSystem`。

`JobHandle{}` 始终是已完成状态。

### 调度形式

| 接口 | 用途 |
| --- | --- |
| `ScheduleJob` | 短 CPU 工作 |
| `ScheduleDifferentJobsConcurrent` | 一批互不相同的并发任务 |
| `ScheduleJobForEach` | 分批遍历，接受调用方控制的 `batchSize` |
| 原生 `IJob` / `IJobParallelFor` 适配器 | 面向 C++ 使用者的类型化封装 |
| `JobBatchDispatcher` | 批量派发与多依赖扇入 |

### 完成与取消

- `Complete` 等待并清空句柄；
- `CompleteNoClear` 保留调用方副本，便于后续查询；
- `HasBeenSynced` 报告句柄是否到达终止状态；
- 前台任务可提供取消回调，用于释放载荷；回调对“被立即关闭取消”或“依赖失败”的排队中工作最多执行一次；任务开始执行后不再触发取消回调。

### 批处理与扇入

`JobBatchDispatcher` 支持显式逐派发器触发、析构触发、自动触发阈值与多依赖扇入。派发器是调用方持有的批处理辅助对象，不是线程安全的；多个生产者必须自行串行化访问。

`IsCompleted` 与 `Complete` 都会让待处理批次对 worker 可见，再查询或等待。不存在全局的 `ScheduleBatchedJobs`，批处理可见性按派发器、扇入创建点或按句柄生效。

### 零 worker 与协助执行

- `workerCount = 0` 是显式的同步／测试模式；
- 完成等待可能在调用线程上执行合格任务以推进依赖；
- 等待只推进被等待栅栏依赖链上的任务，或标记为 `GuaranteedNoSyncWait` 的任务；
- `GuaranteedNoSyncWait` 回调在无同步等待作用域内执行，其中调用 `Complete` 会记录违规并立即返回而不阻塞。

### 已知限制

- `ScheduleJobForEach` 拒绝超出原生 `int` 规划范围的迭代次数或批次数；
- 前台与后台队列仍使用加锁的中央注册表，吞吐优化留待语义稳定之后；
- 跨队列依赖提升使用显式通知加有界定时回退，正确性不依赖回退，但高跨队列流量仍可能付出轮询延迟。

### 内存与数据安全

Job 回调拿到的是裸 `void*`，载荷生命周期由调用方负责，必须保持到句柄终止或清理回调执行完毕。回调可能在完成或关闭过程中执行，因此不能假设固定线程。

Nullus 不提供托管容器的竞态检查（例如 `NativeArray` 或安全句柄）。共享可变状态必须由调用方用原子、锁、不可变快照或依赖顺序保护。`JobSafetyPolicy` 只描述同步等待安全性，不证明内存安全。

回调不应抛出异常。若前台、后台、合并、取消或清理回调抛出，调度器在边界处捕获并记录 `JobViolationKind::CallbackException`，随后按文档化的失败或取消路径推进。

### 后台工作与主线程延续

- 长时或 IO 密集工作使用 `ScheduleBackgroundJob`；
- 主线程后续工作使用 `ScheduleMainThreadContinuation`，只在 `DrainMainThreadContinuations` 中执行；
- 就绪的延续被提升到独立就绪队列，避免每次排空都扫描全部阻塞中的延续；
- 编辑器更新循环每帧在延迟动作之后排空一个有界预算；
- 关闭不会隐式执行主线程延续，所有者需要在停止 JobSystem 之前排空必要延续。

编辑器的后台任务路径通过共享后台队列而不是私有线程池实现，并强制任务容量上限、拒绝停止后提交、在析构时完成已提交句柄。

### 诊断

`JobSystemConfig::enableDiagnostics` 打开有界快照采集；`DisallowJobSyncWaitScope` 在无等待作用域中被违反时记录结构化违规。

### 绑定接口

`JobBindings.h` 暴露最小的 C 兼容子集：`structSize`、`NLS_JOB_BINDING_VERSION`、不透明句柄 ID、命名优先级与安全常量、显式状态码。

- 运行已停止时，非默认的查询、完成与清空返回 `NLS_JOB_STATUS_NOT_INITIALIZED`；
- 取消的绑定句柄完成时返回 `NLS_JOB_STATUS_CANCELLED` 并清空句柄；
- 无法到达终止状态的完成尝试映射为 `NLS_JOB_STATUS_FAILED` 并保留句柄。

绑定结构体是纯 ABI 类型，不持有 STL 对象。原生 C++ 头文件属于同工具链 SDK 接口，不是稳定的跨编译器 ABI。

## TaskGraph

TaskGraph 承担渲染调度与引擎阶段任务：

- 每个 worker 维护按优先级划分的本地队列；
- 外部提交进入共享注入队列；
- 空闲 worker 执行 work stealing；
- `TaskCompletion::Wait()` 在 worker 上协助执行可运行任务，避免嵌套等待死锁；
- `RequestCancel` 沿依赖图传播并唤醒全部等待者。

## 执行通道

渲染使用三条逻辑通道：

| 通道 | 职责 | 判定 |
| --- | --- | --- |
| Game | 产生命令 | `IsInGameThread()` |
| Render | 消费命令并构建帧 | `IsInRenderingThread()` |
| RHI | 翻译与提交原生命令缓冲 | `IsInRhiThread()` |

Render 与 RHI 启动后附着到 TaskGraph。`tasks` 模式下 RHI 通道由 TLS 执行上下文表示，不依赖固定 OS 线程。

## 通道之间的交接

```text
Game Thread
  EnqueueRenderCommand（严格 FIFO 串行队列）
       |  场景代理增删改、帧渲染、resize、资源释放
       v
Render Thread
  构建帧、生成命令
       |
       v
RHI 执行器
  Dispatch（串行接收并分配全局 submission sequence）
  Translate（worker 上并行生成原生命令缓冲）
  Submit（严格按序执行队列提交、跨队列信号量、readback、Present）
```

异步管线只承载独立资源准备，并在消费前以依赖事件显式合并。

## 帧同步深度

| 层级 | 作用 |
| --- | --- |
| `FrameResourceRing` | 判断 GPU 是否仍在使用这批资源 |
| `FrameLatencyController` | 控制 CPU 帧延迟 |
| `RenderCommandFence` | Render / Rhi / Swapchain 三级同步深度 |

## 线程命名

线程名在启动时注册，性能分析中可见。命名对象包括 Game Thread、`Render Thread`、专用 `RHI Thread`、`TaskGraph Worker <n>`、`Background Job Worker <n>`，以及编辑器的元数据/导入、启动缓存、Prefab 准备、着色器编译与反射、文件监听、脚本构建、调试端点、进度对话框与追踪写出线程。

`tasks` 模式下 RHI 工作保留物理 worker 的名称，不会伪装成独立 RHI 线程。

## 下一步

- [渲染概览](../rendering/overview.md)：通道如何构建一帧。
- [性能分析与图形调试](../quality/profiling.md)：如何观察这些线程。
