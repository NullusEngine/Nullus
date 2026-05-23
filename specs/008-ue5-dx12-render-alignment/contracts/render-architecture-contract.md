# Contract: UE5-Style DX12 Render Architecture

## Scope

This contract defines the required architecture boundaries for the `008-ue5-dx12-render-alignment` feature bundle.

## Baseline Contract

- The target alignment baseline is UE 5.7 public rendering architecture.
- Public contract alignment is sufficient to begin implementation.
- Final claims of "fully aligned" require a documented source-level UE audit in addition to the public-contract work.

## Thread Ownership Contract

- The Game Thread owns mutable gameplay and editor state only.
- The Game Thread may publish immutable render input, but it must not directly perform normal DX12 recording, submission, present, or readback work.
- The Render Thread is the sole authoritative owner of frame build, view preparation, and graph construction.
- The RHI Thread is the sole authoritative owner of backend translation, submission, synchronization, readback completion, and retirement.

## Graph Authority Contract

- RDG is the only authoritative scheduler for pass order, dependencies, transient lifetime, external resource import/extract, and graph-visible readback.
- Renderer-local static schemas, helper layers, or editor code may describe pass taxonomy, but they may not remain a second source of scheduling truth.
- Serial execution is allowed only as a scheduling policy inside the same authoritative graph-driven architecture.

## Editor And Game Unification Contract

- Editor and Game rendering must use the same authoritative frame pipeline.
- Scene view, game view, offscreen rendering, picking, gizmo, grid, outline, and overlays must all appear as graph-visible frame work or graph-visible readback work.
- Editor-only submission, present, or readback bypasses are forbidden.

## DX12 Phase-1 Backend Contract

- DX12 is the only active backend in the first implementation phase of this feature.
- If DX12 is unavailable or invalid for the phase-1 path, startup must stop explicitly.
- Future multi-backend support must be preserved through backend-neutral architecture boundaries, not by keeping fallback execution paths alive.

## Central Infrastructure Contract

- `PipelineCache`, `DescriptorAllocator`, and transient lifetime tracking are mandatory mainline systems for accepted frame execution.
- Runtime, renderer, editor, and backend code must not keep accepted bypass paths around these systems.
- Diagnostics must be able to prove whether a bypass occurred.

## Forbidden Behavior Contract

The following are forbidden in the accepted phase-1 architecture:

- runtime direct-submit fallback
- main-thread explicit frame recording for normal rendering
- driver-built fallback scene packages in the normal path
- compatibility on-demand acquire/present behavior
- editor-only submit, present, or readback bypasses
- backend-name-driven execution logic spread through renderer, editor, or game code
- optional adoption of central PSO, descriptor, or transient-lifetime systems in accepted paths

## Validation Contract

- Unit tests must prove ownership boundaries, forbidden-path removal, graph authority, editor-path unification, and centralized infrastructure adoption.
- Runtime validation must prove DX12 Editor and Game behavior separately.
- RenderDoc evidence is required for visible correctness claims.
- Final closure requires a documented UE source audit in addition to product validation.
