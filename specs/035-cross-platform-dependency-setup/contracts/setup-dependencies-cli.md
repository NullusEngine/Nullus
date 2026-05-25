# Contract: Setup Dependencies CLI

## Entrypoints

```text
SetupDependencies.bat [options]
./SetupDependencies.sh [options]
python Tools/SetupDependencies/setup_dependencies.py [options]
```

Root wrappers forward all arguments to the Python implementation.

## Options

```text
--dependency <id>
```

Dependency to prepare. Defaults to `autodesk-fbx-sdk`.

```text
--platform <windows|linux|macos>
```

Override platform detection. Intended for tests and diagnostics.

```text
--arch <x64|arm64>
```

Override Windows SDK architecture detection. Intended for Windows CI jobs that build a known target architecture; values are case-insensitive. This option is rejected for Linux and macOS.

```text
--accept-autodesk-eula
```

Explicitly confirms the caller accepts Autodesk terms for downloading and installing Autodesk FBX SDK content.

```text
--non-interactive
```

Disables prompts. If EULA acceptance is missing, setup exits before download/install.

```text
--dry-run
```

Prints planned actions and validates manifest/options without modifying package cache or SDK root.

```text
--force
```

Allows redownload/reinstall even when a valid SDK root exists.

```text
--cache-dir <path>
```

Overrides the local package cache. Default is `ThirdParty/FBX/packages`.

```text
--repo-root <path>
```

Overrides repository root detection. Intended for tests and advanced CI layouts.

```text
--validate-only
```

Only validates the current SDK root for the selected dependency/platform.

```text
--manifest <path>
```

Overrides the dependency manifest path for tests and diagnostics. Runtime validation still requires official Autodesk HTTPS package URLs, confined SDK roots, plain package file names, relative installer entries, and valid SHA256 digests.

## Environment

```text
NLS_ACCEPT_AUTODESK_FBX_EULA=1
```

Accepted values: `1`, `true`, `yes`, `on`, case-insensitive. Empty, `0`, `false`, `no`, and unset do not count as acceptance.

```text
NLS_DEPENDENCY_CACHE=<path>
```

Optional default package cache override.

## Exit Codes

- `0`: dependency satisfied, installed, or dry-run/validate-only succeeded.
- `1`: user-facing setup failure such as missing EULA acceptance, hash mismatch, unsupported platform, incomplete SDK, download failure, or missing required tool.
- `2`: invalid command-line usage.

## Required Output Behavior

- Missing EULA acceptance must mention `--accept-autodesk-eula` and `NLS_ACCEPT_AUTODESK_FBX_EULA=1`.
- Hash mismatch must print expected and actual SHA256.
- Valid SDK root must print the resolved SDK root.
- Dry-run must clearly state that no files were modified.
- Unsupported platforms must list supported platforms from the manifest.
