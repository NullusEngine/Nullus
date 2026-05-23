# Implementation Plan: Remove Old Reflection Syntax

**Branch**: `remove-old-reflection-syntax` | **Date**: 2026-05-07 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `specs/remove-old-reflection-syntax/spec.md`

## Summary

Remove old reflection call-site forms by keeping MetaParser on the CppAst AST attribute path for generated reflection and moving external reflection to manual runtime registration macros.

## Technical Context

**Language/Version**: C++20 runtime, C# .NET 8 MetaParser
**Primary Dependencies**: CppAst.NET, existing Nullus reflection runtime
**Storage**: N/A
**Testing**: CMake targets `ReflectionTest` and `NullusUnitTests`
**Target Platform**: Windows development build, with existing cross-platform CMake flow preserved
**Project Type**: C++ engine/runtime with C# code generator
**Performance Goals**: Reflection generation and registration should remain build/startup scale equivalent to current flow
**Constraints**: Do not hand-edit `Runtime/*/Gen/`; do not add a second generation workflow
**Scale/Scope**: Reflection macros, MetaParser AST extraction, external reflection headers, reflection tests

## Constitution Check

Reflection changes require spec/plan/tasks and validation with `ReflectionTest` plus relevant unit tests. Generated outputs must be produced by the normal build flow.

## Project Structure

### Documentation

```text
specs/remove-old-reflection-syntax/
├── spec.md
├── plan.md
└── tasks.md
```

### Source Code

```text
Runtime/Base/Reflection/
├── Macros.h
├── ExternalReflectionRegistration.h
└── PrivateReflectionExternalSample.h

Runtime/Math/
└── ExternalReflection.h

Runtime/Engine/
├── Serialize/SceneSerializationData.h
├── Components/MeshRenderer.h
└── SceneSystem/Scene.h

Tools/MetaParser/src/
├── MetaParserTool.Core.cs
├── MetaParserTool.CppAstParser.cs
├── MetaParserTool.Generation.cs
└── Generation/GeneratorRegistry.cs
```

**Structure Decision**: Keep generated reflection in MetaParser and manual external registration in runtime headers.

## Complexity Tracking

No constitution violations.
