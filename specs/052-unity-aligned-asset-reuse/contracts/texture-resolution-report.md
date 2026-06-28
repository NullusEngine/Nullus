# Contract: Texture Resolution Report

## Purpose

Defines the data Asset Properties reads to show the last committed model texture resolution result.

## Logical Shape

```text
NULLUS_MODEL_TEXTURE_RESOLUTION_REPORT=1
reportVersion=1
modelAssetId=<guid>
targetPlatform=<platform>
importerVersion=<integer>
settingsFingerprint=<hex-or-base64url>
entryCount=<count>

entry.<index>.sourceStableKey=<stable-key>
entry.<index>.sourceKey=<source-key>
entry.<index>.materialTextureKey=<material-texture-key>
entry.<index>.displayName=<name>
entry.<index>.uri=<uri>
entry.<index>.normalizedUri=<normalized-uri>
entry.<index>.textureSourceKind=ExternalFile|EmbeddedData|BufferView|Missing
entry.<index>.stableKeyStatus=Stable|OrderDerived|Insufficient
entry.<index>.resolutionKind=ExplicitRemap|SourcePath|NameSearch|ModelEmbeddedFallback|Missing|Invalid
entry.<index>.targetAssetId=<guid>
entry.<index>.targetSubAssetKey=<sub-asset-key>
entry.<index>.resourcePath=<artifact-path>
entry.<index>.modelSubAssetKey=<model-local-texture-key>
entry.<index>.diagnosticCount=<count>
entry.<index>.diagnostic.<n>.severity=Warning|Error
entry.<index>.diagnostic.<n>.code=<stable-code>
entry.<index>.diagnostic.<n>.message=<user-facing-message>
```

## Persistence

- The report is an editor-only sidecar under the committed model artifact root, named `texture-resolution-report.txt`.
- The report is not listed as a runtime manifest sub-asset.
- The importer writes a pending report beside pending artifacts, validates that it parses, and atomically moves/replaces it only after the model import commit succeeds.
- A failed model import must leave the previous committed report unchanged.
- A successful reimport replaces the previous report completely; stale entries from older imports must not remain.

## Encoding

All values are line-based key/value fields. Values that can contain `%`, `=`, `#`, `;`, `|`, carriage return, newline, path separators that need preservation, or non-ASCII bytes use the same percent encoding as `model-texture-remap-settings.md`.

## Rules

- Reports describe the last committed import only.
- Reports may omit fields that are not meaningful for a resolution kind.
- Asset Properties must tolerate missing, stale, or malformed reports.
- Asset Properties must hide a report when `modelAssetId`, `targetPlatform`, `importerVersion`, `settingsFingerprint`, or `reportVersion` does not match the selected model/current import context.
- Diagnostic codes must be stable enough for tests.
- User-facing messages should include enough source texture context to locate the problem.
- `materialTextureKey` is the key used to feed material conversion path lookup; `sourceStableKey` is the key used for remaps. They must not be conflated.
