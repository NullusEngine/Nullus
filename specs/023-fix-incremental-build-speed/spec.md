# Feature Specification: Fix Incremental Build Speed

**Feature Branch**: `023-fix-incremental-build-speed`
**Created**: 2026-05-11
**Status**: Draft
**Input**: User description: "Current compile speed is very slow; investigate the cause and start implementing the optimization plan."

## User Scenarios & Testing

### User Story 1 - Fast No-Change Developer Build (Priority: P1)

As a Nullus developer, I want a repeated build with no source changes to avoid regenerating unchanged reflection files and recompiling dependent C++ targets, so that local edit-test cycles are not dominated by stale generated timestamps.

**Why this priority**: The investigation showed a no-change `NullusUnitTests ReflectionTest` build still took 8 minutes 35 seconds and reran MetaParser plus downstream C++ compilation.

**Independent Test**: Run the same build command twice without editing files. The second run must not refresh `Runtime/*/Gen/MetaGenerated.*` or `Project/Editor/Gen/MetaGenerated.*` timestamps when generated content is unchanged.

**Acceptance Scenarios**:

1. **Given** generated reflection files already match the current inputs, **When** MetaParser is invoked again, **Then** unchanged generated files keep their previous modification time.
2. **Given** no source files changed between two builds, **When** the second build runs, **Then** reflection generation does not force dependent runtime modules to recompile.
3. **Given** a reflected header or template really changes, **When** the build runs, **Then** the affected generated files update and dependent C++ targets rebuild normally.

---

### User Story 2 - Configurable Windows Build Parallelism (Priority: P2)

As a Windows developer or CI maintainer, I want build scripts to use configurable MSBuild parallelism, so that targeted builds are not accidentally forced into single-process execution.

**Why this priority**: The investigation found `build_windows.bat` uses `/m:1` whenever `NLS_BUILD_TARGETS` is set, which amplifies slow rebuilds.

**Independent Test**: Run `build_windows.bat` with `NLS_BUILD_TARGETS` set and verify the generated build command uses the configured job count instead of hard-coded `/m:1`.

**Acceptance Scenarios**:

1. **Given** `NLS_BUILD_JOBS` is not set, **When** a targeted Windows build is launched, **Then** MSBuild parallelism is enabled with a conservative default worker count.
2. **Given** `NLS_BUILD_JOBS` is set to a positive number, **When** a targeted Windows build is launched, **Then** MSBuild receives `/m:<value>`.
3. **Given** existing CI needs to preserve single-worker behavior, **When** it sets `NLS_BUILD_JOBS=1`, **Then** the command remains equivalent to the previous serialized build.

### Edge Cases

- Generated output directories may not exist on a clean build; the build must still create them and write all required files.
- A generated file may exist with different encoding or line endings; content comparison must use the exact bytes that would be written.
- Stale generated files that are no longer expected must still be deleted when inputs change.
- Parallel reflection generation must not have multiple generator invocations copying the same native dependency files into one shared directory.
- The optimization must not hand-edit files under `Runtime/*/Gen/`.

## Requirements

### Functional Requirements

- **FR-001**: The reflection generator MUST avoid rewriting generated files when the generated content is byte-for-byte unchanged.
- **FR-002**: The reflection generator MUST continue writing generated files when generated content differs or the file does not exist.
- **FR-003**: The reflection generator MUST keep stale generated output cleanup behavior intact.
- **FR-004**: The build pipeline MUST keep MetaParser available before reflection generation commands execute.
- **FR-005**: Windows targeted builds MUST allow configurable MSBuild parallelism through an environment variable.
- **FR-006**: Documentation or script behavior MUST preserve a way to request single-worker Windows builds for CI or diagnostic use.
- **FR-007**: Parallel Windows reflection generation MUST avoid shared per-invocation file copy races.

## Success Criteria

### Measurable Outcomes

- **SC-001**: A repeated no-change build of `NullusUnitTests ReflectionTest` preserves all unchanged `MetaGenerated.*` timestamps.
- **SC-002**: A repeated no-change build avoids recompiling runtime modules solely because generated reflection outputs were touched.
- **SC-003**: A targeted Windows build can run with more than one MSBuild worker without editing the script.
- **SC-004**: Reflection and MetaParser validation still pass after the change.

## Assumptions

- The first implementation phase focuses on incrementality and Windows parallelism only.
- Third-party scope reduction for Assimp and Tracy profiler is valuable but will be handled in a separate change to reduce risk.
- Existing generated files remain generated output and are not manually edited.

---

### User Story 3 - Faster Full Developer Build (Priority: P1)

As a Nullus developer, I want a clean/full Windows Debug build to use available CPU parallelism and avoid compiling unused third-party format support, so that first-time setup and clean rebuilds finish fast enough for daily iteration.

**Why this priority**: A clean Debug build in a fresh Visual Studio build tree measured 10 minutes 42 seconds. The largest avoidable costs are project-internal serial MSVC compilation for large Nullus targets and Assimp compiling every importer/exporter even though the editor exposes only FBX/OBJ model import.

**Independent Test**: Configure a fresh Debug build tree with the optimized defaults, build it from scratch, and compare elapsed time plus successful `NullusUnitTests` and `ReflectionTest` execution against the baseline.

**Acceptance Scenarios**:

1. **Given** a fresh Windows Debug build tree, **When** the project is configured with default developer settings, **Then** MSVC C and C++ targets use multi-processor compilation without dropping required exception handling flags.
2. **Given** the default developer build, **When** Assimp is configured, **Then** only the model import formats currently exposed by Nullus are compiled by default and exporter code is disabled.
3. **Given** a developer or CI job requires broad Assimp format coverage, **When** it enables the documented full-format option, **Then** Assimp builds with its upstream default importer/exporter coverage.
4. **Given** built-in editor and engine models use FBX/OBJ assets, **When** optimized defaults are used, **Then** those model formats remain loadable.

### Full Build Edge Cases

- MSVC `/MP` must not replace CMake's default C++ exception handling flags such as `/EHsc`.
- Third-party libraries that already manage their own parallel compilation flags must continue to configure successfully.
- Assimp format pruning must preserve the formats exposed in editor import dialogs and used by built-in assets.
- The full-format Assimp escape hatch must be documented for compatibility testing and future asset pipeline expansion.

### Full Build Requirements

- **FR-008**: Windows MSVC builds MUST enable target-internal multi-processor compilation by default for C and C++ sources.
- **FR-009**: The implementation MUST preserve existing MSVC compile options and C++ exception handling behavior while adding multi-processor compilation.
- **FR-010**: Default Assimp configuration MUST compile only Nullus-supported model import formats unless an explicit full-format option is enabled.
- **FR-011**: Default Assimp configuration MUST disable Assimp exporters because Nullus currently uses Assimp for import/loading only.
- **FR-012**: A documented CMake option MUST allow full Assimp format/exporter coverage for compatibility validation.
- **FR-013**: Documentation MUST describe the faster full-build defaults, the full-format escape hatch, and the measured baseline/optimized build commands.

### Full Build Success Criteria

- **SC-005**: A fresh Windows Debug full build completes at least 40% faster than the measured 10:42 baseline on the same machine class.
- **SC-006**: Optimized full-build configuration still builds `NullusUnitTests`, `ReflectionTest`, `Editor`, `Game`, and `Launcher` successfully.
- **SC-007**: Optimized full-build configuration keeps FBX and OBJ model loading support available.
- **SC-008**: A full-format Assimp configuration remains available without source edits.
