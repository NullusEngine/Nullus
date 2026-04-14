<!--
Sync Impact Report
- Version change: template -> 1.0.0
- Modified principles:
  - Template Principle 1 -> I. Spec-First Major Changes
  - Template Principle 2 -> II. Validation Matches The Subsystem
  - Template Principle 3 -> III. Generated Code And Backend Boundaries Are Sacred
  - Template Principle 4 -> IV. Incremental, Verified Delivery
  - Template Principle 5 -> V. Product Runtime Preservation
- Added sections:
  - Repository Constraints
  - Workflow And Review Gates
- Removed sections:
  - None
- Templates requiring updates:
  - ✅ updated: D:/VSProject/Nullus/.specify/templates/plan-template.md
  - ✅ updated: D:/VSProject/Nullus/.specify/templates/spec-template.md
  - ✅ updated: D:/VSProject/Nullus/.specify/templates/tasks-template.md
  - ⚠ pending-not-present: D:/VSProject/Nullus/.specify/templates/commands/
  - ✅ updated: D:/VSProject/Nullus/AGENTS.md
  - ✅ updated: D:/VSProject/Nullus/Docs/AIWorkflow.md
- Follow-up TODOs:
  - None
-->
# Nullus Constitution

## Core Principles

### I. Spec-First Major Changes
Major changes MUST start with one committed spec bundle under `specs/<change-id>/` before code
implementation begins. In Nullus, this includes behavior changes under `Runtime/`, architecture or
workflow changes under `Project/`, rendering pipeline/backend/shader/frame-graph work, reflection
or `MetaParser` changes, test behavior changes, and changes spanning multiple subsystems or
platforms. Small, low-risk edits MAY skip a spec only when scope is narrow and behavior is
obvious. The same bundle MUST be updated as the change evolves; notes MUST NOT be scattered across
the repository.

Rationale: Nullus changes often span rendering, tooling, runtime, and generated code boundaries.
The spec bundle is the shared source of truth that keeps those changes reviewable.

### II. Validation Matches The Subsystem
Every completed change MUST include validation evidence appropriate to the subsystem it touches.
Rendering changes MUST prefer RenderDoc captures, backend-specific runtime verification, or focused
renderer checks. Reflection and `MetaParser` changes MUST run through the normal generation flow
and relevant tests. Runtime and editor behavior changes MUST run targeted automated tests when
stable entrypoints exist, otherwise they MUST include exact manual verification notes. Claims about
backend support, platform support, or rendering correctness MUST be limited to explicitly validated
targets; one backend or one platform MUST NOT be used as proof for another.

Rationale: Nullus is a multi-backend engine. Validation is only meaningful when it matches the
actual execution surface.

### III. Generated Code And Backend Boundaries Are Sacred
Files under `Runtime/*/Gen/` MUST be treated as generated output and MUST NOT be hand-edited.
Reflection-related work MUST preserve the `MetaParser` pipeline and generated registration flow.
Renderer mainline code MUST move toward the formal RHI contract instead of restoring renderer-side
legacy forks. Backend capability gaps MUST be expressed through capability reporting, documented
degradation, or backend-internal compatibility layers rather than silent architecture splits in
renderer, editor, or game code.

Rationale: Nullus depends on generated reflection output and a single shared rendering architecture.
Breaking either boundary creates regressions that are difficult to trace.

### IV. Incremental, Verified Delivery
Implementation MUST be broken into explicit, reviewable subproblems. Behavior-changing work SHOULD
follow test-first or test-with-change discipline when stable entrypoints exist. Changes MUST remain
incremental, easy to verify, and easy to backtrack. Before any completion claim, the most relevant
build, test, or runtime verification commands MUST be re-run and their results summarized. Task and
spec artifacts MUST be kept in sync with the actual code state.

Rationale: Nullus rendering and tooling work is too coupled for large speculative drops. Small,
verified steps preserve momentum without hiding regressions.

### V. Product Runtime Preservation
`Editor` and `Game` MUST remain runnable during staged architectural work unless a spec explicitly
documents a temporary exception and its recovery plan. Fallback and degraded behavior MUST be
capability-driven, explicit, and documented. Product code MUST NOT silently regress from one
backend path to another without recorded reasoning, and runtime behavior MUST remain correct for the
validated backend matrix in the current phase.

Rationale: Nullus is developed through its real products, not through isolated renderer samples
alone. Preserving runnable products is part of architectural correctness.

## Repository Constraints

- `Driver` remains the repository's graphics entry point for backend selection, swapchain lifecycle,
  frame ownership, and tooling integration unless a future constitution amendment says otherwise.
- Repository contributors MUST use the existing build and test responsibilities instead of inventing
  parallel workflows.
- Official Spec-Kit skills under `.agents/skills/speckit-*` and the bundled `.specify` scripts
  SHOULD be preferred over ad hoc planning and workflow commands for major changes.
- Rendering investigations SHOULD follow `Docs/Rendering/RenderDocDebugging.md` before falling back
  to less precise evidence.
- Cross-backend and cross-platform support statements MUST be backed by recorded evidence in docs or
  spec artifacts.

## Workflow And Review Gates

The default workflow for major changes is:

1. Clarify the goal and affected subsystem.
2. Decide whether a spec is required.
3. Create or update `spec.md`, `plan.md`, and `tasks.md` under one `specs/<change-id>/` bundle.
4. Implement in small, verified steps.
5. Self-review for regressions, missing tests, and backend/platform assumptions.
6. Validate with exact commands or runtime evidence matched to the subsystem.

Every implementation plan MUST include a constitution check that confirms:

- the change is using the correct spec scope,
- generated-file boundaries are respected,
- backend/platform validation expectations are explicit,
- product runtime viability is preserved or intentionally scoped,
- and the final evidence path is known before implementation proceeds.

Workflow rule changes, template changes, or repository-level process changes MUST be routed through
`$speckit-constitution` so the constitution, templates, and guidance docs stay aligned.

## Governance

This constitution supersedes conflicting workflow notes elsewhere in the repository. `AGENTS.md`,
`Docs/AIWorkflow.md`, template files under `.specify/templates/`, and future workflow docs MUST
remain consistent with it.

Amendments MUST:

- explain the principle or governance change,
- update dependent templates and workflow guidance in the same change,
- record any deferred follow-up explicitly,
- and apply semantic versioning to the constitution itself.

Versioning policy:

- MAJOR: remove or redefine a principle or governance rule in a backward-incompatible way,
- MINOR: add a new principle/section or materially expand repository obligations,
- PATCH: clarify wording without changing the meaning of the rules.

Compliance review expectations:

- plans MUST pass the constitution gate before implementation proceeds,
- tasks and validation notes MUST reflect the active constitution,
- and final summaries MUST describe the evidence used to satisfy the applicable rules.

**Version**: 1.0.0 | **Ratified**: 2026-03-28 | **Last Amended**: 2026-03-28
