# Cross-Format Normal Map Import Design

## Problem

Nullus currently treats parser material `bump` channels differently by source
format. FBX ignores every `bump` texture, even when a DCC exporter placed a
tangent-space normal map such as `curtain_fabric_Normal.png` in that channel.
OBJ/MTL does the opposite and treats every `bump` texture as a tangent-space
normal map, including real grayscale height maps. glTF uses its explicit
`normalTexture` semantic and is already correct.

This produces format-dependent material behavior and leaves the Sponza FBX
without the normal map used by the equivalent glTF asset.

## Decision

Use one normal-map selection policy for glTF, FBX, and OBJ/MTL:

1. An explicit normal-map semantic always binds the referenced texture to the
   material `Normal` slot as linear data.
2. A parser `bump` channel may be promoted to the `Normal` slot only when the
   referenced texture identity explicitly identifies a normal map.
3. A real bump, height, or displacement texture is not decoded as a
   tangent-space normal map. Until Nullus has a height conversion or parallax
   path, it remains unbound and produces a diagnostic.
4. When explicit `normal` and `bump` channels both exist, the explicit normal
   channel wins and only one `Normal` slot is serialized.

The policy belongs in `MaterialConversion.cpp`, where all source formats are
converted to the shared runtime material contract. Parsers retain the source
semantics they observed and do not rewrite channels.

## Texture Classification

For a parser `bump` channel, resolve its `textureKey` to the corresponding
`ImportedSceneNamedRecord`. Inspect the texture URI filename, display name, and
source key. Matching is case-insensitive and recognizes identifier boundaries
formed by path separators, punctuation, whitespace, and camel-case changes.

The classifier accepts the tokens `normal` and `normalmap`, including common
forms such as:

- `curtain_fabric_Normal.png`
- `cloth-normal.tga`
- `cloth.normal.jpg`
- `ClothNormal.png`
- `cloth_normalmap.png`

It must not accept incidental substrings such as `abnormal`, nor identities
that explicitly describe `bump`, `height`, or `displacement` without a normal
token. URI is preferred because it identifies the source image; name and
source key provide fallbacks for embedded or parser-generated texture records.

The classifier only resolves ambiguous parser `bump` semantics. It does not
override an explicit glTF or parser normal semantic.

## Material Output And Diagnostics

A promoted normal map uses `MaterialTextureColorSpace::Linear`, serializes the
`_NormalMap` binding, and enables `_NORMALMAP` through the existing material
serialization path.

Emit `material-inferred-normal-map-from-bump-channel` when a normal-named bump
texture is promoted. Emit the format-neutral
`material-ignored-bump-height-map` when an otherwise valid bump texture is not
promoted. Missing texture references continue to use the existing
`material-missing-texture` diagnostic.

Replacing the FBX-specific ignored-bump diagnostic is an intentional contract
change: FBX and OBJ/MTL now expose the same conversion result and diagnostic for
the same source semantics.

## Format Behavior

- glTF: explicit `normalTexture` behavior remains unchanged. No filename
  heuristic is applied because the format already provides an unambiguous
  semantic.
- FBX: explicit normal channels continue to work. A normal-named texture placed
  in `bump` is promoted. Real height textures remain ignored.
- OBJ/MTL: normal-named `map_Bump` textures are promoted. The previous behavior
  that treated every `map_Bump` as a tangent-space normal map is removed; real
  height textures are ignored.

## Cache Invalidation

Increment the shared `ModelScene` importer version from 17 to 18. FBX and OBJ
material artifacts created under the old policy must be regenerated. glTF also
uses the shared model importer version, so its artifacts will be rebuilt even
though its normal-map semantics do not change.

Add the corresponding importer-version invalidation contract test so version
18 remains documented as the cross-format normal-map classification change.

## Tests And Validation

Add focused material-conversion tests covering:

- FBX `bump` plus `_Normal.png` produces one linear `Normal` slot,
  `_NormalMap`, `_NORMALMAP`, and the promotion diagnostic;
- FBX `bump` plus `_Height.png` remains unbound and emits the ignored diagnostic;
- OBJ/MTL `bump` follows the same two outcomes;
- explicit FBX `normal` wins over a simultaneous normal-named `bump`;
- glTF explicit normal behavior remains unchanged;
- punctuation, case, camel-case, and false-positive names exercise the
  classifier boundaries;
- importer version 18 invalidates version 17 artifacts.

Run the focused `AssetMaterialConversionTests` and model-importer version tests,
then the maintained `NullusUnitTests` target if practical. Reimport the Sponza
FBX and glTF assets and inspect their generated material artifacts: both curtain
materials must contain `_NormalMap` and `_NORMALMAP`, while retaining their
format-specific properties such as double-sided state.

For runtime rendering validation, capture the FBX and glTF curtain draws on the
same graphics backend with RenderDoc. Confirm the normal texture resource and
normal-map shader variant are bound in both draws. Record the backend used;
validation on one backend does not prove another backend.

## Acceptance Criteria

- The Sponza FBX curtain material applies `curtain_fabric_Normal.png`.
- Equivalent FBX, OBJ/MTL, and glTF normal-map inputs produce the same runtime
  normal slot, linear color-space metadata, and `_NORMALMAP` keyword behavior.
- Real bump, height, and displacement textures are not misdecoded as
  tangent-space normals in any supported model format.
- Explicit normal semantics take precedence over filename inference.
- Existing cached model artifacts are invalidated and regenerated.

## Non-Goals

- Converting grayscale height maps into tangent-space normal maps.
- Adding parallax, tessellation, or displacement rendering.
- Detecting normal maps from pixel statistics.
- Changing tangent generation, normal-map channel orientation, or shader-side
  tangent-space decoding.
