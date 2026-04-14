# Nullus Codex Workflow

This repository uses a spec-first workflow for major changes.

## Default Order

Follow this order unless the user explicitly asks for a tiny direct fix:

1. Clarify the goal and affected subsystem.
2. Decide whether the change requires a spec.
3. For major changes, create `spec.md`, `plan.md`, and `tasks.md` under `specs/<change-id>/`.
4. Implement in small steps.
5. Self-review before wrapping up.
6. Validate with the right tests or runtime evidence.

## When A Spec Is Required

Create a spec for major changes. In Nullus, major changes usually include:

- Functional behavior changes under `Runtime/`
- Architecture or workflow changes under `Project/`
- Rendering pipeline, backend, shader, or frame-graph changes
- Reflection or `MetaParser` changes
- Test behavior changes under `Tests/`
- Changes that affect multiple platforms or multiple subsystems

Small changes can skip a spec. Typical examples:

- Docs-only edits
- Comment-only edits
- Small renames with no behavior change
- Narrow formatting cleanups
- Tiny bug fixes with obvious scope and low risk

When in doubt, create the spec.

## Spec Layout

Use this layout:

- `specs/<change-id>/spec.md`
- `specs/<change-id>/plan.md`
- `specs/<change-id>/tasks.md`
- `.agents/skills/speckit-*` for the official Codex Spec-Kit skills

Rules:

- `change-id` must be a short kebab-case slug.
- Keep one change per directory.
- Update the same spec bundle as the work evolves instead of scattering notes across the repo.

Template files live under `.specify/templates/`.
PowerShell workflow scripts live under `.specify/scripts/powershell/`.
Project constitution lives under `.specify/memory/constitution.md`.
If repository workflow rules, template expectations, or review gates need to change, update the
constitution and sync dependent templates/docs through `$speckit-constitution` instead of editing
guidance files in isolation.

## Nullus-Specific Rules

- Treat `Runtime/*/Gen/` as generated output. Do not hand-edit generated files.
- Reflection-related changes must account for the `MetaParser` pipeline and the generated registration flow.
- Rendering investigations should prefer RenderDoc evidence over desktop screenshots.
- Follow the RenderDoc workflow in `Docs/Rendering/RenderDocDebugging.md` for rendering bugs.
- Do not assume one graphics backend proves another backend is correct.
- Do not assume Windows behavior proves Linux or macOS behavior is correct.
- Preserve the current build and test responsibilities instead of inventing a parallel workflow.

## Superpowers Rules For Codex

Apply these working rules on top of the repository workflow:

- Break larger work into explicit subproblems before editing code.
- Prefer test-first or test-with-change when behavior is changing and a test entrypoint exists.
- Keep changes incremental and easy to verify.
- Run the most relevant validation you can before finishing.
- Self-review for regressions, missing tests, and cross-platform assumptions before reporting completion.
- Summarize the evidence used to validate the change.

## Official Skills

This repository includes the official Codex Spec-Kit skills:

- `$speckit-specify`
- `$speckit-clarify`
- `$speckit-plan`
- `$speckit-tasks`
- `$speckit-analyze`
- `$speckit-implement`
- `$speckit-checklist`
- `$speckit-constitution`

For major changes, prefer the official skills and the bundled `.specify` scripts over ad-hoc planning notes.
The constitution is the repository's highest workflow authority; plans, tasks, and implementation
summaries should align with it.

## Validation Expectations

Choose validation that matches the subsystem:

- Rendering changes: RenderDoc capture, targeted runtime verification, or renderer-specific checks
- Reflection and `MetaParser` changes: normal build flow plus `NullusUnitTests` and `ReflectionTest`
- Runtime or editor behavior changes: relevant unit tests if available, otherwise a focused manual verification note
- Docs-only changes: spellcheck or link/path sanity where practical

More guidance lives in `Docs/AIWorkflow.md`.
