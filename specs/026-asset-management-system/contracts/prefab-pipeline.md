# Contract: Prefab Pipeline

## Source Prefab Inputs

- `.prefab` source asset with `.meta` GUID.
- `Nullus.ObjectGraph.Prefab` document.
- Optional base prefab asset reference for variants.
- Patch operations for variant or instance overrides.
- Optional nested prefab asset references inside the object graph.
- Source-to-instance object ID mapping when editing or applying scene instance overrides.
- Editor operation context: create prefab, open prefab stage, save, discard, instantiate, apply override, revert override, create variant, unpack, or reimport.

## Generated Model Prefab Inputs

- `ImportedScene` node hierarchy.
- Generated mesh, material, texture, skeleton, skin, animation, and morph sub-assets.
- Import settings that control hierarchy, scale, material extraction, and read-only/editable policy.

## Prefab Outputs

- Runtime prefab artifact with validated object graph.
- Dependency records for base prefabs, nested prefabs, override targets, and `$asset` references.
- Source-to-instance object ID mapping for instantiation and override stability.
- Diagnostics for invalid graph, base chain, unresolved asset references, or runtime capability gaps.
- Editor-facing override list with inherited values, local values, missing targets, owning prefab layer, and apply/revert eligibility.
- Prefab stage state with asset identity, dirty flag, editability, generated-read-only policy, and save/discard result.
- Variant creation result with new editable `.prefab` source asset, `.meta` GUID, `basePrefab` reference, and initial empty override set.
- Unpack result with resolved scene-owned object hierarchy, preserved asset references, and removed prefab dependency.

## Required Behaviors

- `.prefab` sources must import through `Runtime/Engine/Serialize`; no parallel prefab persistence format is allowed.
- Prefab roots must be `GameObject` records.
- Owned hierarchy must use `$owned`; object references must use `$ref`; asset references must use `$asset`.
- Variants must reference base prefabs by `AssetId`.
- Base prefab cycles must be rejected.
- Override targets must resolve to valid base object IDs and property paths before runtime use.
- Editor load may preserve recoverable unknown records and unresolved asset references with diagnostics.
- Runtime load must fail on invalid required prefab data.
- Generated model prefabs must map static renderables to `TransformComponent`, `MeshFilter`, and `MeshRenderer`.
- Generated model prefabs must reference generated artifacts by GUID/sub-asset ID, not by source file paths.
- Generated model prefabs are read-only by default; editable customization must happen through variants or unpacked scene objects.
- Applying a prefab override must mark dependent variants, scenes, and build manifests stale.
- Reverting an override must remove the patch and reveal the current base prefab value.
- Creating a prefab from a scene selection must serialize exactly one root hierarchy, or create a deterministic wrapper root when the editor explicitly supports multi-root prefab creation.
- Instantiating a prefab into a scene must store a prefab asset reference, an instance root, a source-to-instance object map, and local patches instead of copying the prefab graph inline.
- Override discovery must compare the live instance or variant layer against its base graph and produce stable patch records for reflected fields, components, child objects, nested prefabs, and removed objects.
- Apply override must write to the nearest editable prefab layer that owns the target object. Generated model prefab bases are not editable; apply must target an editable variant or fail with diagnostics.
- Revert override must remove only the selected patch and must not disturb sibling overrides or nested prefab overrides.
- Apply-all must be atomic: failed source write or failed reimport keeps the previous successful prefab artifact active.
- Prefab Stage must allow open, save, discard, and dirty-state queries without mutating scene instances until save commits.
- Nested prefab references must keep their own asset references and override chains; cycles must be rejected before runtime use.
- Variant creation from a generated model prefab must keep the generated prefab as base so source model reimport updates unchanged data.
- Unpack must detach the instance from its prefab asset and write ordinary scene-owned object records with current resolved values.
- Missing base prefabs, missing nested prefabs, and unresolved `$asset` references must remain visible as editor diagnostics and must block runtime packaging when required.
