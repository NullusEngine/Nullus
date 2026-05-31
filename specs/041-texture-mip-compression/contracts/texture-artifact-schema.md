# Contract: Texture Artifact Schema

## Scope

The `.ntex` artifact remains stored in the existing native artifact container. The texture payload schema evolves from the current RGBA8-oriented validation to a format-descriptor-driven schema.

## Versioning

- Current payload version: `2`
- Planned payload version: `3`
- Current native container schema version: `3`
- Planned native container schema version: `4`

Readers must keep compatibility for existing native container schema version `3` / payload version `2` RGBA8 artifacts during rollout. Because the current container API accepts a single expected schema version, implementation must add a dual-version read path or migration shim for texture artifacts before writing schema version `4`.

## Header Fields

- `magic`: `NTEX`
- `version`: payload schema version
- `width`, `height`, `depth`
- `dimension`
- `arrayLayers`
- `format`
- `colorSpace`
- `mipCount`
- `subresourceCount`
- `metadataStringTableOffset`
- `metadataStringTableSize`
- `reserved`

## Metadata Table

The payload includes deterministic metadata after the mip and subresource record tables:

- `targetPlatform`
- `buildIdentity`
- `encoderId`
- `encoderVersion` as a little-endian `uint32`

Hashes may be stored in manifests for indexing, but the artifact payload must keep the metadata needed for diagnostics and reproducibility.

## Subresource Record Fields

- `level`
- `arrayLayer`
- `face`: `none`, `positive-x`, `negative-x`, `positive-y`, `negative-y`, `positive-z`, `negative-z`
- `width`, `height`, `depth`
- `rowPitch`
- `slicePitch`
- `dataOffset`
- `dataSize`

## Required Invariants

- Header dimensions are non-zero.
- Mip count is non-zero.
- First-scope runtime artifacts have `dimension=Texture2D`, `arrayLayers=1`, `subresourceCount=mipCount`, and every subresource uses `arrayLayer=0`, `face=none`.
- Schema-level reserved cubemap metadata may be serialized for validation tests, but it is not a runtime cubemap loading support claim; for cubemaps, `mipCount` remains the number of unique mip levels while `subresourceCount` is `mipCount * 6`.
- Mip levels are contiguous from zero.
- Each mip dimension is `max(previous / 2, 1)` unless mips are disabled.
- `rowPitch == ceil(width / blockWidth) * bytesPerBlock`.
- `slicePitch == rowPitch * ceil(height / blockHeight) * ceil(depth / blockDepth)`.
- `dataSize == slicePitch` for first-scope 2D artifacts.
- Future cubemaps must store six faces per mip in the documented face order. Future texture arrays must store ascending `arrayLayer` within each mip. Future streaming can mark subresources as externally resident only after the schema adds an explicit residency flag; first-scope payloads are fully resident.
- Data ranges must not overflow and must stay within payload bounds.
- Import artifacts cannot use depth/stencil formats.

## Compatibility Rules

- Native container schema v3 / payload v2 RGBA8 artifacts decode using current fields and are promoted in memory to descriptor-driven metadata.
- Native container schema v4 / payload v3 artifacts require descriptor and subresource validation.
- Unknown future schema versions fail closed with diagnostics.
