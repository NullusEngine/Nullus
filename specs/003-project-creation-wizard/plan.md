# Implementation Plan: Project Creation Wizard

**Branch**: `003-project-creation-wizard` | **Date**: 2026-04-15 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/003-project-creation-wizard/spec.md`

## Summary

将 Launcher 从 Editor 可执行程序中拆分为独立的 Launcher 可执行程序，并新增项目创建向导功能。Launcher 作为项目管理入口，提供模板选择、项目创建配置（后端、分辨率等）和最近项目列表。创建或选择项目后，Launcher 通过进程抽象层启动 Editor 并传递命令行参数。单独启动 Editor 必须传入项目路径参数。

2026-04-17 追加范围：在既有 Launcher/Editor 拆分和项目创建向导基础上，将 Launcher 项目列表页和新建项目页重构为 Unity Hub 3.3 风格的关键布局。保留 Nullus 品牌和已实现功能，未支持的 Hub 类入口仅静态或禁用展示。Launcher 窗口改为更大的默认尺寸、可调整大小，并设置最小尺寸；项目表格和新建项目三栏布局需要随窗口尺寸变化保持稳定。

2026-04-17 追加待办：进一步收窄 Launcher 范围，只保留 Projects/Installs 两个入口；Installs 继续扩展为可维护多个 Editor/Engine 可执行文件条目的版本列表，并用文件最后修改日期作为版本号显示；创建项目页移除左上角 Back，并要求选择一个可用编辑器版本后才允许创建；本地化资源收敛为单个表文件，避免在 C++ 中硬编码中文 UTF-8 文案；DX12 resize 黑边问题先按渲染侧证据定位根因，再决定修复点。

## Technical Context

**Language/Version**: C++20
**Primary Dependencies**: NLS_Engine (静态库), ImGui, GLFW, portable-file-dialogs
**Storage**: 文件系统（INI 格式的 .nullus 项目文件, JSON 格式的模板元数据）
**Testing**: CTest + NullusUnitTests，手动验证（Launcher/Editor UI 行为）
**Target Platform**: Windows (DX12/Vulkan/OpenGL/DX11), Linux (Vulkan/OpenGL), macOS (Metal)
**Project Type**: Desktop application (游戏引擎工具链)
**Performance Goals**: Launcher 启动 < 2 秒，Editor 进程启动 < 2 秒
**Constraints**: Launcher 和 Editor 构建到同一输出目录，Launcher 需通过相对路径找到 Editor；Launcher UI 使用现有 ImGui/PanelWindow 绘制，不引入新的 UI 框架；窗口默认可调整大小并使用现有 `WindowSettings::minimumWidth/minimumHeight`；Launcher 用户可见中文文案放在外部本地化资源文件，不在 C++ 中硬编码 UTF-8 字符串；DX12 resize 修改必须有根因证据
**Scale/Scope**: 3 个可执行目标（Launcher, Editor, Game），1 个项目模板（v1）

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

### Pre-Design Check

| Gate | Status | Evidence |
|------|--------|----------|
| I. Spec-First | PASS | spec bundle 存在于 `specs/003-project-creation-wizard/` |
| II. Validation Matches Subsystem | PASS | 变更涉及 Project/ (UI 行为) 和 Runtime/Platform/ (新抽象层)，需手动 UI 验证 + 构建验证 |
| III. Generated Code / Backend Boundaries | PASS | 不涉及 `Runtime/*/Gen/` 文件，不修改渲染管线或 RHI 后端 |
| IV. Incremental Delivery | PASS | 分阶段实施：先拆分 → 再加向导 → 再加模板 |
| V. Product Runtime Preservation | PASS | Editor 和 Game 在每个阶段都可独立运行 |

### Post-Design Check

| Gate | Status | Evidence |
|------|--------|----------|
| I. Spec-First | PASS | plan.md、research.md、data-model.md、contracts/ 完整 |
| II. Validation Matches Subsystem | PASS | quickstart.md 定义了每阶段的验证步骤 |
| III. Generated Code / Backend Boundaries | PASS | 设计不触及生成代码或 RHI 后端边界 |
| IV. Incremental Delivery | PASS | 三阶段实施，每阶段独立可验证 |
| V. Product Runtime Preservation | PASS | Editor 保持可运行（命令行参数不变），Game 不受影响 |

## Project Structure

### Documentation (this feature)

```text
specs/003-project-creation-wizard/
├── plan.md              # This file
├── spec.md              # Feature specification
├── research.md          # Phase 0 output - design decisions
├── data-model.md        # Phase 1 output - entities and validation
├── quickstart.md        # Phase 1 output - build and verify guide
├── contracts/           # Phase 1 output - interface contracts
│   ├── launcher-editor-cli.md
│   ├── project-template-format.md
│   └── process-platform-abstraction.md
└── tasks.md             # Phase 2 output (/speckit.tasks)
```

### Source Code (repository root)

```text
# 新增文件
Project/Launcher/
├── CMakeLists.txt                # 新 CMake 目标
├── Main.cpp                      # Launcher 入口点
├── Core/
│   ├── Launcher.h                # 从 Editor 迁移（修改）
│   ├── Launcher.cpp              # 从 Editor 迁移（重构：添加向导状态）
│   ├── LauncherLocalization.h    # 新增 - Launcher 本地化 key/加载接口
│   ├── LauncherLocalization.cpp  # 新增 - 单文件 UTF-8 本地化表解析与回退
│   ├── LauncherSettings.h        # 新增 - Launcher 安装列表/默认版本持久化
│   ├── LauncherSettings.cpp      # 新增
│   ├── ProjectCreationWizard.h   # 新增 - 创建向导面板
│   ├── ProjectCreationWizard.cpp # 新增 - 向导 UI 和逻辑
│   └── TemplateManager.h         # 新增 - 模板发现和加载
│   └── TemplateManager.cpp       # 新增
Assets/Templates/
└── EmptyProject/
    └── template.json             # 空项目模板元数据
App/Assets/Localization/Launcher/
└── strings.csv                   # 新增 - 单表本地化资源
Runtime/Platform/Process/
├── Process.h                     # 新增 - 进程抽象接口
├── Process.cpp                   # 新增 - 平台实现

# 修改文件
Project/CMakeLists.txt            # 添加 add_subdirectory("Launcher")
Project/Editor/Main.cpp           # 移除 Launcher 逻辑，添加无参数错误处理
Project/Editor/Core/              # 删除 Launcher.h/.cpp（迁移到 Launcher 项目）
```

**Structure Decision**: 新增 `Project/Launcher/` 作为与 `Project/Editor/` 和 `Project/Game/` 平级的独立可执行目标。进程抽象放在 `Runtime/Platform/Process/` 以保持平台抽象层的一致性。模板数据放在 `Assets/Templates/` 与引擎资源目录并列。

## Implementation Phases

### Phase A: Launcher/Editor 拆分

1. 创建 `Project/Launcher/` 目录结构和 CMakeLists.txt
2. 将 `Project/Editor/Core/Launcher.h/.cpp` 迁移到 `Project/Launcher/Core/`
3. 编写 `Project/Launcher/Main.cpp`（从当前 `Project/Editor/Main.cpp` 提取 Launcher 流程）
4. 修改 `Project/Editor/Main.cpp`：移除 Launcher 引用，添加无参数错误处理
5. 实现 `Runtime/Platform/Process/Process.h/.cpp`（跨平台进程启动）
6. 修改 Launcher 使用 `Process::Launch()` 启动 Editor
7. 更新 `Project/CMakeLists.txt` 添加 Launcher 子目录
8. 构建验证：Launcher.exe 和 Editor.exe 都能独立构建和运行

### Phase B: 项目创建向导

1. 创建 `Project/Launcher/Core/ProjectCreationWizard.h/.cpp`
2. 实现向导 UI（模板选择、项目信息、渲染设置）
3. 修改 Launcher 状态机：添加 "创建向导" 状态
4. 实现项目创建逻辑（目录结构 + .nullus 文件写入用户设置）
5. 实现输入验证（项目名、路径冲突）
6. 向导完成后自动启动 Editor
7. UI 验证：向导风格与 Launcher ALTERNATIVE_DARK 一致

### Phase C: 模板系统

1. 创建 `Assets/Templates/EmptyProject/template.json`
2. 创建 `Project/Launcher/Core/TemplateManager.h/.cpp`
3. TemplateManager 扫描模板目录、加载 template.json
4. 向导 UI 集成 TemplateManager，动态渲染模板卡片
5. 模板内容复制逻辑（v1 空模板无内容）
6. 验证：模板加载、UI 显示、项目创建正确性

### Phase D: Unity Hub 风格 Launcher 重构

1. 扩展 `LauncherTheme.h` 中的 Hub 风格尺寸、禁用态颜色、分隔线和工具函数，供主界面与创建向导共用。
2. 修改 `Launcher::SetupContext()`：默认窗口尺寸设为更接近 Hub 工具窗口的比例，启用 `resizable`，设置 `minimumWidth/minimumHeight`，并保持现有轻量 Launcher 上下文。
3. 修改 `LauncherPanel`：每帧使用当前窗口尺寸设置主 Panel 大小，实现左侧品牌栏、导航栏、项目列表标题区、搜索/操作区和表格区。
4. 为未支持导航项绘制静态或禁用状态，不触发功能；可用入口保留项目列表与新建项目。
5. 调整项目列表表格为响应式列宽，名称列优先伸缩，修改时间、后端和操作列保持稳定最小宽度。
6. 修改 `ProjectCreationWizard`：保持三栏结构但改为 Unity Hub 截图式比例，底部操作栏固定；模板列表和右侧设置栏在默认、最小和放大尺寸下不重叠、不裁切关键按钮。
7. 验证：构建 `Launcher` 目标，并在默认尺寸、最小尺寸、放大尺寸下手动检查项目页和新建项目页。

### Phase E: Sidebar 简化、Installs 首版、本地化与 DX12 resize 排查

1. 先补测试：Launcher 本地化加载/缺失 key 回退、Launcher 设置中 engine executable 路径持久化、Hub 导航仅暴露 Projects/Installs。
2. 引入 `LauncherLocalization`：用枚举 key 访问文案，从 `Assets/Localization/Launcher/strings.csv` 单表加载所有 locale；缺失资源时回退到英文/ASCII key，避免启动失败。
3. 引入 `LauncherSettings`：读写 `launcher.ini` 中的安装列表与默认版本，验证路径存在，并在运行时把最后修改日期格式化为版本号。
4. 修改 `LauncherPanel` 状态机：导航只显示 Projects 与 Installs，移除账号头像及所有未实现入口；Installs 页提供多版本列表、默认选择、添加/删除已有可执行文件和错误提示。
5. 修改 `ProjectCreationWizard`：移除左上角 Back 按钮，保留底部 Cancel 返回项目列表；新增编辑器版本选择，并在无可用版本时弹窗拦截创建。
6. DX12 resize 调查：复现问题，记录窗口、framebuffer、ImGui draw data、swapchain/backbuffer 尺寸；若 RenderDoc 捕获可用，则用 `renderdoc_runner.py` + `rdc_analyze.py` 检查 resize 后帧的 render target/viewport/scissor；确认根因后只改对应层。
7. 验证：运行新增单元测试、构建 Launcher、扫描 Launcher 源码确认无硬编码中文 UTF-8 字节串、手动验证 Projects/Installs/Create Project 和 DX12 resize。

## Complexity Tracking

无宪法违规需要记录。
