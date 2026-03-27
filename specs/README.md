# Nullus Spec Bundles

This directory stores committed `Spec-Kit` bundles for major changes.

Use one kebab-case change identifier per major change:

- `specs/<change-id>/spec.md`
- `specs/<change-id>/plan.md`
- `specs/<change-id>/tasks.md`

In Nullus, major changes usually include:

- Functional changes under `Runtime/`
- Rendering backend, shader, frame-graph, or pipeline changes
- Reflection and `MetaParser` changes
- Test behavior changes under `Tests/`
- Cross-platform or cross-subsystem changes

Small docs edits, comment cleanups, and tiny low-risk fixes can usually skip a spec.

See `AGENTS.md` and `Docs/AIWorkflow.md` for the full workflow.
