# Research: Project Creation Wizard

**Feature**: `003-project-creation-wizard`
**Date**: 2026-04-15

## R1: Launcher 与 Editor 拆分架构

**Decision**: 将 Launcher 从 `Project/Editor/` 中拆分为 `Project/Launcher/` 独立 CMake 可执行目标。

**Rationale**:
- 当前 Launcher 代码（`Project/Editor/Core/Launcher.h/.cpp`）已经是一个自包含的模块，拥有独立的渲染上下文（Device、Window、Driver、UIManager），不依赖 Editor::Core::Context 或 Editor::Core::Application。
- `Main.cpp` 中的流程是串行的：Launcher 运行完毕返回项目路径后，才创建 Editor Application。两者在运行时不会同时存在。
- CMake 中 `Project/Editor/` 和 `Project/Game/` 已是平级的独立可执行目标，模式成熟，新增 `Project/Launcher/` 符合现有惯例。
- 拆分后 Editor 的 `Main.cpp` 不再包含 Launcher 代码，体积减小，启动路径更清晰。

**Alternatives considered**:
- **方案 B: 保持单可执行文件，通过参数切换模式** - 无法实现"Launcher 启动 Editor 时用指定后端"，因为后端在进程创建时就需要确定。且用户明确要求拆分。
- **方案 C: Launcher 作为动态库** - 增加了不必要的复杂性，Launcher 不需要被其他模块链接。

## R2: 进程创建机制

**Decision**: 在 `Runtime/Platform/` 中新增 `Process.h/.cpp` 平台抽象，封装跨平台进程启动能力。Launcher 通过此抽象启动 Editor 子进程。

**Rationale**:
- 现有的 `SystemCalls::OpenFile()` 使用 `std::system("start ...")`，这是异步的，无法获取子进程句柄或检测启动失败。
- `ShaderCompiler` 中有 `CreateProcessA` 的 Windows 实现，但仅用于 shader 编译，非平台抽象，且无 Linux/macOS 实现。
- Launcher 需要检测 Editor 启动是否成功（可执行文件是否存在、进程是否创建成功），并在失败时向用户显示错误信息。
- 新增的平台抽象应支持：启动进程、传递命令行参数、检测启动是否成功。

**Alternatives considered**:
- **直接使用 `std::system()`** - 无法检测启动失败，无法传递复杂参数（含空格路径），无法获取进程状态。
- **仅 Windows `CreateProcess`** - 项目需要支持 Linux/macOS。

## R3: 项目创建向导 UI 设计

**Decision**: 采用单页面卡片式布局，在 Launcher 窗口内作为新状态切换（而非弹出新窗口）。

**Rationale**:
- 当前 Launcher 窗口（1000x580）已有卡片式设计风格（Header 卡片、项目卡片），向导作为同一窗口的状态切换更自然。
- 弹出新窗口会增加窗口管理复杂度，且与现有 Launcher 的无边框自定义窗口风格不一致。
- 单页面布局将所有设置项（模板选择、名称、路径、后端、初始化设置）组织在一张表单中，用户无需多步导航，符合"不超过 4 次点击"的成功标准。

**Layout 结构**:
```
┌─────────────────────────────────────┐
│  [自定义标题栏 - 最小化/关闭]         │
├─────────────────────────────────────┤
│  ← 返回     创建新项目               │
├─────────────────────────────────────┤
│  ┌─────────────────────────────────┐│
│  │ 模板选择区域                      ││
│  │ [空项目 ▼] (卡片，可横向扩展)      ││
│  └─────────────────────────────────┘│
│  ┌─────────────────────────────────┐│
│  │ 项目信息                         ││
│  │ 名称: [________]                 ││
│  │ 位置: [________] [浏览...]       ││
│  └─────────────────────────────────┘│
│  ┌─────────────────────────────────┐│
│  │ 渲染设置                         ││
│  │ 后端: [Vulkan ▼]                 ││
│  │ 分辨率: [1920] x [1080]          ││
│  │ 垂直同步: [✓]  多重采样: [✓]     ││
│  │ 采样数: [4]                      ││
│  └─────────────────────────────────┘│
│              [创建项目]              │
└─────────────────────────────────────┘
```

**Alternatives considered**:
- **多步向导（分页）** - 当前设置项不多（6-8 项），分页显得空旷。未来设置项增多时可考虑。
- **独立弹出窗口** - 与 Launcher 的自定义窗口管理冲突。

## R4: 项目模板系统设计

**Decision**: 模板以目录结构存储在 `Assets/Templates/` 下，每个模板包含一个 `template.json` 元数据文件和初始文件集。Launcher 通过扫描此目录动态加载模板列表。

**Rationale**:
- 当前 `CreateProject()` 硬编码了目录结构创建。模板系统需要将此逻辑泛化为"从一个模板目录复制初始结构"。
- 使用目录结构而非代码注册，使添加新模板无需重新编译。
- `template.json` 提供模板元数据（名称、描述、预览图路径），支持 UI 展示。

**v1 模板结构**:
```
Assets/Templates/
└── EmptyProject/
    ├── template.json         # {"name": "Empty Project", "description": "..."}
    ├── preview.png           # 缩略预览图（可选）
    └── content/              # 初始文件模板
        └── (空目录结构将被代码创建)
```

**Alternatives considered**:
- **硬编码模板列表** - 不符合 SC-006（可扩展性要求），添加模板需要修改代码。
- **ZIP 包模板** - v1 过度设计，目录结构足够。

## R5: Editor 无参数启动行为

**Decision**: Editor 无项目路径参数时，显示控制台错误信息并退出（返回非零退出码）。不弹出窗口或消息框。

**Rationale**:
- Editor 的 `Main.cpp` 中如果没有项目路径，会尝试创建 Launcher（当前行为）。拆分后，Launcher 代码不再链接到 Editor，无路径时应该直接报错退出。
- 控制台错误信息足够明确：`"No project specified. Launch Editor through Launcher.exe or provide a project path as argument."`
- 不弹窗口是因为 Editor 此时还没有创建渲染上下文，创建一个仅用于显示错误的窗口过于重量级。

**Alternatives considered**:
- **弹出 Win32 MessageBox** - 平台特定，Linux/macOS 需要额外处理。
- **启动微型错误窗口（用 ImGui）** - 需要初始化整个渲染上下文，代价过大。

## R6: CMake 构建目标组织

**Decision**: 新增 `Project/Launcher/` 目录，包含独立的 `CMakeLists.txt`、`Main.cpp`，以及从 `Project/Editor/Core/Launcher.h/.cpp` 迁移的代码。Editor 的 `Main.cpp` 移除 Launcher 相关逻辑。

**Rationale**:
- 与现有 `Project/Game/` 和 `Project/Editor/` 的组织方式完全对称。
- `Project/CMakeLists.txt` 增加 `add_subdirectory("Launcher")` 即可。
- Launcher 仍然链接 `NLS_Engine`（需要 UI、Windowing、Rendering 模块），但不需要 Editor 特有的 Panels、EditorResources、EditorActions 等。

**构建产物**:
```
Build/bin/Debug/
├── Launcher.exe    # 新增
├── Editor.exe      # 精简（无 Launcher 代码）
└── Game.exe        # 不变
```

**Alternatives considered**:
- **Launcher 链接更小的子集库** - Launcher 仍需要完整的 ImGui 和 Driver 初始化，减少链接收益不大，且增加了 CMake 复杂度。
