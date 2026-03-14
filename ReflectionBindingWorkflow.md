# Nullus Reflection Binding Workflow

This document summarizes the reflection pipeline that is currently checked in and verified in the repository.

## Build pipeline

- `Tools/MetaParser` is the maintained reflection generator.
- Runtime modules invoke MetaParser before compilation through `nls_add_meta_generation(...)` in `Runtime/CMakeLists.txt`.
- Generated files are written into `Runtime/<Module>/Gen/MetaGenerated.h/.cpp`.
- On Windows, the generated step prepares the required `libclang` runtime before launching MetaParser.

## Registration model

Each runtime module now emits its own generated registration entry point.

Examples:

- `RegisterReflectionTypes_NLS_Base`
- `RegisterReflectionTypes_NLS_Core`
- `RegisterReflectionTypes_NLS_Engine`

Module startup code calls the local generated entry point through `NLS_META_GENERATED_REGISTER_FUNCTION`, which avoids link-time symbol collisions between modules.

## Reflected engine types

The current engine-side MetaParser coverage includes:

- `NLS::Engine::Components::Component`
- `NLS::Engine::Components::TransformComponent`
- `NLS::Engine::Components::CameraComponent`
- `NLS::Engine::Components::LightComponent`
- `NLS::Engine::Components::MeshRenderer`
- `NLS::Engine::Components::MaterialRenderer`
- `NLS::Engine::Components::SkyBoxComponent`
- `NLS::Engine::GameObject`
- `NLS::Engine::SceneSystem::Scene`

## Parser notes

MetaParser prefers `CppAst`, but keeps a text fallback for a few headers that currently trigger `CppAst` container errors on Windows.

The fallback path is currently used for:

- `Runtime/Engine/Components/MeshRenderer.h`
- `Runtime/Engine/GameObject.h`
- `Runtime/Engine/SceneSystem/Scene.h`

Those headers still generate usable type and method bindings and are covered by tests.

## Verification

`Tools/ReflectionTest` is the registration regression test.

It links against:

- `NLS_Base`
- `NLS_Core`
- `NLS_Engine`

It checks:

- type registration
- key method registration
- key field registration
- inheritance registration for component types

## Verified commands

```powershell
cmake -S . -B build
cmake --build build --config Debug --target ReflectionTest -- /m:1
D:\VSProject\Nullus\Build\bin\Debug\ReflectionTest.exe
```

Expected output:

```text
=== All reflection tests passed ===
```
