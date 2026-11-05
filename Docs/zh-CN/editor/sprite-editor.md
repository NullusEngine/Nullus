# Sprite Editor

在现有项目的 Asset Browser 中右键图片，选择 **Open Sprite Editor**。也可以通过 **Window > Sprite Editor** 打开窗口，再输入 `Assets/Textures/图片.png` 并点击 **Open**。图片必须位于当前项目的 Assets 目录且已导入。

左侧列表选择逻辑 Sprite，右侧显示源图和切片边框。**Select / Move** 可以移动选中矩形；拖动右下角方块调整尺寸。**Create** 可以在源图上拖出矩形。坐标和尺寸以源图像素为单位，原点位于左上角；Pivot 使用从左下角计算的归一化坐标。属性区支持名称、矩形、Pivot 和 Pixels Per Unit。普通素材默认 PPU 为 100。

**Grid Slice** 按 Cell Width/Height、Offset 和 Padding 切分，可选择保留不完整边缘格。**Auto Slice** 为每个四连通不透明区域生成矩形，可以设置 Alpha 阈值与最少不透明像素数。重新生成时，矩形完全一致的切片保留原 UUID、名称和属性。Single 转为多切片时，第一个新切片继承原 Single 引用；删除切片会移除其逻辑引用目标。

拖动过程中只显示预览，释放后记录一次全局 Undo。零距离点击不会修改数据。Escape 取消尚未提交的拖动。**Undo / Redo** 与 Inspector 共用历史记录，关闭并重新打开面板不会建立另一份草稿。

点击 **Apply & Reimport** 保存草稿并重新导入。导入期间暂停编辑，完成后使用新的资源修订。Apply 本身也能撤销或重做，操作会再次执行导入。**Revert** 放弃当前草稿，恢复磁盘上的导入设置。源图片发生变化时，先处理草稿，再点击 **Reload Source**；外部元数据冲突会阻止 Apply，避免覆盖其他修改。

Border 属性区使用 Left、Bottom、Right、Top 四个像素宽度，预览中显示绿色参考线。SpriteRenderer 的 Sliced 模式使用 Size 指定世界尺寸，保持边角的 PPU 尺寸并拉伸中间区域；目标太小时按比例压缩边框。Tiled 模式按原始 PPU 尺寸重复中间区域与边条，末尾不足一个格的部分裁剪 UV，每个 Sprite 最多生成 65,536 个四边形。Simple 继续使用源图尺寸。

C++ 使用 `GetDrawMode / SetDrawMode` 和 `ESpriteDrawMode`；C#、Lua 使用 `GetDrawModeValue / SetDrawModeValue`（0 Simple、1 Sliced、2 Tiled），以及 `GetSize / SetSize`。非法模式或尺寸会被拒绝。Windows/DX12 的资源、序列化、GPU 与独立 Player 验证已通过，所有控件的真实鼠标操作和其他平台仍单独验收。

Trim、Tight Mesh、物理轮廓和 Atlas 编辑仍在实施，完整 Phase 2 验收尚未完成。自动测试、真实桌面鼠标验收和各平台 GPU 验收的状态见 `Docs/Plans/2D/Progress.md`。
