# Nullus dependency bootstrap

The C# generator, MetaParser, and game-script projects use the pinned .NET SDK
8.0.408. Install it into the repository (without changing the system PATH) with:

```text
SetupDependencies.bat --dependency dotnet-sdk
./SetupDependencies.sh --dependency dotnet-sdk
```

The installer downloads the platform/architecture package listed in
`dependency_manifest.json`, verifies its SHA-512 digest, extracts it under
`Tools/Dotnet/<platform>/<arch>`, and writes a repository-local launcher at
`Tools/Dotnet/dotnet.cmd` or `Tools/Dotnet/dotnet`.

CMake performs the same bootstrap automatically by default. Set
`-DNLS_AUTO_INSTALL_DOTNET=OFF` to require a preinstalled .NET 8 SDK, or set
`NLS_DOTNET_EXECUTABLE` to an explicit SDK executable.
