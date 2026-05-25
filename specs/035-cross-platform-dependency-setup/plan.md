# Implementation Plan: Cross-Platform Dependency Setup

**Branch**: `035-cross-platform-dependency-setup` | **Date**: 2026-05-25 | **Spec**: [spec.md](./spec.md)
**Input**: Feature specification from `specs/035-cross-platform-dependency-setup/spec.md`

## Summary

Add a UE-style source dependency setup flow for Nullus developers. The repository will track a lightweight dependency manifest, a cross-platform Python setup tool, platform wrappers, and documentation. The tool downloads Autodesk FBX SDK 2020.3.9 from recorded official URLs only after explicit Autodesk EULA acceptance, verifies package hashes, installs or unpacks into the existing `ThirdParty/FBX/sdk/<platform>` layout, and preserves the current CMake contract that consumes only this bundled SDK root.

## Technical Context

**Language/Version**: Python 3.8+ standard library, PowerShell/batch wrapper for Windows, POSIX shell wrapper for Linux/macOS, existing CMake/C++20 build
**Primary Dependencies**: Autodesk FBX SDK 2020.3.9 packages; platform-native installer/extraction tools where needed (`tar`, Windows installer extraction/install mode, macOS `pkgutil`/`installer`)
**Storage**: Tracked manifest/docs/scripts; ignored local package cache under `ThirdParty/FBX/packages`; ignored SDK output under `ThirdParty/FBX/sdk/<platform>`
**Testing**: Python `unittest` for manifest, EULA, hash, idempotency, and validation logic; dry-run/no-download setup modes for CI-safe tests; targeted CMake configure diagnostics when practical
**Target Platform**: Windows, Linux, and macOS source developer checkouts and CI/headless jobs
**Project Type**: Desktop game engine/editor repository workflow tooling
**Performance Goals**: Reuse a valid SDK root without work; reuse verified cached packages without network; fail before download when EULA acceptance is missing
**Constraints**: Do not commit Autodesk packages or unpacked SDK contents; do not silently accept Autodesk EULA; do not search system FBX SDK installs; preserve `ThirdParty/FBX/sdk/<platform>` as CMake's only FBX SDK source
**Scale/Scope**: One dependency family in this feature, with a manifest shape that can be extended later without adding more dependencies now

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- Spec-first major change: Pass. This bundle is `specs/035-cross-platform-dependency-setup` and scopes a repository workflow/tooling change.
- Validation matches subsystem: Pass. Validation will use script unit tests, dry-run CLI checks, hash/manifest checks, and CMake/source diagnostics for the build workflow.
- Generated code boundaries: Pass. No files under `Runtime/*/Gen/` or `Project/*/Gen/` are touched.
- Incremental verified delivery: Pass. The work is split into manifest/docs, setup tool tests, implementation, wrappers, CMake guidance, and validation.
- Product runtime preservation: Pass. Runtime parser/link behavior remains owned by `specs/029-fbx-sdk-thirdparty`; this feature only prepares the same SDK layout before build.

## Project Structure

### Documentation (this feature)

```text
specs/035-cross-platform-dependency-setup/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── contracts/
│   └── setup-dependencies-cli.md
└── tasks.md
```

### Source Code (repository root)

```text
SetupDependencies.bat
SetupDependencies.sh
Tools/SetupDependencies/
├── setup_dependencies.py
└── dependency_manifest.json

Tests/SetupDependencies/
└── test_setup_dependencies.py

ThirdParty/FBX/
└── README.md

.gitignore
ThirdParty/CMakeLists.txt
README.md
README.en.md
```

**Structure Decision**: Keep the setup implementation in `Tools/SetupDependencies` so it is clearly repository tooling, not runtime engine code. Root wrappers provide the UE-style entrypoint developers expect after clone. `ThirdParty/FBX/README.md` becomes tracked lightweight guidance, while `ThirdParty/FBX/packages/` and `ThirdParty/FBX/sdk/` stay ignored local outputs.

## Complexity Tracking

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|-------------------------------------|
| None | N/A | N/A |

## Phase 0: Research

See [research.md](./research.md). Key decisions:

- Use one Python standard-library implementation with thin platform wrappers.
- Require explicit EULA acceptance through `--accept-autodesk-eula` or `NLS_ACCEPT_AUTODESK_FBX_EULA=1`.
- Use a tracked JSON manifest for official URLs, SHA256 hashes, package filenames, and expected SDK validation files.
- Keep downloads and installed SDKs ignored; track only tooling, docs, and metadata.

## Phase 1: Design & Contracts

- Data model: [data-model.md](./data-model.md)
- CLI contract: [contracts/setup-dependencies-cli.md](./contracts/setup-dependencies-cli.md)
- Quickstart/validation scenarios: [quickstart.md](./quickstart.md)

## Post-Design Constitution Check

- Spec-first scope remains in one bundle.
- No generated files are touched.
- Platform claims are limited to script behavior validated in this environment; real SDK installer execution for Linux/macOS will be documented as expected behavior unless those hosts are available.
- CMake remains the consumer of `ThirdParty/FBX/sdk/<platform>` only.
- Final evidence path: Python unit tests, dry-run CLI checks, manifest validation, `.gitignore` checks, and source diagnostics for CMake guidance.
