# 文档重构验收记录

本地验收日期：2026-10-14；远端发布验收日期：2026-10-15（Asia/Shanghai）。引擎源码基线：`96c005af`。首批 117 个文档及工具文件已在提交 `20272ea6` 推送到 `main`，双语 CI 说明在 `28fb5e61` 推送；本记录只覆盖实际执行的检查。

## 结论

文档静态检查、同步工具回归、示例编译、选定原生测试、实际 Wiki 发布与远端核对，以及 `main` 自动发布和手动重复发布通过。**整体验收尚未通过**：完整桌面操作清单尚未逐项执行，首次组合验证模式的退出等待尚未关闭。

## 结构与内容

- 19 章、41 个主题、82 个逐页对应的中英文正文，加入 CI 说明后的严格检查计 8,514 行。
- Prefab 已独立成章；旧 `zh-CN-scene-prefabs` / `en-scene-prefabs` Wiki 地址保留迁移页。
- 最小完整示例覆盖创建项目、场景、C#、OBJ 导入、Prefab 和可选 UI。
- 已按源码修正菜单路径、创建/挂载脚本、Prefab Stage 保存与未保存保护边界、禁用的 Asset Browser 菜单，以及 C#/Lua 不同的接口表面。
- 本轮未修改引擎实现；两份反射内部 Markdown 仅更新文档链接。
- 首次交付范围共 117 个文件：89 个新增、5 个修改、23 篇旧文档删除；后续验收更新仅涉及本报告及双语调试与测试页。现有工作树、历史工作树和临时目录没有纳入清理或提交。

## 已执行验证

证据文件均位于仓库的 `build/docs-acceptance/`，该目录不进入提交。

| 检查 | 结果 | 证据 |
| --- | --- | --- |
| 文档严格检查 | 19 章、82 页通过；本地链接、锚点、图片、显式源码链接及语言对应通过 | `docs-check.log` |
| Wiki 导出 | 95 个文件：94 个 Markdown 和 1 个生成清单 | `wiki-export.log` |
| Python 同步工具回归 | 28 项通过 | `docs-tests.log` |
| C# 示例 | 两种语言共 10 个代码块编译；零错误、零警告 | `examples.log` |
| Debug 原生测试构建 | 通过 | `native-build.log` |
| Debug Editor / Launcher 构建 | 通过 | `editor-build.log` |
| 选定功能回归 | 496 项中 490 项通过、6 项跳过；没有失败 | `native-tests.log`、`native-shader-tests.log` |
| Editor / DX12 有限帧运行 | 完成 30 帧，自然退出 0 | `editor-frame-smoke.json`、`editor-frame-smoke.exit` |
| Editor / DX12 画面回读 | 导入、实例创建、脚本激活和回读通过，自然退出 0；已查看输出图像 | `editor-readback-only.stdout.log`、`.txt`、`.png`、`.exit` |
| Wiki 实际远端内容 | 全部 95 个原始 Git 文件与本地导出逐字节一致；793 个内部链接通过 | `wiki-verification.json`、`wiki-remote-head.json` |
| Wiki 实际网页 | 首页、中英文侧栏、Prefab/英文 UI/编辑器工作区正文通过；实际点击侧栏、语言切换与旧地址迁移链接成功；共享截图已加载 | `wiki-browser.json`、`wiki-home.png`、`wiki-ui.png`、`wiki-editor.png` |
| 再次发布 | 没有额外提交，显示 `nothing to publish` | `wiki-publish-repeat.log` |
| 变更卫生 | 已跟踪文件 `git diff --check` 通过；新增文件无尾随空白；110 个仓库 Markdown 中无已删除文档链接 | `git-whitespace.log`、`change-audit.json` |
| GitHub Actions 本地配置 | YAML 解析及 9 项关键结构检查通过 | `workflow-check.json` |
| main 自动发布 | 两次 push 的检查/发布任务均成功；第二次使用 `GITHUB_TOKEN` 实际写入两页 Wiki | `ci-38664028743-jobs.json`、`ci-38664159252-jobs.json`、`ci-publish-changed.log` |
| 手动重复发布 | 检查/发布任务均成功，显示 `nothing to publish`，Wiki 无新增提交 | `ci-manual-run.json`、`ci-publish-manual.log` |

测试回归覆盖 Prefab、UI、EditorScript、ScriptRuntime、Launcher、项目启动设置、对象图与资产数据库。首轮因未显式设置 DXC 路径跳过的 13 项，在设置仓库 `Tools/DXC/1.9.2602/bin/x64/dxc.exe` 后全部通过。剩余 6 项跳过涉及 3 项符号链接条件、Autodesk FBX SDK、按需模型性能测试和未启用的 LuaPanda；跳过不计作通过。

工具回归验证页面重名、缺少翻译、空白页、锚点、图片/源码路径、链接转换、代码块保留、托管文件删除、只读差异预览、重复导出/发布、master 与其它默认分支、显式分支、历史与非生成文件保留、远端并发更新、拒绝推送、克隆失败和凭据脱敏。新增回归验证旧 CRLF 仓库在不同 `core.autocrlf` 策略下规范为 LF，后续发布不再产生提交。所有回归远端均为临时本地 bare 仓库，实际 GitHub 发布单独记录。

## Wiki 发布核对

- 远端：`https://github.com/NullusEngine/Nullus.wiki.git`，真实默认分支为 `master`。
- 本地首次发布核对提交：`2ace41de21aac136825e6449582c0e1133195ccd`；Actions 实际写入验收提交：`36780d014400a64a614d4afe3736da253b8a86d8`。后续报告状态修订由同一流程同步，当前远端对象核对结果另存 `wiki-verification.json`。
- 既有 Wiki Git 历史保留；旧正文地址生成双语迁移链接；仅删除生成清单管理的过时文件。
- `Home` 和 `_Sidebar` 均包含全部 82 个正文入口，每个正文的语言切换均指向对应页。
- 直接通过 `git cat-file --batch` 读取远端提交的原始对象，全体 95 个生成文件与本地导出相同。Windows 工作树和 `git archive` 的换行转换不用于判断原始对象差异。
- 独立无头 Chrome 实际加载 5 个 Wiki 页面，均 HTTP 200；页面可见，生成侧栏的 83 个链接含全部 82 个正文入口及首页。实际点击首页侧栏、两种方向的语言切换及旧 Prefab 地址的迁移链接，目标正文均可见。编辑器共享截图加载为 3840×2088。另通过 HTTP 直接读取首页、Prefab、英文 UI、旧 Prefab 地址与共享图片，均为 200。
- 再次实际执行 `publish` 返回无变化，远端提交未增加。`diff --wiki` 同样显示已同步。
- 首次本地发布使用本机 GitHub 身份。随后已单独验收 Actions 的 `GITHUB_TOKEN` 写入：日志确认 `2 wiki file(s) changed` 和成功推送至 `master`；手动再次运行确认 `nothing to publish`。

## GitHub Actions 远端验收

| 运行 | 触发与源码提交 | 结果 |
| --- | --- | --- |
| [38664028743](https://github.com/NullusEngine/Nullus/actions/runs/38664028743) | push，`20272ea6` | Validate handbook / Publish wiki 均成功；Wiki 已同步，无新提交 |
| [38664159252](https://github.com/NullusEngine/Nullus/actions/runs/38664159252) | push，`28fb5e61` | 两项任务均成功；`GITHUB_TOKEN` 更新两页，Wiki 生成 `36780d0` |
| [38664358685](https://github.com/NullusEngine/Nullus/actions/runs/38664358685) | workflow_dispatch，`28fb5e61`，publish=true | 两项任务均成功；无额外 Wiki 提交 |

用户明确确认直接提交 `main`，因此本次没有另建 PR。PR 的检查入口仍在工作流中配置，实际执行的是同一 Validate handbook 任务的 push 和手动入口；不声称另外执行过 PR 事件。

## 可复查命令

```powershell
python Tools/Docs/nullus_docs.py check --strict
python -m unittest discover -s Tools/Docs -p 'test_*.py' -v
python Tools/Docs/nullus_docs.py export --out build/docs-acceptance/wiki-export --prune
python Tools/Docs/nullus_docs.py diff --wiki build/docs-acceptance/wiki-remote
python Tools/Docs/verify_examples.py --dotnet Tools/Dotnet/windows/x64/dotnet.exe
Tools/BuildCleanEnvironment.cmd cmake --build build --config Debug --target NullusUnitTests -- /m:1
Tools/BuildCleanEnvironment.cmd cmake --build build --config Debug --target Editor Launcher -- /m:1
```

直接运行本地测试时须将 `build/runtime/Debug` 与 `App/Win64_Debug_Runtime_Shared` 加入子进程 PATH；可执行文件位于 `build/app-binaries/Debug/NullusUnitTests.exe`，不会部署到 App 目录。

```text
--gtest_filter=Prefab*.*:UIDocument*.*:EditorScriptTests.*:ScriptRuntimeTests.*:Launcher*.*:ProjectCreationWizardValidationTests.*:ProjectLaunchOptionsTests.*:SceneObjectGraphSerializationTests.*:AssetDatabaseFacadeTests.*:EditorAssetDatabaseTests.*
```

## 运行证据与未关闭问题

独立项目来自 EmptyProject 模板内容，位于 `build/docs-acceptance/smoke-project`。导入的 OBJ 与最小完整示例相同。当前重建的 Debug Editor 在 DX12 上成功导入 OBJ、生成网格/材质/Prefab、建立实例，并激活新编译的 C# 程序集。独立画面回读为 1875×1007，`readbackStatus=success`，已查看 PNG 中的三角形。

首轮同时启用画面回读和帧计数模式。回读入口主动关闭窗口，因而帧计数显示不足；这是验收命令的组合错误，不作为引擎帧采样失败证据。首轮进程退出阶段仍存活，主线程栈停在 `ResourceLifetimeRegistry::Release` 的互斥锁获取，由 `ResidentPrefabPreviewResources` 中材质句柄析构触发。栈已保存为 `editor-smoke-stacks.log`；采集后仅结束了该次启动的 PID，退出码为人工终止的 `-1`。

随后将两种模式拆开执行：有限帧模式完成 30 帧后自然退出 0，独立画面回读模式同样自然退出 0。这证明两条独立冒烟路径通过；组合路径的退出等待没有完成根因修复或重复性验证，仍保留为未关闭问题。本轮限于文档及发布工具，没有修改引擎实现，也没有将这组数据作为性能结论。

## 完整桌面工作流待执行清单

以下步骤尚未逐项点击执行，不能用原生测试或自动冒烟结果代替。执行时应按双语“最小完整示例”记录操作、保存的文件、预期结果和日志。

| 步骤 | 验收证据 | 当前状态 |
| --- | --- | --- |
| 从 Launcher 创建全新 Empty Project 并打开 | 项目目录、场景与默认设置存在，Editor 打开 | 待执行 |
| 创建对象、添加组件、修改 Transform，保存并重开场景 | 层级、组件与字段持久化 | 待执行 |
| 从 Asset Browser 导入 OBJ 并修改源文件触发重导入 | 导入产物与场景实例更新，无引用丢失 | 自动导入/实例创建已通过；手工导入/重导入待执行 |
| 创建并挂载 C# Behaviour，进入/退出 Play | 日志、生命周期和对象修改符合文档 | 编译/程序集激活已通过；挂载及 Play/Stop 待执行 |
| 创建 Prefab、实例化、打开 Stage、修改并保存 | 源资产和实例更新，无意外覆盖场景 | 待执行 |
| 修改实例字段、检查和恢复 Override | Inspector 与保存后重开的结果一致 | 待执行 |
| 创建 UI 文档、Canvas，使用 UI Designer 及脚本交互 | UI 可见，文本/事件更新，保存重开一致 | 待执行 |
| 按排错步骤处理一次脚本编译错误并恢复 | Console 和编译诊断位置正确 | 待执行 |

## 关闭整体验收的门槛

1. 完成上述完整桌面工作流，并对首轮组合模式退出等待做独立问题归属与后续验收。
2. 若需要单独验收 PR 事件，在后续真实文档 PR 中核对同一 Validate handbook 任务；本次经用户批准直接提交 `main`。

工作流的本地检查已确认 `main` 路径触发、手动入口、发布任务 `contents: write`、`GITHUB_TOKEN`、检查任务依赖与串行执行。远端 push 自动发布、真实 Wiki 写入以及手动重复发布均已通过，不再列为未验收项。

Windows / DX12 的证据不能推出 Linux、macOS 或其它图形后端已经通过。10 个 C# 示例的编译检查也不等于全部 API 示例、Lua 调试器或桌面脚本断点已经运行验证。
