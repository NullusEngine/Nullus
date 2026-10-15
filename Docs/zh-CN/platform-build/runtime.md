# Game 运行时与资产清单

编辑器产出资产工件，`Game` 运行时消费它们。本页说明运行时如何启动、加载资产，以及命令行入口。

## 运行时入口

| 可执行文件 | 作用 |
| --- | --- |
| `Launcher` | 项目列表、创建与打开项目 |
| `Editor` | 编辑器与 Play 模式 |
| `Game` | 独立运行项目内容 |

三者共用同一套运行时模块：场景系统、资产数据库、渲染管线与脚本后端都是同一份实现。Game 不是编辑器的裁剪副本。

## Game 命令行

```text
Game [options] [project_path]
```

| 选项 | 说明 |
| --- | --- |
| `--backend <name>` / `-b <name>` | 指定图形后端 |
| `--renderdoc` / `--no-renderdoc` | 开关 RenderDoc 集成 |
| `--capture-after-frames <N>` | 第 N 次呈现后自动抓帧，并启用 RenderDoc |
| `--material-validation-output <path>` | 写材质校验读回图 |
| `--material-validation-summary <path>` | 写材质校验分析文本 |
| `--material-validation-after-frames <N>` | 材质校验在第 N 次呈现后执行 |
| `--material-validation-swapchain` | 抓取实际呈现的 Game 输出 |
| `--help` / `-h` | 帮助 |

`project_path` 可以是 `.nullus` 项目文件，也可以是项目目录。

```powershell
Game.exe TestProject.nullus
Game.exe --backend dx12 TestProject
```

## 资产清单

运行时启动时按项目设置解析运行时资产数据库：

1. 定位项目设置文件；
2. 由项目设置解析运行时工件数据库路径；
3. 加载运行时资产数据库；
4. 按需预热根资产（材质等）。

```text
项目设置 -> 运行时工件数据库 -> 资产引用解析 -> 资源预热
```

工件清单里记录了每个工件的目标平台与加载器，运行时只接受与当前平台匹配的工件。`ArtifactType` 覆盖模型、网格、材质、纹理、骨骼、蒙皮、动画片段、形变目标、Prefab、场景、着色器、音频、UI 文档、样式表、字体与本地化表。

模型导入生成的 Prefab 根会以子资源身份引用内嵌的网格与材质，使用子资源自身的源标识，使运行时预热能够在不改变资产或序列化格式的前提下到达这些材质。

## 运行时工件授权

运行时只允许读取被登记为授权的工件路径。未授权路径会被拒绝并记录，避免游戏进程访问工程内任意文件。授权在启动阶段按清单建立。

## 项目文件与会话

Game 运行时会读取项目文件中的启动期选项：

- `x_resolution` / `y_resolution` / `fullscreen`：窗口与呈现；
- `vsync`：垂直同步，与编辑器共享同一键；
- `graphics_backend`：首选后端；
- `start_scene`：启动场景；为空时回退到 `last_opened_scene`；
- `dev_build`：开发构建标记；
- `enable_threaded_rendering`：线程化渲染开关；
- `renderdoc_startup_capture_frames`：启动后自动抓帧。

完整键位说明见[编辑器与项目设置](../editor/settings.md#项目文件)。

## 打包与分发

当前仓库没有面向终端玩家的打包器。已有的是：

- 从编辑器菜单触发的构建动作（`Build game`、`Build game and run`、`Temporary build`）；
- 命名了目标可执行文件的项目文件键 `executable_name`；
- 平台化后的工件与运行时资产数据库。

缺失的是：

- 资产打包与压缩的最终容器格式；
- 依赖收集与去重；
- 安装包与更新通道；
- 目标平台无关的分发描述文件。

这些能力的实现需要先确定运行时加载的最终形态：如果运行时直接读取工程内的工件布局，就不需要新的容器格式；如果要求单文件分发，则需要先定义容器、索引与校验方式。这个取舍决定了打包器的接口形状，不应在实现时临时决定。

## 下一步

- [平台支持矩阵](platforms.md)：后端与平台状态。
- [功能状态矩阵](../roadmap/index.md)：打包能力的当前定位。
