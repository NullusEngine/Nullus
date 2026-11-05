# Nullus 跨平台游戏打包方案（实施记录）

本文记录 `Docs/Plans/CrossPlatformPackaging.md` 所述方案在当前源码中的落地情况、可执行命令、格式约束与后续阶段。

## 1. 已落地的公共接口

| 接口 | 位置 | 说明 |
|---|---|---|
| `BuildProfile` / `BuildTarget` | `Runtime/Build/BuildTypes.h` | 产品信息、版本、目标平台、场景、资源规则、构建模式、图标、输出格式、签名引用 |
| `BuildProfiles.json` | `Project/*/BuildProfiles.json`（示例：`TestProject/BuildProfiles.json`） | 项目内版本化配置，编辑器面板与 `NullusBuild` 共用 |
| `BuildPipeline` | `Runtime/Build/BuildPipeline*.cpp` | 校验与冻结输入、依赖收集、烘焙检查、项目脚本编译、程序准备、内容打包、组装、验证、签名、出包、报告 |
| `ManagedScriptBuildService` | `Runtime/Build/ManagedScriptBuildService.{h,cpp}` | 从项目 `Assets/**/*.cs` 构建项目专用 `GameScripts.dll`（Generator/Managed/EngineScripts 按依赖顺序） |
| `ShaderPassArtifactRegistry` | `Runtime/Rendering/Resources/Loaders/ShaderPassArtifactRegistry.{h,cpp}` | 打包运行时按 ShaderLab 源路径注册并查找 Shader pass 工件 |
| `IBuildPlatform` | `Runtime/Build/IBuildPlatform.h` | 工具链检查、Player 解析、组装、签名、出包、成品自检 |
| `WindowsDesktopPlatform` | `Runtime/Build/Windows/WindowsDesktopPlatform.{h,cpp}` | Windows x64/DX12 Player 模板、Authenticode、ZIP/目录输出、成品自检 |
| `UnixDesktopPlatform` | `Runtime/Build/Unix/UnixDesktopPlatform.{h,cpp}` | Linux x64（Vulkan/OpenGL）与 macOS（Metal）模板解析、`.app` 处理、`$ORIGIN` RPATH、ZIP 模式、目标平台 .NET 运行时 |
| `BuildPlatformRegistry` | `Runtime/Build/BuildPlatformRegistry.cpp` | 注册桌面适配器；对 Android/iOS/Web/ARM64 声明能力与 SDK/NDK/Xcode 探测原因 |
| `BuildReport` | `Runtime/Build/BuildReport.{h,cpp}` | 输入哈希、阶段耗时、资源包含原因、缓存命中、输出文件 SHA-256 |
| `NullusBuild` | `Tools/NullusBuild/Main.cpp` | 无界面工具，编辑器与 CI 共用同一命令 |

## 2. 命令

```text
NullusBuild build --project MyGame.nullus --profile windows-shipping --output Dist
NullusBuild build --project MyGame.nullus --profile linux-shipping --output Dist --player App/Linux_Debug_Shared
NullusBuild build --project MyGame.nullus --profile macos-shipping --output Dist --player App/MacOS_Release_Shared
NullusBuild build --project MyGame.nullus --player <template-dir> [--no-sign] [--validate-launch]
NullusBuild build ... [--no-bake] [--force-bake]
NullusBuild bake  --project MyGame.nullus [--profile <id>] [--force-bake] [--filter <asset-substring>]
NullusBuild validate --package Dist/MyGame
NullusBuild inspect  --package Dist/MyGame.zip
NullusBuild profiles --project MyGame.nullus
NullusBuild assets   --project MyGame.nullus [--filter <source-path>]
NullusBuild capabilities
NullusBuild pack     --filter <input-dir> --output <file.npak> [--profile <archive-prefix>]
```

`build` 默认先执行共享烘焙（可用 `--no-bake` 跳过，`--force-bake` 忽略缓存）；`bake` 可单独运行以预热目标平台工件，`--filter` 支持按资源路径子串局部烘焙。Linux 模板可在 Windows 主机上交叉组装并跳过非本机自检；macOS 适配器要求 macOS 主机，以便校验 Mach-O、更新 `.app` 并执行 `codesign`。

编辑器 `Build` 菜单和 **Build Profiles** 面板通过 `NullusBuild.exe` 子进程调用同一流程。面板保存 `BuildProfiles.json`，并展示当前平台的适配器能力（未实现平台显示缺失原因）。GUI 自动化可设置 `NLS_EDITOR_SKIP_VFX_PANELS=1` 跳过尚未完成的 VFX Editor 面板。

## 3. `BuildProfiles.json`

```json
{
  "schemaVersion": 1,
  "defaultProfile": "windows-development",
  "profiles": [
    {
      "id": "windows-development",
      "displayName": "Windows Development",
      "productName": "TestProject",
      "companyName": "Nullus",
      "version": "0.1.0",
      "mode": "Development",
      "target": {
        "platform": "windows",
        "architecture": "x64",
        "graphicsBackend": "dx12",
        "minimumOSVersion": "10.0.19045",
        "artifactPlatform": "win64-dx12",
        "shaderTarget": "dxil",
        "textureFormatPolicy": "target-native",
        "maxTextureSize": 2048,
        "scriptBackends": ["lua", "csharp"]
      },
      "startScene": "Scenes/Main.scene",
      "assetPackSelections": [],
      "keepAssetPaths": ["Scenes/Dynamic/*.scene", "0123456789abcdef0123456789abcdef"],
      "rawAssetExtensions": [".lua", ".ui", ".style", ".loc", ".csv", ".fnt", ".ttf", ".otf", ".json"],
      "iconPath": "",
      "executableName": "MyGame",
      "outputFormat": "zip",
      "outputDirectory": "",
      "playerTemplatePath": "",
      "signingProfile": "",
      "includeDebugSymbols": true,
      "customNativeModules": [],
      "validateLaunchOnBuild": false
    }
  ]
}
```

约束：

- `Development` 使用 `RelWithDebInfo`，`Shipping` 使用 `Release`。
- `artifactPlatform` 应与目标平台能力匹配，Windows DX12 示例为 `win64-dx12`；`Shipping` 不允许 `editor`。
- `maxTextureSize` 为纹理长边上限（0 表示保持源尺寸）；修改后会参与烘焙缓存键并触发重新烘焙。
- `scriptBackends` 启用 C# 时，构建流水线会从项目源码编译 `GameScripts.dll`，而不是复用 Player 模板中的样本程序集。
- `Shipping` 配置了 `customNativeModules` 时必须使用源码构建的 Player 模板。
- 签名只保存引用（`signingProfile`）。凭据从 `NLS_SIGNING_CERT_SHA1`、`env:<name>` 或 CI 密钥解析，不写入项目。

## 4. `.npak` 容器

实现：`Runtime/Core/Assets/NpakArchive.{h,cpp}`。

- 魔数 `NPAK`，显式小端编码，64 位偏移，版本化头部（128 字节），头部与索引各自使用 FNV-1a 校验。
- Zstandard 1.5.6（`ThirdParty/zstd`，固定版本），默认级别 3、256 KiB 独立块；压缩后无收益的块按原样存储。
- 相同内容按 SHA-256 + 大小去重；文件与块索引稳定排序；索引不包含机器绝对路径或构建时间。
- 相同输入生成字节完全一致的包（测试覆盖）。
- 只读接口支持 `Exists`、按范围读取、整文件读取与分块解压，块缓存有界（默认 8 MiB，可配置）。
- 打开时校验版本、索引边界、索引校验和；读取时校验块校验和与内容哈希。

## 5. 运行时内容接口

实现：`Runtime/Core/Assets/ContentSource.{h,cpp}`。

- `IContentSource`：`Exists` / `GetSize` / `ReadAt` / `ReadAll`。
- `DirectoryContentSource`：松散目录后端。
- `PackageContentSource`：`.npak` 后端，范围读取只解压触及的块。
- `ContentSourceTable`：按宿主根目录挂载，最长前缀优先；支持绝对路径和 `Library/Artifacts/...` 逻辑路径。
- 原生工件读取（`NativeArtifactContainer`、`ArtifactFileReader`、网格/纹理预览）在存在挂载时优先走内容接口，文件系统路径保持不变。
- 打包运行时由 `RuntimeAssetDatabase` 从 `RuntimeAssetManifest.json` 读取 Shader 工件，并把 `sourcePath -> pass artifact` 注册到 `ShaderPassArtifactRegistry`；`MaterialLoader` 优先按该注册表绑定 ShaderLab pass，不依赖 ArtifactDB。

发布版启动顺序（`Project/Game/Core/Context.cpp`、`RuntimeAssetManifestStartup.cpp`、`Main.cpp`）：

1. 通过 `ResolveInstallResourceRoots()` 定位安装根目录，并把工作目录固定到安装根（支持任意启动 cwd）。
2. 读取 `Player.info`，校验引擎版本、目标平台、容器版本。
3. 挂载 `Data/User/Packages/*.npak`，读取 `Data/User/RuntimeAssetManifest.json`。
4. 初始化脚本与图形系统，按 `Game.ini` 的 `start_scene` 加载启动场景。
5. `Player.info` 存在时绝不回退到 `Library/ArtifactDB` 或向上搜索 `TestProject`。

安装布局：

```text
<install>/
  MyGame.exe / MyGame / MyGame.app/Contents/MacOS/MyGame
  NLS_*.dll / libNLS_*.so / *.dylib
  Player.info
  BuildReport.json
  Assets/Engine/...                 # 引擎资源（来自 Player 模板）
  Scripts/GameScripts.dll           # 项目源码构建的 C# 程序集（启用 C# 时）
  Scripts/EngineScripts.dll
  Scripts/Nullus.Managed.dll
  Runtime/dotnet/                   # 自包含 .NET 运行时（启用 C# 时，按目标平台选择）
  Data/User/Game.ini
  Data/User/Assets/...              # 首期松散原始资源（场景、Prefab、UI、Lua 等）
  Data/User/RuntimeAssetManifest.json
  Data/User/Packages/base.npak
  Data/User/Packages/<pack>.npak
```

Unix Player 的共享库使用安装根布局；Linux 适配器会把 ELF 的 `DT_RPATH`/`DT_RUNPATH` 归一化为 `$ORIGIN`，并在 ZIP 中为产品可执行文件和 `Runtime/dotnet/dotnet` 写入 Unix `0755` 模式。macOS `.app` 模板会同步重命名 `Contents/MacOS/Game` 并更新 `Info.plist` 的 `CFBundleExecutable`。

`--package-self-test` 模式只做内容挂载、清单加载、启动场景与脚本载荷检查，不创建图形设备，供 CI 和 `NullusBuild --validate-launch` 使用。

## 6. 构建阶段实现

1. **检查与冻结输入**：校验项目文件、配置、启动场景、ArtifactDB、目标能力、Player 模板与输出目录。构建前后分别计算 Assets 与 ArtifactDB 的目录指纹，变化则失败。
2. **收集依赖**：`BuildDependencyCollector` 以启动场景文档与显式 keep 规则为根。场景/Prefab 作为原始 ObjectGraph 文档参与扫描；解析其中的 PPtr 与 prefab instance 引用，解析到目标平台已烘焙的工件，再沿 `ArtifactManifest.dependencies` 递归收集场景、Prefab、材质、纹理、着色器、脚本、UI、字体、本地化等依赖；报告每个资源的包含原因（`start-scene:reference`、`dependency-of:*`、`keep-rule:*`）；缺失引用、重复 GUID、缺失工件文件直接报错。场景/Prefab 文档本身随包松散交付。
3. **离线烘焙**：共享 `NLS_Cook` 库按目标能力生成工件，写入 `Library/Artifacts` 并更新 `ArtifactDB`；缓存键包含源文件/meta 哈希、目标平台、导入器与工具版本、纹理分辨率上限，第二次构建自动跳过未变化资源。当前共享烘焙覆盖：
   - Texture（PNG/JPG/TGA/BMP/HDR → 目标压缩格式；Windows 使用 DirectXTex 生成 BC1/BC5/BC7，其他平台暂回退 RGBA8；支持按 profile 的 `maxTextureSize` 降采样）
   - Shader（ShaderLab 解析、LightMode pass 拆分、变体收集、DXIL/SPIR-V 编译、反射与原生工件）
   - Material（`shaderLabMaterialVersion=1` 原生容器，扫描 shader/纹理/父材质引用）
   - Prefab（校验 ObjectGraph 文档并收集引用）
   - Model（glTF/GLB/FBX/OBJ：网格工件、材质工件、生成的 Model Prefab；外部纹理复用已烘焙纹理，GLB/glTF 内嵌纹理生成独立纹理工件并绑定到材质槽）
   - UI Document / UI StyleSheet / Localization Table（原生工件）

   引擎内置 Shader 通过虚拟 `Assets/Engine/Shaders/...` 路径在构建前镜像到 `Library/EngineCook` 并用稳定 GUID 烘焙，因此冷启动项目不依赖编辑器 `.meta` 或导入缓存。VFX、Audio 以及模型的骨骼/蒙皮/动画/Morph 子资源仍由编辑器导入器负责；共享烘焙会以 `cook-type-unsupported` 或 `cook-model-*` 警告列出。Shipping 仍拒绝 `editor` 工件。

   实测：4K 粗糙度纹理从 RGBA8 的 89.5 MB 降至 BC1 的 11.2 MB；`maxTextureSize=2048` 时同一纹理为 BC1 的 2.67 MB。Sponza glTF 烘焙出 405 meshes、28 materials、1 prefab，网格工件合计 86 MB。网格工件 v3 把顶点从 56 B 压到 32 B（位置/UV 保持 float，法线/切线/副切线有符号归一化字节），顶点数不超过 65535 时索引自动使用 16 位；旧版 v1/v2 工件仍可加载。
4. **编译项目脚本**：profile 启用 C# 且项目 `Assets/**/*.cs` 存在时，`build-scripts` 阶段调用 `ManagedScriptBuildService`：
   - 使用仓库 `Tools/Dotnet` 中的 SDK 和项目根目录旁的 `Managed/Nullus.GameScripts` 工程；
   - 先 restore，再按 `Nullus.ScriptGenerator` → `Nullus.Managed` → `Nullus.EngineScripts` → `Nullus.GameScripts` 顺序构建；
   - Shipping 使用 `Release`，Development 使用 `Debug`；
   - 只接受本次构建产出的 `GameScripts.dll`，不会使用 Player 模板中可能残留的样本程序集；
   - 失败时把 `last-build.log` 摘要写入 `build-project-scripts` 诊断并终止 Shipping 构建。
5. **准备程序与脚本**：校验 Player 模板版本/目标。编译成功后把项目自己的 `GameScripts.dll`、`EngineScripts.dll`、`Nullus.Managed.dll` 及配置文件装配到成品 `Scripts/`，并从 `Tools/Dotnet/<windows|linux|macos>/<arch>` 补入目标平台自包含 `Runtime/dotnet`（不再无条件使用宿主平台运行时）。Lua 保持资源读取接口加载。
6. **组装与验证**：只复制 Player 模板清单与引擎资源，不整体复制开发 `App` 目录；过滤 `Editor*`/`Launcher*` 等开发入口；组装后重新打开每个 `.npak` 并逐条校验载荷哈希，核对清单引用、`Player.info` 与启动场景。
7. **签名与出包**：配置签名引用时 Windows 调用 `signtool`（SHA-256 + 时间戳）；未配置时明确记录为未签名。Linux/macOS 当前记录为未签名，macOS Developer ID 与公证尚未接入。Unix 适配器支持 ZIP/目录输出，ZIP 会保留可执行模式；`tar.zst`/AppImage/DMG 等格式会明确报未实现。Windows 独立归档 PDB，并写入构建报告与第三方许可清单；失败或取消只清理本次 staging。

## 7. 平台状态

| 平台 | 状态 |
|---|---|
| Windows x64 / DX12 | 已实现：预编译 Player、ZIP/目录、Authenticode、成品自检；当前 TestProject Shipping 增量包 ZIP 为 **341,939,828 B**（`base.npak` 286,839,111 B），自检通过；历史冷启动包约 390.9 MB ZIP |
| Windows x64 / Vulkan | 适配器接受该目标，但当前默认 Player 模板为 DX12 构建 |
| Linux x64 / Vulkan、OpenGL | 已实现：模板解析、开发入口过滤、`$ORIGIN` ELF 修复、目标平台 .NET 运行时、ZIP 可执行模式、ZIP/目录输出；可在 Windows 主机交叉组装，非本机自检明确跳过。现有 `App/Linux_Debug_Shared` 仅为旧 Debug Player，未提供 Release 模板 |
| macOS x64/arm64 / Metal | 适配器源码已实现：`.app` 模板解析、`Info.plist` 可执行名同步、ZIP 可执行模式；要求 macOS 主机。尚无 macOS Player 模板，Developer ID 签名与公证未接入 |
| Windows/Linux ARM64、Android、iOS、Web、主机 | 已声明缺失原因；Android/iOS 能力探测会报告 SDK/NDK/Xcode 检测结果，编辑器面板与 `capabilities` 明确展示 |

## 8. 测试与验收

`NullusBuildTests`（`Tests/Unit/Build*Tests.cpp`、`PackageInspectionTests.cpp`、`AssetCookerTests.cpp`、`UnixDesktopPlatformTests.cpp`）共 **38 个测试**，覆盖：

- 容器往返、跨块读取、确定性字节、去重、截断、损坏、未知版本、路径冲突、缓存预算、store 模式。
- 配置读写与校验、默认配置、目标标识、能力矩阵。
- 依赖收集：场景原始文档引用、嵌套依赖、包含原因、原始文件、缺失工件、重复 GUID、缺失场景、通配 keep 规则。
- 共享烘焙：纹理/Shader/Material/Prefab/Model/UI 工件落盘与容器校验、BC 压缩、纹理分辨率上限、ShaderLab 多 pass 编译、模型内嵌纹理与材质槽绑定、网格顶点/索引压缩、增量跳过、强制重建、未支持类型的警告行为。
- 构建流水线：完整阶段执行、报告落盘、成品 `validate`、相同输入资源包哈希一致、失败保留既有成品并清理 staging；项目级 C# 脚本从源码编译、托管程序集与自包含 .NET 运行时装配。
- 项目脚本验证：在冷启动项目中新增 `ColdMarker.cs` 后 `GameScripts.dll` 哈希变化，删除后哈希恢复，确认成品使用当前项目源码而非模板样本程序集。
- 成品检查：目录与 ZIP、缺失包、未解析清单条目、损坏载荷。
- 运行时兼容：`Player.info` 的 `artifactPlatform` 接受 `win64-*`（DX12/Vulkan）目标，不再把运行时目标写死为裸 `win64`；`targetId` 仍校验主机平台/架构，Shipping 拒绝 `editor` 内容。
- Unix 桌面适配器：Linux 能力矩阵、模板解析与 `Editor*`/`Launcher*` 过滤、可执行文件重命名、macOS `.app` 的 `Info.plist` 更新、ZIP 中央目录 Unix `0755`/`0644` 模式。Linux 交叉成品使用 `$ORIGIN` 修复后的 RPATH，WSL 中可直接加载共享库并启动旧 Player（旧模板不识别 `--package-self-test`，因此原生内容自检尚未完成）。对应命令：`NullusBuildTests --gtest_filter=UnixDesktopPlatformTests.*`。
- 编辑器集成：完整 `Editor` 目标已构建/链接，`BuildProfilesPanel` 与 Build 菜单动作随编辑器编译；`Editor.exe --help` 启动路径通过。
- GUI 冒烟：使用最小项目启动 Editor 后，点击 `Build` 菜单可展开并显示 `Build Profiles...` 入口；面板设置最小尺寸 760×480。完整点击回归暂时被 VFX Editor WIP 启动崩溃（`VfxDocument::New` fast-fail）阻塞。自动化可用 `NLS_EDITOR_SKIP_VFX_PANELS=1` 跳过该 WIP 面板启动 Editor。
- 打包 Shader 绑定：`RuntimeAssetDatabase` 从运行时清单注册 Shader pass 工件，`MaterialLoader` 按 ShaderLab 源路径直接绑定，无需随包 ArtifactDB。

GUI 回归步骤（当前因 VFX WIP 阻塞，保留为验收脚本）：

```powershell
$env:NLS_EDITOR_SKIP_VFX_PANELS = "1"
Editor.exe <project>/<project>.nullus
```

1. 点击顶部 `Build`，确认 `Build Profiles...` 菜单项可见。
2. 点击 `Build Profiles...`，确认面板打开且 `BuildProfiles.json` 被加载或合成默认 profile。
3. 选择 `windows-shipping`，点击 `Reload`，确认输入框显示当前 profile；在 `Target` 区域检查 `Max texture size`。
4. 修改 `Display name`，点击 `Save`，确认 `BuildProfiles.json` 原子写回且面板状态恢复为已保存。
5. 输入新 profile id，点击 `Add profile`，确认列表和选中项更新。
6. 点击 `Build with this profile`，确认启动 `NullusBuild.exe`，并在输出目录生成 `BuildReport.json`。

端到端冒烟（`TestProject`，真实 Windows Player，Sponza glTF，冷启动无 `Library`）：

- 从仅含源码与 `.meta` 的冷启动副本执行 `NullusBuild build --profile windows-shipping`，`bake-assets` 耗时约 1193 秒。
- 依赖收集得到 **516 个工件 / 510 条运行时清单**，其中包含 4 个 `StandardPBR` Shader pass 工件；引擎 Shader 通过虚拟 `Assets/Engine/Shaders` 根和稳定 GUID 自动烘焙。
- 纹理按 profile 的 `maxTextureSize=2048` 降采样；`base.npak` 为 **286,816,426 B（273.5 MiB）**，ZIP 约 **390.9 MB**，相比 4K 纹理版本约 904 MB 的 ZIP 再缩小约 2.3 倍。
- C# Shipping：项目 `Assets/**/*.cs` 经 `build-scripts`（约 12.7 秒）编译为项目专用 `GameScripts.dll`；`Scripts/GameScripts.dll`、`EngineScripts.dll`、`Nullus.Managed.dll` 与 `Runtime/dotnet` 均进入成品。
- 成品目录、ZIP 均通过解包/哈希校验；打包后的 `TestProject.exe --package-self-test` 在 `C:\Windows` 工作目录下通过（1 个包、510 条清单、启动场景 `New Scene.scene`）。
- 网格工件使用 v3 压缩格式，旧 v1/v2 仍兼容。

当前增量验证（2026-10-21，使用已有烘焙缓存）：Windows ZIP 为 **341,939,828 B**，`base.npak` 为 **286,839,111 B**；`Runtime/dotnet` 只复制 `Microsoft.NETCore.App`（不再携带未使用的 ASP.NET Core 框架），因此 ZIP 比历史冷启动包缩小约 49 MB。成品自检仍通过。

Linux x64 交叉组装冒烟（Windows 主机 + `App/Linux_Debug_Shared`，profile `linux-shipping`，`maxTextureSize=1024`）：

- 目标内容全部烘焙为 `linux` / SPIR-V；`base.npak` 为 **123,944,007 B**，510 条清单；ZIP 为 **267,449,670 B**，共 286 个归档条目。
- `Runtime/dotnet` 选用 `Tools/Dotnet/linux/x64`，共 186 个文件、72,269,039 B，不再错误打入 Windows `dotnet.exe`；WSL 中 `./Runtime/dotnet/dotnet --info` 报告 `linux-x64`、.NET 8.0.15。
- ZIP 中 `TestProject` 与 `Runtime/dotnet/dotnet` 的 Unix 模式均为 `0100755`；解压后 WSL 中 `readelf` 显示 RUNPATH 为 `$ORIGIN`，旧 Player 成功加载全部 `libNLS_*d.so` 并启动。
- 该旧 Debug Player 不包含 `--package-self-test`，所以本次只证明组装、权限、RPATH 和动态链接，不构成 Linux 内容自检或 Shipping 设备验收；非本机自检按设计记录 `build-launch-cross-host` 警告并跳过。

## 9. 后续阶段

- 修复 VFX Editor 的 `VfxDocument::New` 栈损坏后，完成 Build Profiles 面板的完整 GUI 点击回归（Reload / 修改字段 / Save / Add profile / Build with this profile）。
- 将 VFX、Audio 的编辑器导入逻辑迁入 `NLS_Cook`，补齐模型的骨骼/蒙皮/动画/Morph 子资源；为 Linux/macOS/Android/iOS 增加 BC/ASTC/ETC 编码器，并评估 meshopt 顶点/索引压缩与自动 LOD 生成。
- 原始资源读取全部迁移到内容接口（当前场景/Prefab/UI/Lua 等仍以松散文件交付）。
- 重新构建带 `--package-self-test` 的 Linux Release Player 模板，补齐 RPM/DEB 或 tar.zst 输出；在 Linux 真机执行内容自检与 Vulkan 设备验收。
- 提供 macOS Player 模板与 macOS CI 节点，接入 `codesign`、Developer ID 与公证；再验证 `.app`/DMG 分发。
- 实现 Android APK/AAB 与 iOS `.app`/IPA 适配器：SDK/NDK/Gradle、Manifest/Info.plist、资源注入、生命周期与脚本宿主、签名/Provisioning。当前仅探测 SDK/NDK/Xcode 并在 `capabilities` 中报告缺口。
- Web/主机适配器；Lua/AOT 宿主适配。
- 商店上传、在线更新与差分包（独立扩展）。
