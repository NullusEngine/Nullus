# Nullus Visual Studio 2022 integration

The generated project workspace contains `Library/IDE/VisualStudio/Nullus.Project.sln`
and its project-local `Nullus.Debug.json`. The bundled package exports a CPS
`NullusEditor` launch provider: F5 asks the absolute project
`NullusDebugBroker` to start or reuse the matching Editor, prepares the
repository-local C# Debug assembly, and returns the validated PID to Visual
Studio's CoreCLR engine. It does not intercept the global F5 command, call
`EnvDTE.Process.Attach`, launch a second Editor, enter Play mode for the
ordinary C# profile, use a process picker, or inspect `PATH`.

`Nullus: C# Attach and Play` sends `EnterPlay` only after Visual Studio has
accepted the CoreCLR target. `Nullus: Editor + C# Mixed` adds the native
debug engine to the same target. `Shift+F5` therefore only detaches the
debugger; use `Nullus: Stop Play` when the Editor should leave Play mode.

The package source is kept with the repository so an offline build pipeline
can package it with the installed Visual Studio 2022 SDK. It is deliberately
separate from the native Editor build: the runtime has no dependency on the
Visual Studio SDK.

Install the generated `Nullus.ScriptDebugger.vsix` with all Visual Studio
instances closed. The installer cannot replace an in-use MEF component. After
installation, restart VS2022 and open the project-local
`Library/IDE/VisualStudio/Nullus.Project.sln`; do not use the shared engine
solution for project F5 debugging.

The active solution configuration is authoritative. Select `Debug|x64` or
`Release|x64` in the VS toolbar before pressing F5; every `NullusEditor` profile
uses that selection. Release therefore starts
`App/Win64_Release_Runtime_Shared/Editor.exe`, without a separate Release
profile or a process picker. The provider logs the selected configuration and
executable to `%TEMP%/Nullus.ScriptDebugger.log`.
