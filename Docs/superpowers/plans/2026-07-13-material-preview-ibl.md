# 材质缩略图中性 Studio IBL 实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 superpowers:subagent-driven-development（推荐）或 superpowers:executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 用共享、预计算、确定性的中性 Studio IBL 替换材质球缩略图的双方向光主照明，同时保留异步、GPU-only prefab/model 预览和源 ShaderLab keyword。

**架构：** 正常 asset-build 路径按 recipe v1 生成 irradiance cube、GGX prefilter cube 和 BRDF LUT 三个 native texture artifacts；运行时只做 artifact-backed HDR 上传并跨请求共享。材质 preview shader 使用 split-sum IBL 和一个 0.15 弱 key；artifact 未就绪时立即走总强度 0.85 的确定性 16-sample softbox，不阻塞 UI。所有 GPU PBR 请求使用 cache v10，legacy v8 保持不变。

**技术栈：** C++20、TextureArtifact、RHI TextureCube/Texture2D、HLSL/ShaderLab、DX12 async readback、GoogleTest。

---

## 文件结构

- 创建：`Runtime/Rendering/Assets/StudioIblArtifacts.h/.cpp`，recipe、Hammersley、analytic studio source、卷积与 artifact descriptors。
- 创建：`Project/Editor/Assets/StudioIblArtifactBuilder.h/.cpp`，通过 artifact database 构建/发布三个共享 artifact。
- 创建：`App/Assets/Engine/Shaders/NullusShaderLibrary/StudioIBL.hlsl`，split-sum IBL 与 specular occlusion。
- 创建：`Tests/Unit/StudioIblArtifactTests.cpp`，描述符、确定性、上传和 recipe invalidation 测试。
- 修改：`Runtime/Rendering/Resources/TextureCube.h/.cpp`，支持 HDR cubemap artifact 全 mip 上传。
- 修改：`Runtime/Rendering/Resources/Loaders/TextureLoader.h/.cpp`，从 native cube artifact 创建 `TextureCube`。
- 修改：`Project/Editor/Assets/AssetDatabaseFacade.h/.cpp`，注册/复用 built-in studio IBL artifact manifest。
- 修改：`Project/Editor/Assets/EditorThumbnailPreviewRenderer.h/.cpp`，共享 IBL 状态、异步 prewarm、fallback 和 bindings。
- 修改：`Project/Editor/Assets/AssetThumbnailService.cpp`，GPU PBR cache version v10。
- 修改：`App/Assets/Engine/Shaders/ShaderLab/StandardPBR.shader`，thumbnail-only IBL 变体。
- 修改：`App/Assets/Engine/Shaders/LightGridCommon.hlsli`，复用安全 PBR/Fresnel 基础函数。
- 修改：`Runtime/Core/Assets/ArtifactLoadTelemetry.h/.cpp`，IBL ready/fallback/failed 与 preview timing stage。
- 修改：`Tests/Unit/AssetMaterialConversionTests.cpp`，IBL shader variant/binding 编译测试。
- 修改：`Tests/Unit/AssetThumbnailCacheTests.cpp`，v10、keyword、fallback 和 GPU-only 行为测试。
- 修改：`Tests/Unit/AssetThumbnailBehaviorTests.cpp`，真实 GPU 材质球像素阈值。
- 修改：`Tests/Performance/AssetThumbnailPerformanceTests.cpp`，共享加载和 preview 延迟性能。

### 任务 1：锁定 Studio IBL recipe 与 artifact 描述符

**文件：**
- 创建：`Runtime/Rendering/Assets/StudioIblArtifacts.h/.cpp`
- 创建：`Tests/Unit/StudioIblArtifactTests.cpp`

- [ ] **步骤 1：写三个 artifact 描述符失败测试**

```cpp
TEST(StudioIblArtifactTests, ProductionRecipeHasApprovedDescriptors)
{
    const auto recipe = StudioIblRecipe::Production();
    EXPECT_EQ(recipe.version, 1u);
    EXPECT_EQ(recipe.sampleCount, 1024u);
    EXPECT_EQ(recipe.irradiance, (StudioIblImageDesc{32u, TextureFormat::RGBA16F, 1u, 6u}));
    EXPECT_EQ(recipe.prefilter, (StudioIblImageDesc{128u, TextureFormat::RGBA16F, 8u, 6u}));
    EXPECT_EQ(recipe.brdfLut, (StudioIblImageDesc{256u, TextureFormat::RG16F, 1u, 1u}));
}
```

- [ ] **步骤 2：运行确认缺少 recipe**

```powershell
cmake --build Build --config Debug --target NullusUnitTests -- /m:4
.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=StudioIblArtifactTests.*Recipe*
```

预期：编译失败，`StudioIblRecipe` 尚未定义。

- [ ] **步骤 3：定义稳定 recipe identity**

`StudioIblRecipe::BuildIdentity()` 必须包含 algorithm 名、version、1024 samples、三个尺寸/格式和固定 environment rotation；artifact 使用 `encoderId="nullus-studio-ibl"`、`encoderVersion=1`、`buildIdentity=BuildIdentity()`。

- [ ] **步骤 4：实现确定性 Hammersley 和 analytic source**

Hammersley 使用 bit-reversal radical inverse，不使用随机数、时间、线程顺序或平台 locale。source 只由 neutral wall 和 broad white softbox 的解析函数组成：

```cpp
Vector2 Hammersley(uint32_t i, uint32_t count);
Vector3 EvaluateNeutralStudioRadiance(const Vector3& direction);
```

方向、软箱角宽、radiance 和 environment rotation 都是 recipe 常量。

- [ ] **步骤 5：增加 determinism 测试并确认绿灯**

以 `StudioIblRecipe::Test(16u)` 生成两次小尺寸输出，逐 byte 相等；检查代表方向均为有限、非负、中性通道，且 softbox 方向亮于 wall。

```powershell
.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=StudioIblArtifactTests.*Recipe*:StudioIblArtifactTests.*Deterministic*
```

预期：`PASS`。

- [ ] **步骤 6：提交 recipe 基础**

```powershell
git add Runtime/Rendering/Assets/StudioIblArtifacts.h Runtime/Rendering/Assets/StudioIblArtifacts.cpp Tests/Unit/StudioIblArtifactTests.cpp
git commit -m "feat: define deterministic studio IBL recipe"
```

### 任务 2：生成 irradiance、prefilter 与 BRDF LUT artifacts

**文件：**
- 修改：`Runtime/Rendering/Assets/StudioIblArtifacts.cpp`
- 修改：`Tests/Unit/StudioIblArtifactTests.cpp`

- [ ] **步骤 1：写输出布局失败测试**

对 test recipe 断言 cube subresource 顺序为每 mip 的 `+X,-X,+Y,-Y,+Z,-Z`，prefilter mip 尺寸连续到 1x1；row/slice pitch 与 RGBA16F/RG16F bytes-per-pixel 一致。

- [ ] **步骤 2：实现公共采样积分**

irradiance 对 cosine hemisphere 积分，prefilter 对 GGX importance sampling，LUT 积分 split-sum A/B。每个 production texel/mip 都恰好 1024 Hammersley samples；half-float conversion 使用仓库已有确定性实现。

```cpp
TextureArtifactData BuildStudioIrradianceArtifact(const StudioIblRecipe&);
TextureArtifactData BuildStudioPrefilterArtifact(const StudioIblRecipe&);
TextureArtifactData BuildStudioBrdfLutArtifact(const StudioIblRecipe&);
```

- [ ] **步骤 3：写物理单调性测试**

constant environment 的 irradiance 各 face 接近相同；prefilter roughness 增加时 softbox peak 非增；BRDF LUT 全部 finite 且 A/B 在 `[0,1]`。

- [ ] **步骤 4：运行生成器测试**

```powershell
.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=StudioIblArtifactTests.*Build*:StudioIblArtifactTests.*Monotonic*
```

预期：`PASS`。单元测试只跑缩小 test recipe；production 1024-sample bake 在 asset-build 集成测试中运行一次并缓存。

- [ ] **步骤 5：提交卷积生成器**

```powershell
git add Runtime/Rendering/Assets/StudioIblArtifacts.cpp Tests/Unit/StudioIblArtifactTests.cpp
git commit -m "feat: bake split-sum studio IBL artifacts"
```

### 任务 3：接入正常 artifact database 构建流程

**文件：**
- 创建：`Project/Editor/Assets/StudioIblArtifactBuilder.h/.cpp`
- 修改：`Project/Editor/Assets/AssetDatabaseFacade.h/.cpp`
- 修改：`Tests/Unit/AssetDatabaseFacadeTests.cpp`

- [ ] **步骤 1：写 recipe cache/invalidation 失败测试**

首次 `EnsureStudioIblArtifacts` 发布三条 texture artifact records；同 recipe 第二次零写入；version 或 build identity 改变时三条全部 stale 并原子替换。

```cpp
EXPECT_EQ(first.publishedArtifactCount, 3u);
EXPECT_EQ(second.publishedArtifactCount, 0u);
EXPECT_EQ(changedRecipe.publishedArtifactCount, 3u);
```

- [ ] **步骤 2：实现 deterministic built-in source identity**

使用固定 source AssetId `Guid::NewDeterministic("builtin:studio-ibl")`，三个 sub-assets 分别命名 `irradiance`、`ggx-prefilter`、`brdf-lut`。artifact payload 写入 `Library/Artifacts` 内容寻址路径，manifest/database 更新沿用 `AssetDatabaseFacade` 的事务和 flush batch。

- [ ] **步骤 3：禁止 editor frame 同步 production bake**

`EnsureStudioIblArtifacts` 只查询状态并排队 asset worker；production bake 发生在正常 import/build worker。缩略图 warmup 遇到 Missing/Building 时直接返回 not-ready，让 fallback 渲染；不得等待 worker、同步 IO 或 GPU readback。

- [ ] **步骤 4：实现三件 artifact 原子发布**

先写 temp content artifacts，三者成功后一次 manifest/database commit；任一失败不发布 partial recipe，保留上一 recipe last-good。失败 diagnostic 带 recipe identity，但不包含绝对用户路径。

- [ ] **步骤 5：运行 database 测试**

```powershell
cmake --build Build --config Debug --target NullusUnitTests -- /m:4
.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetDatabaseFacadeTests.*StudioIbl*
```

预期：`PASS`，second ensure 不重算或重写 artifacts。

- [ ] **步骤 6：提交 asset-build 接入**

```powershell
git add Project/Editor/Assets/StudioIblArtifactBuilder.h Project/Editor/Assets/StudioIblArtifactBuilder.cpp Project/Editor/Assets/AssetDatabaseFacade.h Project/Editor/Assets/AssetDatabaseFacade.cpp Tests/Unit/AssetDatabaseFacadeTests.cpp
git commit -m "feat: build shared studio IBL artifacts"
```

### 任务 4：支持 HDR cubemap artifact-backed 上传

**文件：**
- 修改：`Runtime/Rendering/Resources/TextureCube.h/.cpp`
- 修改：`Runtime/Rendering/Resources/Loaders/TextureLoader.h/.cpp`
- 修改：`Tests/Unit/StudioIblArtifactTests.cpp`
- 修改：`Tests/Unit/DX12TextureUploadUtilsTests.cpp`

- [ ] **步骤 1：写 cube artifact 上传失败测试**

构造 2 mip x 6 face 的 RGBA16F artifact，断言创建 descriptor 为 `TextureCube`、arrayLayers=6、mipLevels=2、format=RGBA16F，并按 mip/face 顺序保留 12 subresources。

- [ ] **步骤 2：运行测试确认当前仅支持 Image/RGBA8/no-mip**

运行 `StudioIblArtifactTests.*TextureCubeUpload*`，预期编译失败：缺少 artifact overload。

- [ ] **步骤 3：实现 `TextureCube::SetTextureResource(artifact)`**

严格验证 dimension=TextureCube、arrayLayers=6、linear color、完整连续 mip/face、非压缩 sampled+upload capability。构造：

```cpp
RHITextureDesc desc{
    .extent = {artifact.width, artifact.height, 1u},
    .dimension = TextureDimension::TextureCube,
    .format = artifact.format,
    .mipLevels = CountArtifactMipLevels(artifact),
    .arrayLayers = 6u,
    .usage = TextureUsageFlags::Sampled
};
```

`RHITextureUploadDesc::subresources` 按 artifact subresources 原顺序引用 payload；资源创建成功后一次替换 explicit texture。

- [ ] **步骤 4：扩展 TextureLoader**

新增 `CreateCubeFromArtifact(const TextureArtifactData&)` 和 path overload；不改变现有六张 LDR image API。失败返回 nullptr 且不保留 1x1 placeholder 为 ready。

- [ ] **步骤 5：运行上传测试**

```powershell
.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=StudioIblArtifactTests.*TextureCube*:DX12TextureUploadUtilsTests.*Cube*
```

预期：`PASS`，RGBA16F 全 mip 数据和 RG16F 2D LUT 都能上传。

- [ ] **步骤 6：提交 HDR 上传**

```powershell
git add Runtime/Rendering/Resources/TextureCube.h Runtime/Rendering/Resources/TextureCube.cpp Runtime/Rendering/Resources/Loaders/TextureLoader.h Runtime/Rendering/Resources/Loaders/TextureLoader.cpp Tests/Unit/StudioIblArtifactTests.cpp Tests/Unit/DX12TextureUploadUtilsTests.cpp
git commit -m "feat: upload HDR cubemap texture artifacts"
```

### 任务 5：实现共享 split-sum IBL shader

**文件：**
- 创建：`App/Assets/Engine/Shaders/NullusShaderLibrary/StudioIBL.hlsl`
- 修改：`App/Assets/Engine/Shaders/ShaderLab/StandardPBR.shader`
- 修改：`App/Assets/Engine/Shaders/LightGridCommon.hlsli`
- 修改：`Tests/Unit/AssetMaterialConversionTests.cpp`

- [ ] **步骤 1：写 shader binding/variant 失败测试**

要求 thumbnail variant 包含 `THUMBNAIL_STUDIO_IBL`，绑定 `TextureCube _StudioIrradiance`、`TextureCube _StudioPrefilter`、`Texture2D _StudioBrdfLut`，并保留 `_NORMALMAP`、`_ALPHATEST_ON`、Cull Off 的组合编译。

- [ ] **步骤 2：实现 split-sum evaluator**

```hlsl
float3 NLSEvaluateStudioIBL(
    float3 geometryNormalWS,
    float3 shadingNormalWS,
    float3 viewDir,
    float3 albedo,
    float metallic,
    float perceptualRoughness,
    float ao);
```

diffuse=`irradiance * kd * albedo / PI`；specular=`prefilter.SampleLevel(reflect(-V,Ns), roughness*maxMip) * (F0*brdf.x+brdf.y)`；AO 衰减 diffuse，specular 使用 roughness/NdotV 派生的 specular occlusion。环境旋转固定且不依赖相机。

- [ ] **步骤 3：避免 direct shadow 污染 ambient**

IBL 在 direct-light loop 外计算，不乘 `NLSEvaluateShadowVisibility`；weak key 仍走正常 geometry normal gate，intensity 固定 0.15。

- [ ] **步骤 4：编译所有目标排列**

```powershell
.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetMaterialConversionTests.*StudioIbl*:ShaderCompilerTests.*StudioIbl*
```

预期：DXIL/SPIR-V 编译成功并通过 `spirv-val`；register/space 与 native binding layout 测试一致。

- [ ] **步骤 5：提交 shader**

```powershell
git add App/Assets/Engine/Shaders/NullusShaderLibrary/StudioIBL.hlsl App/Assets/Engine/Shaders/ShaderLab/StandardPBR.shader App/Assets/Engine/Shaders/LightGridCommon.hlsli Tests/Unit/AssetMaterialConversionTests.cpp
git commit -m "feat: shade material previews with studio IBL"
```

### 任务 6：在 thumbnail renderer 异步加载、共享和 prewarm

**文件：**
- 修改：`Project/Editor/Assets/EditorThumbnailPreviewRenderer.h/.cpp`
- 修改：`Tests/Unit/AssetThumbnailCacheTests.cpp`

- [ ] **步骤 1：写共享状态机失败测试**

状态只允许 `Unrequested -> Loading -> Ready` 或 `Fallback/Failed`；多个 material requests 只发起一次 artifact resolve/upload 和一次 pipeline prewarm。Prefab/model 请求不得等待 IBL。

- [ ] **步骤 2：实现 `StudioIblRuntimeState`**

renderer-owned state 保存三个 shared texture handles、recipe identity、device generation 和 once-per-generation diagnostic。每次 pump 只轮询已有 async artifact result；ready 后绑定所有 stable preview materials。

- [ ] **步骤 3：预热 pipeline 且不阻塞首帧**

thumbnail warmup 排队 `THUMBNAIL_STUDIO_IBL` material variant；未完成时当前请求走 fallback。不能在 `Render`/`Pump` 中调用同步 asset import、`WaitForFence`、`ReadPixels` 或 drain global queue。

- [ ] **步骤 4：保留 source material 状态**

`CreateStablePreviewMaterial` 继续复制 `GetShaderLabKeywordNames()`、textures、sampler overrides、surface/cull/PBR parameters；只额外启用 renderer-owned `THUMBNAIL_STUDIO_IBL`。不得把 IBL keyword 写回源 material。

- [ ] **步骤 5：冻结可比较的 studio framing**

用命名的 `MaterialPreviewStudioConstants` 固定 environment rotation、sphere mesh、camera transform/FOV、exposure 和透明/中性 background；这些值进入 preview render identity 测试。不同材质请求只替换 source material，不改变 framing 或照明参数。

- [ ] **步骤 6：运行状态/keyword 测试**

```powershell
.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetThumbnailCacheTests.*StudioIbl*:AssetThumbnailCacheTests.*Keyword*:AssetThumbnailCacheTests.*GpuOnly*
```

预期：`PASS`，prefab/model complete-snapshot、complete framing、async readback 测试保持通过。

- [ ] **步骤 7：提交 runtime state**

```powershell
git add Project/Editor/Assets/EditorThumbnailPreviewRenderer.h Project/Editor/Assets/EditorThumbnailPreviewRenderer.cpp Tests/Unit/AssetThumbnailCacheTests.cpp
git commit -m "feat: share studio IBL across material previews"
```

### 任务 7：实现确定性 16-sample softbox fallback

**文件：**
- 修改：`Project/Editor/Assets/EditorThumbnailPreviewRenderer.cpp`
- 修改：`App/Assets/Engine/Shaders/ShaderLab/StandardPBR.shader`
- 修改：`Tests/Unit/AssetThumbnailCacheTests.cpp`

- [ ] **步骤 1：写 fallback 能量与非阻塞失败测试**

断言 16 个固定方向/权重的总 intensity 为 0.85，centroid 位于相机上前方；artifact Missing/Loading/Invalid 都在同一 pump 返回可渲染 fallback，而不是 `resources-pending` 无限等待。

- [ ] **步骤 2：实现 renderer-owned fallback samples**

将固定 4x4 softbox sample directions/intensities 放在 thumbnail frame constant/structured buffer；shader 在 `THUMBNAIL_STUDIO_IBL_FALLBACK` 变体循环 16 次调用共享 direct evaluator。所有 sample 合计 0.85，仍受 geometry normal horizon gate。

- [ ] **步骤 3：实现终止失败语义**

IBL 与 fallback pipeline 都失败时返回 stable diagnostic `thumbnail-material-preview-lighting-unavailable` 和现有 placeholder；服务写失败 metadata，不让 request 永久 Pending，也不转 CPU material renderer。

- [ ] **步骤 4：运行 fallback 测试并提交**

```powershell
.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetThumbnailCacheTests.*Softbox*:AssetThumbnailCacheTests.*LightingUnavailable*
git add Project/Editor/Assets/EditorThumbnailPreviewRenderer.cpp App/Assets/Engine/Shaders/ShaderLab/StandardPBR.shader Tests/Unit/AssetThumbnailCacheTests.cpp
git commit -m "fix: provide bounded material preview lighting fallback"
```

预期：`PASS`，fallback 不产生 UI-thread wait。

### 任务 8：升级 cache v10 且保持 prefab/model GPU-only

**文件：**
- 修改：`Project/Editor/Assets/AssetThumbnailService.cpp`
- 修改：`Tests/Unit/AssetThumbnailCacheTests.cpp`

- [ ] **步骤 1：写版本映射失败测试**

精确映射：MaterialSphere、ModelPreview、PrefabPreview 为 v10；Texture/Icon/CPU mesh legacy 路径仍为 v8；不再引用 v9。

```cpp
EXPECT_EQ(material.previewRendererVersion, "asset-browser-thumbnail-renderer:v10");
EXPECT_EQ(prefab.previewRendererVersion, "asset-browser-thumbnail-renderer:v10");
EXPECT_EQ(texture.previewRendererVersion, "asset-browser-thumbnail-renderer:v8");
```

- [ ] **步骤 2：改名并升级单一常量**

将 `kDoubleSidedPbrThumbnailRendererVersion` 改为 `kStudioIblPbrThumbnailRendererVersion`，值为 v10；所有 GPU PBR request 共用该常量，legacy 常量不变。

- [ ] **步骤 3：运行 routing/queue 回归**

```powershell
.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetThumbnailCacheTests.ServiceBuildsRequestsFromSourceAndGeneratedItems:AssetThumbnailCacheTests.*Prefab*Gpu*:AssetThumbnailCacheTests.*Complete*
```

预期：`PASS`；FBX/glTF/prefab 仍 GPU-only、等待所有 mesh/material ready 后一次发布，不恢复 CPU fallback。

- [ ] **步骤 4：提交 cache version**

```powershell
git add Project/Editor/Assets/AssetThumbnailService.cpp Tests/Unit/AssetThumbnailCacheTests.cpp
git commit -m "chore: invalidate PBR thumbnails for studio IBL"
```

### 任务 9：增加图像、telemetry 和性能证据

**文件：**
- 修改：`Runtime/Core/Assets/ArtifactLoadTelemetry.h/.cpp`
- 修改：`Tests/Unit/AssetThumbnailBehaviorTests.cpp`
- 修改：`Tests/Unit/AssetBrowserPresentationTests.cpp`
- 修改：`Tests/Performance/AssetThumbnailPerformanceTests.cpp`

- [ ] **步骤 1：写 telemetry 失败测试**

增加 IBL ready/fallback/failed counts 与 artifact resolve/upload、pipeline prewarm、preview GPU、readback timing；统计不触发 same-frame readback 或 device wait。

- [ ] **步骤 2：实现并接入 telemetry**

沿用 `ArtifactLoadTelemetryStage`，为 IBL resolve/upload/prewarm 增加独立 stage；result disposition 记录 Ready/Fallback/Failed。多请求共享加载只记一次 upload，preview count 按 request 记。

- [ ] **步骤 3：写真实 GPU 材质球夹具**

渲染 red/green/blue dielectrics、metallic 梯度、roughness 0.1/0.5/0.9、强 normal map；断言通道排序、roughness highlight 单调扩大/峰值降低，非轮廓 isolated bright pixels 为零：

```cpp
EXPECT_EQ(CountFireflies(frame, 0.98f, 0.75f, silhouetteMask), 0u);
EXPECT_LE(AdjacentLuminanceDeltaPercentile(frame, 0.99f, interiorMask), 0.12f);
```

- [ ] **步骤 4：运行 behavior 测试**

```powershell
cmake --build Build --config Debug --target NullusUnitTests -- /m:4
.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetThumbnailBehaviorTests.*StudioIbl*:AssetBrowserPresentationTests.*StudioIbl*:AssetThumbnailCacheTests.*StudioIbl*
```

预期：DX12 adapter 上全部 `PASS`；没有 adapter 时 GPU fixture `GTEST_SKIP`，不能当作图像通过证据。

- [ ] **步骤 5：运行性能测试**

```powershell
cmake -S . -B Build -DNLS_BUILD_TESTS=ON -DNLS_BUILD_PERFORMANCE_TESTS=ON
cmake --build Build --config Debug --target NullusPerformanceTests -- /m:4
.\Build\bin\Debug\NullusPerformanceTests.exe --gtest_filter=AssetThumbnailPerformanceTests.*StudioIbl*
```

预期：warmup 后 N 个材质请求无重复 artifact IO/upload/prewarm；pump 仍受既有 GPU/readback budgets 控制，prefab burst 中 material request 能进展，editor frame 无同步 wait。记录 OS/build、CPU、GPU、driver、backend、thumbnail fixture/request mix、确定性请求序列，以及改动前后 artifact resolve/upload/prewarm/preview GPU/readback 的 p50/p95/p99 baseline delta。

- [ ] **步骤 6：提交证据**

```powershell
git add Runtime/Core/Assets/ArtifactLoadTelemetry.h Runtime/Core/Assets/ArtifactLoadTelemetry.cpp Tests/Unit/AssetThumbnailBehaviorTests.cpp Tests/Unit/AssetBrowserPresentationTests.cpp Tests/Performance/AssetThumbnailPerformanceTests.cpp
git commit -m "test: validate studio IBL thumbnail quality"
```

## 阶段完成标准

- 三件 artifacts 的格式/尺寸/全 mip/1024 samples/recipe identity 精确符合规格，构建确定且缓存可失效。
- `TextureCube` 从 native RGBA16F cubemap artifact 上传全部 face/mip；BRDF LUT 使用现有 RG16F。
- runtime 只加载一次并共享；未就绪走 16-sample 0.85 fallback，不阻塞 UI，不永久 pending。
- 材质球以 IBL 为主、弱 key=0.15，无硬 terminator 和 isolated bright pixels。
- v10 只替换 GPU PBR cache identity；legacy v8 不变。
- prefab/model 继续 GPU-only、完整模型、async readback，并继承源 ShaderLab keywords。
- DXIL/SPIR-V、DX12 图像、telemetry 和性能验证有证据；未运行的 backend 不作正确性声明。
