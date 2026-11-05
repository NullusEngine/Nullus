# UI 文档资源引用（纹理 / 字体）设计

状态：P2.1–P2.5 已实现，P2.6 部分完成（2026-10-20）。本文确定 `.ui` 文档如何持久引用纹理与字体、
运行时如何解析、编辑器如何选择与预览，以及导入/打包的依赖收集。实现按 §9 的 gate 分阶段进行，
设计未批准前不改运行时格式。

相关代码：

- `Runtime/Engine/UI/UIDocument.{h,cpp}` — `.ui` schema 解析、`NodeDefinition`
- `Runtime/Engine/UI/UIDocumentReference.h` — 已有的文档资产引用（AssetId + 路径）
- `Runtime/Engine/Assets/RuntimeAssetDatabase.{h,cpp}` — `RuntimeAssetRef → 运行时 manifest 条目`
- `Runtime/Core/ResourceManagement/TextureManager.{h,cpp}` — 纹理解析/异步上传/缓存
- `Runtime/Rendering/Context/Driver.cpp` — `DriverUIAccess::RegisterUiTextureView`
- `Runtime/Rendering/UI/UIFontAtlas.{h,cpp}` — 多字体面字形图集（P2.2：`fontAssetId → FontFace`）
- `Project/Editor/Assets/AssetDatabaseFacade.cpp` — `.ui` 导入与 artifact manifest
- `Project/Editor/Inspector/ObjectReferenceSupport.{h,cpp}` — 资产引用选择器
- `Project/Editor/UI/UIDocumentNodeEdit.{h,cpp}`、`UIDocumentInspectorView.{h,cpp}` — 设计器 Inspector

## 1. 现状与问题

| 环节 | 现状 | 问题 |
| --- | --- | --- |
| 纹理 | `NodeDefinition.textureId` / `textureGeneration` 是运行时 `UiTextureId`（RHI 注册表 id） | 不能引用项目纹理资产；换设备/重启后 id 不稳定；设计器无法选择 |
| 字体 | `fontAssetId` 只是图集键；`UIFontAtlas` 只有一份 `m_fontData`/`m_fontState` | 所有 `fontAssetId` 共用同一个字体面；项目字体没有加载路径 |
| `.ui` 导入 | 只验证 JSON、写 artifact；`additionalDependencies` 为空 | 引用的纹理/字体不会进入构建依赖，打包后丢失 |
| 编辑器 | Inspector 的纹理/字体字段是只读数字 | 不能拖放/选择；Game View 与设计器无法一致 |
| 九宫格 | `NineSlice` 类型存在，但无边框字段与九块绘制 | 只能用单张拉伸纹理 |

## 2. 目标与非目标

**目标**

1. `.ui` 用稳定的资产引用（AssetId + subAssetKey + 显示路径）引用纹理与字体，不再手写运行时 id；
2. 游戏与编辑器共用同一套解析接口；设计器预览与 Game View 走同一解析结果；
3. 导入时收集引用为构建依赖，打包后可无源加载；
4. 字体支持多字体面（默认字体行为不变），缺失字体/纹理可见且有诊断；
5. 九宫格作为独立的运行时 gate 接入同一资源引用。

**非目标（本阶段）**

- 不做运行时纹理图集打包（atlas packing）、不做 Mipmap/压缩策略的 UI 专用配置；
- 不做每纹理独立采样器（当前 UI 纹理共用字体图集采样器）；
- 不做远程/流式 UI 资源；
- 不改变现有 `.ui` schema 版本号（新增字段是可选的，保持 v1）。

## 3. 文档格式

### 3.1 引用结构

节点新增两个**可选**对象；旧字段继续兼容：

```json
{
  "nodeGuid": "...",
  "type": "Image",
  "texture": {
    "assetId": "0f3f794e-7578-4bcb-92cb-b3afb43ee861",
    "subAssetKey": "",
    "path": "Assets/UI/Textures/HealthBar.png"
  },
  "font": {
    "assetId": "861b35f8-3170-4d0b-9fdb-111b8e089a3d",
    "subAssetKey": "",
    "path": "Assets/Fonts/Inter-Regular.ttf"
  },
  "fontPixelSize": 20,
  "nineSlice": { "left": 8, "top": 8, "right": 8, "bottom": 8 }
}
```

- `assetId` 是身份；`subAssetKey` 选择子资源（单纹理文件通常为空）；
- `path` 只用于显示与丢失恢复，**不作为解析身份**；
- `textureId` / `fontAssetId` 保留为**旧文档的运行时 id 兼容字段**。新文档不再写入；
  加载优先级：存在引用 → 解析引用；没有引用 → 使用旧的原始 id。

### 3.2 运行时定义

`Runtime/Engine/UI/UIResourceReference.h`：

```cpp
struct UIResourceReference
{
    Core::Assets::AssetId assetId;
    std::string subAssetKey;
    std::string resourcePath;
    bool IsNull() const;
};
```

`UIDocument::NodeDefinition` 增加：

```cpp
std::optional<UIResourceReference> texture;
std::optional<UIResourceReference> font;
UIInsets nineSlice;          // P2b，默认 0；UIInsets 定义在 UICommon.h
```

`UIElement` 增加：

```cpp
void SetTexture(uint64_t id, uint32_t generation, uint32_t width = 0, uint32_t height = 0);
void SetNineSlice(const UIInsets& insets);
```

宽度/高度供九宫格 UV 计算；旧调用方传 0 时退化为单块拉伸。

### 3.3 设计器属性路径

`UIDocumentDesignerModel` 的路径 API 从“一级 section + key”扩展为**任意点分路径**（`texture.assetId`、
`font.path`、`nineSlice.left`），写入时自动创建中间对象。新增合法路径：

| 路径 | 类型 | 用途 |
| --- | --- | --- |
| `texture.assetId` / `texture.subAssetKey` / `texture.path` | string | 纹理引用 |
| `font.assetId` / `font.subAssetKey` / `font.path` | string | 字体引用 |
| `nineSlice.left/top/right/bottom` | non-negative float | 九宫格边框 |
| 旧 `textureId` / `fontAssetId` | uint64 | 只读兼容/调试 |

删除引用时删除整个 `texture`/`font` 对象（或把三个字段置空），由模型提供 `ClearNodeResource(guid, kind)`。

## 4. 运行时解析

### 4.1 接口

`Runtime/Engine/UI/UIDocumentResourceResolver.h`：

```cpp
struct UIResolvedTexture
{
    uint64_t id = 0u;
    uint32_t generation = 0u;
};

class IUIDocumentResourceResolver
{
public:
    virtual ~IUIDocumentResourceResolver() = default;

    // 返回 nullopt 表示仍在加载或失败；调用方应保持 textureId=0 并稍后重试。
    virtual std::optional<UIResolvedTexture> ResolveTexture(
        const UIResourceReference& reference) = 0;

    // 返回稳定的进程内字体 id（0/1 保留给默认字体）。
    virtual std::optional<uint64_t> ResolveFont(
        const UIResourceReference& reference) = 0;

    // 每帧推进异步加载；就绪的资源由实例在 RefreshResources 中取用。
    virtual void Pump() = 0;

    // 资源重新导入/失效通知；实例据此刷新元素。
    virtual void Invalidate(const UIResourceReference& reference) = 0;
};

void SetUIDocumentResourceResolver(std::shared_ptr<IUIDocumentResourceResolver>);
[[nodiscard]] std::shared_ptr<IUIDocumentResourceResolver> GetUIDocumentResourceResolver();
```

- `UIDocumentInstance` 在 `BuildElementTree` 时解析引用；`RefreshResources()` 每帧检查 id 变化并调用
  `UIElement::SetTexture` / `SetFont`，只标记 paint dirty，不重建树；
- 解析器缺失（无宿主，如单元测试）→ 引用保持未解析，旧 `textureId` 仍生效。

### 4.2 游戏/编辑器共用的解析实现

两侧都已提供 `RuntimeAssetDatabase`、`TextureManager` 与可用的 driver，因此把实现放在
`Runtime/Engine/UI/UIDocumentResourceResolver.cpp`（宿主服务），编辑器只额外负责重导入失效通知。

```mermaid
flowchart LR
    R[UIResourceReference] --> DB[RuntimeAssetDatabase.Resolve]
    DB -->|artifactPath| TM[TextureManager.RequestAsyncArtifact]
    TM -->|Texture2D| VIEW[GetOrCreateExplicitTextureView]
    VIEW --> REG[DriverUIAccess.RegisterUiTextureView]
    REG --> ID[UIResolvedTexture{id,generation}]
    DB -->|font artifact bytes| FA[UIFontAtlas face registry]
    FA --> FID[fontAssetId]
```

- 纹理：
  1. `RuntimeAssetDatabase::Resolve(assetId, subAssetKey)` → `RuntimeAssetManifestEntry.artifactPath`；
  2. `TextureManager::TryRequestAsyncArtifact(artifactPath)`（失败/待加载时返回 nullopt，缓存状态）；
  3. 就绪后 `Texture::GetOrCreateExplicitTextureView()`；
  4. `DriverUIAccess::RegisterUiTextureView(driver, view, PreviousFrameOrStatic)` → `UiTextureId`；
     用 `id.value` / `id.generation` 填入 `UIResolvedTexture`；
  5. 缓存 `reference → {Texture2D*, UiTextureId, state}`，引用计数/会话缓存；卸载时
     `DriverUIAccess::ReleaseUiTextureView`。
- 字体：
  1. 解析字体资产（推荐 Font artifact，见 §5）；读取 TTF 字节；
  2. 分配进程内 `fontId`（从 2 开始，1 保留默认字体），登记到 `UIFontAtlas` 的 face provider；
  3. `ResolveFont` 返回该 id；元素 `fontAssetId` 使用它。
- 异步：`Pump()` 由 `UIRuntimeService::Tick`（游戏）和 `EditorUiOverlayPresenter`/设计器 update（编辑器）
  每帧调用；`ResolveTexture` 在未就绪时只做状态查询，不做磁盘 IO。
- 失效：编辑器资产重新导入后调用 `Invalidate(reference)`；解析器重新请求资源、递增 generation；
  `UIDocumentInstance::RefreshResources` 更新元素。游戏运行时按需支持（P2 先做编辑器侧）。
- 缺失：保持 `textureId=0`；设计器 Problems 给出 warning；运行时每个引用只 warning 一次。
- 采样：当前 `RHIImGuiOverlayRenderer` 对所有 UI 纹理使用字体图集采样器（线性 + clamp），
  本阶段接受该限制；每纹理采样器列入后续。

### 4.3 实例刷新

```cpp
// UIDocumentInstance
void RefreshResources();   // 解析器 Pump 后调用；只更新变化的 texture/font
void Tick(float dt);       // 现有接口，内部调用 RefreshResources（或由宿主显式调用）
```

- 元素纹理变化时 `SetTexture` 已标记 paint dirty；
- 字体变化时 `SetFont` 标记 layout/paint dirty（文本度量可能变化）；
- 解析失败不改变已有 id，避免闪烁。

## 5. 字体管线

### 5.1 多字体面图集

当前 `UIFontAtlas` 只有一份 `m_fontData`/`m_fontState`，但字形键已包含 `fontAssetId`。扩展为：

```cpp
struct FontFace
{
    std::vector<uint8_t> bytes;
    std::unique_ptr<FontRasterizerState> rasterizer;
    uint64_t revision = 1u;
};
std::unordered_map<uint64_t, FontFace> m_faces;   // 1 = 默认字体
```

- `SetFontData` 仍写入默认面（id 1）；
- 新增 `bool EnsureFace(uint64_t fontId)`，通过宿主安装的 provider 取字节；
- provider：
  ```cpp
  using UIFontFaceProvider = std::function<bool(uint64_t fontId, std::vector<uint8_t>& bytes)>;
  void SetUIFontFaceProvider(UIFontFaceProvider provider);
  ```
  由资源解析器安装（按 fontId 找到引用并读取 TTF）；
- `EnsureGlyph` 使用 `key.fontAssetId` 对应面；缺失时回退默认面（可见 tofu + 诊断），
  不阻塞其他文本；
- `InvalidateFont(fontId)` 删除面与该面的字形，重新请求 provider；
- 页分配/淘汰保持现有逻辑，跨面共享页；字形键已按字体 id 分离。

### 5.2 字体资产打包

推荐 **Font artifact**（与纹理一致）：

- `.ttf` / `.otf` 导入器写 `ArtifactType::Font`、`loaderId="font"`、schema v1，
  payload = 原始字体字节（可选 face index）；
- `RuntimeAssetDatabase` 解析后得到 artifactPath，读取字节交给 provider；
- 构建依赖与纹理走同一条 manifest 路径；
- 备选方案是 raw asset（打包文档已列 `.ttf`），但需要额外的 raw-asset manifest 与解析路径，
  不作为首选。

### 5.3 默认字体

- `fontAssetId` 缺失或引用为空 → 1（现有默认）；
- 默认字体仍由 `SetSharedUIFontAtlasFontPath` / `UIFontAtlasUploader` 预热；
- 项目字体的 face 首次使用时按需加载；设计器与 Game View 共用进程级图集。

## 6. 编辑器侧

### 6.1 引用类型与选择器

新增两个外部反射类型（`Runtime/Engine/UI/UIResourceReference.h` 或编辑器侧）：

```cpp
struct UITextureReference { Core::Assets::AssetId assetId; std::string subAssetKey; std::string resourcePath; };
struct UIFontReference    { Core::Assets::AssetId assetId; std::string subAssetKey; std::string resourcePath; };
```

在 `ObjectReferenceSupport` 中注册：

- `IsSupportedInspectorObjectReferenceType` → true；
- `IsCompatibleInspectorReferenceArtifact`：`UITextureReference → Texture`，`UIFontReference → Font`；
- display / clear / assign（拖放 payload）/ ping 四个分支复用现有 `UIDocumentReference` 的实现方式。

这样 Inspector 直接使用共享的对象引用抽屉，无需新控件；选择器条目按 artifact 类型过滤。

### 6.2 Inspector 分节

`UIDocumentContentEdit` 增加：

```cpp
UITextureReference texture;
UIFontReference font;
Maths::Vector4 nineSlice;   // 或 4 个 float 字段；仅 NineSlice 显示
```

按元素类型隐藏无关字段：

- `Image` / `NineSlice`：显示 Texture；
- `Text` / `RichText` / `Button` / `TextField` / `Dropdown`：显示 Font + Pixel Size；
- `NineSlice`：显示四个边框；
- 旧 `textureId` / `fontAssetId` 放入 Advanced，只读并提示“旧文档运行时 id”。

Canvas 与 Inspector 都接受资产浏览器拖放（`EditorAssetDragPayload`）：拖到 Image 上设纹理，
拖到文本元素上设字体；拖到画布空白处仍创建节点。

### 6.3 设计器预览

- 编辑器的资源解析器与 Game View 共用：`RuntimeAssetDatabase`（编辑器从 ArtifactDB 刷新）+
  `TextureManager` + `DriverUIAccess` 注册；设计器画布的 `UIDrawSnapshot` 直接拿到 `textureId`；
- 字体通过共享 `UIFontAtlas` + provider，字形就绪后画布自动重绘；
- 缺失/加载中：Image 显示棋盘格占位 + 名称；Problems 给出 warning；
- 重新导入纹理/字体：`Invalidate` → 新 generation → 画布刷新。

## 7. 导入与打包

`.ui` 导入路径（`AssetDatabaseFacade.cpp` 的 UI artifact 分支）需要：

1. 解析 JSON 后递归读取 `texture` / `font` 引用；
2. 用编辑器资产数据库按 GUID 查找源资产，校验类型（Texture / Font）；
3. 为每个引用追加 `AssetDependencyRecord{SourceAssetGuid, guid, {}}`，传给
   `ImportSingleNativeArtifact(..., additionalDependencies)`；
4. 缺失引用 → warning（不阻断），类型错误 → error；
5. `.ui` 的 artifact manifest 记录依赖；`RuntimeManifestBuilder` 随依赖带入纹理/Font artifact；
6. 打包验收：删除源文件后运行游戏，纹理/字体仍可加载。

字体若采用 Font artifact，导入器需写 artifact；若继续 raw，需在运行时 manifest 中增加 raw 条目
（不推荐，增加第二条解析路径）。

## 8. 九宫格（P2.5，已实现）

- schema：`"nineSlice": {"left","top","right","bottom"}`，单位文档像素；负值解析时 clamp 到 0；
- `UIInsets` 定义在 `UICommon.h`；`NodeDefinition.nineSlice` → `UIElement::SetNineSlice`；
- 纹理尺寸由解析器通过 `UIElement::SetTexture(id, generation, width, height)` 提供；
- `UIElement::BuildDrawSnapshotInternal` 对 `NineSlice` + 非零边框 + 已知纹理尺寸生成 9 个
  `AddTexturedQuad`（每个 quad 一个 batch）：角块固定尺寸、边块单轴拉伸、中心双轴拉伸；
- 目标边框 clamp 到元素宽/高的一半（重叠时按比例缩小）；**源 UV 仍按作者边框裁剪**
  （如 8px 边框 / 32px 纹理 = 0.25），而不是按目标 clamp 拉伸；
- 无纹理、纹理尺寸为 0 或边框全 0 → 退化为单块拉伸（保持旧行为）；
- Inspector：Content 分节在 `NineSlice` 类型下显示 Texture + 四个边框；其他类型隐藏；
- 测试：9 quad UV、目标 clamp（10×10 + 8px 边框 → 4 个角块）、无尺寸回退单块。

## 9. 阶段与验收

| Gate | 内容 | 验收 |
| --- | --- | --- |
| **P2.0** | 本设计评审 | schema/字体打包/九宫格范围确认 |
| **P2.1 格式与依赖** | ✅ `UIResourceReference`、解析/序列化、旧字段兼容、点分属性路径、`.ui` 依赖扫描、Font artifact 导入 | 单元：新/旧文档解析、依赖记录、缺失/类型错误；导入后 manifest 含依赖 |
| **P2.2 字体面** | ✅ `UIFontAtlas` 多面 + provider、默认字体不变、`InvalidateFont` | 单元：两面字形互不串、缺失回退默认、重建后重栅格 |
| **P2.3 运行时解析** | ✅ `IUIDocumentResourceResolver`、`RuntimeUIDocumentResourceResolver`（游戏/编辑器共用）、`RefreshResources`、Pump、纹理注册/释放 | 游戏：纹理异步就绪后出现；引用 id 稳定；退出释放；无每帧磁盘 IO |
| **P2.4 编辑器 UX** | ✅ 引用类型选择器/拖放、Inspector 分节、设计器刷新、重新导入失效 | 拖放纹理/字体 → 保存 → 重载 → 设计器与 Game View 一致（设备验收待做） |
| **P2.5 九宫格** | ✅ schema、运行时 9 quad、Inspector、预览 | 九宫格截图/UV 测试、边框 clamp |
| **P2.6 打包** | ◐ 依赖改为 `ImportedArtifact`、字体读取走 `ContentSourceTable`；cook + 删除源文件验收待做 | 打包游戏加载纹理/字体；日志无缺失 |

## 10. 测试计划

- **单元**：reference JSON 新/旧解析；`GetStable…`；依赖扫描（缺失、重复、类型错误）；点分路径读写；
  resolver 状态机（Pending/Ready/Failed、缓存命中、Invalidate）；字体面隔离/默认回退；
  九宫格 UV/quad。
- **编辑器/设备**：选择器过滤（纹理列表不出现字体）；拖放赋值；保存/重载；外部重新导入；
  设计器画布纹理/字体出现；Game View 对比。
- **打包**：cook 后删除 `Assets` 源文件，运行游戏验证；manifest 依赖完整。
- **性能**：就绪后每帧零资产查找；pending 时每帧至多一次状态查询；纹理只上传一次。

## 11. 风险与开放问题

| 风险/问题 | 处理 |
| --- | --- |
| 字体面栅格器内存/页淘汰跨面 | 页共享、字形键已分离；限制最大面数/页数，先测 4 个面 |
| 纹理注册依赖驱动/渲染线程 | 统一由 `TextureManager` 异步加载 + `DriverUIAccess` 注册；解析器只在主线程查询状态 |
| 所有 UI 纹理共用字体采样器 | 本阶段接受；每纹理采样器后续扩展 `UiTextureId`/registry |
| 字体 raw vs artifact | 推荐 Font artifact；若选 raw 需补 raw manifest |
| 引用字段嵌套 vs 平铺 | 推荐嵌套 `texture`/`font` + 点分路径；平铺字段更简单但 JSON 可读性差 |
| 运行时脚本 API 兼容 | `NodeDefinition` 新增可选字段；旧字段保留；不改脚本可见的 Canvas API |
| 设计期热重载 | P2.4 先做编辑器 Invalidate；运行时热重载列入后续 |

## 12. 文件与类布局

| 文件 | 状态 | 职责 |
| --- | --- | --- |
| `Runtime/Engine/UI/UIResourceReference.h` | 新增 | `UITextureReference` / `UIFontReference`（AssetId + subAssetKey + path） |
| `Runtime/Engine/Serialize/ExternalReflection.h` | 扩展 | 两种引用类型的字段反射注册 |
| `Runtime/Engine/UI/UIDocumentResourceResolver.{h,cpp}` | 新增 | 解析接口 + 基于 RuntimeAssetDatabase/TextureManager/DriverUIAccess 的共享实现 |
| `Runtime/Engine/Assets/RuntimeAssetDatabase.{h,cpp}` | 扩展 | `ResolvePrimary`（空 subAssetKey 的引用回退） |
| `Runtime/Engine/UI/UICommon.h` | 扩展 | `UIInsets`（九宫格边框） |
| `Runtime/Engine/UI/UIDocument.{h,cpp}` | 扩展 | 解析 `texture`/`font`/`nineSlice`，保留旧 id |
| `Runtime/Engine/UI/UIElement.{h,cpp}` | 扩展 | 纹理尺寸、九宫格绘制 |
| `Runtime/Engine/UI/UIDocument.cpp`（实例） | 扩展 | `RefreshResources` |
| `Runtime/Rendering/UI/UIFontAtlas.{h,cpp}` | 扩展 | 多字体面 + provider |
| `Runtime/Engine/Components/UICanvasComponent.cpp` | 扩展 | 每帧刷新资源 |
| `Runtime/UI/UIRuntimeService.cpp` | 扩展 | Pump 解析器 |
| `Project/Editor/UI/EditorUiOverlayPresenter.cpp` | 扩展 | 编辑器侧 Pump/Invalidate |
| `Project/Editor/Assets/AssetDatabaseFacade.cpp` | 扩展 | `.ui` 依赖扫描、Font 导入 |
| `Project/Editor/Inspector/ObjectReferenceSupport.cpp` | 扩展 | 纹理/字体引用抽屉 |
| `Project/Editor/UI/UIDocumentNodeEdit.*` | 扩展 | 引用字段（纹理/字体）；九宫格字段 P2.5 |
| `Project/Editor/UI/UIDocumentInspectorView.*` | 扩展 | Content 分节显示纹理/字体、旧 id Advanced、引用拖放 |
| `Project/Editor/UI/UIDocumentCanvasRenderer.cpp` | 扩展 | 设计器预览每帧 `RefreshResources()` |
| `Project/Editor/UI/UIDocumentDesignerModel.cpp` | 扩展 | 点分路径、清除引用 |
| `Tests/Unit/UIDocumentResourceReferenceTests.cpp` | 新增 | 格式/依赖/解析器/字体/九宫格 |

## 13. 已确认的决策

| 决策 | 结论 |
| --- | --- |
| 字体打包 | **Font artifact**（payload = TTF 字节，`loaderId="font"`，schema v1） |
| 引用字段 | 嵌套 `texture` / `font` 对象 + 点分属性路径 |
| 九宫格 | 随 P2 作为 P2.5 独立 gate |
| 旧运行时 id | 新文档不再写 `textureId`/`fontAssetId`；读取兼容，设计器 Advanced 只读 |

## 14. P2.1 / P2.2 实现记录（2026-10-20）

### P2.1

已完成：

- `Runtime/Engine/UI/UIResourceReference.h` 新增 `UITextureReference` / `UIFontReference`；
  字段在 `Runtime/Engine/Serialize/ExternalReflection.h` 注册（assetId/subAssetKey/resourcePath）。
- `UIDocument::NodeDefinition` 增加 `std::optional<UITextureReference> texture`、
  `std::optional<UIFontReference> font`；`ParseNode` 读取嵌套引用并保留旧 `textureId`/`fontAssetId`。
- `fontPixelSize` 改为独立于 `fontAssetId` 解析（修复“只有 font 引用时字号被忽略”的缺陷）。
- 新增 `CollectUIResourceReferences(document, textures, fonts)` 递归收集器。
- `UIDocumentDesignerModel` 支持任意点分路径；新增
  `texture.assetId` / `texture.subAssetKey` / `texture.path` 与
  `font.assetId` / `font.subAssetKey` / `font.path` 白名单。
- `.ui` 导入：解析后按 GUID 校验纹理/字体引用，写入 `SourceAssetGuid` 依赖；
  缺失/类型错误给出 warning；依赖传给 `ImportSingleNativeArtifact`。
- `.ttf` / `.otf` 导入分支写 Font artifact（`font:<stem>`）。
- 测试：`UIDocumentResourceReferenceTests` 5 个（引用解析/收集、旧 id 兼容、错误引用、
  反射字段、点分路径 + 撤销）。

### P2.2

已完成：

- `UIFontAtlas` 从单一 `m_fontData`/`m_fontState` 改为 `fontAssetId → FontFace`（bytes + 独立
  rasterizer）；默认面固定为 id 1。
- 新增 `SetFaceData` / `EnsureFace` / `HasFace` / `GetFaceCount` / `SetMaxFaces` /
  `ClearFaceFailures`；宿主通过 `SetUIFontFaceProvider` 按需提供 TTF 字节。
- provider 失败结果缓存，避免每个字形重复访问磁盘；`InvalidateFont` / `Clear` /
  `ClearFaceFailures` 可清除失败缓存并重新加载。
- 缺失项目字体时回退默认面（字形仍可读），新增诊断 `fallbackFaceGlyphs`；无默认面时保持 tofu。
  `fontAvailable=false` 仍强制 tofu，保持既有测试语义。
- 面数上限默认 16（`SetMaxFaces` 下限 2），超出时按 `lastUsedFrame` 淘汰非默认面。
- 字体数据入口增加 TrueType/OpenType 魔数校验。仓库中 `App/Assets/Editor/Fonts/**/*.ttf` 有若干
  **文本字体目录描述符**（`type=Font...`）；旧实现会被 `stbtt_InitFont` 误判并越界，现在直接拒绝。
- 测试：`UIFontAtlasFaceTests` 7 个（非字体载荷拒绝、provider 加载/缓存、缺失回退与失败缓存、
  失效重载、面淘汰、无字体/tofu、显式 SetFaceData 覆盖 provider）。

### P2.3

已完成：

- 新增 `IUIDocumentResourceResolver`（`ResolveTexture` / `ResolveFont` / `Pump` / `Invalidate`）
  与 `Set/Get/Clear/Ensure` 安装点；未安装时 `Ensure` 懒创建运行时解析器。
- `RuntimeUIDocumentResourceResolver`：
  - `RuntimeAssetDatabase::Resolve` → `ResolvePrimary` 回退（引用未记录 subAssetKey 时）；
  - `TextureManager::RequestAsyncArtifact` + `TryGetArtifactResource` 异步上传，成功后
    `GetOrCreateExplicitTextureView` → `DriverUIAccess::RegisterUiTextureView`；
  - 纹理 generation/尺寸缓存；失败与长期未就绪路径有上限，避免每帧重试；
  - 字体 artifact 读取（`NativeArtifactContainer` payload）→ `UIFontAtlas::SetFaceData`；
    `TextureManager::ResolveResourcePath` 解析相对路径；
  - `Invalidate` 释放纹理视图/失效字体面，供重新导入使用。
- `UIDocumentInstance` 增加资源槽：`BuildElementTree` 收集 `texture`/`font` 引用，
  `RefreshResources` 只更新变化的 id/generation，不重建元素树；`Tick` 与手动更新策略均会刷新。
- `UICanvasComponent::Tick` 在 `Manual`/`OnDemand` 策略下也调用 `RefreshResources`。
- `Editor`/`Game` 的 `Context` 在驱动与资源服务关闭前调用 `ClearUIDocumentResourceResolver()`，
  避免解析器缓存的 RHI 纹理视图生命周期超过驱动。
- 测试：`UIDocumentResourceResolverTests` 8 个（实例解析、异步 pending → ready、generation 更新、
  旧 id 不被覆盖、Reload 重建槽、`ResolvePrimary` 两种回退、无服务安全）。

### P2.4

已完成：

- `ObjectReferenceSupport` 识别 `UITextureReference` / `UIFontReference`：
  共享引用抽屉的 picker、资产拖放、Clear、Ping、显示名与 artifact 过滤（Texture/Font）。
- Font artifact 纳入对象引用 picker 的 `IsInspectorReferenceableSubAsset`；Asset Browser 的字体源
  拖放映射为 `font:<stem>` + `ArtifactType::Font`，与 Font 导入器的 subAssetKey 对齐。
- `UIDocumentContentEdit` 增加 `texture` / `font` 字段；`ReadContentEdit` / `ApplyContentEdit`
  映射到 `texture.assetId|subAssetKey|path` 与 `font.assetId|subAssetKey|path`，并支持整引用清空。
- Inspector Content 分节按元素类型显示：Image/NineSlice 显示 Texture；文本类显示 Font + Pixel Size；
  旧 `fontAssetId` 移入 **Advanced (Legacy Runtime IDs)** 只读区域。
- 验证了嵌套引用在 `InspectorProjectionBuilder` 中投影为 `ObjectReference`（而不是可展开结构），
  因此复用共享 picker/drop 抽屉，而不是新增一套控件。
- `UIDocumentCanvasRenderer::Render` 每帧调用 `RefreshResources()`，设计器画布与 Game View
  共用同一解析器；异步纹理/项目字体加载后无需重建树即可显示。
- 纹理/字体导入成功后调用 `IUIDocumentResourceResolver::InvalidateAsset(assetId)`，
  释放旧纹理视图并失效旧字体面，下一次 `RefreshResources` 重新加载。
- 测试：`UIDocumentResourceReferenceTests` 新增引用抽屉契约与投影用例；`UIDocumentDesignerEditTests`
  新增 Content 引用读写/清空/撤销用例。

### P2.5

已完成：

- `UICommon.h` 新增 `UIInsets`；`NodeDefinition.nineSlice` 解析
  `nineSlice.left|top|right|bottom`（负值 clamp 到 0）。
- `UIElement` 增加九宫格边框与纹理尺寸；`SetTexture` 扩展为
  `(id, generation, width, height)`，解析器刷新时传入 `UIResolvedTexture` 尺寸。
- `BuildDrawSnapshotInternal` 对 `NineSlice` 生成 9 个带 UV 的 quad：
  - 目标边框 clamp 到元素宽/高一半，源 UV 按作者边框裁剪；
  - 边框全 0 / 无纹理 / 纹理尺寸 0 → 单块拉伸；
  - 目标 clamp 到 10×10 + 8px 时只剩 4 个角块。
- Inspector `UIDocumentContentEdit` 增加 `UIDocumentNineSliceEdit`，仅 `NineSlice` 显示；
  模型支持 `nineSlice.*` 点分属性与撤销。
- 测试：`UIDocumentResourceResolverTests.NineSlice*` 3 个（9 quad UV、目标 clamp、无尺寸回退）。

### P2.6（部分）

已完成：

- `.ui` 导入依赖从仅 `SourceAssetGuid` 扩展为 **`ImportedArtifact`**（精确 subAssetKey）+
  `SourceAssetGuid`（目标 artifact 尚未导入时仍记录源依赖，便于重导入）。
  runtime manifest builder 只跟随 `ImportedArtifact` 边，这是纹理/字体进包的关键修复。
- Font artifact 读取先走 `ContentSourceTable::ReadAll`（支持 NPAK/目录内容源），
  再回退到普通文件读取。
- 测试：
  - `RuntimeManifestFollowsUiTextureAndFontDependencies` 验证 UI → Texture/Font
    的 `ImportedArtifact` 依赖进入 runtime manifest；
  - `RuntimeResolverLoadsFontArtifactFromContentSource` 用临时 Font artifact +
    `DirectoryContentSource` 走完整 `ResolveFont` 路径（解析、读取、SetFaceData、Invalidate 重载）；
  - 全套 81/81 通过。

待完成（需要并行构建恢复后）：

- 真实 cook + 删除源文件运行：确认 Font artifact 被导入计划触发、`.ui` 依赖进包；
- NPAK 模式的字体 artifact 实测；
- 若 `.ui` 在字体/纹理导入前先导入，需要一次重导入补齐 `ImportedArtifact` 边
  （`SourceAssetGuid` 依赖已保留，供资产数据库失效/重导入）。
- 真实编辑器交互验收（保存/重载、设计器与 Game View 对比、字体 picker 条目）。
