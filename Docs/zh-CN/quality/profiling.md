# 性能分析与图形调试

性能工作分两层：先用 CPU/GPU 时间定位到具体阶段，再用抓帧工具看资源与状态。本页给出 Nullus 的标记接口、抓取方式与判读顺序。

## 共享性能标记

引擎与编辑器代码通过统一 facade 打标记，而不是在各调用点写工具特有调用。

| 宏 | 用途 |
| --- | --- |
| `NLS_PROFILE_SCOPE()` | 以调用函数名作为事件名 |
| `NLS_PROFILE_NAMED_SCOPE("名字")` | 使用自定义显示名 |
| `NLS_PROFILE_REGISTER_THREAD("名字")` | 线程入口处注册线程名 |
| `NLS_GPU_PROFILE_SCOPE(nativeCommandBuffer)` | GPU 命令录制范围 |
| `NLS_GPU_PROFILE_NAMED_SCOPE(nativeCommandBuffer, "名字")` | 带自定义名的 GPU 范围 |

```cpp
void Editor::Core::Editor::Update(float p_deltaTime)
{
    NLS_PROFILE_SCOPE();
    // 需要测量的工作
}
```

线程名注册会把名字写入共享作用域事件，并转发给 Tracy，使抓取连接之前线程轨道就已命名。未注册线程会得到稳定的 `Thread <id>` 回退名。

RHI 代码应优先使用 `RHICommandBuffer::BeginGpuProfileScope()` / `EndGpuProfileScope()`，由后端决定如何桥接。

## 构建开关

| 选项 | 默认 | 作用 |
| --- | --- | --- |
| `NLS_ENABLE_PROFILING` | `ON` | 共享性能标记行为 |
| `NLS_ENABLE_TRACY` | `OFF` | Tracy 目标端 |
| `NLS_ENABLE_TIMELINE_PROFILER` | `OFF` | 编辑器内置 Timeline Profiler |
| `NLS_BUILD_TRACY_PROFILER` | 跟随 `NLS_ENABLE_TRACY` | 构建 Tracy GUI 工具 |

三个开关默认全关时，打了标记的代码仍然可以正常构建与运行，只是没有输出目标。

## Tracy

Tracy 提供 CPU 与 GPU 时间线。Windows 上启用后，DX12 命令缓冲桥会携带原生命令队列：每个可分析的直接队列与计算队列创建一个上下文，拷贝队列只在设备报告拷贝队列时间戳支持时才创建。队列校准失败只会让该队列无数据，不会禁用其它队列或 CPU 作用域。

编辑器构建会把三个工具可执行文件部署到 `App/<platform>_<config>_Runtime_*/Tracy/<version>`，与 `Editor.exe` 同目录，保证协议版本匹配。同一份工具也通过 `Tools > Tracy Profiler` 菜单暴露：可以查看解析出的路径、启动 GUI、打开抓取目录。

编辑器每个编辑器帧发送一个 CPU 帧标记；DX12 命令列表提交批次推进对应的 GPU 上下文，因此一次抓取里 CPU 作用域与 GPU pass 时间出现在同一时间线上。

### 无界面抓取

```powershell
tracy-capture -o Build\Tracy\capture.tracy -f -s 30
App\Win64_RelWithDebInfo_Runtime_Shared\Editor.exe --backend dx12 path\to\Project.nullus
tracy-csvexport --truncated_mean=95 Build\Tracy\capture.tracy > Build\Tracy\cpu-total.csv
tracy-csvexport -e --truncated_mean=95 Build\Tracy\capture.tracy > Build\Tracy\cpu-self.csv
tracy-csvexport -g Build\Tracy\capture.tracy > Build\Tracy\gpu-events.csv
```

先启动抓取再启动 Editor，期间触发目标负载（包括 Play 模式），并只比较构建、项目、场景、后端与帧数都匹配的抓取。

### 判读顺序

1. 用 CSV 导出按 CPU total / self 与 GPU pass 时间排序；
2. 需要调用栈或帧关联时用 Tracy GUI；
3. Tracy 定位到具体 pass 或队列后，再用 RenderDoc 看绘制、着色器与资源细节；
4. 需要编辑器内即时观察 DX12 GPU 时间线且不想连接 Tracy 时，使用 Timeline Profiler。

对稳定态的编辑器或 Play 测量，应在启动与资产恢复完成之后再附加 Tracy；只有测量目标就是启动过程时才从头抓取。

## Timeline Profiler

编辑器注册了一个可停靠的 `Profiler` 面板。在未启用目标端时，面板报告可用性并说明为什么没有实时数据；启用 `NLS_ENABLE_TIMELINE_PROFILER` 后，面板绘制 Timeline Profiler HUD。

Timeline Profiler 的 GPU 支持需要原生 DX12 设备与图形队列已初始化。CPU 作用域在 GPU 不可用时仍然有效。

## GPU 范围与队列

- 只有声明支持 `ProfilerCapability_GPUScopes` 的目标端会收到 GPU 事件；
- DX12 后端把 GPU 作用域桥接到 Timeline Profiler 的 D3D12 时间戳分析器（需要 `NLS_ENABLE_TIMELINE_PROFILER=ON`）或 Tracy 的 DX12 上下文（需要 `NLS_ENABLE_TRACY=ON`）；
- 其它后端保留 CPU 性能分析，并报告 GPU 作用域不可用。

## RenderDoc

渲染问题的首选工具是 RenderDoc。运行时会探测以下位置：`RENDERDOC_PATH`、`%ProgramFiles%\RenderDoc`、`%ProgramFiles(x86)%\RenderDoc`、`PATH`。未安装时应用正常运行，抓取动作不可用。

### 应用内操作

| 操作 | 位置 |
| --- | --- |
| Queue 下一帧抓取 | `Debugging > RenderDoc > Capture Next Frame`（F11） |
| 打开最近抓取 | `Debugging > RenderDoc > Open Latest Capture`（Ctrl+F11） |
| 打开抓取目录 | `Debugging > RenderDoc > Open Capture Folder` |
| 自动打开 Replay UI | `Debugging > RenderDoc > Auto Open Replay UI` |

### 命令行

```powershell
Editor.exe --renderdoc MyProject.nullus
Editor.exe --capture-after-frames 60 MyProject.nullus
Game.exe --renderdoc
Game.exe --capture-after-frames 120
Editor.exe --backend dx12 --renderdoc MyProject.nullus
```

### 环境变量

| 变量 | 作用 |
| --- | --- |
| `NLS_RENDERDOC_ENABLE=1` | 启用 RenderDoc |
| `NLS_RENDERDOC_CAPTURE=1` | 启动时排队一次抓取 |
| `NLS_RENDERDOC_CAPTURE_AFTER_FRAMES=N` | 第 N 帧后抓取 |
| `NLS_RENDERDOC_CAPTURE_DIR=<path>` | 自定义抓取目录 |
| `NLS_RENDERDOC_CAPTURE_LABEL=<name>` | 抓取文件标签 |
| `NLS_RENDERDOC_AUTO_OPEN=1` | 抓取后自动打开 Replay UI |

命令行参数优先于环境变量，环境变量优先于默认值。

### 抓取输出位置

| 目标 | 目录 |
| --- | --- |
| Editor | `<project>/Logs/RenderDoc/Editor/` |
| Game | `<project>/Logs/RenderDoc/Game/` |

仓库的运行器会在其下再加一层后端名，例如 `TestProject/Logs/RenderDoc/Editor/dx12/`。输出目录优先级是 `--capture-dir`、`NLS_RENDERDOC_CAPTURE_DIR`、项目默认值。

### 运行器

```powershell
py -3 Tools/RenderDoc/renderdoc_runner.py --target editor --backend dx12 --capture --open-capture-ui
```

运行器默认使用 `auto` 启动模式，在 DX12 下可调用 `renderdoccmd capture`；它会在 `App` 下定位可执行文件、默认把 Editor 项目设为 `TestProject/TestProject.nullus`、把抓取写入 `<project>/Logs/RenderDoc/<target>/<backend>`，并在抓取后报告确切的 `latest_capture` 路径。

### 抓取分析

```powershell
py -3 Tools/RenderDoc/rdc_analyze.py TestProject/Logs/RenderDoc/Editor/dx12/editor_dx12_DX12_frame2.rdc
py -3 Tools/RenderDoc/rdc_analyze.py path\to\capture.rdc --focus-eid 66
py -3 Tools/RenderDoc/rdc_analyze.py path\to\capture.rdc --json-out Build\RenderDocAnalysis\capture.json
py -3 Tools/RenderDoc/rdc_analyze.py path\to\capture.rdc --skip-doctor
```

分析器会运行 `rdc doctor`（可用 `--skip-doctor` 跳过）、打开抓取、收集 `info`、`stats`、`passes`、`draws` 与 `events`，自动选取采样焦点事件并加载管线、绑定与着色器元数据，最后打印 Markdown 分析骨架。若 `rdc --session` 失败，会回退到 RenderDoc Python replay。

## 调试策略

- 渲染问题优先使用 RenderDoc；
- 只有 RenderDoc 无法覆盖的桌面或界面问题才退回截图；
- 发生回退时记录请求的后端与实际的后端；
- 性能结论必须来自匹配构建与匹配负载的重复测量。

## HZB 与大场景验证

大场景遮挡验证必须在当前 DX12 运行时路径上进行，并把抓取产物与验证记录一起归档：

```powershell
py -3 Tools/RenderDoc/renderdoc_runner.py --target editor --backend dx12 --capture --open-capture-ui
```

需要记录的内容见[可见性与遮挡剔除](../large-scenes/visibility-occlusion.md)。单元测试不能替代真实 DX12 抓帧证据。

## 常见问题

| 现象 | 原因 |
| --- | --- |
| `TimelineProfiler: Disabled` | 构建开关关闭，需要 `NLS_ENABLE_TIMELINE_PROFILER=ON` |
| `TimelineProfiler: Unavailable` | 目标端已启用但当前环境无法初始化 |
| Tracy 目标端不可用 | 未启用 `NLS_ENABLE_TRACY`，或第三方源码缺失 |
| Tracy 连上但没有 GPU 数据 | 需要 Windows/DX12、队列支持时间戳校准，并以 `--backend dx12` 启动 |
| Timeline GPU 无数据 | 需要原生 DX12 设备与图形队列已初始化；CPU 数据仍然有效 |

## 下一步

- [调试与测试](debugging.md)：行为回归与 CI 覆盖。
- [图形后端与大场景特性](../rendering/backends.md)：能力查询与保守回退。
