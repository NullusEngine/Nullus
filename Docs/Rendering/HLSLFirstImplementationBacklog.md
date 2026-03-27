# HLSL-First Implementation Backlog

## Purpose

This backlog turns the rendering migration into executable work items. The target architecture is:

- HLSL-first shader authoring
- one maintained shader source per feature
- generated shader products for OpenGL, Vulkan, and DX12
- reflection-driven resource binding
- renderer and material systems consuming RHI-facing shader metadata instead of OpenGL-era string bindings

## Current State Snapshot

Observed from current codebase:

- backend scaffolding exists for OpenGL, Vulkan, and DX12 selection paths
- renderer-side FrameGraph usage is moving toward RHI types
- shader assets are still primarily GLSL in `App/Assets/Engine/Shaders`
- `Shader`, `Material`, and `ShaderLoader` are still strongly OpenGL-oriented
- Vulkan and DX12 device layers exist, but scene rendering is not yet backend-native end to end

This backlog assumes migration continues from that state without reverting in-flight work from other contributors.

## Task Format

Each task includes:

- `ID`
- `Goal`
- `Scope`
- `Dependencies`
- `Acceptance`
- `Risks`

## Phase A: Foundation

### RHI-SHADER-001

Goal:
Define the canonical HLSL-first shader pipeline contract.

Scope:

- align shader naming, stage naming, register-space rules, and source layout with [ShaderConventions.md](/d:/VSProject/Nullus/Docs/Rendering/ShaderConventions.md)
- document backend output formats per target
- define generated artifact layout and ownership

Dependencies:

- none

Acceptance:

- `Docs/Rendering/ShaderConventions.md` is approved by rendering owners
- a short architecture note exists in the backlog summary for artifact flow: `HLSL -> compiler -> reflection -> runtime metadata -> backend binary`
- no open disagreement remains on space allocation for Frame/Pass/Material/Object

Risks:

- inconsistent binding conventions across teams will cause reflection and material work to fork later

### RHI-SHADER-002

Goal:
Create the shader source tree and generated output layout without switching runtime yet.

Scope:

- add canonical source directories for HLSL and shared includes
- add generated output directories for DXIL, SPIR-V, translated OpenGL artifacts, and reflection metadata
- define naming for permutation outputs

Dependencies:

- `RHI-SHADER-001`

Acceptance:

- build system or asset pipeline can reference deterministic source and output roots
- directory naming is documented and referenced by compiler tasks
- no runtime code still assumes shaders only live under legacy GLSL asset names for new passes

Risks:

- if output naming is unstable, hot-reload and cache invalidation will become fragile

## Phase B: Shader Compiler And Artifact Pipeline

### RHI-SHADER-010

Goal:
Implement `ShaderCompiler` as a backend-agnostic service entry point.

Scope:

- introduce a compiler service API that accepts HLSL source, stage, entry point, macro set, and backend target
- define target outputs for:
  - DX12: DXIL
  - Vulkan: SPIR-V
  - OpenGL: translated OpenGL-consumable shader artifact from the chosen toolchain
- define error reporting format consumed by editor/runtime tools

Dependencies:

- `RHI-SHADER-001`
- `RHI-SHADER-002`

Acceptance:

- a CLI or test harness can compile one trivial vertex+pixel HLSL shader to all configured targets
- failure output reports source file, entry point, stage, and compiler diagnostics
- `cmake --build build --config Debug --target NLS_Render` succeeds with compiler integration enabled

Risks:

- OpenGL translation path is the highest uncertainty item
- toolchain choices may constrain HLSL feature usage

### RHI-SHADER-011

Goal:
Add deterministic shader cache keys and build products.

Scope:

- define cache key inputs: file hash, includes, defines, target backend, shader model, entry point
- write compiled outputs and metadata to stable artifact paths
- support incremental rebuilds

Dependencies:

- `RHI-SHADER-010`

Acceptance:

- recompiling unchanged shaders is a cache hit
- changing one include invalidates dependent outputs
- cache behavior is verifiable with two consecutive `NLS_Render` builds

Risks:

- missing include dependency tracking will silently serve stale binaries

### RHI-SHADER-012

Goal:
Add shader compilation smoke tests.

Scope:

- add one minimal compile smoke target for `VSMain` + `PSMain`
- add one compute compile smoke target
- validate outputs for OpenGL, Vulkan, and DX12 paths where toolchains are present

Dependencies:

- `RHI-SHADER-010`
- `RHI-SHADER-011`

Acceptance:

- CI or local test step fails if any backend artifact generation fails
- minimum local validation command is documented
- `cmake --build build --config Debug --target NLS_Render NLS_Engine` passes after smoke integration

Risks:

- backend toolchain optionality may require feature flags for developer machines

## Phase C: Reflection

### RHI-REFLECT-020

Goal:
Generate reflection metadata from compiled shader outputs.

Scope:

- extract resource bindings, register spaces, stage visibility, arrays, and cbuffer layouts
- define one stable reflection schema consumed by runtime
- support at least textures, samplers, cbuffers, storage buffers, and UAVs

Dependencies:

- `RHI-SHADER-010`

Acceptance:

- one sample shader produces machine-readable reflection metadata
- metadata includes resource name, kind, set/space, binding/register, and cbuffer member offsets
- reflection schema version is documented

Risks:

- reflection shape may diverge between DXIL and SPIR-V unless normalized explicitly

### RHI-REFLECT-021

Goal:
Wire reflection metadata into runtime `Shader` objects.

Scope:

- replace OpenGL-only reflection gathering with compiler/reflection-backed metadata ingestion
- keep runtime-facing shader resource layout backend-neutral
- support hot reload path

Dependencies:

- `RHI-REFLECT-020`

Acceptance:

- loading a migrated shader does not depend on OpenGL active-uniform enumeration
- runtime shader object exposes reflection data consistently across backends
- `cmake --build build --config Debug --target NLS_Render` passes

Risks:

- current `Shader` class shape may still assume one OpenGL program object

### RHI-REFLECT-022

Goal:
Add reflection validation tests.

Scope:

- compare generated reflection against expected layout snapshots for representative shaders
- include one material-heavy pass and one compute pass

Dependencies:

- `RHI-REFLECT-020`
- `RHI-REFLECT-021`

Acceptance:

- a failing binding layout change is caught by tests
- expected register-space assignments are part of the test oracle

Risks:

- snapshot brittleness if schema churns too late

## Phase D: Material And Binding Migration

### RHI-MAT-030

Goal:
Introduce backend-neutral shader resource layout objects.

Scope:

- define runtime layout types for Frame/Pass/Material/Object bindings
- map reflection output to `BindingSet` or successor abstraction
- preserve deterministic binding order

Dependencies:

- `RHI-REFLECT-021`

Acceptance:

- one migrated shader can produce a runtime binding layout without string-based uniform scanning
- material system can inspect required resources from layout metadata
- no renderer code for migrated pass relies on hardcoded texture-slot conventions

Risks:

- partial coexistence with legacy material code may create dual paths that drift

### RHI-MAT-031

Goal:
Migrate `Material` from string-driven uniform submission to reflection-backed parameter binding.

Scope:

- replace primary reliance on per-uniform OpenGL submission
- bind material constants and textures through material layout metadata
- support default values, missing resources, and editor overrides

Dependencies:

- `RHI-MAT-030`

Acceptance:

- one migrated material-backed pass renders without calling the legacy OpenGL uniform update path for its material parameters
- runtime validation errors are emitted for missing required bindings
- `cmake --build build --config Debug --target NLS_Render NLS_Engine` passes

Risks:

- current editor/UI may still assume mutable named uniforms

### RHI-MAT-032

Goal:
Split CPU-side parameter blocks by update frequency.

Scope:

- define upload structs or generated layouts for Frame, Pass, Material, and Object constants
- update upload path to avoid catch-all constant blocks

Dependencies:

- `RHI-MAT-030`

Acceptance:

- one migrated pass uses distinct frame/pass/material/object data uploads
- layout validation catches size or offset mismatches in debug builds

Risks:

- alignment and packing mismatches across backends if CPU layouts are handwritten without validation

## Phase E: OpenGL Runtime Path

### RHI-GL-040

Goal:
Define and implement the OpenGL consumption path for HLSL-first shaders.

Scope:

- choose and document the translation path from HLSL artifacts to OpenGL runtime shader objects
- adapt shader loading/runtime linkage to consume generated artifacts
- preserve editor hot reload behavior where possible

Dependencies:

- `RHI-SHADER-010`
- `RHI-REFLECT-021`

Acceptance:

- one migrated HLSL shader pass runs through the OpenGL renderer path
- no handwritten GLSL mirror is required for that pass
- `cmake --build build --config Debug --target NLS_Render NLS_Engine` passes
- runtime smoke: launch the app/editor with OpenGL backend and render one migrated pass without shader load failure

Risks:

- OpenGL translation quality and feature coverage may lag DX12/Vulkan

### RHI-GL-041

Goal:
Remove OpenGL-only active-uniform reflection dependency for migrated shaders.

Scope:

- bypass `glGetActiveUniform`-style reflection path for HLSL-generated shaders
- keep legacy fallback only for non-migrated shaders during transition

Dependencies:

- `RHI-GL-040`

Acceptance:

- migrated shaders load and bind with compiler-generated reflection only
- legacy GLSL shaders still function until fully retired

Risks:

- mixed-mode shader loading can increase maintenance cost if migration drags on

## Phase F: DX12 Runtime Path

### RHI-DX12-050

Goal:
Bring the DX12 shader pipeline from compiler output to runtime binding.

Scope:

- consume DXIL outputs in the DX12 backend
- create root-signature or equivalent binding layout from normalized reflection data
- bind Frame/Pass/Material/Object resources according to register-space convention

Dependencies:

- `RHI-SHADER-010`
- `RHI-REFLECT-021`
- `RHI-MAT-031`

Acceptance:

- one simple unlit or fullscreen pass renders in DX12 using generated DXIL
- backend does not require a separate shader authoring path
- runtime smoke: launch with DX12 backend and render one migrated pass without backend fallback

Risks:

- root signature design must align with normalized binding layout, not ad hoc per-pass code

### RHI-DX12-051

Goal:
Migrate one material-driven graphics pass to DX12.

Scope:

- select one representative pass such as deferred lighting or standard PBR
- wire material constants/textures through DX12 binding path

Dependencies:

- `RHI-DX12-050`

Acceptance:

- pass renders through DX12 with material parameters sourced from reflection-backed bindings
- no OpenGL-specific shader resource assumptions remain in that pass path

Risks:

- material system gaps will surface here before Vulkan

## Phase G: Vulkan Runtime Path

### RHI-VK-060

Goal:
Bring the Vulkan shader pipeline from compiler output to runtime binding.

Scope:

- consume SPIR-V outputs in Vulkan backend
- create descriptor set layouts and pipeline layouts from normalized reflection data
- map project register spaces to Vulkan set indices deterministically

Dependencies:

- `RHI-SHADER-010`
- `RHI-REFLECT-021`
- `RHI-MAT-031`

Acceptance:

- one simple unlit or fullscreen pass renders in Vulkan using generated SPIR-V
- runtime smoke: launch with Vulkan backend and render one migrated pass without backend fallback

Risks:

- Vulkan pipeline and synchronization requirements may expose missing RHI abstractions earlier than DX12

### RHI-VK-061

Goal:
Migrate one material-driven graphics pass to Vulkan.

Scope:

- select the same representative pass used for DX12 where possible
- validate descriptor updates and per-frequency constant data uploads

Dependencies:

- `RHI-VK-060`

Acceptance:

- chosen pass renders in Vulkan with correct material data and texture bindings
- reflection-driven layouts match runtime descriptor allocation

Risks:

- descriptor lifetime and update strategy may need dedicated allocator work

## Phase H: Legacy Shader Retirement

### RHI-MIGRATE-070

Goal:
Migrate deferred path shaders to HLSL-first.

Scope:

- replace legacy deferred GBuffer and deferred lighting shader source with HLSL-first equivalents
- integrate generated artifacts into OpenGL, DX12, and Vulkan runtime paths

Dependencies:

- `RHI-GL-040`
- `RHI-DX12-050`
- `RHI-VK-060`

Acceptance:

- deferred path has one maintained HLSL source per shader pass
- OpenGL, DX12, and Vulkan can all load the migrated deferred shaders through generated artifacts
- runtime smoke on at least one backend confirms the deferred path renders without legacy GLSL assets

Risks:

- deferred path currently mixes renderer evolution and shader evolution; migration should avoid moving both contracts at once without tests

### RHI-MIGRATE-071

Goal:
Migrate standard material/PBR path to HLSL-first.

Scope:

- replace the standard forward/PBR shader source path with HLSL-first equivalents
- ensure shared BRDF and material bindings are factored into reusable includes

Dependencies:

- `RHI-MIGRATE-070`
- `RHI-MAT-031`

Acceptance:

- standard PBR path uses HLSL-only maintained source
- material reflection data drives editor/runtime parameter surfaces

Risks:

- feature parity gaps between old GLSL and new HLSL path may be subtle without image comparison tests

### RHI-MIGRATE-072

Goal:
Retire legacy handwritten GLSL for migrated passes.

Scope:

- remove or archive legacy GLSL assets only after equivalent HLSL-backed runtime paths are validated
- update asset references and tooling

Dependencies:

- `RHI-MIGRATE-070`
- `RHI-MIGRATE-071`

Acceptance:

- no migrated pass depends on handwritten GLSL at runtime
- asset database and shader manager resolve HLSL-generated outputs correctly

Risks:

- premature removal can break hot reload, tooling, or fallback preview flows

## Recommended Execution Order

1. `RHI-SHADER-001`
2. `RHI-SHADER-002`
3. `RHI-SHADER-010`
4. `RHI-SHADER-011`
5. `RHI-REFLECT-020`
6. `RHI-REFLECT-021`
7. `RHI-MAT-030`
8. `RHI-MAT-031`
9. `RHI-GL-040`
10. `RHI-DX12-050`
11. `RHI-VK-060`
12. representative migrated pass tasks
13. legacy retirement

## Minimum Acceptance Matrix

Before calling the HLSL-first effort "usable", all of the following should be true:

- `cmake --build build --config Debug --target NLS_Render` passes
- `cmake --build build --config Debug --target NLS_Engine` passes
- one migrated shader compiles to DX12, Vulkan, and OpenGL target artifacts
- one migrated pass renders on OpenGL
- one migrated pass renders on DX12
- one migrated pass renders on Vulkan
- migrated materials bind from reflection metadata, not handwritten OpenGL uniform enumeration

## Open Issues Requiring Code Alignment

These are not blockers for the documents, but they are code realities the implementation backlog must respect:

- the project is not HLSL-first yet; current engine shaders are still largely GLSL assets
- `Shader`, `ShaderLoader`, and `Material` are still strongly tied to OpenGL program/uniform behavior
- OpenGL remains the most complete runtime rendering path
- Vulkan and DX12 scaffolding exist, but scene rendering is not yet parity-complete
- legacy naming in some code uses OpenGL-oriented terms such as "uniform" and "texture unit", which should not leak into the final reflection-driven API
