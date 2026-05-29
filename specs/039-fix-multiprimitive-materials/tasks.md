# Tasks: Fix Multi-Primitive Materials

1. Add a failing regression test that shows a multi-primitive imported mesh cannot be represented as a single material-indexed mesh without losing primitive material assignment.
2. Update scene sub-asset or prefab generation so multi-primitive meshes produce renderable primitive mesh records with the correct material index.
3. Keep single-primitive mesh keys and existing generated prefab behavior compatible.
4. Run focused unit tests for asset import, prefab generation, deferred renderer material cache, and editor render-path contracts.
5. Run plan-review/self-review and document remaining manual RenderDoc/editor verification needs.
