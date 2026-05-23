# Quickstart: 多线程渲染框架

## Purpose

验证三线程渲染框架在保持 Editor / Game 可运行的前提下，正确完成帧快照发布、场景准备、RHI 提交、present / offscreen 输出与帧退休。

## Prerequisites

- 仓库根目录：`D:/VSProject/Nullus`
- 已存在可用的 `build` 目录
- 可启动的测试项目或默认测试工程
- RenderDoc 已按仓库文档安装（仅在需要图像证据时）
- 如需让 `Editor` / `Game` / `Launcher` 走多线程运行时，请设置环境变量 `NLS_ENABLE_THREADED_RENDERING=1`

## Build And Test Commands

### 1. 重建聚焦目标

```powershell
cmake --build build --config Debug --target NLS_Render NLS_Engine NullusUnitTests -- /m:1
```

### 2. 运行基础回归

```powershell
ctest --test-dir build -C Debug --output-on-failure
```

### 3. 运行聚焦渲染测试

```powershell
Build/bin/Debug/NullusUnitTests.exe --gtest_filter=ThreadedRenderingLifecycleTests.*:DriverSwapchainResizeTests.*:DriverNullDeviceFallbackTests.*:CompositeRendererExplicitDrawOrderTests.*:RendererFrameObjectBindingTests.*:RendererStatsTests.*:LightingDataProviderTests.*:DebugDrawPassTests.*:PanelWindowHookTests.*:DebugSceneLifecycleTests.*
```

### 4. 检查三线程生命周期关键入口是否已集中

```powershell
rg -n "BeginExplicitFrame|EndExplicitFrame|PresentSwapchain|ResizeSwapchain|AcquireNextImage|NLS_ENABLE_THREADED_RENDERING|Frame Stage|Retirement State|frameContexts|RHIFrameContext" Runtime Project Tests -g"*.h" -g"*.cpp"
```

期望：

- backend-facing 生命周期入口集中在 `Runtime/Rendering/Context/Driver.*` 与 RHI 所有权范围内
- 产品层只通过 `NLS_ENABLE_THREADED_RENDERING` 进入 threaded runtime，不新增直接 submit / present 旁路
- `FrameInfo` 面板可见 `Publish State`、`Frame Stage`、`Retirement State` 诊断文本

## Manual Validation Checklist

### 1. Editor 主路径

1. 设置 `NLS_ENABLE_THREADED_RENDERING=1`，用受支持后端启动 `Editor.exe`。
2. 打开包含普通 mesh、skybox、灯光和编辑器辅助元素的场景。
3. 确认 scene view 与 game view 都能持续刷新，没有卡死、黑屏或一帧后停止更新的问题。
4. 打开 `Frame Info` 面板，确认 `Publish State`、`Frame Stage`、`Retirement State` 会随生命周期变化更新。

### 2. Runtime / Game 主路径

1. 设置 `NLS_ENABLE_THREADED_RENDERING=1`，用同一后端启动 `Game.exe`。
2. 确认普通场景能正常渲染、移动相机后画面继续更新。
3. 确认逻辑更新不会因为单帧 GPU 提交完成而整体停顿。

### 3. Offscreen 输出

1. 打开依赖 offscreen 纹理的编辑器视图或面板。
2. 确认该路径在没有 swapchain present 的情况下也能持续产出有效图像。
3. 确认 offscreen 帧不会误触发 present 或错误复用仍在途的目标资源。

### 4. Resize 与 Shutdown

1. 在 Editor 运行时拖动窗口边缘触发 resize。
2. 确认旧在途帧被安全 drain，旧 backbuffer / swapchain 资源在 resize 前被释放。
3. 确认后续 swapchain 帧只会在 pending resize 真正应用后继续发布，并以新尺寸继续渲染。
4. 在渲染工作仍活跃时关闭 Editor/Game。
5. 确认进程可正常退出，没有死锁、长时间挂起或资源回收崩溃。

### 5. 调试与编辑器辅助路径

1. 打开 grid、gizmo、outline、debug helper 可见的场景。
2. 确认这些路径在多线程生命周期下仍按正确顺序出现。
3. 确认没有重复绘制、缺失绘制或跨帧残留。
4. 确认 helper pass 禁用时不会被 threaded helper 统计错误计入。

## RenderDoc Guidance

当需要对图像正确性做声明时，按仓库文档执行：

- `D:/VSProject/Nullus/Docs/Rendering/RenderDocDebugging.md`

示例：

```powershell
py -3 Tools/RenderDoc/renderdoc_runner.py --target editor --backend dx12 --capture --capture-after-frames 120
```

## Slice Completion Criteria

某个迁移切片只有同时满足以下条件，才算可以进入下一步：

- 基础 `ctest` 通过
- 聚焦渲染测试通过
- Editor 与 Game 都仍可运行
- offscreen、resize、shutdown 至少完成一次针对性验证
- `FrameInfo` 的 `Publish State`、`Frame Stage`、`Retirement State` 已人工或测试验证
- 如声明了图像正确性，已补充对应后端的 RenderDoc 证据
- 没有引入新的产品层旁路接口去直接操作后端提交或 present

当前实现状态：

- fallback-enabled threaded 路径已通过 DX12 Game/Editor 可见性验证。
- runtime 可见帧的 direct submit fallback 已从 `ABaseRenderer` / `Driver` / `Launcher` 代码路径中移除。
- 真实 RHI-thread-only command recording / submit / present 已完成到 runtime 主可见帧；剩余交付风险集中在 `tasks.md` 中的 T021b 尾项与 T033b 的完整运行时/RenderDoc 证据。

## Validation Log

### 2026-04-19 运行时黑屏回归修复（fallback-enabled）

环境：Windows Debug，`App/Win64_Debug_Runtime_Static`，`TestProject/TestProject.nullus`，DX12，`NLS_ENABLE_THREADED_RENDERING=1`。

结果：

- `cmake --build build --config Debug --target NullusUnitTests -- /m:1`：通过。
- `Build/bin/Debug/NullusUnitTests.exe --gtest_filter="ThreadedRenderingLifecycleTests.*"`：25/25 通过。
- `Build/bin/Debug/NullusUnitTests.exe`：170/170 通过。
- `ctest --test-dir build -C Debug --output-on-failure`：1/1 通过。
- `cmake --build build --config Debug --target Game -- /m:1`：通过。
- `cmake --build build --config Debug --target Editor -- /m:1`：通过。
- Game 线程模式 8 秒冒烟：启动成功，窗口句柄非 0，`CloseMainWindow()` 与 `WaitForExit()` 成功，日志到达 `Application::Run: game loop ended`；日志没有 `DXGI_ERROR_DEVICE_REMOVED`、`Present failed` 或 on-demand acquire。进程退出码为 2173；非线程模式同样返回 2173，因此该退出码记录为既有 shutdown/返回码 caveat，不作为本轮 threaded 渲染黑屏回归证据。
- Editor 线程模式 resize/shutdown 冒烟：启动成功，两次 `MoveWindow` 均成功，关闭成功，退出码 0。
- RenderDoc Game DX12 capture：`Build/RenderDocCaptures/game/dx12/game_dx12_DX12_capture.rdc`，D3D12，1 次 draw / 12 triangles，swapchain `R8G8B8A8_UNORM 1600x900`；导出的 `game_dx12_rt29.png` 显示 sky/ground，中心像素约 `(0.537, 0.537, 0.522, 1.0)`，RT min/max 不是全黑。
- RenderDoc Editor DX12 capture：`Build/RenderDocCaptures/editor/dx12/editor_dx12_DX12_capture.rdc`，D3D12，33 次 draw / 3 个 color pass；导出的 `editor_dx12_rt385.png` 显示 Editor UI、Scene View、skybox、grid、gizmo，最终 swapchain 中心像素约 `(0.455, 0.431, 0.416, 1.0)`，高/中等级 capture log 为空。
- 入口审计 `rg -n "BeginExplicitFrame|EndExplicitFrame|PresentSwapchain|ResizeSwapchain|AcquireNextImage|NLS_ENABLE_THREADED_RENDERING|Frame Stage|Retirement State|frameContexts|RHIFrameContext" Runtime Project Tests -g"*.h" -g"*.cpp"`：产品层仍通过 Driver access 调用 present / threading flag，backend-facing 入口集中在 Driver / RHI 层。

调查结论：

- 黑屏/崩溃根因之一是线程模式下主线程 `PresentSwapchain()` 会在没有 standalone UI frame 时走兼容 on-demand acquire/present，和 RHI worker 的 swapchain 生命周期冲突。
- 另一个根因是线程模式下场景 renderer 跳过 `ParseScene()`，并且逻辑线程没有 active explicit command buffer，导致真实 draw 没有被记录。
- 为恢复可见画面，当前运行时可见帧保留既有 explicit command buffer 记录和直接提交路径；RHI worker 生命周期测试仍覆盖 snapshot/package/submit/retire 骨架。真正把已记录 command buffer 安全移交给 RHI 线程提交仍需要后续补充同步和资源所有权约束，不能把本轮验证解读为完整 RHI 线程提交交付。
- 本节证据对应 `tasks.md` 中的 T033a；T033b 需要在 direct submit fallback 关闭后重新运行 Editor/Game/RenderDoc 验证。

### 2026-04-19 无 direct submit fallback 验证（进行中）

环境：Windows Debug，`App/Win64_Debug_Runtime_Static`，`TestProject/TestProject.nullus`，DX12，`NLS_ENABLE_THREADED_RENDERING=1`。

结果：

- `cmake --build build --config Debug --target NullusUnitTests -- /m:1`：通过。
- `Build/bin/Debug/NullusUnitTests.exe --gtest_filter="ThreadedRenderingLifecycleTests.ThreadedRendererNeverUsesDirectExplicitFrameRecordingForRuntimeVisibility:ThreadedRenderingLifecycleTests.ThreadedRendererPublishesSnapshotForRuntimeVisibility:ThreadedRenderingLifecycleTests.ThreadedVisibleFrameRecordsSwapchainPassPlanThroughRhiWorker:ThreadedRenderingLifecycleTests.ThreadedVisibleFrameRecordsPreparedDrawBindingsAndMeshByDefault:ThreadedRenderingLifecycleTests.ThreadedRendererSkipsDirectExplicitFrameRecordingForOffscreenFrames"`：5/5 通过。
- `Build/bin/Debug/NullusUnitTests.exe`：180/180 通过。
- `cmake -S . -B build_run`：通过。
- `dotnet restore build_run\Tools\MetaParser\src\MetaParser.csproj`：通过。
- `cmake --build build_run --config Debug --target Editor Game -- /m:1`：通过；用于绕开主 `build` 目录被 `devenv.exe` 占用的增量文件锁。
- Game 线程模式 8 秒冒烟：启动成功，窗口句柄非 0，`CloseMainWindow()` 成功，15 秒内退出，退出码 `2173`；日志到达 `Application::Run: game loop ended`，未见 `Present failed`、`DXGI_ERROR_DEVICE_REMOVED` 或 on-demand acquire 关键字。
- Editor 线程模式 resize/shutdown 冒烟：启动成功，窗口句柄非 0，两次 `MoveWindow()` 都返回 `true`，`CloseMainWindow()` 成功，30 秒内退出，退出码 `0`。
- Editor RenderDoc DX12 capture：`Build/RenderDocCaptures/editor/dx12/Editor_DX12_capture.rdc`，`py -3 Tools/RenderDoc/rdc_analyze.py ...`（提权以允许 `rdc-cli` session 写入）结果为 `D3D12`、`30` 个事件、`22` 次 indexed draw、`1` 个 `Colour Pass`、约 `989` triangles，说明无 fallback threaded Editor 路径可产出可分析的非空 capture。
- Game RenderDoc 尝试：
  - `renderdoc_runner.py --capture --capture-after-frames 60/1`：日志显示 RenderDoc 已加载并 arm 了 startup capture，但 capture 目录没有新 `.rdc`。
  - 直接启动 `Game.exe` 并发送 `F11`：日志出现 `RenderDoc queued next-frame capture: Game`，但 capture 目录仍没有新 `.rdc`。
  - `renderdoccmd capture` 包装 + `F12`：同样没有新 `.rdc` 写入。

调查结论：

- 代码路径上，runtime 主可见帧已不再拿主线程 explicit frame context，submit / acquire / present 只由 RHI worker 执行；这部分由单测和 Editor/Game 冒烟共同支撑。
- Editor 的 no-fallback DX12 RenderDoc capture 已经成立，说明线程化主路径在 Editor 下能产出有效 GPU 帧。
- Game 的 no-fallback DX12 RenderDoc capture 目前仍被阻塞：应用日志能确认 capture 被成功排队，但当前环境里没有 fresh `.rdc` 落盘。这个问题更像 RenderDoc 集成/写盘路径问题，而不是主渲染线程回退到 direct submit，因为 Game 冒烟和相关单测都已证明运行态没有重新拿回 direct explicit path。
- 因此，T010b、T022b、T032b 可以视为完成；T033b 仍需补齐 Game RenderDoc fresh capture 或明确修复其写盘阻塞后再关闭。
