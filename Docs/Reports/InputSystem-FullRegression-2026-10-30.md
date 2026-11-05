# Input System：Windows Debug 全量回归

日期：2026-10-29 至 2026-10-30（Asia/Shanghai）。

已执行全部 56 个已配置 CTest 目标，并补齐聚合进程卡住后未完成的用例。
**全量验收尚未通过**：原始 CTest 为 49 个目标通过、7 个失败；其中 3 个
修复后正式复验通过，仍有 4 个目标开放。输入专项 199/199 成功。

## 构建及覆盖范围

使用当前主工程 C++20/MSVC、已有 `build` 多配置目录及 Debug 配置。
`EditorProject`、`NullusUnitTests` 及实际依赖构建成功；52 个非聚合原生测试目标
的最终串行构建全部成功。没有替换实现、删除功能或降低测试断言来通过构建。

`NLS_BUILD_TESTS=ON`、`NLS_BUILD_PERFORMANCE_TESTS=OFF`、
`NLS_ENABLE_TIMELINE_PROFILER=OFF`。本轮覆盖全部已注册行为测试；
Release 性能/分配结果仍以 2026-10-29 的独立报告为准。

Native/ImGui/GPU 测试串行执行；`TMP/TEMP=D:/Code/Nullus/tmp/ir`，避免过长临时路径
影响 DXC。使用既有 `Tools/BuildCleanEnvironment` 处理 Path/PATH 环境问题。
早期测试占用 DLL 导致的部署失败在停止本轮已确认卡住的测试后重新构建解决。

仓库仍有其他线程并行开发。最终重链接使聚合库存从 5,095 增至 5,102，新增 7 项
已逐项补跑，没有删除旧用例；CTest 仍是 56 个目标。结果对应记录的构建产物，
不代表更晚的源码变化已全部验证。

## 执行结果

| 执行 | 成功 | 失败 | 跳过 | 说明 |
| --- | ---: | ---: | ---: | --- |
| 完整 CTest | 49 | 7 | — | 56 个目标，3,025.03 秒；不是全部通过 |
| 聚合进程已完成部分 | 3,306 | 33 | 36 | 完成 3,375 项后卡住 |
| 未完成用例隔离补跑 | 1,681 | 9 | 30 | 1,720 项、134 批次，无批次超时；累计 586.069 秒 |
| 两者合计覆盖原库存 | 4,987 | 42 | 66 | 5,095 项全部有结果，缺失集合为空 |
| 收尾 Console/面板/完整渲染执行复验 | 300 | 0 | 1 | 同一个实际聚合可执行文件筛选执行 301 项 |
| 并行开发新增用例 | 6 | 1 | 0 | 6 项任务系统成功、1 项 SpriteMixedGpu 失败 |

按每个用例最后一次观测去重，5,102 项中为 5,009 成功、27 失败、66 跳过。
这个数字混合了独立进程和聚焦复验，**不是一次成功的聚合执行**，
不能据此关闭 JobSystem 全局状态污染问题。原始失败日志保持原样。
跳过包含不可用图形后端、关闭的 TimelineProfiler/LuaPanda，以及无符号链接权限等条件。

| 输入和相关目标 | 成功 | 失败 | 跳过 |
| --- | ---: | ---: | ---: |
| NullusInputTests | 60 | 0 | 0 |
| NullusInputPlatformTests | 13 | 0 | 0 |
| NullusInputEditorTests | 73 | 0 | 0 |
| NullusInputIntegrationTests | 42 | 0 | 0 |
| NullusInputUsageTests | 11 | 0 | 0 |
| NullusPlayModeTests | 42 | 0 | 0 |
| NullusPlayLoopNoUiTests | 1 | 0 | 0 |
| NullusPlayIntegrationTests | 1 | 0 | 0 |
| NullusScriptRuntimeTests | 62 | 0 | 1 |
| NullusUIDesignerTests | 83 | 0 | 0 |
| NullusSceneViewToolbarTests | 169 | 0 | 2 |
| NullusSceneViewDrawModeGpuTests | 12 | 0 | 0 |
| NullusBuildTests | 40 | 0 | 0 |
| NullusEditorStatusBarTests（修复后） | 47 | 0 | 0 |
| NullusMaterialTests（修复后） | 21 | 0 | 1 |

前五个输入目标合计 199 次成功执行。其他目标与聚合测试有重复，不可直接相加称为
独立用例总数。DX12 测试使用实际 NVIDIA GeForce RTX 5070 Laptop GPU。

## 本轮实际修改

| 文件 | 修改及验证 |
| --- | --- |
| `Project/Editor/Panels/AView.cpp` | 配置帧描述时先检查可选 EditorActions。独立 View 不再 Get 缺失服务，避免 bad_any_cast 及注册表空项污染；真实 Editor 保留 Play 帧信息 |
| `Tests/Unit/PanelWindowHookTests.cpp` | 原帧信息测试增加服务存在状态不变断言；后台日志测试检查当前日志模型、版本和状态快照，保留后台不提交、UI Flush 后才可见的行为 |
| `Tests/Unit/EditorStatusBarTests.cpp` | Console 复制和 Enter 打开测试接入真实 InputSystem → ShortcutService → Action → Command，加载正式默认资产，使用真实面板焦点；保留剪贴板、清除、源码回退断言和 ImGui 原生按键 |
| `Tests/Unit/AssetBrowserPresentationTests.cpp` | 穷举预期补充新增 Sprite Artifact → Texture 的生产映射，保留其他枚举断言 |
| `Tests/Plugins/CMakeLists.txt` | 静态宿主及消费者取消继承的 NLS_SHARED_LIB，修复 ServiceLocator 定义误用 dllimport 的 C2491 |
| `Tests/Plugins/CMakeInputs.py` | SDK 消费测试匹配带 Debug 后缀和平台前缀的共享宿主库，复制到实际消费程序目录，修复漏 DLL 后的加载对话框/超时 |
| `Tests/Unit/CMakeLists.txt` | 材质测试使用既有传递共享库部署；状态栏独立目标编译真实 Shortcut Registry/Service/Profile/Adapter 并链接 Input/Platform，继续不依赖完整 Editor/Scene renderer |

上述文件原本已有其他未提交或未跟踪内容，本表只说明本轮具体改动，不认领整个文件差异。
未改动原有 Undo、物理、任务系统、Prefab、VFX 或 ECS 业务来掩盖失败。

## 原始七个失败目标的处置

| 目标 | 当前结果 |
| --- | --- |
| NullusPluginCMakeInputsShared | **已关闭**：原先超时 240 秒；修复共享宿主库部署后，真实配置/构建/SDK 消费场景及正式 CTest 通过 |
| NullusMaterialTests | **已关闭**：原先进入 GoogleTest 前因缺少传递依赖 NLS_Inputd.dll 触发 STATUS_DLL_NOT_FOUND (0xc0000135)；部署修复后正式 CTest 21 成功、1 跳过 |
| NullusEditorStatusBarTests | **已关闭**：旧测试只向 ImGui 注入已迁移业务快捷键；接入新输入路径后正式 CTest 47/47，聚合对应测试亦成功 |
| NullusUnitTests | **开放**：进程级 JobSystem 污染导致渲染测试卡住；另有资源异步请求上限/取消、启动预导入、缩略图、Tracy 发现、Prefab、VFX 等失败。新增 SpriteMixedGpu 还报告排序、记录 draw 数量和像素不符 |
| NullusSpriteImportTests | **开放**：5 项 PrefabUtilityFacade 场景结构覆盖/重新连接/缺失 Artifact 恢复测试在 InstantiateScene 后得到空 loadedScene |
| NullusVfxEditorTests | **开放**：6 项 DX12 VfxSceneTests 找不到预期 passCommandInputs；独立目标与隔离补跑均复现 |
| NullusECSPlayerTests | **开放**：5 项 staging/打包启动测试抛出 The system cannot find the path specified.；其他 6 项成功。具体缺失路径尚未定位，不能归因为输入或宣称修复 |

AView 故障用 Panel/Prefab 原顺序探针定位到缺失 EditorActions 的 Get；修复后不再出现
该 bad_any_cast。Prefab 剩余 5 项是另外的空场景断言，没有算作已修复。
所有失败用例完整名称和可用消息保存在 `final-summary.json`。

### 聚合卡住的隔离问题

`ScopedMaterialConversionJobSystem`、`ScopedThumbnailPerformanceJobSystem` 强制关闭并
重置进程级 JobSystem，退出时没有恢复已有共享执行环境。后续 Driver/TaskGraph 仍在，
实际观测到 foreground/background worker 变成 0/0。

两项最小顺序样本：

```text
AssetMaterialConversionTests.MaterialManagerPreviewAsyncArtifactReturnsMaterialBeforeTexturesResolve
DriverNullDeviceReadinessTests.PublishesTheActualSharedJobWorkerCounts
```

第一项成功，第二项因 foreground=0 而失败。完整运行随后在
`RenderThreadServicesCommandsWhileExecutorTranslatesAProductionFrame` 发生排队/退休超时并卡住；
该项单独运行成功，全部 RenderFrameExecutionTests 在新进程中 213/213 成功。
本轮未修改仍在并行开发的 Jobs 实现；拆进程补跑不能视作修复共享生命周期。

确认卡住后仅停止本轮启动、已核对 PID/父进程/启动时间的测试子进程，保留非侵入式
堆栈和停止记录；CTest 父进程继续执行其他目标。材质 loader 失败同样先保存证据。
这些主动停止不能描述为程序自行崩溃；没有停止用户 Editor 或其他构建进程。

## 命令与证据

证据目录：`build/input-full-regression-20261029/`。

```powershell
.\Tools\BuildCleanEnvironment.cmd cmake -S . -B build
.\Tools\BuildCleanEnvironment.cmd cmake --build build --config Debug `
  --target EditorProject NullusUnitTests NullusPluginTestsStatic NullusECSEditorBakingTests `
  -- /m:1 /v:minimal /p:LinkIncremental=false
powershell -File build/input-full-regression-20261029/BuildTargets.ps1

$env:TEMP = "$PWD/tmp/ir"
$env:TMP = $env:TEMP
$env:GTEST_OUTPUT = "xml:$PWD/build/input-full-regression-20261029/gtest-final/"
ctest --test-dir build -C Debug -j1 --verbose --timeout 3600 `
  --output-junit "$PWD/build/input-full-regression-20261029/complete-ctest.xml"
python build/input-full-regression-20261029/RecoverUnitCoverage.py
python build/input-full-regression-20261029/SummarizeFinal.py
```

| 证据 | 内容 |
| --- | --- |
| `synchronized-build.log`、`target-build-results.json` | 实际依赖/Editor/聚合及 52 个非聚合测试目标构建 |
| `first-pass-builds/` | 早期静态宏错误及运行中测试占用 DLL 的失败记录 |
| `complete-ctest.log/.xml`、`gtest-final/` | 原始 56 个目标执行与可用 GoogleTest XML |
| `unit-recovery/planned.json`、`results.json`、逐批 `.log/.xml` | 1,720 项补跑、退出码、超时标记和覆盖集合 |
| `final-summary.json`、`SummarizeFinal.py` | 原始失败、覆盖、复验及最后观测，保持聚合结论独立 |
| `panel-prefab-throw.log`、`repaired-order.xml` | AView 异常调用链和修复后有序探针 |
| `jobs-state-leak.log/.xml`、`unit-stacks.log` | JobSystem 顺序污染样本及卡住堆栈 |
| `confirmed-hang.json`、`final-unit-confirmed-hang.json`、`material-startup-failure.json` | 本轮子进程身份、停止动作与原因 |
| `material-startup-stacks.log`、`material-deployment-build.log`、`material-fixed-ctest.xml` | 缺 DLL loader 证据、修复构建、材质正式复验 |
| `console-migration-final-build.log`、`fixed-targets-ctest.xml` | 状态栏适配构建及状态栏/共享插件正式复验 |
| `console-render-final.log/.xml` | 19 项状态栏、69 项面板（1 跳过）、213 项渲染执行收尾验证 |
| `unit-inventory-after-repairs.txt`、`final-added-*.log/.xml` | 最终 5,102 项库存及新增 7 项的逐项执行 |
| `tested-runtime-artifacts.json`、`retest-artifacts.json` | 被测运行库与最终重链接产物的 SHA-256/时间 |

本轮修改的 `git diff --check` 通过；相关未跟踪测试/脚本另行检查无行尾空白。
文本质量检查不替代构建或测试。

## 后续验收

优先修复共享 JobSystem 隔离，再关闭资源/Prefab、VFX、Sprite 混合渲染和 ECS 失败，
在稳定产物上重新运行一次不中断的完整 CTest。聚焦通过不替代这一步。

Windows 可见 Editor 的连续保存、Scene 右键导航、文本/弹窗、多浮动窗口拖拽，真实
手柄/键盘布局/IME 候选窗仍需实机验收。隐藏窗口、ImGui 自动化及 DX12 读回不等价于
这些人工操作。macOS/Linux CI、实机与原生 IME 未在本机验证。
五阶段计划尚不能标记为全部验收完成。
