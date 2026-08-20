# C# 脚本调试

Nullus 的首版 C# 调试运行在本机 Editor Play 中，Visual Studio 2022 和 VS Code 都通过项目级调试工作区附加到正在运行的 CoreCLR。调试器不会自动进入 Play，除非选择带 `Attach and Play` 的 profile。

## 一次性准备

1. 生成或打开项目工作区：`<Project>/Library/IDE/VisualStudio/Nullus.Project.sln`。
2. 关闭所有 Visual Studio 实例，安装仓库内的 `Tools/Debug/VisualStudioExtension/Nullus.ScriptDebugger.vsix`。
3. 重启 Visual Studio 并重新打开项目级方案。共享的 `build/Nullus.sln` 只用于引擎源码，不作为项目脚本 F5 工作区。

VSIX 是离线包，不依赖系统 `PATH`，也不会在线下载 NuGet 或调试组件。更新 VSIX 时必须先退出 VS，否则正在使用的 MEF 组件无法替换。

## F5 配置

Visual Studio 顶部的方案配置是唯一的原生 Editor 配置来源：

| 方案配置 | 启动的 Editor |
| --- | --- |
| `Debug|x64` | `App/Win64_Debug_Runtime_Shared/Editor.exe` |
| `Release|x64` | `App/Win64_Release_Runtime_Shared/Editor.exe` |

不需要创建单独的 Release profile。以下 profile 都读取当前方案配置：

- `Nullus: C# Scripts`：附加 C# 调试器，保持 Edit 状态。
- `Nullus: C# Attach and Play`：CoreCLR 附加成功后进入 Play。
- `Nullus: Editor + C# Mixed`：同时附加 Native 和 CoreCLR；需要引擎源码及匹配 PDB。

按 F5 后，VSIX 调用项目 `Library/IDE/Nullus.Debug.json` 中的绝对 Broker 路径。Broker 按项目 ID 复用现有 Editor，校验 PID、启动时间、项目和可执行文件；没有匹配实例时只启动一个目标配置的 Editor。

## 断点与热重载

普通 C# profile 不会自动进入 Play。附加完成后点击 Editor 的 Play，`Awake`、`Start` 和 `Update` 断点即可命中。保存 `Assets/**/*.cs` 后，Editor 自动进行 Debug 构建；构建成功时在帧边界切换新的 collectible ALC 并重新绑定 PDB 断点，构建失败则保留旧程序集继续运行。

## 验证和故障排查

VSIX 会将最近一次启动写入 `%TEMP%/Nullus.ScriptDebugger.log`。Release 验证必须同时看到：

```text
configuration=Release
executable=...\Win64_Release_Runtime_Shared\Editor.exe
```

Editor 当前实例还可以通过 `<Project>/Library/IDE/EditorInstance.json` 核对 `editorExecutable` 和 `processId`。如果非正常关闭留下旧记录，下一次 Broker 调用会验证进程和 IPC；确认记录失效后自动清理，不会因为旧 PID 启动第二个 Editor。

原生 Mixed 构建使用仓库内 `Tools/BuildCleanEnvironment.cmd`，它只清理子进程中的重复 `Path`/`PATH`，不修改用户环境。没有引擎源码或 PDB 时应使用 `Nullus: C# Scripts`，不要选择 Mixed。
