# Player Packaging and Distribution

`NullusBuild` is the headless packaging entry point; editor build actions share its pipeline. It collects scene dependencies, cooks target content, assembles a Player, writes `.npak` containers, and validates output. Desktop adapters exist; installers, automatic updates, and mobile delivery remain incomplete.

## Prepare the Player and project

Follow [Environment and Build](../getting-started/build.md), then build `Game` and `NullusBuild` in the existing build directory:

```powershell
cmake --build build --config RelWithDebInfo --target Game NullusBuild
build/bin/RelWithDebInfo/NullusBuild.exe capabilities
```

The Windows template uses `App/Win64_RelWithDebInfo_Runtime_Shared` and needs `Game.exe`, runtime libraries, and engine assets; C# projects also need the matching scripting host. Provide a loadable start scene and preserve source `.meta` files to keep GUID references stable.

## BuildProfiles.json

Place versioned configuration in the project root. This Windows DX12 example needs its start scene replaced with a real project path:

```json
{
  "schemaVersion": 1,
  "defaultProfile": "windows-shipping",
  "profiles": [
    {
      "id": "windows-shipping",
      "displayName": "Windows DX12",
      "productName": "MyGame",
      "executableName": "MyGame",
      "version": "0.1.0",
      "mode": "Shipping",
      "startScene": "Assets/Scenes/Main.scene",
      "target": {
        "platform": "windows",
        "architecture": "x64",
        "graphicsBackend": "dx12",
        "textureFormatPolicy": "target-native",
        "maxTextureSize": 2048,
        "scriptBackends": ["csharp", "lua"]
      },
      "outputFormat": "zip"
    }
  ]
}
```

| Field | Use |
| --- | --- |
| `defaultProfile` / `--profile` | Default profile and the ID selected for this build |
| `startScene` | Project-relative dependency root and Player start scene |
| `keepAssetPaths` | Explicit paths, directory prefixes, or GUIDs for dynamically loaded content |
| `assetPackSelections` | Additional asset packs and variants |
| `target.maxTextureSize` | Texture size cap; 0 preserves source size, with power-of-two processing during cooking |
| `target.textureFormatPolicy` | `target-native` for Shipping; `passthrough` is for validation profiles |
| `playerTemplatePath` / `--player` | Prebuilt Player template directory |
| `signingProfile` | Signing credential reference; missing signing configuration leaves output unsigned |

`Shipping` requires complete content and a compatible template. Shipping profiles with custom native modules are currently rejected and need a separate source-build workflow. Assets available only in editor caches are not delivered content.

## Build, inspect, and run

```powershell
build/bin/RelWithDebInfo/NullusBuild.exe build --project MyGame/MyGame.nullus --profile windows-shipping --player App/Win64_RelWithDebInfo_Runtime_Shared --output Dist --validate-launch
build/bin/RelWithDebInfo/NullusBuild.exe inspect --package Dist/MyGame
build/bin/RelWithDebInfo/NullusBuild.exe validate --package Dist/MyGame
```

Replace `Dist/MyGame` with the actual product directory or ZIP reported by the build. Exit codes are 0 for success, 1 for build/validation failure, and 2 for invalid arguments. A successful report is written to `BuildReport.json`; failure reports identify phases and diagnostics. A leftover directory is not a successful product.

Output includes the product executable, runtime libraries, `Player.info`, a runtime manifest, and content packages such as `base.npak`. C# scripts compile from the project's `Assets/**/*.cs` into a project-specific assembly; template sample scripts are not reused as project code. `Player.info` records the target, engine version, packages, and input hash, and the runtime checks compatibility before mounting content.

Run the product executable with `--package-self-test` in its install directory to check packages, the manifest, start scene, and script payload. This does not replace visible rendering, real input, script behavior, or GPU acceptance. Test each target and verify startup without the project's `Library/` directory.

## Platform limits

| Target | Current scope |
| --- | --- |
| Windows x64 | Desktop assembly, content containers, ZIP, and validation; graphics backends still need device acceptance |
| Linux x64 | Prebuilt ELF templates, ZIP/directory output, executable modes, and `$ORIGIN` paths; assembly can run on another host |
| macOS x64 / arm64 | Requires a macOS host and matching template; ZIP/directory output; Developer ID and notarization are not integrated |
| Android / iOS / Web | Capability declarations report gaps; they do not provide shipping adapters |

Building Linux output on another host skips the packaged launch self-test with a `build-launch-cross-host` warning. Accepted profile values do not establish runtime backend support: Vulkan is the delivery path, while OpenGL remains gated. A macOS build does not establish Metal execution, signing, or notarization. Installers, store submission, and automatic updates are outside current output.

## Related pages

- [Platform Support](platforms.md) separates build support from device validation.
- [Game Runtime and Asset Manifests](runtime.md) covers startup, authorization, and loading.
