# Quickstart: Optional Assimp FBX Import

## Configure Builds

Default minimal build keeps the current behavior:

```powershell
cmake -S . -B build
```

Enable only Assimp FBX import support without all Assimp formats:

```powershell
cmake -S . -B build-assimp-fbx -DNLS_ENABLE_ASSIMP_FBX_IMPORTER=ON
```

Full Assimp compatibility mode remains available:

```powershell
cmake -S . -B build-assimp-full -DNLS_ASSIMP_BUILD_ALL_FORMATS=ON
```

## Verify Default FBX Behavior

1. Import an existing `.fbx` asset with no reader setting.
2. Confirm the import succeeds through the Autodesk reader when the bundled SDK is available.
3. Confirm no fallback warning appears.
4. Confirm generated prefab, mesh, material, and texture artifacts still appear in the asset artifact manifest.

## Verify Explicit Assimp FBX

1. Configure with `NLS_ENABLE_ASSIMP_FBX_IMPORTER=ON`.
2. Set the FBX asset reader setting to `assimp`.
3. Reimport the asset.
4. Confirm generated artifacts are written through the normal model import pipeline.
5. Confirm texture dependencies found by Assimp are recorded in the artifact manifest.

## Verify Controlled Fallback

1. Configure with Assimp FBX enabled.
2. Set the FBX asset reader setting to `autodesk-with-assimp-fallback`.
3. Use an environment where Autodesk is unavailable or a test hook forces Autodesk failure.
4. Reimport the asset.
5. Confirm import attempts Assimp and emits a fallback warning.

## Targeted Tests

Run the relevant unit tests after implementation:

```powershell
ctest --test-dir build -C Debug -R NullusUnitTests --output-on-failure
```

For a narrower loop, run `NullusUnitTests` with filters covering asset importer settings, asset import pipeline, and FBX SDK integration contract tests.
