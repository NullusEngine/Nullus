# Contract: Scene Import Pipeline

## Import Inputs

- Source asset record for `.gltf`, `.glb`, `.fbx`, or `.obj`.
- Import settings from `.meta`.
- Target platform and importer version.
- Source dependency resolver for external buffers, MTL files, and images.

## Import Outputs

- One `ImportedScene` intermediate record.
- One per-import artifact manifest.
- Runtime artifact payloads for generated mesh, material, texture, skeleton, skin, animation clip, morph target, model, and prefab outputs.
- Deterministic sub-asset IDs for generated mesh, material, texture, skeleton, skin, animation clip, morph target, model, and prefab artifacts.
- Persisted diagnostics for unsupported or invalid source data.

## Runtime Artifact Payload Contract

Every model import writes its generated payloads through the artifact writer, never directly into the active committed artifact location.

Required payload kinds:

- `mesh`: runtime mesh data, vertex streams, index buffers, material slots, bounds, and source primitive mapping.
- `material`: converted Nullus material data, shader model, factors, texture slots, sampler state, alpha mode, and fallback values. Generated model material sampler uniforms must point at committed imported texture artifacts when a corresponding texture sub-asset exists, not at the original source image path.
- `texture`: extensionless imported texture artifact payload with URI, MIME type, bufferView identity, embedded flag, byte length metadata, and the original source/embedded encoded image bytes after `PAYLOAD_BEGIN`. Texture payloads must be runtime-loadable by the texture resource loader and identified by ArtifactDB metadata rather than by a filename extension.
- `model`: model-level hierarchy, node transforms, mesh/material bindings, skin bindings, animation bindings, and default prefab reference.
- `skeleton`: joint hierarchy, inverse bind data, bind pose metadata, and source node mapping.
- `skin`: mesh-to-skeleton binding, joint remap, and weight/index stream references.
- `animation`: clip metadata, channel targets, interpolation, timing range, and payload references.
- `morph`: morph target deltas, weights, names, and mesh binding.
- `prefab`: read-only generated model prefab object graph with GUID/sub-asset references to generated artifacts.

Native mesh payloads must use the same source-local mesh key space as `ImportedScene.meshes` and generated `mesh:<sourceKey>` sub-assets. A payload writer must not assume that parser traversal order equals source mesh index order. For FBX/OBJ parser output, each parsed mesh must carry its parser/source mesh key. For glTF/GLB, `.nmesh` payloads must be decoded from glTF accessors/bufferViews by original mesh index and merge that mesh's primitives deterministically, preserving at least positions, indices, UV0, normals, and tangents when present.

Converted material payloads must include every texture/factor channel the importer exposes. glTF materials map PBR metallic-roughness fields directly. FBX/OBJ parser materials map diffuse/base color, normal/bump, opacity, shininess, roughness, metallic, occlusion, emissive, specular, and double-sided channels when available. Independent metallic and roughness texture slots must be preserved separately, with packed metallic-roughness used only as a fallback when a source format exposes that packed texture.

Startup prewarm may load generated `.mat` artifacts with sampler textures intentionally deferred. The editor renderer resource-resolution queue must repair declared sampler uniforms before binding those materials for generated model instances, and deferred prewarm must not report missing-texture warnings unless a real texture load was requested and failed.

Each artifact manifest entry must include:

- Source asset GUID.
- Stable sub-asset key.
- Artifact type.
- Runtime loader ID.
- Target platform.
- Committed artifact path.
- Content hash over the committed payload bytes.

Artifact dependencies must include source file hashes, external buffer/image/MTL dependencies, path-to-GUID mappings, importer version, postprocessor version, and target platform records when those inputs affect payload generation.

## Required Behaviors

- Importers must reject normalized paths that escape mounted asset roots.
- glTF/GLB import must support glTF 2.0 meshes, vertex attributes, PBR metallic-roughness materials, external and embedded buffers/images, node hierarchy, transforms, skeletons, skins, animations, and morph targets.
- FBX import must convert all parser-exposed data possible through `ImportedScene` and report diagnostics for unsupported channels.
- OBJ import must convert static meshes, MTL materials, and textures; it must report that skeleton, skin, animation, and morph data are unsupported by the format.
- Model importer version changes must invalidate stale artifacts whenever native mesh payload semantics or converted material channel semantics change.
- Sub-asset IDs must stay stable across imports when source-local identifiers, node names, mesh names, material names, or image URIs remain stable.
- Duplicate names or missing source-local identifiers must be disambiguated deterministically and reported.
- External dependencies must be recorded with source file hashes and path-to-GUID mappings when copied into project assets.
- Successful imports must commit artifacts atomically.
- Successful commits must only publish a manifest after every staged payload is writable to the committed artifact root.
- Failed imports must preserve the previous successful artifact manifest, previous committed payload files, and diagnostics.
- Failed commits must remove staged artifacts and roll back any already-moved replacement files before returning a failed result.
- Generated model prefabs must be emitted as read-only generated artifacts unless the user creates an editable variant or unpacked copy.
