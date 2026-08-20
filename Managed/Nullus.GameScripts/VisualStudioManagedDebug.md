# Nullus C# debugging in Visual Studio

This project uses the embedded CoreCLR hosted by the Nullus Editor.

1. Build `GameScripts` in Debug configuration. Visual Studio validates the linked project scripts; the Editor build service uses the repository-local .NET 8 SDK to produce the runtime assembly and portable PDB.
2. Start the Nullus Editor and enter Play.
3. In Visual Studio, open `Nullus.GameScripts.csproj`. The generated project is intentionally non-launchable because it is a class library.
4. Use Debug > Attach to Process (Ctrl+Alt+P), select the running `Editor.exe`, and choose the .NET Core/Managed code type. Do not press F5; stock Visual Studio C# launch profiles cannot express an attach target.
5. Set breakpoints in `Assets/**/*.cs` or `Managed/Nullus.GameScripts/Scripts/**/*.cs`.

Directory rule: open only `build/Nullus.sln`; `Managed/Nullus.GameScripts` is the source C# project, `TestProject/Assets` contains project scripts, and `build/Managed` plus `TestProject/Library` are generated outputs. Older `build-*`, `ci-*`, and `scripting-*` directories are not active debug projects.

The Editor keeps the old assembly active when a Debug build fails. A successful build swaps the collectible CoreCLR load context at a frame boundary and preserves serialized fields.

The Editor executable path is deliberately not stored in the Visual Studio launch profile. This prevents duplicate Editor processes and keeps attachment tied to the process currently running the project.

6. For one-click attach without starting a second Editor, run `powershell -ExecutionPolicy Bypass -File AttachNullusCSharp.ps1 -SolutionPath <path-to-Nullus.sln>`. The helper only attaches to the already running Editor process.
