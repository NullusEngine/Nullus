# Tasks: Shader Cache Uses Project Library

**Input**: Design documents from `specs/fix-shader-cache-library-root/`
**Prerequisites**: plan.md, spec.md
**Tests**: Required by FR-004 and TDD workflow.

## Phase 1: Setup

- [X] T001 Confirm existing shader cache path flow in `Runtime/Rendering/Resources/Loaders/ShaderLoader.cpp` and `Runtime/Core/ResourceManagement/ShaderManager.cpp`

## Phase 2: User Story 1 - Shader Cache Stays With Project (Priority: P1)

**Goal**: Configured project shader loads use `<project>/Library/ShaderCache/ShaderCache.tsv` even when the shader source lives under `App/Assets`.

**Independent Test**: Unit test cache path resolution with a configured project assets root and an engine shader source path.

### Tests

- [X] T002 [US1] Add failing unit tests in `Tests/Unit/ShaderCompilerTests.cpp` for configured project library cache path and direct `App/Assets` fallback guard

### Implementation

- [X] T003 [US1] Add testable cache database path resolver API in `Runtime/Rendering/Resources/Loaders/ShaderLoader.h`
- [X] T004 [US1] Implement resolver preference for configured project assets root in `Runtime/Rendering/Resources/Loaders/ShaderLoader.cpp`
- [X] T005 [US1] Pass `ShaderManager` project assets root to shader loader in `Runtime/Core/ResourceManagement/ShaderManager.cpp`
- [X] T006 [US1] Run focused unit test and related shader compiler cache tests

## Phase 3: User Story 2 - Shader Binaries Stay With Project (Priority: P1)

**Goal**: Configured project shader cache database paths also place generated DXIL/SPIR-V binaries under `<project>/Library/ShaderCache`.

**Independent Test**: Compile a small shader with `ShaderCompiler::SetCacheDatabasePath(<project>/Library/ShaderCache/ShaderCache.tsv)` and verify the successful `.dxil` output path lives in the same directory.

### Tests

- [X] T009 [US2] Add failing unit test in `Tests/Unit/ShaderCompilerTests.cpp` for configured project shader binary artifact output

### Implementation

- [X] T010 [US2] Add compiler artifact directory resolution from configured cache database in `Runtime/Rendering/ShaderCompiler/ShaderCompiler.cpp`
- [X] T011 [US2] Preserve user-local fallback when no cache database path is configured
- [X] T012 [US2] Run focused shader compiler cache tests

## Phase 4: Polish

- [X] T007 Self-review for regressions, missing tests, and project/library path assumptions
- [X] T008 Run plan-review gate before final completion report
- [X] T013 Self-review updated artifact relocation behavior
- [X] T014 Run plan-review gate before final completion report
