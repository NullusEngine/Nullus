# Contract: Model Texture Remap Settings

## Purpose

Defines the persisted user-authored model texture resolution settings stored with a model asset.

## Settings Keys

```text
MODEL_TEXTURE_SETTINGS_VERSION=1
MODEL_TEXTURE_USE_EXTERNAL_TEXTURES=true|false
MODEL_TEXTURE_SEARCH_BY_NAME=true|false
MODEL_TEXTURE_AUTO_IMPORT_MISSING=true|false
MODEL_TEXTURE_EMBEDDED_MODE=ModelSubAsset
MODEL_TEXTURE_REMAP.<encoded-source-stable-key>=<texture-guid>#<sub-asset-key>
```

## Defaults

```text
MODEL_TEXTURE_SETTINGS_VERSION=1
MODEL_TEXTURE_USE_EXTERNAL_TEXTURES=true
MODEL_TEXTURE_SEARCH_BY_NAME=true
MODEL_TEXTURE_AUTO_IMPORT_MISSING=true
MODEL_TEXTURE_EMBEDDED_MODE=ModelSubAsset
```

## Stable Source Key Contract

Remap keys use the `TextureSourceIdentifier.stableKey` defined in `data-model.md`.

```text
mtxsrc:v1:kind=<kind>;source=<sourceKey>;uri=<normalizedUri>
mtxsrc:v1:kind=BufferView;source=<sourceKey>;bufferView=<bufferViewKey>
mtxsrc:v1:kind=EmbeddedData;source=<sourceKey>;embedded=<embeddedIndex>
mtxsrc:v1:kind=ExternalFile;uri=<normalizedUri>;discriminator=<stableDiscriminator>
mtxsrc:v1:kind=ExternalFile;name=<displayName>
mtxsrc:v1:kind=ExternalFile;uri=<normalizedUri>;dup=<n>
```

Rules:
- `source`, `uri`, `bufferView`, `embedded`, and `name` are included only when meaningful.
- Component values are percent-encoded before joining.
- `name` is included only when no source key, URI, buffer view key, embedded index, or stable discriminator is available.
- Intra-model collisions first append a stable discriminator such as format image/texture index, container ordinal, material texture key, or material channel path. `dup=<zero-based occurrence>` is allowed only as an order-derived last resort and must be reported with `model-texture-source-key-order-derived`.
- The encoded key in `MODEL_TEXTURE_REMAP.<encoded-source-stable-key>` is the whole stable key after percent encoding for metadata storage.

## Encoding

Use percent encoding for setting key suffixes and remap value fields:

```text
%  -> %25
=  -> %3D
#  -> %23
;  -> %3B
|  -> %7C
\r -> %0D
\n -> %0A
non-ASCII UTF-8 bytes -> %HH
```

Malformed percent sequences make that setting invalid and produce `model-texture-remap-malformed`.

## Rules

- Remap keys are scoped to the model `.meta` file.
- `encoded-source-stable-key` must be deterministic and safe for sidecar text storage.
- Remap values must refer to project texture assets.
- Empty or malformed remap values are ignored with an import warning.
- Clearing a remap removes the `MODEL_TEXTURE_REMAP.<key>` setting.
- Phase one supports only `ModelSubAsset` embedded mode.
- Unknown settings versions are ignored with defaults rather than partially interpreted.
