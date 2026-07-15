# 跨格式法线贴图导入实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 superpowers:subagent-driven-development（推荐）或 superpowers:executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 让 glTF、FBX 和 OBJ/MTL 遵循同一法线贴图语义，并让 Sponza FBX 正确应用 `curtain_fabric_Normal.png`。

**架构：** 保留各解析器记录的原始材质通道，在 `MaterialConversion.cpp` 的内部命名空间集中分类 parser `bump` 引用。显式 normal 始终优先；只有纹理身份包含独立 `normal`/`normalmap` 标记时才把 bump 提升到线性 `Normal` 槽，其余高度图统一忽略并诊断。

**技术栈：** C++20、GoogleTest、Nullus ImportedScene/MaterialConversion、CMake/MSVC、RenderDoc DX12。

---

## 文件结构

- 修改：`Tests/Unit/AssetMaterialConversionTests.cpp`
  - 固定 glTF、FBX、OBJ/MTL 的统一法线选择、命名边界、优先级和诊断契约。
- 修改：`Runtime/Rendering/Assets/MaterialConversion.cpp`
  - 添加局部纹理身份分类 helper，并在 parser channel 转换中应用统一规则。
- 修改：`Tests/Unit/AssetImportPipelineTests.cpp`
  - 记录 v19 必须淘汰 v17 和不完整的 v18 模型材质产物。
- 修改：`Runtime/Core/Assets/AssetMeta.cpp`
  - 将共享 `ModelScene` importer 版本从 17 提升到 19。
- 修改：`Runtime/Rendering/Resources/Parsers/AssimpParser.cpp`
  - 在 Nullus 适配层从 ASCII/Binary FBX Objects/Connections 元数据恢复 3ds Max
    `ai_bump2d` 中间节点连接的文件 Texture；保留 FBX `bump_map` 与显式 normal
    属性的原始语义，并为无法识别 raw 属性的 `NORMAL_CAMERA` 保留保守 fallback，
    不修改 Assimp 三方代码。
- 验证：`TestProject/Assets/Model/pkg_a_curtains/NewSponza_Curtains_FBX_ZUp.fbx`
  - 重导入后材质应包含 curtain normal binding。
- 验证：`TestProject/Assets/Model/pkg_a_curtains/NewSponza_Curtains_glTF.gltf`
  - 重导入后保持显式 glTF normal binding。

### 任务 1：锁定跨格式法线选择契约

**文件：**
- 修改：`Tests/Unit/AssetMaterialConversionTests.cpp:6289`
- 测试：`Tests/Unit/AssetMaterialConversionTests.cpp`

- [ ] **步骤 1：把现有 FBX/OBJ bump 测试改为统一策略测试**

用以下测试结构替换 `FbxBumpOnlyChannelDoesNotEnableNormalMapping` 和
`ObjBumpChannelStillUsesNormalMapCompatibility`。复用文件内现有的
`FindSlot`、`CountSlots`、`FindFactor`、`HasDiagnosticCode` helper：

```cpp
TEST(AssetMaterialConversionTests, ParserBumpChannelsOnlyPromoteExplicitNormalMapIdentities)
{
    NLS::Render::Assets::ImportedScene scene;
    scene.sourceAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("e2010303-0303-4303-8303-030303030303"));
    scene.textures.push_back({"texture/fbx-normal", "CurtainNormal", "textures/curtain_fabric_Normal.png", "image/png"});
    scene.textures.push_back({"texture/obj-normal", "StoneNormalMap", "textures/StoneNormalMap.tga", "image/tga"});
    scene.textures.push_back({"texture/height", "CurtainHeight", "textures/curtain_height.png", "image/png"});
    scene.textures.push_back({"texture/false-positive", "AbnormalDetail", "textures/abnormal_detail.png", "image/png"});

    const auto convertBump = [&scene](const MaterialSourceModel sourceModel, const std::string& textureKey)
    {
        NLS::Render::Assets::ImportedSceneNamedRecord material;
        material.sourceKey = "material/bump";
        material.name = "BumpMaterial";
        material.materialChannels.push_back({"bump", textureKey, {}, false, 0.0});
        return NLS::Render::Assets::ConvertImportedSceneMaterial(scene, material, sourceModel);
    };

    for (const auto sourceModel : {MaterialSourceModel::FbxParserMaterial, MaterialSourceModel::ObjMtl})
    {
        const auto underscoredNormal = convertBump(sourceModel, "texture/fbx-normal");
        ASSERT_NE(FindSlot(underscoredNormal, "Normal"), nullptr);
        EXPECT_EQ(FindSlot(underscoredNormal, "Normal")->colorSpace, MaterialTextureColorSpace::Linear);
        EXPECT_EQ(CountSlots(underscoredNormal, "Normal"), 1u);
        EXPECT_TRUE(HasDiagnosticCode(underscoredNormal, "material-inferred-normal-map-from-bump-channel"));
        EXPECT_NE(underscoredNormal.serializedPayload.find("_NormalMap"), std::string::npos);
        EXPECT_NE(underscoredNormal.serializedPayload.find("keyword _NORMALMAP"), std::string::npos);

        const auto camelCaseNormal = convertBump(sourceModel, "texture/obj-normal");
        EXPECT_NE(FindSlot(camelCaseNormal, "Normal"), nullptr);

        for (const auto* rejectedTexture : {"texture/height", "texture/false-positive"})
        {
            const auto rejected = convertBump(sourceModel, rejectedTexture);
            EXPECT_EQ(FindSlot(rejected, "Normal"), nullptr);
            EXPECT_TRUE(HasDiagnosticCode(rejected, "material-ignored-bump-height-map"));
            EXPECT_EQ(rejected.serializedPayload.find("keyword _NORMALMAP"), std::string::npos);
        }
    }
}
```

- [ ] **步骤 2：添加显式 normal 优先级和 glTF 哨兵测试**

```cpp
TEST(AssetMaterialConversionTests, ExplicitNormalSemanticsWinOverBumpInferenceAcrossFormats)
{
    NLS::Render::Assets::ImportedScene scene;
    scene.textures.push_back({"texture/explicit", "AuthoredNormal", "AuthoredNormal.png", "image/png"});
    scene.textures.push_back({"texture/inferred", "FallbackNormal", "Fallback_Normal.png", "image/png"});

    NLS::Render::Assets::ImportedSceneNamedRecord fbx;
    fbx.sourceKey = "fbx/material/normal-priority";
    fbx.materialChannels.push_back({"normal", "texture/explicit", {}, false, 0.0});
    fbx.materialChannels.push_back({"bump", "texture/inferred", {}, false, 0.0});
    const auto convertedFbx = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene, fbx, MaterialSourceModel::FbxParserMaterial);
    ASSERT_EQ(CountSlots(convertedFbx, "Normal"), 1u);
    EXPECT_EQ(FindSlot(convertedFbx, "Normal")->textureKey, "texture/explicit");
    EXPECT_FALSE(HasDiagnosticCode(convertedFbx, "material-inferred-normal-map-from-bump-channel"));

    NLS::Render::Assets::ImportedSceneNamedRecord gltf;
    gltf.sourceKey = "material/0";
    gltf.normalTextureKey = "texture/explicit";
    const auto convertedGltf = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene, gltf, MaterialSourceModel::GltfPbrMetallicRoughness);
    ASSERT_EQ(CountSlots(convertedGltf, "Normal"), 1u);
    EXPECT_EQ(FindSlot(convertedGltf, "Normal")->textureKey, "texture/explicit");
    EXPECT_NE(convertedGltf.serializedPayload.find("keyword _NORMALMAP"), std::string::npos);
}
```

- [ ] **步骤 3：构建并运行测试，确认新契约失败**

运行：

```powershell
cmake --build Build --config Debug --target NullusUnitTests -- /m:4
Build/bin/Debug/NullusUnitTests.exe --gtest_filter="AssetMaterialConversionTests.ParserBumpChannelsOnlyPromoteExplicitNormalMapIdentities:AssetMaterialConversionTests.ExplicitNormalSemanticsWinOverBumpInferenceAcrossFormats"
```

预期：`ParserBumpChannelsOnlyPromoteExplicitNormalMapIdentities` 失败。FBX `_Normal`
没有 `Normal` slot，OBJ height 仍错误地产生 `Normal` slot，且两个新诊断均不存在。
显式 normal/glTF 哨兵可通过。

- [ ] **步骤 4：提交失败测试**

```powershell
git add Tests/Unit/AssetMaterialConversionTests.cpp
git commit -m "test: define cross-format normal map import"
```

### 任务 2：实现共享 bump 法线分类

**文件：**
- 修改：`Runtime/Rendering/Assets/MaterialConversion.cpp:18-97`
- 修改：`Runtime/Rendering/Assets/MaterialConversion.cpp:677-724`
- 测试：`Tests/Unit/AssetMaterialConversionTests.cpp`

- [ ] **步骤 1：添加纹理身份分词 helper**

在 `FindTexture` 后添加内部 helper。它只分析路径最后一段的 stem，将标点和
camel-case 边界分词，避免 `abnormal` 子串误判：

```cpp
std::vector<std::string> TextureIdentityTokens(const std::string_view identity)
{
    const auto stem = std::filesystem::path(identity).stem().string();
    std::vector<std::string> tokens;
    std::string token;
    for (size_t index = 0u; index < stem.size(); ++index)
    {
        const auto character = static_cast<unsigned char>(stem[index]);
        const bool camelBoundary =
            index > 0u && std::isupper(character) != 0 &&
            std::islower(static_cast<unsigned char>(stem[index - 1u])) != 0;
        if (!std::isalnum(character) || camelBoundary)
        {
            if (!token.empty())
            {
                tokens.push_back(std::move(token));
                token.clear();
            }
            if (!std::isalnum(character))
                continue;
        }
        token.push_back(static_cast<char>(std::tolower(character)));
    }
    if (!token.empty())
        tokens.push_back(std::move(token));
    return tokens;
}

bool IdentitySuggestsNormalMap(const std::string& identity)
{
    const auto tokens = TextureIdentityTokens(identity);
    for (size_t index = 0u; index < tokens.size(); ++index)
    {
        if (tokens[index] == "normal" || tokens[index] == "normalmap")
            return true;
        if (tokens[index] == "normal" && index + 1u < tokens.size() && tokens[index + 1u] == "map")
            return true;
    }
    return false;
}

bool TextureSuggestsNormalMap(const ImportedSceneNamedRecord& texture)
{
    return IdentitySuggestsNormalMap(texture.uri) ||
        IdentitySuggestsNormalMap(texture.name) ||
        IdentitySuggestsNormalMap(texture.sourceKey);
}
```

- [ ] **步骤 2：用显式 normal 优先的统一 parser 转换替换格式分支**

用以下逻辑替换现有 `normal`/`bump` 分支：

```cpp
if (const auto* normal = FindChannel(source, "normal"))
    AddTextureSlot(scene, material, context, "Normal", normal->textureKey, MaterialTextureColorSpace::Linear, source.sampler);

if (const auto* bump = FindChannel(source, "bump");
    bump && !bump->textureKey.empty() && FindTextureSlot(material, "Normal") == nullptr)
{
    const auto* bumpTexture = FindTexture(scene, bump->textureKey);
    if (!bumpTexture)
    {
        AddTextureSlot(scene, material, context, "Normal", bump->textureKey, MaterialTextureColorSpace::Linear, source.sampler);
    }
    else if (TextureSuggestsNormalMap(*bumpTexture))
    {
        AddTextureSlot(scene, material, context, "Normal", bump->textureKey, MaterialTextureColorSpace::Linear, source.sampler);
        if (FindTextureSlot(material, "Normal") != nullptr)
        {
            AddDiagnostic(
                material,
                "material-inferred-normal-map-from-bump-channel",
                "Parser bump texture was promoted because its identity explicitly identifies a tangent-space normal map.");
        }
    }
    else
    {
        AddDiagnostic(
            material,
            "material-ignored-bump-height-map",
            "Parser bump/height texture was ignored because it is not identified as a tangent-space normal map.");
    }
}
```

- [ ] **步骤 3：构建并运行定向材质转换测试**

运行：

```powershell
cmake --build Build --config Debug --target NullusUnitTests -- /m:4
Build/bin/Debug/NullusUnitTests.exe --gtest_filter="AssetMaterialConversionTests.ParserBumpChannelsOnlyPromoteExplicitNormalMapIdentities:AssetMaterialConversionTests.ExplicitNormalSemanticsWinOverBumpInferenceAcrossFormats:AssetMaterialConversionTests.ConvertsImportedSceneMaterialChannels"
```

预期：全部 PASS；FBX 和 OBJ 对相同纹理身份产生相同 Normal slot 与诊断，glTF
显式 normal 行为不变。

- [ ] **步骤 4：运行所有材质转换测试**

```powershell
Build/bin/Debug/NullusUnitTests.exe --gtest_filter="AssetMaterialConversionTests.*"
```

预期：全部 PASS，且没有旧 FBX diagnostic 或 OBJ bump 兼容性断言残留。

- [ ] **步骤 5：提交转换实现**

```powershell
git add Runtime/Rendering/Assets/MaterialConversion.cpp Tests/Unit/AssetMaterialConversionTests.cpp
git commit -m "fix: classify normal maps consistently across model formats"
```

### 任务 3：使旧模型产物失效

**文件：**
- 修改：`Tests/Unit/AssetImportPipelineTests.cpp:9535`
- 修改：`Runtime/Core/Assets/AssetMeta.cpp:141`
- 测试：`Tests/Unit/AssetImportPipelineTests.cpp`

- [ ] **步骤 1：添加 v17 失效测试**

```cpp
TEST(AssetImportPipelineTests, ModelSceneImporterVersionInvalidatesCrossFormatNormalMapVersion17Artifacts)
{
    EXPECT_GT(
        NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene),
        17u)
        << "Importer version 17 FBX materials can omit normal-named bump textures and OBJ materials can misdecode height maps as tangent-space normals.";
}
```

- [ ] **步骤 2：构建并运行测试，确认 v17 红灯**

```powershell
cmake --build Build --config Debug --target NullusUnitTests -- /m:4
Build/bin/Debug/NullusUnitTests.exe --gtest_filter="AssetImportPipelineTests.ModelSceneImporterVersionInvalidatesCrossFormatNormalMapVersion17Artifacts"
```

预期：FAIL，当前 `ModelScene` importer version 等于 17。

- [ ] **步骤 3：将 ModelScene importer version 更新为 19**

在 `Runtime/Core/Assets/AssetMeta.cpp` 中修改：

```cpp
case AssetType::ModelScene:
    return 19u;
```

- [ ] **步骤 4：重新构建并确认缓存测试通过**

```powershell
cmake --build Build --config Debug --target NullusUnitTests -- /m:4
Build/bin/Debug/NullusUnitTests.exe --gtest_filter="AssetImportPipelineTests.ModelSceneImporterVersionInvalidates*"
```

预期：全部 PASS，包括新增 v17 失效契约。

- [ ] **步骤 5：提交 importer 版本变更**

```powershell
git add Runtime/Core/Assets/AssetMeta.cpp Tests/Unit/AssetImportPipelineTests.cpp
git commit -m "fix: invalidate legacy model normal map artifacts"
```

### 任务 4：回归验证与 Sponza 运行时证据

**文件：**
- 验证：`TestProject/Library/Artifacts/`
- 验证：`TestProject/Logs/RenderDoc/Editor/dx12/`

- [ ] **步骤 1：运行完整行为测试**

```powershell
ctest --test-dir Build -C Debug -L behavior --output-on-failure
```

预期：`NullusUnitTests` PASS。若完整套件存在与本改动无关的基线失败，记录完整测试名，
并再次运行任务 2、3 的定向过滤器确认本改动测试仍通过。

- [ ] **步骤 2：检查代码差异卫生**

```powershell
git diff --check HEAD~3..HEAD
git status --short
```

预期：`git diff --check` 无输出；工作区不包含未提交的本任务代码。

- [ ] **步骤 3：启动 Editor 触发 importer v19 重导入并生成 DX12 capture**

先按 `nullus-renderdoc-debug` 技能和 `Docs/Rendering/RenderDocDebugging.md` 执行：

```powershell
py -3 Tools/RenderDoc/renderdoc_runner.py --target editor --backend dx12 --capture
```

预期：runner 报告实际 backend 为 DX12，并在
`TestProject/Logs/RenderDoc/Editor/dx12/` 写出新的 `.rdc`。Editor 启动时因
ModelScene importer v19 重建 FBX、OBJ 和 glTF 模型产物。

- [ ] **步骤 4：核对 Sponza FBX 与 glTF 材质产物**

在 ArtifactDB 中定位两个 source asset 的当前 v19 manifest，读取 curtain material
payload。两个产物都必须包含：

```text
texture _NormalMap
keyword _NORMALMAP
curtain_fabric_Normal.png
```

FBX 产物还应包含 `material-inferred-normal-map-from-bump-channel`；glTF 不应包含该
推断诊断，因为它使用显式 `normalTexture`。

- [ ] **步骤 5：分析 RenderDoc draw binding**

```powershell
py -3 Tools/RenderDoc/rdc_analyze.py <步骤3输出的latest_capture路径> --json-out Build/RenderDocAnalysis/cross-format-normal-map.json
```

在 curtain 的 FBX 和 glTF draw 上确认 `_NormalMap` 对应的 texture SRV 已绑定，且
pixel shader 使用 `_NORMALMAP` variant。记录请求 backend 和实际 backend 均为 DX12；
不以 DX12 结果外推 Vulkan、OpenGL 或 macOS。

- [ ] **步骤 6：最终自审**

逐项确认：没有修改 `Runtime/*/Gen/`；没有把真实 height/bump 绑定为 Normal；显式
normal 不会产生重复 slot；所有格式都通过相同分类规则；提交只包含计划列出的文件。
