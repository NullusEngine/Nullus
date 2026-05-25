# Autodesk FBX SDK

Nullus uses Autodesk FBX SDK 2020.3.9 for `.fbx` import. The SDK payload is not committed to this repository. Source developers should run the repository setup command to download the official package, accept Autodesk's terms, verify the package hash, and install it into the canonical bundled SDK layout.

## Setup

Windows:

```powershell
.\SetupDependencies.bat
```

Linux/macOS:

```bash
./SetupDependencies.sh
```

CI/headless environments must pass explicit EULA acceptance:

```bash
NLS_ACCEPT_AUTODESK_FBX_EULA=1 ./SetupDependencies.sh --non-interactive
```

For Windows CI jobs, pass `--arch x64` or `--arch ARM64` when the job's target architecture is known. The option is Windows-only.

The setup command downloads packages into `ThirdParty/FBX/packages/` and installs SDK files into `ThirdParty/FBX/sdk/<platform>`. Both directories are ignored local outputs.

## Official Sources

Source page:

- https://aps.autodesk.com/developer/overview/fbx-sdk

Official package URLs:

- Windows VS2022: https://damassets.autodesk.net/content/dam/autodesk/www/files/fbx202039_fbxsdk_vs2022_win.exe
- Linux GCC: https://damassets.autodesk.net/content/dam/autodesk/www/files/fbx202039_fbxsdk_gcc_linux.tar.gz
- macOS Clang Universal: https://damassets.autodesk.net/content/dam/autodesk/www/files/fbx202039_fbxsdk_clang_mac.pkg.tgz

Package hashes:

| Package | SHA256 |
| --- | --- |
| `packages/fbx202039_fbxsdk_vs2022_win.exe` | `B1DB4330D327C8983C237D8FEA605FD9447E28E9D0D6E08139D87D6A86C1C987` |
| `packages/fbx202039_fbxsdk_gcc_linux.tar.gz` | `25D3CFD72A8A02070630A8F939BC2A41D48B9A8D87905488C352E949FA5C8635` |
| `packages/fbx202039_fbxsdk_clang_mac.pkg.tgz` | `C2BD9E9882C87368691E546A6EE8676646C81192C178150AC53F7BB3AC04262A` |

## Bundled SDK Layout

Nullus CMake consumes only the engine-bundled SDK roots:

- Windows: `ThirdParty/FBX/sdk/windows`
- Linux: `ThirdParty/FBX/sdk/linux`
- macOS: `ThirdParty/FBX/sdk/macos`

The build intentionally does not search system Autodesk FBX SDK installations or SDKs bundled by another engine.
