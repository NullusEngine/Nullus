# 2D Core 制作入口

本文对应 Phase 1 的已实现入口。构建、自动测试、可见编辑器操作与独立发布的实际验收状态分别记录在 [进度文档](../Plans/2D/Progress.md)；后续制作工具仍按功能矩阵推进。

## 创建项目与场景

在 Launcher 中选择 **2D Core** 模板。模板包含 `Assets/Scenes/Main.scene`、正交 Main Camera 和 Hero Sprite，以及新图片的导入默认值。Scene View 复用现有 **2D** 开关和平面导航。

普通素材默认每世界单位 100 像素：64×64 图片对应 0.64×0.64 世界单位。修改导入器的 Pixels Per Unit 会改变几何尺寸，Transform Scale 则改变场景中该实例的尺寸。

模板采用 1/60 秒固定步长，每帧最多八次固定更新。没有这些项目设置的旧项目继续沿用原有时钟默认值。

## 导入图片与创建 Sprite

1. 将图片放入项目的 Assets 文件夹。2D Core 的 `Assets/ImportDefaults.ini` 使新图片采用 Single Sprite 设置；已有 `.meta` 保留原设置。
2. 在图片导入设置中选择 Sprite 类型，设置 PPU 与 Pivot 后执行重新导入。非法 PPU 会产生诊断并保留上次成功发布的资源。
3. 在资源浏览器展开图片，可以选择其 Sprite 子资源。将子资源拖到 Scene View 或 Hierarchy，创建 SpriteRenderer 对象。
4. 只有一个 Sprite 的图片也可直接拖入场景。包含多个 Sprite 时，需要选择具体子资源。

Texture 仍是图片的主资源，Sprite 使用独立的持久子资源标识。重命名或重新导入图片不应更换现有 Sprite 的标识。删除并新建资源会产生新的身份。

也可从 **2D Objects → Sprite** 创建对象，再向 SpriteRenderer 的 Sprite 字段分配资源。

## 实例属性与保存

SpriteRenderer 提供 Sprite、Color（RGBA）、Flip X、Flip Y、Sorting Layer 和 Order in Layer。Flip 围绕 Sprite 的 Pivot 翻转几何；Color 与图片颜色相乘，Alpha 影响透明覆盖和拾取。

通过 Sorting Layer 下拉菜单编辑项目排序层。排序层以持久 ID 保存，名称和层的位置可单独调整。更高的 Order in Layer 在同层中后绘制；相同排序设置继续按视深度排序。默认透明 Sprite 测试场景深度并关闭深度写入。

拖入场景使用现有子树 Undo 操作；属性修改沿用 Inspector 的 Undo/Redo。保存场景后重新打开，资源引用仍指向相同的逻辑 Sprite。Prefab 与场景使用同一套组件和持久引用序列化。

Scene View 的 Camera/Advanced 控件显示 Sprite 的提交、可见、剔除、批次、Draw Call 及拆批原因，并可切换 CPU 合批路径。摄像机的几何剔除开关也作用于 Sprite。

## 运行与发布验证

使用 Editor 的 Play/Stop 检查场景。正式 Player 通过现有 Build Profiles 与 NullusBuild 使用目标平台工件；不从源图片或 Sprite 着色器作者文件创建运行时资源。

Play 中可以切到 Scene View 拖动物体，也可以修改 Inspector 属性。停止后恢复进入 Play 前的场景状态，包括此前未保存的编辑。脚本宿主恢复失败会记录诊断，场景仍可停止并恢复编辑，Editor 也可正常关闭。

本机开发验证可使用：

```powershell
NullusBuild.exe build --project "D:\Projects\SpriteDemo\SpriteDemo.nullus" --output "D:\Projects\SpriteDemo\Dist" --player "<当前构建的 Player 目录>" --no-sign
```

`--no-sign` 用于本机未签名验证。发布配置和签名继续使用现有构建流程。

对未改动的 2D Core 模板成品，运行：

```powershell
Tools\2D\ValidateTemplatePlayer.ps1 -ProductDirectory "<成品目录>" -EvidenceDirectory "<新的证据目录>" -HideSpriteAuthoringSources
```

该 Windows/DX12 检查会保存实际呈现的 PNG、日志和 `evidence.json`，检查一个可见 Sprite Draw、脚本宿主初始化、原生模块路径及源图片未随内容包发布，并在检查期间隐藏 Sprite 着色器作者文件。它不能替代 Editor 的实际鼠标验收或其他平台验证。

已有场景可以指定 `-ExpectedSubmittedSprites`、`-ExpectedVisibleSprites` 和 `-ExpectedStartScene`，避免为了验收改写场景。可见 Sprite 仍须共用一个批次。

## Sprite Prefab

把 Hierarchy 中的 Sprite 拖到 Asset Browser 文件夹，可以创建并连接 Prefab。修改实例属性后，右键该属性可选择 `Apply Override` 或 `Revert Override`；还原操作支持 Undo/Redo。保存场景会记录实例与覆盖，重新打开时保留连接。

打包阶段从目标平台 Prefab 工件展开运行场景，保留实例属性、层级和场景内引用。作者场景仍保留 Prefab 连接。缺失工件或依赖循环会终止打包并给出诊断。

## 固定种子场景

```powershell
python Tools\2D\GenerateSpriteBenchmark.py --project "D:\Projects\SpriteDemo\SpriteDemo.nullus" --count 10000 --seed 12345
```

生成器创建 `Assets/Scenes/SpriteBenchmark.scene` 与配置记录，保留 Main.scene，默认拒绝覆盖已有基准。它替换源场景中的全部基础 Sprite，使输出数量与 `--count` 一致，并记录源场景哈希。源 Sprite 必须是仅含 Transform 和 SpriteRenderer 的根对象；带 Prefab 连接、额外组件或外部对象引用的场景会给出诊断。配置记录不包含实测性能。比较两条合批路径时，需要相同的输出分辨率、摄像机、后端、线程模式、预热和采样区间。

已有成品的批次功能验证可使用：

```powershell
Tools\2D\ValidateSpriteBenchmark.ps1 -ProductDirectory "<成品目录>" -BenchmarkScene "<已有的 SpriteBenchmark.scene>" -EvidenceDirectory "<新的证据目录>"
```

此脚本在临时目录复制成品并替换副本的启动场景，检查 10,000 Sprite、一个批次、一次 Draw 和实际 GPU 回读。它保留原始成品和工程。记录的总时间包含启动、加载、绘制、回读及退出，不代表稳定帧时间或性能分位数。

Sprite Editor、Atlas、Pixel Perfect、Physics2D、Tilemap、逐帧动画、2D 光照和高级制作能力的入口与验收按后续阶段补齐，参见[完整功能矩阵](../Plans/2D/FeatureMatrix.md)。
