# Feature Specification: Cross-Platform Dependency Setup

**Feature Branch**: `035-cross-platform-dependency-setup`
**Created**: 2026-05-25
**Status**: Implemented
**Input**: User description: "Add a cross-platform source-developer dependency setup flow that downloads Autodesk FBX SDK 2020.3.9 from official URLs for Windows, Linux, and macOS, requires explicit EULA acceptance for interactive and CI/headless runs, verifies SHA256 hashes, installs or unpacks into ThirdParty/FBX/sdk/<platform>, keeps SDK packages and unpacked SDK ignored, preserves the existing CMake bundled SDK discovery contract, and documents clone-to-build usage."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - One Command Local Dependency Setup (Priority: P1)

A source developer clones Nullus on Windows, Linux, or macOS and runs a repository-provided setup command that obtains the Autodesk FBX SDK for the current platform, verifies it, and prepares the existing `ThirdParty/FBX/sdk/<platform>` layout before the developer configures or builds the engine.

**Why this priority**: The current FBX SDK layout works only after a developer manually finds, downloads, accepts, and unpacks the Autodesk package. This is the main friction preventing clone-to-build source onboarding.

**Independent Test**: Start from a checkout without `ThirdParty/FBX/sdk/<platform>`, run the setup command interactively, accept the Autodesk EULA prompt, and verify the platform SDK root contains the expected headers and runtime library.

**Acceptance Scenarios**:

1. **Given** a supported platform checkout with no local FBX SDK, **When** the developer runs the setup command and explicitly accepts the Autodesk EULA prompt, **Then** the SDK is downloaded from the recorded official URL, hash-verified, installed into `ThirdParty/FBX/sdk/<platform>`, and reported ready for build configuration.
2. **Given** the SDK already exists and passes validation, **When** the developer runs the setup command again, **Then** the command reports the dependency is already satisfied and does not redownload by default.
3. **Given** a developer declines the EULA prompt, **When** setup runs interactively, **Then** setup exits without downloading or installing the Autodesk package and explains how to rerun it.

---

### User Story 2 - CI/Headless Dependency Setup (Priority: P1)

A CI job or headless build environment prepares Nullus dependencies without an interactive prompt by passing an explicit EULA acceptance flag or environment variable, then configures and builds against the same bundled SDK layout used by local developers.

**Why this priority**: The dependency setup must support automated validation without silently accepting Autodesk terms on behalf of the user or CI owner.

**Independent Test**: Run setup in non-interactive mode without acceptance and verify it fails before download; rerun with an explicit acceptance flag or environment variable and verify it downloads, verifies, installs, and exits successfully.

**Acceptance Scenarios**:

1. **Given** a headless environment without explicit Autodesk EULA acceptance, **When** setup runs, **Then** it fails before downloading SDK content with a clear message naming the required flag or environment variable.
2. **Given** a headless environment with explicit Autodesk EULA acceptance, **When** setup runs, **Then** it completes without prompts and prepares the same SDK root expected by CMake.
3. **Given** CI uses a restored local package cache, **When** setup verifies the cached package hash, **Then** it reuses the cache instead of downloading again.

---

### User Story 3 - Discoverable Clone-To-Build Documentation (Priority: P2)

A new source developer can read the repository docs or build script output and understand the exact dependency setup step required before building, including platform support, cache behavior, EULA acceptance, and how to diagnose SDK layout failures.

**Why this priority**: The setup script solves the mechanical work, but developers still need a reliable path when setup fails due to network, permissions, or package format differences.

**Independent Test**: Review the dependency README and run a build without the SDK; verify the guidance points to the setup command and describes the expected SDK layout.

**Acceptance Scenarios**:

1. **Given** a developer reads the repository setup docs, **When** they follow the documented clone-to-build flow, **Then** they run dependency setup before build and know how to pass explicit EULA acceptance in CI.
2. **Given** CMake cannot locate the SDK, **When** the configure message appears, **Then** it directs the developer to the setup command instead of leaving them to manually infer the `ThirdParty/FBX` layout.

### Edge Cases

- Network download fails, times out, or returns a partial file.
- A downloaded or cached package does not match the recorded SHA256.
- The package URL changes or the official host becomes unavailable.
- The platform is unsupported or has no manifest entry.
- The developer has insufficient permission to create files under `ThirdParty/FBX/sdk/<platform>`.
- Setup is interrupted midway and leaves a partial SDK directory.
- Windows, Linux, or macOS package installers require different extraction/install mechanics.
- The SDK exists but is incomplete, version-mismatched, or missing the runtime library.
- CI sets a misleading EULA variable value, such as `false`, `0`, or an empty string.
- The repository ignores local SDK output, but still needs to track the lightweight setup docs and manifest.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The repository MUST provide one documented dependency setup entrypoint usable from Windows, Linux, and macOS source checkouts.
- **FR-002**: The setup flow MUST support an interactive local mode that asks for explicit Autodesk EULA acceptance before downloading or installing Autodesk FBX SDK content.
- **FR-003**: The setup flow MUST support a CI/headless mode that requires explicit Autodesk EULA acceptance through a command-line flag or environment variable before downloading or installing Autodesk FBX SDK content.
- **FR-004**: The setup flow MUST download Autodesk FBX SDK 2020.3.9 packages from recorded official Autodesk URLs for Windows, Linux, and macOS.
- **FR-005**: The setup flow MUST verify downloaded and cached packages against recorded SHA256 hashes before extraction or installation.
- **FR-006**: The setup flow MUST install or unpack each platform package into the existing canonical bundled SDK root pattern: `ThirdParty/FBX/sdk/<platform>`.
- **FR-007**: The setup flow MUST validate the completed SDK root by checking the expected version header, top-level `fbxsdk.h`, platform link library, and platform runtime library.
- **FR-008**: The setup flow MUST be idempotent when the SDK root is already valid and MUST avoid redownloading by default.
- **FR-009**: The setup flow MUST maintain a local package cache that is not committed to the repository.
- **FR-010**: The repository MUST ignore downloaded packages and unpacked SDK outputs while tracking lightweight setup scripts, manifests, and documentation.
- **FR-011**: Build or configure guidance MUST direct developers to the dependency setup command when FBX SDK is missing or incomplete.
- **FR-012**: The setup flow MUST fail with actionable, platform-specific diagnostics for missing tools, failed downloads, hash mismatches, unsupported platforms, interrupted installs, and incomplete SDK layouts.
- **FR-013**: The change MUST preserve the existing CMake contract that consumes only the engine-bundled SDK layout and does not search external Autodesk FBX SDK installs.

### Key Entities

- **Dependency Manifest**: Versioned metadata describing the Autodesk FBX SDK package per platform, including official URL, SHA256, target SDK root, and expected validation files.
- **Setup Entrypoint**: The command developers and CI use to prepare source dependencies.
- **Package Cache**: A local ignored directory storing verified downloaded packages for reuse.
- **SDK Root**: The validated platform-specific unpacked SDK directory under `ThirdParty/FBX/sdk/<platform>`.
- **EULA Acceptance Signal**: A command-line flag or environment variable that records the caller's explicit choice to accept Autodesk terms for automated setup.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: On Windows, Linux, and macOS, a developer can move from a fresh source checkout to a valid FBX SDK root by running one documented setup command plus explicit EULA acceptance.
- **SC-002**: In headless mode without explicit EULA acceptance, setup fails before any Autodesk package download or install action.
- **SC-003**: With a valid package cache, setup completes without network access after verifying the cached package hash.
- **SC-004**: Corrupt or mismatched packages are rejected before extraction or installation with a message that names the expected and actual hash.
- **SC-005**: Re-running setup against a valid SDK root exits successfully without modifying the SDK root.
- **SC-006**: CMake configuration behavior remains compatible with `specs/029-fbx-sdk-thirdparty`: it resolves the SDK only from `ThirdParty/FBX/sdk/<platform>`.
- **SC-007**: Documentation and missing-SDK diagnostics make the clone-to-build flow discoverable without requiring developers to manually search for FBX SDK package links.

## Assumptions

- Source developers have Python 3.8 or newer available, or can use a thin platform wrapper that locates Python and reports a clear error when it is missing.
- The setup flow may invoke platform-native package/install tooling when direct archive extraction is not sufficient.
- Autodesk package downloads remain available at the recorded official URLs used by the existing `ThirdParty/FBX/README.md`.
- The setup tool will not redistribute Autodesk package contents through the Nullus repository; it will download from official URLs into local ignored paths.
- The first implementation focuses on Autodesk FBX SDK setup. The manifest format should allow future dependencies, but no additional dependency setup is required in this feature.
