# Tasks: Cross-Platform Dependency Setup

**Input**: Design documents from `specs/035-cross-platform-dependency-setup/`
**Prerequisites**: `plan.md`, `spec.md`, `research.md`, `data-model.md`, `contracts/setup-dependencies-cli.md`, `quickstart.md`

**Tests**: Included because the feature changes developer workflow behavior and has a stable Python CLI test entrypoint.

**Organization**: Tasks are grouped by user story to enable independent implementation and testing.

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Establish tracked metadata and ignored local dependency outputs.

- [X] T001 Update `.gitignore` so `ThirdParty/FBX/README.md` and setup metadata can be tracked while `ThirdParty/FBX/packages/` and `ThirdParty/FBX/sdk/` remain ignored.
- [X] T002 [P] Add tracked `ThirdParty/FBX/README.md` documenting official Autodesk URLs, hashes, EULA expectations, and the canonical SDK layout.
- [X] T003 [P] Add `Tools/SetupDependencies/dependency_manifest.json` with Autodesk FBX SDK 2020.3.9 metadata for Windows, Linux, and macOS.

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Create testable setup-tool structure before user-story implementation.

- [X] T004 Create `Tools/SetupDependencies/setup_dependencies.py` CLI skeleton with argument parsing and no download/install side effects.
- [X] T005 Create root wrappers `SetupDependencies.bat` and `SetupDependencies.sh` that forward arguments to the Python CLI.
- [X] T006 Create `Tests/SetupDependencies/test_setup_dependencies.py` test harness using Python `unittest` and temporary repository roots.
- [X] T007 Verify the initial setup dependency tests fail for missing manifest loading, EULA gating, SDK validation, and cache/hash behavior. Evidence: `py -3 -m unittest discover -s Tests/SetupDependencies -v` first failed with `ModuleNotFoundError: No module named 'setup_dependencies'`.

**Checkpoint**: Test harness exists and proves behavior is not implemented yet.

---

## Phase 3: User Story 1 - One Command Local Dependency Setup (Priority: P1) MVP

**Goal**: Local developers can run one setup command to prepare a valid FBX SDK root after explicit EULA acceptance.

**Independent Test**: Use the Python tests and dry-run CLI to verify local mode prompts/gates acceptance, validates existing SDK roots, plans downloads, verifies cached package hashes, and reports success without modifying valid SDK roots.

### Tests for User Story 1

- [X] T008 [P] [US1] Add tests in `Tests/SetupDependencies/test_setup_dependencies.py` for interactive EULA decline blocking download/install.
- [X] T009 [P] [US1] Add tests in `Tests/SetupDependencies/test_setup_dependencies.py` for valid existing SDK root idempotency.
- [X] T010 [P] [US1] Add tests in `Tests/SetupDependencies/test_setup_dependencies.py` for package SHA256 verification and mismatch diagnostics.
- [X] T011 [P] [US1] Add tests in `Tests/SetupDependencies/test_setup_dependencies.py` for SDK validation requiring `fbxsdk.h`, version header, link library, and runtime library.

### Implementation for User Story 1

- [X] T012 [US1] Implement manifest loading and platform package resolution in `Tools/SetupDependencies/setup_dependencies.py`.
- [X] T013 [US1] Implement EULA prompt/acceptance gating in `Tools/SetupDependencies/setup_dependencies.py`.
- [X] T014 [US1] Implement package cache path resolution, SHA256 verification, and safe download planning in `Tools/SetupDependencies/setup_dependencies.py`.
- [X] T015 [US1] Implement SDK root validation and idempotent already-valid behavior in `Tools/SetupDependencies/setup_dependencies.py`.
- [X] T016 [US1] Implement platform install/extraction dispatch and actionable diagnostics in `Tools/SetupDependencies/setup_dependencies.py`.
- [X] T017 [US1] Run `python -m unittest discover -s Tests/SetupDependencies` and verify US1 tests pass. Evidence: `py -3 -m unittest discover -s Tests/SetupDependencies -v` passed 44/44 after final-audit fixes, including Windows x64/arm64 SDK layout validation, SDK symlink/reparse/mount redirection rejection, pre-cache safety failure, manifest hardening, and transient download retry.

**Checkpoint**: The setup CLI supports local one-command dependency setup behavior and validation.

---

## Phase 4: User Story 2 - CI/Headless Dependency Setup (Priority: P1)

**Goal**: CI can run setup non-interactively only when explicit Autodesk EULA acceptance is provided.

**Independent Test**: Run non-interactive setup tests with and without `NLS_ACCEPT_AUTODESK_FBX_EULA=1` and verify missing acceptance fails before download while accepted runs proceed through dry-run/cache validation.

### Tests for User Story 2

- [X] T018 [P] [US2] Add tests in `Tests/SetupDependencies/test_setup_dependencies.py` for non-interactive missing EULA acceptance failing before download.
- [X] T019 [P] [US2] Add tests in `Tests/SetupDependencies/test_setup_dependencies.py` for `NLS_ACCEPT_AUTODESK_FBX_EULA=1` enabling non-interactive dry-run setup.
- [X] T020 [P] [US2] Add tests in `Tests/SetupDependencies/test_setup_dependencies.py` for rejected environment values such as `0`, `false`, and empty strings.

### Implementation for User Story 2

- [X] T021 [US2] Implement non-interactive mode and environment acceptance parsing in `Tools/SetupDependencies/setup_dependencies.py`.
- [X] T022 [US2] Ensure dry-run mode performs no filesystem modifications beyond reading manifests in `Tools/SetupDependencies/setup_dependencies.py`.
- [X] T023 [US2] Run CI-style dry-run commands through `SetupDependencies.bat` and `SetupDependencies.sh` where supported by the host shell. Evidence: `.\SetupDependencies.bat --dry-run --accept-autodesk-eula` passed on Windows; `bash ./SetupDependencies.sh --dry-run --accept-autodesk-eula` passed through the available shell environment; `py -3 Tools\SetupDependencies\setup_dependencies.py --platform windows --arch ARM64 --dry-run --accept-autodesk-eula` passed and reported `windows (arm64)`; subprocess installer paths use a bounded timeout; downloads use unique same-directory temporary files before atomic replacement; tooling documents and checks Python 3.8+; Windows/POSIX wrapper portability is covered by source-level tests; `SetupDependencies.sh` is staged as executable `100755`.

**Checkpoint**: CI/headless setup has explicit acceptance semantics and deterministic dry-run behavior.

---

## Phase 5: User Story 3 - Discoverable Clone-To-Build Documentation (Priority: P2)

**Goal**: Developers can discover setup steps from docs and missing-SDK configure diagnostics.

**Independent Test**: Read docs and inspect CMake missing-SDK messages to verify they name the setup command and expected SDK layout.

### Tests for User Story 3

- [X] T024 [P] [US3] Add source-level test or assertion in `Tests/SetupDependencies/test_setup_dependencies.py` verifying docs/manifest mention all three supported platforms.
- [X] T025 [P] [US3] Add source-level test in `Tests/SetupDependencies/test_setup_dependencies.py` verifying `ThirdParty/CMakeLists.txt` missing-SDK guidance mentions `SetupDependencies`.

### Implementation for User Story 3

- [X] T026 [US3] Update `ThirdParty/CMakeLists.txt` missing-SDK diagnostics to point developers at `SetupDependencies`.
- [X] T027 [US3] Update `README.md` and `README.en.md` clone-to-build guidance to include dependency setup before build.
- [X] T028 [US3] Update `ThirdParty/FBX/README.md` with local and CI setup examples.
- [X] T029 [US3] Run documentation/source-level tests for platform coverage and CMake guidance. Evidence: `py -3 -m unittest discover -s Tests/SetupDependencies -v` passed source-level docs/CMake checks, including strict `NLS_REQUIRE_BUNDLED_FBX_SDK=ON` guidance and README/manifest URL/hash consistency.

**Checkpoint**: New source developers can discover the setup command from docs and configure errors.

---

## Phase 6: Polish & Cross-Cutting Concerns

**Purpose**: Validate the full workflow and keep artifacts in sync.

- [X] T030 Run `python Tools/SetupDependencies/setup_dependencies.py --dry-run --accept-autodesk-eula` and record behavior. Evidence: `py -3 Tools\SetupDependencies\setup_dependencies.py --dry-run --accept-autodesk-eula` passed, planned the Windows SDK/cache paths, and printed `Dry run: No files were modified.` `--platform windows --arch ARM64` also passed and planned the arm64 SDK validation path.
- [X] T031 Run `python Tools/SetupDependencies/setup_dependencies.py --non-interactive --dry-run` and verify missing EULA acceptance fails before download. Evidence: command exited 1 with guidance to pass `--accept-autodesk-eula` or `NLS_ACCEPT_AUTODESK_FBX_EULA=1`.
- [X] T032 Run `python -m unittest discover -s Tests/SetupDependencies`. Evidence: `py -3 -m unittest discover -s Tests/SetupDependencies -v` passed 44/44; the x64 rejection test was temporarily verified red against the old architecture-agnostic release-library matching.
- [X] T033 Run a source check confirming `ThirdParty/FBX/packages/` and `ThirdParty/FBX/sdk/` are ignored while `ThirdParty/FBX/README.md` is trackable. Evidence: `git check-ignore -v ThirdParty/FBX/packages/ ThirdParty/FBX/sdk/` matched `.gitignore`; `git check-ignore -q ThirdParty/FBX/README.md` reported trackable.
- [X] T034 Run `/plan-review` quality gate to the repository-required threshold and address P0/P1 findings before reporting completion. Evidence: multi-agent review found and verified fixes for SDK path confinement, wrapper portability, manifest diagnostics, CMake strict guidance, and retry robustness; final deeper audit reported no P0/P1/P2 and independently reran 44/44 tests plus the symlinked `ThirdParty` cache-escape repro.
- [X] T035 Update this `tasks.md` with completed task checkboxes and validation evidence.

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies.
- **Foundational (Phase 2)**: Depends on Phase 1 metadata and ignore decisions.
- **User Stories (Phases 3-5)**: Depend on the CLI skeleton and test harness from Phase 2.
- **Polish (Phase 6)**: Depends on desired user stories being complete.

### User Story Dependencies

- **US1 (P1)**: Starts after Phase 2 and is the MVP.
- **US2 (P1)**: Starts after Phase 2; shares EULA/cache code with US1 and should be completed before CI docs are finalized.
- **US3 (P2)**: Starts after setup command behavior is stable enough to document.

### Parallel Opportunities

- T002 and T003 can run in parallel.
- US1 tests T008-T011 can be authored in parallel before implementation.
- US2 tests T018-T020 can be authored in parallel.
- US3 documentation/source checks T024-T025 can be authored in parallel.

## Implementation Strategy

1. Complete Phase 1 and Phase 2.
2. Implement US1 as the MVP with tests first.
3. Implement US2 CI semantics with tests first.
4. Add US3 docs/CMake guidance.
5. Run final validation and plan-review.
