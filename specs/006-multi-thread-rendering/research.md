# Research: 多线程渲染框架

## Decision 1: 保留 `Driver` 作为图形入口，在其之上引入三阶段帧编排

**Decision**: 不把后端选择、swapchain 生命周期和工具集成从 `Driver` 中拆走；多线程渲染框架在现有入口之上增加帧编排与线程移交层。

**Rationale**:

- 仓库宪章已经要求 `Driver` 继续作为图形入口、swapchain 生命周期与工具集成的宿主。
- 当前 `BeginExplicitFrame()`、`EndExplicitFrame()`、`CreateFrameGraphExecutionContext()` 等关键桥点都围绕 `Driver` 工作。
- 如果同时改入口与线程模型，风险会从“帧生命周期重构”扩大成“整套渲染引导重构”。

**Alternatives considered**:

- **把所有渲染生命周期全部迁出 `Driver`**: 放弃，因为这会直接违反当前仓库约束，并扩大改动面。
- **继续保持单线程，只在 `Driver` 内部加少量锁**: 放弃，因为这无法形成清晰的逻辑线程 / 渲染场景线程 / RHI 线程职责边界。

## Decision 2: 逻辑线程向下游发布不可变 `Frame Snapshot`

**Decision**: 逻辑主线程在帧边界发布不可变帧快照，之后渲染场景线程与 RHI 线程都只消费该快照及其派生数据，不再回读可变 gameplay/editor 状态。

**Rationale**:

- 当前 `BaseSceneRenderer::ParseScene()` 仍直接遍历 live scene，对多线程来说这是最危险的共享状态入口。
- 不可变快照是切断逻辑写入与渲染读取冲突的最小边界。
- 这也让 Editor/Game/offscreen 输出能够共享一套统一生命周期，而不是各自保留隐式特殊路径。

**Alternatives considered**:

- **在渲染场景线程直接读取 live scene 并加锁**: 放弃，因为会把 gameplay/editor 的锁争用扩散到整帧。
- **只复制部分 camera 数据，不复制 scene 输入**: 放弃，因为渲染场景线程仍会在对象收集阶段碰到 live state。

## Decision 3: 渲染场景线程产出 `Render Scene Package`，而不是直接做后端提交

**Decision**: 渲染场景线程负责可见性收集、draw 排序、pass 意图、目标使用和每帧渲染包组装，但不直接拥有 backend 提交与呈现。

**Rationale**:

- 当前 scene renderer 已经承担解析场景、构建 frame graph、准备 draw 的职责，天然适合成为中间准备阶段。
- 将场景准备与后端提交分开，可以让 RHI 线程保持单一责任：命令录制、同步、present、retire。
- 这样更容易定位问题，到底是“场景包准备错了”还是“后端提交错了”。

**Alternatives considered**:

- **渲染场景线程直接录制并提交 GPU 工作**: 放弃，因为这会把 backend 约束、swapchain acquire/present 与场景准备耦合到一起。
- **逻辑线程直接产出 GPU 可提交对象**: 放弃，因为逻辑线程不应承担后端相关准备成本。

## Decision 4: 采用有界 `In-Flight Frame Slot`，背压默认选择节流而不是丢帧

**Decision**: 使用固定数量的 in-flight frame slots 跟踪快照、场景包、RHI 提交和退休状态；当下游落后时，默认策略是对逻辑发布边界施加可观测节流，而不是静默丢弃渲染帧。

**Rationale**:

- 现有 `framesInFlight`、`RHIFrameContext` 和 per-frame 资源模型已经是固定槽位思路，延续这一点风险最小。
- 对编辑器与运行时来说，静默丢帧会让状态与可视结果脱节，更难调试。
- 节流更符合当前 feature 的目标：先保证正确性和可验证性，再去优化吞吐策略。

**Alternatives considered**:

- **队列无界增长**: 放弃，因为会放大内存与资源退休问题，也会让 resize/shutdown 失控。
- **逻辑线程满队列时直接丢弃待渲染帧**: 放弃，因为这会破坏帧生命周期可追踪性，也会让编辑器交互变得不可预测。

## Decision 5: RHI 线程独占命令录制、提交、present、资源退休与 resize drain

**Decision**: RHI 线程是唯一允许拥有 backend-facing 命令录制、队列提交、fence/semaphore 同步、swapchain acquire/present、帧退休和 resize drain 的线程。

**Rationale**:

- 当前 `Driver` 中这些职责已经高度集中，只是仍在调用线程上执行。
- 多线程以后，若这些动作分散到逻辑线程或渲染场景线程，资源所有权会立即变得模糊。
- resize 和 shutdown 本质上都是“等待在途帧退休后再回收”的同类问题，交给同一线程最清楚。

**Alternatives considered**:

- **让逻辑线程保留 present / resize**: 放弃，因为这会使 swapchain 生命周期与 RHI 提交分裂。
- **让渲染场景线程处理资源退休**: 放弃，因为它并不拥有最终 GPU 完成信号。

## Decision 6: 现有 renderer-owned frame/object data、lighting、stats 保持有效，只改变线程边界

**Decision**: 不重新设计 renderer-owned frame/object data、scene lighting publication 与 renderer statistics 的职责归属；这次只改变它们处于哪一阶段准备、绑定和退休。

**Rationale**:

- 前一个渲染 ownership 重构已经把这些能力从 feature hook 中抽回 renderer 主线。
- 这次若再同时改 ownership 语义，会把“线程模型迁移”和“渲染职责迁移”叠加在一起。
- 更稳妥的做法是保留既有 owner，只把数据准备和消费时机纳入三阶段生命周期。

**Alternatives considered**:

- **借本次迁移顺手重写 binding / lighting / stats 模型**: 放弃，因为这会扩大 scope。
- **把这些能力延后到多线程落地后再考虑**: 放弃，因为它们本身已经处在主线上，必须纳入生命周期设计。

## Decision 7: 验证路径采用“单测 + 产品冒烟 + RenderDoc 证据”三层组合

**Decision**: 该特性的验证以 `NullusUnitTests` / `ctest` 为回归基础，以 Editor/Game 冒烟验证证明产品路径仍可运行，以 RenderDoc 证据支撑可视正确性声明。

**Rationale**:

- 线程生命周期、资源退休、resize/shutdown 更适合通过单测和 focused validation 抓回归。
- 产品运行验证仍然是宪章硬要求，不能只靠单测宣布完成。
- 多线程渲染一旦出现图像错误，RenderDoc 仍然是最可信的图形证据来源。

**Alternatives considered**:

- **只做单测**: 放弃，因为它无法证明 Editor/Game 真正可运行。
- **只做手工冒烟**: 放弃，因为线程生命周期问题需要可重复的自动回归。
