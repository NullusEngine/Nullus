# Input System Release 性能与堆分配检查：2026-10-29

本轮核心 Release 检查完成。18 种预热输入负载在三轮采样中均为零次 C++ 堆分配。
修复了两个变更队列的重复分配，并减少了回调分发和 Context 消费的无效工作。
这份结果覆盖输入核心，整编辑器、物理设备和其他平台仍需要各自验收。

## 实际实现与文件

| 文件 | 职责与本轮变化 |
| --- | --- |
| `Runtime/Input/InputUser.cpp` | 变更执行后复用队列容量；Update 复用同一执行函数；只为实际匹配的回调保留临时订阅所有权；Never 不扫描消费事件，OnActuated 不扫描 Started/Performed |
| `Runtime/Input/InputDomain.cpp` | 安全边界执行后复用变更队列容量；嵌套变更仍进入下一边界，异常不重放已分离批次 |
| `Tests/Unit/InputActionTests.cpp` | 新增订阅向量增长和嵌套/异常边界回归；原有功能断言保留；修正旧计时输出固定写成 Debug 的描述 |
| `Tests/InputPerformance/CMakeLists.txt` | 独立 Release 构建、相同生产源码的静态基准依赖、既有核心行为测试、分配门槛 |
| `Tests/InputPerformance/HeapAllocationCounter.h/.cpp` | 当前线程 C++ 分配计数和普通/对齐/STL 校验；不进入生产模块 |
| `Tests/InputPerformance/InputBenchmark.cpp` | 21 项负载、分离计时/计数、回调与事件数量验证、JSON 输出 |
| `Tests/InputPerformance/README.md`、`Tests/CMakeLists.txt` | 复现说明、可选主工程性能目标接入 |
| `Docs/InputSystem.md`、`Docs/Plans/InputSystem-Implementation.md` | 使用方式、验收状态及测量范围 |

没有改变公开输入 API、资源或绑定 ID、事件顺序、Fixed Tick 归属、消费策略和编辑器命令入口。
没有截断事件、减少订阅、添加生产 Stub 或删除功能测试。

## 环境与方法

- Windows 11 x64，10.0.26200；AMD Ryzen 9 8945HX，16 核/32 逻辑处理器。
- Visual Studio 2022 / MSVC 19.44.35228，C++20；确认 `/O2 /Ob2 /DNDEBUG /MD`，使用 `/Z7` 符号，没有 LTO。
- 每轮先预热 256 次，再记录 64 个样本，每个样本 32 次操作；随后独立计数 2,048 次操作的分配。修改前后各运行三轮。
- 下表是三轮 P50 的中位数，单位为微秒/操作。P95/P99 也是 **32 次操作平均值的分位数**，不等同单帧尾延迟。
- 输入使用可注入的单调时钟，逻辑 Update 120 Hz；计时使用独立的 `steady_clock`。测试使用虚拟输入，不包含平台事件泵 I/O。
- 采样时其他任务仍有后台构建，保留了正在运行的 Editor；未调整亲和性或电源计划。因此不设置绝对耗时门槛，也不把小幅差异解释为优化。

为了准确计数，基准将相同生产 Input Core 源码和既有 Guid/向量实现静态链接进可执行文件。
仅在可执行文件替换 `operator new` 无法覆盖普通 DLL 内的分配。
计数覆盖当前线程的普通、数组、对齐及 nothrow C++ 分配；初始化校验同时验证直接分配和 STL 容器。
计时与分配分别运行，JSON、控制台输出和样本容器不在计数范围中。

## 实测结果

| 负载 | 修改前 P50 | 修改后 P50 | 修改后 P95 | 分配次数/操作：前 → 后 |
| --- | ---: | ---: | ---: | ---: |
| 128 Action 空闲 | 17.678 | 17.081 | 17.741 | 0 → 0 |
| 128 Action 持续按住 | 19.728 | 19.350 | 20.406 | 0 → 0 |
| 128 Action/订阅，同帧四次短按 | 103.097 | 89.603 | 106.244 | 0 → 0 |
| 同一 Control 同时触发 128 Action/回调 | 488.972 | 158.438 | 167.062 | 0 → 0 |
| 上述同时触发，OnActuated 消费 | 534.681 | 170.116 | 188.103 | 0 → 0 |
| 上述同时触发，OnStarted 消费 | 542.922 | 258.784 | 312.519 | 0 → 0 |
| 上述同时触发，OnPerformed 消费 | 546.550 | 248.403 | 309.309 | 0 → 0 |
| 128 精确 Primary 组合键，两次短按 | 143.297 | 146.781 | 158.453 | 0 → 0 |
| 同一 Action 两个 Binding，部分释放 | 1.956 | 1.703 | 1.938 | 0 → 0 |
| 4 玩家，各 128 Action，1 Fixed Tick | 179.428 | 144.988 | 177.353 | 0 → 0 |
| 鼠标 Delta/手柄 Rate，循环 0/1/3 Fixed Tick | 2.688 | 2.653 | 2.978 | 0 → 0 |
| 无新设备事件的 Hold/Tap/Repeat | 0.878 | 0.866 | 0.991 | 0 → 0 |
| 每次回调提交两条路由修改 | 0.887 | 0.756 | 0.766 | 2 → 0 |
| 每次提交一条 Domain 修改 | 0.397 | 0.350 | 0.356 | 1 → 0 |
| Context 禁用、取消、RequireRelease 恢复 | 3.131 | 2.916 | 3.291 | 0 → 0 |

两个 Binding 的一项操作包含 2 次 Update；Context 恢复包含 6 次 Update；四玩家一项包含全部玩家的处理。
回调路由队列原先每次分配 192 字节，Domain 队列原先每次分配 64 字节；预热复用后均为 0。
128 Action 同时触发是压力样本，不能按该改善比例推导编辑器帧率。

另有 3 种查询各执行 1,024 次：缓存 Handle 约 3.009 μs，稳定 ID 约 19.600 μs，按名称查找约 355.084 μs。
名称查找保留初始化用途；高频使用缓存句柄，并在定义 generation 改变后刷新。

结构性操作单独记录，未要求零分配：128 Action 的冷编译约 29.228 μs，399 次分配/74,804 字节；
单次用户 Override 应用约 22.219 μs，2 次分配/144 字节。空基准约 0.003 μs/操作。

## 构建与行为验证

| 检查 | 实际结果与日志 |
| --- | --- |
| 主工程 `NLS_Input` Release DLL | 成功；`build/input-release-production-build.log`。复用现有 Base/Math Release 依赖，`BuildProjectReferences=false`，未覆盖运行中的 Debug Editor |
| 独立 Release 基准及行为目标 | 成功；`build/input-release-build.log`、`input-release-optimized-build.log`、`input-release-coverage-build.log` |
| Release CTest | 2/2 通过；分配门槛 21 项检查，既有核心行为测试 60/60；`build/input-release-final-ctest.log`、`input-release-final-gtests.xml` |
| Debug DLL 与回归目标 | 独立目录构建成功；`build/input-release-debug-regression-build.log` |
| Debug 回归 | 5/5 目标，157/157 用例：核心 60、编辑器适配 73、平台 13、相机状态机 9、焦点 2；`build/input-release-debug-ctest.log`、`input-release-debug-results/*.xml` |
| 三轮前后采样 | `build/input-release-paired-before-{1,2,3}.json`、`input-release-paired-after-{1,2,3}.json` |
| 汇总与源码摘要 | `build/input-release-performance-summary.json`；修改前两个生产实现快照位于 `build/input-perf-baseline-source` |

未以减少回调/事件数量换取性能：负载验证同帧多次短按、多 Binding、独立玩家、Fixed 事件零丢失/零重复、
无事件计时和 Context 恢复。新增回归验证回调注册 64 个订阅导致向量增长时的批次边界，以及异常/嵌套 Domain 修改的顺序。

复现命令见 `Tests/InputPerformance/README.md`。CI 分配门槛不使用 `--allow-allocations`；该参数只用于保留修改前的失败计数，行为错误始终返回失败。

## 边界与后续验收

零分配结论限于预热后的被测容量和输入 Control。新设备、新 Control、更大突发、长时间不消费 Fixed Tick、
大回调捕获、注册、重载和任意递归排队配置仍可能分配；不能为了该指标丢弃输入或无限预留内存。
当前仍保留按事件扫描订阅的路径，高并发订阅规模需要继续按实际游戏负载评估。

本轮没有覆盖 C/OS 堆、其他线程或外部 DLL，不能作为泄漏检测结论；未做整个 Editor 的 Tracy/实机帧延迟采样。
Windows 可见操作、物理设备、完整引擎单测以及 macOS/Linux 性能/IME 验收状态不因本轮核心检查通过而改变。
