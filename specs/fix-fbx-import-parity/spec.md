# Feature Specification: Fix FBX Import Parity

**Feature Branch**: `fix-fbx-import-parity`  
**Created**: 2026-06-07  
**Status**: Draft  
**Input**: User reports imported FBX models look much darker than glTF and feel much slower when dragged into the scene; imported engine assets should load through the same path after conversion.

## User Scenarios & Testing

### User Story 1 - FBX Materials Preserve Authoring Brightness (Priority: P1)

Artists importing FBX models that use classic Phong/Lambert material data should see generated engine materials that feed the active PBR shaders with comparable roughness and base material parameters instead of silently dropping shininess data.

**Why this priority**: The visible darkness regression directly affects imported model fidelity.

**Independent Test**: Convert an FBX parser material containing diffuse, specular, and shininess but no authored roughness, then verify the serialized material contains a PBR roughness value derived from shininess and still preserves authored roughness scalar or texture data when present.

**Acceptance Scenarios**:

1. **Given** an FBX material with shininess and no roughness, **When** it is converted to a generated material artifact, **Then** the artifact contains `u_Roughness` derived from shininess so current PBR shaders can consume the gloss information.
2. **Given** an FBX material with an explicit roughness scalar, **When** it is converted, **Then** the explicit roughness value wins over any shininess fallback.
3. **Given** an FBX material with a roughness texture and shininess but no roughness scalar, **When** it is converted, **Then** the roughness texture remains the PBR roughness source and shininess does not add a multiplier scalar.

---

### User Story 2 - Imported FBX Drops Use Unified Hot Cache (Priority: P2)

Users dragging an already-imported FBX model into a scene should get the same non-blocking generated prefab fast path and repeated-drop cache behavior as glTF.

**Why this priority**: Perceived drag/drop slowness after import should not depend on the original source format.

**Independent Test**: Import a small FBX through the existing asset database, drop its generated prefab twice, and verify the repeated drop hits the unified hot cache without reloading the prefab graph.

**Acceptance Scenarios**:

1. **Given** a successfully imported FBX model, **When** it is dropped into a scene twice using the generated model payload, **Then** the second drop records a cache hit and no prefab graph load.
2. **Given** generated renderer artifacts are missing or invalid, **When** the FBX model is dropped, **Then** the drop remains pending instead of re-parsing the source file on the foreground path.

---

### User Story 3 - FBX Bump Maps Do Not Masquerade As Tangent Normals (Priority: P1)

Artists importing FBX assets that expose a classic bump or height channel should not get generated materials that enable tangent-space normal mapping unless a real normal map was authored or the bump texture was explicitly converted into a normal-map asset.

**Why this priority**: Treating a height/bump texture as a tangent-space normal map can severely distort lighting, making FBX materials look dark or partially unlit compared with glTF.

**Independent Test**: Convert an FBX parser material containing only a `bump` texture channel and verify the generated material does not contain a `Normal` slot, `u_NormalMap`, or enabled normal mapping. Convert a material with a true `normal` channel and verify normal mapping still works.

**Acceptance Scenarios**:

1. **Given** an FBX material with only a bump/height texture, **When** it is converted to a generated material artifact, **Then** the artifact keeps normal mapping disabled.
2. **Given** an FBX material with a true normal texture, **When** it is converted, **Then** the artifact binds `u_NormalMap` and enables normal mapping.
3. **Given** an FBX material with both normal and bump texture channels, **When** it is converted, **Then** the true normal texture remains authoritative.

---

### User Story 4 - FBX Parser Roughness Sentinels Stay Out Of PBR Shaders (Priority: P1)

Artists importing FBX files through parser-backed material paths should not get generated materials whose PBR roughness uniforms contain parser sentinel or otherwise invalid values.

**Why this priority**: Generated FBX artifacts were observed with `u_Roughness=-2.200000`, which can push the PBR shader into invalid lighting math and make only some materials respond correctly to scene lighting.

**Independent Test**: Convert an FBX parser material containing a roughness scalar outside `[0, 1]` and verify the generated material does not serialize that scalar. If a roughness texture exists, the material keeps the texture with a neutral scalar. If shininess exists and no valid roughness texture exists, shininess fallback supplies the roughness.

**Acceptance Scenarios**:

1. **Given** an FBX parser material with a roughness texture and an invalid roughness scalar, **When** it is converted, **Then** the texture remains bound and `u_Roughness` stays neutral instead of multiplying by the invalid scalar.
2. **Given** an FBX parser material with invalid roughness and valid shininess but no roughness texture, **When** it is converted, **Then** the invalid roughness is ignored and shininess-derived roughness is serialized.
3. **Given** an FBX parser material with invalid roughness, **When** it is converted, **Then** a diagnostic records that the parser roughness scalar was ignored.

---

### User Story 5 - FBX Normal Mapping Cannot Poison GBuffer Normals (Priority: P1)

Artists viewing imported FBX materials that still bind a normal map, including previously generated assets, should not see surfaces turn black or partially unlit because the GBuffer normal target received NaN values.

**Why this priority**: RenderDoc evidence from `Editor_DX12_frame2018.rdc` shows the darkening starts in `DeferredGBuffer` normal output: FBX draw events such as `EID 17374` write `NaN,NaN,NaN,1` to `GBufferNormal`, which becomes black before the lighting pass.

**Independent Test**: Verify the PBR and deferred GBuffer shaders use safe vector normalization and safe TBN construction so zero or degenerate normal/tangent/bitangent inputs fall back to a finite geometric normal instead of writing NaN.

**Acceptance Scenarios**:

1. **Given** normal mapping is enabled but tangent or bitangent input is zero/degenerate, **When** the GBuffer shader computes `Normal`, **Then** it falls back to a finite normal and never serializes NaN to the normal render target.
2. **Given** a normal map sample decodes to a zero-length tangent-space vector, **When** Standard, StandardPBR, or DeferredGBuffer computes the shaded normal, **Then** the shader uses a finite default tangent-space normal.
3. **Given** a previously generated material still has `u_EnableNormalMapping=1`, **When** it is rendered through the deferred path, **Then** invalid TBN data degrades to geometry-normal lighting instead of making the lighting pass appear dark.

---

### User Story 6 - FBX Textured Diffuse Materials Match glTF Brightness (Priority: P1)

Artists comparing the same model imported from FBX and glTF should not see FBX materials rendered roughly half as bright because the parser's legacy diffuse color default is multiplied into an authored base-color texture.

**Why this priority**: Current imported Sponza FBX material artifacts show `u_Albedo=0.500000 0.500000 0.500000 1.000000` while the glTF artifacts use `1.000000`; this directly explains the remaining darker FBX appearance after the normal NaN fix.

**Independent Test**: Convert an FBX parser material with a diffuse texture and neutral grey diffuse color, then verify the generated material keeps `u_Albedo` white and records a diagnostic that the parser default diffuse tint was ignored. Convert a texture-less FBX material and verify its diffuse color is still preserved.

**Acceptance Scenarios**:

1. **Given** an FBX material with a diffuse/base-color texture and neutral parser diffuse color such as `0.5, 0.5, 0.5`, **When** it is converted, **Then** the base-color texture is bound but `u_Albedo` remains white.
2. **Given** an FBX material with no diffuse/base-color texture, **When** it is converted, **Then** its diffuse color still serializes as `u_Albedo`.
3. **Given** a non-FBX parser material, **When** it has a diffuse texture and diffuse color, **Then** the existing tint behavior is preserved.

---

### User Story 7 - FBX Decal-Like Materials Generate Decal Surfaces (Priority: P1)

Artists importing FBX materials named as decals and carrying opacity textures/scalars should get the same generated `Decal` surface classification as matching glTF blend decal materials.

**Why this priority**: Current imported FBX Sponza `dirt_decal` is serialized as `Opaque`, while the matching glTF material is serialized as `Decal`.

**Independent Test**: Convert an FBX parser material named `dirt_decal` with an opacity texture and verify the generated material has blend alpha mode, no depth write, and `surfaceMode=Decal`.

**Acceptance Scenarios**:

1. **Given** an FBX parser material with a decal token in the material name and an opacity texture, **When** it is converted, **Then** the material serializes as `Decal`.
2. **Given** an FBX parser material with an opacity scalar below 1, **When** it is converted, **Then** the existing blend/decal inference continues to work.
3. **Given** a non-decal FBX material with opacity, **When** it is converted, **Then** it remains a normal transparent surface instead of becoming a decal.

---

### User Story 8 - Assimp Baked Direction Streams Stay Directional (Priority: P1)

Artists importing FBX through the Assimp fallback should not get normals, tangents, or bitangents polluted by node translation when node transforms are baked into source mesh payloads.

**Why this priority**: The Assimp parser currently multiplies normal/tangent/bitangent vectors by the full node matrix when baking transforms, unlike the FBX SDK path which uses direction-only transforms. Bad direction streams can make only parts of an FBX model respond correctly to lighting.

**Independent Test**: Load a small model through Assimp with baked node transforms and a translated node, then verify positions are translated while normals and tangents remain normalized directions.

**Acceptance Scenarios**:

1. **Given** a translated node and baked transforms, **When** Assimp emits mesh vertices, **Then** positions include translation but normals/tangents/bitangents do not.
2. **Given** a transformed direction stream, **When** it is serialized to a mesh artifact, **Then** it remains finite and normalized.
3. **Given** non-baked parser loading, **When** source meshes are emitted for generated prefab reuse, **Then** existing source-space mesh sharing is preserved.

---

### User Story 9 - FBX Decal Queue And Root Naming Parity (Priority: P1)

Artists importing FBX models whose authored material is named as a decal should see those generated materials enter the deferred decal pass when the FBX parser exposes alpha through an alpha-bearing base-color texture rather than a separate opacity texture. The generated prefab root should also use the source file stem instead of the parser's synthetic `RootNode` label.

**Why this priority**: RenderDoc evidence from `Editor_DX12_frame3909.rdc` shows no `Nullus/DeferredDecal` pass at all, and current Sponza FBX artifacts show `dirt_decal` serialized as `Opaque` while the generated prefab root is still a parser name.

**Independent Test**: Convert an FBX parser material named `dirt_decal` with only an alpha-bearing diffuse/base-color texture and verify it serializes as `Decal`. Build a generated prefab from a single imported root named `RootNode` and verify the root GameObject name is the scene key/file stem.

**Acceptance Scenarios**:

1. **Given** an FBX parser material named `dirt_decal` with an alpha-bearing diffuse/base-color texture and no separate opacity channel, **When** it is converted, **Then** it serializes with `alphaMode=Blend`, `surfaceMode=Decal`, and disabled depth writing.
2. **Given** an FBX material with a diffuse/base-color texture but no source texture alpha evidence, **When** it is converted, **Then** it remains `Opaque` even if its name contains a decal token.
3. **Given** a normal FBX material with a diffuse/base-color texture and no decal token, **When** it is converted, **Then** it remains `Opaque`.
3. **Given** an imported FBX scene with one parser root node named `RootNode`, **When** the generated prefab is built, **Then** the root GameObject name is the file stem/scene key and child parent links are preserved.

---

### User Story 10 - Assimp FBX Raw Opacity Map Compatibility (Priority: P1)

Artists importing 3ds Max FBX assets through the Assimp fallback should get the same decal/opacity behavior when Assimp exposes `Parameters` transparency or cutout maps as raw UNKNOWN material properties instead of standard opacity textures.

**Why this priority**: The current Sponza FBX source connects its decal mask through `3dsMax|Parameters|transparency_map` and `3dsMax|Parameters|cutout_map`; Assimp keeps those connections in raw properties, so Nullus previously serialized the decal material without an opacity channel and skipped the deferred decal pass.

**Independent Test**: Parse an FBX fixture whose opacity texture connection is rewritten to `3dsMax|Parameters|transparency_map` and `3dsMax|Parameters|cutout_map`, then verify the imported scene material has an `opacity` texture channel and the texture is tracked as an external dependency.

**Acceptance Scenarios**:

1. **Given** an Assimp FBX material with `$raw.3dsMax|Parameters|transparency_map|file`, **When** Nullus builds parser scene data, **Then** the material exposes that texture as an `opacity` channel.
2. **Given** an Assimp FBX material with `$raw.3dsMax|Parameters|cutout_map|file`, **When** Nullus builds parser scene data, **Then** the material exposes that texture as an `opacity` channel.
3. **Given** a raw FBX opacity texture path, **When** dependency tracking runs, **Then** the texture URI is included in the external dependency list.

### Edge Cases

- FBX files with explicit roughness scalar or texture data must not have author-provided PBR roughness overwritten or multiplied by shininess fallback.
- FBX parser roughness scalar values outside `[0, 1]` or non-finite values must not be serialized into `u_Roughness`.
- Parser-exposed shininess/gloss textures must keep their source semantic and must not be consumed as PBR roughness maps unless a future conversion explicitly inverts/transcodes them.
- Parser-exposed bump or height textures must not be consumed as tangent-space normal maps unless a future conversion explicitly generates normal-map texture data.
- Runtime normal mapping shaders must tolerate zero-length or non-finite normal/tangent/bitangent values from old generated assets or parser edge cases.
- FBX diffuse colors that are parser/default neutral tints for textured materials must not darken base-color textures, but texture-less legacy diffuse colors must still be preserved.
- FBX opacity textures must make generated parser materials participate in transparent/decal surface classification even when no opacity scalar is present.
- FBX decal-named parser materials may expose usable alpha through the base-color texture only; these must enter decal surface classification only when source texture alpha evidence is available.
- Assimp FBX raw UNKNOWN properties named `3dsMax|Parameters|transparency_map|file` or `3dsMax|Parameters|cutout_map|file` must be interpreted by Nullus as parser opacity textures without modifying Assimp source.
- Direction streams must be transformed as directions, not positions, when Assimp baked transforms are requested.
- Single-root parser scenes named `RootNode` must keep object identity and child hierarchy stable while presenting the source file stem as the root name.
- Invalid or unavailable FBX parser builds may skip integration tests behind existing build capability macros.
- Repeated drops may still scan manifest metadata, but must not repeat prefab graph import on cache hit.

## Requirements

### Functional Requirements

- **FR-001**: The material conversion pipeline MUST convert FBX parser shininess into a PBR roughness scalar when no authored roughness scalar or texture exists.
- **FR-002**: Authored FBX roughness scalar or texture data MUST take precedence over any derived shininess fallback.
- **FR-003**: Generated FBX material artifacts MUST continue to serialize existing diffuse, specular, opacity, normal, metallic, roughness, occlusion, and emissive channels.
- **FR-004**: Repeated drops of already-imported FBX generated model prefabs MUST use the same unified hot-cache behavior verified for glTF.
- **FR-005**: Drag/drop of imported FBX assets MUST not synchronously reparse the source FBX when committed artifacts are current.
- **FR-006**: Assimp shininess/gloss texture inputs MUST remain `shininess` material channels and MUST NOT be mislabeled as explicit PBR roughness textures.
- **FR-007**: FBX parser `bump`, `height`, and displacement-style channels MUST NOT enable runtime normal mapping unless an explicit normal-map conversion path exists.
- **FR-008**: FBX parser `normal` channels MUST continue to bind generated material normal slots and enable runtime normal mapping.
- **FR-009**: FBX parser roughness scalars MUST be ignored and diagnosed when they are non-finite or outside the PBR `[0, 1]` range.
- **FR-010**: Invalid FBX roughness scalars MUST NOT suppress shininess-derived roughness fallback when no valid roughness texture is present.
- **FR-011**: Standard, StandardPBR, and DeferredGBuffer shaders MUST use finite fallback normals when normal, tangent, bitangent, or decoded normal-map vectors are zero-length or non-finite.
- **FR-012**: Deferred lighting MUST NOT receive NaN-encoded GBuffer normals from material normal-map paths; invalid normal-map/TBN inputs MUST degrade to geometry-normal lighting.
- **FR-013**: FBX parser materials with diffuse/base-color textures and neutral parser diffuse color defaults MUST serialize white `u_Albedo` instead of multiplying the texture darker.
- **FR-014**: FBX parser materials without diffuse/base-color textures MUST continue to serialize authored diffuse color values.
- **FR-015**: FBX parser materials with opacity textures MUST set blend alpha mode so decal-name surface inference can produce `surfaceMode=Decal`.
- **FR-016**: Assimp baked transform processing MUST transform normals, tangents, and bitangents as normalized direction vectors without translation.
- **FR-017**: The model-scene importer version MUST advance when generated FBX material/mesh artifact semantics change so stale artifacts containing bad roughness, albedo, or decal metadata are treated as out of date.
- **FR-018**: FBX parser materials whose material identity suggests a decal MUST become blend/decal surfaces when their diffuse/base-color texture is the only available alpha-bearing texture path and the source texture has alpha evidence.
- **FR-019**: Non-decal FBX parser materials with ordinary diffuse/base-color textures MUST remain opaque unless opacity data or alpha mode data indicates transparency.
- **FR-020**: Generated model prefabs MUST present a single parser root named `RootNode` as the source scene key/file stem while preserving deterministic object IDs and hierarchy links.
- **FR-021**: The model-scene importer version MUST advance again when version 7 artifacts can contain opaque FBX decals or parser root names.
- **FR-022**: The Nullus AssimpParser compatibility layer MUST map raw FBX `3dsMax|Parameters|transparency_map` and `3dsMax|Parameters|cutout_map` texture properties into the parser `opacity` channel without changing ThirdParty Assimp code.
- **FR-023**: Raw FBX opacity-map compatibility paths MUST participate in external dependency tracking so imported artifacts are invalidated when the mask texture changes.
- **FR-024**: The model-scene importer version MUST advance again when version 8 artifacts can miss raw FBX opacity/cutout maps.

### Key Entities

- **Imported Scene Material**: Parser-exposed material channel data from FBX/glTF/OBJ import.
- **Generated Material Artifact**: Serialized `.mat` payload consumed by runtime material loaders and shaders.
- **Unified Prefab Load Key**: Runtime cache identity built from source asset identity, manifest, prefab artifact, and renderer artifact stamps.

## Success Criteria

### Measurable Outcomes

- **SC-001**: A focused material conversion test proves FBX shininess-only materials produce a `u_Roughness` value usable by PBR shaders while authored roughness scalar or texture inputs remain authoritative.
- **SC-002**: A focused repeated-drop test proves imported FBX generated prefabs cache-hit on the second drop with zero prefab graph reloads.
- **SC-003**: Existing glTF generated model drag/drop cache behavior remains passing.
- **SC-004**: Focused unit tests pass in the current Windows test build.
- **SC-005**: A parser channel test proves Assimp `map_Ns` shininess/gloss textures are not emitted as PBR roughness texture channels.
- **SC-006**: A parser material conversion test proves FBX bump-only materials serialize with normal mapping disabled while true FBX normal-map materials still enable it.
- **SC-007**: A parser material conversion test proves invalid FBX roughness scalars do not serialize into `u_Roughness`, keep roughness textures neutral, and allow shininess fallback.
- **SC-008**: A shader regression test proves the PBR and deferred GBuffer normal-map paths contain safe normalization/TBN fallback logic that prevents NaN GBuffer normals.
- **SC-009**: A material conversion test proves FBX textured neutral diffuse materials serialize with white albedo while texture-less diffuse materials keep their color.
- **SC-010**: A material conversion test proves FBX opacity-texture decal materials serialize `surfaceMode=Decal`.
- **SC-011**: An Assimp parser test proves baked node translation affects positions but not normal/tangent/bitangent directions.
- **SC-012**: An importer version test proves current model-scene artifacts will invalidate importer version 6 outputs.
- **SC-013**: Material conversion and external FBX import tests prove FBX decal-named base-color alpha materials serialize as `Decal` without requiring a separate opacity texture, while decal-named materials without alpha evidence remain opaque.
- **SC-014**: A prefab generation test proves single-root parser `RootNode` hierarchies use the source scene key as the root GameObject name.
- **SC-015**: An importer version test proves current model-scene artifacts will invalidate importer version 7 outputs.
- **SC-016**: An Assimp parser test proves raw 3ds Max `Parameters` transparency and cutout maps surface as parser `opacity` texture channels and external dependencies.
- **SC-017**: An importer version test proves current model-scene artifacts will invalidate importer version 8 outputs.

## Assumptions

- The current default FBX reader in this build is Assimp unless importer metadata selects another reader.
- glTF remains the reference for generated model prefab loading behavior after import.
- UE 4.27 and Unity 2018.4 both normalize imported source material data into engine material assets; drag/drop after import should not use a format-specific source-file loading path.
- UE 4.27 uses FBX bump as a normal fallback only while importing that texture with normal-map settings, and Unity enables normal-map shading only when a resolved `_BumpMap` texture exists.
- Rendering appearance parity is addressed first through material artifact parameters and normal-map enablement; full visual RenderDoc comparison is out of scope for this narrow fix.
