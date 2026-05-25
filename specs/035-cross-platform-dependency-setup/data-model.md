# Data Model: Cross-Platform Dependency Setup

## DependencyManifest

Represents the tracked setup metadata file.

**Fields**:

- `schemaVersion`: manifest schema version string.
- `dependencies`: map of dependency identifiers to dependency records.

**Validation rules**:

- `schemaVersion` must be present.
- Each dependency must provide a human-readable name and at least one platform package.

## DependencyRecord

Represents one dependency family, initially Autodesk FBX SDK.

**Fields**:

- `id`: stable dependency identifier, such as `autodesk-fbx-sdk`.
- `name`: display name for logs and docs.
- `version`: dependency version, expected to be `2020.3.9` for Autodesk FBX SDK.
- `eula`: metadata describing the EULA prompt and acceptance environment variable.
- `platforms`: map from platform key to `PlatformPackage`.

**Validation rules**:

- `id`, `name`, `version`, and `platforms` are required.
- `platforms` must include `windows`, `linux`, and `macos` for this feature.

## PlatformPackage

Represents the package and installed SDK layout for one host platform.

**Fields**:

- `platform`: `windows`, `linux`, or `macos`.
- `url`: official package download URL.
- `fileName`: local cache filename.
- `sha256`: expected package SHA256.
- `sdkRoot`: relative target SDK root under the repository.
- `archiveType`: package/extraction strategy identifier.
- `installerEntry`: optional installer executable or package entry inside an archive.
- `validation`: `SdkValidationSpec`.

**Validation rules**:

- `url`, `fileName`, `sha256`, `sdkRoot`, and `validation` are required.
- `sha256` must be a 64-character hexadecimal string.
- `sdkRoot` must stay under `ThirdParty/FBX/sdk/`.

## SdkValidationSpec

Represents files and version values expected after setup.

**Fields**:

- `headers`: required header paths relative to SDK root.
- `versionHeader`: version header path relative to SDK root.
- `version`: expected major, minor, and point values.
- `linkLibraries`: required or candidate link library paths relative to SDK root.
- `runtimeLibraries`: required or candidate runtime library paths relative to SDK root.

**Validation rules**:

- `include/fbxsdk.h` and `include/fbxsdk/fbxsdk_version.h` must be required headers.
- Version values must match Autodesk FBX SDK 2020.3.9.
- Runtime libraries must include the platform shared library (`libfbxsdk.dll`, `libfbxsdk.so`, or `libfbxsdk.dylib`).

## SetupOptions

Represents one setup command invocation.

**Fields**:

- `dependency`: dependency id, defaulting to Autodesk FBX SDK.
- `platform`: detected or user-overridden platform.
- `acceptEula`: explicit command-line acceptance flag.
- `nonInteractive`: disables prompts.
- `dryRun`: validates decisions without downloading or installing.
- `force`: redownload/reinstall even when a valid SDK root exists.
- `cacheDir`: package cache directory.
- `repoRoot`: repository root.

**Validation rules**:

- Download/install actions require `acceptEula` or accepted environment variable.
- `dryRun` must not modify cache or SDK root.
- Platform override must match a manifest platform.

## SetupResult

Represents the outcome printed by the setup command.

**States**:

- `already-valid`: SDK root was valid before setup.
- `downloaded`: package was downloaded and hash-verified.
- `cache-hit`: package cache was reused and hash-verified.
- `installed`: SDK root was created and validated.
- `blocked-by-eula`: setup stopped before download/install.
- `failed`: setup could not complete and emitted an actionable diagnostic.
