# Quickstart: Cross-Platform Dependency Setup

## Local Developer Flow

```powershell
git clone <repo-url> Nullus
cd Nullus
.\SetupDependencies.bat
.\build_windows.bat Debug x64
```

```bash
git clone <repo-url> Nullus
cd Nullus
./SetupDependencies.sh
./build_linux.sh debug ninja
```

```bash
git clone <repo-url> Nullus
cd Nullus
./SetupDependencies.sh
./build_macos.sh debug
```

Interactive setup prompts for Autodesk EULA acceptance before it downloads or installs Autodesk FBX SDK content.

## CI Flow

```bash
NLS_ACCEPT_AUTODESK_FBX_EULA=1 ./SetupDependencies.sh --non-interactive
```

```powershell
$env:NLS_ACCEPT_AUTODESK_FBX_EULA = "1"
.\SetupDependencies.bat --non-interactive
```

CI must set the acceptance signal explicitly. Without it, setup fails before downloading packages.
Windows CI jobs may pass `--arch x64` or `--arch ARM64` to validate/install the SDK layout that matches the build target. Linux and macOS jobs should not pass `--arch`.

## Validation Commands

```bash
python Tools/SetupDependencies/setup_dependencies.py --dry-run --accept-autodesk-eula
python Tools/SetupDependencies/setup_dependencies.py --validate-only
python -m unittest discover -s Tests/SetupDependencies
```

## Expected SDK Layout

```text
ThirdParty/FBX/
├── README.md
├── packages/          # ignored local package cache
└── sdk/               # ignored local installed SDKs
    ├── windows/
    ├── linux/
    └── macos/
```

The CMake build continues to consume only `ThirdParty/FBX/sdk/<platform>`.

## Failure Checks

- Run setup without acceptance in non-interactive mode and verify it exits before download.
- Corrupt a cached package and verify setup rejects it by SHA256.
- Remove `include/fbxsdk.h` from a test SDK root and verify validation reports the missing file.
- Configure CMake without a valid SDK and verify the message points to `SetupDependencies`.
