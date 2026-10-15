# Models, Meshes, and Textures

This page covers the two most common asset kinds in the content pipeline: models (with meshes and LODs) and textures (with mips and compression). Both keep import settings separate from artifacts, so changing settings regenerates artifacts.

## Model import

Models are imported through Assimp, covering glTF, FBX, OBJ, and related formats. One import produces several sub-assets:

| Sub-asset type | Notes |
| --- | --- |
| `Mesh` | Mesh geometry |
| `Material` | Material asset converted from the model's material |
| `Texture` | Textures embedded in or referenced by the model |
| `Prefab` | Prefab generated from the scene hierarchy, marked generated read-only |

After import, the relationships between model, meshes, materials, and textures are recorded in the asset database and the artifact manifest, and resolved at runtime through asset IDs.

## Material conversion

Materials carried by a model are converted into engine materials. The conversion maps the external description onto the StandardPBR parameter set, produces a `.mat` asset, and wires textures into the matching slots. The result is an independent asset that can be edited on its own.

## Static mesh LODs

Static meshes support multiple LOD levels. Import settings include:

| Field | Default | Meaning |
| --- | --- | --- |
| `lodGroup` | `None` | LOD group preset name |
| `minLOD` | `0` | Lowest level allowed at runtime |
| `autoComputeLODScreenSize` | `true` | Whether to derive per-level screen size thresholds |
| `sourceModels` | One per LOD | Source kind, screen size, and mesh data per level |

Each LOD source model carries a source kind: `Imported` (from the source file), `Authored` (built inside the engine), or `Generated` (produced by the simplification flow).

LOD group presets expose a name, a level count, a triangle percentage, and a pixel error. Sharing a preset keeps level transitions consistent across many meshes instead of each mesh tuning its own thresholds.

Mesh simplification happens during import. The runtime only selects a level; see [Graphics Backends and Large-Scene Features](../rendering/backends.md).

## Texture import settings

Defaults for the texture import snapshot:

| Field | Default | Notes |
| --- | --- | --- |
| `textureType` | `default` | Semantic type |
| `srgbTexture` | `true` | Whether this is a color texture |
| `alphaIsTransparency` | `false` | Whether alpha means transparency |
| `mipmapEnabled` | `true` | Whether to generate mips |
| `maxTextureSize` | `2048` | Maximum edge length |
| `resizePolicy` | `keep` | Resize policy |
| `compressionIntent` | `default` | Compression intent |
| `explicitFormat` | empty | Explicit format override |
| `platformOverrides` | empty | Per-platform overrides |

Color textures and linear data textures must not be mixed up: color textures use an sRGB view, while data textures such as normals and occlusion should disable sRGB, otherwise results skew bright or washed out.

A platform override entry can set maximum size, format, compression quality, resize policy, and mip generation independently, which is how you lower the specification for a specific platform without touching the source settings.

## Mips and compression

The offline artifacts currently support RGBA8, RGBA16F, BC1, BC3, BC5, and BC7:

- The Windows editor's `directxtex-bc` encoder has unit-level coverage for BC1, BC3, BC5, and BC7 artifact payloads.
- Artifacts preserve target platform, build identity, encoder ID and version, array layers, and cube face ordering metadata.
- Loading a compressed artifact at runtime requires backend format capability. Missing capability fails closed and logs an unsupported compressed texture artifact warning instead of silently degrading.

BC6H, ASTC4x4, and ETC2 RGBA8 are currently descriptor and registry extension points only. Vulkan, ASTC, ETC2, BC6H, cubemap runtime loading, and streaming mips each need their own encoder or backend evidence before support can be claimed.

## Texture runtime behavior

- Mip generation happens during import; the runtime selects a level based on sampling.
- Compressed bytes are copied by block row, with format and sRGB view selection applied before upload.
- Unit tests do not replace a real backend capture: claiming a format works at runtime requires an actual load on that platform.

## Next steps

- [References, Artifacts, and Caches](references.md) explains how artifacts are tracked and loaded.
- [Materials and ShaderLab](../rendering/materials.md) covers how materials consume these textures.
