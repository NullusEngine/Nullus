# Contract: Editor Asset Drag/Drop, Import Progress, And Material Preview

## Drag Payload Inputs

- Asset Browser payload with one or more source assets, imported artifacts, sub-assets, folders, materials, textures, prefabs, generated model prefabs, or scenes.
- Imported asset handle payloads containing source asset GUID, normalized source asset path, default prefab sub-asset key, artifact type, generated-model-prefab flag, import readiness, and optional committed artifact path summary.
- Hierarchy payload with one or more `GameObject` records, prefab instances, generated model prefab instances, renderer components, or material slots.
- Drop target describing the destination panel, scene, parent object, insertion index, renderer component, material slot, or Asset Browser folder.
- Editor command context with undo/redo mode, user-action flag, target scene, active asset root, and conflict policy.

## Drag/Drop Outputs

- Command result with status, created/modified asset IDs, modified scene IDs, selected object IDs, diagnostics, and dependency refresh requests.
- Connected prefab instance when dropping prefab or generated model prefab assets onto a scene or Hierarchy object.
- Material assignment patch when dropping material assets onto renderer targets.
- Deterministic create-material-and-assign action when dropping texture assets onto renderer targets and policy allows it.
- New `.prefab` source asset, prefab variant, or unpacked copy when dragging supported Hierarchy objects into an Asset Browser folder.
- Rejection diagnostics for invalid source/target combinations, read-only destinations, generated artifact mutation attempts, or missing renderer/material targets.

## Drag/Drop Required Behaviors

- Drag/drop must resolve the intended operation from source type, target type, keyboard modifiers or command mode, and editor policy.
- Asset Browser to Hierarchy prefab drops must call the prefab workflow instantiation path and store prefab asset references plus source-to-instance maps.
- Scene View and Hierarchy drops must prefer imported asset handle payloads over raw file path payloads, matching reference editor object-reference drag/drop semantics.
- Model and prefab handle drops must not synchronously refresh the entire asset database, import source files, parse model source formats, or decode renderer payloads on the UI thread.
- If the dragged handle has no committed prefab artifact yet, the drop result must be pending or rejected with import diagnostics; asynchronous import may continue through the import progress pipeline, but the drop callback must not block waiting for it.
- Generated model prefab drops must instantiate connected model prefab instances; customization must use scene overrides, variants, or unpacking.
- Material drops must assign GUID-backed material references to renderer material slots and must participate in undo/redo or equivalent command history.
- Texture drops must not directly mutate renderer shader data. They must create or update a material asset according to policy and then assign that material, or fail with diagnostics.
- Hierarchy to Asset Browser drops must create new prefab sources for ordinary hierarchies, create variants for connected prefab/model prefab instances when requested, and use deterministic destination conflict suffixes.
- Dropping onto read-only package/engine roots must fail unless the user explicitly copies into an editable project asset root.
- All drag/drop writes must mark affected scenes/assets dirty and enqueue dependency refresh for changed prefab, material, scene, and build manifest records.

## Import Progress Inputs

- Import request with source asset IDs, target platform, importer settings, dependency copy policy, batch ID, and cancellation token.
- Importer phase callbacks from dependency copy, source parse, intermediate conversion, artifact write, postprocess, and atomic commit.
- Diagnostics produced during scan, dependency resolution, conversion, artifact writing, postprocessing, cancellation, or commit.

## Import Progress Outputs

- Batch progress events with total assets, completed assets, failed assets, cancelled assets, and active asset.
- Per-asset progress events with source path, GUID, phase, normalized percent, message, diagnostics, and cancellation state.
- Terminal result event with committed artifact manifest, preserved previous artifact manifest, or failure/cancellation diagnostics.

## Import Progress Required Behaviors

- Import jobs must not block editor panel navigation, selection, hierarchy inspection, or asset browser search while running.
- Progress must not report success until the artifact commit has completed.
- Cancellation must be cooperative and safe at dependency copy, parse, conversion, artifact write, postprocess, and pre-commit boundaries.
- Cancelled or failed imports must discard staged artifacts and keep the previous successful artifact manifest active.
- Import diagnostics must remain visible after restart through persisted diagnostic records.

## Material Preview Inputs

- Imported material records from glTF/GLB, FBX parser data, or OBJ MTL.
- Texture dependency records with color-space intent, sampler state, UV set, UV transform, and source file hashes.
- Target Nullus material model capabilities and importer settings overrides.

## Material Preview Outputs

- Nullus material artifact with deterministic texture slots, scalar/vector factors, alpha mode, normal/emissive settings, sampler state, and fallback values.
- Texture artifact references with correct sRGB/linear policy.
- Diagnostics for unsupported channels, missing textures, unsupported texture encodings, ambiguous material models, or runtime shader capability gaps.

## Material Preview Required Behaviors

- glTF PBR metallic-roughness materials must map base color, metallic, roughness, normal, occlusion, emissive, alpha mode, double-sided, UV transform, and sampler state.
- FBX materials must map parser-exposed diffuse/base color, specular/glossiness or roughness, normal/bump, emissive, opacity, texture paths, and UV channels.
- OBJ MTL materials must map diffuse, specular, shininess, opacity/dissolve, normal/bump, and illumination hints where supported.
- Color textures default to sRGB; data textures default to linear. Import settings may override this policy and must become part of the asset version key.
- Generated model prefabs must reference converted material artifacts, not source material records, so preview, scene viewport, runtime manifest, and build output agree.
- Correct visual display requires renderer-facing evidence, not just artifact inspection: representative glTF, FBX, and OBJ fixtures must pass editor preview or scene viewport validation and record RenderDoc capture or deterministic screenshot evidence for regression comparison.
