# 调试与测试

本页覆盖引擎侧的调试入口与测试体系：怎么跑测试、测试放在哪里、CI 覆盖了什么、以及 Windows 上哪些回归需要本地执行。

## 测试目标

| 目标 | CTest 标签 | 定位 |
| --- | --- | --- |
| `NullusUnitTests` | `behavior` | 确定性的行为与集成回归 |
| `NullusPerformanceTests` | `performance` | 按需构建的性能与压力测试 |
| `ReflectionTest` | 无 | 反射链路的轻量冒烟工具 |

`NullusPerformanceTests` 默认既不构建也不注册到 CTest。可重复、确定性的行为回归应放进单元测试；性能测量、压力场景与依赖真实资产的验证放进性能测试，避免默认行为测试受机器状态影响。

## 单元测试覆盖范围

- 资产导入、数据库、工件、Prefab、缩略图与资源生命周期；
- 编辑器工作流、设置、快捷键、输入、面板与工具行为；
- 渲染、RHI、FrameGraph、着色器与材质、场景与可见性管线；
- JobSystem、性能标记、序列化，以及 `Base`、`Core`、`Engine` 的其它确定性行为。

反射与 MetaParser 是其中的专项重点，包括关键类型的运行时注册验证、生成内容验证，以及构建期才会暴露的集成回归。

## 运行行为测试

```powershell
cmake -S . -B Build -G "Visual Studio 17 2022" -A x64 -DNLS_BUILD_TESTS=ON
cmake --build Build --config Debug --target NullusUnitTests ReflectionTest -- /m:4
ctest --test-dir Build -C Debug -L behavior --output-on-failure
```

`ReflectionTest` 会随命令一起构建，但它是独立工具，不带 `behavior` 标签。

## 运行性能测试

```powershell
cmake -S . -B Build -G "Visual Studio 17 2022" -A x64 -DNLS_BUILD_PERFORMANCE_TESTS=ON
cmake --build Build --config RelWithDebInfo --target NullusPerformanceTests
ctest --test-dir Build -C RelWithDebInfo -L performance --output-on-failure
```

性能结论应来自与目标配置匹配的重建二进制，并记录重复测量而不是单次结果。

## 职责边界

| 目标 | 适合承载 |
| --- | --- |
| `NullusUnitTests` | 与机器性能无关的行为断言、子系统契约、细粒度回归 |
| `NullusPerformanceTests` | 性能测量、压力场景、真实资产负载 |
| `ReflectionTest` | 模式级哨兵验证与最小成本的注册链路检查 |

不要让 `ReflectionTest` 维护与单元测试等长的覆盖清单，两边会持续重复。

## 构建并行度与诊断开关

| 参数 | 作用 |
| --- | --- |
| `NLS_BUILD_JOBS` | 覆盖外层 MSBuild 并行度 |
| `NLS_MSVC_MP_JOBS` | 限制目标内 MSVC `/MP` 并行进程数 |
| `NLS_ENABLE_MSVC_MULTIPROCESSOR_COMPILATION=OFF` | 关闭目标内并行编译 |
| `NLS_ENABLE_PCH=OFF` | 关闭预编译头，用于排查 include 顺序与宏泄漏 |

预编译头只应包含稳定的基础头文件。把 Assimp、ImGui、Windows、DX12 或 JSON 库塞进 PCH 会掩盖公共头文件的耦合问题。

## CI 覆盖

| 工作流 | 覆盖 |
| --- | --- |
| Build Windows | Release 配置构建 |
| Build Linux | Debug 与 Release |
| Build MacOS | Debug 与 Release |
| Metal Threading Validation | Metal 线程归属验证 |

Windows CI 只跑 Release，因此 Windows 上的 Debug 回归需要本地执行。跨平台结论必须来自各自平台的实际运行，不能从 Windows 结果推断。

## 调试渲染问题

渲染问题的第一手段是 RenderDoc 抓帧，而不是桌面截图。工作流见[性能分析与图形调试](profiling.md)。

## 调试脚本问题

脚本断点、附加进程与热重载的故障排查见[脚本调试与热重载](../scripting/debugging.md)。

## 崩溃与栈

`Tools/StackDump` 提供崩溃栈转储工具。它需要先构建出对应可执行文件：

```powershell
Tools/StackDump/StackDump.exe
```

在调试构建中，断言失败会直接中断并给出调用栈，比事后分析日志更有效。

## 本轮文档验收范围

2026-10-14 在 Windows 本地按当前源码重建 `NullusUnitTests`、`Editor` 与 `Launcher`，构建通过。文档中的 10 个 C# 代码块分别编译到两个语言的验证程序集，零错误、零警告。

Prefab、UI、脚本运行时、编辑器脚本扩展、Launcher、对象图与资产数据库共选取 496 项测试：490 项通过、6 项因环境或显式开关跳过。着色器相关 13 项在设置仓库 DXC 路径后补跑通过。测试覆盖原生/托管运行时契约，不能替代逐步点击桌面界面的完整流程。

当前 Debug Editor 在独立项目中完成 OBJ 导入、生成资源与实例创建、C# 程序集激活及 DX12 画面回读。分别执行的 30 帧运行与画面回读均自然退出，退出码为 0。首轮组合两种验证模式时曾出现退出等待，已保存线程栈；后续独立运行通过，尚不能据此关闭组合路径的问题。

完整桌面练习、组合路径的退出问题和 `main` 上的文档自动发布仍需单独验收。每项命令、日志位置、跳过原因和 Wiki 同步结果记录在仓库的 `Tools/Docs/ACCEPTANCE.md`。测试数据描述本轮证据，不构成其它平台的支持结论。

## 继续阅读

- [性能分析与图形调试](profiling.md)：CPU/GPU 时间与抓帧分析。
- [构建选项](../reference/build-options.md)：开关的完整清单。
