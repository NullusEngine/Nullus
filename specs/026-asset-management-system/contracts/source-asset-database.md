# Contract: Source Asset Database

## Scan Inputs

- One root path.
- Root mutability flag: editable project root or read-only engine/package root.
- Recursive scan includes regular files and skips `.meta` sidecars as source assets.

## Scan Outputs

- Source asset records indexed by GUID and normalized absolute path.
- A `.meta` sidecar for every editable source asset.
- Diagnostics for inaccessible files, invalid GUIDs, repaired metadata, duplicate GUIDs, and read-only assets missing metadata.

## Required Behaviors

- Existing valid GUID values remain stable across scans.
- Missing GUID values are generated for editable assets.
- Existing importer settings in `.meta` files are preserved when identity fields are added.
- `.gltf`, `.glb`, `.fbx`, and `.obj` classify as model scene source assets with importer `scene-model`.
- `.prefab` classifies as prefab source assets with importer `prefab`.
- `.png`, `.jpeg`, `.jpg`, and `.tga` classify as texture source assets with importer `texture`.
- `.hlsl` classifies as shader source assets with importer `shader`.
- `.mat` and `.nmat` classify as material source assets with importer `material`.
- `.scene` and `.objectgraph.json` classify as scene source assets with importer `scene`.
- Source paths are normalized before indexing and must not escape the mounted root.
- Project roots can repair missing or invalid metadata; read-only roots report diagnostics instead of writing sidecars.
