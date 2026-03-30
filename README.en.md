# Nullus
<p align="center">
    <img src="NullusLogo.png" width="400" alt="Nullus Engine logo">
</p>

![License MIT](https://img.shields.io/badge/License-MIT-yellow.svg)
![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C)
![CMake >=3.16](https://img.shields.io/badge/CMake-%E2%89%A53.16-064F8C)
![.NET 8](https://img.shields.io/badge/.NET-8.0-512BD4)
![Build Windows](https://github.com/NullusEngine/Nullus/workflows/Build%20Windows/badge.svg)
![Build Linux](https://github.com/NullusEngine/Nullus/workflows/Build%20Linux/badge.svg)
![Build macOS](https://github.com/NullusEngine/Nullus/workflows/Build%20MacOS/badge.svg)

[中文版](./README.md) | [English](./README.en.md)

Nullus is an evolving C++ 3D engine project focused on the scene system, resource system, editor tooling, reflection code generation, and a modern rendering pipeline.

## Preview

### Launcher

![Nullus Launcher](Docs/Screenshots/launcher.png)

### Editor

![Nullus Editor](Docs/Screenshots/editor.png)

## Highlights

- Runtime modules are split into `Base`, `Core`, `Engine`, `Math`, `Platform`, `Rendering`, and `UI`
- Ships both an editor and a runtime `Game` application on the same engine mainline
- MetaParser-based code-generated reflection system
- Reflection-driven scene and object serialization flow
- Forward and deferred scene rendering paths with `FrameGraph` integration
- Cross-platform build flow for Windows, Linux, and macOS

## Core Capabilities

- scene system: `Scene / SceneManager / GameObject / Component`
- resource system: `Model / Texture / Shader / Material`
- reflection system: MetaParser-driven code-generated reflection
- serialization: reflection-based scene and object serialization
- rendering system: forward rendering, deferred rendering, GBuffer, clustered shading
- editor tooling: core views, debug drawing, picking, Gizmo, and resource preview

## Quick Start

### Requirements

- CMake 3.16 or newer
- a compiler with C++20 support
- .NET SDK 8.0 or newer
- Git

### Clone

```bash
git clone https://github.com/NullusEngine/Nullus.git
cd Nullus
git submodule update --init --recursive
```

### Build on Windows

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
```

You can also use the bundled scripts:

```powershell
build_windows.bat Debug
build_windows.bat Release
build_windows.bat Debug ARM64
```

### Build on Linux

```bash
./build_linux.sh debug
./build_linux.sh release
```

### Build on macOS

```bash
./build_macos.sh debug
./build_macos.sh release
```

### Run

After the build, you can run:

- `Editor`
- `Game`

## Documentation

- Reflection workflow (Chinese): `Docs/Reflection/ReflectionWorkflow.zh-CN.md`
- Reflection workflow (English): `Docs/Reflection/ReflectionWorkflow.en.md`
- Testing guide: `Docs/Testing.md`
- AI workflow and repository rules: `Docs/AIWorkflow.md`

## Reflection And Generation

Nullus uses MetaParser to generate reflection code:

1. the main build first builds `Tools/MetaParser`
2. `Runtime` modules run MetaParser before compilation
3. matching `*.generated.h` / `*.generated.cpp` files are emitted
4. runtime type declaration, definition, and registration are completed through generated code

Do not hand-edit anything under `Runtime/*/Gen/`.

## Platforms And CI

GitHub Actions currently covers:

- Windows
- Linux
- macOS

CI builds the project normally and continues with reflection-related and unit-test targets.

## Troubleshooting

### `Failed to Init GLFW`

This usually means the graphics environment is not ready. Check:

- whether the machine has a working desktop environment
- whether `DISPLAY` is set correctly on Linux / WSL

### MetaParser / libclang crashes

Check these first:

- the .NET 8 SDK is installed
- `dotnet restore` completed correctly
- you did not accidentally force the generator onto an old system `libclang`

## License

This repository uses the MIT License. See [LICENSE](./LICENSE) for details.
