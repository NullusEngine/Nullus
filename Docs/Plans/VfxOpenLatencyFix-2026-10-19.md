# 粒子资产打开卡顿修复（2026-10-19）

双击 `.vfx` 经 `AssetBrowser::OpenProjectGridItem` 进入 `VfxEditor::OpenDocument`。首帧 `Preview` 原先无条件创建独立 AssetDatabaseFacade 并同步 Refresh，重复扫描工程/引擎资产，默认无资源引用的粒子也会走此路径。无渲染 TaskGraph 时，预览 Shader 编译还会通过 MergeAsync 进入串行帧队列。

改动：

- AssetBrowser 暴露已发布的只读数据库快照，Preview 逐帧复用；不为打开预览重新扫描源目录，引用资源等待索引就绪后重试。
- 保存后不再刷新第二份预览数据库；后续使用 AssetBrowser 发布的快照。
- 缺少渲染 TaskGraph 但应用 JobSystem 存在时，将 Shader 编译放入应用后台队列；帧队列不合并等待。纯工具/测试且未启动 JobSystem 时保留已有串行 drain 合同。
- 后台任务只捕获独立状态；关闭和换代取消发布，保留上一成功图像。后台任务被取消/拒绝时提供明确状态。
- 为完成集成验证，修复当前发射器面板中阻碍构建/回归的颜色重复转换和 Overview 样式栈恢复；模块编辑行为仍由原工作实现负责。

验证：

- `build/vfx-open-editor-final.xml`：24 项通过，0 失败、0 跳过，包含现有 UI/模块/导入以及 DX12/Vulkan 设备回归。
- `VfxPreviewLoading.SerialRenderLaneDoesNotWaitForPreviewCompilationOrClosing` 使用实际 DX12 设备，将后台编译队列阻塞在受控 gate：后续帧命令仍执行、释放 gate 后取得真实预览图、重编译期间保留旧图、关闭后没有迟到结果回写。不是只检查调用耗时。
- 正常 Debug Editor 已链接至 `App/Win64_Debug_Runtime_Shared/Editor.exe`，`--help` 通过。所有运行库 SHA256 与回归程序使用的库一致。
- 构建：`build/vfx-open-delivery-verified-build.log`；部署：`build/vfx-open-deploy.log`；启动：`build/vfx-open-editor-help.log`。
- 默认后置部署因 staging 缺少 NLS_Cored.dll 失败；最终构建临时使用 `PostBuildEventUseInBuild=false`，再从回归实际使用的完整运行库集合通过 DeployRuntime.cmake 部署，没有修改持久构建配置。
- 尚未进行桌面鼠标双击的同工程/同构建前后 Tracy 对比；这里的测试耗时不代表完整桌面资产打开时间，也不构成 Niagara 视觉验收。

最终回归中，编译 gate 保持关闭时的 `Update + Dispatch` 为 0.185500 ms；只代表受控回归的这一段。
