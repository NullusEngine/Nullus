# Tasks: Project Creation Wizard

**Input**: Design documents from `/specs/003-project-creation-wizard/`
**Prerequisites**: plan.md (required), spec.md (required for user stories), research.md, data-model.md, contracts/

**Organization**: Tasks are grouped by user story to enable independent implementation and testing of each story.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: 创建 Launcher 项目目录结构、CMake 配置、进程平台抽象层和模板数据

- [x] T001 Create `Project/Launcher/` directory structure with `Core/` subdirectory
- [x] T002 Create `Project/Launcher/CMakeLists.txt` defining `Launcher` executable target linking `NLS_Engine`, output to `NLS_APP_OUTPUT_PATH`, following pattern from `Project/Editor/CMakeLists.txt`
- [x] T003 [P] Create `Runtime/Platform/Process/Process.h` defining `ProcessLaunchResult` struct and `Platform::Process::Launch()` / `FindExecutable()` declarations per contract `contracts/process-platform-abstraction.md`
- [x] T004 [P] Implement `Runtime/Platform/Process/Process.cpp` with Windows `CreateProcessA` + `DETACHED_PROCESS` and Linux/macOS `fork()+execvp()+setsid()` implementations
- [x] T005 [P] Create `Assets/Templates/EmptyProject/template.json` with Empty Project template metadata per contract `contracts/project-template-format.md`
- [x] T006 Update `Project/CMakeLists.txt` to add `add_subdirectory("Launcher")` with folder property

**Checkpoint**: Directory structure and CMake targets ready, process abstraction compilable

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: 将 Launcher 代码从 Editor 迁移到独立项目，实现 Editor 无参数报错，建立 Launcher→Editor 进程启动

**⚠️ CRITICAL**: No user story work can begin until this phase is complete

- [x] T007 Move `Project/Editor/Core/Launcher.h` to `Project/Launcher/Core/Launcher.h` (adjust namespace includes, keep `NLS::Launcher` namespace)
- [x] T008 Move `Project/Editor/Core/Launcher.cpp` to `Project/Launcher/Core/Launcher.cpp` (update include paths to use relative references within Launcher project)
- [x] T009 Write `Project/Launcher/Main.cpp` entry point: parse `--backend`/`--renderdoc`/`--capture-after-frames` args, create `Launcher` instance, call `Run()`, then use `Platform::Process::Launch()` to start Editor with returned project path and backend args, then exit Launcher
- [x] T010 Modify `Project/Editor/Main.cpp`: remove `#include "Core/Launcher.h"`, remove `Launcher` creation branch, when no project path is provided print `"No project specified. Launch Editor through Launcher.exe or provide a project path as argument."` to stderr and return `EXIT_FAILURE`
- [x] T011 Delete `Project/Editor/Core/Launcher.h` and `Project/Editor/Core/Launcher.cpp` (now in `Project/Launcher/Core/`)
- [x] T012 Verify build: `cmake --build build --config Debug` succeeds producing `Launcher.exe`, `Editor.exe`, `Game.exe` in output directory

**Checkpoint**: Foundation ready - Launcher.exe shows project hub, Editor.exe requires project path, process launch works

---

## Phase 3: User Story 1 - Launcher 与 Editor 分离为独立可执行程序 (Priority: P1) 🎯 MVP

**Goal**: Launcher 作为独立进程运行，选择项目后通过命令行参数启动 Editor。单独启动 Editor 必须传入项目路径。

**Independent Test**: 启动 Launcher.exe → 显示项目选择界面 → 选择已有项目 → Editor.exe 自动启动并加载项目。直接运行 Editor.exe（无参数）→ 显示错误信息并退出。

### Implementation for User Story 1

- [x] T013 [US1] In `Project/Launcher/Core/Launcher.cpp`, modify `LauncherPanel::OpenProject()` to use `Platform::Process::Launch()` instead of returning to `Main.cpp`: build command line args from project path and backend override, call `Process::Launch()`, display error dialog if launch fails, exit Launcher on success
- [x] T014 [US1] In `Project/Launcher/Core/Launcher.cpp`, modify `LauncherPanel::CreateProject()` flow to use `Platform::Process::Launch()` after project creation: create project directories + `.nullus` file, then launch Editor with new project path and exit Launcher
- [x] T015 [US1] In `Project/Launcher/Main.cpp`, update `TryRun()` helper to use `Platform::Process::FindExecutable("Editor.exe")` to locate Editor, construct command line per contract `contracts/launcher-editor-cli.md`, handle `FindExecutable` returning `nullopt` with user-facing error
- [x] T016 [US1] In `Project/Editor/Main.cpp`, update help text to indicate Editor must be launched through Launcher or with project path argument, remove reference to "shows launcher" behavior
- [ ] T017 [US1] Verify Launcher→Editor launch: run `Launcher.exe`, select existing project from recent list, confirm `Editor.exe` starts with correct project path and backend
- [ ] T018 [US1] Verify Editor standalone: run `Editor.exe` without args, confirm error message printed and process exits with code 1
- [ ] T019 [US1] Verify Editor with args: run `Editor.exe --backend vulkan "path/to/TestProject/TestProject.nullus"`, confirm Editor starts normally with Vulkan backend

**Checkpoint**: Launcher 和 Editor 完全分离，进程间通信通过命令行参数工作正常

---

## Phase 4: User Story 2 - 项目创建向导窗口 (Priority: P2)

**Goal**: 替代系统文件夹对话框，提供集成化的项目创建向导 UI，包含模板选择、项目信息填写、渲染后端选择和初始化设置。

**Independent Test**: 在 Launcher 中点击 "New Project" → 显示创建向导（而非系统文件夹对话框）→ 填写项目信息 → 创建项目 → Editor 自动启动。

### Implementation for User Story 2

- [x] T020 [US2] Create `Project/Launcher/Core/ProjectCreationWizard.h` defining `ProjectCreationWizard` class inheriting from `UI::PanelWindow`, with fields for `ProjectCreationConfig` data model: projectName, projectLocation, selectedBackend, windowWidth, windowHeight, vsync, multiSampling, sampleCount
- [x] T021 [US2] Create `Project/Launcher/Core/ProjectCreationWizard.cpp` implementing the wizard panel UI with single-page card layout per research.md R3: template selection area, project info card (name InputText, location InputText + browse button using `Dialogs::SelectFolderDialog`), rendering settings card (backend Combo, resolution InputInt x InputInt, vsync Checkbox, multiSampling Checkbox, sampleCount Combo with values 1/2/4/8)
- [x] T022 [US2] In `Project/Launcher/Core/ProjectCreationWizard.cpp`, implement "Create" button with validation per data-model.md V-001 through V-005: project name regex `[a-zA-Z0-9_-]` length 1-128, location must exist, path conflict check (V-003), backend must be valid for platform (V-004), resolution range check (V-005). Show inline error messages in red for each failed validation
- [x] T023 [US2] In `Project/Launcher/Core/ProjectCreationWizard.cpp`, implement project creation logic: create directory structure (`<path>/<name>/`, `Assets/`, `Logs/`, `UserSettings/`, `ProjectSettings/`), create `.nullus` INI file with all settings from `ProjectCreationConfig` (graphics_backend, x_resolution, y_resolution, vsync, multi_sampling, samples, and all defaults from `Context::ResetProjectSettings()`)
- [x] T024 [US2] In `Project/Launcher/Core/ProjectCreationWizard.cpp`, style the wizard panel using ALTERNATIVE_DARK theme consistency: PushStyleColor for cards matching Launcher's current card style (WindowBg 0.11,0.12,0.13), unityBlue (0.23,0.49,0.82) for primary button, no rounding, Ruda font, section headers with Separator
- [x] T025 [US2] Modify `Project/Launcher/Core/Launcher.cpp` to add wizard state: when "New Project" button is clicked, replace main panel content with `ProjectCreationWizard` panel instead of calling `Dialogs::SelectFolderDialog`. Add "← Back" button in wizard to return to main launcher view
- [x] T026 [US2] In `Project/Launcher/Core/ProjectCreationWizard.cpp`, after successful project creation: call `RegisterProject()` to add to `projects.ini`, then use `Platform::Process::Launch()` to start Editor with new project path and selected backend, then signal Launcher to exit
- [x] T027 [US2] Verify wizard flow: launch `Launcher.exe`, click "New Project", confirm wizard shows instead of folder dialog, confirm template selection area, project info fields, backend dropdown, resolution inputs all render correctly with ALTERNATIVE_DARK styling
- [x] T028 [US2] Verify project creation: fill wizard with name "TestWizardProject", location in temp directory, backend Vulkan, resolution 1920x1080, click Create, confirm `.nullus` file has `graphics_backend=vulkan`, `x_resolution=1920`, `y_resolution=1080`, confirm Editor launches

**Checkpoint**: 项目创建向导完全可用，替代了旧的文件夹对话框流程

---

## Phase 5: User Story 3 - 项目模板选择 (Priority: P3)

**Goal**: 实现动态模板加载系统，当前提供"空项目"模板，UI 框架支持未来扩展。

**Independent Test**: 向导中正确显示"空项目"模板卡片，选中后创建的项目仅包含基础目录结构。

### Implementation for User Story 3

- [x] T029 [US3] Create `Project/Launcher/Core/TemplateManager.h` defining `ProjectTemplate` struct (name, description, previewImagePath, templateDirectory, sortOrder) and `TemplateManager` class with `LoadTemplates(templateRoot)`, `GetTemplates()`, and `GetTemplateById(id)` methods
- [x] T030 [US3] Create `Project/Launcher/Core/TemplateManager.cpp` implementing template discovery: scan `<templateRoot>/` subdirectories, parse each `template.json` using JSON parsing (nlohmann/json or manual parsing), validate per contract `contracts/project-template-format.md`, sort by sortOrder ascending, skip invalid templates silently
- [x] T031 [US3] In `Project/Launcher/Core/ProjectCreationWizard.cpp`, replace hardcoded template area with dynamic template cards from `TemplateManager::GetTemplates()`: render each template as a selectable card showing name and description, highlight selected template with unityBlue border, support horizontal scroll for multiple templates
- [x] T032 [US3] In `Project/Launcher/Core/ProjectCreationWizard.cpp`, integrate template into project creation: if selected template has `content/` directory, copy its contents recursively to new project path using `std::filesystem::copy` with `overwrite_existing`; standard directories (Assets, Logs, etc.) are always created regardless
- [x] T033 [US3] In `Project/Launcher/Core/Launcher.cpp`, initialize `TemplateManager` during `SetupContext()`, loading templates from `Assets/Templates/` relative to executable, pass to `ProjectCreationWizard`
- [x] T034 [US3] Verify template loading: confirm `Assets/Templates/EmptyProject/template.json` is loaded and "Empty Project" template appears in wizard with correct name and description
- [x] T035 [US3] Verify extensibility: temporarily create a second test template directory with `template.json`, confirm it appears alongside Empty Project without code changes, then remove test template

**Checkpoint**: 模板系统可扩展工作，添加新模板无需修改代码

---

## Phase 6: Polish & Cross-Cutting Concerns

**Purpose**: 跨用户故事的改进和最终验证

- [x] T036 [P] Update `CLAUDE.md` "Editor / Game Targets" section to list `Launcher.exe` as a build product
- [x] T037 [P] Update `CLAUDE.md` "Graphics Backend Selection" section to reflect Launcher→Editor flow and Editor mandatory project argument
- [x] T038 [P] Verify cross-platform process abstraction: confirm `Runtime/Platform/Process/Process.cpp` compiles on Linux/macOS (fork+execvp path), even if primary testing is Windows
- [x] T039 Run full build: `cmake --build build --config Debug` succeeds with all three targets (Launcher, Editor, Game)
- [x] T040 Run tests: `ctest --test-dir build -C Debug --output-on-failure` passes with no regressions
- [x] T041 Run quickstart.md validation end-to-end: Launcher launch, project creation via wizard, Editor standalone error, Editor with args

---

## Phase 7: Unity Hub 风格 Launcher 重构

**Purpose**: 将现有 Launcher 项目列表页和新建项目页改为 Unity Hub 3.3 风格关键布局，保留 Nullus 品牌和已有功能，未支持入口只做静态或禁用展示。

- [x] T042 Update `Project/Launcher/Core/LauncherTheme.h` with shared Hub layout constants, disabled text/background colors, and compact icon helpers for Launcher and wizard chrome
- [x] T043 Update `Project/Launcher/Core/Launcher.cpp` window setup to default to a larger resizable Launcher window with minimum size using `Windowing::Settings::WindowSettings`
- [x] T044 Update `Project/Launcher/Core/Launcher.cpp` `LauncherPanel::Draw()` to size the root panel from the current `Windowing::Window::GetSize()` instead of fixed `1200x700`
- [x] T045 Update `Project/Launcher/Core/Launcher.cpp` to draw Unity Hub style left brand rail and navigation rail, with unsupported entries static/disabled and Project selected
- [x] T046 Update `Project/Launcher/Core/Launcher.cpp` project page header/action/search layout to match Unity Hub key placement while preserving Open Project and New Project behavior
- [x] T047 Update `Project/Launcher/Core/Launcher.cpp` project table layout to use responsive column widths and stable row interactions across default, minimum, and expanded window sizes
- [x] T048 Update `Project/Launcher/Core/ProjectCreationWizard.cpp` new-project layout to use responsive Unity Hub style category/template/preview columns and fixed footer without clipping key controls
- [x] T049 Verify `cmake --build build --config Debug --target Launcher` succeeds after the UI refactor
- [x] T050 Manually verify Launcher project page and new-project page at default, minimum, and expanded sizes; record any platform/backend limitation in the final summary

---

## Phase 8: Sidebar 简化、Installs 首版、本地化与 DX12 resize 排查

**Purpose**: 按最新待办收窄 Launcher 功能边界，加入可复用本地化方案，并用渲染侧证据定位 DX12 resize 黑边/旧尺寸区域问题。

- [x] T051 [P] Add unit tests for `Project/Launcher/Core/LauncherLocalization.*`: load strings from a single `strings.csv` table, return localized text by key for multiple locales, and fall back when a locale/key is missing
- [x] T052 [P] Add unit tests for `Project/Launcher/Core/LauncherSettings.*`: persist and reload a multi-install editor list plus default selection; reject missing paths and invalid executable extensions
- [x] T053 [P] Add unit tests or layout-helper tests ensuring Launcher navigation exposes only Projects and Installs in this first pass
- [x] T054 Create `Project/Launcher/Core/LauncherLocalization.h/.cpp` with a key-based API and single-table UTF-8 resource loading from `App/Assets/Localization/Launcher/strings.csv`
- [x] T055 Create `App/Assets/Localization/Launcher/strings.csv` resource table for all Launcher visible text across `en-US` and `zh-CN`
- [x] T056 Create `Project/Launcher/Core/LauncherSettings.h/.cpp` for `launcher.ini` persistence of a multi-install editor list and default selection
- [x] T057 Update `Project/Launcher/Core/Launcher.cpp` to replace hardcoded Chinese UI text with `LauncherLocalization`, remove account/avatar and unused sidebar entries, and add Projects/Installs navigation state
- [x] T058 Update `Project/Launcher/Core/Launcher.cpp` Installs page to show the maintained editor-version list, derive version labels from file last-write date, support add/remove/default selection, and choose existing executables with `Dialogs::OpenFileDialog`
- [x] T059 Update `Project/Launcher/Core/ProjectCreationWizard.cpp` to replace hardcoded Chinese UI text with `LauncherLocalization`, add editor-version selection, and validate the chosen install path
- [x] T060 Update `Project/Launcher/Core/ProjectCreationWizard.cpp` to remove the top-left Back button; keep footer Cancel as the return path, and show a modal prompt that blocks creation when no usable editor versions exist
- [ ] T061 Investigate DX12 Launcher resize root cause: reproduce, collect resize/window/framebuffer/ImGui/swapchain/backbuffer evidence, and capture/analyze a DX12 frame when RenderDoc automation can observe the resized frame
- [ ] T062 Implement the minimal DX12 resize fix only after T061 identifies the failing layer
- [x] T063 Verify `cmake --build build --config Debug --target Launcher` succeeds
- [x] T064 Verify targeted unit tests for Launcher localization/settings/layout pass
- [x] T065 Verify source scan confirms Launcher UI source no longer contains hardcoded Chinese UTF-8 byte string literals
- [ ] T067 [P] Add unit tests for project metadata persistence so `.nullus` files store and reload `last_editor_executable`, and version labels can be derived from the bound executable path
- [ ] T068 Update `Project/Launcher/Core/ProjectCreationWizard.cpp` and Launcher open-project flow to persist project-bound editor executable metadata on create/open
- [ ] T069 Update `Project/Launcher/Core/Launcher.cpp` Projects table to replace the backend column with project-bound editor version display and a clickable three-dot action menu
- [ ] T070 Update `Project/Launcher/Core/Launcher.cpp` Installs page header/layout to align with Projects page action area and move the add-version button to the top-right header region
- [ ] T071 Update `Project/Launcher/Core/Launcher.cpp` brand rail drawing to remove the unused placeholder square
- [x] T072 Create shared `Runtime/UI/Icons/*` vector icon library with a reusable draw API for Launcher and Editor surfaces
- [x] T073 Update `Project/Launcher/Core/Launcher.cpp` to use the shared icon library for search, more-actions, Projects, and Installs icons, and add the missing left-side vertical divider
- [ ] T066 Manually verify Projects, Installs, and Create Project pages plus DX12 resize behavior; record RenderDoc/log evidence and any remaining backend-specific limitation

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies - can start immediately
- **Foundational (Phase 2)**: Depends on Phase 1 completion - BLOCKS all user stories
- **US1 (Phase 3)**: Depends on Phase 2 - No dependencies on other stories
- **US2 (Phase 4)**: Depends on Phase 2 - Builds on US1's process launch mechanism (Launcher→Editor flow)
- **US3 (Phase 5)**: Depends on Phase 4 (needs wizard UI from US2 to integrate template cards)
- **Polish (Phase 6)**: Depends on all user stories being complete
- **Phase 8**: Depends on Phase 7 UI structure; T062 depends on T061 root-cause evidence

### User Story Dependencies

- **User Story 1 (P1)**: Can start after Phase 2 - completes Launcher/Editor separation
- **User Story 2 (P2)**: Can start after Phase 2 - but pragmatically needs US1's process launch code (T013-T015) to be done, as the wizard must launch Editor after project creation
- **User Story 3 (P3)**: Depends on US2 - needs wizard UI panel to integrate template cards

### Within Each User Story

- Models/entities before UI components
- UI components before integration with Launcher
- Verification tasks after implementation
- Story complete before moving to next priority

### Parallel Opportunities

- T003, T004, T005 can run in parallel (different files, no dependencies)
- T007, T008 can run in parallel (both are file moves)
- T020, T021 can start together (header + implementation)
- T029, T030 can start together (header + implementation)
- T036, T037, T038 can run in parallel (different files)

---

## Parallel Example: Phase 1

```text
# Launch together (no dependencies between them):
T003: "Create Runtime/Platform/Process/Process.h"
T004: "Implement Runtime/Platform/Process/Process.cpp"
T005: "Create Assets/Templates/EmptyProject/template.json"
```

## Parallel Example: User Story 3

```text
# Launch together:
T029: "Create Project/Launcher/Core/TemplateManager.h"
T030: "Implement Project/Launcher/Core/TemplateManager.cpp"
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup (CMake targets, process abstraction, template data)
2. Complete Phase 2: Foundational (code migration, Editor standalone error)
3. Complete Phase 3: User Story 1 (process launch integration)
4. **STOP and VALIDATE**: Test Launcher→Editor launch, Editor standalone error
5. At this point, Launcher and Editor are fully separated and functional

### Incremental Delivery

1. Phase 1 + Phase 2 → Foundation ready (Launcher/Editor separated)
2. Add Phase 3 (US1) → Process launch works → MVP!
3. Add Phase 4 (US2) → Project creation wizard → Full creation experience
4. Add Phase 5 (US3) → Template system → Extensible project scaffolding
5. Phase 6 → Polish and documentation

### Suggested MVP Scope

**User Story 1 only** (Phases 1-3): Launcher/Editor separation with process-based launch. This delivers the core architectural change and is independently valuable - users get a cleaner separation between Launcher and Editor.

---

## Notes

- [P] tasks = different files, no dependencies
- [Story] label maps task to specific user story for traceability
- Each user story should be independently completable and testable
- Commit after each task or logical group
- Stop at any checkpoint to validate story independently
- The `Project/Launcher/` CMakeLists.txt follows the exact pattern of `Project/Game/CMakeLists.txt` for consistency
- Template JSON parsing: project currently has no JSON library in ThirdParty - may need to add nlohmann/json or use simple manual parsing for v1
