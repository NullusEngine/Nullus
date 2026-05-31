# Contract: Texture Build Settings And Resolver

## Input

- Source image bytes or source file path
- `TextureImporterSettings`
- Target platform name
- Backend format capability set
- Encoder registry
- DirectXTex encoder availability on Windows editor/tool builds

## Output

- `TextureBuildSettings`
- List of `TextureLoadDiagnostic` entries

## Resolution Order

1. Start from default importer settings.
2. Apply source-derived information: dimensions, alpha presence, HDR status.
3. Apply matching platform override by canonical target name.
4. Resolve texture intent.
5. Resolve mip policy and max size.
6. Resolve requested format from explicit override or compression intent.
7. Check format descriptor, backend format capability, source constraints, top-level alignment policy, and encoder availability.
8. Emit fallback diagnostics when requested output changes.
9. Produce deterministic build identity.

## Windows/DX12 Default Mapping

- `uncompressed`: `RGBA8`
- `default` color without alpha: `BC1` if supported, otherwise `RGBA8`
- `default` color with alpha: `BC7` if supported, otherwise `RGBA8`
- `low-quality` color with alpha: `BC3` if supported, otherwise `RGBA8`
- `high-quality` color: `BC7` if supported, otherwise `RGBA8`
- `normal`: `BC5` if supported, otherwise `RGBA8`
- `mask`: `BC1` for packed RGB masks or `BC5` for two-channel masks when configured, otherwise `RGBA8`
- `ui`: `RGBA8`, mips disabled by default
- `hdr`: `RGBA16F` uncompressed artifact/runtime format for first scope; BC6H is reserved and HDR must not fall back to `RGBA8` unless the user explicitly allows range loss with a diagnostic

Tiny or non-block-aligned textures are not invalid solely because their dimensions are below `4x4`; BC payload pitch uses ceil block counts. Fallback is required only when the selected backend lacks unaligned block-texture support for top-level dimensions or the importer policy disables padded encoding.

## Backend Capability Requirements

The resolver must consume per-format capabilities with at least:

- Sampled texture support
- Upload/copy destination viability
- Color attachment support where relevant
- Storage support where relevant
- sRGB view support
- Block-compressed top-level alignment policy

For Windows/DX12, capability construction must query `D3D12_FEATURE_FORMAT_SUPPORT` for each first-scope format and store a diagnostic reason when a format cannot be selected.

## Diagnostics

Resolver diagnostics must include:

- Asset path
- Target platform
- Requested format
- Resolved format
- Reason for fallback or failure

## Determinism

The build identity must include:

- Source content hash
- Source asset GUID/path identity
- Normalized importer settings
- Normalized platform override ordering
- Importer version
- Postprocessor version
- Dependency hash for source-derived side inputs
- Target platform
- Resolved format
- Mip policy
- Color-space intent
- Encoder id and version
- Encoder options and DirectXTex version for BC outputs
- Artifact schema version
