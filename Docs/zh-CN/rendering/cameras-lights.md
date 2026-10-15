# 相机、光源与画面输出

相机决定“看什么”，光源决定“怎么亮”。两者都是组件，可以在编辑器中调整，也可以通过脚本驱动。

## 相机

`CameraComponent` 的脚本可访问成员：

| 成员 | 说明 |
| --- | --- |
| `GetFov()` / `SetFov(float)` | 透视投影的视场角 |
| `GetNear()` / `SetNear(float)` | 近裁剪面 |
| `GetFar()` / `SetFar(float)` | 远裁剪面 |
| `GetSize()` / `SetSize(float)` | 正交投影的尺寸 |
| `GetProjectionMode()` / `SetProjectionMode(...)` | 投影模式 |
| `GetClearColor()` / `SetClearColor(Vector3&)` | 清屏颜色 |

投影模式在透视与正交之间切换。切换时仍然使用同一套裁剪与可见性逻辑，`Size` 只在正交模式下生效。

编辑器侧 Scene View 使用独立的编辑器相机，不读取场景中的 `CameraComponent`。进入 Play 后 Game View 使用场景相机，因此“编辑器里看到的角度”和“运行时的画面”是两件事。

## 光源

`LightComponent` 的脚本可访问成员：

| 成员 | 说明 |
| --- | --- |
| `GetLightType()` / `SetLightType(...)` | 光源类型 |
| `GetColor()` / `SetColor(Vector3&)` | 颜色 |
| `GetIntensity()` / `SetIntensity(float)` | 强度 |
| `GetRange()` / `SetRange(float)` | 影响范围 |
| `GetOuterCutoff()` / `SetOuterCutoff(float)` | 外锥角 |

光源按类型进入 clustered lighting 的簇划分。方向光只与主光阴影相关，点光与聚光灯使用 `Range` 与 `OuterCutoff` 界定影响体积。

## 画面输出

渲染输出按使用场景分为三类：

| 输出 | 说明 |
| --- | --- |
| Swapchain | 最终呈现给窗口的画面 |
| 离屏场景目标 | 场景渲染到纹理，再交给输出批次 |
| 编辑器视图目标 | Scene View 与 Game View 各自的输出目标 |

前向路径会先渲染到离屏场景目标，再走单独的交换链输出批次。这样 readback、截图与后续 pass 都能拿到稳定的输入，而不依赖当前交换链图像。

交换链图像的读取在 Present 之前开始，避免“呈现后再读同一张图”带来的同步风险。

## 清屏与天空盒

相机清屏颜色在天空盒缺席时决定背景。存在天空盒时由天空盒 pass 写入背景，清屏颜色只作为回退值。

天空盒是独立 pass，在透明物体之前绘制，因此透明物体可以正确混合在背景之上。

## UI 与相机的组合

`UICanvasComponent` 的渲染模式与相机相关：

| 模式 | 说明 |
| --- | --- |
| `ScreenOverlay` | 覆盖在屏幕上，不参与场景深度 |
| `ScreenCamera` | 与指定相机绑定，可以参与深度测试 |
| `World` | 在世界空间中渲染，可以使用直接路径或渲染目标路径 |

世界空间 UI 的路径选择、深度测试与深度写入开关见[UI 系统概览](../ui/overview.md)。

## 调试辅助

编辑器的 `Debugging > Debug Draw` 可以叠加显示相机与光源的可视化辅助图形，用于检查朝向、范围与锥角。这些绘制只存在于编辑器视口。

## 下一步

- [材质与 ShaderLab](materials.md)：给网格配上表现。
- [图形后端与大场景特性](backends.md)：后端能力如何影响特性启用。
