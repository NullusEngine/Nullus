# 模块结构

Nullus 按“运行时库 + 项目程序 + 工具”三层组织。运行时库不依赖项目程序，项目程序共享同一份运行时库。

## 仓库布局

| 目录 | 内容 |
| --- | --- |
| `Runtime/` | 引擎运行时模块 |
| `Project/` | 可执行程序：Launcher、Editor、Game |
| `Managed/` | C# 侧程序集与代码生成工具 |
| `Tools/` | 构建、生成、调试与性能工具 |
| `App/` | 输出目录与随引擎部署的资源 |
| `Tests/` | 单元测试与性能测试 |
| `Docs/` | 本手册的源文件 |
| `App/Assets/Templates/` | 项目模板，Launcher 创建工程时复制其 `content/` |

## 运行时模块

| 模块 | 职责 |
| --- | --- |
| `Base` | 反射、对象模型、事件、JobSystem、TaskGraph、线程、时间、日志与性能标记 |
| `Core` | 资产标识与元数据、资产数据库、工件写入与解析、文件系统、资源管理与序列化基础 |
| `Engine` | 场景系统、组件、引擎侧资产、渲染桥接、引擎设置、UI 运行时与 AI |
| `Math` | 向量、四元数、矩阵与几何类型 |
| `Platform` | 窗口、输入、光标、对话框与进程调用 |
| `Rendering` | RHI、FrameGraph、着色器编译与 ShaderLab、渲染资源、编辑器 UI 桥接 |
| `Scripting` | 语言无关脚本运行时与 C#/Lua 后端 |
| `UI` | 编辑器界面框架与 Dear ImGui 扩展 |

依赖方向是单向的：`Base` 不依赖 `Engine`，`Engine` 不依赖 `Editor`。`Project/*` 可以依赖 `Runtime/*`，反之不成立。

## 项目程序

| 程序 | 职责 |
| --- | --- |
| `Launcher` | 项目列表、创建向导、模板管理、项目设置编辑 |
| `Editor` | 编辑器外壳、面板、命令与撤销、资产工作流、Play 模式 |
| `Game` | 独立运行时：项目解析、资产清单加载、渲染循环与脚本驱动 |

Editor 与 Game 共享引擎代码，因此编辑器视口与运行时画面来自同一条渲染管线。

## C# 程序集

| 程序集 | 作用 |
| --- | --- |
| `Nullus.Managed` | 运行时 C# API：`Object`、`Component`、`Behaviour`、`GameObject`、`Transform`、`Camera`、`Light`、`Time`、`Input`、`SceneManager`、`Debug` 与数学类型 |
| `Nullus.EngineScripts` | 引擎自带脚本示例 |
| `Nullus.GameScripts` | 项目脚本，进入玩家构建 |
| `Nullus.GameScriptsEditor` | 项目编辑器脚本，只在编辑器进程加载 |
| `Nullus.Editor` | 编辑器脚本 API：`ComponentEditor`、`EditorWindow`、`EditorGUILayout`、`SerializedObject` |
| `Nullus.ScriptGenerator` | 由 API 清单生成 C# 绑定 |
| `Nullus.ScriptAnalysis` | 分析项目脚本，产出行为清单与字段描述 |

## 工具

| 工具 | 作用 |
| --- | --- |
| `Tools/MetaParser` | 扫描 C++ 声明，生成反射注册代码与脚本 API 清单 |
| `Tools/DXC` | 着色器编译器 |
| `Tools/ReflectionTest` | 反射链路的轻量冒烟工具 |
| `Tools/Performance` | 资产预热与性能辅助脚本 |
| `Tools/RenderDoc` | RenderDoc 启动、自检与分析脚本 |
| `Tools/Tracy` | 性能分析器部署目录 |
| `Tools/Debug` | Visual Studio 与 VS Code 扩展 |
| `Tools/StackDump` | 崩溃栈转储工具 |
| `Tools/BuildCleanEnvironment` | 清理构建子进程的重复 `Path`/`PATH` |
| `Tools/Docs` | 本手册的校验与 Wiki 同步工具 |

## 生成代码

MetaParser 在构建期产出代码到：

- `Runtime/<Module>/Gen/`
- `Project/<Target>/Gen/`

这些目录下的文件是构建产物，不应手工编辑。修改源声明后重新构建即可。

## 目录与产物的对应关系

| 路径 | 是否入库 | 内容 |
| --- | --- | --- |
| `Runtime/`、`Project/`、`Managed/`、`Tools/`、`Docs/` | 是 | 源码与文档 |
| `Runtime/*/Gen/`、`Project/*/Gen/` | 否 | 生成的反射与绑定代码 |
| `build/` | 否 | CMake 构建树 |
| `App/Win64_*`、`App/Linux_*` | 否 | 部署的可执行文件与运行库 |
| `Library/`（项目内） | 否 | 导入结果、缓存与 IDE 工作区 |

## 下一步

- [反射与序列化](reflection-serialization.md)：生成代码驱动的两条数据通路。
- [任务系统与线程模型](jobs-threading.md)：运行时模块的并发结构。
