# Data Model: Optional Assimp FBX Import

## FBX Reader Selection

Represents the reader policy for a `.fbx` model asset.

**Fields**:

- `mode`: one of `autodesk`, `assimp`, `autodesk-with-assimp-fallback`
- `serializedKey`: persisted model importer setting key
- `defaultMode`: `autodesk`

**Validation Rules**:

- Empty or missing value resolves to `autodesk`.
- Unknown values resolve to `autodesk` for backward compatibility and may produce a warning only when surfaced by importer UI or diagnostics.
- Non-FBX assets ignore this setting.

**State Transitions**:

- Missing -> `autodesk`
- `autodesk` -> `assimp` when user explicitly selects Assimp
- `autodesk` -> `autodesk-with-assimp-fallback` when user enables fallback
- Any explicit value -> another explicit value marks importer settings dirty and requires reimport

## FBX Reader Availability

Represents whether a reader can be attempted in the current build.

**Fields**:

- `autodeskAvailable`: true when the bundled Autodesk SDK target compiled with SDK support
- `assimpFbxAvailable`: true when the build includes Assimp FBX importer support

**Validation Rules**:

- Autodesk may be unavailable in fresh clones unless the SDK package is installed.
- Assimp FBX is unavailable unless the narrow option or all-format Assimp build is enabled.

## FBX Reader Attempt Result

Represents the result of trying one reader for one source asset.

**Fields**:

- `reader`: attempted reader
- `loaded`: whether mesh data was produced
- `hasDetailedScene`: whether parser-specific detailed scene data was produced
- `meshes`: parsed mesh data
- `materialNames`: parser material names
- `externalDependencies`: texture or linked file dependencies found by the parser
- `diagnostics`: warnings or errors emitted by reader selection

**Validation Rules**:

- A successful result must contain at least one mesh.
- Detailed scene data is optional; the existing generic parsed-scene conversion remains the fallback.
- External dependencies must be merged without duplicates into artifact dependencies.

## Fallback Diagnostic

Warning emitted when `autodesk-with-assimp-fallback` succeeds with Assimp after Autodesk cannot complete.

**Fields**:

- `code`
- `assetId`
- `sourcePath`
- `primaryReader`
- `fallbackReader`
- `reason`

**Validation Rules**:

- Emitted only when fallback mode is selected.
- Emitted only after the primary reader is unavailable or fails and the fallback reader is attempted.
- Error diagnostics from both readers are preserved or summarized when both fail.
