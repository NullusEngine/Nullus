# Feature Specification: Project Creation Wizard

**Feature Branch**: `003-project-creation-wizard`
**Created**: 2026-04-15
**Status**: Draft
**Input**: User description: "现在创建项目的方式是调用系统弹窗选择一个文件夹，然后往里创建初始内容。但我想要的是有个单独创建项目的窗口，然后可以选择不同的项目模板，目前就默认一个空项目模板就好了。然后还可以选择渲染后端，等一些初始化设置。这个窗口要设计的好看，布局合理，并且要和当前风格保持一致，这个可能需要把Launch和Editor拆成两个项目才可以做到用指定后端启动editor，Launch启动editor的时候就是传editor的启动参数就好了，单独启动editor必须要传参数"

## Clarifications

### Session 2026-04-17

- Q: Launcher UI should follow which Unity Hub matching scope? → A: A, high-fidelity key layout: project list, sidebar navigation, search/action controls, and new-project template layout; unsupported entries are static or disabled.
- Q: Should the redesign preserve Nullus branding or mirror Unity Hub product identity? → A: Preserve Nullus branding while matching Unity Hub layout, dark visual hierarchy, control placement, and interaction states.
- Q: Should the Launcher window remain fixed-size or resize like a Hub-style tool? → A: Use a larger default resizable window with minimum size and responsive constraints for project table and new-project columns.
- Q: Which Unity Hub sidebar features should remain in the Nullus Launcher first pass? → A: Remove unused sidebar features and account/avatar UI; keep only Projects and Installs.
- Q: What should the initial Installs page support? → A: It only needs to let the user choose one existing engine/editor executable file and remember that choice for future launch flow.
- Q: How should the create-project page return to the project list? → A: Remove the top-left Back button; use the footer Cancel action as the only return path for now.
- Q: How should Launcher text be localized? → A: Use a shared localization scheme backed by resource files; do not hardcode UTF-8 Chinese strings in C++ source.
- Q: Should Launcher localization stay split by locale file or move to a single shared table? → A: Use one shared localization table file containing all supported locales.
- Q: Should Installs stay as one executable path or grow into a multi-version list? → A: Expand Installs into a maintained list of editor versions; each version label is the executable's last modified date.
- Q: How should create-project choose an editor version? → A: The wizard must let the user choose an editor version from Installs, and creation must be blocked with a modal prompt when no usable version exists.
- Q: How should the project list show editor version and row actions? → A: Each project row should show the editor version bound on last open, and the trailing three-dot button should open a Unity Hub style action menu for show in explorer, disabled command-line args, and remove from list.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Launcher 与 Editor 分离为独立可执行程序 (Priority: P1)

用户目前使用的 Launcher 和 Editor 位于同一个可执行文件中。现在需要将 Launcher 拆分为独立的可执行程序。当用户启动 Launcher 时，Launcher 自身是一个轻量级应用，仅负责项目管理和创建引导。当用户在 Launcher 中选择或创建项目后，Launcher 通过进程启动 Editor 可执行程序，并将项目路径、后端选择等参数通过命令行传递给 Editor。单独启动 Editor 必须传入项目路径等必要参数，否则无法正常运行。

**Why this priority**: 这是整个功能的基础架构变更，所有后续的创建向导、后端选择等功能都依赖于 Launcher 和 Editor 的分离。没有这个拆分，就无法实现"在 Launcher 中选择后端然后用指定后端启动 Editor"的核心流程。

**Independent Test**: 可以通过命令行直接启动 Editor（传入 `--backend` 和项目路径参数）验证 Editor 独立运行是否正常，再通过 Launcher 启动 Editor 验证参数传递是否正确。

**Acceptance Scenarios**:

1. **Given** Launcher 可执行程序已构建完成，**When** 用户双击启动 Launcher，**Then** Launcher 显示项目选择/创建界面（不加载 Editor 任何代码）
2. **Given** Launcher 中用户已选择一个已有项目，**When** 用户点击打开项目，**Then** Launcher 启动 Editor 可执行程序并传入项目路径和后端参数，Editor 正常加载该项目
3. **Given** 用户直接运行 Editor 可执行程序，**When** 未传入必要的命令行参数（项目路径），**Then** Editor 显示错误提示或引导用户通过 Launcher 启动
4. **Given** 用户直接运行 Editor 可执行程序，**When** 传入了有效的项目路径和后端参数，**Then** Editor 正常启动并加载指定项目

---

### User Story 2 - 项目创建向导窗口 (Priority: P2)

用户点击 Launcher 中的"新建项目"后，不再弹出系统文件夹选择对话框，而是打开一个全新的项目创建向导窗口。该向导窗口引导用户完成以下步骤：选择项目模板（初始版本仅提供"空项目"模板）、填写项目名称、选择项目存储位置、选择渲染后端、配置初始设置（分辨率、垂直同步、采样等）。向导采用 Unity Hub 3.3 风格的深色双栏布局：左侧模板分类，中间模板列表，右侧模板预览和项目设置，底部固定操作栏。

**Why this priority**: 这是用户可见的核心体验改进。项目创建向导替代了原有的简陋流程（系统文件夹对话框），提供了模板选择、后端选择等新功能，是用户最直接感知到的变化。

**Independent Test**: 可以在 Launcher 中点击"新建项目"验证向导窗口是否正确显示，填写各字段后验证项目文件是否按预期创建。

**Acceptance Scenarios**:

1. **Given** Launcher 已启动并显示主界面，**When** 用户点击"New Project"按钮，**Then** 打开项目创建向导窗口（而非系统文件夹对话框）
2. **Given** 向导窗口已打开，**When** 用户查看向导内容，**Then** 显示项目模板选择区域（默认选中"空项目"模板）、项目名称输入框、存储位置选择器、渲染后端下拉选择器、以及基础初始化设置（分辨率、垂直同步、采样等）
3. **Given** 用户在向导中填写了项目名称为"MyGame"、选择了存储位置为"D:/Projects"、选择了 Vulkan 后端、设置了分辨率为 1920x1080，**When** 用户点击"创建"按钮，**Then** 在指定位置创建项目目录结构（包含 .nullus 配置文件），配置文件中包含用户选择的后端、分辨率等设置
4. **Given** 向导窗口已打开，**When** 用户未填写项目名称或未选择存储位置就点击"创建"，**Then** 显示验证错误提示，阻止创建
5. **Given** 项目已成功创建，**When** 创建完成后，**Then** 自动启动 Editor 并打开新创建的项目

---

### User Story 3 - 项目模板选择 (Priority: P3)

用户在创建向导中可以选择不同的项目模板。当前版本仅提供一个"空项目"模板作为默认选项，但模板选择的 UI 框架需要支持未来扩展更多模板。每个模板以卡片形式展示，包含模板名称、简短描述和缩略预览图。模板信息从模板目录中动态加载。

**Why this priority**: 模板选择功能为未来扩展奠定了基础。虽然当前只有一个模板，但 UI 设计需要预留扩展空间，确保后续添加模板时不需要重新设计整个向导布局。

**Independent Test**: 可以验证向导中是否正确显示"空项目"模板卡片，以及该模板被选中后是否影响最终创建的项目结构。

**Acceptance Scenarios**:

1. **Given** 向导窗口已打开，**When** 用户查看模板选择区域，**Then** 显示至少一个"空项目"模板卡片，包含模板名称和描述
2. **Given** 向导窗口显示了模板列表，**When** 用户点击某个模板卡片，**Then** 该模板被选中（高亮显示），其他模板取消选中
3. **Given** "空项目"模板被选中，**When** 用户创建项目，**Then** 创建的项目仅包含基础目录结构和默认配置文件（无预置场景或资源）

---

### Edge Cases

- 用户在向导中选择的存储位置已经存在同名项目目录时，系统应提示冲突并让用户选择处理方式（更换名称或选择其他位置）
- 用户在向导中选择了当前平台不支持的渲染后端时，系统应给出友好提示
- Launcher 启动 Editor 进程后，如果 Editor 启动失败（找不到可执行文件或崩溃），Launcher 应显示错误信息
- 用户在向导中输入特殊字符或超长项目名称时，应进行合法性校验
- 系统文件夹浏览器中用户取消选择存储位置时，应安全返回向导而不会崩溃

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: 系统 MUST 将 Launcher 和 Editor 拆分为两个独立可执行程序，Launcher 作为项目管理的入口，Editor 仅负责项目编辑
- **FR-002**: Launcher 启动 Editor 时 MUST 通过命令行参数传递项目路径、渲染后端、RenderDoc 设置等信息
- **FR-003**: 单独启动 Editor（无命令行参数）时 MUST 显示明确的错误提示或引导信息，告知用户应通过 Launcher 启动
- **FR-004**: 系统 MUST 提供项目创建向导窗口，替代原有的系统文件夹对话框
- **FR-005**: 创建向导 MUST 允许用户填写项目名称、选择存储位置、选择渲染后端
- **FR-006**: 创建向导 MUST 提供项目模板选择功能，初始版本包含"空项目"模板
- **FR-007**: 创建向导 MUST 允许用户配置项目初始化设置（窗口分辨率、垂直同步、多重采样、采样数）
- **FR-008**: 系统 MUST 根据向导中的用户选择生成 .nullus 项目配置文件，将后端、分辨率等设置写入配置
- **FR-009**: 创建向导 MUST 在用户提交前验证所有必填字段（项目名称、存储位置）
- **FR-010**: Launcher 项目列表页和创建向导 MUST 高保真对齐 Unity Hub 3.3 的关键布局和样式，包括左侧导航、项目列表表头、搜索和打开/新项目操作区、新建项目模板分类栏、模板列表、右侧预览设置栏、底部操作栏；窗口标题、Logo、可用入口和产品文案 MUST 保留 Nullus 品牌身份
- **FR-015**: Launcher 窗口 MUST 使用更接近 Hub 工具窗口的默认尺寸并允许用户调整大小，同时设置最小尺寸；项目列表表格、新建项目模板列表和右侧设置栏 MUST 在窗口尺寸变化时保持稳定排版，不重叠、不裁切关键操作按钮
- **FR-011**: 项目创建成功后，系统 MUST 自动启动 Editor 并打开新创建的项目
- **FR-012**: 系统 MUST 支持 Launcher 和 Editor 之间的进程生命周期管理（Launcher 在 Editor 启动后退出）
- **FR-013**: 创建向导中存储位置选择 MUST 提供浏览文件夹的交互方式
- **FR-014**: 系统 MUST 在存储位置已存在同名目录时提示用户
- **FR-016**: Launcher 左侧导航 MUST 移除账号头像、学习、社区、在线服务、开发者服务、云桌面、云渲染、游戏、下载等未实现入口；首版仅保留 Projects 和 Installs 两个可见导航项
- **FR-017**: Installs 页面 MUST 支持维护多个已存在的 Editor/Engine 可执行文件路径，并将这些安装项及默认选择持久化到 Launcher 设置文件；当路径不存在时 MUST 显示用户可理解的提示并允许删除或重新选择
- **FR-018**: 创建项目页面 MUST 移除左上角 Back 按钮；用户取消创建时 MUST 通过底部 Cancel 按钮返回项目列表
- **FR-019**: Launcher 用户可见字符串 MUST 通过通用本地化服务按 key 获取，并从单个外部 UTF-8 本地化表文件加载所有支持的 locale；C++ 源码中不得硬编码中文 UTF-8 字符串或中文字节转义作为界面文案
- **FR-020**: DX12 Launcher resize 问题 MUST 通过渲染侧证据定位根因后再修复；调查至少记录窗口尺寸、framebuffer/swapchain 尺寸、UI draw data 尺寸、DX12 backbuffer 尺寸或 RenderDoc/rdc 可用证据
- **FR-021**: 创建项目页面 MUST 提供编辑器版本选择控件，候选项来自 Installs 中的已持久化安装列表，并使用对应可执行文件的最后修改日期作为版本号展示
- **FR-022**: 当创建项目时不存在任何可用的编辑器版本时，系统 MUST 弹出用户可理解的模态提示并阻止项目创建
- **FR-023**: Launcher 在打开已有项目时 SHOULD 使用 Installs 中当前默认选中的可用编辑器版本；当默认项不可用且同目录存在 `Editor.exe` 等默认可执行文件时 MAY 回退到该本地可执行文件
- **FR-024**: Projects 页面 MUST 将项目列表中的“编辑器版本”列显示为该项目上次打开时绑定的编辑器可执行文件版本，并将该绑定持久化到项目 `.nullus` 文件中；创建项目成功后 MUST 将所选编辑器路径写入该绑定
- **FR-025**: Projects 页面每一行末尾 MUST 提供一个可点击的三点操作菜单，菜单样式与 Unity Hub 关键视觉一致，至少包含“在资源管理器中显示”“添加命令行参数（可禁用）”“从列表移除项目”三项
- **FR-026**: Installs 页面 MUST 与 Projects 页面共享关键头部排版骨架，并将“Add version”主操作按钮放在内容区右上角
- **FR-027**: Launcher 左侧品牌栏 MUST 移除当前无功能的占位方块，仅保留品牌图标和有效导航结构
- **FR-028**: 系统 MUST 提供一套位于共享 UI 层的矢量图标库，供 Launcher 与 Editor 等所有界面复用；首批图标至少覆盖搜索、更多操作、项目、安装、文件夹与删除等常用入口
- **FR-029**: Launcher 左侧导航栏 MUST 在品牌栏与导航栏之间显示明确的竖向细分割线，并将 Projects/Installs 前导占位圆点替换为来自共享图标库的对应图标

### Key Entities

- **Project Template（项目模板）**: 代表一种项目初始结构，包含模板名称、描述、预览图、初始文件集。当前仅有"空项目"模板（基础目录结构 + 默认配置）
- **Project Creation Config（项目创建配置）**: 用户在向导中填写的所有信息集合，包括项目名称、存储路径、所选模板、渲染后端、初始化设置（分辨率、垂直同步、采样等）
- **Launcher Launch Params（Launcher 启动参数）**: Launcher 传递给 Editor 进程的命令行参数集合，包括项目路径、后端选择、RenderDoc 设置
- **Launcher Install Entry（Launcher 安装项）**: Launcher 持久化的一条 Editor/Engine 可执行文件记录，包含路径、默认选中状态，以及由文件最后修改时间派生的显示版本号

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: 用户可以从 Launcher 中通过创建向导完成项目创建，整个过程不超过 4 次点击（打开向导 → 填写信息 → 确认创建 → 自动打开项目）
- **SC-002**: Launcher 项目列表页和创建向导在关键排版、间距、深色层级、表格结构、按钮位置、选中态和强调色上与 Unity Hub 3.3 截图保持一致，同时保留 Nullus 品牌名称和可用功能边界
- **SC-003**: Launcher 启动 Editor 的延迟不超过 2 秒（从用户点击"打开项目"到 Editor 窗口出现）
- **SC-004**: 通过向导创建的项目包含正确的 .nullus 配置文件，所有用户在向导中选择的设置（后端、分辨率等）均准确反映在配置文件中
- **SC-005**: 单独启动 Editor（无参数）时，用户能够明确知道应通过 Launcher 启动，无需查阅文档
- **SC-006**: 向导中的模板选择区域可扩展，添加新模板不需要修改向导的核心布局代码
- **SC-007**: Launcher 在默认尺寸、最小尺寸和放大后的窗口尺寸下，项目列表页和新建项目页均保持 Unity Hub 风格的主次区域比例，关键按钮、搜索框、表头、模板列表和底部操作栏可见且可点击
- **SC-008**: 用户可以在 Installs 页选择现有 `Editor.exe` 或等效引擎可执行文件，重启 Launcher 后仍能看到该路径
- **SC-009**: 源码扫描不再在 Launcher UI 源文件中发现硬编码中文 UTF-8 字节串；中文文本集中存放在单个本地化表文件中
- **SC-010**: DX12 resize 验证能够说明黑边/旧尺寸区域来自哪一层尺寸不同步，并给出对应修复或未决阻塞
- **SC-011**: 用户可以在 Installs 页维护多个编辑器版本条目，并看到每个条目以最后修改日期显示的版本号；重启 Launcher 后列表和默认选择保持一致
- **SC-012**: 当 Installs 中没有任何可用版本时，创建项目操作会被模态提示拦截，且不会落盘任何新项目目录
- **SC-013**: Projects 列表中的编辑器版本列能够在重新启动 Launcher 后继续显示该项目上次绑定的编辑器版本，而不是始终回落到当前默认安装项
- **SC-014**: 用户点击 Projects 行尾三点按钮时，会出现 Unity Hub 风格的行操作菜单，且不会误触发行打开行为

## Assumptions

- Launcher 和 Editor 在同一构建目录下生成，Launcher 可以通过相对路径找到 Editor 可执行程序
- 项目模板信息存储在文件系统中的固定目录下（如 Editor 安装目录的 Templates 子目录），便于未来扩展
- CMake 构建系统需要更新以支持构建两个独立可执行目标（Launcher 和 Editor）
- 当前 Launcher 的最近项目列表功能（projects.ini）将保留在新的 Launcher 可执行程序中
- Editor 的命令行参数解析逻辑保持不变（--backend、--renderdoc 等参数格式不变）
- Launcher 使用与当前相同的轻量级引擎上下文（仅窗口、UI、无完整渲染管线），但窗口不再固定为单一尺寸
- "空项目"模板不包含任何预置场景或资源，仅创建标准目录结构和默认 .nullus 配置文件
- Launcher 在启动 Editor 后自动退出（不保持后台运行）
- 本地化资源首版使用单个表文件存放 `en-US` 与 `zh-CN` 文案，缺失 key 时回退到 ASCII key 名称或英文默认值，避免因资源缺失导致 Launcher 无法启动
- 编辑器版本号首版不从可执行文件内部元数据读取，而是直接使用可执行文件最后修改时间格式化为显示字符串
