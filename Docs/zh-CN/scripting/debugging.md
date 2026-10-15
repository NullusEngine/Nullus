# 脚本调试与热重载

C# 调试运行在本机编辑器的 Play 会话中，通过项目级调试工作区附加到正在运行的 CoreCLR。调试器不会自动进入 Play，除非选择带 `Attach and Play` 的配置。

## 一次性准备

1. 生成或打开项目工作区：`<Project>/Library/IDE/VisualStudio/Nullus.Project.sln`。
2. 关闭所有 Visual Studio 实例，安装仓库内的 `Tools/Debug/VisualStudioExtension/Nullus.ScriptDebugger.vsix`。
3. 重启 Visual Studio 并重新打开项目级方案。

仓库根的 `build/` 方案只用于引擎源码，不作为项目脚本的 F5 工作区。

VSIX 是离线包，不依赖系统 `PATH`，也不会在线下载 NuGet 或调试组件。更新 VSIX 时必须先退出 Visual Studio，否则正在使用的组件无法替换。

## F5 配置

Visual Studio 顶部的方案配置决定启动哪个编辑器：

| 方案配置 | 启动的编辑器 |
| --- | --- |
| `Debug\|x64` | `App/Win64_Debug_Runtime_Shared/Editor.exe` |
| `Release\|x64` | `App/Win64_Release_Runtime_Shared/Editor.exe` |

不需要为 Release 另建 profile。以下 profile 都读取当前方案配置：

| Profile | 行为 |
| --- | --- |
| `Nullus: C# Scripts` | 只附加 C# 调试器，保持编辑态 |
| `Nullus: C# Attach and Play` | C# 附加成功后进入 Play |
| `Nullus: Editor + C# Mixed` | 同时附加原生与 CoreCLR，需要引擎源码与匹配的 PDB |

按 F5 后，VSIX 读取项目内的 `Library/IDE/Nullus.Debug.json` 中的 Broker 路径。Broker 按项目 ID 复用已有编辑器实例，并校验 PID、启动时间、项目与可执行文件；没有匹配实例时才启动一个新编辑器。这样可以避免同一个项目被多个编辑器实例同时写入。

## 断点与热重载

普通 C# profile 不会自动进入 Play。附加完成后在编辑器里点 Play，`Awake`、`Start` 与 `Update` 断点即可命中。

保存 `Assets/**/*.cs` 后编辑器自动执行 Debug 构建：

- 构建成功：在帧边界切换到新的 collectible 程序集并重新绑定 PDB 断点；
- 构建失败：保留上一份有效程序集继续运行，错误进入 Console。

热重载时已存在的脚本实例只经历 `OnDisable` 与 `OnEnable` 环绕的后端替换，不会补造 `Awake`、`Start` 或 `OnDestroy`。

## Lua 调试

Lua 侧提供可选的 LuaPanda 集成，需要构建时打开 `NLS_ENABLE_LUAPANDA_DEBUGGING`（默认关闭）。启用后项目文件中的 `lua_panda_debugging` 与 `lua_panda_port` 决定是否连接以及连接端口。

Lua 没有编译步骤，保存文件后重新进入运行态或触发重载即可生效。

## 验证与故障排查

VSIX 把最近一次启动写入 `%TEMP%/Nullus.ScriptDebugger.log`。Release 验证必须同时看到：

```text
configuration=Release
executable=...\Win64_Release_Runtime_Shared\Editor.exe
```

当前编辑器实例可以通过 `<Project>/Library/IDE/EditorInstance.json` 核对 `editorExecutable` 与 `processId`。非正常退出留下的旧记录会在下一次 Broker 调用时被校验并清理，不会因为旧 PID 而启动第二个编辑器。

原生混合构建使用仓库内的 `Tools/BuildCleanEnvironment.cmd`，它只清理子进程中的重复 `Path`/`PATH`，不修改用户环境。没有引擎源码或 PDB 时应选择 `Nullus: C# Scripts`。

## 检查清单

| 现象 | 检查点 |
| --- | --- |
| 断点不命中 | 是否选了带 Play 的 profile，是否已进入运行态 |
| 启动的是旧编辑器 | 方案配置与 `EditorInstance.json` 是否匹配 |
| 修改脚本后行为不变 | Console 是否有构建错误，是否仍在运行上一份程序集 |
| Mixed 调试无法附加 | 是否有匹配的引擎 PDB，是否使用了环境清理入口 |

## 下一步

- [调试与测试](../quality/debugging.md)：引擎侧的调试与测试入口。
- [脚本系统概览](overview.md)：回看脚本模型。
