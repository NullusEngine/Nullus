# Nullus 插件与包系统实施契约

本文件落实最终修订版的 Module → Plugin → Extension → Editor → Package → Player 顺序。NLS_Core 管宿主权威状态、模块事务和扩展租约；NLS_Plugins 管描述、加载和会话；NLS_Packages 只供 Editor 和包工具。默认保证为保存目标配置、重启生效。验收记录见 `PluginSystemValidation.md`，没有运行的验收不计通过。

## 宿主、阶段与二进制边界

Editor/Game 在 Context 构造前创建 ApplicationPlugins/PluginHost，在 Application 完整销毁后销毁宿主。PluginHost 拒绝第二个进程权威实例，ServiceLocator 绑定该实例。Catalog 是元数据目录，会话、模块作用域和内容租约由项目选择持有；不为项目创建另一套 Registry。当前应用采用新进程打开不同项目；在同一管理器中改变项目或原生选择会明确要求重启。

Source Plugin 通过本次目标工具链编译的 STATIC/OBJECT 工厂登记，生成源显式调用 `AddSourceFactory`。工厂携带编译输入指纹，当前源码不匹配时拒绝启动。Binary Plugin 只能使用 SDK C 表，SDK CMake 检查直接和传递的引擎实现链接。真实 DLL 测试分别链接静态和共享宿主实现；插件本身不链接任一宿主实现。

C ABI 的表大小、major/minor、pack(8)、固定宽度类型、cdecl/导出、UTF-8 借用与拷贝、成对分配/释放、上下文 destroy、线程限制、opaque owner、阶段及过期访问规则见 `Runtime/Plugins/SDK/README.md`。先检查 PE/ELF/Mach-O 的 OS/架构/位宽/字节序，再映射库并协商 ABI。依赖路径受宿主发布位置、插件目录和系统库约束。当前 Mach-O 仅支持 thin binary；不会把未验证的 universal binary 当作通过。

| 阶段 | 服务与宿主限制 |
|---|---|
| PreEngine | Context 成员构造前；基础路径、诊断、登记；无资源、RHI、窗口 |
| PostEngine | 已初始化的运行时/资源/脚本；Build 仅拥有显式提供的工具服务 |
| PostEditor | 仅 Editor，实际面板、命令和快捷键 Adapter |
| PrePlay | Editor/Game 每实例初始化一次；Begin/Tick/End 仍由 RuntimePlaySession 驱动 |

模块图拒绝循环、加载阶段倒置及 Runtime 到 Editor/Developer 的传递依赖。Game 部署去掉 Editor/Developer 模块及 Editor-only 内容。NullusBuild 只解析插件元数据、构建和 Cook，不自动加载第三方插件代码。

## 所有权、事务与卸载

每次 Startup 获得 ModuleScope。扩展在提交前不可见；注册、事件撤销、资源清理和异步取消/完成检查显式归属作用域。返回失败或抛出异常时关闭入口、撤销登记、取消并等待已启动工作，再清理。新启动依赖逆序回滚；活动共享使用者与原有显式启动根受到保护。不能只把失败回滚委托给 Shutdown。

私有资源必须自行处理创建失败，并实现无异常、幂等的 RequestStop/AbortStartup/Shutdown 及准确的 IsStopped。TaskGraph/渲染任务或 GPU fence 的拥有者必须通过 TrackAsync 保留执行状态、在完成后报告；取消不等于完成。未纳入协议的第三方原生工作无法由宿主证明已停止，不能宣称可热卸载。

卸载请求始终排队。运行边界按 Running → Quiescing → Revoking → Draining → Cleaning → SafeToUnload 推进：先关闭新调用/租约，再移除发现入口，等既有调用、任务、对象和设备工作完成，再销毁上下文。缓存扩展每次调用仍过 gate。Registry 锁不覆盖回调或析构；自身回调内请求卸载立即返回。清理失败保留失败闭包和模块状态，诊断原始异常；无法排空时标记 RestartRequired，保留映射到进程退出，不强制卸载。

UI Adapter 的菜单、面板和快捷键登记可以单项撤销；面板 lease 在帧边界释放后再 Pump。Runtime 与脚本适配器共享同一个 ExtensionRegistry。旧 owner 不会复用，新会话无法通过旧句柄访问。

未知组件默认阻止可编辑 Scene/Prefab 实例化。诊断恢复保存原始 ObjectGraph 文档并标记只读；SerializeScene、SerializePrefab 和统一 SaveSceneToPath 拒绝输出，覆盖普通保存、另存和自动保存调用路径。恢复插件后重新读取原文档；没有宣称支持未知组件占位编辑。

## 构建和 Player 冻结

CMake regeneration 已打开。NullusPlugins.cmake 跟踪 manifest、lock、启用配置、解析集合、构建信任、插件 manifest/CMake、源码集合、目标及工具链。已解析输入摘要失配时明确失败。构建授权使用 `package-content-v1`：来源及包内全部普通文件（排除 `.git`）的指纹，覆盖任意后缀的间接脚本和工具；内容未接受或改变时，在 add_subdirectory 之前拒绝执行。旧策略授权必须重新接受。构建输出必须在包外，授权不是 CMake 沙箱。安装操作从不调用 CMake。

配置确定目标并生成工厂源；编译产生与实际宿主二进制绑定的 `Game.source-plugins.json`；BuildPipeline 冻结配置、二进制、内容和资源。Source Plugin 的发布检查平台、架构、配置、引擎及源码输入哈希，且记录必须匹配发布 Game。Binary Plugin 验证目标格式和 ABI，自行承担其运行库对象；声明必要的 runtimeDependencies 并部署。缺失依赖由受限加载器明确拒绝，尚未验证的平台不计通过。

运行清单位于 `Plugins/runtime-plugins.json`，逐文件 SHA-256；Player.info 保存该清单路径及哈希。Game 只读发布清单和发布目录。每个可执行目标按其链接闭包生成部署清单，Game 不再被强制依赖所有共享目标。Shipping 的真实配置和依赖审计单独记录，Debug 构建不替代 Shipping 验收。

## 内容、导入与包

资源继续使用现有数据库和 `Assets/Plugins/<id>/<mount>`。缺失 .meta 的只读资源使用 `nullus.plugin.asset.v1`、规范插件 ID/挂载/相对路径生成 GUID；版本和物理目录不参与。路径统一为 Unicode 15.1 NFC、UTF-8、`/`，拒绝越界及不兼容文件名，用固定 full casefold 检测别名冲突。已有有效 .meta GUID 保留；只读包不回写元数据。

GUID 是逻辑身份。实际 Editor 新鲜度检查加入插件导入输入哈希；Cook 的源内容、元数据、Importer 版本、处理器版本、目标及参数决定缓存有效性。Build 的插件适配器建立临时 Cook 视图并生成虚拟元数据，输出进入现有 ArtifactDB/Artifact 存储和依赖收集。插件字节改变不会因为 GUID 相同而复用旧产物；反向依赖、普通资源引用进入既有闭包。动态引用必须列入 runtimeKeep，保留文件进入 base NPAK；原始流式读取内容同时部署到声明挂载。禁用保存目标配置，当前原生会话保持至重启；撤销挂载后停止新访问，活动内容句柄及缓存读取租约延长底层内容寿命。

PackageManager 提供目录、ZIP、HTTPS Git、Nullus HTTPS Registry；CLI 为 NullusPlugins，Editor 的 Plugin Manager 调用同一后端并在后台执行扫描/网络工作。SemVer 支持精确、比较、^、~、OR、预发布；有效锁优先，更新根主动重新选择。安装先下载/解析/校验整个 Registry 依赖图，最后一次提交；默认禁用新增包。安装、启用、接受源码构建分别操作。

Git 使用完整环境、空系统/用户配置和模板，固定完整 commit，HTTPS transport 是明确允许的外部程序。无 checkout；读取对象并提取受控 ZIP，禁 hooks、filters、子模块、LFS、askpass、凭据助手、自定义 transport 和自动维护。HTTPS 使用正常证书验证、不读默认 curl 配置、显式来源重定向白名单、最多三跳、流式计数。默认下载 1 GiB、展开 4 GiB、十万项；Git 对暂存仓库轮询限额，可能超出一个轮询间隔，超额内容不会提交。ZIP 拒绝穿越、链接、加密和规范化冲突。SHA-256 只证明锁定内容一致，不证明发布者身份；首版信任用户选择的 HTTPS 来源，不提供发布者签名保证。

项目进程锁保护 Manifest、Lockfile、启用配置和日志，缓存按哈希加发布锁及共享读取租约。网络期间不持有项目写锁，提交前重新读最新代并求解；相同 ID 的冲突并发更新失败重试，其他安装意图合并。缓存不可变，同卷临时目录加持久日志、备份和 rename 提交；prepared 崩溃回滚、committed 崩溃前滚。多文件不是一次原子操作，协作读者必须持锁。仅接受已支持的本地文件系统，不承诺网络文件系统、断电或介质故障的绝对可靠性。Windows 短暂扫描器占用只有限重试，失败不覆盖有效项目状态。

## 脚本

C# 复用 CoreClrHost 与 ScriptAssemblyLoadContext，默认运行库提供固定表；用户程序集在 collectible ALC。PluginContext 显式追踪取消、任务、线程、退订、IDisposable 和 GCHandle。先停止并清理，再发起 Unload，WeakReference 证实回收后才完成。未追踪的强引用/线程会阻止卸载并产生诊断及 RestartRequired。

Lua 复用 LuaScriptBackend 的 VM，每插件独立环境、Registry 引用和受控协程。关闭访问后 reset 协程并撤销回调；__close/shutdown 失败保留状态。回调和协程有指令预算，VM 共享寿命由未完成模块延长。没有通过重建游戏 VM 掩盖清理失败。

## 交付顺序

1. Core 模块图、阶段、事务、Manifest 和测试。
2. Catalog/会话、受限加载器、C ABI、SDK、真实 DLL 测试。
3. Editor/Runtime Adapter、内容挂载、Editor Tools/Runtime 示例。
4. 真实 DLL → 冻结清单 → 部署 → 独立 Game 执行。
5. Plugin Manager、持久化目标状态、重启、项目切换和有损保存保护。
6. 本地/ZIP、锁、SemVer、离线恢复、进程锁、基础事务。
7. Git/Registry、恢复/并发、Cook/NPAK、发布隔离及平台强化。
8. C#/Lua、脚本示例、实际清理诊断、SDK 与教程、回归。

远程来源、复杂恢复和通用热卸载不作为前四步的先决条件。阶段代码和验收证据分开记录；本文件不以文档替代功能。
