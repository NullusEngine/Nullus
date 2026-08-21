# Nullus

<p align="center">
    <img src="NullusLogo.png" width="320" alt="Nullus Engine logo">
</p>

<p align="center">
    <strong>面向实时 3D 创作的 C++20 原生引擎</strong><br>
    从场景、资产和脚本，到 Editor、运行时与多后端渲染，使用同一条引擎主链路。
</p>

<p align="center">
    <a href="./README.md">中文</a> | <a href="./README.en.md">English</a>
</p>

![License Source Available](https://img.shields.io/badge/License-Source%20Available-blue.svg)
![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C)
![CMake >=3.18](https://img.shields.io/badge/CMake-%E2%89%A53.18-064F8C)
![.NET 8](https://img.shields.io/badge/.NET-8.0-512BD4)
![Build Windows](https://github.com/NullusEngine/Nullus/workflows/Build%20Windows/badge.svg)
![Build Linux](https://github.com/NullusEngine/Nullus/workflows/Build%20Linux/badge.svg)
![Build macOS](https://github.com/NullusEngine/Nullus/workflows/Build%20MacOS/badge.svg)

![Nullus Editor: Sponza scene, hierarchy, inspector, transform gizmo, and Asset Browser](Docs/Screenshots/editor.png)

## 为实时创作而生

Nullus 是一个持续演进的 C++ 3D 引擎。它将原生运行时、项目启动器、可视化 Editor、资产导入、场景序列化与游戏脚本放在同一套工作流中：既能直接在 Editor 中组织和运行场景，也保留了面向引擎研发的清晰模块边界与显式 RHI 架构。

### 现代且可演进的渲染主线

- **显式多后端 RHI**：Windows 的 DX12 与 Linux/WSL 的 Vulkan 1.1 是当前已验证的运行时主线；macOS 的 Metal 原生后端和构建目标也已纳入工程。
- **FrameGraph 驱动的场景渲染**：前向与延迟路径覆盖 GBuffer、clustered lighting、decal、skybox、透明物体和编辑器视图输出，资源依赖与状态转换由图管理。
- **统一 HLSL 工作流**：HLSL 是唯一维护的着色器源，DXIL、SPIR-V 与反射数据由编译产物生成，材质、Pass 和 Object 的绑定空间保持一致。
- **面向大场景的渲染优化**：已有可见性收集、绘制命令缓存、实例化合批、静态网格 LOD 构建与运行时选择；DX12 上的 HZB 遮挡剔除按设备能力保守启用。

### 可视化的材质与 ShaderLab 工作流

- **内置 StandardPBR 基线**：引擎自带 `StandardPBR` ShaderLab 资源；模型导入会将材质、纹理和子资源接入这一套 PBR 内容链路。
- **由着色器定义材质面板**：ShaderLab 属性支持 Float、Int、Color、Vector 与 Texture。材质还可管理关键词集合和渲染队列覆盖，使参数、变体与渲染状态保持在同一资源对象中。
- **Editor 内直接编辑和预览**：Material Editor 可选择或拖入材质与着色器，按当前着色器生成参数面板，并提供预览与重置操作；Asset Browser 为材质提供 GPU 缩略图预览。
- **资源化与安全重载**：材质作为可异步加载和预热的资源工件管理。ShaderLab 重载先校验请求的变体，成功后更新关联材质并使受影响的管线代次失效。

### 从源文件到可用资产

- **项目级 Asset Browser**：资源浏览、搜索、拖放、缩略图、预览、对象引用选择和资源目录贯穿 Editor 工作流。
- **模型与材质导入**：支持 glTF / FBX 场景和模型导入，生成网格、材质、纹理与子资源；静态网格可配置 LOD 导入、简化和打包。
- **PBR 内容管线**：标准 PBR 材质、纹理颜色空间处理、Mip 与压缩构建、材质资源和 ShaderLab 资产导入已经接入资产数据库。
- **Prefab 与场景资产**：可从场景对象创建 Prefab Variant，并通过反射式对象图序列化保存场景、对象关系和组件字段。

### 一套对象模型，两种脚本语言

- **C# 与 Lua 并行可用**：CoreCLR 与 Lua 后端通过同一套原生脚本 ABI 和 API 清单连接 `GameObject`、`Transform`、`Component`、`Camera`、`Light`、输入与时间系统。
- **熟悉的组件化生命周期**：`Awake`、`OnEnable`、`Start`、`Update`、`OnDisable` 和 `OnDestroy` 让脚本逻辑直接贴合场景对象。
- **迭代不离开 Editor**：保存 C# 脚本后由 Asset Watcher 增量构建，成功程序集会在帧边界热重载并保留字段；失败时继续运行上一份有效程序集。
- **面向调试的工具链**：可生成 VS Code / Visual Studio 配置，附加到正在运行的 Editor，支持 C# 断点和混合调试工作流。

### 让创作流程留在 Editor 内

- **项目启动器**：创建、打开和管理项目，统一进入对应 Editor。
- **可视化场景编辑**：Hierarchy、Scene View、Game View、Inspector、组件选择器、播放控制、变换 Gizmo 与命中代理选取组成日常编辑闭环。
- **资源即时可见**：模型、材质、贴图、Prefab 与脚本在 Asset Browser 中拥有分类、图标和异步预览能力。
- **材质调试在场景旁完成**：从资源选择、属性编辑到球体预览均在 Editor 内完成，不需要离开当前项目上下文。
- **性能可观察**：共享 Profiling facade 可接入 Tracy 或 Editor Timeline Profiler，渲染线程与 RHI 线程具备统一的 CPU/GPU 标记入口。

### 原生引擎基础设施

- `Scene / SceneManager / GameObject / Component` 组件化场景系统。
- MetaParser 代码生成反射，驱动类型注册、Inspector、对象序列化和脚本 API 清单。
- 原生 JobSystem 提供依赖、批处理、并行 for 和后台任务调度。
- Runtime 分为 `Base`、`Core`、`Engine`、`Math`、`Platform`、`Rendering`、`Scripting` 与 `UI`，Editor 与 Game 共享运行时主链路。

## 项目启动器

<p align="center">
    <img src="Docs/Screenshots/launcher.png" width="760" alt="Nullus Hub project launcher">
</p>

从 Launcher 创建或打开项目后，即可进入上方展示的 Editor 工作区。

## 快速开始

### 环境要求

- Git
- CMake 3.18 及以上
- 支持 C++20 的编译器
- Python 3.8 及以上（依赖准备脚本）
- .NET SDK 8.0.408：CMake 会在缺失时下载安装到仓库内，不修改系统 `PATH`

### 获取代码并准备依赖

```bash
git clone https://github.com/NullusEngine/Nullus.git
cd Nullus
git submodule update --init --recursive
```

在 Windows：

```powershell
.\SetupDependencies.bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug --target Editor
```

在 Linux：

```bash
./SetupDependencies.sh
./build_linux.sh debug
```

在 macOS：

```bash
./SetupDependencies.sh
./build_macos.sh debug
```

构建完成后运行 `Editor` 开始创建项目和场景；`Game` 使用同一套运行时主链路运行项目内容。

若构建宿主环境报出 `MSB6001` 且提示 `Path` / `PATH` 重复，可使用仅影响构建子进程的环境清理入口：

```powershell
.\Tools\BuildCleanEnvironment.cmd cmake --build build --config Debug --target Editor
```

### 图形后端说明

- Windows 默认选择 `DX12`。
- Linux / WSL 默认选择 `Vulkan`；首次在 WSL 中可使用 `./build_wsl_vulkan.sh --install-deps` 安装脚本所需依赖，再运行 `./build_wsl_vulkan.sh`。
- macOS 构建默认选择 `Metal`。
- DX11 与 OpenGL 代码仍在迁移中，当前被显式限制为非主线运行时后端，不应作为项目交付目标。

## 深入了解

- [多后端 RHI 架构](Docs/Rendering/RHIMultiBackendArchitecture.md)
- [HLSL 与资源绑定约定](Docs/Rendering/ShaderConventions.md)
- [HZB 遮挡剔除验证](Docs/Rendering/HZBOcclusionValidation.md)
- [C# / Lua 脚本 API](Docs/Scripting/ScriptingApi.zh-CN.md)
- [C# 脚本调试指南](Docs/Scripting/ScriptDebugging.zh-CN.md)
- [反射工作流](Docs/Reflection/ReflectionWorkflow.zh-CN.md)
- [原生 JobSystem](Docs/Jobs.md)
- [测试说明](Docs/Testing.md)

## 跨平台与 CI

工程提供 Windows、Linux 与 macOS 的构建脚本，GitHub Actions 覆盖三平台构建，并继续执行反射和单元测试相关目标。具体构建选项与依赖准备方式见各平台脚本和上方文档。

## 参与贡献

欢迎通过 Issue、建议或 Pull Request 参与。提交前请阅读 [CONTRIBUTING.md](./CONTRIBUTING.md) 与 [CLA.md](./CLA.md)。

## License

Nullus Engine 使用自定义 Source-Available License。允许查看和修改源码、商业使用以及发布使用 Nullus Engine 制作的游戏；禁止分发引擎源码或使用本项目制作竞争引擎。详情见 [LICENSE](./LICENSE)。
