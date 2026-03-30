# Nullus AI Workflow

This document explains how `Spec-Kit` and `Superpowers` are used in the Nullus repository.

## Roles

- `Spec-Kit` is the repository's planning layer.
  It answers: what are we changing, why are we changing it, and how will we know it is done?
- `Superpowers` is the execution discipline for Codex.
  It answers: how do we implement safely, incrementally, and with good validation?

Nullus does not vendor the full `Superpowers` upstream templates or scripts. We only adopt the method as repository rules.

## Current Scope

This repository integration is intentionally lightweight:

- Repository rules and templates are committed.
- Spec bundles are committed for major changes.
- There is currently no CI gate, PR template requirement, or automatic enforcement layer for this workflow.

## Repository Layout

The repository uses these workflow paths:

- `AGENTS.md`: Codex entrypoint and working rules
- `.specify/templates/`: official Spec-Kit templates
- `.specify/scripts/powershell/`: official PowerShell helper scripts
- `.specify/memory/constitution.md`: project constitution used by Spec-Kit workflows
- `.agents/skills/speckit-*/SKILL.md`: official Codex Spec-Kit skills
- `specs/<change-id>/`: committed spec bundles for major changes

The standard `Spec-Kit` initialization command for this repository is:

```powershell
specify init --here --ai codex --ai-skills --script ps --offline --force
```

The current repository already contains the initialized scaffold, so contributors usually do not need to run `init` again unless they are refreshing the upstream scaffold.

## Constitution Authority

The project constitution at `.specify/memory/constitution.md` is the authoritative workflow policy
for Nullus. If repository-level workflow rules, template expectations, or governance need to
change, update the constitution and sync dependent templates/docs in the same change by using
`$speckit-constitution`.

## Minimal Codex Flow

For a typical major change:

1. Read `AGENTS.md`.
2. Use the official Spec-Kit Codex skills for major changes:
   `speckit-specify`, `speckit-clarify`, `speckit-plan`, `speckit-tasks`, and `speckit-analyze`.
3. Pick a kebab-case `change-id`.
4. Create or update the bundle under `specs/<change-id>/`.
5. Implement in small steps.
6. Validate with the right tests or runtime evidence.
7. Update the same spec bundle if scope or acceptance changes.

If the workflow itself needs to change, run `speckit-constitution` before editing the templates or
repository guidance in isolation.

If you need to scaffold a new feature bundle directly from the repository scripts, use:

```powershell
pwsh .specify/scripts/powershell/create-new-feature.ps1 -Json -ShortName my-change "Describe the change"
```

## When You Must Create A Spec

Create a spec before editing code for:

- New features
- Behavior changes
- Architecture changes
- Rendering pipeline, backend, shader, or frame-graph work
- Reflection or `MetaParser` work
- Test behavior changes
- Changes that span multiple platforms or multiple subsystems

## When You Can Skip A Spec

You can usually skip a spec for:

- Docs-only edits
- Comment-only edits
- Small formatting cleanups
- Tiny renames with no behavior change
- Small low-risk fixes with obvious scope

If the scope grows while you work, stop and create the spec bundle.

## Validation By Area

### Rendering

- Prefer RenderDoc evidence for rendering bugs or rendering regressions.
- Use the workflow in `Docs/Rendering/RenderDocDebugging.md`.
- Record the backend used for validation, because one backend does not prove the others.
- Desktop screenshots are a fallback for UI or windowing issues that RenderDoc cannot explain well.

### Reflection And MetaParser

- Use the normal project build flow so `MetaParser` runs the same way it does in real development.
- Use the maintained workflow guides in `Docs/Reflection/ReflectionWorkflow.zh-CN.md` and
  `Docs/Reflection/ReflectionWorkflow.en.md` instead of relying on older reflection notes.
- Validate with the current test entrypoints documented in `Docs/Testing.md`.
- If generated output changes, explain why the generated result is expected instead of hand-editing generated files.

### Tests And Runtime Behavior

- Add or update tests when behavior changes and a test path exists.
- Prefer focused verification over broad unrelated test runs.
- If no useful automated test exists, include a short manual verification note with exact steps.

## Example Spec Bundle

See `specs/example-rendering-major-change/` for a repository example of a complete major-change bundle.

That example is illustrative only. It shows the expected shape and level of detail, not a committed product roadmap.
