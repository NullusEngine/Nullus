# Feature Specification: Shader Cache Uses Project Library

**Feature Branch**: `fix-shader-cache-library-root`
**Created**: 2026-05-18
**Status**: Implemented
**Input**: User description: "Library目录不应该在App目录下，应该在项目目录下"

## User Scenarios & Testing

### User Story 1 - Shader Cache Stays With Project (Priority: P1)

When the editor or game compiles shaders while a project is open, generated shader cache metadata is written under the opened project's `Library` directory, not under the engine `App` directory.

**Why this priority**: `App` contains engine assets and build output; project-local generated cache data must not pollute it.

**Unity reference model**: Unity treats built-in/prebuilt engine assets as reusable source resources, while imported game-ready data, artifact databases, and cache data are stored in the current project's `Library`. Nullus follows the same boundary: `App/Assets/Engine` is a built-in source root, never a generated cache root.

**Independent Test**: Configure project assets and engine assets roots, request an engine shader through the shader resource path, and verify the compiler cache database path resolves to `<project>/Library/ShaderCache/ShaderCache.tsv`.

**Acceptance Scenarios**:

1. **Given** a project root with `Assets` and an engine shader under `App/Assets`, **When** a shader is loaded through a configured shader manager, **Then** shader cache metadata is associated with the project root's `Library/ShaderCache`.
2. **Given** a source shader path under a project-owned `Assets` folder and no configured project root, **When** cache path fallback is used, **Then** the cache path remains beside that source asset root for compatibility.
3. **Given** a direct shader source path under `App/Assets` and no configured project root, **When** cache path fallback is used, **Then** no shader cache database path is produced for `App/Library`.

### Edge Cases

- Direct `ShaderLoader` callers without configured project assets keep the existing source-path fallback for non-`App` asset roots.
- Direct `ShaderLoader` callers for engine shaders under `App/Assets` must not create `App/Library`.
- Empty project asset roots do not produce malformed cache paths.
- Relative and absolute project asset roots normalize to a stable project-root cache path.

## Requirements

### Functional Requirements

- **FR-001**: Shader cache database paths MUST prefer the currently configured project `Assets` root when one is available.
- **FR-002**: Engine shader source paths under `App/Assets` MUST NOT cause cache metadata to be written under `App/Library`.
- **FR-003**: Existing direct shader source loading MUST retain a fallback cache location derived from the source asset root when no project root is configured, except for `App/Assets` engine roots.
- **FR-004**: The fix MUST be covered by a focused unit test that fails before implementation and passes after.

## Success Criteria

### Measurable Outcomes

- **SC-001**: A unit test proves configured project shader loads resolve the cache database to `<project>/Library/ShaderCache/ShaderCache.tsv`.
- **SC-002**: Loading engine shaders no longer creates or updates `App/Library/ShaderCache/ShaderCache.tsv` through configured project resource loading.
- **SC-003**: Existing shader compiler cache database persistence tests continue to pass.
- **SC-004**: A unit test proves direct engine shader fallback does not infer `App/Library/ShaderCache/ShaderCache.tsv`.

## Assumptions

- The active project root is the parent directory of the configured project `Assets` path.
- Engine built-in assets may still live under `App/Assets`; generated project cache data belongs under the active project `Library`, following Unity's source-vs-artifact split.
- Existing generated files under `Runtime/*/Gen/` are out of scope and must not be edited.
