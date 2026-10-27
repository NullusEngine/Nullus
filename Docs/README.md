# Nullus Documentation

This is the source of the Nullus handbook. The published GitHub Wiki is generated from this directory; edit here and let the sync workflow republish.

中文读者请从 [入门与手册索引](#中文) 开始。English readers can start at [Getting started](#english).

## 中文

| 章节 | 内容 |
| --- | --- |
| [认识 Nullus](zh-CN/start/overview.md) | 引擎定位、能力边界与平台支持 |
| [安装与快速入门](zh-CN/getting-started/build.md) | 环境准备、构建与创建第一个项目 |
| [使用编辑器](zh-CN/editor/workspace.md) | 工作区、面板、视口操作与设置 |
| [场景与对象](zh-CN/scene/objects.md) | GameObject、组件与生命周期 |
| [Prefab 工作流](zh-CN/prefab/workflow.md) | 创建、实例化、Stage、Override 与 Variant/Unpack 边界 |
| [资产与内容管线](zh-CN/assets/pipeline.md) | Asset Browser、导入、模型、纹理与工件 |
| [渲染、材质与光照](zh-CN/rendering/overview.md) | 渲染路径、相机、光源、材质与后端 |
| [大场景与资源管理](zh-CN/large-scenes/visibility-occlusion.md) | 可见性、遮挡剔除、LOD/HLOD 与驻留 |
| [游戏逻辑与脚本](zh-CN/scripting/overview.md) | C#、Lua、API 参考与调试 |
| [用户界面](zh-CN/ui/overview.md) | UI 运行时、文档格式与 UI Designer |
| [动画与角色](zh-CN/animation/workflow.md) | 导入、Workbench、Animator、GPU 蒙皮与验证 |
| [物理与交互](zh-CN/physics/roadmap.md) | 静态碰撞体、场景查询与动力学边界 |
| [音频、特效与媒体](zh-CN/audio-vfx/particles.md) | 粒子制作、CPU/GPU 播放与媒体状态 |
| [AI 与联网](zh-CN/ai-networking/roadmap.md) | AI 与联网现状及规划 |
| [平台、构建与发布](zh-CN/platform-build/packaging.md) | 平台矩阵、Build Profile、Cook 与 Player 打包 |
| [扩展与引擎架构](zh-CN/architecture/modules.md) | 模块、反射、序列化、任务系统与编辑器扩展 |
| [调试、性能与质量](zh-CN/quality/debugging.md) | 调试、测试、性能分析与抓帧 |
| [参考手册](zh-CN/reference/layout-cli.md) | 目录结构、命令行与构建选项 |
| [示例与发展路线](zh-CN/roadmap/minimal-example.md) | 最小完整示例、功能状态矩阵与阶段依赖 |

## English

| Chapter | Content |
| --- | --- |
| [Getting to Know Nullus](en/start/overview.md) | Positioning, capability boundaries, platform support |
| [Install and Quick Start](en/getting-started/build.md) | Environment, build, and your first project |
| [Using the Editor](en/editor/workspace.md) | Workspace, panels, viewport controls, and settings |
| [Scenes and Objects](en/scene/objects.md) | GameObjects, components, and lifecycle |
| [Prefab Workflow](en/prefab/workflow.md) | Creation, instantiation, Stage, overrides, and Variant/Unpack boundaries |
| [Assets and Content Pipeline](en/assets/pipeline.md) | Asset Browser, import, models, textures, and artifacts |
| [Rendering, Materials, and Lighting](en/rendering/overview.md) | Render paths, cameras, lights, materials, backends |
| [Large Scenes and Resource Management](en/large-scenes/visibility-occlusion.md) | Visibility, occlusion, LOD/HLOD, residency |
| [Gameplay and Scripting](en/scripting/overview.md) | C#, Lua, API reference, and debugging |
| [User Interface](en/ui/overview.md) | UI runtime, document format, and UI Designer |
| [Animation and Characters](en/animation/workflow.md) | Import, Workbench, Animator, GPU skinning, and validation |
| [Physics and Interaction](en/physics/roadmap.md) | Static colliders, scene queries, and dynamics limits |
| [Audio, Visual Effects, and Media](en/audio-vfx/particles.md) | Particle authoring, CPU/GPU playback, and media status |
| [AI and Networking](en/ai-networking/roadmap.md) | AI and networking status and plans |
| [Platforms, Builds, and Releases](en/platform-build/packaging.md) | Platform matrix, Build Profiles, cooking, and Player packaging |
| [Extending the Engine](en/architecture/modules.md) | Modules, reflection, serialization, jobs, editor extensions |
| [Debugging, Performance, and Quality](en/quality/debugging.md) | Debugging, testing, profiling, and captures |
| [Reference](en/reference/layout-cli.md) | Layout, command line, and build options |
| [Examples and Roadmap](en/roadmap/minimal-example.md) | Minimal complete example, feature matrix, and dependencies |

## Structure

| Path | Content |
| --- | --- |
| `nav.json` | Single navigation manifest: chapters, pages, titles, and language mapping |
| `zh-CN/` | Chinese source pages |
| `en/` | English source pages |
| `Screenshots/` | Images shared by both languages |
| `REVIEW_PATTERNS.md` | Internal checklist for reviewing larger engine changes |

Every page under `zh-CN/` and `en/` must be declared in `nav.json`, and a page's first heading must match the manifest title. The checker enforces both.

The public handbook consists of this index, `nav.json`, the declared bilingual pages, and shared screenshots. Local planning/design notes under `Plans/` and `Design/` are private working material: do not include them in public documentation commits or link to them from published pages.

## Tooling

```powershell
python Tools/Docs/nullus_docs.py check --strict # validate manifest, pages, links, anchors
python -m unittest discover -s Tools/Docs -p "test_*.py" -v
python Tools/Docs/verify_examples.py --dotnet Tools/Dotnet/windows/x64/dotnet.exe
python Tools/Docs/nullus_docs.py export --out out --prune
python Tools/Docs/nullus_docs.py diff           # show what a wiki sync would change
python Tools/Docs/nullus_docs.py publish        # export into a wiki clone and push
```

`Docs/` is the single source of truth. The wiki is generated, never edited by hand, and the exporter only deletes files it generated previously, tracked through `nullus-docs-manifest.json` in the wiki repository.

Validation evidence and remaining acceptance gates are recorded in [the acceptance report](../Tools/Docs/ACCEPTANCE.md). Compilation checks are separate from running the full desktop workflow.

## Writing conventions

- Mark each capability as implemented, partial, planned, or long-term, and keep platform and validation scope separate.
- Operational pages include prerequisites, steps, expected results, limits, and related references.
- API pages use real signatures from the current source.
- Planned features describe goal, current foundation, missing capability, and dependency. Never invent menus, settings, or APIs.
- Use relative links inside `Docs/`; the exporter rewrites them into wiki links and raw URLs.
