# Player 打包与分发

`NullusBuild` 是无界面的打包入口，编辑器构建动作复用同一条流水线。它收集场景依赖、按目标平台 Cook 内容、组装 Player、写出 `.npak` 并校验成品。桌面适配器已有实现，安装器、自动更新和移动平台交付仍未完成。

## 准备 Player 与项目

先完成[环境准备与构建](../getting-started/build.md)，并在已有构建目录构建 `Game` 和 `NullusBuild`：

```powershell
cmake --build build --config RelWithDebInfo --target Game NullusBuild
build/bin/RelWithDebInfo/NullusBuild.exe capabilities
```

Windows 模板使用 `App/Win64_RelWithDebInfo_Runtime_Shared`，需要 `Game.exe`、运行库与引擎资源；C# 项目还需要对应脚本宿主。项目须有可加载的启动场景。保留源资产的 `.meta` 文件，确保 GUID 引用稳定。

## BuildProfiles.json

在项目根目录放置版本化配置。下面是 Windows DX12 示例；将启动场景替换为实际项目路径：

```json
{
  "schemaVersion": 1,
  "defaultProfile": "windows-shipping",
  "profiles": [
    {
      "id": "windows-shipping",
      "displayName": "Windows DX12",
      "productName": "MyGame",
      "executableName": "MyGame",
      "version": "0.1.0",
      "mode": "Shipping",
      "startScene": "Assets/Scenes/Main.scene",
      "target": {
        "platform": "windows",
        "architecture": "x64",
        "graphicsBackend": "dx12",
        "textureFormatPolicy": "target-native",
        "maxTextureSize": 2048,
        "scriptBackends": ["csharp", "lua"]
      },
      "outputFormat": "zip"
    }
  ]
}
```

| 字段 | 使用方式 |
| --- | --- |
| `defaultProfile` / `--profile` | 默认配置与本次选择的配置 ID |
| `startScene` | 项目相对路径，作为依赖收集根和 Player 启动场景 |
| `keepAssetPaths` | 为运行时动态加载的资源显式保留路径、目录前缀或 GUID |
| `assetPackSelections` | 在基础包之外选择资源包及变体 |
| `target.maxTextureSize` | 纹理尺寸上限；0 保留源尺寸，Cook 按二次幂策略处理 |
| `target.textureFormatPolicy` | Shipping 使用 `target-native`；`passthrough` 仅用于验证配置 |
| `playerTemplatePath` / `--player` | 预构建 Player 模板目录 |
| `signingProfile` | 签名凭据引用；缺少签名配置时输出保持未签名 |

`Shipping` 要求完整内容和兼容模板；包含自定义原生模块的 Shipping 配置当前会被拒绝，需要另行提供相应的源码构建流程。不要把仅在编辑器缓存中可用的资源当作已交付内容。

## 构建、检查与运行

```powershell
build/bin/RelWithDebInfo/NullusBuild.exe build --project MyGame/MyGame.nullus --profile windows-shipping --player App/Win64_RelWithDebInfo_Runtime_Shared --output Dist --validate-launch
build/bin/RelWithDebInfo/NullusBuild.exe inspect --package Dist/MyGame
build/bin/RelWithDebInfo/NullusBuild.exe validate --package Dist/MyGame
```

将 `Dist/MyGame` 替换为构建报告给出的实际成品目录或 ZIP。退出码为 0 表示该命令成功，1 表示构建或校验失败，2 表示参数错误。成功报告写入 `BuildReport.json`；失败报告记录阶段和诊断，不应把残留目录作为成功交付。

成品包含产品可执行文件、运行库、`Player.info`、运行时清单和 `base.npak` 等内容包。C# 脚本从项目 `Assets/**/*.cs` 编译项目专用程序集；不会把模板中的样本游戏脚本当作项目代码。`Player.info` 记录目标、引擎版本、内容包和输入哈希，运行时检查兼容性后挂载资源。

在成品目录运行产品程序的 `--package-self-test` 可校验资源包、清单、启动场景和脚本载荷。该检查不替代可见画面、真实输入、脚本行为或 GPU 功能验收。按目标平台分别测试，并确认成品脱离项目 `Library/` 后仍可启动。

## 平台边界

| 目标 | 当前范围 |
| --- | --- |
| Windows x64 | 桌面 Player 组装、内容包、ZIP 与校验；图形后端仍需设备验收 |
| Linux x64 | 预构建 ELF 模板，ZIP/目录输出、可执行权限和 `$ORIGIN` 路径处理；可在其他宿主组装 |
| macOS x64 / arm64 | 需要 macOS 宿主和匹配模板；ZIP/目录输出，Developer ID 与公证尚未接入 |
| Android / iOS / Web | 能力声明用于报告缺口，不代表已有可交付打包适配器 |

非 Linux 宿主组装 Linux 包时会以 `build-launch-cross-host` 警告跳过成品启动自检。Linux 配置字段接受的后端不代表当前 Player 都可运行；交付以 Vulkan 为主，OpenGL 仍受运行时限制。macOS 构建成功不能替代 Metal 设备执行、签名或公证验证。安装器、商店上传与自动更新不在当前输出范围内。

## 相关内容

- [平台支持矩阵](platforms.md)：构建、后端与设备验证的区别。
- [Game 运行时与资产清单](runtime.md)：启动、授权与资源加载。
