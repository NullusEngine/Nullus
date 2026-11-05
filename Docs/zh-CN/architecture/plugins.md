# 开发、安装和发布 Nullus 插件

先阅读 `Runtime/Plugins/SDK/README.md` 的 ABI、所有权和卸载契约。完整设计、已运行验证和限制分别在 `Docs/Plans/PluginSystemImplementation.md`、`PluginSystemValidation.md`。

## 运行原生示例

在仓库 PowerShell 中执行（已有构建目录为 build）：

```powershell
& Tools/BuildCleanEnvironment.ps1 cmake --build build --config Debug --target NullusRuntimeSamplePackage NullusPluginSDKPackage
& build/bin/Debug/NullusPlugins.exe D:/MyGame install-dir D:/Code/Nullus/build/plugin-examples/RuntimeSample/Debug
& build/bin/Debug/NullusPlugins.exe D:/MyGame enable org.nullus.sample.runtime
```

安装和启用不会执行包内脚本，也不会热替换当前 Editor 中的原生代码。重启 Editor 后 Runtime sample 注册 `runtime.play` 扩展；Play Tick 增加计数。Plugin Manager 位于 Window 菜单，可查看来源、版本、依赖、目标/运行状态及诊断。禁用和更新保存后按提示重启。

EditorTools 是同次构建的源码示例。把它和 RuntimeSample 放入项目 Plugins，启用两者；在查看 CMake 内容后执行 `approve-build org.nullus.sample.editor`。然后配置：

```powershell
& Tools/BuildCleanEnvironment.ps1 cmake -S . -B build '-DNLS_PLUGIN_PROJECT_ROOT=D:/MyGame'
& Tools/BuildCleanEnvironment.ps1 cmake --build build --config Debug --target Editor Game
& App/Win64_Debug_Runtime_Shared/Editor.exe D:/MyGame
```

生成的工厂源必须参与宿主构建；未编入目标或源码已改变时会拒绝启动。示例新增 Window 下的状态面板和 Tools 下的日志命令。Source Plugin 不使用全局静态构造登记。

## Manifest 与后端

原生模块声明 name/type/loadingPhase/hosts，以及唯一的 binary 或 sourceTarget；需要热卸载能力时显式 safeUnload。必需/可选插件依赖在 dependencies 中，模块依赖在各模块 dependencies 中。Runtime 不能依赖 Editor/Developer；资源挂载命名空间不可重叠，动态读取需要 runtimeKeep。必要私有动态依赖列入 runtimeDependencies。

Lua 示例为 `Examples/Plugins/LuaRuntime`，返回 startup/shutdown 表，使用 host.register_extension 和 host.start_coroutine。插件环境共享现有 VM、隔离游戏全局；用完的资源归属当前模块。

C# 示例为 `Examples/Plugins/CSharpRuntime`，使用仓库 .NET 8 SDK 编译 csproj，将输出 DLL 放入 Binaries。模块声明 scriptBackend=CSharp、script 和 entry；入口实现 Nullus.Plugins.IPlugin。使用 PluginContext 登记扩展、追踪退订/对象/句柄/任务/线程。Unload 是协作过程；静态事件、GCHandle 或线程栈保留引用会要求重启。示例中的故意泄漏类型只在验收宏下编译。

## 包命令

```text
NullusPlugins <project> list
NullusPlugins <project> search <text>
NullusPlugins <project> install-dir <directory>
NullusPlugins <project> install-zip <archive.zip>
NullusPlugins <project> install-git <https-url> <full-commit>
NullusPlugins <project> search-registry <https-index-url> <text>
NullusPlugins <project> install-registry <https-index-url> <id> <range>
NullusPlugins <project> update-registry <https-index-url> <id> <range>
NullusPlugins <project> enable|disable|uninstall <id>
NullusPlugins <project> resolve|validate|restore|repair
NullusPlugins <project> approve-build <id>
```

更新目录来源可再次 install-dir 或 update-dir。restore 仅使用有效 Lockfile 和项目缓存；repair 只能按锁中充分的来源/完整性信息重新取得缺失或损坏内容。源码构建授权绑定来源与包内全部文件（排除 `.git` 版本库目录），包括由 CMake 间接执行的 Python/PowerShell 和工具二进制，不按扩展名判断可执行内容。包内任何文件变化都要求重新 `approve-build`；旧的脚本文件授权策略不再有效。生成文件和编译输出必须放在包外的构建目录，避免写回只读缓存或形成重新配置循环。授权不是 CMake 沙箱；接受后，构建脚本可以运行其中声明的外部步骤。

Registry 是版本化 JSON，不兼容 npm/UPM 服务端协议：顶层 `schemaVersion:1, packages:[]`；每个包含 id/versions，版本含 version、完整 manifest、`archive:{url,sha256}`。默认 archive 和重定向必须同源；嵌入方可在 RemotePackageOptions 指定额外受信来源、工具绝对路径和 CA。Git/curl 来自配置时找到的可信工具；缺失工具明确报错。TLS 与哈希不是发布者签名。

## 发布与故障定位

常规发布使用 NullusBuild/Editor 现有 BuildPipeline。Game 目标及源码插件必须按目标平台/工具链/配置构建；选择相应 Player template。Build 冻结插件配置、产物与内容，把 Runtime 模块、显式依赖和保留资源部署到 Plugins，并将插件清单哈希写入 Player.info。Game 不从源项目或全局 PackageCache 读取插件。

最小原生发布验证可执行 `NullusPlugins <project> stage-player <fresh-output>`，再复制真正 Game 和其 Game 部署清单列出的依赖到输出，运行 `Game --plugin-self-test`。这是真实 Game 的无图形插件诊断入口；它不替代普通 Game 场景/图形运行，也不会假装初始化脚本 VM。

遇到 ABI/目标不匹配先检查 SDK major、位宽、架构及声明的依赖；遇到 Source build stale 重新配置和编译；遇到 restart required 保留当前运行状态并重启；遇到缺失场景类型恢复正确插件后从原文件重载，不保存诊断恢复的部分场景。提交中断先运行 resolve/repair；不要手工删除事务日志或正在使用的缓存。
