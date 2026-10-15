# Minimal Complete Example

This exercise connects building, scenes, assets, scripts, and Prefabs into one reviewable workflow. The result is a saved scene, a rotating object, and a reusable Prefab. No downloaded sample assets are required.

**Status: uses existing capabilities.** The steps require the Windows editor with C# enabled. Source review, snippet compilation, runtime tests, and full desktop interaction have separate evidence in [Debugging and Testing](../quality/debugging.md#documentation-acceptance-scope).

## Prepare the project and scene

1. Build `Editor` and `Launcher` using [Environment and Build](../getting-started/build.md).
2. Follow [Your First Project](../getting-started/first-project.md) to create `DocsDemo`, create a basic scene, place Cube at `(0, 0, 5)`, and keep the camera at the origin.
3. Save as `Assets/Scenes/FirstScene.scene`. Later Prefab operations require a saved scene.
4. Create and attach the quick start's `Rotator.cs`, enter Play, confirm that Cube rotates and Console shows `Rotator awake`, then Stop.

## Verify one source asset import

Create a plain text `Triangle.obj` under `Assets/Models` on disk with the following content. It defines three vertices and one face, already positioned in front of the camera:

```obj
o DocsTriangle
v -1.0 -1.0 5.0
v 0.0 1.0 5.0
v 1.0 -1.0 5.0
f 1 2 3
```

1. Return to the editor and wait for the file to appear and finish importing in Asset Browser. `Import Here...` is currently disabled; copying source files into `Assets/` is an available entry point.
2. Expand the generated sub-assets and confirm a mesh, material, and Prefab. Drag the generated Prefab into empty Hierarchy space.
3. Keep the instance Transform from adding another `+Z` offset: the OBJ vertices already include their position. Disable Cube temporarily to avoid overlap.
4. Observe the triangle in Scene View. A single-sided mesh may be invisible from the back; the default material has no texture in this example.
5. Change a vertex in the OBJ and save. Use Asset Browser's `Reimport` if necessary, inspect the reimport log, and save the scene again.

The model's generated Prefab is read-only. For import failures, check logs and dependencies using [the asset pipeline guide](../assets/pipeline.md); do not edit `Library/` artifacts by hand.

## Save and reuse a Prefab

1. In edit mode, enable Cube again, name it `RotatingCube`, and save the scene.
2. Open `Assets/Prefabs` and drag the `RotatingCube` Hierarchy root into that folder.
3. Drag the resulting Prefab into the scene, move the second instance to one side, and save.
4. Double-click the Prefab to enter Stage, change a visible property, and press `Ctrl+S`. Save explicitly before switching Stage.
5. Inspect Apply/Revert on the instances. See [Prefab Workflow](../prefab/workflow.md) for available commands and the boundaries of Variant/Unpack support.

## Optional: add a user interface

Follow [UI Designer](../ui/designer.md) to create a `.ui` document, add a Panel, save, and assign it to the Document field on `UICanvasComponent`. Verify layout using Screen Overlay first, then explore camera and world-space paths. The designer exposes only a subset of document schema fields.

## Completion checks and troubleshooting

| Check | Expected result | First things to inspect |
| --- | --- | --- |
| Reopen project | Scene and asset references return | Save paths and preserved `.meta` identity |
| Script Play | Object rotates; Console records Awake | Compilation, object-row attachment, activation |
| Stop | Transform returns to edit state | Play sandbox versus the saved edit scene |
| OBJ import | Mesh, material, and Prefab generated | Source encoding, face indices, Console |
| Prefab | Two instances reference the same asset | Saved owning scene and existing connection |
| UI (optional) | Panel is visible | Document reference, size, render mode |

These checks form a desktop acceptance checklist. Compiling the script or passing subsystem tests alone does not establish that every interaction was performed.

## Next steps

- [Feature Status Matrix](index.md) connects this example to future directions.
- [Scripting API Reference](../scripting/api.md) gives signatures and language scope before extending the behavior.
