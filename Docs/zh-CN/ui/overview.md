# UI 系统概览

Nullus 的 UI 是保留模式的：文档描述元素树，运行时按树构建实例、布局并生成绘制快照。UI 既可以画在屏幕上，也可以贴在世界空间的物体上。

## 组成

| 组件 | 作用 |
| --- | --- |
| `UIDocument` | 从 JSON 加载的文档定义，含元素树与样式 |
| `UIDocumentInstance` | 文档在运行时的实例，持有元素树、代次与信号总线 |
| `UIElement` | 元素节点，含类型、布局参数、样式、文本与交互状态 |
| `UIStyleSheet` | 状态化样式表，按状态给元素打补丁 |
| `UIInputRouter` | 命中测试、指针捕获、焦点与键盘输入分发 |
| `UIAnimationPlayer` | 基于轨道的简单动画（当前支持不透明度轨道） |
| `UIDataContext` | 路径化数据绑定与变更订阅 |
| `UISignalBus` | 命名信号与订阅 |
| `UILocalizationTable` | 本地化表与回退语言 |
| `UICanvasComponent` | 把文档实例接入场景、渲染模式与排序 |

## 文档格式

`UIDocument` 当前的 schema 版本是 `1`。每个节点包含：

| 字段 | 说明 |
| --- | --- |
| `nodeGuid` | 节点标识，跨重载保持状态 |
| `type` | 元素类型 |
| `name` / `className` | 名字与样式类名 |
| `layout` | 布局参数 |
| `style` | 内联样式 |
| `text` / `passwordMode` | 文本与密码模式 |
| `requiresRenderTarget` | 是否需要独立渲染目标 |
| `textureId` / `textureGeneration` | 纹理引用与代次 |
| `fontAssetId` / `fontPixelSize` | 字体与字号 |
| `children` | 子节点 |

## 元素类型

`Panel`、`Image`、`NineSlice`、`Text`、`RichText`、`Button`、`Toggle`、`Radio`、`Slider`、`ProgressBar`、`TextField`、`Dropdown`、`ScrollView`、`Scrollbar`、`ListView`。

## 布局模式

| 模式 | 说明 |
| --- | --- |
| `Absolute` | 按绝对坐标定位 |
| `Stack` | 单向堆叠 |
| `Flex` | 弹性布局 |
| `Grid` | 网格布局 |
| `Overlay` | 叠加布局，子元素重叠 |
| `Scroll` | 可滚动容器 |

布局在 `UpdateLayout(viewportSize, dpiScale)` 中计算。文档实例会缓存最后一次布局结果，视口与 DPI 都没有变化时跳过重复布局，同时仍然产出帧作用域的快照。

## 样式

`UIStyleSheet` 支持按状态生效的样式补丁。可用的状态位：

`Hovered`、`Pressed`、`Focused`、`Disabled`、`Checked`、`Selected`、`Invalid`。

补丁可以覆盖背景色、前景色、不透明度等属性。状态由输入路由在交互过程中维护，元素自身也保存 `hovered`、`checked`、`selected`、`invalid` 等状态位。

## 输入与焦点

`UIInputRouter` 负责：

- `HitTest`：按位置命中测试；
- `DispatchPointer` / `DispatchScroll`：指针与滚轮事件；
- `DispatchKey` / `DispatchTextInput`：键盘与文本输入；
- `Navigate`：按方向做焦点导航；
- `CapturePointer` / `ReleasePointer`：指针捕获；
- `Focus` / `ClearFocus`：焦点管理。

事件沿父链构建路径后分发，元素可以用 `UIEventListenerHandle` 管理订阅。

## 动画

`UIAnimationPlayer` 使用轨道模型：`AddOpacityTrack` 为一个元素添加不透明度轨道，返回轨道句柄；`Play`、`Stop`、`Tick` 驱动时间；`RemoveTrack` 与 `Clear` 释放轨道。默认使用不受时间缩放影响的时间。

当前只提供不透明度轨道。更完整的动画属性与曲线属于规划内容，见[发展路线](../roadmap/index.md)。

## 数据绑定与信号

- `UIDataContext` 保存路径到值的映射，值类型为 `bool`、`int64`、`double`、`string`、颜色或二维向量。`Set` 触发变更通知，`Subscribe` 按路径订阅。
- `UISignalBus` 提供命名信号：`Connect` 订阅，`Emit` 广播并返回通知数量。

两者把 UI 与游戏逻辑解耦：UI 不直接持有脚本对象，脚本也不需要遍历元素树。

## 本地化

`UILocalizationTable` 当前 schema 版本为 `1`，包含语言标识与回退语言。查找顺序是当前语言优先、回退语言兜底。

## 渲染模式

`UICanvasComponent` 提供三种渲染模式：

| 模式 | 说明 |
| --- | --- |
| `ScreenOverlay` | 屏幕覆盖，不参与场景深度 |
| `ScreenCamera` | 与指定相机绑定，可参与深度测试 |
| `World` | 世界空间渲染 |

世界空间模式下的渲染路径有三种取值：`Auto`（按需自动选择）、`Direct`（直接绘制到目标）、`RenderTarget`（先渲染到纹理再使用）。需要被其他材质引用时走 `RenderTarget`。

其他影响渲染的属性：

| 属性 | 说明 |
| --- | --- |
| `sortOrder` / `sortLayer` | 排序序号与排序层 |
| `cameraId` | 关联相机 |
| `referenceResolution` | 参考分辨率 |
| `dpiScaleMode` | `ConstantPixelSize` / `ScaleWithScreenSize` / `ConstantPhysicalSize` |
| `dpiMatch` | 缩放模式的插值系数 |
| `pixelsPerUnit` | 世界空间像素密度 |
| `depthTest` / `depthWrite` | 深度测试与写入 |
| `alwaysOnTop` | 置顶显示 |
| `worldSize` | 世界空间尺寸 |
| `updatePolicy` | `EveryFrame` / `OnDemand` / `Manual` |

`updatePolicy` 决定文档实例多久更新一次：每帧、按需或手动触发。静态界面使用 `OnDemand` 可以省掉每帧布局与几何重建。

## 文档重载

`UIDocumentInstance::Reload` 重建元素树，并按 `nodeGuid` 保留兼容的节点状态。重载会递增代次，脚本持有的元素句柄随之失效，这与原生对象的热重载语义一致；不要跨重载缓存元素句柄。

## 脚本访问

脚本通过 `UICanvasComponent` 的成员访问 UI：`GetElementHandle(name)` 由名字取句柄，然后读写文本、数值、勾选状态与焦点。Lua 侧不直接持有 64 位句柄，全部通过名字接口在内部解析。

## 编辑器内预览

编辑器提供 `UIDocumentDesigner` 面板与文档设计模型，可以在编辑器内打开并编辑 UI 文档，见[UI Designer 与编辑器扩展](designer.md)。

## 当前边界

- 动画只覆盖不透明度轨道；
- 没有富文本排版与文本换行的完整实现；
- 主题与样式没有跨项目复用机制；
- UI 与 Prefab、场景共用对象图，但 UI 文档本身是独立 JSON 格式。

## 下一步

- [UI Designer 与编辑器扩展](designer.md)：在编辑器里编辑与扩展 UI。
- [脚本 API 参考](../scripting/api.md)：`UICanvasComponent` 的成员清单。
