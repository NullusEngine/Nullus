# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

### Windows
```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
```

### Linux/macOS
```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

### Run Tests
```powershell
ctest --test-dir build -C Debug --output-on-failure
```

### Run Single Test Target
```powershell
cmake --build build --config Debug --target NullusUnitTests -- /m:1
```

### Editor / Game / Launcher Targets
After building, executables are under `Build/bin/Debug/` (Windows) or `Build/bin/` (Linux/macOS):
- `Launcher.exe` - Project hub (create/open projects, then launches Editor)
- `Editor.exe` - Desktop editor (requires project path argument)
- `Game.exe` - Runtime game application

## Architecture Overview

### Runtime Modules
`Runtime/` contains shared engine code:
- **Base**: Core utilities
- **Core**: Base types, reflection, math wrappers
- **Engine**: Scene, components, rendering
- **Math**: Vector, matrix, quaternion types
- **Platform**: OS abstractions
- **Rendering**: Graphics resources, RHI, frame graph
- **UI**: ImGui-based UI system

### Products
- **Project/Editor/**: Desktop editor application
- **Project/Game/**: Runtime game application

### Rendering Architecture

**Dual-layer model**:
1. **Formal RHI** (`Runtime/Rendering/RHI/Core/`): Platform-agnostic device, pipeline, binding, command, and resource abstractions. The target contract for renderer mainline.
2. **Legacy IRenderDevice** (`Runtime/Rendering/RHI/IRenderDevice.h`): Immediate-mode backend contract. Being phased out of renderer mainline but still used by compatibility layers and Tier B backends.

**Backend organization**:
- `Runtime/Rendering/RHI/Backends/DX12/`, `Vulkan/`, `DX11/`, `OpenGL/`, `Null/`
- All backends inherit from `NullRenderDevice` as the common scaffold
- DX12 and Vulkan are Tier A (full explicit RHI execution)
- DX11 and OpenGL are Tier B (compatibility-backed)

**Driver** (`Runtime/Rendering/Context/Driver.h/cpp`): Central entry point for backend selection, swapchain lifecycle, frame ownership, and tooling. Renderer mainline should consume formal RHI objects, not call `IRenderDevice` directly.

**Frame graph** (`Runtime/Rendering/FrameGraph/`): Explicit resource management pipeline. Frame graph textures/buffers own formal `RHITexture`/`RHIBuffer` handles.

**Renderer hierarchy**: `IRenderer` → `ABaseRenderer` → `CompositeRenderer` → scene renderers (`ForwardSceneRenderer`, `DeferredSceneRenderer`)

**Material** (`Runtime/Rendering/Resources/Material.h/cpp`): Owns both legacy `MaterialResourceSet` for compatibility and formal RHI pipeline/binding objects (`BuildRecordedGraphicsPipeline`, `GetExplicitBindingSet`). Draw submission routes through `Driver::SubmitMaterialDraw`.

### Reflection System

Nullus uses **MetaParser** for code generation-based reflection:
- `Runtime/*/Gen/MetaGenerated.h/.cpp` are generated files — **do not hand-edit**
- Run MetaParser by building the `Runtime` module
- Registration flow: generated code registers types at startup through `Runtime/Core/Gen/`

## Workflow Rules

### Major Changes Require Specs
Major changes must have a committed spec bundle under `specs/<change-id>/` containing `spec.md`, `plan.md`, and `tasks.md`. This includes functional changes in `Runtime/`, architecture changes in `Project/`, rendering/backend/frame-graph work, reflection changes, and multi-subsystem changes.

### Generated Files Are Sacred
`Runtime/*/Gen/` files are generated output — never hand-edit them.

### Validation Standards
- **Rendering**: Use RenderDoc. See `Docs/Rendering/RenderDocDebugging.md` for the workflow. One backend does not prove another.
- **Reflection/MetaParser**: Run normal build + `NullusUnitTests`
- **Runtime/editor**: Run targeted tests or provide manual verification notes with exact steps

## Key Files and Locations

- `AGENTS.md`: Repository workflow rules
- `Docs/AIWorkflow.md`: Spec-Kit and Superpowers integration details
- `Docs/Testing.md`: Test structure and execution
- `Docs/Rendering/RenderDocDebugging.md`: Graphics debugging workflow
- `.specify/memory/constitution.md`: Project constitution (highest workflow authority)
- `specs/multi-backend-rhi-mainline/`: Active major change spec bundle for RHI migration

## Environment Variables for Graphics

- `NLS_RENDERDOC_CAPTURE`: Request RenderDoc frame capture
- `NLS_RENDERDOC_CAPTURE_AFTER_FRAMES`: Delay capture until N frames

## Graphics Backend Selection

Use command-line arguments to select graphics backend:

```powershell
# DX12 backend
Editor.exe --backend dx12 MyProject.nullus

# Vulkan backend
Editor.exe --backend vulkan MyProject.nullus

# OpenGL backend
Editor.exe --backend opengl MyProject.nullus

# Without project argument, Editor exits with error
# Use Launcher.exe to browse/create projects
Launcher.exe
```

If no `--backend` argument is provided, the backend is read from `projectSettings` in the `.nullus` project file. Editor MUST be launched with a project path argument — use `Launcher.exe` for the interactive project selection and creation experience.
