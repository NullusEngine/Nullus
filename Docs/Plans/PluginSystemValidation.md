# 插件系统实施与验收记录

日期：2026-10-29。工作区：`D:/Code/Nullus`。本记录区分源码、自动化、真实 Player 和可见 Editor 交互；没有运行的项目不计为通过。原有未提交修改、未跟踪文件和其他 Editor/构建进程保留，未执行 reset/clean/commit。

## 已交付代码

| 阶段 | 真实实现与接入位置 | 验证边界 |
|---|---|---|
| 1 | `Runtime/Core/Modules/ModuleSystem.*`；Manifest/SemVer/依赖求解；`ModuleScope` 启动事务、执行和资源租约、失败清理及反向回滚 | 启动部分失败/异常、共享依赖、延迟任务排空、阶段/类型/循环冲突自动化 |
| 2 | `Runtime/Plugins` 的 PluginHost、Catalog、Session、Manager；`Platform/Modules/NativeLibrary.*`；C11 SDK 与真实 DLL | 相同宿主源码分别静态/共享链接，实际 DLL 注册、权威 ID、调用、注销和 ABI/目标/导出拒绝；完整静态 Editor/Game 尚未运行 |
| 3 | `Project/Editor/Plugins/EditorExtensionAdapter.*`；`Core/Modules/ModuleServices.h` 和 RuntimePlaySession 接入；ContentSourceTable 插件挂载；两个原生示例 | Source Editor 示例真实菜单、窗口、命令已操作；独立 Game 的 DLL RuntimePlaySession 已运行。Editor 内 Play/暂停/单步尚未计为通过 |
| 4 | `RuntimeDeployment.*`、实际 Game 启动、PlayerDescriptor、BuildPipeline 冻结和各可执行目标部署清单 | 真正 Game 在仓库外加载 DLL 和部署资源；完整 Development BuildPipeline、Cook/NPAK/ZIP/launch 已运行 |
| 5 | `PluginManagerPanel.*`、后台操作、目标/当前状态、重启、项目选择比较；统一场景/Prefab 拒写保护 | 实际 UI 禁用/启用写入配置并显示当前仍运行与重启提示；不同原生版本切换拒绝、代次保留、缺失组件保存入口自动化 |
| 6 | `Runtime/Packages` 的本地/ZIP、内容寻址缓存、Lockfile、求解、离线恢复、修复、源码授权及进程锁/日志 | 实际文件安装、更新、卸载、损坏修复、独立进程并发和崩溃恢复；实际 CMake 目标/静态注册源重新生成 |
| 7 | 受控 Git/curl/HTTPS Registry；完整图提交；Cook/ArtifactDB/NPAK 保留；Shipping 模板隔离与 Windows PE 依赖检查 | 本地真实 TLS/Git HTTP 服务、证书/完整性/重定向、filter/gitlink/LFS/ZIP 拒绝；Windows Shipping 最终运行结果见下文；Linux/macOS 未运行 |
| 8 | CoreCLR/collectible ALC、Lua VM 的统一 Module/Extension Adapter；原生/脚本示例、SDK 包及开发教程 | 实际 Lua 回调/协程清理、C# 回调/协作卸载、GCHandle 阻止卸载诊断、Lua 部分失败和清理失败；脚本 Player 最终结果见下文 |

实施契约见 [PluginSystemImplementation.md](PluginSystemImplementation.md)，使用说明见 [PluginDevelopment.md](../zh-CN/PluginDevelopment.md)，ABI 见 [SDK README](../../Runtime/Plugins/SDK/README.md)。

## 构建与自动化命令

所有根工程 MSVC 构建使用 `Tools/BuildCleanEnvironment.ps1`，并在 Windows 正常访问权限下执行。受限环境曾出现 C1902 PDB-manager mismatch；相同命令在正常权限下进入并完成源码编译，因此不把该现象记为源码故障。反射、自定义生成和增量构建随真实 Editor/Game 构建执行，没有关闭生成器跳过检查。

独立测试工程编译的是实际 Core/Platform/Plugins/Packages 源码，测试 DLL 只包含 SDK；不是模拟 Registry，也没有用此 Host 替代真正 Game 验收。

```powershell
& Tools/BuildCleanEnvironment.ps1 cmake -S Tests/Plugins -B build/plugin-validation -G 'Visual Studio 17 2022' -A x64
& Tools/BuildCleanEnvironment.ps1 cmake --build build/plugin-validation --config Debug --target NullusPluginTestsStatic NullusPluginTestsShared -- /m:1 /v:minimal
ctest --test-dir build/plugin-validation -C Debug --output-on-failure
```

结果：退出 0，4/4 CTest 通过，包含静态和共享各 **38 项** GoogleTest，以及两套 CMake 实际配置/编译/运行测试。授权修正后的最终一轮 81.96 秒。日志：`build/Logs/plugin-trust-build.log`、`plugin-trust-tests.log`；每项输出见 `build/plugin-validation/Testing/Temporary/LastTest.log`。嵌套模块、依赖和资源字段的未知项保留并诊断，仍缺失必需字段时拒绝；CLI 和 Plugin Manager 展示警告。

覆盖真实 DLL 回调中请求卸载、旧 lease 阻止清理、扩展 gate、不同项目原生版本拒绝、NFD 物理资源经 NFC 名字读取/部署、挂载撤销后内容句柄存活、最后句柄释放前缓存独占发布锁不能取得。包测试派生独立进程，并在 prepared/文件替换/committed 持久点退出进程，验证回滚或前滚。

CMake 测试实际改变 Lockfile、解析清单、启用状态、源码/构建声明和来源授权，执行 ZERO_CHECK 并编译、启动 Source Plugin Host。输入摘要过期时明确失败，禁用后生成注册源与链接目标同步移除；SDK 的传递静态引擎链接被配置检查拒绝。新增同来源、同版本、同 CMakeLists 的间接 Python 脚本更新：未重新授权前，实际脚本标记文件不能被改写；授权后执行。旧静态 Host 被真实启动并因输入过期拒绝，重新编译后实际扩展返回 42。

根工程集成命令：

```powershell
& Tools/BuildCleanEnvironment.ps1 cmake --build build --config Debug --target Editor Game NullusBuild NullusPlugins NullusPluginScriptTests NullusPluginAssetTests NullusPluginSerializationTests NullusPluginManagedBuildTests NullusPluginSDKPackage -- /m:1 /v:minimal
ctest --test-dir build -C Debug -R '^NullusPlugin(Script|Asset|Serialization|ManagedBuild)Tests$' --output-on-failure
& Tools/BuildCleanEnvironment.ps1 cmake --build build --config Release --target Game NullusRuntimeSamplePackage -- /m:1 /v:minimal
```

最终默认配置 Debug 构建退出 0（`build/Logs/plugin-complete-debug-retry.log`），四个根工程 CTest 组退出 0（`plugin-complete-integration-tests.log`），合计脚本 3 项、资源 Cook/隔离 4 项、序列化保护 1 项、托管构建/文件占用回归 1 项，31.78 秒。资源组包括真实 Unicode 纹理的首次 Cook、缓存命中和 NPAK 保留；托管回归实际执行 Debug/Release 构建，并检查被占用产物的隔离和同字节复用。此前 Source Editor 示例构建也退出 0（`plugin-demo-build.log`、`plugin-editor-demo-build.log`）。独立 EngineIntegration 工程的真实 DLL 验证记录保留，但不能据此宣称整个 UnitTests 测试集通过。

末轮 Release Game 与真实 Runtime DLL 示例包也构建退出 0（`build/Logs/plugin-complete-release-build.log`）。以上均使用默认空 `NLS_PLUGIN_PROJECT_ROOT`，专用 Source Editor 项目的构建证据另列。

最终重编遇到现有 `Runtime/Engine/Assets/PhysicsAssets.cpp` 使用 `MakeLocalIdentifierInFile` 却没有包含声明头的问题。仅补上 `Engine/Serialize/ObjectGraphDocument.h`，不改动物理行为，其余现有修改保留。

工作区并行新增 ECS 文件期间，末轮 Debug 联编曾在 Editor 链接处缺少 `SceneBakingService`、`ECSBakingPanel` 四个符号（`plugin-complete-debug-build.log`）。随后生成的工程已包含这两个源文件；本任务重新执行上述全部 Debug 目标，均退出 0（`plugin-complete-debug-retry.log`），包括两个新增 ECS 文件的真实编译和最终 Editor 链接。没有删除或回退这些并行修改，首次失败记录保留。

## 真实 Editor 与 Player

测试夹具由 `Tests/Plugins/PrepareAcceptance.py` 创建，只能移动带 `.nullus-plugin-acceptance` 标记的专用项目。没有移动用户项目。夹具有实际 Scene、插件 DLL、保留文本和可 Cook 的 TGA。

Source Editor 示例项目：`build/plugin-acceptance/Project`。通过实际 `resolve`、`approve-build org.nullus.sample.editor` 后，配置 `NLS_PLUGIN_PROJECT_ROOT`，生成 Editor 显式工厂并编译。授权已强化为来源及包内全部普通文件的 `package-content-v1` 指纹，覆盖任意后缀的脚本/工具；静态工厂和发布新鲜度也使用该完整输入集合。构建输出必须在包外。根工程重新审批、配置和 Editor 编译退出 0，日志 `plugin-trust-source-editor.log`。

实际可见交互已补测：启动完成后整理本次新建窗口，在每次点击前用 WindowFromPoint 和 PID/路径/启动时间检查所有权，真正打开 Window 菜单、Source 插件状态面板、Tools 命令及 Plugin Manager。面板显示 `Nullus Editor Tools Plugin` 和从同一 Registry 查询的 Runtime DLL 计数；日志确认命令实际执行两次。通过 UI 禁用 Source 示例后，配置写入 false、当前模块仍运行并显示 Restart required；再通过 UI 启用并读取配置确认恢复为 true。没有向其他窗口发送输入。

截图在本次可视化目录：`plugin-editor-window-menu.png`、`plugin-editor-source-panel.png`、`plugin-editor-tools-menu.png`、`plugin-editor-disabled-pending.png`、`plugin-editor-enabled-restored.png`。对应日志 `plugin-editor-final-ui.stdout.log`、`plugin-editor-final-ui.stderr.log`。

Editor 内 Play 尚未计为通过。第一轮暴露既有托管产物命名只覆盖部分源码的问题：暂存文件齐全，但编译结果与旧同名 DLL 不同。当前工作区的并行修改已加入产物/SDK 指纹、同字节复用及准确复制诊断，本任务保留这些修改，并以真实 Debug/Release 构建与 Windows 文件占用测试验证通过（`plugin-managed-revision-tests.log`，49.55 秒）。专用项目旧托管缓存移入同目录备份，没有删除；重启后日志确认新托管构建已激活。后续测试 Editor 进程退出，未取得完整 Play/暂停/单步及插件计数增长证据，退出原因未被本次日志确认，不能推定通过或归因于插件。其它已有 Editor 未被结束。

最小仓库外发布：

```powershell
python Tests/Plugins/VerifyNativePlayer.py build/bin/Debug/NullusPlugins.exe build/plugin-acceptance/NativePlayerProject App/Win64_Debug_Runtime_Shared/Game.exe build/runtime-manifests/Game-Debug.txt C:/Users/fuche/.codex/visualizations/2026/10/29/01a18640-2cfb-7e00-b46a-520843d69691/PluginPlayer
```

退出 0。复制真实 Game 和其运行时链接闭包，临时移走专用开发项目，仅保留 System32 PATH，Game 从发布目录加载真实 Runtime DLL，执行 Begin/三次 Tick/End 并读取部署资源。结果为该目录 `plugin-validation.json`。本诊断入口不构造图形/脚本 Context，另有普通 Game 运行证据。

完整 Development 发布：

```powershell
& build/bin/Debug/NullusBuild.exe build --project build/plugin-acceptance/NativePlayerProject/PluginAcceptance.nullus --profile plugins-debug --player App/Win64_Debug_Runtime_Shared --output build/plugin-acceptance/FullPlayer --no-sign --validate-launch
```

退出 0；日志 `build/Logs/plugin-player-build.log`。输出 `build/plugin-acceptance/FullPlayer/PluginAcceptance-1.0.0-windows-x64-dx12`，包含实际 Cook 纹理、RuntimeAssetManifest、base NPAK、插件 DLL/保留资源、带插件清单哈希的 Player.info，以及 ZIP 提取和启动验证。初次仅空场景/文本的夹具因现有 Runtime Manifest 要求至少一个 Artifact 被拒绝；加入真实插件纹理后成功，没有伪造 Cook 结果。

普通 Game 运行退出 0，DLL 日志确认 RuntimePlaySession Begin、第三次 Tick、End（`plugin-game-normal-run.log`、`plugin-game-normal-result.json`）。该场景没有相机，图像读回没有有效帧，`frameCaptured=false`，不算 GPU/图像验收。`VerifyPublishedPlayer.py` 固化了这种有界真实运行验证，并分别记录脚本回调、Shipping 描述、禁带文件和是否产生图像。

## A–J 验收解释

| 编号 | 证据与限制 |
|---|---|
| A | 静态/共享两种宿主边界均用真实 DLL 查询同一权威 ID，并验证调用/注销；SDK 不带实现库。实际共享 Game 已运行；完整静态 Editor/Game 未跑 |
| B | Startup 返回失败/异常后扩展、撤销回调、资源清理、异步取消/排空和新依赖回滚；已有共享使用者保留；38 项组内通过 |
| C | DLL 自身回调请求立即返回；调用/外部租约排空后才清理；不支持或被依赖模块进入 RestartRequired；没有宣称通用即时热卸载 |
| D | 实际第二项目修改同 ID 原生版本，OpenProject 拒绝，旧路径/代次及旧 Runtime 扩展保持有效；没有构造冲突会话 |
| E | 实际纹理 Cook/持久 ArtifactDB 验证内容、元数据/配置和 Importer 版本失效；GUID 不变，实际产物内容改变；动态保留通过真实 NPAK |
| F | 两套实际 CMake configure/build/run 测试通过，覆盖 Lockfile、启用、解析集合、源码和构建声明/来源授权。根工程开启 regeneration 并执行反射生成 |
| G | 仓库外真正 Game 和完整 BuildPipeline 输出均加载真实 DLL、运行 Runtime 扩展、读部署资源；源目录不可用且移除开发 PATH |
| H | 真实 Release Game 的 Shipping 发布、普通运行及 23 个 PE 文件的依赖审计通过，无 Native/Managed Editor、Package Manager、Build/Cook 工具依赖或部署文件；见末尾记录 |
| I | 缺失组件默认拒绝实例化；诊断恢复保存原文档并标只读；Scene/Prefab 序列化及统一保存入口拒绝写入，原文件不变；菜单可见交互未跑 |
| J | 独立进程同包/不同包并发安装、项目锁竞争、持久点崩溃恢复和真实缓存损坏恢复通过；不承诺网络文件系统或断电强原子性 |

## 正式限制

- 重启生效是正式保证；SafeToUnload 表示作用域排空/清理已完成。Catalog/工厂仍可能持有库引用，当前宿主不承诺立刻取消库映射。任何未知代码/对象/线程/GPU 引用都不能强制卸载。
- 在同一 Application 内的项目会话重初始化暂采用保守的 RestartRequired；当前 Editor 的跨项目行为沿用新进程打开，不能将该结果描述为任意热项目切换成功。
- Windows x64 是本次实际构建/运行平台。Linux/macOS 的源代码分支存在，但没有本次工具链和运行证据。ELF/Mach-O 依赖搜索/RPATH、系统运行库和跨平台发布强化仍需对应平台验收，PE 依赖审计不代表其他平台已审计。
- 单元/集成日志不替代未取得证据的 Editor Play、图像、第三方插件任意私有线程安全性或整个仓库测试集。可见菜单/面板/命令及启用持久化已有独立截图，不能扩大为全部 Editor 验收通过。
- 包 SHA-256 证明内容一致，不是发布者签名；Git HTTPS transport/curl 是受控但真实的外部工具。安装不执行包内构建，源码脚本需要来源绑定的明确授权。

## 最终回归补录

以下命令均已执行，退出码均为 0。最终恢复 `NLS_PLUGIN_PROJECT_ROOT` 为原先空值；示例项目、源码工厂库和全部专用发布证据保留。默认 Editor 不自动编入该专用示例，按教程显式选择项目后可重新构建它。

| 操作 | 最终证据 |
|---|---|
| 默认配置 Debug Editor/Game/NullusBuild/NullusPlugins/四个集成目标/SDK | `build/Logs/plugin-complete-debug-retry.log`；首次链接失败另见 `plugin-complete-debug-build.log` |
| 默认配置 Release Game 与 Runtime 示例 | `build/Logs/plugin-complete-release-build.log` |
| 静态/共享各 38 项、两套 CMake 构建输入测试 | `build/Logs/plugin-trust-tests.log`，4/4 CTest，81.96 秒 |
| 真实引擎脚本/资源/序列化/托管构建 9 项 | `build/Logs/plugin-complete-integration-tests.log`，4/4 CTest，31.78 秒 |
| 独立工程链接真实 Debug 库的资源 4 项 | `build/Logs/plugin-unicode-cook-run.log`，0.67 秒 |
| 真实托管 Debug/Release 产物区分与文件占用复用 | 独立工程 `build/Logs/plugin-managed-revision-tests.log`，49.55 秒；正式根工程目标末轮 30.22 秒，包含于上述四组测试 |
| 既有 DeployRuntime 回归 | `build/Logs/plugin-deploy-regression.log`，DeployRuntime integration tests passed |
| 最终 C#/Lua/原生完整 BuildPipeline 和 ZIP/启动验证 | `build/Logs/plugin-script-final-build.log` |
| 最终 Shipping 完整 BuildPipeline 和 ZIP/启动验证 | `build/Logs/plugin-shipping-final-build.log` |
| 最终两个普通 Game 的独立运行 | `build/Logs/plugin-script-final-gameplay.log`、`plugin-shipping-final-gameplay.log` |
| Shipping 的 23 个实际 PE 文件依赖审计 | `build/Logs/plugin-shipping-imports.log`，没有 Package/Editor/Build/Cook 依赖 |
| 相关已跟踪差异及插件文本格式检查 | `build/Logs/plugin-complete-whitespace-check.log`，`git diff --check` 退出 0，61 个文本文件无行尾空白；不是功能测试或完整仓库干净状态证明 |

最后两次发布采用新的专用目录，避免把旧发布文件算入本次结果。命令：

```powershell
& build/bin/Debug/NullusBuild.exe build --project build/plugin-acceptance/ScriptProject/PluginAcceptance.nullus --profile plugins-script --player App/Win64_Debug_Runtime_Shared --output C:/Users/fuche/.codex/visualizations/2026/10/29/01a18640-2cfb-7e00-b46a-520843d69691/PluginScriptFinal --no-sign --validate-launch
& build/bin/Debug/NullusBuild.exe build --project build/plugin-acceptance/ShippingProject/PluginAcceptance.nullus --profile plugins-shipping --player App/Win64_Release_Runtime_Shared --output C:/Users/fuche/.codex/visualizations/2026/10/29/01a18640-2cfb-7e00-b46a-520843d69691/PluginShippingFinal --no-sign --validate-launch

python Tests/Plugins/VerifyPublishedPlayer.py C:/Users/fuche/.codex/visualizations/2026/10/29/01a18640-2cfb-7e00-b46a-520843d69691/PluginScriptFinal/PluginScripts-1.0.0-windows-x64-dx12 build/plugin-acceptance/ScriptProject --scripts
python Tests/Plugins/VerifyPublishedPlayer.py C:/Users/fuche/.codex/visualizations/2026/10/29/01a18640-2cfb-7e00-b46a-520843d69691/PluginShippingFinal/PluginShipping-1.0.0-windows-x64-dx12 build/plugin-acceptance/ShippingProject --shipping
```

实际 C# 示例在执行发布前通过仓库 SDK 显式 `dotnet build` 编译，未启用故意泄漏的验收宏；日志 `plugin-published-script-build.log`。BuildPipeline 部署其 DLL、Lua 脚本和自包含 .NET 运行时。两个 Game 运行时都临时移走专用源项目，并仅保留 System32 PATH。

- Script Final：原生、C#、Lua 的 Begin、第三次 Tick、End **9/9** 日志检查通过。Game SHA-256：`28560cd255b7f462cb90b1dfe9c65fa80f91fc0edee2f4517432dfd321b3a877`。
- Shipping Final：真实 Runtime DLL 的 Begin、第三次 Tick、End **3/3** 通过，`Player.info.buildMode=Shipping`。Game SHA-256：`21fc0cfad6b1d0b98abf8e7f53e5d96609eabad3af779f9ebd2d0631c5d28636`。
- 两者目录审计 `forbiddenFiles=[]`、进程退出 0、`sourceProjectUnavailable=true`、`developerPathRemoved=true`；原项目均在 finally 中恢复。
- 两者 `frameCaptured=false`；无相机场景没有被误记为图像/GPU 验收。JSON 与完整日志位于各产品目录的 `plugin-gameplay.json` 和 `plugin-gameplay.log`，构建报告位于其上级发布目录。

SDK 输出：`build/plugin-sdk/NullusPluginSDK.zip`，只有 C 头、CMake 声明与 README，不含引擎实现库。原生示例包位于 `build/plugin-examples/RuntimeSample/Debug`、`Release`。开发文档和四种插件示例均是实际编译/加载路径，尚未通过的 Editor Play、完整静态引擎和其他平台验收仍以上文限制为准。

两个发布产品以各自冻结清单和上述 Game SHA-256 标识。其后完成的完整包内容授权、新鲜度检查及托管构建修正，另由末轮源码构建、两套 CMake 运行测试和四组引擎集成测试验证。
