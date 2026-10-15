# Prefab Workflow

A Prefab turns an object hierarchy into a reusable asset: the source Prefab defines the structure, instances in a scene reference it, and per-instance edits are recorded as overrides. Editing a Prefab happens in a dedicated Prefab Stage rather than directly in the scene.

**Current status: partial.** Creation, instantiation, Stage saving, and Inspector overrides are integrated into the editor. Variants, nesting, and Unpack have native workflows and tests, but their panel entry points are incomplete. The steps below distinguish direct editor operations from engine extension interfaces.

## Core concepts

| Concept | Description |
| --- | --- |
| Prefab asset | A `.prefab` asset serialized from an object hierarchy, with an asset ID and sub-asset key |
| Prefab instance | The instance root in a scene that references a Prefab and records the mapping back to source objects |
| Override | The difference between an instance and its source Prefab, recorded as patch operations |
| Prefab Stage | A mode that opens one Prefab for editing |
| Prefab Variant | A Prefab derived from another Prefab |
| Unpack | Expanding an instance into scene-owned objects and severing the link to the source Prefab |
| Generated read-only | A Prefab sub-asset produced by an importer and marked as not directly editable |

## Creating a Prefab from a scene

Prerequisites: edit mode, a scene already saved under the project's `Assets/`, and a root object that is not connected to a Prefab.

1. Arrange the hierarchy and name its root, for example `Crate`.
2. Create and open `Assets/Prefabs` with `Create > Folder` in the Asset Browser.
3. Drag the root from the Hierarchy into that directory. The editor creates `Crate.prefab`, appending a number on collisions, and connects the original objects as an instance.
4. Wait for import, then drag `Crate.prefab` onto empty space in the Hierarchy for a second instance, or onto an object row for a child instance.
5. Save the scene. Both instances should appear and retain their Prefab references after reopening the scene.

If the Console reports an owning-scene or Undo-scope error, save the scene and retry. Connected instances cannot use this same save path to create a new Prefab; see the Variant/Unpack limits below.

Model import also generates read-only Prefab sub-assets. They can be instantiated but cannot overwrite the source Prefab directly.

## Prefab Stage

Opening a Prefab enters the Prefab Stage: an isolated editing context containing only that hierarchy, in which the scene and the stage each own their own object graph.

Stage state includes:

- The associated Prefab asset ID, path, and sub-asset key
- Whether it is loaded and whether it has unsaved changes
- Whether it is editable and whether it is generated read-only
- The stage root object and the object graph captured when it opened

Double-click a Prefab in the Asset Browser to open its Stage. Edit objects in the Hierarchy and use `File > Save Scene` / `Ctrl+S`; while a Stage is active this saves the Prefab. The Console should report `Active prefab stage saved.`.

The current Stage exit/replacement flow does not provide complete unsaved-change protection. Save explicitly before switching Prefabs or leaving the Stage; do not rely on a close-confirmation dialog. A generated read-only Prefab Stage cannot save edits.

## Overrides

Overrides are recorded as patch operations instead of per-object payloads:

- A patch records the source object, instance object, property path, and operation content.
- Structural changes such as added or removed objects are recorded as object record arrays.
- Each record carries the owning Prefab layer name, which is what makes nesting work.

The Hierarchy colors instances by state: normal, connected root, connected child, containing overrides, missing reference, generated read-only, pending, and unpacked. Selecting an instance makes the revertible overrides visible in the Inspector.

After editing an instance property, right-click it for `Apply Override` / `Revert Override`. The object toolbar's `Overrides` menu provides `Apply All Overrides` / `Revert All Overrides`. Apply writes to the source asset; Revert restores its value. Apply is disabled for generated read-only or unavailable sources. Save the scene to retain instance-specific edits.

## Prefab Variants

A Variant is created from another Prefab and stores only the differences. Creation needs:

- The base Prefab's asset ID and sub-asset key
- The destination path and the new Variant's asset ID
- Whether the base is generated read-only and whether the destination already exists

When the base is generated read-only, the creation flow first converts it into an editable form before producing the Variant.

These capabilities are provided by the native [PrefabUtilityFacade](../../../Project/Editor/Assets/PrefabUtilityFacade.h) and [AssetDragDropWorkflow](../../../Project/Editor/Assets/AssetDragDropWorkflow.h). The current Asset Browser has not wired the `Create Variant` command descriptor into a clickable menu; do not assume another engine's menu path exists here.

## Nesting and preserved data

An instance record preserves:

- The source-to-instance object ID mapping and the reverse lookup from instance objects to source IDs
- The local patch list
- The preserved instance root object ID
- Preserved records for added objects and the object correspondence table
- Preserved asset references and resolved assets
- The list of nested child instances

This is what lets an instance map back to its source correctly after saving, reloading, and reimporting.

## Unpacking

Unpacking expands an instance into scene-owned objects:

- It returns the expanded root object.
- It reports the object IDs that became scene-owned.
- It preserves references that still point at external assets.

The hierarchy stops following the source Prefab afterwards.

The native `PrefabUtilityFacade` supports Unpack; the current panels do not provide a complete Unpack entry point. Do not simulate it by editing `.prefab` files, GUIDs, or serialized records by hand.

## Resource lifetime

Prefab instances share the read-only data of their source Prefab. Creation, saving, and destruction are tracked by a dedicated resource lifetime manager, which avoids decoding the same Prefab repeatedly and avoids leaving dangling references during teardown.

## Related formats

Prefabs and scenes share one object graph format: `$owned` expresses lifecycle ownership, while `fileID` / `guid` / `type` expresses references. The read and write rules are in [Reflection and Serialization](../architecture/reflection-serialization.md).

## Next steps

- [Reflection and Serialization](../architecture/reflection-serialization.md) explains the file structure behind scenes and Prefabs.
- [Asset Browser and Import](../assets/pipeline.md) manages Prefab assets and their dependencies.
