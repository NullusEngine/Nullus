# Static Mesh LOD System 实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 `superpowers:subagent-driven-development` 或 `superpowers:executing-plans` 逐任务实现此计划。步骤使用复选框（`- [ ]`）跟踪进度。

**目标：** 完成 StaticMesh 的 SourceModel、自动减面、LODGroup、Cook、运行时选择、生命周期和正式 LOD 缩略图闭环。

**架构：** `StaticMeshSourceAsset` 保存源模型与构建设置；统一 `StaticMeshBuilder` 在编辑器和 Cook 中生成多级 `MeshArtifact`。运行时只消费派生资源，通过 projection-aware `SceneLODSystem` 选择 `PrimitiveLODSet`。缩略图复用正式 LOD 选择器，不生成预览简化网格。

**技术栈：** C++20、CMake/MSVC、GoogleTest、ArtifactManifest、meshoptimizer、Nullus DX12。

---

## 文件结构

- 创建 `Runtime/Rendering/Assets/StaticMeshLODSettings.h/.cpp`：SourceModel、LODGroup、校验和指纹。
- 创建 `Runtime/Rendering/Assets/MeshReduction.h/.cpp`：减面接口和 meshoptimizer 实现。
- 创建 `Runtime/Rendering/Assets/StaticMeshBuilder.h/.cpp`：统一编辑器/Cook 构建器。
- 创建 `Tests/Unit/StaticMeshLODTests.cpp`：资产、预设、减面、构建和兼容测试。
- 修改 `Runtime/Rendering/Assets/MeshArtifact.h/.cpp`：多 LOD artifact 和旧格式兼容。
- 修改 `Project/Editor/Assets/ExternalAssetImporter.cpp`、`Runtime/Engine/Assets/ModelPrefabBuilder.cpp`：导入、重导入和依赖。
- 修改 `Runtime/Engine/Rendering/SceneLOD.*`、`RenderScene.*`、`SceneVisibilityPipeline.cpp`、`BaseSceneRenderer.cpp`：运行时选择和生命周期。
- 修改 Mesh components：生产路径注册、更新和注销 `PrimitiveLODSet`。
- 修改 `Project/Editor/Assets/AssetThumbnail*` 和 `EditorThumbnailPreviewRenderer.cpp`：删除简化，选择正式 LOD。
- 扩展 `SceneLODTests.cpp`、`SceneVisibilityPipelineTests.cpp`、`AssetThumbnailBehaviorTests.cpp`。

## 任务 1：多 LOD 数据契约

**文件：** `MeshArtifact.*`、`StaticMeshLODSettings.*`、`StaticMeshLODTests.cpp`。

- [ ] **步骤 1：编写失败测试**：默认 None 只有 LOD0；七个 UE4.26 预设值；LOD0 必须有效；ScreenSize 严格递减；旧 artifact 读取为单级；build identity 稳定。
- [ ] **步骤 2：运行失败测试**：`& .\build\bin\Debug\NullusUnitTests.exe --gtest_filter="StaticMeshLODTests.*"`，预期因新类型不存在而失败。
- [ ] **步骤 3：最小实现**：定义 `StaticMeshSourceModel`、`StaticMeshSourceAsset`、`StaticMeshLODGroupPreset`、`MeshLODResource` 和 `MeshArtifact::lodResources`；增加 schema、provenance、override 和校验。
- [ ] **步骤 4：运行同一测试**，预期全部通过。
- [ ] **步骤 5：Commit**：`git commit -m "feat: add static mesh LOD data contracts"`。

## 任务 2：LODGroup 和自动减面

**文件：** `StaticMeshLODSettings.*`、`MeshReduction.*`、`StaticMeshBuilder.*`、`StaticMeshLODTests.cpp`、Rendering CMakeLists。

- [ ] **步骤 1：编写失败测试**：累计比例 50/25/12.5、Section/材质槽和顶点通道保持、退化面清理、best-effort warning、非法输入 error、输出和 hash 确定性。
- [ ] **步骤 2：运行失败测试**：`--gtest_filter="StaticMeshLODTests.Reduction*:*LODGroup*"`。
- [ ] **步骤 3：实现 `MeshReductionInterface`**：按 Section 调用 meshoptimizer，保留位置/法线/切线/UV/顶点色，清理并优化索引和顶点。
- [ ] **步骤 4：实现 Registry/Builder**：写入 None、LevelArchitecture、SmallProp、LargeProp、Deco、Vista、Foliage、HighDetail；生成级直接以 LOD0 为基准；保留 authored/override。
- [ ] **步骤 5：运行定向测试**，预期通过。
- [ ] **步骤 6：Commit**：`git commit -m "feat: add deterministic static mesh LOD reduction"`。

## 任务 3：导入、重导入和 Cook

**文件：** `ExternalAssetImporter.*`、`ModelPrefabBuilder.cpp`、`ArtifactManifest.*`、`StaticMeshBuilder.*`、资产导入测试。

- [ ] **步骤 1：编写失败测试**：默认仅 LOD0；开启后识别 `_LOD1`；重导入保留 preset/override/authored LOD；材质槽稳定；源变化使生成级失效；Cook cache hit/miss 和平台隔离。
- [ ] **步骤 2：运行失败测试**：`--gtest_filter="AssetImportPipelineTests.StaticMeshLOD*:*StaticMeshCook*"`。
- [ ] **步骤 3：实现 importer source model 生成**：Importer 只读取正式源数据和保存设置，默认 `lodGroup=None`，不包含第二套减面算法。
- [ ] **步骤 4：实现统一 Builder/指纹**：身份包含 source hash、normalized settings、LODGroup、importer/postprocessor、reducer、schema、target platform；失败保留旧 artifact，成功原子发布。
- [ ] **步骤 5：接入旧 artifact 升级、Prefab resolved asset 和 Manifest dependencies**。
- [ ] **步骤 6：运行导入/Cook 测试**，预期通过。
- [ ] **步骤 7：Commit**：`git commit -m "feat: build static mesh LODs during import and cook"`。

## 任务 4：运行时选择和 RenderScene 生命周期

**文件：** `SceneLOD.*`、`RenderScene.*`、`SceneVisibilityPipeline.cpp`、`BaseSceneRenderer.cpp`、Mesh components、Scene LOD tests。

- [ ] **步骤 1：编写失败测试**：projection/FOV/viewport、ForcedLOD、MinLOD、质量限制、缺级、驻留回退、hysteresis、多 View、transition、注册/更新/注销/重复 Handle。
- [ ] **步骤 2：运行失败测试**：`--gtest_filter="SceneLODTests.*:SceneVisibilityPipelineTests.*"`。
- [ ] **步骤 3：实现屏幕尺寸和选择链**：bounds sphere + projection；按 forced、MinLOD、show flag、quality/platform、threshold、residency、hysteresis 顺序选择。
- [ ] **步骤 4：实现 history/transition**：使用稳定 primitive/view key；输入变化、View 销毁和 Primitive 移除清理历史；支持 transition mask/alpha，不支持的材质 hard switch。
- [ ] **步骤 5：接入生产组件**：创建 render state 注册集合，热重载/Bounds/设置/驻留变化原子更新，销毁/换 Mesh 注销；拒绝空、重复、重叠和非法 LOD0。
- [ ] **步骤 6：运行定向测试**，预期通过。
- [ ] **步骤 7：Commit**：`git commit -m "feat: complete runtime static mesh LOD selection"`。

## 任务 5：正式 LOD 缩略图

**文件：** `AssetThumbnail*`、`EditorThumbnailPreviewRenderer.cpp`、`AssetThumbnailBehaviorTests.cpp`。

- [ ] **步骤 1：编写失败测试**：缩略图使用正式 LOD selector；固定 View 选择稳定；LOD 配置使缓存失效；不存在预览减面；LOD0-only 正常。
- [ ] **步骤 2：运行失败测试**：`--gtest_filter="AssetThumbnailBehaviorTests.StaticMeshLOD*"`。
- [ ] **步骤 3：移除 `LoadMeshArtifactPreviewSample`、`SimplifyMeshArtifactForPreview` 及 scheduler/cache/fallback 分支和声明**。
- [ ] **步骤 4：缩略图 View 传入真实 viewport/projection，调用 `SceneLODSystem` 选择正式 `lodResources`；缺级走正常驻留回退**。
- [ ] **步骤 5：缓存键加入多 LOD build fingerprint、threshold、bounds、material、environment、viewport、renderer version**。
- [ ] **步骤 6：运行定向测试**，预期通过。
- [ ] **步骤 7：Commit**：`git commit -m "feat: select formal mesh LODs for thumbnails"`。

## 任务 6：集成和验证

- [ ] **步骤 1：构建**：`cmake --build build --config Debug --target NullusUnitTests -- /m:4`，预期成功。
- [ ] **步骤 2：LOD 套件**：运行 `StaticMeshLODTests.*:SceneLODTests.*:SceneVisibilityPipelineTests.*:AssetThumbnailBehaviorTests.StaticMeshLOD*`，预期全部通过。
- [ ] **步骤 3：全量测试**：运行 `NullusUnitTests.exe --gtest_brief=1`。基线已观察到 `AssetThumbnailBehaviorTests.GpuPrefabPreviewDetectsTerminalAsyncMeshFailureWithoutSynchronousPrewarm` 失败，必须单独记录并确认没有新增非基线失败。
- [ ] **步骤 4：端到端**：使用 LOD0-only 和 SmallProp 网格验证导入、保存/重开、场景选择、多 View、缩略图、Cook、Cook artifact 加载。
- [ ] **步骤 5：检查**：运行 `git diff --check`、`git status --short`、`git log --oneline -8`，确保无 build/artifact 和无关文件进入提交。
- [ ] **步骤 6：使用 `verification-before-completion` 完成最终证据验证，再使用 `finishing-a-development-branch` 收尾**。
