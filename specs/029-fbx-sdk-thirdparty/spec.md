# Feature Specification: Autodesk FBX SDK ThirdParty Integration

**Feature Branch**: `029-fbx-sdk-thirdparty`
**Created**: 2026-05-18
**Status**: Draft
**Input**: User description: "Put the official Autodesk FBX SDK packages into ThirdParty/FBX without a versioned path, integrate them into the engine, support Windows/Linux/macOS, and make FBX parsing use FBX SDK only with no fallback."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Configure FBX SDK From ThirdParty (Priority: P1)

An engine developer configures Nullus on Windows, Linux, or macOS and the build system resolves the Autodesk FBX SDK from the repository's `ThirdParty/FBX` location.

**Why this priority**: The importer cannot move away from Assimp until the dependency is deterministic and platform-aware.

**Independent Test**: Configure the project with CMake on a machine where the SDK is installed or unpacked and verify that an imported `Autodesk::FbxSdk` target is created with include, link, compile-definition, and runtime-copy behavior for the active platform.

**Acceptance Scenarios**:

1. **Given** a supported desktop platform and an unpacked engine-bundled Autodesk FBX SDK, **When** CMake configures Nullus, **Then** the engine links against FBX SDK and records the SDK version/source path in configuration output.
2. **Given** no usable FBX SDK installation, **When** CMake configures Nullus, **Then** configuration fails with a clear message explaining where the SDK packages live and which root path must be provided.

---

### User Story 2 - Parse FBX With Autodesk SDK Only (Priority: P1)

An editor user imports or drags an `.fbx` model and the engine parses it through Autodesk FBX SDK, not through Assimp.

**Why this priority**: The user-visible performance issue is Assimp FBX source parsing taking tens of seconds for large models.

**Independent Test**: Import a valid FBX source and verify logs, generated assets, and tests show `FbxSdkParser` is used for `.fbx`; invalid FBX files fail through FBX SDK diagnostics rather than silently routing to Assimp.

**Acceptance Scenarios**:

1. **Given** a valid `.fbx` asset, **When** the editor preimports or drag-drops it, **Then** the scene import path uses Autodesk FBX SDK and generates mesh/material/texture metadata.
2. **Given** an invalid `.fbx` asset, **When** import runs, **Then** the import fails with a clear FBX SDK parser diagnostic and does not retry through Assimp.

---

### User Story 3 - Preserve Non-FBX Model Importers (Priority: P2)

An editor user imports OBJ or glTF assets and those existing paths continue to work while FBX is moved to the new parser.

**Why this priority**: The change should target FBX performance without breaking other model workflows.

**Independent Test**: Run existing OBJ/glTF import tests and confirm Assimp remains available for OBJ while glTF uses the existing glTF pipeline.

**Acceptance Scenarios**:

1. **Given** an `.obj` model, **When** import runs, **Then** the existing Assimp OBJ pipeline remains active.
2. **Given** a `.gltf` or `.glb` model, **When** import runs, **Then** the existing glTF importer remains active.

### Edge Cases

- The SDK packages are present in `ThirdParty` but not installed/unpacked for the host platform.
- The host compiler differs from the SDK package naming, such as VS2022 building against a VS2022 FBX SDK package on Windows or GCC/Clang on Linux.
- The runtime dynamic library is found for compile/link but not copied beside editor/game/unit-test outputs.
- A platform-specific SDK package is unavailable or incomplete.
- A caller still tries to send `.fbx` through `AssimpParser`.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The repository MUST contain the official Autodesk FBX SDK package files under `ThirdParty/FBX/packages` with source URLs and hashes recorded.
- **FR-002**: The build system MUST expose an imported FBX SDK target for Windows, Linux, and macOS desktop builds.
- **FR-003**: The build system MUST fail configuration when a supported desktop build cannot locate usable FBX SDK headers, link libraries, and runtime libraries.
- **FR-004**: The build system MUST copy the FBX SDK runtime library to the same output location used by Nullus editor/game/test binaries.
- **FR-005**: `.fbx` source imports MUST use Autodesk FBX SDK parser code only.
- **FR-006**: `.fbx` source imports MUST NOT fall back to Assimp when FBX SDK parsing fails.
- **FR-007**: OBJ and glTF import behavior MUST remain available through their existing import paths.
- **FR-008**: Parser diagnostics MUST make it clear whether failure occurred during SDK discovery, FBX scene loading, triangulation, material extraction, or mesh extraction.
- **FR-009**: The implementation MUST preserve existing asset artifact IDs and importer registration semantics unless importer version changes are required to invalidate stale Assimp-derived artifacts.

### Key Entities

- **FBX SDK Package Set**: The platform-specific official Autodesk package archives stored in `ThirdParty/FBX/packages`, with unpacked SDK contents under `ThirdParty/FBX/sdk/<platform>`.
- **Autodesk::FbxSdk Target**: The CMake imported target carrying include paths, libraries, runtime DLL/shared library, and compile definitions.
- **FbxSdkParser**: The model parser responsible for loading `.fbx` source files and producing parsed meshes plus imported scene metadata.
- **Scene Import Request**: The editor/runtime import path that selects the parser based on source extension.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: CMake configuration succeeds on Windows, Linux, and macOS when the repository ThirdParty SDK layout contains a valid SDK installation for that platform.
- **SC-002**: CMake configuration fails within one configure pass with an actionable message when FBX SDK is missing or incomplete.
- **SC-003**: Automated tests or source-level contract checks prove `.fbx` import paths instantiate `FbxSdkParser` and do not instantiate `AssimpParser`.
- **SC-004**: A valid small FBX model can be imported through the new parser and produces at least one mesh, one scene node, and material metadata when present in the source.
- **SC-005**: Existing OBJ/glTF tests still pass after the FBX path changes.

## Assumptions

- The Autodesk SDK installers require EULA acceptance before the SDK can be unpacked into usable include/lib/bin directories.
- The repository stores the official package archives and the engine-bundled unpacked SDK layout under `ThirdParty/FBX`.
- The first implementation targets mesh, node transform, material name, and texture path import; animation, skinning, cameras, and lights are outside this immediate performance fix unless already cheap to expose from SDK data.
- Windows development uses the VS2022 package by default. Linux uses the GCC package. macOS uses the Clang universal package.
