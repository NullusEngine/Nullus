# C# Script Debugging

The first Nullus C# debugging workflow runs in local Editor Play. Visual
Studio 2022 and VS Code attach to the CoreCLR hosted by the project Editor.
The debugger does not enter Play automatically unless an `Attach and Play`
profile is selected.

## One-time setup

1. Generate or open `<Project>/Library/IDE/VisualStudio/Nullus.Project.sln`.
2. Close every Visual Studio instance and install the offline package at
   `Tools/Debug/VisualStudioExtension/Nullus.ScriptDebugger.vsix`.
3. Restart Visual Studio and reopen the project solution. The shared
   `build/Nullus.sln` remains the engine-source solution and is not the project
   script F5 workspace.

The VSIX is bundled with the repository. It does not use the system `PATH` or
download NuGet/debugging components. Exit VS before upgrading it because an
in-use MEF component cannot be replaced.

## F5 configurations

The active Visual Studio solution configuration is the only source of the
native Editor configuration:

| Solution configuration | Editor executable |
| --- | --- |
| `Debug|x64` | `App/Win64_Debug_Runtime_Shared/Editor.exe` |
| `Release|x64` | `App/Win64_Release_Runtime_Shared/Editor.exe` |

There is no separate Release launch profile. All of these profiles read the
active solution configuration:

- `Nullus: C# Scripts`: attach CoreCLR and remain in Edit mode.
- `Nullus: C# Attach and Play`: enter Play only after CoreCLR attaches.
- `Nullus: Editor + C# Mixed`: attach Native and CoreCLR; matching engine
  sources and PDBs are required.

When F5 is pressed, the VSIX uses the absolute Broker path in the project's
`Library/IDE/Nullus.Debug.json`. The Broker reuses an existing Editor only when
the project ID, PID, start time, IPC endpoint, and executable all match. If no
matching instance exists, it starts exactly one Editor for the selected
configuration.

## Breakpoints and hot reload

The ordinary C# profile does not enter Play. Click Play in the Editor after the
debugger attaches; `Awake`, `Start`, and `Update` breakpoints can then bind.
Saving `Assets/**/*.cs` queues an automatic Debug build. A successful build
switches the collectible ALC at a frame boundary and rebinds PDB breakpoints;
a failed build keeps the previous assembly running.

## Verification and troubleshooting

The VSIX writes the latest launch to `%TEMP%/Nullus.ScriptDebugger.log`. A
Release launch must contain both lines:

```text
configuration=Release
executable=...\Win64_Release_Runtime_Shared\Editor.exe
```

The active instance can also be checked in
`<Project>/Library/IDE/EditorInstance.json` using `editorExecutable` and
`processId`. After an abnormal Editor exit, the Broker validates the process
and IPC record and removes it once stale, so a reused PID cannot select the
wrong Editor.

Native Mixed builds use the repository wrapper
`Tools/BuildCleanEnvironment.cmd`. It normalizes duplicate `Path`/`PATH` only
for child build processes and does not modify the user's environment. If engine
sources or matching PDBs are unavailable, use `Nullus: C# Scripts` instead of
Mixed.
