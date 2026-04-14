# Feature Specification: RHI Framework Cleanup

**Feature Branch**: `002-rhi-framework-cleanup`
**Created**: 2026-04-03
**Updated**: 2026-04-08 (synced to runtime review, DX12 stabilization follow-up, and latest smoke evidence)
**Status**: In Progress
**Input**: User description: "制定完全移除 IRenderDevice的计划" + 2026-04-08 request to sync the spec bundle with actual runtime state

## Clarifications

### Session 2026-04-03

- Q: 后端实现范围 → A: **选项 C** - 与 DX12/Vulkan 同等级，作为 Tier A 后端实现，具有完整的 Formal RHI 支持，无兼容性封装层
- Q: 范围处理方式 → A: **选项 B** - 拆分为两个规范；当前规范专注于清理任务，Metal/DX11 实现需要单独规范

### Session 2026-04-03 (Extended)

- Q: Legacy 代码迁移策略 → A: **选项 A** - 全部迁移；Editor/Game 所有代码迁移到 Formal RHI，一次性完成
- Q: OpenGL 后端处理方式 → A: **选项 A** - 完全迁移到 Formal RHI，移除兼容性封装
- Q: 迁移执行顺序 → A: **选项 A** - 自底向上：Descriptor/Format → Backend → UI → 最后移除 Legacy

### Session 2026-04-08 (Review Sync)

- Q: 规格包如何与当前代码状态同步？ → A: 保持同一个 `002-rhi-framework-cleanup` bundle，不新建变更；把当前阶段明确为“稳定化 + 证据校准 + 重新打开未完成工作”
- Q: 当前后端支持如何表述？ → A: 只有有明确运行证据的 backend 才能继续被表述为 supported；未完成 backend 必须显式 unsupported、gated 或 fallback，不能依赖空指针路径或崩溃来暴露问题
- Q: 当前验证结论以什么为准？ → A: 2026-04-08 Windows review 里 `cmake --build build --config Debug` 与 `ctest --test-dir build -C Debug --output-on-failure` 通过；`Editor --backend vulkan <project>` 存活 6 秒；`DX12` 游戏默认路径在首帧命中 root-signature 错误；`DX12/DX11/OpenGL` Editor smoke 提前退出；`Game.exe --backend` 现阶段尚未真正接线，不能作为有效 backend 证据

## Current Evidence Snapshot

### 2026-04-08 Windows Review

- Build and current unit tests pass on Windows debug build
- `Editor --backend vulkan <project>` survives smoke startup and is the only backend with fresh Windows Editor runtime evidence in this review cycle
- Latest `DX12` game log shows `NativeDX12PipelineLayout: D3D12SerializeRootSignature failed: Unsupported RangeType value ...`, followed by graphics pipeline creation failure
- `Editor --backend dx12 <project>`, `Editor --backend dx11 <project>`, and `Editor --backend opengl <project>` all exited during smoke startup with access violation / device-loss style failure instead of graceful fallback
- `Game.exe` currently does not parse `--backend` or project-path arguments, so the simple backend scripts can report misleading Game results unless that routing is repaired

### 2026-04-08 DX12 Follow-Up

- `Runtime/Rendering/RHI/Backends/DX12/DX12ExplicitDeviceFactory.cpp` now builds pipeline-layout descriptor tables from owned storage instead of loop-local vectors, closing the reviewed dangling-pointer path.
- New focused Windows unit coverage in `Tests/Unit/DX12PipelineLayoutUtilsTests.cpp` and `Tests/Unit/DX12GraphicsPipelineUtilsTests.cpp` now exercises DX12 descriptor-table grouping and input-layout construction helpers.
- Fresh verification again passed with `cmake --build build --config Debug` and `ctest --test-dir build -C Debug --output-on-failure`.
- Latest default-backend Game smoke log `App/Win64_Debug_Runtime_Static/2026-04-08_11-48-17.log` no longer contains `Unsupported RangeType` or `D3D12SerializeRootSignature failed`, so the original reviewed DX12 root-signature corruption is fixed.
- The same latest Game smoke still reports `NativeDX12GraphicsPipeline: CreateGraphicsPipelineState failed with hr=-2147024809`, followed by `DXGI_ERROR_DEVICE_REMOVED`, which means DX12 remains unstable beyond the original root-signature bug.
- Latest short `Editor --backend dx12 <project>` smoke survives a 12-second window (`EXITCODE=STILL_RUNNING` before manual termination), so the earlier startup crash is no longer reproduced in this short-run follow-up. The same forced-stop run did not emit a fresh `.log` file, so evidence should continue to combine process-survival output with log-path checks.
- Startup fallback handling now treats missing explicit devices as an explicit failure path: `RHIDeviceFactory.cpp` reports unsupported outcomes clearly, `Driver.cpp` guards fallback/capability queries against `nullptr` devices, and `Editor/Game` contexts fail fast with explicit startup errors if requested backend creation and OpenGL fallback both fail.
- `Game` launch parsing now accepts `--backend` / `-b` and project path arguments, and threads both through `Application`/`Context`; short command-line smokes for `dx12` and `vulkan` both survive the validation window.
- Backend verification scripts (`verify_backends_simple.ps1`, `verify_all_backends.ps1`) now execute without parser errors and report requested backend plus per-process status/exit-code/log-path fields.

## User Scenarios & Testing

### User Story 1 - Unify TextureDesc and BufferDesc Structures (Priority: P1)

Engine developers need consistent texture and buffer descriptor definitions across the rendering system. Duplicate structures in `RHITypes.h` and legacy RHI descriptor surfaces cause confusion and potential runtime mismatches.

**Why this priority**: Descriptor duplication is a maintenance hazard that can lead to subtle rendering bugs and API confusion.

**Independent Test**: Can be verified by:
- Searching for duplicate runtime descriptor definitions returns only one authoritative location for engine-facing texture and buffer descriptors
- All texture and buffer creation code uses the unified structures
- No duplicate runtime descriptor types remain in active renderer codepaths

**Acceptance Scenarios**:

1. **Given** a developer creates a texture, **Then** the descriptor structure is defined in exactly one authoritative engine-facing location
2. **Given** existing code references duplicate descriptor definitions, **When** migration is complete, **Then** all references use the unified structure
3. **Given** a new field is added to a texture descriptor, **Then** developers update only one authoritative location

---

### User Story 2 - Unify UI Framework with Formal RHI (Priority: P1)

The UI rendering system currently uses a hybrid path with backend-specific implementations. UI rendering must use Formal RHI for every backend that is still claimed as supported at runtime, and incomplete backends must be explicitly gated instead of silently drifting onto legacy or null-renderer paths.

**Why this priority**: UI rendering is a critical path that must work consistently across any backend the product still exposes as supported.

**Independent Test**: Can be verified by:
- UI renders correctly using only Formal RHI resources for the backends still marked supported in the current phase
- UI backend selection follows project settings graphics backend or explicit fallback messaging
- No calls to legacy `PrepareUIRender()` survive in product UI code
- Frame timing remains stable on validated backends

**Acceptance Scenarios**:

1. **Given** a supported backend is selected, **When** UI is rendered, **Then** rendering uses Formal RHI resources and backend-appropriate command submission
2. **Given** UI textures are displayed, **Then** texture handles are resolved through Formal RHI interfaces without legacy extraction
3. **Given** a backend does not yet provide runnable UI support, **Then** startup reports explicit unsupported/fallback behavior instead of pretending the backend is fully supported

---

### User Story 3 - Type-Safe Native Handle Access (Priority: P2)

Backend developers need type-safe access to native device handles. The current migration replaces raw `void*` style resource access with tagged native-handle structures that are validated by backend type.

**Why this priority**: Type safety prevents runtime crashes and improves developer experience.

**Independent Test**: Can be verified by:
- Native handle accessors return tagged handles instead of untyped pointers
- Backend-specific code validates handle backend type before use
- No raw casting is required in ordinary consumer code

**Acceptance Scenarios**:

1. **Given** a developer accesses a native buffer handle, **Then** they receive a typed handle appropriate to the backend
2. **Given** code attempts to use a handle with incorrect backend assumptions, **Then** the runtime or compiler catches the mismatch before the resource is used unsafely
3. **Given** multiple backends are supported, **Then** each backend provides appropriately tagged handles

---

### User Story 4 - Unify Backend Factory Pattern (Priority: P2)

The current dual factory system (`CreateRenderDevice()` for legacy and `CreateExplicitDevice()` for Formal RHI) creates confusion about which API to use. Device creation must funnel through a single Formal RHI entry point, and unsupported backends must be surfaced explicitly rather than continuing into a null-device path.

**Why this priority**: Clear factory responsibilities reduce developer confusion and are a prerequisite for truthful backend readiness behavior.

**Independent Test**: Can be verified by:
- Single factory entry point exists for Formal RHI device creation
- Product startup does not dereference a null `RHIDevice` when a backend cannot be created
- Unsupported backend selection produces explicit fallback or unsupported behavior

**Acceptance Scenarios**:

1. **Given** a developer creates a rendering device, **When** they use the standard factory, **Then** they receive a Formal RHI device or an explicit unsupported result
2. **Given** the requested backend cannot create a usable device, **Then** product startup falls back or aborts cleanly instead of dereferencing a null `RHIDevice`

---

### User Story 5 - Remove Legacy IRenderDevice and Compatibility Layer (Priority: P1)

The legacy `IRenderDevice` interface and all compatibility wrappers must be removed from the active renderer architecture. Editor and Game applications must use Formal RHI directly, while unsupported backends rely on explicit capability reporting instead of hidden legacy fallback.

**Why this priority**: Eliminating duplicate abstractions reduces maintenance burden and prevents bugs from divergent implementations.

**Independent Test**: Can be verified by:
- No references to `IRenderDevice` remain in active renderer code
- No compatibility wrapper files remain in the active codepath
- Editor and Game runtime rendering use Formal RHI interfaces directly
- Unsupported backend behavior is explicit and capability-driven

**Acceptance Scenarios**:

1. **Given** a developer builds the project, **When** they search for `IRenderDevice`, **Then** no active renderer references remain
2. **Given** a developer searches for compatibility wrapper files, **Then** the removed wrapper layer does not reappear in active runtime codepaths
3. **Given** Editor application runs on a validated backend, **Then** rendering uses Formal RHI without legacy fallback
4. **Given** Game application runs on a validated backend, **Then** rendering uses Formal RHI without legacy fallback

---

### User Story 6 - Stabilize Backend Startup, Fallback, and Validation Truthfulness (Priority: P1)

Engine developers and QA need backend selection, startup behavior, and smoke-validation tooling to reflect reality. The current migration must stop reporting unvalidated backends as complete, must fix the reviewed `DX12` startup corruption, and must make Editor/Game backend selection and verification scripts trustworthy again.

**Why this priority**: Build-only validation is not enough for a multi-backend renderer. Runtime truthfulness is now the main blocker to claiming this migration is complete.

**Independent Test**: Can be verified by:
- `DX12` startup no longer fails with the reviewed root-signature corruption
- Unsupported or incomplete backends do not crash through null-device paths
- `Game.exe --backend <name> <project>` honors the requested backend or emits explicit fallback evidence
- `verify_all_backends.ps1` and `verify_backends_simple.ps1` execute successfully and report per-backend results

**Acceptance Scenarios**:

1. **Given** `DX12` is selected, **When** the first frame creates pipeline layouts, **Then** root-signature creation uses stable descriptor-range storage and no reviewed `Unsupported RangeType` error appears
2. **Given** an unsupported or not-ready backend is requested, **When** Editor or Game initializes, **Then** startup falls back explicitly or reports unsupported without null-device crashes
3. **Given** `Game.exe` is launched with `--backend` and a project path, **Then** runtime uses the requested backend or logs explicit fallback behavior tied to that request
4. **Given** the backend smoke scripts are run, **Then** they execute successfully and report backend-specific Editor/Game outcomes with usable log provenance

---

### Edge Cases

- What if some Editor panels still rely on legacy assumptions? Response: they may use high-level abstractions, but backend support claims must still be limited to the backends verified through current runtime evidence
- What if `DX11` Formal RHI remains incomplete after legacy removal? Response: `DX11` must not be advertised as a supported main-runtime backend until missing Formal RHI objects, UI behavior, and smoke validation are completed
- What if Metal is unavailable on the current Windows validation matrix? Response: Metal must be explicitly marked unsupported on unsupported builds/platforms and must not crash backend selection
- What if OpenGL startup regressed during the migration? Response: OpenGL must be repaired or explicitly gated in the current phase rather than assumed correct because it used to work
- What if backend creation fails before fallback evaluation? Response: startup must not dereference a null `RHIDevice`; failure must surface through explicit fallback or unsupported handling
- What if build/tests pass but runtime validation fails? Response: runtime evidence wins; task/spec state must be reopened until the backend matrix is truthful again

## Requirements

### Functional Requirements

- **FR-001**: System MUST define TextureDesc and BufferDesc structures in exactly one canonical engine-facing location
- **FR-002**: System MUST provide type-safe native handle accessors for all active resource types
- **FR-003**: System MUST route all UI rendering through Formal RHI for every backend currently exposed as supported at runtime; incomplete backends MUST be explicitly gated or marked unsupported
- **FR-004**: System MUST provide a unified backend factory that creates Formal RHI devices
- **FR-005**: System MUST remove the legacy `IRenderDevice` interface from the active renderer architecture
- **FR-006**: System MUST remove all active compatibility wrapper files and codepaths tied to the legacy renderer abstraction
- **FR-007**: System MUST migrate all Editor application rendering code to use Formal RHI-facing abstractions
- **FR-008**: System MUST migrate all Game application rendering code to use Formal RHI-facing abstractions
- **FR-009**: System MUST continue to pass the existing automated unit-test suite after migration updates
- **FR-010**: System MUST implement complete `DX11` Formal RHI before `DX11` is reported as a supported main-runtime backend
- **FR-011**: System MUST implement Metal Formal RHI or mark Metal unsupported on platforms/builds where it is not available
- **FR-012**: System MUST remove `IRenderDevice` inheritance from all active backend device implementations
- **FR-013**: System MUST fail gracefully or fall back explicitly when the requested backend cannot create a usable `RHIDevice`
- **FR-014**: System MUST honor backend selection inputs for both Editor and Game entrypoints and for backend verification tooling
- **FR-015**: System MUST build `DX12` pipeline layouts and root signatures from stable descriptor-range storage without runtime corruption
- **FR-016**: System MUST keep backend capability reporting, documentation, and validation scripts aligned with actual runtime behavior and recorded evidence

### Key Entities

- **TextureDesc**: Unified texture descriptor structure (single authoritative definition)
- **BufferDesc**: Unified buffer descriptor structure (single authoritative definition)
- **RHIDevice**: Formal RHI device interface (only device abstraction after migration)
- **RHIUIBridge**: UI rendering bridge that must reflect the truthful backend support matrix
- **NativeHandle**: Tagged native resource handle that replaces untyped `void*` access
- **RuntimeBackendFallbackDecision**: Capability-driven startup decision that must remain valid even when backend creation fails
- **Backend Smoke Script**: PowerShell validation entrypoint that must route to the requested backend and report trustworthy results

## Success Criteria

### Measurable Outcomes

- **SC-001**: No duplicate runtime TextureDesc/BufferDesc structures exist in the active rendering codebase
- **SC-002**: UI renders correctly via Formal RHI on every backend still claimed as supported in the current phase
- **SC-003**: Native handle accessors return tagged handles instead of untyped pointers
- **SC-004**: All device creation flows through a single Formal RHI factory entry point
- **SC-005**: No active renderer references to `IRenderDevice` remain in the codebase
- **SC-006**: No active compatibility wrapper files remain in the supported runtime path
- **SC-007**: Existing automated unit tests continue to pass after each stabilization step
- **SC-008**: Editor and Game applications render correctly without legacy dependencies on the backends explicitly validated in the current phase
- **SC-009**: CI and local build/test entrypoints complete successfully after stabilization updates
- **SC-010**: `DX11` is either fully validated as runnable through Formal RHI or explicitly reported unsupported with no null-device crash path
- **SC-011**: Metal is either fully implemented on the platform where it is exposed or explicitly reported unsupported with no partial-state startup path
- **SC-012**: No backend class inherits from `IRenderDevice`
- **SC-013**: Requesting an unsupported backend never causes a null `explicitDevice` dereference during startup
- **SC-014**: `Game.exe --backend <name> <project>` and backend smoke scripts route to the requested backend or emit explicit fallback evidence
- **SC-015**: `verify_all_backends.ps1` executes without PowerShell parse errors and reports per-backend results
- **SC-016**: Windows `DX12` startup smoke no longer emits the reviewed `Unsupported RangeType value` root-signature error
- **SC-017**: Post-fix DX12 validation evidence distinguishes the original root-signature corruption from later PSO/runtime failures instead of collapsing them into one generic crash result

## Assumptions

- Descriptor structure deduplication remains a pure refactoring with no intended serialized-data changes
- Build and unit-test success do not prove backend correctness; runtime/backend-specific evidence is required before a support claim is considered complete
- The 2026-04-08 Windows review is the current authoritative runtime snapshot for this bundle until newer evidence is recorded
- Metal is not expected to be runnable on the current Windows-focused validation matrix; explicit unsupported reporting is acceptable there
- `DX11`, `OpenGL`, and `DX12` support statements must remain provisional until the stabilization tasks restore truthful runtime evidence

## Scope Changes

| Date | Change | Rationale |
|------|--------|-----------|
| 2026-04-03 | DX11/Metal out of scope | Original spec had limited scope |
| 2026-04-04 | DX11/Metal in scope | User requested complete `IRenderDevice` removal |
| 2026-04-08 | Added stabilization and validation-truthfulness scope | Runtime review showed the migration bundle had drifted ahead of actual backend evidence |
