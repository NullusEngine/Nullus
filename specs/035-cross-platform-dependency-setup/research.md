# Research: Cross-Platform Dependency Setup

## Decision: Use one Python standard-library setup tool with thin wrappers

**Rationale**: The dependency workflow needs identical manifest parsing, EULA handling, hash validation, cache behavior, and SDK-root validation across Windows, Linux, and macOS. Python keeps that logic in one place while root `SetupDependencies.bat` and `SetupDependencies.sh` provide familiar platform entrypoints.

**Alternatives considered**:

- Three native scripts: rejected because URL/hash/platform validation would drift across PowerShell, shell, and macOS-specific branches.
- CMake download/install during configure: rejected because dependency acquisition and EULA acceptance should be explicit before configure, not hidden in build generation.
- Committing SDK packages or unpacked SDK contents: rejected because packages are large local artifacts and Autodesk terms require explicit user acceptance.

## Decision: Explicit Autodesk EULA acceptance is mandatory before download/install

**Rationale**: Local developers can answer an interactive prompt. CI must provide an auditable signal, either `--accept-autodesk-eula` or `NLS_ACCEPT_AUTODESK_FBX_EULA=1`. Values such as empty, `0`, `false`, and `no` are not acceptance.

**Alternatives considered**:

- Download first, ask before install: rejected because downloading Autodesk package content is still dependency acquisition that should be guarded.
- Assume acceptance in CI: rejected because headless automation must not silently accept terms on behalf of the CI owner.

## Decision: Track a manifest, not SDK payloads

**Rationale**: A JSON manifest can record official URLs, package filenames, SHA256 hashes, expected SDK roots, and validation files in a reviewable format. It is small enough for Git and becomes the single source of truth for docs, setup, and tests.

**Alternatives considered**:

- Hardcode metadata in Python: rejected because docs/tests cannot consume it cleanly and changes are harder to review.
- Keep metadata only in README prose: rejected because setup needs structured data.

## Decision: Preserve the existing CMake bundled-SDK contract

**Rationale**: `specs/029-fbx-sdk-thirdparty` already established that CMake consumes only `ThirdParty/FBX/sdk/<platform>` and does not search external SDK installs. The new setup flow prepares that layout; it does not change parser routing or dependency discovery semantics.

**Alternatives considered**:

- Search user-installed Autodesk SDKs as fallback: rejected because it weakens reproducibility and conflicts with the existing contract.
- Change CMake to auto-run setup: rejected because configure-time downloads are opaque and harder to diagnose in CI.

## Decision: Use dry-run and validation-only modes for tests

**Rationale**: Automated tests must not download large third-party packages or run platform installers. Dry-run mode can validate platform selection, EULA gating, manifest parsing, cache/hash behavior, and destination planning. Validation-only mode can check a synthetic SDK root in tests.

**Alternatives considered**:

- Integration tests that download real packages: rejected for routine test runs because they are slow, network-dependent, and require explicit license acceptance.
- No tests for setup: rejected because EULA gating and hash validation are core safety behavior.
