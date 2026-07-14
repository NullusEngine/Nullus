# PBR 阴影与材质预览 IBL 实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 superpowers:subagent-driven-development（推荐）或 superpowers:executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 按可独立验证的阶段交付 PBR 几何法线正确性、缓存式场景阴影和中性材质预览 IBL，解决漏光、硬明暗界限、高光白点与 prefab/model 缩略图体验问题。

**架构：** 先稳定 Forward/Deferred 的两法线与 GBuffer 数据契约，再添加独立 shadow metadata/pool/planner，最后接入只用于材质缩略图的共享 Studio IBL。三个计划通过对象 receiver bit、共享 PBR evaluator 和 ShaderLab variants 连接；每阶段有单独 commit 与验证门，失败不得被后续阶段掩盖。

**技术栈：** C++20、HLSL/ShaderLab、Nullus RenderScene/FrameGraph/RHI、TextureArtifact、MetaParser、GoogleTest、DX12。

---

## 计划索引与依赖

| 顺序 | 子计划 | 独立产出 | 前置依赖 |
|---:|---|---|---|
| 1 | `Docs/superpowers/plans/2026-07-13-pbr-shading-normal-correctness.md` | 两法线 PBR、Deferred oct geometry normal、receiver object ABI、decal alpha 保护 | 当前分支 |
| 2 | `Docs/superpowers/plans/2026-07-13-pbr-shadow-system.md` | 方向/点/聚光缓存阴影、三预算、last-good、fail closed | 阶段 1 对象 ABI 与共享 PBR API |
| 3 | `Docs/superpowers/plans/2026-07-13-material-preview-ibl.md` | 共享 Studio IBL artifacts、HDR cube 上传、fallback、v10 | 阶段 1 共享 PBR API；不依赖阶段 2 pool |

阶段 3 在代码层可以与阶段 2 并行，但默认串行执行，避免同时修改 `LightGridCommon.hlsli`、`StandardPBR.shader` 和 thumbnail behavior fixtures。任何并行执行都必须先拆成不重叠提交并在合并后重跑下面的交叉验证。

## 固定约束

- 不手工编辑 `Runtime/*/Gen/` 或 `Project/*/Gen/`；反射改动走正常 MetaParser 构建。
- prefab/model preview 继续 GPU-only、完整资源快照、完整模型 framing 和 async readback。
- shadow light record 保持 16 words；新增 buffers 不复用其空位。
- 默认 shadow budgets 固定为 12 views、2048 draws、2,000,000 triangles。
- 缺失/首次未发布/容量 suppressed shadow map fail closed，不退化成 unshadowed direct light。
- 不增加 Deferred GBuffer attachment；三个 alpha 分别承载 oct X、oct Y、receiver bit。
- 遵守用户要求，本轮验证不使用 RenderDoc；用 targeted runtime、DX12 GPU fixtures 和 Debug Layer。
- DX12 通过不代表 Vulkan/macOS 正确；SPIR-V 编译只作为 portability evidence。

### 任务 1：建立执行基线

**文件：**
- 读取：`Docs/superpowers/specs/2026-07-13-pbr-shadows-preview-ibl-design.md`
- 读取：本索引中的三份子计划

- [ ] **步骤 1：确认 worktree 与分支**

```powershell
git branch --show-current
git status --short
git log -1 --oneline
```

预期：分支为 `fix/pbr-shadows-preview-ibl`；开始实现前只有计划/规格文档提交，production tree 干净。

- [ ] **步骤 2：确认配置和基础目标**

```powershell
cmake -S . -B Build -DNLS_BUILD_TESTS=ON
cmake --build Build --config Debug --target NullusUnitTests -- /m:4
```

预期：`NullusUnitTests` 构建成功。Autodesk FBX SDK 未准备时允许使用 Assimp FBX，但必须保留警告记录。

- [ ] **步骤 3：记录已知 baseline 限制**

不要在实现前重跑已知超过 10 分钟的整套 `ctest -L behavior` 作为阻塞步骤。先使用每个任务给出的 gtest filter；阶段结束后再运行长测试并记录实际完成/超时，不能把超时写成通过。

### 任务 2：执行阶段 1 并冻结数据契约

**文件：**
- 执行：`Docs/superpowers/plans/2026-07-13-pbr-shading-normal-correctness.md`

- [ ] **步骤 1：逐任务执行阶段 1 的红灯、最小实现、绿灯和 commit**

每个生产改动之前必须能指出对应失败测试；不要把六个任务压成一个提交。

- [ ] **步骤 2：检查冻结 ABI**

实现后确认：

```text
ObjectDrawConstants = { uint objectIndex; uint objectFlags; uint padding0; uint padding1; }  // 16 bytes
GBuffer.Albedo.a   = geometry oct X
GBuffer.Normal.a   = geometry oct Y
GBuffer.Material.a = ReceiveShadows ? 1 : 0
```

- [ ] **步骤 3：运行阶段 1 门禁**

```powershell
.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=PBRShadingContractTests.*:RendererFrameObjectBindingTests.*:DeferredSceneRendererMaterialCacheTests.*:AssetMaterialViewportTests.*PBR*
```

预期：全部运行项 `PASS`；GPU skip 必须单独列为未验证证据。

- [ ] **步骤 4：检查阶段边界**

此时没有 shadow map 也没有 Studio IBL，但现有 direct lights 应保持可见；normal map 无法照亮几何背面；Deferred decal 不破坏 alpha 控制通道。

### 任务 3：执行阶段 2 并验证有界阴影

**文件：**
- 执行：`Docs/superpowers/plans/2026-07-13-pbr-shadow-system.md`

- [ ] **步骤 1：逐任务执行阶段 2**

优先完成 pure CPU planner、cache/invalidation 和 resource descriptors，再接入 FrameGraph/GPU。组件反射生成只允许由 MetaParser 产生。

- [ ] **步骤 2：运行 CPU 契约门禁**

```powershell
.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=ShadowPlannerTests.*:SceneShadowProviderTests.*:LightingDataProviderTests.*Shadow*:RenderSceneCacheTests.*Shadow*:FrameGraphSceneTargetsTests.*Shadow*:RendererStatsTests.*Shadow*
```

预期：`PASS`；边界测试证明 12/2048/2,000,000 不能超出，首次 point/directional group 不会 partial publish。

- [ ] **步骤 3：运行 shader 与 DX12 图像门禁**

```powershell
.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetMaterialConversionTests.*Shadow*:PBRShadingContractTests.*Shadow*:DX12ShadowRenderingTests.*
```

预期：方向/点/聚光、closed building、single/two-sided wall、AlphaTest fixture 达到子计划阈值；记录实际 backend/adapter。

- [ ] **步骤 4：运行性能门禁**

```powershell
.\Build\bin\Debug\NullusPerformanceTests.exe --gtest_filter=RenderScenePerformanceTests.*Shadow*
```

预期：达到子计划的 p95/p99 阈值；若未达到，回到 candidate/draw reuse 优化，不通过丢 caster 或发布 partial maps 掩盖。

### 任务 4：执行阶段 3 并验证材质预览

**文件：**
- 执行：`Docs/superpowers/plans/2026-07-13-material-preview-ibl.md`

- [ ] **步骤 1：逐任务执行阶段 3**

先完成 deterministic artifact builder 与 HDR cubemap upload，再修改 thumbnail renderer。不得在 editor frame 首次请求时同步做 1024-sample production bake。

- [ ] **步骤 2：运行 artifact/上传门禁**

```powershell
.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=StudioIblArtifactTests.*:AssetDatabaseFacadeTests.*StudioIbl*:DX12TextureUploadUtilsTests.*Cube*
```

预期：`PASS`；production descriptors 精确为 32 RGBA16F cube、128 RGBA16F full-mip cube、256 RG16F LUT。

- [ ] **步骤 3：运行缩略图行为门禁**

```powershell
.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetThumbnailCacheTests.*StudioIbl*:AssetThumbnailCacheTests.*GpuOnly*:AssetThumbnailBehaviorTests.*StudioIbl*:AssetBrowserPresentationTests.*StudioIbl*
```

预期：IBL ready/fallback/failed 都有终止状态；v10 映射正确；prefab/model 没有 CPU fallback、partial model cache 或 UI wait。

- [ ] **步骤 4：运行 thumbnail 性能门禁**

```powershell
.\Build\bin\Debug\NullusPerformanceTests.exe --gtest_filter=AssetThumbnailPerformanceTests.*StudioIbl*
```

预期：warmup 后共享资源不重复 IO/upload/prewarm，既有 GPU/readback pump budgets 仍生效。

### 任务 5：运行交叉回归和平台证据

**文件：**
- 验证：所有阶段修改文件

- [ ] **步骤 1：构建 Debug Editor、测试和反射工具**

```powershell
cmake --build Build --config Debug --target Editor NullusUnitTests ReflectionTest -- /m:4
.\Build\bin\Debug\ReflectionTest.exe
```

预期：构建和 ReflectionTest `PASS`，MetaParser 无 rejected member/parse error。

- [ ] **步骤 2：运行合并后的 focused behavior 集**

```powershell
.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=PBRShadingContractTests.*:ShadowPlannerTests.*:SceneShadowProviderTests.*:StudioIblArtifactTests.*:DX12ShadowRenderingTests.*:AssetThumbnailBehaviorTests.*StudioIbl*
```

预期：所有可运行测试 `PASS`。

- [ ] **步骤 3：运行整套 behavior 并如实记录**

```powershell
ctest --test-dir Build -C Debug --output-on-failure -L behavior
```

预期：完成时 `100% tests passed`。若再次超时，终止残留测试进程并在交付中写明“focused tests passed，full behavior timed out”；不得宣称全套通过。

- [ ] **步骤 4：验证 DX12 Debug Layer**

以 DX12 Debug Layer 运行 shadow 与 thumbnail GPU fixtures，记录 adapter、driver 和 diagnostics。确认没有 resource-state、descriptor heap、invalid depth、device removal 或同步 wait 警告。

- [ ] **步骤 5：验证 shader portability**

运行所有受影响 shader 的 DXIL/SPIR-V permutation tests；对输出执行 `spirv-val`。这证明编译/验证，不替代 Vulkan runtime 测试。

### 任务 6：最终自审与提交整合

**文件：**
- 检查：`git diff --check`
- 检查：`git status --short`

- [ ] **步骤 1：按规格逐条映射**

逐条检查设计规格的 Component defaults、16-word ABI、CSM/spot/point、budgets、cache、last-good、AlphaTest/Cull Off、两法线、GBuffer/decal、IBL artifacts/fallback/v10、telemetry、failure handling 和性能阈值。每项必须对应测试或明确 manual evidence。

- [ ] **步骤 2：检查生成文件与无关改动**

```powershell
git status --short
git diff --check
git diff --stat
```

预期：只有本功能文件与 MetaParser 实际生成差异；没有手工编辑 Gen、RenderDoc capture、临时 PNG、artifact cache 或 build output。

- [ ] **步骤 3：检查提交历史粒度**

```powershell
git log --oneline 20afdafc..HEAD
```

预期：每个子任务为可回退的小提交；测试提交紧邻行为提交；没有把三阶段 squash 成无法定位回归的单提交。

- [ ] **步骤 4：记录最终验证矩阵**

交付说明必须列出：执行命令、PASS/FAIL/SKIP/timeout、OS/build、CPU、DX12 adapter/driver、未运行 backend、fixture 与确定性 camera/request path、改动前后 per-stage baseline delta、performance p50/p95/p99、是否启用 Debug Layer。RenderDoc 明确标为按用户要求未使用。

## 整体验收标准

- 建筑内部 shadow-casting light 不再直接照亮外部；单面墙背面和砖缝不因 normal map 漏光。
- 双面材质背面翻转几何/tangent frame 后正常 PBR 受光并双面投射。
- 方向、点、聚光阴影在 Forward/Deferred 一致，静态 cache hit 零 shadow draws，GPU 工作不越预算。
- 材质球由中性 IBL 主导，无明显硬交界和 isolated white pixels；fallback 可立即显示且不阻塞。
- FBX/glTF/prefab 缩略图仍 GPU-only、完整、异步，并继承源 ShaderLab keywords。
- 没有用一个 backend 的结果推断其他 backend；没有把超时、skip 或缺少工具描述为通过。
