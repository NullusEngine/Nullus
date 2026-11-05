# Editor Scene 身份与 Live Baking

在 Editor 的 `Window > ECS Baking` 打开面板。默认关闭 Live Baking，普通编辑不会
自动启动全场景 Capture。面板提供 `Bake Incremental`、`Bake Full`、`Live Baking`
和 `Export ECS Scene`，显示最近一次诊断、Baker 复用统计、实体列表及组件字段。

当前内置 Baker 生成原生 LocalTransform、Parent 和 WorldTransform；预览是独立
CPU World。Mesh/Sprite、物理、动画和 VFX 的原生 Baker/运行时适配尚未完成。

## 身份如何保存

`PersistentManager` 在 Scene 作用域中关联实际 ObjectGraph ID 与当前对象。
新对象获得随机 GUID，保存时写入 ObjectRecord.id。加载保留文件中的对象、组件
和文档 ID，包括旧格式已经存在的 ID；运行时 InstanceID 只用于查找缓存。
已有文件无需离线迁移，保存后也不会因改名、tag 或遍历顺序而重编号。

同一文件加载为多个 Scene 时，各自使用独立缓存。Scene 销毁会释放该作用域，
定期 Capture/保存清除已销毁对象的缓存。Prefab 实例使用已有实例化器产生的
实例 ID，因此重复拖入同一个模板不会复用实例身份。

Hierarchy 的删除、创建和复制快照保留完整 Scene ID。Undo 恢复真实对象后重绑
原 ID；Duplicate 先产生新身份，随后的 Undo/Redo 保留副本自己的身份。
普通 Prefab 模板保存与 Prefab Stage 的完整持久身份工作流仍需后续实现。

## 自动更新与一致性

`SceneBakingService` 由 Editor 持有，订阅同一个 EditorUndoService。提交、Undo/Redo、
异步完成及历史重置只请求刷新，在后续 Editor 更新点合并为一次 Capture/Bake。
连续编辑、未提交事务、Pending 命令、冻结和损坏的 Undo 历史都会阻止 Capture。
失败保留最近成功的 artifact 和预览。

Live 开启后还每 250 ms 检查一次输入，覆盖没有 Undo 通知的传统 authoring API。
输入相同时复用 Baker 缓存，并保留预览实体句柄。内容改变时沿用 BakingPreview
的候选加载后替换规则；消费者必须通过 authoring ID 重新查找 Entity。
这不是原位实体补丁，也没有磁盘 Baking Cache。

进入 Play/Pause/Step 后停止 authoring Capture，保留 Edit 预览。Play sandbox 不会
替换其缓存或共享这个 World。回到 Edit 后继续更新；真正的 Edit Scene 切换或卸载
会清空旧预览及其缓存。未知插件类型的只读恢复 Scene 不允许 Baking。

## 导出与扩展

输出路径相对于项目 Assets，扩展名必须为 `.ecsscene`。导出重新执行 Full Baking，
通过 WorldArchive 原子写入，然后调用生产 AssetDatabaseFacade.ImportAsset，生成
原生 artifact 与 `.meta`。再次导出保留资产 GUID；导入失败会说明源文件已写出，
并保留预览。原生 Cook、打包和 Player 启动见 [Asset Pipeline](Asset-Pipeline.md)
与 [Player Startup](Player-Startup.md)。

`CaptureSceneBakingSources(scene)` 可供独立 C++ 工具使用。扩展程序可在
`SceneBakingService.GetPipeline()` 注册自己的 Baker，并在其预览 World 的共享
registry 中先注册组件及 codec。
资源 Baker 通过 `SetResourceRevisionProvider` 提供 GUID 对应的内容版本；Live 轮询
会检测版本变化，预览发布前再次校验，依赖缺失时保留旧产物。Editor 扩展可以通过
`Editor.GetSceneBakingService()` 访问这个受 Editor 管理的服务。
Capture/Bake、预览发布和对象解析在所属线程执行，
不会另外创建线程池。系统与 Chunk 并行仍使用已有 JobSystem。

当前面板检查 CPU 产物，不向 Scene View 提交 GPU 绘制，也未接通 Scene View 选择、
通用 ECS Authoring Inspector 或 World/Archetype/System 性能面板。自动化用例和
原生构建证据见 [Validation](Validation.md)；可见鼠标交互需要单独验收。
