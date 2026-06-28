# Quickstart: Unity-Aligned Asset Reuse

## Automated Validation

1. Configure and build `NullusUnitTests` with normal project settings.

```powershell
cmake -S . -B Build
cmake --build Build --config Debug --target NullusUnitTests -- /m /nodeReuse:false
```

2. Run the focused asset import tests:

```powershell
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetImportPipelineTests.AssetPropertiesModelTexture*:AssetImportPipelineTests.ModelTextureResolutionReport*:AssetImportPipelineTests.ModelTextureRemapSettings*
```

Expected result for the focused Asset Properties/report/remap validation: all listed tests pass. During implementation, use the narrower executable filter available in the local build directory for `AssetImportPipelineTests` when iteration speed matters.

For full-suite validation in this multi-configuration build, include `-C Debug`:

```powershell
ctest --test-dir Build -C Debug --output-on-failure -R NullusUnitTests
```

As of 2026-06-17, the full suite still has unrelated baseline failures outside this feature slice. Record those separately instead of treating them as 052 regressions.

3. Verify these scenarios are covered by unit tests:

- Two model sources that reference the same project texture produce material paths to the same texture artifact.
- The model manifests do not contain model-owned texture sub-assets for externally resolved textures.
- Explicit remap beats source path and name search.
- Stable remap keys are deterministic for duplicate names, empty source keys, normalized relative/absolute paths, data URIs, GLB buffer-view textures, unchanged URIs with changed display names, semicolon/pipe characters, and order-derived fallback warnings.
- Material path maps are keyed by the exact imported material texture key for glTF base color/normal, FBX diffuse/opacity, and OBJ MTL texture references.
- Ambiguous name search produces a warning and no automatic binding.
- Unique-name search is deterministic across project roots, case collisions, disabled automatic import, and automatic import failure.
- Embedded or data-backed textures still produce model-owned texture sub-assets when no external target is resolved.
- Disabling `useExternalTextures`, `searchByName`, or `autoImportMissingTextureFiles` changes the next import result as documented.
- Model imports become stale when a reused texture file/meta/artifact/path mapping or unique-name candidate set changes, including same-count candidate sets with different GUIDs or paths.
- Failed model imports do not replace the previous texture resolution report; successful reimports replace it atomically.
- Reimporting a legacy model removes only externally resolved duplicate texture sub-assets while preserving mesh/material/prefab keys and embedded/unresolved texture sub-assets.
- Unsupported texture encodings produce visible diagnostics without creating duplicate externally resolved texture sub-assets.

## Manual Editor Verification

1. Open a project with at least two models that reference the same texture file under `Assets/Textures/`.
2. Select the first model in Asset Browser and open Asset Properties.
3. Confirm model texture resolution settings are visible.
4. Reimport the model.
5. Confirm the External Textures/Remaps section reports `SourcePath` or `ExplicitRemap` for the shared texture.
6. Expand the model asset in Asset Browser.
7. Confirm the shared texture no longer appears as a model-owned generated texture sub-asset.
8. Preview the model or instantiate its prefab.
9. Confirm materials still show the expected texture.

## Manual Remap Verification

1. Select a model with a texture name that could match more than one project texture.
2. Confirm Asset Properties reports an ambiguous name warning after import.
3. Choose a specific texture asset in the remap control.
4. Apply and reimport.
5. Confirm the report changes to `ExplicitRemap`.
6. Confirm material preview uses the selected texture.

## Regression Checks

- Existing models are unchanged until reimported.
- Reimporting a model with embedded-only textures still creates model-owned texture sub-assets.
- Invalid remap targets warn and fall back instead of failing the whole import.
- Stale, malformed, or mismatched reports are hidden in Asset Properties instead of blocking preview.
- Applying, clearing, and displaying remaps in Asset Properties changes only model metadata until a reimport commits new artifacts.
- `Asset View` remains a preview surface and does not own remap logic.
