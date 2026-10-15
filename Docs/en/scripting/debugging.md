# Script Debugging and Hot Reload

C# debugging runs against a local editor Play session by attaching to the running CoreCLR through a project-level debug workspace. The debugger does not enter Play on its own unless you pick a profile that includes `Attach and Play`.

## One-time setup

1. Generate or open the project workspace at `<Project>/Library/IDE/VisualStudio/Nullus.Project.sln`.
2. Close all Visual Studio instances and install `Tools/Debug/VisualStudioExtension/Nullus.ScriptDebugger.vsix` from the repository.
3. Restart Visual Studio and reopen the project-level solution.

The `build/` solution at the repository root is for engine source only and is not the F5 workspace for project scripts.

The VSIX is an offline package: it does not depend on the system `PATH` and does not download NuGet packages or debugging components. Close Visual Studio before updating the VSIX, otherwise the components in use cannot be replaced.

## F5 profiles

The Visual Studio solution configuration decides which editor starts:

| Solution configuration | Editor that starts |
| --- | --- |
| `Debug\|x64` | `App/Win64_Debug_Runtime_Shared/Editor.exe` |
| `Release\|x64` | `App/Win64_Release_Runtime_Shared/Editor.exe` |

No separate Release profile is needed. These profiles read the current solution configuration:

| Profile | Behavior |
| --- | --- |
| `Nullus: C# Scripts` | Attach the C# debugger only and stay in edit mode |
| `Nullus: C# Attach and Play` | Enter Play after the CoreCLR attach succeeds |
| `Nullus: Editor + C# Mixed` | Attach both native and CoreCLR; requires engine source and matching PDBs |

After pressing F5, the VSIX reads the broker path from the project's `Library/IDE/Nullus.Debug.json`. The broker reuses an existing editor instance by project ID and validates the PID, start time, project, and executable; it only starts a new editor when no instance matches. This prevents two editors from writing to the same project.

## Breakpoints and hot reload

Plain C# profiles do not enter Play automatically. Once attached, press Play in the editor and `Awake`, `Start`, and `Update` breakpoints hit.

Saving `Assets/**/*.cs` triggers an automatic Debug build:

- On success, the editor swaps to a new collectible assembly at a frame boundary and rebinds PDB breakpoints.
- On failure, the previous working assembly keeps running and errors appear in the Console.

During a hot reload, existing script instances only see the backend replacement wrapped in `OnDisable` and `OnEnable`. The engine does not synthesize `Awake`, `Start`, or `OnDestroy`.

## Lua debugging

Lua has an optional LuaPanda integration that requires building with `NLS_ENABLE_LUAPANDA_DEBUGGING` (off by default). Once enabled, the project file's `lua_panda_debugging` and `lua_panda_port` decide whether to connect and on which port.

Lua has no build step: saving a file takes effect the next time you enter Play or trigger a reload.

## Verification and troubleshooting

The VSIX writes its most recent launch to `%TEMP%/Nullus.ScriptDebugger.log`. A Release verification must show both:

```text
configuration=Release
executable=...\Win64_Release_Runtime_Shared\Editor.exe
```

The current editor instance can be checked through `<Project>/Library/IDE/EditorInstance.json` for `editorExecutable` and `processId`. A stale record left by an abnormal exit is validated and cleaned up on the next broker call, so an old PID never starts a second editor.

Native mixed builds use `Tools/BuildCleanEnvironment.cmd`, which only cleans duplicated `Path`/`PATH` in the subprocess and does not modify the user environment. Without engine source or PDBs, choose `Nullus: C# Scripts`.

## Checklist

| Symptom | Check |
| --- | --- |
| Breakpoints do not hit | Whether a Play profile was selected and Play actually started |
| The wrong editor launched | Solution configuration versus `EditorInstance.json` |
| Script edits have no effect | Console build errors; the previous assembly may still be running |
| Mixed debugging cannot attach | Matching engine PDBs and the environment cleanup entry point |

## Next steps

- [Debugging and Testing](../quality/debugging.md) covers engine-side debugging and tests.
- [Scripting Overview](overview.md) returns to the scripting model.
