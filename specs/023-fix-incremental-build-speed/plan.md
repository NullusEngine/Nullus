# Implementation Plan: Fix Incremental Build Speed

**Branch**: `023-fix-incremental-build-speed` | **Date**: 2026-05-11 | **Spec**: `specs/023-fix-incremental-build-speed/spec.md`
**Input**: Feature specification from `specs/023-fix-incremental-build-speed/spec.md`

## Summary

Fix the high-cost no-change build path by preventing MetaParser from touching generated reflection files when their content has not changed, by avoiding parallel MetaParser launcher copy races, and by replacing Windows targeted build hard-coding of `/m:1` with configurable parallelism. Keep this phase narrow: no third-party dependency pruning, no rendering behavior changes, and no manual edits to generated reflection outputs.

## Technical Context

**Language/Version**: C# .NET 8 MetaParser, CMake 3.18+, MSBuild/Visual Studio 2022, C++20  
**Primary Dependencies**: CMake custom targets, generated SDK-style MetaParser project, MSBuild  
**Storage**: Generated files under `Runtime/*/Gen/` and `Project/Editor/Gen/`  
**Testing**: PowerShell timestamp checks, targeted CMake/MSBuild builds, `NullusUnitTests`, `ReflectionTest`  
**Target Platform**: Windows first; generator behavior is cross-platform  
**Project Type**: C++ engine with C# build-time code generator  
**Performance Goals**: repeated no-change targeted builds must not recompile due to unchanged generated files  
**Constraints**: preserve MetaParser pipeline, do not hand-edit generated output, keep clean builds correct  
**Scale/Scope**: MetaParser generator writes and Windows build script only

## Constitution Check

- Spec-first scope: satisfied by this bundle.
- Validation matches subsystem: use MetaParser generation checks, targeted builds, and reflection/unit tests.
- Generated-file boundaries: generated outputs are observed but not hand-edited.
- Incremental delivery: implement content-stable generator writes first, then Windows parallelism.
- Product runtime preservation: no product runtime behavior changes.

## Project Structure

### Documentation

```text
specs/023-fix-incremental-build-speed/
├── spec.md
├── plan.md
└── tasks.md
```

### Source Code

```text
Tools/MetaParser/src/
└── MetaParserTool.Generation.cs

Runtime/
└── CMakeLists.txt

build_windows.bat
Docs/Testing.md
```

**Structure Decision**: Keep all changes at the existing generator and script boundaries. Do not introduce a parallel build workflow.

## Complexity Tracking

No constitution violations.

## Validation Plan

1. Record current timestamp-changing behavior with a focused MetaParser invocation.
2. Add content-stable writes in `MetaParserTool.Generation.cs`.
3. Remove shared per-invocation native dependency copies from the generated Windows MetaParser launchers.
4. Rebuild/regenerate CMake files and verify repeated generation preserves timestamps when content is unchanged.
5. Run targeted Windows build twice and compare elapsed time plus generated output timestamps.
6. Run `NullusUnitTests` and `ReflectionTest` when build validation succeeds.
