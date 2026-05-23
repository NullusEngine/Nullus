# Contract: Render Feature Ownership Boundary

## Purpose

Define which rendering responsibilities belong to renderer core, render passes, data providers, explicit editor helpers, and debug draw ownership after removing `ARenderFeature`.

## Scope

This contract applies to:

- `CompositeRenderer` and `ABaseRenderer` draw orchestration
- core frame/object draw preparation
- debug drawing ownership
- lighting data ownership
- renderer statistics ownership
- prevention rules that keep `ARenderFeature` from being reintroduced

## Ownership Rules

### 1. Renderer Core Owns Mandatory Draw Preparation

Renderer core is the authoritative owner for:

- frame-level draw data required by normal scene rendering
- object-level draw data required by normal scene rendering
- mandatory draw preparation ordering before material submission

Renderer core responsibilities must remain available even if optional extensions are disabled or absent.

### 2. Material Ownership Is Limited To Material Data

Material submission may depend on:

- per-material constants
- per-material textures and samplers
- material-specific state overrides already defined by renderer/material contracts

Material submission must not become the owner of frame-level, object-level, or scene-lighting data.

### 3. Debug Drawing Has Two Distinct Contracts

The debug drawing capability is split into:

- **Submission contract**: collecting debug primitives, global/category state, style, depth/fill mode, lifetime, and frame limits
- **Rendering contract**: executing a dedicated debug drawing stage or pass that consumes visible primitives

Submission semantics must remain stable during the migration, even if rendering ownership changes.

Debug draw callers must submit geometry and style only. They must not own or construct debug render pipeline state, debug materials, debug meshes, or pass-local resources. High-level helpers such as frustums, light volumes, bounding boxes, spheres, capsules, cones, and grid axes must expand into the same point/line/triangle primitive queue used by direct debug draw submissions.

### 4. Lighting Is Scene/Renderer Data

Lighting data is owned by scene or renderer data providers and must be consumable by multiple renderer modes.

Lighting consumers:

- may read a shared authoritative lighting source
- must not require hidden draw hook side effects to obtain lighting data

### 5. Renderer Statistics Are Renderer Diagnostics

Renderer statistics are owned by renderer diagnostics and updated by draw submission behavior.

Statistics must:

- reset at frame begin
- reflect actual submitted draw work
- remain queryable without optional feature registration

### 6. The Feature Registry Is Removed

`ARenderFeature` and the `CompositeRenderer` feature registry are no longer part of the rendering ownership model.

The codebase must not reintroduce:

- `ARenderFeature`
- `CompositeRenderer::AddFeature`
- `CompositeRenderer::GetFeature`
- `CompositeRenderer::HasFeature`
- `CompositeRenderer::RemoveFeature`
- renderer-owned feature maps or draw-time feature hooks

Current migrated ownership is:

- `CompositeRenderer` owns frame/object draw preparation through `FrameObjectBindingProvider`.
- `CompositeRenderer` owns frame diagnostics through `RendererStats`.
- scene renderers own lighting publication through `SceneLightingProvider`.
- debug draw submission is owned by `DebugDrawService`.
- debug draw rendering is owned by `DebugDrawPass`.
- editor utility model, outline, and gizmo rendering are explicit helper objects owned by the pass or renderer path that uses them.

## Draw-Time Ordering Contract

For any scene draw that needs frame/object/material data, the required ordering is:

1. renderer verifies frame state is prepared
2. renderer prepares object state for the draw if needed
3. renderer prepares or binds any required pass-owned state
4. renderer prepares/binds material state
5. renderer submits the draw
6. renderer updates statistics for the submitted work

No optional extension may be the sole owner of steps 1, 2, 3, or 6.

## Migration Acceptance Rules

The migration slice is considered contract-compliant only when all of the following are true:

- normal scene draws no longer depend on optional extension hooks for mandatory frame/object draw preparation
- debug draw queue semantics remain unchanged for category visibility, lifetime, and frame limits
- debug draw callers can submit point, line, triangle, frustum, light-volume, and bounds helpers without providing pipeline state
- debug draw global/category toggles control editor helper visualization consistently
- forward and deferred renderer consumers read from the same lighting ownership source
- renderer statistics remain available without optional feature registration
- editor panels read renderer-owned statistics directly
- editor debug views enqueue shapes through the explicit debug draw service/pass path, including selected camera frustums, selected light volumes, mesh bounds, and grid helpers
- source checks find no `ARenderFeature` or feature registry references in Runtime, Project, or Tests C++ code
- editor/runtime product flows remain runnable after the slice

## Out Of Scope For This Contract

This contract does not redesign:

- the full shader variant system
- the full pipeline cache strategy
- backend capability policy
- unrelated editor utility rendering beyond compatibility needs required by this migration
