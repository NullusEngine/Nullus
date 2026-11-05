# 编辑器 UI 图标管理体系设计

状态：**修订版（Rev. 2）** · 2026-10-19 · 本文只定义方案，不包含本轮代码改动。
实施进度：**P0–P4 已落地**；**2026-10-21 Unity 风格视觉重构（P5）已落地**：38 个静态 SVG 重绘、5 个字形转为 monochrome SVG、`generated.missing` 洋红诊断图标、126 PNG、4 个 512 Atlas 页；35 项图标测试通过。异步/FA6/归档按触发条件延后。仅余实机 DPI 截图验收（受工作区 WIP 构建阻塞）。
基线：`main @ 0b154c12` + 当前工作区改动。本轮结论全部基于对实际代码的审查（附录 A 列出证据位置），不依赖旧文档推断。

---

## 0. 结论摘要

| # | 决策 | 状态 |
| --- | --- | --- |
| D1 | **SVG 是所有普通静态图标唯一允许人工编辑的图像源**；PNG / Atlas 一律是构建产物，可在 CI 中确定性重建 | P3 |
| D2 | **`icons.manifest.json` 是图标元数据唯一事实来源**；通过 IconBaker 生成 C++ 常量、目录表、资源记录与 PNG，运行时不读 `Tools/Icons` | P3 |
| D3 | 已发布 `editor.icon.*` ID 永久稳定；重命名只通过 alias；内置 ID 生成类型安全常量，插件 ID 运行期注册 | P1 接口 / P3 生成 |
| D4 | 模块方向固定：`Panels → IconPainter → IconService → (IconRegistry/IconCache) → Runtime/RHI`。**Runtime/UI 不依赖 Project/Editor** | P1 |
| D5 | `IconService` 不依赖 ImGui、不暴露 `void*`；解析结果使用引擎已有的 `Render::UI::UiTextureId`（含 generation） | P1（替换试验实现） |
| D6 | `IconPainter` 是编辑器图标绘制唯一入口；**Widget ID 与 Icon ID 强制分离** | P1 |
| D7 | 复用引擎现有 **UiTextureId 注册表 + 按帧延迟销毁**，不自建生成器/快照框架 | P1 |
| D8 | DPI：`IconToken → 逻辑尺寸 → UI/Framebuffer 指标 → 目标像素 → 档位`，禁止重复相乘；档位由 manifest 按图标配置 | P1 逻辑 / P3 数据 |
| D9 | 主题：单色图标白色+alpha，运行期 tint；状态色来自统一 EditorTheme；彩色图标禁用态单独定义 | P1 接口 / P3 主题 |
| D10 | 资产类型 → 图标只有一个权威实现 `AssetIconResolver`；优先 `AssetType`，扩展名只作子类型特例 | P2 |
| D11 | Atlas 构建期可选实现；`ResolvedIcon` 永久保留 `uvMin/uvMax`，接入后面板代码不变 | 已实施 |
| D12 | 字体：确认 ImGui 1.91.0（`ImTextureID = void*`，无 `ImTextureRef`）；Editor 主动注册字形集合，`Runtime/UI` 不包含 Editor 头文件 | P1（独立测试） |
| D13 | Scene Billboard 与 ImGui 图标共用 SVG 源，但保持渲染层独立绘制，不依赖 IconPainter/ImGui | P3 |
| D14 | Icon Browser、CI 校验、热重载属于交付物，不是可选项 | P3 |

**必须现在做**：D4、D5、D6、D7、D10、D12（P1/P2）。
**必须预留接口**：D3 插件注册、D8 多档位、D11 UV、D13 场景位图、热重载 generation。
**延后到性能数据**：异步图标加载、字体系统升级（FA6）；图标使用统计与可选 Atlas 已落地。
**保持现有实现**：`EditorResourceCatalog` 的路径解析职责、`EditorResources::GetTexture` 兼容 API、`RHIImGuiTextureRegistry`、`UIManager` 纹理桥接、缩略图管线。

---

## 1. 代码审查结果（现状事实）

### 1.1 图标载体与加载

| 事实 | 证据 |
| --- | --- |
| 20 个图标 ID 登记在 `EditorResourceCatalog::DefaultRecords`，含开发/打包两条路径 | `Project/Editor/Core/EditorResourceCatalog.cpp` |
| `EditorResources` 构造时**同步加载全部** `EditorResourceType::Icon` 记录（20 张 PNG） | `Project/Editor/Core/EditorResources.cpp`（`loadEditorIcon` 循环） |
| `TextureLoader::CreateFromImageFile` = `Image(stbi)` 同步解码 + `Texture2D::SetTextureResource` 同步建 GPU 资源 | `Runtime/Rendering/Resources/Loaders/TextureLoader.cpp:97`、`Runtime/Base/Image.cpp:38` |
| 纹理创建在调用线程完成（当前为主线程），无异步/暂存队列 | 同上 |
| `RawTextures.h` 仅保留 `EMPTY_TEXTURE`；`ICON_*` / `BILL_*` / `BUTTON_*` 已全部随生成流程/IconPainter 迁移删除 | `Project/Editor/Resources/RawTextures.h`、`EditorResources.cpp` |
| `asset-scene.png` / `asset-audio.png` 已补齐，catalog 已修正 | P0 记录见 §16 |

### 1.2 ImGui 与纹理桥接（决定 API 形态）

| 事实 | 证据 |
| --- | --- |
| **ImGui 1.91.0 WIP**；`typedef void* ImTextureID`；不存在 `ImTextureRef` | `ThirdParty/ImGui/imgui.h:30`、`:250` |
| Docking 开启：Editor `EnableDocking(true)`；多视口（`ViewportsEnable`）在全仓库**未启用** | `Project/Editor/Core/Context.cpp:1246`；搜索无 `ImGuiConfigFlags_ViewportsEnable` |
| 引擎已有 `Render::UI::UiTextureId { value, generation }`，可打包为 ImGui 纹理 ID（`PackUiTextureIdForImGui`） | `Runtime/Rendering/UI/UiDrawDataSnapshot.h:29`、`UiDrawDataSnapshot.cpp:93` |
| `RHIImGuiTextureRegistry`：按 `RHITextureView` 注册、带 generation、只接受 `PreviousFrameOrStatic` | `Runtime/Rendering/UI/RHIImGuiTextureRegistry.{h,cpp}` |
| **按帧延迟释放已存在**：`ReleaseTextureView(view, frameId)` + `ReleaseRetiredTextureViewsUpTo(completedFrameId)`，由帧执行器调用 | `RHIImGuiTextureRegistry.h:59`、`RhiCommandListExecutorFrame.cpp:4730` |
| RGBA8 上传支持 **Atlas 区域**：`atlasPageKey/atlasPageSize/atlasX/atlasY`，DX12/Vulkan/Metal 均声明 `UITextureAtlasRegionUpload` | `DriverAccess.h:300`、`RhiCommandListExecutorFrame.cpp:4874`、`DX12Device.cpp:390` |
| `UIManager::ResolveTextureId` 当前每帧注册并返回 `void*`；`ResolveTextureView` 使用 `PreviousFrameOrStatic` | `Runtime/UI/UIManager.cpp:820-854` |

**结论**：不需要自建 generation/snapshot/延迟销毁框架。`IconService` 应持有 `UiTextureId`（值语义 + generation），绘制时由 `IconPainter` 用 `PackUiTextureIdForImGui` 转成 ImGui 纹理引用。

### 1.3 字体

| 事实 | 证据 |
| --- | --- |
| FA4，合并范围 `ICON_MIN_FA..ICON_MAX_FA`（0xf000–0xf2e0，737 码点） | `Runtime/UI/Icons/FontAwesomeIconFont.cpp:36` |
| 每个字体实例（Ruda 12/14/16，启动器 CJK）都合并整段范围；每次 `LoadFont`/`RebuildFonts` 都 `io.Fonts->Build()` | `Runtime/UI/UIManager.cpp:583-595`、`:960-970` |
| `UIManager` 直接调用 `UI::Icons::EnsureFontAwesomeIconFontLoaded`，没有“应用注册字形”的接口 | 同上 |
| `kFontAwesome*Glyph` 10 个常量仅测试引用；Launcher 直接使用 `ICON_FA_*` 宏 | `FontAwesomeIconFont.h`、`Project/Launcher/Core/Launcher.cpp` |
| ImGui 1.91 使用静态图集 + GlyphRanges（动态字体是 1.92 的能力，不可用） | `imgui.h` 版本宏 |

### 1.4 DPI / UI Scale

| 事实 | 证据 |
| --- | --- |
| `UIManager::BeginFrame()` **每帧** `RefreshScale()` | `Runtime/UI/UIManager.cpp:356-358` |
| Scale 来源：`glfwGetWindowContentScale`，clamp 0.75–3.0；macOS 固定 1.0（依赖 `DisplayFramebufferScale`） | `UIManager.cpp:930-947` |
| Scale 变化 → `ApplyStyle` + `RebuildFonts`（图集重建） | `UIManager.cpp:949-958` |
| 无公开 `SetUiScale`、无用户设置项、无多显示器/视口级 DPI | 全仓库搜索；`EditorSettings` 无 appearance 项 |
| 面板自行乘 `UIManager::GetScale()`（如顶栏 `kSceneToolIconSize=16`） | `Project/Editor/Panels/EditorTopBar.cpp:28-29`、`:90-105` |
| ImGui `DisplayFramebufferScale` 由后端处理；UI 快照按逻辑坐标记录 | `UiDrawDataSnapshot` |

**结论**：当前是“单窗口 + 启动/尺寸变化时整体重建”的 DPI 模型。图标像素目标必须同时考虑 `uiScale` 与 `DisplayFramebufferScale`，且只乘一次（见 §10）。

### 1.5 资产类型与图标映射

| 事实 | 证据 |
| --- | --- |
| 正式资产类型已存在：`NLS::Core::Assets::AssetType`（ModelScene/Texture/Shader/ShaderGraph/ShaderFunction/Material/Audio/Scene/Script/Prefab/UIDocument/UIStyleSheet/Font/LocalizationTable/VfxSystem/VfxScript/VfxEmitter） | `Runtime/Core/Assets/AssetMeta.h:13` |
| `AssetTypeFromString` / `InferAssetType(path)` / `InferImporterId` 已提供 | `AssetMeta.h:36-41` |
| 现有图标映射第 1 处：`EditorResources::GetFileIconId`，基于 `PathParser::EFileType` + `.cs/.lua` 特例 | `EditorResources.cpp:337` |
| 第 2 处：`AssetBrowserPresentation::AssetBrowserFallbackIconId` / `AssetBrowserScriptIconId` | `AssetBrowserPresentation.cpp:1191-1233` |
| 第 3 处：`AssetThumbnailService` 的 `AssetThumbnailKindPolicy::fallbackIcon` 表 | `AssetThumbnailService.cpp:468-482` |
| 可信回退判断在 `ResolveAssetBrowserDisplayFallbackIconId` | `AssetBrowserPresentation.cpp:1235` |
| 资产浏览器绘制用 `ImGui::GetWindowDrawList()->AddImage` + `ResolveAssetBrowserTextureHandle` | `AssetBrowser.cpp:8507`、`:9146/9174/9336` |
| `PathParser::EFileType` 已含 `UI_DOCUMENT/UI_STYLE/LOCALIZATION/VFX` 等编辑器类型 | `Runtime/Platform/Utils/PathParser.cpp:96-120` |

### 1.6 模块依赖与打包

| 事实 | 证据 |
| --- | --- |
| `NLS_UI` 依赖 `NLS_Core`、`NLS_Render`、`ImGui`；**不依赖 Editor**，仓库内无 Editor 反向引用 | `Runtime/UI/CMakeLists.txt`、全目录搜索无 `Project/Editor` |
| `EditorProject` 依赖 `NLS_Engine/Scripting/VFXCompiler/MaterialCompiler`（PUBLIC）、`DirectXTex/NLS_StaticMeshBuild`（PRIVATE） | `Project/Editor/CMakeLists.txt:41-51` |
| Editor 源文件 `GLOB_RECURSE CONFIGURE_DEPENDS`，新增生成文件无需手改 CMake | `Project/Editor/CMakeLists.txt:3-10` |
| 打包：`nls_copy_app_assets_to_target` 整体复制 `App/Assets → <target>/Assets`；`ResolveInstallResourceRoots` 兼容 `<root>/Assets` 与 `<root>/App/Assets` | `CMakeLists.txt:392-400`、`Runtime/Platform/Process/Process.cpp:236` |
| 图标加载固定使用 Development 解析（`ResolveDevelopmentPath`），`Packaged` 枚举仅测试使用 | `EditorResources.cpp:407-410` |
| 依赖安装器已有“版本 + 平台 + SHA-256”清单模式 | `Tools/SetupDependencies/setup_dependencies.py` |
| ThirdParty 中**没有** SVG 光栅化库（无 lunasvg/skia/cairo） | `ThirdParty/` 目录清单 |
| CI 有 5 个 workflow；docs 使用 Python 3.x；无图标校验 | `.github/workflows/` |

### 1.7 场景 Billboard

`DebugSceneRenderer` 用 `Vertical_Plane` 网格 + 光照位图 + 材质绘制灯光辅助体（`DebugSceneRenderer.cpp:519-533`），不经过 ImGui。`GetLightTypeTextureName` 把 `ELightType` 映射到 `editor.icon.scene.light.*`（由同一 Manifest 生成）。**统一源资源、保持独立绘制实现**已实现。

### 1.8 已实施状态

| 阶段 | 状态 |
| --- | --- |
| P0（scene/audio 修复、删除 8 段内嵌数组初始化、一致性测试） | **已落地并通过测试**（12 项图标相关测试全绿） |
| P1 核心架构（`IconId`/`IconRegistry`/`IconService`/`IconPainter`、`UiTextureId` 桥接、顶栏迁移、字体子集） | **已落地并通过定向测试**（26 项图标测试 + 2 项字体测试）。`IconService` 不再暴露 `void*`；仍有 6 个手写内置定义等待 P3 换生成表。 |
| P2 资源映射统一（`AssetIconResolver`、三处映射委托、插件扩展名/导入器） | **已落地并通过定向测试**（新增 7 项映射测试，累计 33 项图标测试通过）。`AssetType` 优先，扩展名只做语言/插件特例。 |
| P3 资源工程化核心（Manifest、SVG 源、IconBaker、多档位、生成代码、CI） | **已落地**：38 个静态图标（含 5 个 Scene Billboard、5 个播放控制）SVG 源、`icons.manifest.json`、确定性的 `iconbaker.py`、104 张多档位 PNG、生成表、Catalog 切换、档位选择、Icon Browser + 热重载、Scene Billboard 迁移、图标主题 Dark/Light tint、已发布 ID 保护、`RawTextures` 清理、`validate_icons.py`、`icons.yml`。34 项图标测试通过。 |
| P4 数据驱动项（使用遥测、空闲预加载、诊断、废弃校验、Atlas） | **已落地**：按 ID 使用计数、used/unused 过滤、每帧 2 个图标的空闲预加载、显存/回退/命中诊断、deprecatedBy 校验、构建期 Atlas（512px × 4 页）；35 项图标测试通过。 |

---

## 2. 问题清单（按真实性分级）

### 2.1 真实问题（本次必须解决）

| # | 当前实现 | 位置 | 影响 | 优先级 |
| --- | --- | --- | --- | --- |
| R1 | 图标元数据分散在 4-5 处（路径、类型映射、回退、脚本特例、缩略图策略） | 1.5 节三处 + `EditorResources` | 新增图标/类型需改多处，行为漂移 | P1/P2 |
| R2 | `IconService` 试验实现暴露 `void*`，绕过 `UiTextureId` 与 generation | `Icons/IconService.h` | 无法安全热重载/设备重建；与引擎桥接重复 | P1 |
| R3 | 顶栏自行完成“取纹理→建视图→拿 ID→UV 翻转→缩放→着色→tooltip” | `EditorTopBar.cpp:236-307` | 每个面板重复实现，主题/DPI 无法统一 | P1 |
| R4 | Widget ID 复用图标 ID（`PushID(p_iconId)`） | `EditorTopBar.cpp:255` | 同一窗口两次使用同一图标按钮时 ID 冲突 | P1 |
| R5 | 无主题层：高亮色硬编码、禁用态仅整体降 alpha | `MakeToolbarHighlight`、`Toolbar.cpp`、`UIManager::ApplyStyle` | 无法统一状态色；浅色主题不可用 | P1/P3 |
| R6 | 图标资产只有 PNG，无 SVG 源、无生成工具、无来源/授权台账 | `App/Assets/Editor/Icons/`、`Tools/` | 无法重建、无法审计、多档位不可行 | P3 |
| R7 | 档位/DPI 逻辑不存在：单文件图标 + 面板裸乘 scale | `EditorResources.cpp:221`、`EditorTopBar.cpp:97` | 高 DPI 模糊；未来多档位会重复相乘 | P1/P3 |
| R8 | 运行时没有热重载与代际切换；清缓存会悬挂 TextureView | 无实现 | 迭代慢；未来实现易出 GPU 悬垂 | P3 |
| R9 | 无插件图标注册/注销接口；注册表是编译期常量 | `EditorResourceCatalog` | 插件无法贡献图标 | P1 接口 |
| R10 | 字形范围不可配置；Editor 无法注册自己需要的码点；FA4 全量注入每个字体 | `UIManager.cpp:592/969` | 图集浪费；Runtime/UI 与具体图标集耦合 | P1（独立） |
| R11 | 资产类型映射基于 `PathParser`，而非已有 `AssetType`；`PathParser` 已被塞入编辑器类型 | 1.5 节 | 继续膨胀 Runtime 枚举的风险 | P2 |
| R12 | 无 Icon Browser / 无 CI 校验 / 无诊断指标 | 无实现 | 问题只能靠肉眼发现 | P3 |

### 2.2 真实但可接受（本阶段不处理）

| # | 现状 | 判断 |
| --- | --- | --- |
| A1 | 启动加载首屏 + 后台空闲预加载 | 已用每帧 2 个图标的空闲填充替代全量启动加载；无需异步资源引擎 |
| A2 | `EditorResources::GetTexture` 作为兼容 API 保留 | 迁移期需要；P3 后仅非图标资源使用 |
| A3 | `EMPTY_TEXTURE` 内嵌数组 | 仅剩 `GUIDrawer` 的空纹理；其余内嵌图标/按钮/billboard 数组已删除 |
| A4 | 字体 `io.Fonts->Build()` 在加载字体时同步执行 | 启动期可接受；字体升级独立实施 |

### 2.3 理论风险（当前不构成问题，不提前实现）

| # | 风险 | 结论 |
| --- | --- | --- |
| T1 | 多视口/多显示器 DPI | 全仓库未启用 `ViewportsEnable`；本次只保留 `IconMetrics` 参数，不实现 per-viewport |
| T2 | 图标纹理切换成为 GPU 热点 | 20–100 张独立纹理在 UI 中可忽略；Atlas 由性能数据触发（§11） |
| T3 | 异步图标解码 | 当前同步成本可接受；不引入第二套资源系统 |
| T4 | 设备重建后 registry generation | 现有 registry 的 generation 恒为 1；设备重建路径需 P1 用测试确认（见开放问题 Q2） |
| T5 | 图标 ID 哈希成为热路径开销 | `string_view` 透明哈希 + 缓存可满足；不引入 intern 表这种额外抽象 |

---

## 3. 目标与非目标

### 目标

- G1 新增一个静态图标 = 一个 SVG + 一条 manifest 记录；其余自动生成。
- G2 面板只依赖 `IconPainter`；不接触文件、纹理句柄、UV、DPI、主题。
- G3 `IconService` 与 ImGui 零依赖；`Runtime/UI` 与 Editor 零反向依赖。
- G4 图标 ID 与 GPU 资源解耦；热重载/主题/DPI 变化不改变业务持有的 ID。
- G5 热路径无磁盘 I/O、无解码、无非必要分配；首用加载有明确策略与预算。
- G6 资产类型映射只有一个权威实现，并可被插件扩展。
- G7 CI 可验证“源、清单、生成物、ID”四向一致且可重建。

### 非目标

- 不实现运行时 SVG 解析。
- 不实现独立图标资源引擎/线程池。
- 不改动缩略图生成管线本身。
- 不在本次引入 Atlas、FA6、异步加载。
- 不要求把 `PathParser` 的所有类型搬进 `AssetType` 或反之。

---

## 4. 目标架构

### 4.1 模块依赖（以实际 CMake 为准）

```text
Project/Editor/Panels/*            (ImGui 业务)
        │
        ▼
Project/Editor/Icons/IconPainter   (ImGui 绘制层；唯一入口)
        │
        ├──────────────► Project/Editor/Theme/EditorTheme
        ▼
Project/Editor/Icons/IconService   (无 ImGui)
        │
        ├── IconRegistry           (内置 + 插件；alias；generation)
        ├── IconCatalog            (生成的只读元数据)
        ├── IconAssetLoader        (文件 → Texture2D；当前同步)
        └── IconTextureCache       (path/tier → Texture2D* + UiTextureId)
        │
        ▼
Runtime/UI/UIManager               (纹理桥接：注册/释放 UiTextureId)
        │
        ▼
Runtime/Rendering/RHI              (RHITextureView, UiTextureId, 帧延迟释放)

Project/Editor/Assets/AssetIconResolver  →  IconRegistry
Project/Editor/Assets/AssetThumbnailService → AssetIconResolver（只取回退 ID）
```

硬性约束：

1. `Runtime/UI` **不得** `#include` 任何 `Project/Editor` 头文件；当前仓库满足，新增代码必须保持。
2. `IconService` **不得** `#include <ImGui/imgui.h>`；只使用 `Render::UI::UiTextureId` 等引擎类型。
3. `IconPainter` 是唯一允许把 `UiTextureId` 转成 ImGui 纹理引用的 Editor 组件。
4. `DebugSceneRenderer` 不经过 `IconPainter`/ImGui，继续直接使用渲染资源。

### 4.2 职责边界

| 组件 | 负责 | 不负责 |
| --- | --- | --- |
| `IconRegistry` | ID/alias 解析、内置 + 插件注册、模块卸载、generation | 文件、GPU、DPI |
| `IconCatalog`（生成） | 不可变元数据：kind、尺寸档、token、themeable、tags、source | 运行期状态 |
| `IconService` | 路径请求、档位选择、纹理缓存、Missing 回退、热重载、GPU 生命周期 | 控件交互、tooltip、布局、ImGui ID |
| `IconPainter` | Draw/Button/Toggle/TextButton/MenuItem、悬停/按下、禁用、tooltip、tint、Widget ID | 资源解析、文件 I/O |
| `EditorTheme` | 状态颜色 token | 图标资源 |
| `AssetIconResolver` | AssetType/扩展名/缩略图回退 → IconId | 纹理生命周期 |

### 4.3 数据流

```text
[构建期]
icons.manifest.json + *.svg
        │  IconBaker（固定版本）
        ├── PNG 档位（提交）
        ├── IconIds.generated.h
        ├── IconCatalog.generated.cpp
        ├── IconResources.generated.cpp
        └── iconbaker-report.json（CI 用）

[运行期]
IconId（生成的常量 / 插件字符串）
        │ IconRegistry
        ▼
IconDefinition（不可变）
        │ IconService：targetPixels → 选择档位 → 路径
        ▼
Texture2D + Render::UI::UiTextureId（generation）
        │ IconPainter：UiTextureId → ImTextureID（ImGui 1.91 = void*）
        ▼
ImGui
```

---

## 5. 资源规范：SVG 为唯一静态设计源

### 5.1 目录

| 路径 | 内容 | 是否发布 |
| --- | --- | --- |
| `Tools/Icons/Source/<module>/<name>.svg` | **唯一人工编辑源**（模块：editor/scene/plugins） | 否 |
| `Tools/Icons/icons.manifest.json` | 唯一元数据源 | 否 |
| `Tools/Icons/iconbaker.py` | 生成器 + 内置确定性光栅化器（版本 = 脚本） | 否 |
| `App/Assets/Editor/Icons/<id-name>@<size>.png` | 生成产物（提交） | 是 |
| `Project/Editor/Icons/Generated/*.generated.*` | 生成代码（提交） | 编译进二进制 |
| `Tools/Icons/build_icons.py` / `validate_icons.py` | 工具 | 否 |

命名契约（与现有扁平命名兼容）：

```text
editor.icon.asset.script.csharp  ->  asset-script-csharp@<size>.png
editor.icon.toolbar.move         ->  toolbar-move@<size>.png
```

`<size>` 为实际像素（16/24/32/48/64/128…），由 manifest 为每个图标声明。旧的无后缀文件在 P3 迁移完成后删除；迁移期允许 `legacyFileName` 字段。

### 5.2 强制规则

1. SVG 是普通静态图标唯一人工编辑源；禁止手改生成 PNG。
2. 禁止新增代码内嵌静态图标像素数组。
3. 运行时禁止解析 SVG。
4. 生成物提交 Git，但必须可由固定工具链确定性重建（同输入 → 相同字节）。
5. 所有既有 ID 兼容；文件/路径可以迁移，ID 不变。
6. 不允许长期存在来源不明、无法重建的普通静态 PNG。
7. 每个条目必须有 `license`（name/author/source）；缺失即 CI 失败。

### 5.3 允许的例外（必须在 manifest 中声明 `Raster`）

| 类型 | 示例 | 要求 |
| --- | --- | --- |
| 图标字体 | FontAwesome（FA4） | 记录字体版本、来源、许可 |
| 动态资源缩略图 | Material/Mesh/Texture 预览 | 不进入图标体系，沿用 `AssetThumbnailService` |
| 诊断占位 | Missing 标记 | 当前为 `generated.missing` 的静态诊断图标（16/32/48），不参与未知图标的默认回退链 |
| 无法矢量化的特殊位图 | 品牌 Logo 特例、扫描件 | `"kind": "raster"` + `reason` + `source` + `license` |

Scene Billboard 属于“同源不同绘制”：SVG 源在 manifest 中声明 `"targets": ["ui", "scene"]`，生成 UI 档位与场景位图（128/256），但 `DebugSceneRenderer` 仍独立绘制。

### 5.4 视觉与格式规范

- 单色（Monochrome）：白色 `#FFFFFF` + alpha，绘制时 tint。禁止预乘颜色。
- 彩色（Color）：sRGB、透明背景；禁用态策略见 §12。
- 画布 128×128，内容安全边距 ≥ 4 px；光学对齐以视觉重心为准。
- 输出 RGBA8；小于目标显示尺寸的档位不生成；不为每个 Hover/Selected 生成独立 PNG。
- 默认不生成 mipmap；仅当图标会在同一帧被明显缩小显示（如资源浏览器大图标缩略）时，由 manifest 的 `"mipmaps": true` 逐项开启。

---

## 6. Manifest：唯一元数据来源

### 6.1 Schema（v2）

```json
{
  "schemaVersion": 2,
  "generator": "iconbaker/1",
  "toolchain": {
    "rasterizer": "iconbaker-builtin/1",
    "deterministic": true
  },
  "icons": [
    {
      "id": "editor.icon.toolbar.move",
      "module": "editor",
      "kind": "monochrome",
      "source": "editor/toolbar/move.svg",
      "targets": ["ui"],
      "sizes": [16, 32, 48],
      "defaultToken": "Small",
      "themeable": true,
      "mipmaps": false,
      "aliases": [],
      "tags": ["tool", "transform"],
      "deprecatedBy": "",
      "license": { "name": "CC0-1.0", "author": "Nullus", "source": "original" },
      "legacyFileName": "toolbar-move.png"
    }
  ],
  "rasters": [
    {
      "id": "editor.icon.brand.nullus-mark",
      "module": "editor",
      "kind": "raster",
      "file": "editor/brand/nullus-mark.png",
      "reason": "approved brand bitmap",
      "source": "design/brand/nullus-mark.ai",
      "license": { "name": "Proprietary", "author": "Nullus", "source": "internal" }
    }
  ]
}
```

字段语义：

| 字段 | 必填 | 说明 |
| --- | --- | --- |
| `id` | 是 | `editor.icon.<domain>.<name>[.<variant>]`，全局唯一，发布后不可改 |
| `module` | 是 | `editor` / `scene` / 插件模块名；用于卸载与归属 |
| `kind` | 是 | `monochrome` / `color` / `glyph` / `raster` |
| `source` | 是（非 glyph） | 相对 `Tools/Icons/Source` 的 SVG/位图路径 |
| `targets` | 是 | `ui`（ImGui 纹理）与/或 `scene`（渲染层位图） |
| `sizes` | 是 | 需要生成的像素档位；不做固定 16/32/48 假设 |
| `defaultToken` | 是 | 面板未显式指定时的逻辑尺寸建议 |
| `themeable` | 是 | 仅 monochrome 可 true |
| `mipmaps` | 否 | 默认 false |
| `aliases` | 否 | 旧 ID；禁止循环/悬空 |
| `tags` | 否 | Icon Browser 检索 |
| `deprecatedBy` | 否 | 非空则进入废弃流程，至少保留一个大版本 |
| `license` | 是 | name/author/source |
| `legacyFileName` | 否 | 迁移期旧文件名 |

Glyph 条目只声明 `id/kind/defaultToken` 与码点来源；内置 5 个字形已迁移为 monochrome SVG 并生成 PNG，`Glyph` kind 保留给插件/字体路径。

### 6.2 生成物

| 文件 | 内容 | 使用方 |
| --- | --- | --- |
| `Project/Editor/Icons/Generated/IconIds.generated.h` | `namespace EditorIcons { inline constexpr IconId ... }` | 所有面板 |
| `Project/Editor/Icons/Generated/IconCatalog.generated.cpp` | `IconDefinition` 表（含档位、路径模板、tags、alias） | `IconRegistry` |
| `Project/Editor/Icons/Generated/IconResources.generated.cpp` | `std::vector<EditorResourceRecord> GeneratedIconRecords()` | `EditorResourceCatalog::DefaultRecords()` |
| `App/Assets/Editor/Icons/*.png` | 多档位产物 | `IconService` |
| `Project/Editor/Icons/Generated/IconAtlas.generated.{h,cpp}` | 构建期图集帧表（启用 Atlas 时生成） | `IconService` |

约束：

- 运行时**不读取** manifest/Tools 目录；所有元数据编译进二进制。
- `EditorResourceCatalog` 继续拥有路径解析，但图标记录由生成文件提供，不再手写。
- 打包路径由 IconBaker 从开发路径确定性推导（小写、`editor/icons/<same-name>`），不手工维护两套路径。
- 生成代码放在 `Project/Editor/Icons/Generated/`。注意：Editor CMake 的源文件 GLOB **排除了 `/Gen/`**（`Project/Editor/CMakeLists.txt:19-20`），因此生成物不能放进 `Project/Editor/Gen/`，除非显式 `target_sources`；放在 `Icons/Generated/` 可被现有 GLOB 自动纳入，且不触发 `/Gen/` 过滤。
- 插件图标不进 manifest：运行期通过 `IconRegistry::RegisterPluginIcon` 注册；不生成 C++ 常量。
- 生成记录必须与 `App/Assets` 实际文件一一对应；缺失即 CI 失败（§15.2）。

### 6.3 ID 与代码生成策略

- 内置 ID：生成的 `IconId`（`struct IconId { std::string_view value; }`）提供编译期常量，避免字符串拼写错误。
- 插件 ID：`IconId::FromRuntime(std::string_view)`；注册表内部持有 `std::string`，保证生命周期。
- 别名：`IconRegistry::Find` 归一化；生成器检测 alias 冲突与重复。
- 废弃：`deprecatedBy` 生成注释 + 运行期一次性告警；至少保留一个发布周期。

---

## 7. IconBaker

### 7.1 职责

```text
icons.manifest.json + Source/**/*.svg
        │
        ▼
IconBaker (Python, 固定光栅化器)
        ├── 校验 manifest（ID/alias/尺寸/授权/路径）
        ├── SVG → PNG（每个声明的尺寸；targets=scene 额外输出）
        ├── 生成 C++（IconIds / IconCatalog / IconResources）
        ├── 生成 report（输入/输出 SHA-256、尺寸、耗时）
        └── --check 模式：重建到临时目录并与仓库逐字节比较
```

### 7.2 工具链固定

- 当前实现采用**仓库内置的确定性光栅化器**（`iconbaker.py`，Python 标准库，支持扁平 SVG 子集：rect/circle/ellipse/polygon/path）。工具版本即脚本版本，CI 与本地输出字节一致；不引入网络依赖。复杂 SVG（text/stroke/gradient）会被显式拒绝。
- 后续若需要完整 SVG 支持，可增加可选的 `resvg` 后端；Manifest/生成物格式不变。

> 当前不依赖外部光栅化器：`iconbaker.py` 内置确定性扁平 SVG 光栅化器。若未来需要完整 SVG 支持，可增加可选 `resvg` 后端；Manifest/生成物不变。

### 7.3 与 CMake 的关系

- **构建不依赖 IconBaker**：生成物已提交，普通开发者拉代码即可编译。
- 新增 CMake 自定义目标 `NullusIconCheck`（可选执行）：运行 `build_icons.py --check`，供开发者手动验证。
- CI 单独 job 执行 `--check` + `validate_icons.py`；不进入每个 PR 的编译链路。

### 7.4 校验器（避免脆弱正则）

不使用正则解析 C++ 初始化代码。一致性由**代码生成**保证：

1. `IconIds.generated.h` / `IconCatalog.generated.cpp` / `IconResources.generated.cpp` 由同一 manifest 一次生成。
2. CI 重新生成到临时目录，与仓库逐文件比较（`git diff --exit-code` 等价）。
3. 纯文件级校验补充：ID 唯一、alias 无环、档位文件存在且尺寸正确、无孤儿 PNG、license 完整、打包路径可解析。
4. 已发布 ID 保护：CI 对比 `main` 与当前分支的 `IconIds` 集合，禁止删除；重命名必须伴随 alias。

---

## 8. 核心 API

### 8.1 类型与注册表

```cpp
namespace NLS::Editor::Icons
{
    enum class IconKind { Monochrome, Color, Glyph, Generated, Raster };
    enum class IconToken { Tiny, Small, Default, Medium, Large, Huge };
    enum class IconState { Normal, Hovered, Pressed, Selected, Disabled, Danger };

    struct IconId
    {
        std::string_view value;
        constexpr explicit IconId(std::string_view v) : value(v) {}
        static IconId FromRuntime(std::string_view v) { return IconId{ v }; }
        friend bool operator==(IconId, IconId) = default;
    };

    struct IconTier { std::uint32_t pixels; std::string_view fileName; };

    struct IconDefinition
    {
        IconId id;
        IconKind kind;
        IconToken defaultToken;
        bool themeable;
        std::string_view module;
        std::vector<IconTier> tiers;
        std::vector<std::string_view> aliases;
        std::vector<std::string_view> tags;
        std::string_view deprecatedBy;
    };

    class IconRegistry
    {
    public:
        // 内置表来自 generated catalog；插件在运行期追加。
        void RegisterBuiltins(std::span<const IconDefinition> builtins);
        bool RegisterPluginIcon(IconDefinition definition); // 持有字符串
        void UnregisterModule(std::string_view module);
        const IconDefinition* Find(IconId id) const;        // 含 alias
        std::uint32_t Generation() const;                   // 注册/注销递增
    };
}
```

`IconDefinition` 的字符串所有权：内置表为静态存储；插件注册由 registry 持有（`std::deque<std::string>` 或 `std::unordered_set`），保证 `Find` 返回的视图稳定。

### 8.2 IconService（无 ImGui）

```cpp
namespace NLS::Editor::Icons
{
    struct ResolvedIcon
    {
        NLS::Render::UI::UiTextureId texture; // value + generation
        Maths::Vector2 uvMin { 0.0f, 0.0f };
        Maths::Vector2 uvMax { 1.0f, 1.0f };
        float pixelSize = 0.0f;   // 选中的档位
        bool tintable = false;
        bool valid = false;
        std::uint32_t registryGeneration = 0;
    };

    struct IconLoadResult
    {
        ResolvedIcon icon;
        enum class Status { Resolved, Fallback, Missing } status = Status::Missing;
    };

    class IconService
    {
    public:
        struct Bindings
        {
            // 资源路径（由 EditorResourceCatalog 提供，保持单一路径解析）
            std::function<std::optional<std::string>(std::string_view resourceId)> resolvePath;
            // 文件 → Texture2D*（当前同步；未来可替换为异步提交）
            std::function<Render::Resources::Texture2D*(const std::string&)> loadTexture;
            std::function<void(Render::Resources::Texture2D*)> destroyTexture;
            // Texture2D 的显式 view ↔ UiTextureId（UIManager 桥接；失败返回无效）
            std::function<Render::UI::UiTextureId(
                const std::shared_ptr<Render::RHI::RHITextureView>&)> registerUiTextureView;
            std::function<void(
                const std::shared_ptr<Render::RHI::RHITextureView>&)> releaseUiTextureView;
            float uiScale = 1.0f;
            float framebufferScale = 1.0f;
        };

        explicit IconService(IconRegistry& registry, Bindings bindings);

        // 目标像素由 IconPainter 根据 metrics 计算后传入；service 只做档位选择。
        IconLoadResult Resolve(IconId id, std::uint32_t targetPixels);

        std::size_t Preload(std::span<const IconId> ids, float uiScale, float framebufferScale);

        void InvalidateModule(std::string_view module); // 安全代际切换，见 §10
        void TickRetirement(std::uint64_t completedFrameId);
        void OnDeviceRecreated();
    };
}
```

说明：

- `ResolvedIcon` 只暴露 `UiTextureId`（值 + generation）与 UV，不暴露 `void*`、`Texture2D*` 或 `RHITextureView`。
- `Bindings` 是唯一与外部交互的接缝，测试可注入 fake；生产由 `EditorResources` 构造一次。绑定使用引擎真实类型（`Texture2D*` / `RHITextureView`），不做类型擦除。
- `Resolve` 不返回 `const char*`，字形走独立的 `ResolveGlyph(IconId, logicalPx)`（返回 UTF-8 + 字体大小），避免位图/字形混在同一结构里造成语义模糊。

### 8.3 IconPainter（ImGui）

```cpp
namespace NLS::Editor::Icons
{
    struct IconMetrics { float uiScale; float framebufferScale; };

    struct ButtonOptions
    {
        const char* tooltip = nullptr;
        bool selected = false;
        bool enabled = true;
        bool danger = false;
        ImVec2 size {}; // 0 => token 计算
    };

    // Widget ID 必须由调用方显式提供；绝不使用 IconId 作为 ImGui ID。
    bool Button(const char* widgetId, IconId icon, IconToken token,
                const ButtonOptions& options = {});
    bool Toggle(const char* widgetId, IconId icon, IconToken token,
                bool value, const ButtonOptions& options = {});
    bool TextButton(const char* widgetId, IconId icon, const char* label,
                    IconToken token, const ButtonOptions& options = {});
    bool MenuItem(const char* widgetId, IconId icon, const char* label,
                  const char* shortcut, bool selected, bool enabled);

    // 自定义绘制（资源浏览器网格、时间轴、节点编辑器）
    void Draw(ImDrawList& drawList, const ImVec2& topLeft, IconId icon,
              IconToken token, IconState state, const IconMetrics& metrics);
}
```

行为约定：

- `Hovered/Pressed` 来自 ImGui 交互状态；`Selected/Enabled/Danger` 来自业务参数；`Disabled` 真正 `BeginDisabled`。
- `Enabled=false` 时不响应点击，tint 使用 `IconDisabled`，不仅降 alpha。
- `tintable=false`（彩色图标）忽略主题色；禁用态使用 `Theme.IconDisabledColorIcon`（约定：整体 35% alpha + 去饱和，P3 由主题实现）。
- `Button` 内部使用 `ImGui::ImageButton` 时，`widgetId` 通过 `ImGui::PushID(widgetId)` 提供；图像 `uv` 来自 `ResolvedIcon`。
- `IconMetrics` 由 `IconPainter` 从 `UIManager` 读取：`uiScale = GetScale()`，`framebufferScale = io.DisplayFramebufferScale`（避免调用方自行相乘）。
- `Draw` 使用 `PackUiTextureIdForImGui(icon.texture.value)` 转 ImGui 纹理引用（ImGui 1.91：`void*`）。

### 8.4 AssetIconResolver

```cpp
namespace NLS::Editor::Assets
{
    class AssetIconResolver
    {
    public:
        static AssetIconResolver& Default();

        IconId ResolveAssetType(NLS::Core::Assets::AssetType type) const;
        IconId ResolvePath(std::string_view path) const;
        IconId ResolveImporter(std::string_view importerId) const;
        IconId ResolveBrowserType(AssetBrowserItemType type) const;
        IconId ResolveThumbnailFallback(AssetThumbnailKind kind,
                                        std::string_view sourcePath = {}) const;

        // 插件：扩展名/导入器 → 图标；不修改 Runtime 的 AssetType 枚举。
        void RegisterExtension(std::string_view extension, IconId icon);
        void RegisterImporter(std::string_view importerId, IconId icon);
    };
}
```

优先级固定：

```text
AssetMeta.assetType（明确类型）
        ↓
扩展名/语言子类型（.cs/.lua/...）
        ↓
PathParser::EFileType（通用文件类型）
        ↓
editor.icon.asset.default
```

`AssetThumbnailService` 只调用 `ResolveThumbnailFallback` 获取回退 ID；保留现有 `ResolveAssetBrowserDisplayFallbackIconId` 的“可信回退”业务语义。

---

## 9. 加载、缓存与 GPU 生命周期

### 9.1 加载策略

| 阶段 | 行为 |
| --- | --- |
| 启动 | 预加载首屏集合：顶栏 7 个 + `asset.default` + 当前可见面板的少量图标（显式列表，不扫描全部） |
| 首次使用 | 同步解码 + 创建纹理；每图标一次性；记录耗时（>5 ms 告警） |
| 启动后空闲 | `PreloadIdleStep(2, 32)`：每帧最多 2 个未解析图标，约 20 帧填满，避免首次打开面板卡顿 |
| 后续 | 命中缓存：`IconRegistry::Find`（哈希）→ `IconService` 的 `(id, pixels)` 决策缓存 → `UiTextureId`，**无磁盘/解码/分配** |
| 压力 | 若首用卡顿超预算，P4 复用 `DriverUIAccess::RequestUiRgba8TextureUpload` / 渲染线程上传，不新建异步框架 |

### 9.2 缓存层级

| 层级 | 键 | 失效 |
| --- | --- | --- |
| 定义 | IconId（含 alias） | 插件注册/注销（generation） |
| 档位决策 | `(IconId, targetPixels)` | UI scale 变化/热重载 |
| 文件→Texture2D | 绝对路径 + mip 设置 | 热重载/模块卸载 |
| Texture2D→UiTextureId | `Texture2D*` | 设备重建/注册表释放 |

### 9.3 热重载（安全代际切换）

```text
SVG 修改 → IconBaker 生成新 PNG
        ↓
IconService 加载新 Texture2D（旧条目仍可绘制）
        ↓
注册新 UiTextureId（新 generation）
        ↓
在下一次 Resolve 命中时原子替换缓存条目
        ↓
旧 UiTextureId 通过桥接释放（内部以最近一次 UI 快照帧号作为 retireFrameId）
        ↓
RHIImGuiTextureRegistry 在 safeToReleaseFrameId 后由帧执行器清理
        ↓
IconService::TickRetirement 在确认旧纹理不再被采样后销毁旧 Texture2D
```

规则：

1. 热重载不改变 IconId。
2. 旧条目在新条目就绪前继续服务，**绝不清空缓存**。
3. 旧 GPU 资源由 `RHIImGuiTextureRegistry` 的帧延迟释放兜底；`Texture2D` 析构再延迟至少 2 个已完成帧。
4. 插件卸载 = `UnregisterModule` → 所有该模块条目走同一 retire 流程。
5. 普通 swapchain resize 与 device reset 分开：resize 不影响纹理；device reset 调用 `OnDeviceRecreated()`（清空 UiTextureId → 重新注册，IconId/定义不变）。
6. UI 快照引用的纹理由 `CollectReferencedTextureIdentities` 追踪；`IconService` 不绕过该机制。

### 9.4 所有权

- `IconService` 拥有 `Texture2D*`（通过 `destroyTexture` 释放）。
- `UiTextureId` 的所有权属于 `RHIImGuiTextureRegistry`；`IconService` 只持有值。
- `EditorResources` 不再持有图标纹理；其 `GetTexture(id)` 兼容壳委托 `IconService`（非图标 ID 仍走原 map）。
- 面板不得长期缓存 `UiTextureId`/`Texture2D*`，只持有 `IconId`。

---

## 10. DPI、尺寸与档位

```text
IconToken
   │ IconTokenToLogicalPixels
   ▼
LogicalSize（UI 点）
   │ × UIManager::GetScale()          （UI Scale / 内容缩放）
   │ × io.DisplayFramebufferScale     （物理像素比，macOS 等）
   ▼
TargetRasterPixels = round(...)
   │ IconService::Resolve
   ▼
SelectTier：≥ target 的最小档；否则最大档
```

要求：

- 只允许在 `IconPainter` 内完成“逻辑 → 目标像素”的乘法；面板禁止再乘 `GetScale()`。
- 档位由 manifest 按图标声明，不写死 16/32/48；大图标可声明 96/128。
- `IconToken` 仍保留为语义 API，但 `IconPainter` 的 `ButtonOptions.size` 允许显式逻辑尺寸（如 40）用于空状态。
- 档位选择缓存在 `(IconId, targetPixels)`；UI Scale 改变时清理决策缓存（纹理不重载，除非新档位）。
- `DisplayFramebufferScale` 每帧可能变化；`IconMetrics` 每帧读取，不在启动时固化。
- **本仓库的实际不变量**：Windows 下 `uiScale` 承担内容缩放（通常 `framebufferScale=1`）；macOS 下 `uiScale` 固定 1.0，由 `framebufferScale=2` 承担（见 §1.4）。若未来两者同时大于 1，必须先确认 ImGui `DisplaySize` 的坐标系再决定是否相乘，禁止直接套用公式重复放大。
- 多视口未启用；若未来启用，`IconMetrics` 由 `ImGui::GetCurrentWindow()->Viewport` 读取，API 不变。

---

## 11. Atlas（构建期，可选）

已实现（2026-10-20）：构建期图集，面板代码零改动。

- Manifest：`"atlas": { "enabled": true, "pageSize": 512, "padding": 1 }`；按图标声明的档位打包，**`targets` 含 `scene` 的 Billboard 排除**，保持渲染层独立采样单图。
- IconBaker：按高度排序 shelf packing，生成 `editor-icons-atlas-<n>.png`（RGBA8）与 `IconAtlas.generated.{h,cpp}` 帧表（id、像素档、页资源 ID、UV、页尺寸）；1px 边缘出血。
- IconService：优先使用帧 → 页面 `UiTextureId` + UV；页缺失/加载失败自动回退独立档位 PNG；`ResolvedIcon` 结构不变。
- 诊断：图集页按 `pageSize² × 4` 计入估算显存；页纹理按路径去重，多个图标共享一次加载。
- 当前配置产生 4 个 512×512 页（约 4 MB），用于验证链路；`enabled: false` 时完全回到独立纹理，无需改代码。

触发与取舍：Atlas 默认开启仅用于验证；若实机 Profiler 显示独立纹理绑定更优，可在 manifest 关闭。后续可切换到引擎已有的 `UITextureAtlasRegionUpload` 做运行时更新。

---

## 12. 主题、着色与状态

### 12.1 分类

| Kind | 着色 | 禁用态 | 备注 |
| --- | --- | --- | --- |
| Monochrome | 主题色 tint | `IconDisabled` | 白色 + alpha 源 |
| Color | 原样 | 35% alpha + 去饱和（P3） | 文件类型、品牌 |
| Glyph | 主题文字色 | 文字禁用色 | 仅文本内联 |
| Generated | 固定 | 固定 | 运行时/插件生成标记 |
| Raster | 原样 | 按业务 | 必须声明例外 |

### 12.2 EditorTheme token

```cpp
struct EditorTheme
{
    ImU32 IconPrimary;   // 默认
    ImU32 IconSecondary; // 次级/未激活
    ImU32 IconMuted;     // 弱化
    ImU32 IconAccent;    // 强调/选中
    ImU32 IconSuccess;
    ImU32 IconWarning;
    ImU32 IconDanger;
    ImU32 IconDisabled;
};
```

- 颜色来源统一：`ImGuiCol_*` 推导 + 编辑器强调色 token；删除 `MakeToolbarHighlight` 等面板内联颜色。
- 状态映射集中在 `IconStateColor(IconState, tintable)`；面板不写颜色。
- 已实现 `EditorIconThemeVariant { Dark, Light }` 与 `SetEditorIconThemeVariant`；同一 SVG 仅替换 tint。Icon Browser 提供 Dark/Light 预览切换；完整 ImGui 浅色样式仍属于后续主题工作。
- 确需时 manifest 可声明 `themeVariants: {"light": "..."}`；当前所有单色图标共用同一 SVG。
- 不为 Hover/Selected/Disabled 生成独立 PNG。

---

## 13. 字体与字形

### 13.1 已确认事实

- ImGui **1.91.0**：静态图集、GlyphRanges；无 `ImTextureRef`；`ImTextureID = void*`。
- 每个字体实例合并 FA4 全段 737 码点；Editor 使用 3 个字体，启动器另有 CJK 字体。

### 13.2 目标

1. `Runtime/UI` 提供通用接口，不包含 Editor 头文件：

```cpp
class UIManager
{
public:
    // 由应用（Editor/Launcher）在加载字体前设置；Runtime/UI 只消费。
    void SetFontIconCodepoints(std::vector<ImWchar> sortedCodepoints);
};
```

2. Editor 在 `Context` 初始化时调用：

```cpp
uiManager->SetFontIconCodepoints(IconCatalog::UsedGlyphCodepoints());
```

   其中 `IconCatalog` 是生成的只读元数据（`IconCatalog.generated.cpp`），不依赖 ImGui 之外的类型；`Runtime/UI` 只接收一组已排序的 `ImWchar`。

3. `EnsureFontAwesomeIconFontLoaded` 由“全段范围”改为“调用方提供的码点集合”构建 range（连续段合并，最多几十段）。
4. Launcher 维护自己的码点集合（保持现有 `ICON_FA_*` 用法）。
5. 字形只用于必须参与文本排版的场景（CollapsingHeader caret、关闭 ×、搜索）；工具栏/资源类型/状态图标一律位图。
6. 字体优化独立实施、独立测试（`FontAwesomeIconFontTests` 扩展 range 断言）；不与 IconService 重构绑定。
7. FA6/动态字体列为 P4；1.91 不支持 1.92 的动态图集 API，禁止假装可用。

---

## 14. Scene Billboard（渲染层独立）

- manifest 中同一 SVG 声明 `"targets": ["ui", "scene"]`；IconBaker 生成 UI 档位与场景位图（128/256）。
- `EditorResourceCatalog` 生成 `editor.scene.light.point/spot/directional/ambient-box/ambient-sphere` 记录。
- `DebugSceneRenderer` 通过 `EditorResources::GetTexture(id)` 取纹理，继续由场景渲染器绘制；**不依赖 IconPainter/ImGui/UiTextureId**。
- 已实现：5 个 Scene Billboard SVG（`Source/editor/scene-light-*.svg`）生成 128px 位图；`DebugSceneRenderer` 改用 `editor.icon.scene.light.*` ID；`BILL_*` 内嵌数组已删除。
- 语言/绘制实现分离，但源资源统一。

---

## 15. Icon Browser 与 CI

### 15.1 Icon Browser（开发版窗口）

保留原方案，并补充：

- 搜索：ID / alias / tag；过滤：尺寸、状态、Icon 主题、**使用情况（all/used/unused）**、是否缺失。
- 已发布 ID 使用计数与 unused 统计；支持重置计数，为废弃清理提供依据。
- 预览：尺寸滑块、UI Scale 预设（100/125/150/200%）、深/浅主题、Normal/Hovered/Selected/Disabled。
- 诊断：纹理数、估算显存、unused 数量、解析/回退/缓存命中、texture generation。
- 说明：SVG 源定位属于构建期信息，运行时不加载 manifest，因此未在面板中实现。
- 操作：复制 ID、一键重载 editor 图标模块。
- 构建版本检查由 CI `iconbaker.py --check` 负责（运行时不读 manifest）。

### 15.2 CI 校验（10 项）

| # | 校验 | 实现 |
| --- | --- | --- |
| 1 | 所有静态图标有合法源文件 | `validate_icons.py` |
| 2 | manifest ID 全局唯一 | 同上 |
| 3 | alias 无循环/悬空 | 同上 |
| 4 | 每个生成物可重建 | `build_icons.py --check` |
| 5 | SVG 与 PNG 一致 | 同上（重建比较） |
| 6 | 生成尺寸与声明一致 | `validate_icons.py` 读 PNG IHDR |
| 7 | 发布路径可解析 | 校验 `EditorResourceCatalog` 生成记录 + 文件存在 |
| 8 | 已发布 ID 未删除 | `validate_icons.py --baseline-ref origin/main`（删除必须补 alias） |
| 9 | 来源/授权完整 | manifest schema 校验 |
| 10 | 无来源不明的普通静态 PNG | 孤儿文件扫描（排除 `rasters` 声明项） |

额外门禁：生成代码与仓库一致（重新生成后 `git diff --exit-code`）。不通过正则解析 C++。

---

## 16. 迁移路线（修订）

### P0 —— 已完成，保持不变

- `asset-scene.png` / `asset-audio.png` 补齐，catalog 修正。
- 删除 8 段 `ICON_*` 内嵌初始化（“创建后立即销毁”）。
- 新增 `EditorIconConsistencyTests`（文件名/存在性/唯一性/打包键/`GetFileIconId` 登记）。
- `AssetBrowserPresentationTests` 断言更新。
- 验证：12 项图标相关测试通过。

### P1 —— 核心架构（下一阶段）

| 动作 | 说明 |
| --- | --- |
| `IconId` / `IconRegistry` | 类型安全内置 ID（先用静态表，P3 换生成表）；插件注册/注销；alias |
| 重写 `IconService` | 用 `ResolvedIcon { UiTextureId, uv, pixelSize, tintable }` 替换试验实现的 `void*` |
| `UiTextureId` 桥接 | `UIManager` 增加“注册静态纹理/释放”接口；`IconService` 通过 `Bindings` 注入 |
| `IconPainter` | Draw/Button/Toggle/TextButton/MenuItem；Widget ID 显式参数 |
| 迁移 `EditorTopBar` | 7 个按钮改用 `IconPainter`；删除 `GetToolbarIconView`/`ResolveTextureId` 重复逻辑 |
| 字体接口 | `UIManager::SetFontIconCodepoints`；Editor 注册子集；Runtime/UI 不引用 Editor |
| 测试 | 注册表、回退、档位边界、禁用交互、Widget ID 不冲突、字体 range 子集 |

验收：顶栏视觉一致（100/150/200% 截图）；同一窗口重复使用同一图标按钮不冲突；`IconService` 无 ImGui 头文件；`Runtime/UI` 无 Editor include。

P1 实施记录（2026-10-19）：

- 新增 `Project/Editor/Icons/EditorIconCatalog.{h,cpp}`：`IconId`、`IconKind`（含 `Raster`）、`IconDefinition`、`IconRegistry`（alias、模块注销、generation、字形码点）。
- 新增 `Project/Editor/Icons/IconService.{h,cpp}`：`ResolvedIcon` 使用 `Render::UI::UiTextureId` + UV；按路径去重、按图标/像素缓存、`editor.icon.asset.default` 回退、缺失诊断、`InvalidateModule` 两帧延迟回收、`OnDeviceRecreated`、legacy 纹理查询。
- 新增 `Project/Editor/Icons/EditorTheme.{h,cpp}` 与 `IconPainter.{h,cpp}`：Draw/Button/Toggle/TextButton/MenuItem；Widget ID 显式传入；`ComputeIconTargetPixels` 统一 DPI 换算。
- `UIManager` 新增 `RegisterStaticTextureView`/`ReleaseStaticTextureView`（复用 `RHIImGuiTextureRegistry`）与 `SetFontIconCodepoints`；`FontAwesomeIconFont` 支持按码点子集合并，空集合保持旧的全区间接管行为。
- `EditorResources` 拥有 registry/service/painter；`GetTexture` 只提供非 UI 图标纹理（场景 Billboard/EmptyTexture）；UI 图标统一走 `IconPainter`。
- `EditorTopBar` 的 7 个工具按钮与 2 个图标文字按钮迁移到 `IconPainter`，控件 ID 与图标 ID 分离；首帧预加载 8 个图标。
- `Context` 不再注册内置字形码点（为空）；`SetFontIconCodepoints` 保留给插件 glyph。
- 测试：`IconRegistryTests` 6 项、`IconServiceTests` 12 项、`IconPainterTests` 3 项、`EditorIconConsistencyTests` 5 项、`FontAwesomeIconFontTests` 2 项，全部通过（定向链接运行；完整 `ctest` 仍受工作区 WIP 编译错误阻塞）。

### P2 —— 资源映射集成

- `AssetIconResolver`（AssetType 优先，扩展名/PathParser 兜底）。
- `EditorResources::GetFileIconId` 委托；`AssetBrowserPresentation` 与 `AssetThumbnailService` 委托。
- 插件扩展名/导入器映射注册。
- `EditorResources::GetTexture` 兼容壳委托 `IconService`。
- 测试：全 `AssetType`/`AssetBrowserItemType`/`EFileType` 覆盖；旧断言不变。

验收：三处重复表删除；新增资产类型只改 resolver 一处。

P2 实施记录（2026-10-20）：

- 新增 `Project/Editor/Assets/AssetIconResolver.{h,cpp}`：`AssetType` 主映射、路径解析（插件扩展名 → `.cs/.lua` → `InferAssetType` → default）、Browser 类型、缩略图回退、插件扩展名/导入器注册。
- `EditorResources::GetFileIconId`、`AssetBrowserPresentation::AssetBrowserFallbackIconId` / `AssetBrowserScriptIconId`、`AssetThumbnailService::FallbackIconForKind` 全部委托解析器；缩略图策略表删除 `fallbackIcon` 字段。
- 已为 UIDocument/UI Style/Localization/VFX（System/Script/Emitter）/ShaderGraph/ShaderFunction 补齐专用图标；仅 `AssetType::Unknown` 回退 `editor.icon.asset.default`。
- 新增 `AssetIconResolverTests` 7 项：全 `AssetType`、Browser 类型、缩略图 kind、扩展名/语言特例、插件覆盖与清理、导入器映射、结果必为已登记 ID。
- 场景 Billboard 使用 `EditorResourceType::SceneTexture`，由 `EditorResources` 以 NEAREST 独立加载，不经过 IconService/Atlas。\n- 说明：`AssetType` 仍是 Runtime 的封闭枚举；插件通过 resolver 的扩展名/导入器映射接入，不修改 `PathParser`/`AssetType`。

### P3 —— 资源工程化

1. `Tools/Icons/`：manifest、SVG 源、IconBaker、校验器、toolchain 固定。
2. 生成 `IconIds/IconCatalog/IconResources`；`EditorResourceCatalog` 改用生成记录。
3. 现有 20 个图标迁移为 SVG（含 scene/audio 的 P0 源）；工具栏生成 16/32/48；资产图标 32/64/128。
4. `EditorTheme` + 状态色 + 浅色主题基础（单色图标）。
5. `Icon Browser` + 单图标热重载 + 加载诊断。
6. `RawTextures.h` 清理：`BUTTON_*` 迁移到 `editor.icon.tool.*`；`BILL_*` 迁移到 scene targets；仅 `EMPTY_TEXTURE` 保留。
7. CI `NullusIconCheck` job；已发布 ID 保护。
8. 文档手册更新（workspace/editor-extensions）。

验收：新增图标只需 SVG + manifest；`git diff` 无手改 PNG；CI 全绿；DPI 矩阵截图通过。

P3 实施记录（2026-10-20，核心完成）：

- **已完成**：
  - `Tools/Icons/icons.manifest.json`（44 条：21 资源 + 7 工具 + 5 播放 + 5 场景 + 5 字形 + 1 诊断占位）与 44 个 SVG 源（`Source/editor/`）。
  - `Tools/Icons/iconbaker.py`：Python 标准库、内置确定性光栅化器、`--check` 重建比较；输出 104 张 PNG（工具栏/播放控制 16/32/48，资产 32/64/128，Scene Billboard 128）与 `IconIds.generated.h`、`IconCatalog.generated.cpp`、`IconResources.generated.cpp`。
  - `EditorResourceCatalog::DefaultRecords` 改为“手写非图标记录 + 生成图标记录”；`EditorIconCatalog::DefaultDefinitions` 改为生成表。
  - `IconDefinition.tiers` + `IconService` 档位选择（`@<size>` 路径替换）；legacy `GetTexture` 固定取最大档。
  - `Tools/Icons/validate_icons.py`（清单/源/PNG 尺寸/孤儿文件/生成 ID 校验）与 `.github/workflows/icons.yml`（`--check` + 校验）。
  - 测试更新：多档位路径、生成记录文件名（`@128`）；35 项图标测试通过。
- **新增完成**：Icon Browser（搜索/尺寸/状态/主题/Missing 过滤/详情/Copy ID/一键热重载）；Scene Billboard 迁移；播放控制图标与旧 Toolbar 迁移；`RawTextures` 的 `ICON_*`/`BILL_*`/`BUTTON_*` 全量删除；EditorTheme Dark/Light 图标 tint；`validate_icons.py --baseline-ref` 已发布 ID 保护；AssetBrowser 类型/文件夹/缩略图回退图标迁移到 `IconPainter`，删除 `ResolveAssetBrowserTextureHandle`/纹理句柄缓存与 `GetFileIcon` 遗留 API。
- **仍待完成（P3 剩余）**：完整 DPI/实机截图与点击验收（受工作区 WIP 构建阻塞）。

### P4 —— 按需优化

已落地（2026-10-20，数据驱动项）：

- **使用遥测**：按 ID 记录请求/解析次数；Icon Browser 提供 `all/used/unused` 过滤、unused 统计与重置。
- **空闲预加载**：`PreloadIdleStep(2, 32)` 每帧最多加载 2 个未解析图标，约 20 帧完成，避免首次打开面板的卡顿。
- **诊断指标**：纹理数、估算显存、解析/回退/缓存命中；`deprecatedBy` 目标由校验器检查。
- **废弃流程**：`deprecatedBy` + unused 统计 + 已发布 ID 保护，形成可执行的下线闭环。
- **构建期 Atlas**：manifest 配置页尺寸/间距；IconBaker 生成 4 个 512 页 + 帧表；`IconService` 使用页面 `UiTextureId` + UV，页缺失自动回退独立 PNG；scene Billboard 排除。

### P5 —— Unity 风格视觉重构（2026-10-21）

已落地：

- **38 个静态 SVG 重绘**：24 × 24 网格、2 px 视觉笔画、纯色平涂；资源类型使用色相编码，工具/播放保持单色。
- **5 个字形转为 SVG**：`editor.icon.glyph.*` 保留 ID，kind 从 `glyph` 改为 `monochrome`，档位 16/32/48，内置 glyph codepoint 集合为空；Font Awesome 仍可供插件 glyph 使用。
- **脚本家族**：浅色页面；通用脚本大 `</>`；C# 大绿色 `#` + 左上角 `</>`；Lua 大月亮 + 左上角 `</>`。
- **语义修正**：Local 线框立方体、Refresh 单圈箭头、Missing 洋红问号、Scene 空心立方体、Shader 页面 S、Shader Graph 节点路径、Shader Function Subgraph 节点树。
- **`generated.missing`**：改为 `color` 诊断图标（16/32/48），烘焙进 Atlas；未知/加载失败的默认回退仍为 `asset.default`，不改既有回退语义。
- **资源结果**：44 个 manifest 条目、126 个 PNG、4 个 512×512 Atlas 页；`iconbaker --check` / `validate_icons` 通过，35 项图标测试通过。
- **设计稿**：`Docs/Design/EditorIcons/unity-style-icons-v4.png`。

延后（明确触发条件）：

- **异步/渲染线程图标上传**：仅当首用加载持续超出预算；当前空闲预加载已覆盖。
- **FA6/动态字体**：ImGui 1.91 无 1.92 动态图集能力；升级需独立验证，不与图标服务绑定。
- **安装器/归档打包**：当前无归档容器，沿用 `Assets` 目录复制。
- **跨会话使用报表**：当前为会话内计数；需要时再落盘/汇总。

---

## 17. 验收标准

### 资源

- [ ] 普通静态图标只有 SVG 源；PNG/Atlas 全部可重建且与 manifest 一致。
- [ ] manifest 是唯一元数据源；生成代码/资源记录不含手工重复。
- [ ] 20 个存量 ID 全部保留；重命名有 alias。
- [ ] 所有条目有来源与授权；例外项显式声明 `raster`。

### 架构

- [ ] `Runtime/UI` 不包含 Editor 头文件；`IconService` 不包含 ImGui。
- [ ] `IconPainter` 是唯一图标绘制入口；面板不出现 `GetTexture("editor.icon...")`。
- [ ] 资产类型映射只有 `AssetIconResolver` 一处。
- [ ] Scene Billboard 不依赖 ImGui/IconPainter。
- [ ] 内置 ID 类型安全；插件 ID 可注册/注销。

### 运行时

- [ ] 热路径无磁盘 I/O/解码/非必要分配；首用加载有预算与日志。
- [ ] DPI/UI Scale/主题行为一致，无重复相乘。
- [ ] Widget ID 与 Icon ID 分离；重复图标按钮不冲突。
- [ ] 热重载/插件卸载不产生悬挂 `UiTextureId`/`TextureView`。
- [ ] Missing 回退链稳定（精确 → default → 占位）。
- [ ] 设备重建后图标可恢复。

### 工程化

- [ ] IconBaker `--check` 在 CI 可复现。
- [ ] 10 项 CI 校验全绿。
- [ ] Icon Browser 可检索/预览/定位源/查看回退与 generation。
- [ ] 诊断指标：加载耗时、纹理数、缓存命中、回退次数。

---

## 附录 A：审查证据索引

| 主题 | 位置 |
| --- | --- |
| 图标记录/加载 | `Project/Editor/Core/EditorResourceCatalog.cpp`、`EditorResources.{h,cpp}` |
| 内嵌位图 | `Project/Editor/Resources/RawTextures.h` |
| 类型映射（3 处，已统一） | `Assets/AssetIconResolver.{h,cpp}`；委托方 `EditorResources.cpp`、`AssetBrowserPresentation.cpp:1191-1240`、`AssetThumbnailService.cpp:468-482,623` |
| 顶栏绘制与 ID | `EditorTopBar.cpp:68-105`、`:236-307` |
| 缩略图回退 | `AssetThumbnailCache.h:20`、`AssetThumbnailService.cpp:468` |
| ImGui 版本/纹理 ID | `ThirdParty/ImGui/imgui.h:30,250` |
| UiTextureId/注册表/延迟释放 | `Runtime/Rendering/UI/UiDrawDataSnapshot.{h,cpp}`、`RHIImGuiTextureRegistry.{h,cpp}`、`RhiCommandListExecutorFrame.cpp:4730` |
| Atlas 上传能力 | `DriverAccess.h:300`、`RhiCommandListExecutorFrame.cpp:4874`、`DX12Device.cpp:390` |
| 字体/字形 | `Runtime/UI/Icons/FontAwesomeIconFont.{h,cpp}`、`UIManager.cpp:583-595,960-970` |
| DPI | `UIManager.cpp:356,930-958`、`EditorTopBar.cpp:90-105` |
| 正式资产类型 | `Runtime/Core/Assets/AssetMeta.h:13-41` |
| 模块依赖 | `Runtime/UI/CMakeLists.txt`、`Project/Editor/CMakeLists.txt` |
| 打包 | `CMakeLists.txt:392-400,480-488`、`Process.cpp:236` |
| 场景 Billboard | `DebugSceneRenderer.cpp:319-329,515-533` |
| 依赖固定模式 | `Tools/SetupDependencies/setup_dependencies.py` |
| 现有测试 | `Tests/Unit/{EditorIconConsistencyTests,EditorResourceCatalogTests,AssetBrowserPresentationTests,IconServiceTests,IconPainterTests,AssetIconResolverTests,FontAwesomeIconFontTests}.cpp` |

## 附录 B：生成物示例

```cpp
// Project/Editor/Icons/Generated/IconIds.generated.h（由 IconBaker 生成，勿手改）
namespace NLS::Editor::Icons::EditorIcons
{
    inline constexpr IconId AssetScene { "editor.icon.asset.scene" };
    inline constexpr IconId ToolbarMove { "editor.icon.toolbar.move" };
    // ...
}
```

```cpp
// Project/Editor/Icons/Generated/IconResources.generated.cpp（由 IconBaker 生成）
std::vector<NLS::Editor::Core::EditorResourceRecord>
GeneratedIconRecords()
{
    return {
        { "editor.icon.toolbar.move", EditorResourceType::Icon,
          "Editor/Icons/toolbar-move@16.png", "editor/icons/toolbar-move@16.png" },
        // ...
    };
}
```

## 附录 C：开放问题

| # | 问题 | 决策/下一步 |
| --- | --- | --- |
| Q1 | 外部 SVG 光栅化器 | 已决策：P3 使用内置确定性光栅化器（扁平子集）；完整 SVG 支持留作可选 `resvg` 后端 |
| Q2 | DX12/Vulkan device reset 时 `RHIImGuiTextureRegistry` 的 generation/清理路径 | P1 用现有 device-reset 测试确认；必要时在 `UIManager` 增加显式 reset 通知 |
| Q3 | 浅色主题的首版范围 | 先支持单色图标 tint；彩色图标是否需浅色变体在 P3 评审 |
| Q4 | `AssetType` 是否允许插件扩展 | 暂不修改 Runtime 枚举；插件通过 resolver 的扩展名/导入器映射接入 |
| Q5 | 多视口若启用 | `IconMetrics` 已按视口可取；不提前实现 per-viewport 缓存 |
| Q6 | P1 `void*` 版 `IconService` 原型 | 已解决：P1 已重写为 `UiTextureId` 版本并接入顶栏；P3 仅需把手写定义换成生成表 |
